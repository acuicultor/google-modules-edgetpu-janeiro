// SPDX-License-Identifier: GPL-2.0
/*
 * Janeiro platform device driver for the Google Edge TPU ML accelerator.
 *
 * Copyright (C) 2020 Google, Inc.
 */

#include <linux/device.h>
#include <linux/module.h>
#include <linux/iopoll.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/platform_device.h>

#include "edgetpu-config.h"
#include "edgetpu-debug-dump.h"
#include "edgetpu-firmware.h"
#include "edgetpu-internal.h"
#include "edgetpu-iremap-pool.h"
#include "edgetpu-mmu.h"
#include "edgetpu-telemetry.h"
#include "janeiro-platform.h"
#include "janeiro-pm.h"
#include "mobile-firmware.h"

static const struct of_device_id edgetpu_of_match[] = {
	{
		.compatible = "google,darwinn",
	},
	{ /* end of list */ },
};
MODULE_DEVICE_TABLE(of, edgetpu_of_match);

#define EDGETPU_POOL_MEM_OFFSET (EDGETPU_TELEMETRY_BUFFER_SIZE * 2)

static void janeiro_get_telemetry_mem(struct janeiro_platform_dev *etpdev,
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

static int janeiro_platform_setup_fw_region(struct janeiro_platform_dev *etpdev)
{
	struct edgetpu_dev *etdev = &etpdev->edgetpu_dev;
	struct device *dev = etdev->dev;
	struct resource r;
	struct device_node *np;
	int err;
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
		dev_err(dev, "No memory address assigned to firmware region\n");
		return err;
	}

	if (resource_size(&r) < region_map_size) {
		dev_err(dev,
			"Memory region for firmware too small (%zu bytes needed, got %llu)\n",
			region_map_size, resource_size(&r));
		return -ENOSPC;
	}

	etpdev->fw_region_paddr = r.start;
	etpdev->fw_region_size = EDGETPU_FW_SIZE_MAX;

	etpdev->shared_mem_vaddr =
		memremap(r.start + EDGETPU_REMAPPED_DATA_OFFSET,
			 EDGETPU_REMAPPED_DATA_SIZE, MEMREMAP_WC);
	if (!etpdev->shared_mem_vaddr) {
		dev_err(dev, "Shared memory remap failed\n");
		return -EINVAL;
	}
	etpdev->shared_mem_paddr = r.start + EDGETPU_REMAPPED_DATA_OFFSET;

	return 0;
}

static void
janeiro_platform_cleanup_fw_region(struct janeiro_platform_dev *etpdev)
{
	if (!etpdev->shared_mem_vaddr)
		return;
	memunmap(etpdev->shared_mem_vaddr);
	etpdev->shared_mem_vaddr = NULL;
}

int edgetpu_chip_setup_mmu(struct edgetpu_dev *etdev)
{
	int ret;

	ret = edgetpu_mmu_attach(etdev, NULL);
	if (ret)
		dev_err(etdev->dev, "failed to attach IOMMU: %d\n", ret);
	return ret;
}

void edgetpu_chip_remove_mmu(struct edgetpu_dev *etdev)
{
	edgetpu_mmu_detach(etdev);
}

/*
 * Set shareability for enabling IO coherency in Janeiro
 */
//TODO(b/185301967): check if sysreg is to be moved under TrustZone
static int janeiro_mmu_set_shareability(struct device *dev)
{
	void __iomem *addr = ioremap(EDGETPU_SYSREG_TPU_BASE, PAGE_SIZE);

	if (!addr) {
		dev_err(dev, "sysreg ioremap failed\n");
		return -ENOMEM;
	}

	writel_relaxed(SHAREABLE_WRITE | SHAREABLE_READ | INNER_SHAREABLE,
		       addr + EDGETPU_SYSREG_TPU_SHAREABILITY);
	iounmap(addr);

	return 0;
}

static int edgetpu_platform_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct janeiro_platform_dev *edgetpu_pdev;
	struct resource *r;
	struct edgetpu_mapped_resource regs;
	int ret, i;

	edgetpu_pdev = devm_kzalloc(dev, sizeof(*edgetpu_pdev), GFP_KERNEL);
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

	ret = janeiro_pm_create(&edgetpu_pdev->edgetpu_dev);
	if (ret) {
		dev_err(dev, "Failed to initialize PM interface (%d)\n", ret);
		return ret;
	}

	ret = janeiro_platform_setup_fw_region(edgetpu_pdev);
	if (ret) {
		dev_err(dev, "%s setup fw regions failed: %d\n", DRIVER_NAME,
			ret);
		goto out;
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
		goto out_cleanup_fw;
	}

	edgetpu_pdev->edgetpu_dev.mcp_id = -1;
	edgetpu_pdev->edgetpu_dev.mcp_die_index = 0;

	for (i = 0; i < EDGETPU_NCONTEXTS; i++)
		edgetpu_pdev->irq[i] = platform_get_irq(pdev, i);

	janeiro_mmu_set_shareability(dev);

	ret = edgetpu_device_add(&edgetpu_pdev->edgetpu_dev, &regs);

	if (ret) {
		dev_err(dev, "%s edgetpu setup failed: %d\n", DRIVER_NAME, ret);
		goto out_destroy_iremap;
	}
	for (i = 0; i < EDGETPU_NCONTEXTS; i++) {
		if (edgetpu_pdev->irq[i] >= 0)
			ret = edgetpu_register_irq(&edgetpu_pdev->edgetpu_dev,
						   edgetpu_pdev->irq[i]);
		if (ret)
			break;
	}
	if (ret) {
		while (i-- > 0) {
			edgetpu_unregister_irq(&edgetpu_pdev->edgetpu_dev,
					       edgetpu_pdev->irq[i]);
		}
		dev_err(dev, "%s edgetpu irq registration failed: %d\n",
			DRIVER_NAME, ret);
		goto out_remove_device;
	}

	janeiro_get_telemetry_mem(edgetpu_pdev, EDGETPU_TELEMETRY_LOG,
				  &edgetpu_pdev->log_mem);
	janeiro_get_telemetry_mem(edgetpu_pdev, EDGETPU_TELEMETRY_TRACE,
				  &edgetpu_pdev->trace_mem);

	ret = edgetpu_telemetry_init(&edgetpu_pdev->edgetpu_dev,
				     &edgetpu_pdev->log_mem,
				     &edgetpu_pdev->trace_mem);
	if (ret)
		goto out_remove_device;

	ret = mobile_edgetpu_firmware_create(&edgetpu_pdev->edgetpu_dev);
	if (ret) {
		dev_err(dev, "%s initialize firmware downloader failed: %d\n",
			DRIVER_NAME, ret);
		goto out_tel_exit;
	}

	dev_info(dev, "%s edgetpu initialized. Build: %s\n",
		 edgetpu_pdev->edgetpu_dev.dev_name, GIT_REPO_TAG);

	edgetpu_pm_shutdown(&edgetpu_pdev->edgetpu_dev, false);
out:
	dev_dbg(dev, "Probe finished\n");

	return 0;
out_tel_exit:
	edgetpu_telemetry_exit(&edgetpu_pdev->edgetpu_dev);
out_remove_device:
	edgetpu_device_remove(&edgetpu_pdev->edgetpu_dev);
out_destroy_iremap:
	edgetpu_iremap_pool_destroy(&edgetpu_pdev->edgetpu_dev);
out_cleanup_fw:
	janeiro_platform_cleanup_fw_region(edgetpu_pdev);
	dev_dbg(dev, "Probe finished with error %d, powering down\n", ret);
	return ret;
}

static int edgetpu_platform_remove(struct platform_device *pdev)
{
	int i;
	struct edgetpu_dev *etdev = platform_get_drvdata(pdev);
	struct janeiro_platform_dev *janeiro_pdev = to_janeiro_dev(etdev);

	/* TODO(b/189906347): Use edgetpu_device_remove() for cleanup after
	 * having GSA/TZ support.
	 */
	edgetpu_pm_get(etdev->pm);

	for (i = 0; i < EDGETPU_NCONTEXTS; i++) {
		if (janeiro_pdev->irq[i] >= 0)
			edgetpu_unregister_irq(etdev, janeiro_pdev->irq[i]);
	}

	edgetpu_telemetry_exit(etdev);
	edgetpu_chip_exit(etdev);
	edgetpu_debug_dump_exit(etdev);
	edgetpu_mailbox_remove_all(etdev->mailbox_manager);
	mobile_edgetpu_firmware_destroy(etdev);
	edgetpu_pm_put(etdev->pm);
	edgetpu_pm_shutdown(etdev, true);
	edgetpu_usage_stats_exit(etdev);
	edgetpu_chip_remove_mmu(etdev);
	edgetpu_fs_remove(etdev);
	edgetpu_iremap_pool_destroy(etdev);
	janeiro_platform_cleanup_fw_region(janeiro_pdev);
	janeiro_pm_destroy(etdev);
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

MODULE_DESCRIPTION("Janeiro Edge TPU platform driver");
MODULE_LICENSE("GPL v2");
module_init(edgetpu_platform_init);
module_exit(edgetpu_platform_exit);
MODULE_FIRMWARE(EDGETPU_DEFAULT_FIRMWARE_NAME);
