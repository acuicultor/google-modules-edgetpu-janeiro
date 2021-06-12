/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Platform device driver for the Google Edge TPU ML accelerator.
 *
 * Copyright (C) 2020 Google, Inc.
 */
#ifndef __JANEIRO_PLATFORM_H__
#define __JANEIRO_PLATFORM_H__

#include <linux/device.h>
#include <linux/io.h>
#include <linux/mutex.h>
#include <linux/types.h>

#include "edgetpu-internal.h"
#include "janeiro-pm.h"

#define to_janeiro_dev(etdev)                                                  \
	container_of(etdev, struct janeiro_platform_dev, edgetpu_dev)

// TODO(b/176881607): merge with abrolhos
struct janeiro_platform_pwr {
	struct mutex state_lock;
	u64 min_state;
	u64 requested_state;
};

struct janeiro_platform_dev {
	struct edgetpu_dev edgetpu_dev;
	struct janeiro_platform_pwr platform_pwr;
	int irq[EDGETPU_NCONTEXTS];
	phys_addr_t fw_region_paddr;
	void *fw_region_vaddr;
	size_t fw_region_size;
	void *shared_mem_vaddr;
	phys_addr_t shared_mem_paddr;
	size_t shared_mem_size;
	phys_addr_t csr_paddr;
	dma_addr_t csr_iova;
	size_t csr_size;
	struct device *gsa_dev;
	void __iomem *ssmt_base;
	struct edgetpu_coherent_mem log_mem;
	struct edgetpu_coherent_mem trace_mem;
};

#endif /* __JANEIRO_PLATFORM_H__ */
