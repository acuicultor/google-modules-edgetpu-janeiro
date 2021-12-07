/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Module that defines structure to retrieve debug dump segments
 * specific to the family of EdgeTPUs for mobile devices.
 *
 * Copyright (C) 2021 Google, Inc.
 */

#ifndef __MOBILE_DEBUG_DUMP_H__
#define __MOBILE_DEBUG_DUMP_H__

#include "edgetpu-debug-dump.h"

struct mobile_sscd_info {
	void *pdata; /* SSCD platform data */
	void *dev; /* SSCD platform device */
};

struct mobile_sscd_mappings_dump {
	u64 host_address;
	u64 device_address;
	u64 alloc_iova;
	u64 size;
};

#endif /* MOBILE_DEBUG_DUMP_H_ */
