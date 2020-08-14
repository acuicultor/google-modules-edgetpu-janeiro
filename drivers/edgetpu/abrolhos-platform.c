// SPDX-License-Identifier: GPL-2.0
/*
 * Platform device driver for the Google Edge TPU ML accelerator.
 *
 * Copyright (C) 2019 Google, Inc.
 */

#include <linux/dma-mapping.h>
#include <linux/init.h>
#include <linux/io.h>
#include <linux/iommu.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/types.h>

#include "abrolhos-firmware.h"
#include "abrolhos-platform.h"
#include "abrolhos-pm.h"
#include "edgetpu-config.h"
#include "edgetpu-firmware.h"
#include "edgetpu-internal.h"
#include "edgetpu-iremap-pool.h"
#include "edgetpu-mmu.h"
#include "edgetpu-telemetry.h"

static const struct of_device_id edgetpu_of_match[] = {
	{ .compatible = "google,darwinn", },
	{ /* end of list */ },
};
MODULE_DEVICE_TABLE(of, edgetpu_of_match);

/*
 * Log and trace buffers at the beginning of the remapped region,
 * pool memory afterwards.
 */

#define EDGETPU_POOL_MEM_OFFSET (EDGETPU_TELEMETRY_BUFFER_SIZE * 2)

static void abrolhos_get_telemetry_mem(struct edgetpu_platform_dev *etpdev,
				       enum edgetpu_telemetry_type type,
				       struct edgetpu_coherent_mem *mem)
{
	int offset = type == EDGETPU_TELEMETRY_TRACE ?
				   EDGETPU_TELEMETRY_BUFFER_SIZE :
				   0;
	mem->vaddr = etpdev->shared_mem_vaddr + offset;
	mem->dma_addr = EDGETPU_REMAPPED_DATA_ADDR + offset;
	mem->tpu_addr = EDGETPU_REMAPPED_DATA_ADDR + offset;
	mem->host_addr = 0;
	mem->size = EDGETPU_TELEMETRY_BUFFER_SIZE;
}

/* Setup the firmware region carveout. */
static int edgetpu_platform_setup_fw_region(struct edgetpu_platform_dev *etpdev)
{
	struct edgetpu_dev *etdev = &etpdev->edgetpu_dev;
	struct device *dev = etdev->dev;
	struct resource r;
	struct device_node *np;
	int err;
	u32 csr_phys, csr_iova, csr_size;
	size_t region_map_size =
		EDGETPU_FW_SIZE_MAX + EDGETPU_REMAPPED_DATA_SIZE;

	np = of_parse_phandle(dev->of_node, "memory-region", 0);
	if (!np) {
		dev_err(dev, "No memory region for firmware\n");
		return -ENODEV;
	}

	err = of_address_to_resource(np, 0, &r);
	of_node_put(np);
	if (err) {
		dev_err(dev,
			"No memory address assigned to firmware region\n");
		return err;
	}

	if (resource_size(&r) < region_map_size) {
		dev_err(dev,
			"Memory region for firmware too small (%zu bytes needed, got %llu)\n",
			region_map_size, resource_size(&r));
		return -ENOSPC;
	}

	etpdev->fw_region_vaddr =
		memremap(r.start, region_map_size, MEMREMAP_WC);
	if (!etpdev->fw_region_vaddr) {
		dev_err(dev, "Firmware memory remap failed\n");
		return -EINVAL;
	}

	etpdev->shared_mem_vaddr =
		etpdev->fw_region_vaddr + EDGETPU_REMAPPED_DATA_OFFSET;
	etpdev->shared_mem_paddr = r.start + EDGETPU_REMAPPED_DATA_OFFSET;

	etpdev->fw_region_size = EDGETPU_FW_SIZE_MAX;

	/* Add an IOMMU translation to the physical address of the region. */
	err = edgetpu_mmu_add_translation(etdev, FW_IOVA, r.start,
					  region_map_size,
					  IOMMU_READ | IOMMU_WRITE |
					  IOMMU_PRIV, EDGETPU_CONTEXT_KCI);
	if (err) {
		dev_err(dev, "Unable to map firmware memory into IOMMU\n");
		memunmap(etpdev->fw_region_vaddr);
		etpdev->fw_region_vaddr = NULL;
		return err;
	}

	err = of_property_read_u32(dev->of_node, "csr-iova", &csr_iova);
	/* Device did not define a CSR region */
	if (err)
		return 0;

	/* If an IOVA was found, we must also have physical address and size */
	err = of_property_read_u32(dev->of_node, "csr-phys", &csr_phys);
	if (err) {
		dev_err(dev,
			"Device tree: invalid CSR physical address\n");
		goto out_unmap;
	}

	err = of_property_read_u32(dev->of_node, "csr-size", &csr_size);
	if (err) {
		dev_err(dev, "Device tree: invalid CSR size\n");
		goto out_unmap;
	}

	dev_dbg(dev, "Mapping device CSRs: %X -> %X (%d bytes)\n", csr_iova,
		 csr_phys, csr_size);
	/* Add an IOMMU translation for the Mailbox CSRs */
	err = edgetpu_mmu_add_translation(etdev, csr_iova, csr_phys, csr_size,
					  IOMMU_READ | IOMMU_WRITE | IOMMU_PRIV,
					  EDGETPU_CONTEXT_KCI);
	if (err) {
		dev_err(dev, "Unable to map device CSRs into IOMMU\n");
		goto out_unmap;
	}
	etpdev->csr_iova = csr_iova;
	etpdev->csr_size = csr_size;
	return 0;
out_unmap:
	memunmap(etpdev->fw_region_vaddr);
	etpdev->fw_region_vaddr = NULL;
	edgetpu_mmu_remove_translation(etdev, FW_IOVA, region_map_size,
				       EDGETPU_CONTEXT_KCI);
	return err;
}

static void edgetpu_platform_cleanup_fw_region(
	struct edgetpu_platform_dev *etpdev)
{
	if (!etpdev->fw_region_vaddr)
		return;

	edgetpu_mmu_remove_translation(&etpdev->edgetpu_dev, FW_IOVA,
				       EDGETPU_FW_SIZE_MAX +
					       EDGETPU_REMAPPED_DATA_SIZE,
				       EDGETPU_CONTEXT_KCI);
	if (etpdev->csr_iova) {
		edgetpu_mmu_remove_translation(&etpdev->edgetpu_dev,
					       etpdev->csr_iova,
					       etpdev->csr_size,
					       EDGETPU_CONTEXT_KCI);
	}
	memunmap(etpdev->fw_region_vaddr);
	etpdev->fw_region_vaddr = NULL;
}

void edgetpu_setup_mmu(struct edgetpu_dev *etdev)
{
	int ret;

	/* No MMU info to pass to attach, IOMMU API will handle. */
	ret = edgetpu_mmu_attach(etdev, NULL);
	if (ret)
		dev_warn(etdev->dev, "failed to attach IOMMU: %d\n", ret);
}

static int edgetpu_platform_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct edgetpu_platform_dev *edgetpu_pdev;
	struct resource *r;
	struct edgetpu_mapped_resource regs;
	int ret;
	struct edgetpu_coherent_mem log_mem, trace_mem;

	edgetpu_pdev =
		devm_kzalloc(dev, sizeof(*edgetpu_pdev), GFP_KERNEL);
	if (!edgetpu_pdev)
		return -ENOMEM;

	platform_set_drvdata(pdev, &edgetpu_pdev->edgetpu_dev);
	edgetpu_pdev->edgetpu_dev.dev = dev;

	r = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (IS_ERR_OR_NULL(r)) {
		dev_err(dev, "failed to get memory resource\n");
		return -ENODEV;
	}
	regs.phys = r->start;
	regs.size = resource_size(r);

	regs.mem = devm_ioremap_resource(dev, r);
	if (IS_ERR_OR_NULL(regs.mem)) {
		dev_err(dev, "failed to map registers\n");
		return -ENODEV;
	}

	mutex_init(&edgetpu_pdev->platform_pwr.policy_lock);
	edgetpu_pdev->platform_pwr.curr_policy = TPU_POLICY_MAX;

	ret = abrolhos_pm_create(&edgetpu_pdev->edgetpu_dev);

	if (ret) {
		dev_err(dev, "Failed to initialize PM interface (%d)\n", ret);
		return ret;
	}

	edgetpu_pdev->edgetpu_dev.mcp_id = -1;
	edgetpu_pdev->edgetpu_dev.mcp_die_index = 0;
	edgetpu_pdev->irq = platform_get_irq(pdev, 0);
	ret = edgetpu_device_add(&edgetpu_pdev->edgetpu_dev, &regs);

	if (!ret && edgetpu_pdev->irq >= 0)
		ret = edgetpu_register_irq(&edgetpu_pdev->edgetpu_dev,
					   edgetpu_pdev->irq);

	if (ret) {
		dev_err(dev, "%s edgetpu setup failed: %d\n", DRIVER_NAME,
			ret);
		goto out;
	}

	dev_info(dev, "%s edgetpu initialized\n",
		 edgetpu_pdev->edgetpu_dev.dev_name);

	ret = edgetpu_platform_setup_fw_region(edgetpu_pdev);
	if (ret) {
		dev_err(dev, "%s setup fw regions failed: %d\n", DRIVER_NAME,
			ret);
		goto out;
	}

	abrolhos_get_telemetry_mem(edgetpu_pdev, EDGETPU_TELEMETRY_LOG,
				   &log_mem);
	abrolhos_get_telemetry_mem(edgetpu_pdev, EDGETPU_TELEMETRY_TRACE,
				   &trace_mem);

	ret = edgetpu_telemetry_init(&edgetpu_pdev->edgetpu_dev, &log_mem,
				     &trace_mem);
	if (ret)
		goto out_cleanup_fw;

	ret = abrolhos_edgetpu_firmware_create(&edgetpu_pdev->edgetpu_dev);
	if (ret) {
		dev_err(dev,
			"%s initialize firmware downloader failed: %d\n",
			DRIVER_NAME, ret);
		goto out_tel_exit;
	}

	ret = edgetpu_iremap_pool_create(
		&edgetpu_pdev->edgetpu_dev,
		/* Base virtual address (kernel address space) */
		edgetpu_pdev->shared_mem_vaddr + EDGETPU_POOL_MEM_OFFSET,
		/* Base DMA address */
		EDGETPU_REMAPPED_DATA_ADDR + EDGETPU_POOL_MEM_OFFSET,
		/* Base TPU address */
		EDGETPU_REMAPPED_DATA_ADDR + EDGETPU_POOL_MEM_OFFSET,
		/* Base physical address */
		edgetpu_pdev->shared_mem_paddr + EDGETPU_POOL_MEM_OFFSET,
		/* Size */
		EDGETPU_REMAPPED_DATA_SIZE - EDGETPU_POOL_MEM_OFFSET,
		/* Granularity */
		PAGE_SIZE);
	if (ret) {
		dev_err(dev,
			"%s failed to initialize remapped memory pool: %d\n",
			DRIVER_NAME, ret);
		goto out_fw_destroy;
	}

	dev_dbg(dev, "Creating thermal device\n");
	edgetpu_pdev->edgetpu_dev.thermal = devm_tpu_thermal_create(dev);

out:
	dev_dbg(dev, "Probe finished, powering down\n");
	/* Turn the device off until a client request is received */
	edgetpu_pm_shutdown(&edgetpu_pdev->edgetpu_dev);

	return ret;
out_fw_destroy:
	abrolhos_edgetpu_firmware_destroy(&edgetpu_pdev->edgetpu_dev);
out_tel_exit:
	edgetpu_telemetry_exit(&edgetpu_pdev->edgetpu_dev);
out_cleanup_fw:
	edgetpu_platform_cleanup_fw_region(edgetpu_pdev);
	dev_dbg(dev, "Probe finished with error %d, powering down\n", ret);
	/* Turn the device off until a client request is received */
	edgetpu_pm_shutdown(&edgetpu_pdev->edgetpu_dev);
	return ret;
}

static int edgetpu_platform_remove(struct platform_device *pdev)
{
	struct edgetpu_dev *etdev = platform_get_drvdata(pdev);
	struct edgetpu_platform_dev *edgetpu_pdev = container_of(
			etdev, struct edgetpu_platform_dev, edgetpu_dev);
	edgetpu_pm_shutdown(etdev);
	abrolhos_edgetpu_firmware_destroy(etdev);
	if (edgetpu_pdev->irq >= 0)
		edgetpu_unregister_irq(etdev, edgetpu_pdev->irq);
	edgetpu_telemetry_exit(etdev);
	edgetpu_iremap_pool_destroy(etdev);
	edgetpu_platform_cleanup_fw_region(edgetpu_pdev);
	edgetpu_device_remove(etdev);
	abrolhos_pm_destroy(etdev);
	return 0;
}

static struct platform_driver edgetpu_platform_driver = {
	.probe = edgetpu_platform_probe,
	.remove = edgetpu_platform_remove,
	.driver = {
			.name = "edgetpu_platform",
			.of_match_table = edgetpu_of_match,
		},
};

static int __init edgetpu_platform_init(void)
{
	int ret;

	ret = edgetpu_init();
	if (ret)
		return ret;
	return platform_driver_register(&edgetpu_platform_driver);
}

static void __exit edgetpu_platform_exit(void)
{
	platform_driver_unregister(&edgetpu_platform_driver);
	edgetpu_exit();
}

MODULE_DESCRIPTION("Google Edge TPU platform driver");
MODULE_LICENSE("GPL v2");
module_init(edgetpu_platform_init);
module_exit(edgetpu_platform_exit);
MODULE_FIRMWARE(EDGETPU_DEFAULT_FIRMWARE_NAME);
