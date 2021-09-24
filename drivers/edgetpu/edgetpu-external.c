// SPDX-License-Identifier: GPL-2.0
/*
 * Utility functions for interfacing other modules with Edge TPU ML accelerator.
 *
 * Copyright (C) 2021 Google, Inc.
 */

#include <linux/device.h>
#include <linux/err.h>

#include <soc/google/tpu-ext.h>

#include "edgetpu-config.h"
#include "edgetpu-device-group.h"
#include "edgetpu-internal.h"
#include "edgetpu-mailbox.h"

static int edgetpu_external_mailbox_info_get(struct edgetpu_external_mailbox_info *info,
					     struct edgetpu_external_mailbox *ext_mailbox)
{
	int i;
	u32 count = ext_mailbox->count;
	struct edgetpu_mailbox_descriptor *desc;

	if (!info)
		return -EINVAL;

	if (info->count < count) {
		etdev_err(ext_mailbox->etdev,
			  "Insufficient space in provided buffer expected: %d received: %d\n",
			  count, info->count);
		return -ENOMEM;
	}

	for (i = 0; i < count; i++) {
		desc = &ext_mailbox->descriptors[i];
		info->mailboxes[i].cmdq_pa = desc->cmd_queue_mem.phys_addr;
		info->mailboxes[i].respq_pa = desc->resp_queue_mem.phys_addr;
		info->mailboxes[i].mailbox_id =
			desc->mailbox->mailbox_id - (EDGETPU_NUM_VII_MAILBOXES + 1);
	}

	info->cmdq_size = ext_mailbox->attr.cmd_queue_size;
	info->respq_size = ext_mailbox->attr.resp_queue_size;
	info->count = count;

	return 0;
}

static bool is_edgetpu_valid_client(struct edgetpu_external_mailbox *ext_mailbox,
				    enum edgetpu_external_client_type client_type)
{
	switch (client_type) {
	case EDGETPU_EXTERNAL_CLIENT_TYPE_DSP:
		return ext_mailbox->client_type == EDGETPU_EXT_MAILBOX_TYPE_DSP;
	default:
		return false;
	}

	return false;
}

static int edgetpu_mailbox_external_info_get_cmd(struct device *edgetpu_dev,
						 enum edgetpu_external_client_type client_type,
						 int *client_fd,
						 struct edgetpu_external_mailbox_info *info)
{
	struct edgetpu_client *client;
	struct edgetpu_device_group *group;
	struct edgetpu_external_mailbox *ext_mailbox;
	int ret = 0;
	struct fd f = fdget(*client_fd);
	struct file *file = f.file;

	if (!file)
		return -ENOENT;

	if (!is_edgetpu_file(file)) {
		ret = -ENOENT;
		goto out;
	}

	client = file->private_data;
	if (!client) {
		ret = -EINVAL;
		goto out;
	}

	mutex_lock(&client->group_lock);
	if (!client->group || client->etdev->dev != edgetpu_dev) {
		ret = -EINVAL;
		mutex_unlock(&client->group_lock);
		goto out;
	}
	group = edgetpu_device_group_get(client->group);
	mutex_unlock(&client->group_lock);

	mutex_lock(&group->lock);
	ext_mailbox = group->ext_mailbox;
	if (!ext_mailbox) {
		ret = -ENOENT;
		goto unlock;
	}
	if (!is_edgetpu_valid_client(ext_mailbox, client_type)) {
		ret = -EINVAL;
		goto unlock;
	}
	ret = edgetpu_external_mailbox_info_get(info, ext_mailbox);
unlock:
	mutex_unlock(&group->lock);
	edgetpu_device_group_put(group);
out:
	fdput(f);
	return ret;
}

int edgetpu_ext_driver_cmd(struct device *edgetpu_dev,
			   enum edgetpu_external_client_type client_type,
			   enum edgetpu_external_commands cmd_id, void *in_data, void *out_data)
{
	int ret = 0;

	switch (cmd_id) {
	case MAILBOX_EXTERNAL_INFO_GET:
		ret = edgetpu_mailbox_external_info_get_cmd(edgetpu_dev, client_type, in_data,
							    out_data);
		break;
	default:
		return -ENOENT;
	}

	return ret;
}
EXPORT_SYMBOL_GPL(edgetpu_ext_driver_cmd);
