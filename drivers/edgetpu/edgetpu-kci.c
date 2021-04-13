// SPDX-License-Identifier: GPL-2.0
/*
 * Kernel Control Interface, implements the protocol between AP kernel and TPU
 * firmware.
 *
 * Copyright (C) 2019 Google, Inc.
 */

#include <linux/circ_buf.h>
#include <linux/device.h>
#include <linux/dma-mapping.h> /* dmam_alloc_coherent */
#include <linux/errno.h>
#include <linux/seq_file.h>
#include <linux/slab.h>
#include <linux/string.h> /* memcpy */

#include "edgetpu-firmware.h"
#include "edgetpu-internal.h"
#include "edgetpu-kci.h"
#include "edgetpu-iremap-pool.h"
#include "edgetpu-mmu.h"
#include "edgetpu-telemetry.h"
#include "edgetpu-usage-stats.h"

/* the index of mailbox for kernel should always be zero */
#define KERNEL_MAILBOX_INDEX 0

/* size of queue for KCI mailbox */
#define QUEUE_SIZE MAX_QUEUE_SIZE

/* Timeout for KCI responses from the firmware (milliseconds) */
#if IS_ENABLED(CONFIG_EDGETPU_FPGA)
/* Set extra ludicrously high to 60 seconds for (slow) Palladium emulation. */
#define KCI_TIMEOUT	(60000)
#elif IS_ENABLED(CONFIG_EDGETPU_TEST)
/* fake-firmware could respond in a short time */
#define KCI_TIMEOUT	(200)
#else
/* 5 secs. */
#define KCI_TIMEOUT	(5000)
#endif

static inline u32 edgetpu_kci_queue_element_size(enum mailbox_queue_type type)
{
	if (type == MAILBOX_CMD_QUEUE)
		return sizeof(struct edgetpu_command_element);
	else
		return sizeof(struct edgetpu_kci_response_element);
}

static int edgetpu_kci_alloc_queue(struct edgetpu_dev *etdev,
				   struct edgetpu_mailbox *mailbox,
				   enum mailbox_queue_type type,
				   struct edgetpu_coherent_mem *mem)
{
	u32 queue_size = QUEUE_SIZE;
	u32 size = queue_size * edgetpu_kci_queue_element_size(type);
	int ret;

	ret = edgetpu_iremap_alloc(etdev, size, mem, EDGETPU_CONTEXT_KCI);
	if (ret)
		return ret;

	ret = edgetpu_mailbox_set_queue(mailbox, type, mem->tpu_addr,
					queue_size);
	if (ret) {
		etdev_err(etdev, "failed to set mailbox queue: %d", ret);
		edgetpu_iremap_free(etdev, mem, EDGETPU_CONTEXT_KCI);
		return ret;
	}

	return 0;
}

static void edgetpu_kci_free_queue(struct edgetpu_dev *etdev,
				   struct edgetpu_coherent_mem *mem)
{
	edgetpu_iremap_free(etdev, mem, EDGETPU_CONTEXT_KCI);
}

/* Handle one incoming request from firmware */
static void
edgetpu_reverse_kci_consume_response(struct edgetpu_dev *etdev,
				     struct edgetpu_kci_response_element *resp)
{
	if (resp->code <= RKCI_CHIP_CODE_LAST) {
		edgetpu_chip_handle_reverse_kci(etdev, resp);
		return;
	}

	switch (resp->code) {
	case RKCI_FIRMWARE_CRASH:
		edgetpu_handle_firmware_crash(
		      etdev, (enum edgetpu_fw_crash_type)resp->retval);
		break;
	default:
		etdev_warn(etdev, "%s: Unrecognized KCI request: 0x%x\n",
			   __func__, resp->code);
	}
}

/* Remove one element from the circular buffer */
static int
edgetpu_reverse_kci_remove_response(struct edgetpu_reverse_kci *rkci,
				    struct edgetpu_kci_response_element *resp)
{
	unsigned long head, tail;
	int ret = 0;

	spin_lock(&rkci->consumer_lock);

	/*
	 * Prevents the compiler from discarding and reloading its cached value
	 * additionally forces the CPU to order against subsequent memory
	 * references.
	 * Shamelessly stolen from:
	 * https://www.kernel.org/doc/html/latest/core-api/circular-buffers.html
	 */
	head = smp_load_acquire(&rkci->head);
	tail = rkci->tail;
	if (CIRC_CNT(head, tail, REVERSE_KCI_BUFFER_SIZE) >= 1) {
		*resp = rkci->buffer[tail];
		tail = (tail + 1) & (REVERSE_KCI_BUFFER_SIZE - 1);
		ret = 1;
		smp_store_release(&rkci->tail, tail);
	}
	spin_unlock(&rkci->consumer_lock);
	return ret;
}

/* Worker for incoming requests from firmware */
static void edgetpu_reverse_kci_work(struct work_struct *work)
{
	struct edgetpu_kci_response_element resp;
	struct edgetpu_reverse_kci *rkci =
		container_of(work, struct edgetpu_reverse_kci, work);
	struct edgetpu_kci *kci = container_of(rkci, struct edgetpu_kci, rkci);

	while (edgetpu_reverse_kci_remove_response(rkci, &resp))
		edgetpu_reverse_kci_consume_response(kci->mailbox->etdev,
						     &resp);
}

/*
 * Add an incoming request from firmware to the circular buffer and
 * schedule the work queue for processing
 */
static int edgetpu_reverse_kci_add_response(
	struct edgetpu_kci *kci,
	const struct edgetpu_kci_response_element *resp)
{
	struct edgetpu_reverse_kci *rkci = &kci->rkci;
	unsigned long head, tail, flags;
	int ret = 0;

	spin_lock_irqsave(&rkci->producer_lock, flags);
	head = rkci->head;
	tail = READ_ONCE(rkci->tail);
	if (CIRC_SPACE(head, tail, REVERSE_KCI_BUFFER_SIZE) >= 1) {
		rkci->buffer[head] = *resp;
		smp_store_release(&rkci->head,
				  (head + 1) & (REVERSE_KCI_BUFFER_SIZE - 1));
		schedule_work(&rkci->work);
	} else {
		ret = -ENOSPC;
	}
	spin_unlock_irqrestore(&rkci->producer_lock, flags);
	return ret;
}

/* Initialize the Reverse KCI handler */
static void edgetpu_reverse_kci_init(struct edgetpu_reverse_kci *rkci)
{
	spin_lock_init(&rkci->producer_lock);
	spin_lock_init(&rkci->consumer_lock);
	INIT_WORK(&rkci->work, edgetpu_reverse_kci_work);
}

/*
 * Pops the wait_list until the sequence number of @resp is found, and copies
 * @resp to the found entry.
 *
 * Both entry in wait_list and response handling should have sequence number in
 * increasing order.
 * Compare the #seq of head of wait_list with @resp->seq, we have three cases:
 * 1. #seq > @resp->seq:
 *   - Nothing to do, @resp is not needed and we're done.
 * 2. #seq == @resp->seq:
 *   - Copy @resp, pop the head and we're done.
 * 3. #seq < @resp->seq:
 *   - Should not happen, this implies the sequence number of either entries in
 *     wait_list or responses are out-of-order, or remote didn't respond to a
 *     command. In this case, the status of response will be set to
 *     KCI_STATUS_NO_RESPONSE.
 *   - Pop until case 1. or 2.
 */
static void edgetpu_kci_consume_wait_list(
		struct edgetpu_kci *kci,
		const struct edgetpu_kci_response_element *resp)
{
	struct edgetpu_kci_wait_list *cur, *nxt;
	unsigned long flags;

	spin_lock_irqsave(&kci->wait_list_lock, flags);

	list_for_each_entry_safe(cur, nxt, &kci->wait_list, list) {
		if (cur->resp->seq > resp->seq)
			break;
		if (cur->resp->seq == resp->seq) {
			memcpy(cur->resp, resp, sizeof(*resp));
			list_del(&cur->list);
			kfree(cur);
			break;
		}
		/* #seq < @resp->seq */
		cur->resp->status = KCI_STATUS_NO_RESPONSE;
		list_del(&cur->list);
		kfree(cur);
	}

	spin_unlock_irqrestore(&kci->wait_list_lock, flags);
}

/*
 * Handler of a response.
 * if seq has the MSB set, forward the response to the reverse KCI handler
 */
static void
edgetpu_kci_handle_response(struct edgetpu_kci *kci,
			    const struct edgetpu_kci_response_element *resp)
{
	if (resp->seq & KCI_REVERSE_FLAG) {
		int ret = edgetpu_reverse_kci_add_response(kci, resp);

		if (ret)
			etdev_warn(
				kci->mailbox->etdev,
				"Failed to handle reverse KCI code %u (%d)\n",
				resp->code, ret);
		return;
	}
	edgetpu_kci_consume_wait_list(kci, resp);
}

/*
 * Fetches elements in the response queue.
 *
 * Returns the pointer of fetched response elements.
 * @total_ptr will be the number of elements fetched.
 *
 * Returns -ENOMEM if failed on memory allocation.
 * Returns NULL if the response queue is empty or there is another worker
 * fetching responses.
 */
static struct edgetpu_kci_response_element *edgetpu_kci_fetch_responses(
		struct edgetpu_kci *kci, u32 *total_ptr)
{
	u32 head;
	u32 tail;
	u32 count;
	u32 i;
	u32 j;
	u32 total = 0;
	const u32 size = kci->mailbox->resp_queue_size;
	const struct edgetpu_kci_response_element *queue = kci->resp_queue;
	struct edgetpu_kci_response_element *ret = NULL;
	struct edgetpu_kci_response_element *prev_ptr = NULL;

	/* someone is working on consuming - we can leave early */
	if (!spin_trylock(&kci->resp_queue_lock))
		goto out;

	head = kci->mailbox->resp_queue_head;
	/* loop until our head equals to CSR tail */
	while (1) {
		tail = EDGETPU_MAILBOX_RESP_QUEUE_READ_SYNC(kci->mailbox, tail);
		count = circular_queue_count(head, tail, size);
		if (count == 0)
			break;

		prev_ptr = ret;
		ret = krealloc(prev_ptr, (total + count) * sizeof(*queue),
			       GFP_ATOMIC);
		/*
		 * Out-of-memory, we can return the previously fetched responses
		 * if any, or ENOMEM otherwise.
		 */
		if (!ret) {
			if (!prev_ptr)
				ret = ERR_PTR(-ENOMEM);
			else
				ret = prev_ptr;
			break;
		}
		/* copy responses */
		j = CIRCULAR_QUEUE_REAL_INDEX(head);
		for (i = 0; i < count; i++) {
			memcpy(&ret[total], &queue[j], sizeof(*queue));
			ret[total].status = KCI_STATUS_OK;
			j = (j + 1) % size;
			total++;
		}
		head = circular_queue_inc(head, count, size);
	}
	edgetpu_mailbox_inc_resp_queue_head(kci->mailbox, total);

	spin_unlock(&kci->resp_queue_lock);
	/*
	 * We consumed a lot of responses - ring the doorbell of *cmd* queue to
	 * notify the firmware, which might be waiting us to consume the
	 * response queue.
	 */
	if (total >= size / 2)
		EDGETPU_MAILBOX_CMD_QUEUE_WRITE(kci->mailbox, doorbell_set, 1);
out:
	*total_ptr = total;
	return ret;
}

/*
 * Fetches and handles responses, then wakes up threads that are waiting for a
 * response.
 *
 * Note: this worker is scheduled in the IRQ handler, to prevent use-after-free
 * or race-condition bugs, edgetpu_kci_release() must be called before free the
 * mailbox.
 */
static void edgetpu_kci_consume_responses_work(struct work_struct *work)
{
	struct edgetpu_kci *kci = container_of(work, struct edgetpu_kci, work);
	struct edgetpu_mailbox *mailbox = kci->mailbox;
	struct edgetpu_kci_response_element *responses;
	u32 i;
	u32 count = 0;

	/* fetch responses and bump RESP_QUEUE_HEAD */
	responses = edgetpu_kci_fetch_responses(kci, &count);
	if (count == 0)
		return;
	if (IS_ERR(responses)) {
		etdev_err(mailbox->etdev,
			  "KCI failed on fetching responses: %ld",
			  PTR_ERR(responses));
		return;
	}

	for (i = 0; i < count; i++)
		edgetpu_kci_handle_response(kci, &responses[i]);
	/*
	 * Responses handled, wake up threads that are waiting for a response.
	 */
	wake_up(&kci->wait_list_waitq);
	kfree(responses);
}

/* Returns the number of responses fetched - either 0 or 1. */
static int
edgetpu_kci_fetch_one_response(struct edgetpu_kci *kci,
			       struct edgetpu_kci_response_element *resp)
{
	u32 head;
	u32 tail;
	const struct edgetpu_kci_response_element *queue = kci->resp_queue;

	if (!spin_trylock(&kci->resp_queue_lock))
		return 0;

	head = kci->mailbox->resp_queue_head;
	tail = EDGETPU_MAILBOX_RESP_QUEUE_READ_SYNC(kci->mailbox, tail);
	/* queue empty */
	if (head == tail) {
		spin_unlock(&kci->resp_queue_lock);
		return 0;
	}

	memcpy(resp, &queue[CIRCULAR_QUEUE_REAL_INDEX(head)], sizeof(*queue));
	resp->status = KCI_STATUS_OK;
	edgetpu_mailbox_inc_resp_queue_head(kci->mailbox, 1);

	spin_unlock(&kci->resp_queue_lock);

	return 1;
}

static void edgetpu_kci_consume_one_response(struct edgetpu_kci *kci)
{
	struct edgetpu_kci_response_element resp;
	int ret;

	/* fetch (at most) one response */
	ret = edgetpu_kci_fetch_one_response(kci, &resp);
	if (!ret)
		return;
	edgetpu_kci_handle_response(kci, &resp);
	/*
	 * Responses handled, wake up threads that are waiting for a response.
	 */
	wake_up(&kci->wait_list_waitq);
}

/*
 * IRQ handler of KCI mailbox.
 *
 * Consumes one response (if any) and puts edgetpu_kci_consume_responses_work()
 * into the system work queue.
 */
static void edgetpu_kci_handle_irq(struct edgetpu_mailbox *mailbox)
{
	struct edgetpu_kci *kci = mailbox->internal.kci;

	/* Wake up threads that are waiting for response doorbell to be rung. */
	wake_up(&kci->resp_doorbell_waitq);
	/*
	 * Quickly consumes one response, which should be enough for usual
	 * cases, to prevent the host from being too busy to execute the
	 * scheduled work.
	 */
	edgetpu_kci_consume_one_response(kci);
	schedule_work(&kci->work);
}

int edgetpu_kci_init(struct edgetpu_mailbox_manager *mgr,
		     struct edgetpu_kci *kci)
{
	struct edgetpu_mailbox *mailbox = edgetpu_mailbox_kci(mgr);
	int ret;

	if (IS_ERR(mailbox))
		return PTR_ERR(mailbox);

	ret = edgetpu_kci_alloc_queue(mgr->etdev, mailbox, MAILBOX_CMD_QUEUE,
				       &kci->cmd_queue_mem);
	if (ret) {
		edgetpu_mailbox_remove(mgr, mailbox);
		return ret;
	}

	kci->cmd_queue = kci->cmd_queue_mem.vaddr;
	mutex_init(&kci->cmd_queue_lock);
	etdev_dbg(mgr->etdev, "%s: cmdq kva=%pK iova=0x%llx dma=%pad", __func__,
		  kci->cmd_queue_mem.vaddr, kci->cmd_queue_mem.tpu_addr,
		  &kci->cmd_queue_mem.dma_addr);

	ret = edgetpu_kci_alloc_queue(mgr->etdev, mailbox, MAILBOX_RESP_QUEUE,
				      &kci->resp_queue_mem);
	if (ret) {
		edgetpu_kci_free_queue(mgr->etdev, &kci->cmd_queue_mem);
		edgetpu_mailbox_remove(mgr, mailbox);
		return ret;
	}
	kci->resp_queue = kci->resp_queue_mem.vaddr;
	spin_lock_init(&kci->resp_queue_lock);
	etdev_dbg(mgr->etdev, "%s: rspq kva=%pK iova=0x%llx dma=%pad", __func__,
		  kci->resp_queue_mem.vaddr, kci->resp_queue_mem.tpu_addr,
		  &kci->resp_queue_mem.dma_addr);

	mailbox->handle_irq = edgetpu_kci_handle_irq;
	mailbox->internal.kci = kci;
	kci->mailbox = mailbox;
	kci->cur_seq = 0;
	mutex_init(&kci->mailbox_lock);
	init_waitqueue_head(&kci->resp_doorbell_waitq);
	INIT_LIST_HEAD(&kci->wait_list);
	spin_lock_init(&kci->wait_list_lock);
	init_waitqueue_head(&kci->wait_list_waitq);
	INIT_WORK(&kci->work, edgetpu_kci_consume_responses_work);
	edgetpu_reverse_kci_init(&kci->rkci);
	EDGETPU_MAILBOX_CONTEXT_WRITE(mailbox, context_enable, 1);
	return 0;
}

int edgetpu_kci_reinit(struct edgetpu_kci *kci)
{
	struct edgetpu_mailbox *mailbox = kci->mailbox;
	int ret;

	if (!mailbox)
		return -ENODEV;
	ret = edgetpu_mailbox_set_queue(mailbox, MAILBOX_CMD_QUEUE,
					kci->cmd_queue_mem.tpu_addr,
					QUEUE_SIZE);
	if (ret)
		return ret;
	ret = edgetpu_mailbox_set_queue(mailbox, MAILBOX_RESP_QUEUE,
					kci->resp_queue_mem.tpu_addr,
					QUEUE_SIZE);
	if (ret)
		return ret;
	edgetpu_mailbox_init_doorbells(mailbox);
	EDGETPU_MAILBOX_CONTEXT_WRITE(mailbox, context_enable, 1);

	return 0;
}

void edgetpu_kci_cancel_work_queues(struct edgetpu_kci *kci)
{
	/* Cancel KCI and reverse KCI workers */
	cancel_work_sync(&kci->work);
	cancel_work_sync(&kci->rkci.work);
}

void edgetpu_kci_release(struct edgetpu_dev *etdev, struct edgetpu_kci *kci)
{
	if (!kci)
		return;
	/*
	 * Command/Response queues are managed (dmam_alloc_coherent()), we don't
	 * need to free them.
	 */

	edgetpu_kci_cancel_work_queues(kci);

	edgetpu_kci_free_queue(etdev, &kci->cmd_queue_mem);
	edgetpu_kci_free_queue(etdev, &kci->resp_queue_mem);

	/*
	 * Non-empty @kci->wait_list means someone (edgetpu_kci_send_cmd) is
	 * waiting for a response.
	 *
	 * Since this function is only called when removing a device,
	 * it should be impossible to reach here with edgetpu_kci_send_cmd() is
	 * still waiting (rmmod should fail), add a simple check here so we can
	 * more easily figure it out when this happens.
	 */
	if (!list_empty(&kci->wait_list))
		etdev_warn(etdev, "KCI commands still pending");
	/* detach the mailbox */
	kci->mailbox = NULL;
}

/*
 * Adds @resp to @kci->wait_list.
 *
 * wait_list is a FIFO queue, with sequence number in increasing order.
 *
 * Returns 0 on success, or -ENOMEM if failed on allocation.
 */
static int edgetpu_kci_push_wait_resp(struct edgetpu_kci *kci,
				      struct edgetpu_kci_response_element *resp)
{
	struct edgetpu_kci_wait_list *entry =
		kzalloc(sizeof(*entry), GFP_KERNEL);
	unsigned long flags;

	if (!entry)
		return -ENOMEM;
	entry->resp = resp;
	spin_lock_irqsave(&kci->wait_list_lock, flags);
	list_add_tail(&entry->list, &kci->wait_list);
	spin_unlock_irqrestore(&kci->wait_list_lock, flags);

	return 0;
}

/*
 * Removes the response previously pushed with edgetpu_kci_push_wait_resp().
 *
 * This is used when the kernel gives up waiting for the response.
 */
static void edgetpu_kci_del_wait_resp(struct edgetpu_kci *kci,
				      struct edgetpu_kci_response_element *resp)
{
	struct edgetpu_kci_wait_list *cur;
	unsigned long flags;

	spin_lock_irqsave(&kci->wait_list_lock, flags);

	list_for_each_entry(cur, &kci->wait_list, list) {
		if (cur->resp->seq > resp->seq)
			break;
		if (cur->resp->seq == resp->seq) {
			list_del(&cur->list);
			kfree(cur);
			break;
		}
	}

	spin_unlock_irqrestore(&kci->wait_list_lock, flags);
}

int edgetpu_kci_push_cmd(struct edgetpu_kci *kci,
			 struct edgetpu_command_element *cmd,
			 struct edgetpu_kci_response_element *resp)
{
	int ret;
	u32 tail;

	mutex_lock(&kci->cmd_queue_lock);

	cmd->seq = kci->cur_seq;
	/*
	 * The lock ensures mailbox->cmd_queue_tail cannot be changed by
	 * other processes (this method should be the only one to modify the
	 * value of tail), therefore we can remember its value here and use it
	 * in the condition of wait_event() call.
	 */
	tail = kci->mailbox->cmd_queue_tail;
	/*
	 * Waits until the cmd queue is not full.
	 * Response doorbell rung means remote might have consumed commands.
	 */
	ret = wait_event_timeout(
		       kci->resp_doorbell_waitq,
		       EDGETPU_MAILBOX_CMD_QUEUE_READ(kci->mailbox, head)
		       != (tail ^ CIRCULAR_QUEUE_WRAP_BIT),
		       msecs_to_jiffies(KCI_TIMEOUT));
	if (!ret) {
		ret = -ETIMEDOUT;
		goto out;
	}
	if (resp) {
		/*
		 * Add @resp to the wait_list only if the cmd can be pushed
		 * successfully.
		 */
		resp->seq = cmd->seq;
		resp->status = KCI_STATUS_WAITING_RESPONSE;
		ret = edgetpu_kci_push_wait_resp(kci, resp);
		if (ret)
			goto out;
	}
	/* size of cmd_queue is a multiple of sizeof(*cmd) */
	memcpy(kci->cmd_queue + CIRCULAR_QUEUE_REAL_INDEX(tail), cmd,
	       sizeof(*cmd));
	edgetpu_mailbox_inc_cmd_queue_tail(kci->mailbox, 1);
	/* triggers doorbell */
	EDGETPU_MAILBOX_CMD_QUEUE_WRITE_SYNC(kci->mailbox, doorbell_set, 1);
	/* bumps sequence number after the command is sent */
	kci->cur_seq++;
	ret = 0;
out:
	mutex_unlock(&kci->cmd_queue_lock);
	if (ret)
		etdev_dbg(kci->mailbox->etdev, "%s: ret=%d", __func__, ret);

	return ret;
}

/*
 * Pushes an element to cmd queue and waits for the response.
 * Returns -ETIMEDOUT if no response is received within KCI_TIMEOUT msecs.
 *
 * Returns the code of response, or a negative errno on error.
 * @resp is updated with the response, as to retrieve returned retval field.
 */
static int edgetpu_kci_send_cmd_return_resp(
	struct edgetpu_kci *kci, struct edgetpu_command_element *cmd,
	struct edgetpu_kci_response_element *resp)
{
	int ret;

	ret = edgetpu_kci_push_cmd(kci, cmd, resp);
	if (ret)
		return ret;
	ret = wait_event_timeout(kci->wait_list_waitq,
				 resp->status != KCI_STATUS_WAITING_RESPONSE,
				 msecs_to_jiffies(KCI_TIMEOUT));
	if (!ret) {
		etdev_dbg(kci->mailbox->etdev, "%s: event wait timeout",
			  __func__);
		edgetpu_kci_del_wait_resp(kci, resp);
		return -ETIMEDOUT;
	}
	if (resp->status != KCI_STATUS_OK) {
		etdev_dbg(kci->mailbox->etdev, "%s: resp status=%u", __func__,
			  resp->status);
		return -ENOMSG;
	}

	return resp->code;
}

int edgetpu_kci_send_cmd(struct edgetpu_kci *kci,
			 struct edgetpu_command_element *cmd)
{
	struct edgetpu_kci_response_element resp;

	return edgetpu_kci_send_cmd_return_resp(kci, cmd, &resp);
}

int edgetpu_kci_unmap_buffer(struct edgetpu_kci *kci, tpu_addr_t tpu_addr,
			     u32 size, enum dma_data_direction dir)
{
	struct edgetpu_command_element cmd = {
		.code = KCI_CODE_UNMAP_BUFFER,
		.dma = {
			.address = tpu_addr,
			.size = size,
			.flags = dir,
		},
	};

	return edgetpu_kci_send_cmd(kci, &cmd);
}

int edgetpu_kci_map_log_buffer(struct edgetpu_kci *kci, tpu_addr_t tpu_addr,
			       u32 size)
{
	struct edgetpu_command_element cmd = {
		.code = KCI_CODE_MAP_LOG_BUFFER,
		.dma = {
			.address = tpu_addr,
			.size = size,
		},
	};

	return edgetpu_kci_send_cmd(kci, &cmd);
}

int edgetpu_kci_map_trace_buffer(struct edgetpu_kci *kci, tpu_addr_t tpu_addr,
				 u32 size)
{
	struct edgetpu_command_element cmd = {
		.code = KCI_CODE_MAP_TRACE_BUFFER,
		.dma = {
			.address = tpu_addr,
			.size = size,
		},
	};

	return edgetpu_kci_send_cmd(kci, &cmd);
}

int edgetpu_kci_join_group(struct edgetpu_kci *kci, struct edgetpu_dev *etdev,
			   u8 n_dies, u8 vid)
{
	struct edgetpu_kci_device_group_detail *detail;
	const u32 size = sizeof(*detail);
	dma_addr_t dma_addr;
	tpu_addr_t tpu_addr;
	struct edgetpu_command_element cmd = {
		.code = KCI_CODE_JOIN_GROUP,
		.dma = {
			.size = size,
		},
	};
	const u32 flags = EDGETPU_MMU_DIE | EDGETPU_MMU_32 | EDGETPU_MMU_HOST;
	int ret;

	if (!kci)
		return -ENODEV;
	detail = dma_alloc_coherent(etdev->dev, sizeof(*detail), &dma_addr,
				    GFP_KERNEL);
	if (!detail)
		return -ENOMEM;
	detail->n_dies = n_dies;
	detail->vid = vid;

	tpu_addr = edgetpu_mmu_tpu_map(etdev, dma_addr, size, DMA_TO_DEVICE,
				       EDGETPU_CONTEXT_KCI, flags);
	if (!tpu_addr) {
		etdev_err(etdev, "%s: failed to map group detail to TPU",
			  __func__);
		dma_free_coherent(etdev->dev, size, detail, dma_addr);
		return -EINVAL;
	}

	cmd.dma.address = tpu_addr;
	etdev_dbg(etdev, "%s: map kva=%pK iova=0x%llx dma=%pad", __func__,
		  detail, tpu_addr, &dma_addr);

	ret = edgetpu_kci_send_cmd(kci, &cmd);
	edgetpu_mmu_tpu_unmap(etdev, tpu_addr, size, EDGETPU_CONTEXT_KCI);
	dma_free_coherent(etdev->dev, size, detail, dma_addr);
	etdev_dbg(etdev, "%s: unmap kva=%pK iova=0x%llx dma=%pad", __func__,
		  detail, tpu_addr, &dma_addr);

	return ret;
}

int edgetpu_kci_leave_group(struct edgetpu_kci *kci)
{
	struct edgetpu_command_element cmd = {
		.code = KCI_CODE_LEAVE_GROUP,
	};

	if (!kci)
		return -ENODEV;
	return edgetpu_kci_send_cmd(kci, &cmd);
}

enum edgetpu_fw_flavor edgetpu_kci_fw_info(struct edgetpu_kci *kci,
					   struct edgetpu_fw_info *fw_info)
{
	struct edgetpu_dev *etdev = kci->mailbox->etdev;
	struct edgetpu_command_element cmd = {
		.code = KCI_CODE_FIRMWARE_INFO,
		.dma = {
			.address = 0,
			.size = 0,
		},
	};
	struct edgetpu_coherent_mem mem;
	struct edgetpu_kci_response_element resp;
	enum edgetpu_fw_flavor flavor = FW_FLAVOR_UNKNOWN;
	int ret;

	ret = edgetpu_iremap_alloc(etdev, sizeof(*fw_info), &mem,
				   EDGETPU_CONTEXT_KCI);

	/* If allocation failed still try handshake without full fw_info */
	if (ret) {
		etdev_warn(etdev, "%s: error setting up fw info buffer: %d",
			   __func__, ret);
		memset(fw_info, 0, sizeof(*fw_info));
	} else {
		memset(mem.vaddr, 0, sizeof(*fw_info));
		cmd.dma.address = mem.tpu_addr;
		cmd.dma.size = sizeof(*fw_info);
	}

	ret = edgetpu_kci_send_cmd_return_resp(kci, &cmd, &resp);
	if (cmd.dma.address) {
		memcpy(fw_info, mem.vaddr, sizeof(*fw_info));
		edgetpu_iremap_free(etdev, &mem, EDGETPU_CONTEXT_KCI);
	}

	if (ret == KCI_ERROR_UNIMPLEMENTED) {
		etdev_dbg(etdev, "old firmware does not report flavor\n");
	} else if (ret == KCI_ERROR_OK) {
		switch (fw_info->fw_flavor) {
		case FW_FLAVOR_BL1:
		case FW_FLAVOR_SYSTEST:
		case FW_FLAVOR_PROD_DEFAULT:
		case FW_FLAVOR_CUSTOM:
			flavor = fw_info->fw_flavor;
			break;
		default:
			etdev_dbg(etdev, "unrecognized fw flavor 0x%x\n",
				  fw_info->fw_flavor);
		}
	} else {
		etdev_dbg(etdev, "firmware flavor query returns %d\n", ret);
		if (ret < 0)
			flavor = ret;
		else
			flavor = -EIO;
	}

	return flavor;
}

int edgetpu_kci_update_usage(struct edgetpu_dev *etdev)
{
#define EDGETPU_USAGE_BUFFER_SIZE	4096
	struct edgetpu_command_element cmd = {
		.code = KCI_CODE_GET_USAGE,
		.dma = {
			.address = 0,
			.size = 0,
		},
	};
	struct edgetpu_coherent_mem mem;
	struct edgetpu_kci_response_element resp;
	int ret;

	/* Quick return if device already powered down, else get PM ref. */
	if (!edgetpu_is_powered(etdev))
		return -EAGAIN;
	ret = edgetpu_pm_get(etdev->pm);
	if (ret)
		return ret;
	ret = edgetpu_iremap_alloc(etdev, EDGETPU_USAGE_BUFFER_SIZE, &mem,
				   EDGETPU_CONTEXT_KCI);

	if (ret) {
		etdev_warn_once(etdev, "%s: failed to allocate usage buffer",
				__func__);
		goto out;
	}

	cmd.dma.address = mem.tpu_addr;
	cmd.dma.size = EDGETPU_USAGE_BUFFER_SIZE;
	memset(mem.vaddr, 0, sizeof(struct edgetpu_usage_header));
	ret = edgetpu_kci_send_cmd_return_resp(etdev->kci, &cmd, &resp);

	if (ret == KCI_ERROR_UNIMPLEMENTED || ret == KCI_ERROR_UNAVAILABLE)
		etdev_dbg(etdev, "firmware does not report usage\n");
	else if (ret == KCI_ERROR_OK)
		edgetpu_usage_stats_process_buffer(etdev, mem.vaddr);
	else if (ret != -ETIMEDOUT)
		etdev_warn_once(etdev, "%s: error %d", __func__, ret);

	edgetpu_iremap_free(etdev, &mem, EDGETPU_CONTEXT_KCI);

out:
	edgetpu_pm_put(etdev->pm);
	return ret;
}

/* debugfs mappings dump */
void edgetpu_kci_mappings_show(struct edgetpu_dev *etdev, struct seq_file *s)
{
	struct edgetpu_kci *kci = etdev->kci;

	if (!kci || !kci->mailbox)
		return;

	seq_printf(s, "kci context %u:\n", EDGETPU_CONTEXT_KCI);
	seq_printf(s, "  0x%llx %lu cmdq - %pad\n",
		   kci->cmd_queue_mem.tpu_addr,
		   QUEUE_SIZE *
		   edgetpu_kci_queue_element_size(MAILBOX_CMD_QUEUE)
		   / PAGE_SIZE, &kci->cmd_queue_mem.dma_addr);
	seq_printf(s, "  0x%llx %lu rspq - %pad\n",
		   kci->resp_queue_mem.tpu_addr,
		   QUEUE_SIZE *
		   edgetpu_kci_queue_element_size(MAILBOX_RESP_QUEUE)
		   / PAGE_SIZE, &kci->resp_queue_mem.dma_addr);
	edgetpu_telemetry_mappings_show(etdev, s);
	edgetpu_firmware_mappings_show(etdev, s);
}

int edgetpu_kci_shutdown(struct edgetpu_kci *kci)
{
	struct edgetpu_command_element cmd = {
		.code = KCI_CODE_SHUTDOWN,
	};

	if (!kci)
		return -ENODEV;
	return edgetpu_kci_send_cmd(kci, &cmd);
}

int edgetpu_kci_get_debug_dump(struct edgetpu_kci *kci, tpu_addr_t tpu_addr,
			       size_t size)
{
	struct edgetpu_command_element cmd = {
		.code = KCI_CODE_GET_DEBUG_DUMP,
		.dma = {
			.address = tpu_addr,
			.size = size,
		},
	};

	if (!kci)
		return -ENODEV;
	return edgetpu_kci_send_cmd(kci, &cmd);
}

int edgetpu_kci_open_device(struct edgetpu_kci *kci, u32 mailbox_ids)
{
	struct edgetpu_command_element cmd = {
		.code = KCI_CODE_OPEN_DEVICE,
		.dma = {
			.flags = mailbox_ids,
		},
	};

	if (!kci)
		return -ENODEV;
	return edgetpu_kci_send_cmd(kci, &cmd);
}

int edgetpu_kci_close_device(struct edgetpu_kci *kci, u32 mailbox_ids)
{
	struct edgetpu_command_element cmd = {
		.code = KCI_CODE_CLOSE_DEVICE,
		.dma = {
			.flags = mailbox_ids,
		},
	};

	if (!kci)
		return -ENODEV;
	return edgetpu_kci_send_cmd(kci, &cmd);
}
