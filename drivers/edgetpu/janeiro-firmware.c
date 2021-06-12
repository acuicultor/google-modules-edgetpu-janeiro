// SPDX-License-Identifier: GPL-2.0
/*
 * Janeiro Edge TPU ML accelerator firmware download support.
 *
 * Copyright (C) 2020 Google, Inc.
 */

#include <linux/dma-mapping.h>
#include <linux/sizes.h>
#include <linux/slab.h>

#include <linux/iommu.h>

#include "edgetpu.h"
#include "edgetpu-config.h"
#include "edgetpu-firmware.h"
#include "edgetpu-internal.h"
#include "edgetpu-mmu.h"
#include "edgetpu-kci.h"
#include "edgetpu-mailbox.h"
#include "janeiro-platform.h"
#include "mobile-firmware.h"

#define MAX_IOMMU_MAPPINGS 26

#define CONFIG_TO_SIZE(a) ((1 << ((a) & 0xFFF)) << 12)

struct iommu_mapping {
	/* TPU virt address */
	__u32 virt_address;
	/*
	 * contains a 12-bit aligned address and a page-order size into a
	 * 32-bit value i.e. a physical address and size in page order.
	 */
	__u32 image_config_value;
};

struct janeiro_image_config {
	__u32 carveout_base;
	__u32 firmware_base;
	__u32 firmware_size;
	struct edgetpu_fw_version firmware_versions;
	__u32 config_version;
	__u32 privilege_level;
	__u32 remapped_region_start;
	__u32 remapped_region_end;
	__u32 num_iommu_mapping;
	struct iommu_mapping mappings[MAX_IOMMU_MAPPINGS];
} __packed;

struct janeiro_firmware_data {
	__u32 num_mapping;
	struct iommu_mapping mappings[MAX_IOMMU_MAPPINGS];
};
/*
 * Sets the reset state of the TPU CPU.
 * @val: 1 to put the core in reset state, 0 to release core from reset state.
 */
static void tpu_cpu_reset(struct edgetpu_dev *etdev, u64 val)
{
	edgetpu_dev_write_32_sync(etdev, EDGETPU_REG_RESET_CONTROL, val);
}

static int janeiro_firmware_after_create(struct edgetpu_firmware *et_fw)
{
	struct janeiro_firmware_data *data;

	data = kzalloc(sizeof(*data), GFP_KERNEL);
	if (!data)
		return -ENOMEM;

	edgetpu_firmware_set_data(et_fw, data);
	return 0;
}

static void janeiro_firmware_before_destroy(struct edgetpu_firmware *et_fw)
{
	struct janeiro_firmware_data *data;
	u32 i, tpu_addr, size;
	struct edgetpu_dev *etdev = et_fw->etdev;

	tpu_cpu_reset(et_fw->etdev, 1);
	/* TODO(b/189906347): Remove when GSA/TZ support is available. */
	/* Remove mappings created by setup_buffer() */
	data = edgetpu_firmware_get_data(et_fw);

	if (data) {
		for (i = 0; i < data->num_mapping; i++) {
			tpu_addr = data->mappings[i].virt_address;
			size = CONFIG_TO_SIZE(data->mappings[i].image_config_value);
			edgetpu_mmu_remove_translation(etdev, tpu_addr, size,
						       EDGETPU_CONTEXT_KCI);
		}
		edgetpu_firmware_set_data(et_fw, NULL);
		kfree(data);
	}
}

static int janeiro_firmware_alloc_buffer(struct edgetpu_firmware *et_fw,
					 struct edgetpu_firmware_buffer *fw_buf)
{
	struct edgetpu_dev *etdev = et_fw->etdev;
	struct janeiro_platform_dev *edgetpu_pdev =
		container_of(etdev, struct janeiro_platform_dev, edgetpu_dev);
	/* Allocate extra space for the image header */
	size_t buffer_size =
		edgetpu_pdev->fw_region_size + MOBILE_FW_HEADER_SIZE;

	fw_buf->vaddr = kzalloc(buffer_size, GFP_KERNEL);
	if (!fw_buf->vaddr) {
		etdev_err(etdev, "%s: failed to allocate buffer (%zu bytes)\n",
			  __func__, buffer_size);
		return -ENOMEM;
	}
	fw_buf->dma_addr = 0;
	fw_buf->alloc_size = buffer_size;
	fw_buf->used_size_align = 16;
	return 0;
}

static void janeiro_firmware_free_buffer(struct edgetpu_firmware *et_fw,
					 struct edgetpu_firmware_buffer *fw_buf)
{
	kfree(fw_buf->vaddr);
	fw_buf->vaddr = NULL;
	fw_buf->alloc_size = 0;
	fw_buf->used_size_align = 0;
}

static int janeiro_firmware_setup_buffer(struct edgetpu_firmware *et_fw,
					 struct edgetpu_firmware_buffer *fw_buf)
{
	int ret = 0;
	void *image_vaddr;
	u32 tpu_addr, phys_addr, size, i;
	struct janeiro_image_config *image_config;
	struct janeiro_firmware_data *data;
	struct edgetpu_dev *etdev = et_fw->etdev;
	struct janeiro_platform_dev *edgetpu_pdev =
		container_of(etdev, struct janeiro_platform_dev, edgetpu_dev);

	if (fw_buf->used_size < MOBILE_FW_HEADER_SIZE) {
		etdev_err(etdev, "Invalid buffer size: %zu < %d\n",
			  fw_buf->used_size, MOBILE_FW_HEADER_SIZE);
		return -EINVAL;
	}

	image_vaddr = memremap(edgetpu_pdev->fw_region_paddr,
			       edgetpu_pdev->fw_region_size, MEMREMAP_WC);
	if (!image_vaddr) {
		etdev_err(etdev, "memremap failed\n");
		return -ENOMEM;
	}

	/* fetch the firmware versions */
	image_config = fw_buf->vaddr + MOBILE_IMAGE_CONFIG_OFFSET;
	memcpy(&etdev->fw_version, &image_config->firmware_versions,
	       sizeof(etdev->fw_version));

	/* TODO(b/189906347): Remove when GSA/TZ support is available. */
	data = edgetpu_firmware_get_data(et_fw);
	/* Remove old mappings created for previous firmware. */
	for (i = 0; i < data->num_mapping; i++) {
		tpu_addr = data->mappings[i].virt_address;
		size = CONFIG_TO_SIZE(data->mappings[i].image_config_value);
		phys_addr = (data->mappings[i].image_config_value & ~(0xFFF));

		edgetpu_mmu_remove_translation(etdev, tpu_addr, size, EDGETPU_CONTEXT_KCI);
	}
	for (i = 0; i < image_config->num_iommu_mapping; i++) {
		tpu_addr = image_config->mappings[i].virt_address;
		size = CONFIG_TO_SIZE(image_config->mappings[i].image_config_value);
		phys_addr = (image_config->mappings[i].image_config_value & ~(0xFFF));

		ret = edgetpu_mmu_add_translation(etdev, tpu_addr, phys_addr, size,
						  IOMMU_READ | IOMMU_WRITE, EDGETPU_CONTEXT_KCI);
		if (ret) {
			etdev_err(etdev,
				  "Unable to Map: %d tpu_addr: 0x%x phys_addr: 0x%x size: 0x%x\n",
				  ret, tpu_addr, phys_addr, size);
			goto err;
		}
		data->mappings[i].virt_address = tpu_addr;
		data->mappings[i].image_config_value = image_config->mappings[i].image_config_value;
	}

	data->num_mapping = image_config->num_iommu_mapping;

	/* Skip the header */
	memcpy(image_vaddr, fw_buf->vaddr + MOBILE_FW_HEADER_SIZE,
	       fw_buf->used_size - MOBILE_FW_HEADER_SIZE);
	memunmap(image_vaddr);
	return 0;
err:
	while (i--) {
		tpu_addr = data->mappings[i].virt_address;
		size = CONFIG_TO_SIZE(data->mappings[i].image_config_value);
		edgetpu_mmu_remove_translation(etdev, tpu_addr, size, EDGETPU_CONTEXT_KCI);
	}
	data->num_mapping = 0;
	memunmap(image_vaddr);
	return ret;
}

static void
janeiro_firmware_teardown_buffer(struct edgetpu_firmware *et_fw,
				 struct edgetpu_firmware_buffer *fw_buf)
{
}

static int janeiro_firmware_prepare_run(struct edgetpu_firmware *et_fw,
					struct edgetpu_firmware_buffer *fw_buf)
{
	struct edgetpu_dev *etdev = et_fw->etdev;
	struct janeiro_platform_dev *edgetpu_pdev =
		container_of(etdev, struct janeiro_platform_dev, edgetpu_dev);
	dma_addr_t fw_dma_addr;
	int ret = 0;

	fw_dma_addr = edgetpu_pdev->fw_region_paddr;

	//TODO: enable as and when sysmmu started working correctly on hybrid
	//platform
#if 0
	/* Clear Substream ID (aka SCID) for instruction remapped addresses */
	u32 sec_reg = edgetpu_dev_read_32(
			etdev, EDGETPU_REG_INSTRUCTION_REMAP_SECURITY);
	sec_reg &= ~(0x0F << 16);
	edgetpu_dev_write_32(etdev, EDGETPU_REG_INSTRUCTION_REMAP_SECURITY,
			sec_reg);

	/* Clear Substream ID (aka SCID) for all other addresses */
	sec_reg = edgetpu_dev_read_32(etdev, EDGETPU_REG_SECURITY);
	sec_reg &= ~(0x0F << 16);
	edgetpu_dev_write_32(etdev, EDGETPU_REG_SECURITY, sec_reg);
//	dma_sync_single_for_device(etdev->dev, fw_dma_addr,
//				   fw_buf->used_size - JANEIRO_FW_HEADER_SIZE,
//				   DMA_TO_DEVICE);
#endif
	tpu_cpu_reset(etdev, 1);

	/* Reset KCI mailbox before starting f/w, don't process anything old.*/
	edgetpu_mailbox_reset(etdev->kci->mailbox);

	tpu_cpu_reset(etdev, 0);
	//TODO: cleanup
	return ret;
}

static const struct edgetpu_firmware_handlers janeiro_firmware_handlers = {
	.after_create = janeiro_firmware_after_create,
	.before_destroy = janeiro_firmware_before_destroy,
	.alloc_buffer = janeiro_firmware_alloc_buffer,
	.free_buffer = janeiro_firmware_free_buffer,
	.setup_buffer = janeiro_firmware_setup_buffer,
	.teardown_buffer = janeiro_firmware_teardown_buffer,
	.prepare_run = janeiro_firmware_prepare_run,
};

int mobile_edgetpu_firmware_create(struct edgetpu_dev *etdev)
{
	return edgetpu_firmware_create(etdev, &janeiro_firmware_handlers);
}

void mobile_edgetpu_firmware_destroy(struct edgetpu_dev *etdev)
{
	edgetpu_firmware_destroy(etdev);
}

int edgetpu_chip_firmware_run(struct edgetpu_dev *etdev, const char *name,
			      enum edgetpu_firmware_flags flags)
{
	return edgetpu_firmware_run(etdev, name, flags);
}

unsigned long edgetpu_chip_firmware_iova(struct edgetpu_dev *etdev)
{
	/*
	 * There is no IOVA in Janeiro, since firmware the IOMMU is
	 * bypassed and the only translation in effect is the one
	 * done by instruction remap registers
	 */
	return EDGETPU_INSTRUCTION_REMAP_BASE;
}
