/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Defines chipset dependent configuration.
 *
 * Copyright (C) 2019 Google, Inc.
 */

#ifndef __EDGETPU_CONFIG_H__
#define __EDGETPU_CONFIG_H__

#ifdef CONFIG_HERMOSA

#include "hermosa/config.h"

#else /* !CONFIG_HERMOSA */

#ifndef CONFIG_ABROLHOS
#define CONFIG_ABROLHOS
#warning "Building default chipset abrolhos"
#endif

#include "abrolhos/config.h"

#endif /* CONFIG_HERMOSA */

#define EDGETPU_DEFAULT_FIRMWARE_NAME "google/edgetpu-" DRIVER_NAME ".fw"
#define EDGETPU_TEST_FIRMWARE_NAME "google/edgetpu-" DRIVER_NAME "-test.fw"

#endif /* __EDGETPU_CONFIG_H__ */
