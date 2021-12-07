// SPDX-License-Identifier: GPL-2.0
/*
 * Implements methods common to the family of EdgeTPUs for mobile devices to retrieve host side
 * debug dump segments and report them to SSCD.
 *
 * Copyright (C) 2021 Google, Inc.
 */

#include <linux/mutex.h>
#include <linux/platform_data/sscoredump.h>
#include <linux/platform_device.h>
#include <linux/rbtree.h>
#include <linux/slab.h>

#include "edgetpu-config.h"
#include "edgetpu-device-group.h"
#include "edgetpu-mailbox.h"
#include "edgetpu-mobile-platform.h"
#include "mobile-debug-dump.h"

#include "edgetpu-debug-dump.c"

static void sscd_release(struct device *dev)
{
	pr_debug(DRIVER_NAME " release\n");
}

static struct sscd_platform_data sscd_pdata;
static struct platform_device sscd_dev;

/*
 * Collects the mapping information of all the host mapping and dmabuf mapping buffers of all
 * @groups as an array of struct mobile_sscd_mappings_dump and populates the @sscd_seg.
 *
 * Returns the pointer to the first element of the mappings dump array. The allocated array should
 * be freed by the caller after the sscd segment is reported.
 * Returns NULL in case of failure.
 */
static struct mobile_sscd_mappings_dump *
mobile_sscd_collect_mappings_segment(struct edgetpu_device_group **groups, size_t num_groups,
				     struct sscd_segment *sscd_seg)
{
	struct mobile_sscd_mappings_dump *mappings_dump;
	struct edgetpu_mapping_root *mappings;
	struct rb_node *node;
	void *resized_arr;
	size_t idx = 0, mappings_num = 0, new_size = 0;

	mappings_dump = kmalloc(sizeof(struct mobile_sscd_mappings_dump), GFP_KERNEL);
	for (idx = 0; idx < num_groups; idx++) {
		mutex_lock(&groups[idx]->lock);
		new_size +=
			groups[idx]->host_mappings.count * sizeof(struct mobile_sscd_mappings_dump);
		resized_arr = krealloc(mappings_dump, new_size, GFP_KERNEL);
		if (!resized_arr) {
			kfree(mappings_dump);
			mutex_unlock(&groups[idx]->lock);
			return NULL;
		}

		mappings = &groups[idx]->host_mappings;
		for (node = rb_first(&mappings->rb); node; node = rb_next(node)) {
			struct edgetpu_mapping *map =
				container_of(node, struct edgetpu_mapping, node);

			mappings_dump[mappings_num].host_address = map->host_address;
			mappings_dump[mappings_num].device_address = map->device_address;
			mappings_dump[mappings_num].alloc_iova = map->alloc_iova;
			mappings_dump[mappings_num].size = (u64)map->alloc_size;
			mappings_num++;
		}
		new_size += groups[idx]->dmabuf_mappings.count *
			    sizeof(struct mobile_sscd_mappings_dump);
		resized_arr = krealloc(mappings_dump, new_size, GFP_KERNEL);
		if (!resized_arr) {
			kfree(mappings_dump);
			mutex_unlock(&groups[idx]->lock);
			return NULL;
		}

		mappings = &groups[idx]->dmabuf_mappings;
		for (node = rb_first(&mappings->rb); node; node = rb_next(node)) {
			struct edgetpu_mapping *map =
				container_of(node, struct edgetpu_mapping, node);

			mappings_dump[mappings_num].host_address = map->host_address;
			mappings_dump[mappings_num].device_address = map->device_address;
			mappings_dump[mappings_num].alloc_iova = map->alloc_iova;
			mappings_dump[mappings_num].size = (u64)map->alloc_size;
			mappings_num++;
		}
		mutex_unlock(&groups[idx]->lock);
	}

	sscd_seg->addr = mappings_dump;
	sscd_seg->size = new_size;
	sscd_seg->vaddr = mappings_dump;

	return mappings_dump;
}

/*
 * Collects the VII cmd and resp queues of all @groups that @etdev belongs to and the KCI cmd and
 * resp queues and populates them as @sscd_seg_arr elements.
 *
 * Returns the total number of queues collected since some queues may have been released for groups
 * with detached mailboxes. The return value is less than or equal to the total number of queues
 * expected based on @num_groups i.e. (2 * @num_groups +2).
 */
static size_t mobile_sscd_collect_cmd_resp_queues(struct edgetpu_dev *etdev,
						  struct edgetpu_device_group **groups,
						  size_t num_groups,
						  struct sscd_segment *sscd_seg_arr)
{
	struct edgetpu_kci *kci;
	size_t idx;
	u16 num_queues = 0;

	/* Collect VII cmd and resp queues */
	for (idx = 0; idx < num_groups; idx++) {
		mutex_lock(&groups[idx]->lock);
		if (!edgetpu_group_mailbox_detached_locked(groups[idx])) {
			sscd_seg_arr[num_queues].addr =
				(void *)groups[idx]->vii.cmd_queue_mem.vaddr;
			sscd_seg_arr[num_queues].size = groups[idx]->vii.cmd_queue_mem.size;
			sscd_seg_arr[num_queues].paddr =
				(void *)groups[idx]->vii.cmd_queue_mem.tpu_addr;
			sscd_seg_arr[num_queues].vaddr =
				(void *)groups[idx]->vii.cmd_queue_mem.vaddr;
			num_queues++;

			sscd_seg_arr[num_queues].addr =
				(void *)groups[idx]->vii.resp_queue_mem.vaddr;
			sscd_seg_arr[num_queues].size = groups[idx]->vii.resp_queue_mem.size;
			sscd_seg_arr[num_queues].paddr =
				(void *)groups[idx]->vii.resp_queue_mem.tpu_addr;
			sscd_seg_arr[num_queues].vaddr =
				(void *)groups[idx]->vii.resp_queue_mem.vaddr;
			num_queues++;
		}
		mutex_unlock(&groups[idx]->lock);
	}

	/* Collect KCI cmd and resp queues */
	kci = etdev->kci;
	sscd_seg_arr[num_queues].addr = (void *)kci->cmd_queue_mem.vaddr;
	sscd_seg_arr[num_queues].size = MAX_QUEUE_SIZE * sizeof(struct edgetpu_command_element);
	sscd_seg_arr[num_queues].paddr = (void *)kci->cmd_queue_mem.tpu_addr;
	sscd_seg_arr[num_queues].vaddr = (void *)kci->cmd_queue_mem.vaddr;
	num_queues++;

	sscd_seg_arr[num_queues].addr = (void *)kci->resp_queue_mem.vaddr;
	sscd_seg_arr[num_queues].size =
		MAX_QUEUE_SIZE * sizeof(struct edgetpu_kci_response_element);
	sscd_seg_arr[num_queues].paddr = (void *)kci->resp_queue_mem.tpu_addr;
	sscd_seg_arr[num_queues].vaddr = (void *)kci->resp_queue_mem.vaddr;
	num_queues++;

	return num_queues;
}

static int mobile_sscd_generate_coredump(void *p_etdev, void *p_dump_setup)
{
	struct edgetpu_dev *etdev;
	struct edgetpu_debug_dump_setup *dump_setup;
	struct edgetpu_mobile_platform_dev *pdev;
	struct sscd_platform_data *pdata;
	struct platform_device *sscd_dev;
	struct sscd_segment *segs;
	struct edgetpu_debug_dump *debug_dump;
	struct edgetpu_crash_reason *crash_reason;
	struct edgetpu_dump_segment *dump_seg;
	struct edgetpu_device_group *group;
	struct edgetpu_device_group **groups;
	struct edgetpu_list_group *g;
	struct mobile_sscd_mappings_dump *mappings_dump = NULL;
	char crash_info[128];
	int sscd_dump_segments_num;
	int i, ret;
	size_t num_groups = 0, num_queues = 0;
	u64 offset;

	if (!p_etdev || !p_dump_setup)
		return -EINVAL;

	etdev = (struct edgetpu_dev *)p_etdev;
	dump_setup = (struct edgetpu_debug_dump_setup *)p_dump_setup;
	pdev = to_mobile_dev(etdev);
	pdata = (struct sscd_platform_data *)pdev->sscd_info.pdata;
	sscd_dev = (struct platform_device *)pdev->sscd_info.dev;
	if (!pdata->sscd_report) {
		etdev_err(etdev, "failed to generate coredump");
		return -ENOENT;
	}

	debug_dump = (struct edgetpu_debug_dump *)(dump_setup + 1);

	/* Populate crash reason */
	crash_reason =
		(struct edgetpu_crash_reason *)((u8 *)dump_setup + debug_dump->crash_reason_offset);
	scnprintf(crash_info, sizeof(crash_info), "[edgetpu_coredump] error code: %#llx",
		  crash_reason->code);

	mutex_lock(&etdev->groups_lock);
	groups = kmalloc_array(etdev->n_groups, sizeof(*groups), GFP_KERNEL);
	if (!groups) {
		mutex_unlock(&etdev->groups_lock);
		return -ENOMEM;
	}

	etdev_for_each_group(etdev, g, group) {
		if (edgetpu_device_group_is_disbanded(group))
			continue;
		groups[num_groups++] = edgetpu_device_group_get(group);
	}
	mutex_unlock(&etdev->groups_lock);

	/* Allocate memory for dump segments */
	sscd_dump_segments_num = debug_dump->dump_segments_num;
	sscd_dump_segments_num += 2 * num_groups; /* VII cmd and resp queues */
	sscd_dump_segments_num += num_groups ? 1 : 0; /* Mappings info */
	sscd_dump_segments_num += 2; /* KCI cmd and resp queues */

	segs = kmalloc_array(sscd_dump_segments_num, sizeof(struct sscd_segment), GFP_KERNEL);
	if (!segs) {
		ret = -ENOMEM;
		goto out_sscd_generate_coredump;
	}

	/* Populate sscd segments */
	dump_seg = (struct edgetpu_dump_segment *)((u8 *)dump_setup +
						   debug_dump->dump_segments_offset);
	offset = debug_dump->dump_segments_offset;
	for (i = 0; i < debug_dump->dump_segments_num; i++) {
		segs[i].addr = dump_seg;
		segs[i].size = sizeof(struct edgetpu_dump_segment) + dump_seg->size;
		segs[i].paddr = (void *)(etdev->debug_dump_mem.tpu_addr + offset);
		segs[i].vaddr = (void *)(etdev->debug_dump_mem.vaddr + offset);
		offset += sizeof(struct edgetpu_dump_segment) + dump_seg->size;
		dump_seg = (struct edgetpu_dump_segment *)((u8 *)dump_setup +
							   ALIGN(offset, sizeof(uint64_t)));
	}

	if (num_groups) {
		mappings_dump = mobile_sscd_collect_mappings_segment(groups, num_groups, &segs[i]);
		if (!mappings_dump) {
			ret = -ENOMEM;
			goto out_sscd_generate_coredump;
		}
		i++;
	}

	num_queues = mobile_sscd_collect_cmd_resp_queues(etdev, groups, num_groups, &segs[i]);

	/*
	 * Adjust num of segments as some groups may have a detached mailbox.
	 * Subtract number of VII and KCI queues according to num_groups.
	 */
	sscd_dump_segments_num -= (2 * num_groups + 2);
	sscd_dump_segments_num += num_queues; /* Add actual number of valid VII and KCI queues */

	/* Pass dump data to SSCD daemon */
	etdev_dbg(etdev, "report: %d segments", sscd_dump_segments_num);
	ret = pdata->sscd_report(sscd_dev, segs, sscd_dump_segments_num, SSCD_FLAGS_ELFARM64HDR,
				 crash_info);
out_sscd_generate_coredump:
	for (i = 0; i < num_groups; i++)
		edgetpu_device_group_put(groups[i]);
	kfree(mappings_dump);
	kfree(segs);
	kfree(groups);

	return ret;
}

int edgetpu_debug_dump_init(struct edgetpu_dev *etdev)
{
	size_t size;
	int ret;
	struct edgetpu_debug_dump_setup *dump_setup;
	struct edgetpu_mobile_platform_dev *pdev;

	pdev = to_mobile_dev(etdev);

	size = EDGETPU_DEBUG_DUMP_MEM_SIZE;

	sscd_dev = (struct platform_device) {
		.name = DRIVER_NAME,
		.driver_override = SSCD_NAME,
		.id = PLATFORM_DEVID_NONE,
		.dev = {
			.platform_data = &sscd_pdata,
			.release = sscd_release,
		},
	};
	/* Register SSCD platform device */
	ret = platform_device_register(&sscd_dev);
	if (ret) {
		etdev_err(etdev, "SSCD platform device registration failed: %d", ret);
		return ret;
	}
	/*
	 * Allocate a buffer for various dump segments
	 */
	ret = edgetpu_alloc_coherent(etdev, size, &etdev->debug_dump_mem, EDGETPU_CONTEXT_KCI);
	if (ret) {
		etdev_err(etdev, "Debug dump seg alloc failed");
		etdev->debug_dump_mem.vaddr = NULL;
		goto out_unregister_platform;
	}
	dump_setup = (struct edgetpu_debug_dump_setup *)etdev->debug_dump_mem.vaddr;
	memset(dump_setup, 0, size);
	dump_setup->dump_mem_size = size;

	/*
	 * Allocate memory for debug dump handlers
	 */
	etdev->debug_dump_handlers =
		kcalloc(DUMP_REASON_NUM, sizeof(*etdev->debug_dump_handlers), GFP_KERNEL);
	if (!etdev->debug_dump_handlers)
		return -ENOMEM;
	etdev->debug_dump_handlers[DUMP_REASON_REQ_BY_USER] = mobile_sscd_generate_coredump;

	pdev->sscd_info.pdata = &sscd_pdata;
	pdev->sscd_info.dev = &sscd_dev;
	return ret;
out_unregister_platform:
	platform_device_unregister(&sscd_dev);
	return ret;
}

void edgetpu_debug_dump_exit(struct edgetpu_dev *etdev)
{
	if (!etdev->debug_dump_mem.vaddr) {
		etdev_dbg(etdev, "Debug dump not allocated");
		return;
	}
	/*
	 * Free the memory assigned for debug dump
	 */
	edgetpu_free_coherent(etdev, &etdev->debug_dump_mem, EDGETPU_CONTEXT_KCI);
	kfree(etdev->debug_dump_handlers);
	platform_device_unregister(&sscd_dev);
}
