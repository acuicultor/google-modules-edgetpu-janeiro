/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Power management header for mobile chipsets.
 *
 * Copyright (C) 2021 Google, Inc.
 */

#ifndef __MOBILE_PM_H__
#define __MOBILE_PM_H__

#include "edgetpu-internal.h"
#include "edgetpu-kci.h"

/* Can't build out of tree with acpm_dvfs unless kernel supports ACPM */
#if IS_ENABLED(CONFIG_ACPM_DVFS) || IS_ENABLED(CONFIG_EDGETPU_TEST)

#include <linux/acpm_dvfs.h>

#else

static unsigned long exynos_acpm_rate;
static inline int exynos_acpm_set_rate(unsigned int id, unsigned long rate)
{
	exynos_acpm_rate = rate;
	return 0;
}
static inline int exynos_acpm_set_init_freq(unsigned int dfs_id,
					    unsigned long freq)
{
	return 0;
}
static inline unsigned long exynos_acpm_get_rate(unsigned int id,
						 unsigned long dbg_val)
{
	return exynos_acpm_rate;
}
static inline int exynos_acpm_set_policy(unsigned int id, unsigned long policy)
{
	return 0;
}
#endif /* IS_ENABLED(CONFIG_ACPM_DVFS) || IS_ENABLED(CONFIG_EDGETPU_TEST) */

/*
 * Request codes from firmware
 * Values must match with firmware code base
 */
enum mobile_reverse_kci_code {
	RKCI_CODE_PM_QOS = RKCI_CHIP_CODE_FIRST + 1,
	RKCI_CODE_BTS = RKCI_CHIP_CODE_FIRST + 2,
};

/*
 * Initialize a power management interface for an edgetpu device on mobile
 * chipsets.
 */
int mobile_pm_create(struct edgetpu_dev *etdev);

/*
 * Destroy power management interface for an edgetpu device on mobile chipsets.
 */
void mobile_pm_destroy(struct edgetpu_dev *etdev);

/* Set required QoS value for the edgetpu device. */
void mobile_pm_set_pm_qos(struct edgetpu_dev *etdev, u32 pm_qos_val);

/* Set BTS value for the edgetpu device. */
void mobile_pm_set_bts(struct edgetpu_dev *etdev, u32 bts_val);

#endif /* __MOBILE_PM_H__ */
