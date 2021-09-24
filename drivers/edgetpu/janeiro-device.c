// SPDX-License-Identifier: GPL-2.0
/*
 * Janeiro Edge TPU ML accelerator device host support.
 *
 * Copyright (C) 2020 Google, Inc.
 */

#include <linux/irqreturn.h>
#include <linux/uaccess.h>

#include "edgetpu-config.h"
#include "edgetpu-internal.h"
#include "edgetpu-mailbox.h"
#include "edgetpu-mobile-platform.h"
#include "edgetpu-telemetry.h"
#include "mobile-pm.h"

#define SSMT_NS_READ_STREAM_VID_OFFSET(n) (0x1000u + (0x4u * (n)))
#define SSMT_NS_WRITE_STREAM_VID_OFFSET(n) (0x1200u + (0x4u * (n)))

#define SSMT_NS_READ_STREAM_VID_REG(base, n)                                   \
	((base) + SSMT_NS_READ_STREAM_VID_OFFSET(n))
#define SSMT_NS_WRITE_STREAM_VID_REG(base, n)                                  \
	((base) + SSMT_NS_WRITE_STREAM_VID_OFFSET(n))

static irqreturn_t janeiro_mailbox_handle_irq(struct edgetpu_dev *etdev,
					      int irq)
{
	struct edgetpu_mailbox *mailbox;
	struct edgetpu_mobile_platform_dev *etmdev = to_mobile_dev(etdev);
	struct edgetpu_mailbox_manager *mgr = etdev->mailbox_manager;
	uint i;

	if (!mgr)
		return IRQ_NONE;
	for (i = 0; i < etmdev->n_irq; i++)
		if (etmdev->irq[i] == irq)
			break;
	if (i == etmdev->n_irq)
		return IRQ_NONE;
	read_lock(&mgr->mailboxes_lock);
	mailbox = mgr->mailboxes[i];
	if (!mailbox)
		goto out;
	if (!EDGETPU_MAILBOX_RESP_QUEUE_READ(mailbox, doorbell_status))
		goto out;
	EDGETPU_MAILBOX_RESP_QUEUE_WRITE(mailbox, doorbell_clear, 1);
	etdev_dbg(mgr->etdev, "mbox %u resp doorbell irq tail=%u\n", i,
		  EDGETPU_MAILBOX_RESP_QUEUE_READ(mailbox, tail));
	if (mailbox->handle_irq)
		mailbox->handle_irq(mailbox);
out:
	read_unlock(&mgr->mailboxes_lock);
	return IRQ_HANDLED;
}

irqreturn_t edgetpu_chip_irq_handler(int irq, void *arg)
{
	struct edgetpu_dev *etdev = arg;

	edgetpu_telemetry_irq_handler(etdev);
	return janeiro_mailbox_handle_irq(etdev, irq);
}

u64 edgetpu_chip_tpu_timestamp(struct edgetpu_dev *etdev)
{
	return edgetpu_dev_read_64(etdev, EDGETPU_REG_CPUNS_TIMESTAMP);
}

void edgetpu_chip_init(struct edgetpu_dev *etdev)
{
	int i;
	struct edgetpu_mobile_platform_dev *etmdev = to_mobile_dev(etdev);

	/*
	 * This only works if the SSMT is set to client-driven mode, which only GSA can do.
	 * Skip if GSA is not available
	 */
	if (!etmdev->ssmt_base || !etmdev->gsa_dev)
		return;

	/* Setup non-secure SCIDs, assume VID = SCID */
	for (i = 0; i < EDGETPU_NCONTEXTS; i++) {
		writel(i, SSMT_NS_READ_STREAM_VID_REG(etmdev->ssmt_base, i));
		writel(i, SSMT_NS_WRITE_STREAM_VID_REG(etmdev->ssmt_base, i));
	}
}

void edgetpu_chip_exit(struct edgetpu_dev *etdev)
{
}

void edgetpu_mark_probe_fail(struct edgetpu_dev *etdev)
{
}

void edgetpu_chip_handle_reverse_kci(struct edgetpu_dev *etdev,
				    struct edgetpu_kci_response_element *resp)
{
	switch (resp->code) {
	case RKCI_CODE_PM_QOS:
		mobile_pm_set_pm_qos(etdev, resp->retval);
		break;
	case RKCI_CODE_BTS:
		mobile_pm_set_bts(etdev, resp->retval);
		break;
	default:
		etdev_warn(etdev, "%s: Unrecognized KCI request: %u\n",
			   __func__, resp->code);
		break;
	}
}

int edgetpu_chip_acquire_ext_mailbox(struct edgetpu_client *client,
				     struct edgetpu_ext_mailbox_ioctl *args)
{
	struct edgetpu_external_mailbox_req req;

	if (args->type == EDGETPU_EXT_MAILBOX_TYPE_DSP) {
		if (!args->count || args->count > EDGETPU_NUM_EXT_MAILBOXES)
			return -EINVAL;
		if (copy_from_user(&req.attr, (void __user *)args->attrs, sizeof(req.attr)))
			return -EFAULT;
		req.count = args->count;
		req.start = JANEIRO_EXT_DSP_MAILBOX_START;
		req.end = JANEIRO_EXT_DSP_MAILBOX_END;
		req.client_type = EDGETPU_EXT_MAILBOX_TYPE_DSP;
		return edgetpu_mailbox_enable_ext(client, EDGETPU_MAILBOX_ID_USE_ASSOC, &req);
	}
	return -ENODEV;
}

int edgetpu_chip_release_ext_mailbox(struct edgetpu_client *client,
				     struct edgetpu_ext_mailbox_ioctl *args)
{
	if (args->type == EDGETPU_EXT_MAILBOX_TYPE_DSP)
		return edgetpu_mailbox_disable_ext(client, EDGETPU_MAILBOX_ID_USE_ASSOC);
	return -ENODEV;
}

void edgetpu_chip_client_remove(struct edgetpu_client *client)
{
}
