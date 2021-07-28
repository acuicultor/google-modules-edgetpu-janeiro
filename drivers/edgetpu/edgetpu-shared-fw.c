// SPDX-License-Identifier: GPL-2.0
/*
 * Edge TPU shared firmware management.
 *
 * Copyright (C) 2020 Google, Inc.
 */

#include <linux/device.h>
#include <linux/firmware.h>
#include <linux/list.h>
#include <linux/mutex.h>
#include <linux/refcount.h>
#include <linux/slab.h>
#include <linux/string.h>

#include "edgetpu-firmware.h"
#include "edgetpu-firmware-util.h"
#include "edgetpu-internal.h"
#include "edgetpu-shared-fw.h"

struct edgetpu_shared_fw_buffer {
	/*
	 * Shared firmware buffer is managed by `global.firmware_list`, so that
	 * each data member is protected by `global.lock`.
	 */
	struct list_head list;
	/*
	 * Counting for devices holding the buffer. We can only release the data
	 * buffer if there is no device nor sysfs holding the firmware.
	 *
	 * Even when the reference count atomically decreased down to 0, there's
	 * a chance that someone is traversing list and trying to read this
	 * `ref`.  So `ref` must still be protected by `glock.lock` in this
	 * case.
	 */
	refcount_t ref;
	/*
	 * Indicates if this buffer is loaded by sysfs.
	 *
	 * Reference count caused by sysfs load should be exactly 1, and we can
	 * only unload firmware by sysfs if already loaded by sysfs.
	 */
	bool is_sysfs_loaded;
	/* Firmware name, the same as that loaded by request_firmware() API. */
	const char *name;
	void *vaddr;
	/* The size of buffer is aligned to `global.init_data.size_align`. */
	size_t size;
};

const char *
edgetpu_shared_fw_buffer_name(const struct edgetpu_shared_fw_buffer *buffer)
{
	return buffer->name;
}

void *
edgetpu_shared_fw_buffer_vaddr(const struct edgetpu_shared_fw_buffer *buffer)
{
	return buffer->vaddr;
}

size_t
edgetpu_shared_fw_buffer_size(const struct edgetpu_shared_fw_buffer *buffer)
{
	return buffer->size;
}

/*
 * Lock protected global data.
 *
 * global.lock is required for invoking _locked functions in this file.
 */
static struct {
	struct mutex lock;
	struct edgetpu_shared_fw_init_data init_data;
	struct list_head firmware_list;
} global = {
	.lock = __MUTEX_INITIALIZER(global.lock),
	.firmware_list = LIST_HEAD_INIT(global.firmware_list),
};

#define for_each_shared_fw_buffer(buffer) \
	list_for_each_entry(buffer, &global.firmware_list, list)
#define for_each_shared_fw_buffer_safe(cur_buf, nxt_buf) \
	list_for_each_entry_safe(cur_buf, nxt_buf, &global.firmware_list, list)

static struct edgetpu_shared_fw_buffer *
edgetpu_shared_fw_find_locked(const char *name)
{
	struct edgetpu_shared_fw_buffer *buffer;

	for_each_shared_fw_buffer(buffer) {
		if (!strcmp(name, buffer->name))
			return buffer;
	}
	return NULL;
}

void
edgetpu_shared_fw_init(const struct edgetpu_shared_fw_init_data *init_data)
{
	if (list_empty(&global.firmware_list))
		global.init_data = *init_data;
}

void edgetpu_shared_fw_exit(void)
{
	struct edgetpu_shared_fw_buffer *cur_buf, *nxt_buf;

	mutex_lock(&global.lock);

	if (!list_empty(&global.firmware_list))
		pr_warn("%s: firmware not released on exiting\n", __func__);

	for_each_shared_fw_buffer_safe(cur_buf, nxt_buf)
		list_del(&cur_buf->list);

	mutex_unlock(&global.lock);
}

static struct edgetpu_shared_fw_buffer *
edgetpu_shared_fw_get_locked(struct edgetpu_shared_fw_buffer *buffer)
{
	if (!buffer)
		return NULL;
	if (!refcount_inc_not_zero(&buffer->ref))
		return NULL;
	return buffer;
}

struct edgetpu_shared_fw_buffer *
edgetpu_shared_fw_get(struct edgetpu_shared_fw_buffer *buffer)
{
	mutex_lock(&global.lock);
	buffer = edgetpu_shared_fw_get_locked(buffer);
	mutex_unlock(&global.lock);
	return buffer;
}

struct edgetpu_shared_fw_buffer *
edgetpu_shared_fw_get_by_name(const char *name)
{
	struct edgetpu_shared_fw_buffer *buffer;

	mutex_lock(&global.lock);
	buffer = edgetpu_shared_fw_get_locked(
			edgetpu_shared_fw_find_locked(name));
	mutex_unlock(&global.lock);
	return buffer;
}

static struct edgetpu_shared_fw_buffer *
edgetpu_shared_fw_load_locked(const char *name, struct edgetpu_dev *etdev)
{
	int ret;
	const struct firmware *fw;
	size_t aligned_size;
	struct edgetpu_shared_fw_buffer *buffer;

	buffer = edgetpu_shared_fw_get_locked(
			edgetpu_shared_fw_find_locked(name));
	if (buffer) {
		pr_debug("%s: found shared fw image %s\n", __func__, name);
		return buffer;
	}

	pr_debug("%s: shared fw image %s not found, requesting\n",
		 __func__, name);
	ret = request_firmware(&fw, name, etdev ? etdev->etiface->etcdev : NULL);
	if (ret)
		goto out;

	aligned_size = ALIGN(fw->size, global.init_data.size_align);

	buffer = kzalloc(sizeof(*buffer), GFP_KERNEL);
	if (!buffer) {
		ret = -ENOMEM;
		goto out_release_firmware;
	}

	buffer->name = kstrdup(name, GFP_KERNEL);
	if (!buffer->name) {
		ret = -ENOMEM;
		goto out_kfree_buffer;
	}

	/* Allocated in page alignment for mmu and dma mapping. */
	if (aligned_size < PAGE_SIZE)
		buffer->vaddr = kmalloc_order(aligned_size, GFP_KERNEL, 1);
	else
		buffer->vaddr = kmalloc(aligned_size, GFP_KERNEL);
	if (!buffer->vaddr) {
		ret = -ENOMEM;
		goto out_kfree_buffer_name;
	}
	memcpy(buffer->vaddr, fw->data, fw->size);
	release_firmware(fw);

	buffer->size = aligned_size;
	refcount_set(&buffer->ref, 1);

	list_add(&buffer->list, &global.firmware_list);
	return buffer;

out_kfree_buffer_name:
	kfree(buffer->name);
out_kfree_buffer:
	kfree(buffer);
out_release_firmware:
	release_firmware(fw);
out:
	return ERR_PTR(ret);
}

struct edgetpu_shared_fw_buffer *edgetpu_shared_fw_load(
	const char *name, struct edgetpu_dev *etdev)
{
	struct edgetpu_shared_fw_buffer *buffer;

	mutex_lock(&global.lock);
	buffer = edgetpu_shared_fw_load_locked(name, etdev);
	mutex_unlock(&global.lock);
	return buffer;
}

int edgetpu_firmware_chip_load_locked(
		struct edgetpu_firmware *et_fw,
		struct edgetpu_firmware_desc *fw_desc, const char *name)
{
	int ret;
	struct edgetpu_dev *etdev = et_fw->etdev;
	struct edgetpu_shared_fw_buffer *shared_buf;

	shared_buf = edgetpu_shared_fw_load(name, etdev);
	if (IS_ERR(shared_buf)) {
		ret = PTR_ERR(shared_buf);
		etdev_dbg(etdev, "shared buffer loading failed: %d\n", ret);
		return ret;
	}
	fw_desc->shared_buf = shared_buf;
	fw_desc->buf.vaddr = edgetpu_shared_fw_buffer_vaddr(shared_buf);
	fw_desc->buf.alloc_size = edgetpu_shared_fw_buffer_size(shared_buf);
	fw_desc->buf.used_size = fw_desc->buf.alloc_size;
	fw_desc->buf.name = edgetpu_shared_fw_buffer_name(shared_buf);
	return 0;
}

void edgetpu_firmware_chip_unload_locked(
		struct edgetpu_firmware *et_fw,
		struct edgetpu_firmware_desc *fw_desc)
{
	fw_desc->buf.vaddr = NULL;
	fw_desc->buf.alloc_size = 0;
	fw_desc->buf.used_size = 0;
	fw_desc->buf.name = NULL;
	edgetpu_shared_fw_put(fw_desc->shared_buf);
	fw_desc->shared_buf = NULL;
}

static void
edgetpu_shared_fw_put_locked(struct edgetpu_shared_fw_buffer *buffer)
{
	if (!buffer)
		return;

	/*
	 * buffer->ref IS protected by global.lock. See also `ref` in `struct
	 * edgetpu_shared_fw_buffer`.
	 */
	if (refcount_dec_and_test(&buffer->ref)) {
		kfree(buffer->vaddr);
		kfree(buffer->name);
		list_del(&buffer->list);
		kfree(buffer);
	}
}

void edgetpu_shared_fw_put(struct edgetpu_shared_fw_buffer *buffer)
{
	mutex_lock(&global.lock);
	edgetpu_shared_fw_put_locked(buffer);
	mutex_unlock(&global.lock);
}
