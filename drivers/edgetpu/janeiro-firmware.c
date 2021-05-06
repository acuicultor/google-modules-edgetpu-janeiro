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

#include "edgetpu-config.h"
#include "edgetpu-firmware.h"
#include "edgetpu-internal.h"
#include "edgetpu-mmu.h"
#include "edgetpu-kci.h"
#include "edgetpu-mailbox.h"
#include "janeiro-platform.h"
#include "mobile-firmware.h"

/*
 * Sets the reset state of the R52 core.
 * @val: 1 to put the core in reset state, 0 to release core from reset state.
 */
static void r52_reset(struct edgetpu_dev *etdev, u64 val)
{
	edgetpu_dev_write_32_sync(etdev, EDGETPU_REG_RESET_CONTROL, val);
}

static void janeiro_firmware_before_destroy(struct edgetpu_firmware *et_fw)
{
	r52_reset(et_fw->etdev, 1);
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
	struct mobile_image_config *image_config;
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

	/* Skip the header */
	memcpy(image_vaddr, fw_buf->vaddr + MOBILE_FW_HEADER_SIZE,
	       fw_buf->used_size - MOBILE_FW_HEADER_SIZE);

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
	r52_reset(etdev, 1);

	/* Reset KCI mailbox before starting f/w, don't process anything old.*/
	edgetpu_mailbox_reset(etdev->kci->mailbox);

	/* Remap TPU CPU instructions to the carveout IOVA. */
	edgetpu_dev_write_32(etdev, EDGETPU_REG_INSTRUCTION_REMAP_NEW_BASE,
			     /*FW_IOVA*/ fw_dma_addr);
	edgetpu_dev_write_32(etdev, EDGETPU_REG_INSTRUCTION_REMAP_CONTROL, 1);
	r52_reset(etdev, 0);
	//TODO: cleanup
	return ret;
}

static const struct edgetpu_firmware_handlers janeiro_firmware_handlers = {
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
