/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Defines chipset dependent configuration.
 *
 * Copyright (C) 2019 Google, Inc.
 */

#ifndef __EDGETPU_CONFIG_H__
#define __EDGETPU_CONFIG_H__

#ifdef CONFIG_JANEIRO

#include "janeiro/config.h"
#endif /* CONFIG_JANEIRO */
#define EDGETPU_DEFAULT_FIRMWARE_NAME "google/edgetpu-" DRIVER_NAME ".fw"
#define EDGETPU_TEST_FIRMWARE_NAME "google/edgetpu-" DRIVER_NAME "-test.fw"

#endif /* __EDGETPU_CONFIG_H__ */
