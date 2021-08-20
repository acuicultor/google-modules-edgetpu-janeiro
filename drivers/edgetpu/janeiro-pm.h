/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Power management header for Janeiro.
 *
 * Copyright (C) 2020 Google, Inc.
 */
#ifndef __JANEIRO_PM_H__
#define __JANEIRO_PM_H__

#include "edgetpu-internal.h"
#include "edgetpu-kci.h"

/* Can't build out of tree with acpm_dvfs unless kernel supports ACPM */
#if IS_ENABLED(CONFIG_ACPM_DVFS)

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
#endif /* IS_ENABLED(CONFIG_ACPM_DVFS) */
//TODO(b/185797093): check abrolhos ported values for janeiro
/*
 * TPU Power States:
 * 0:		Off
 * 227000	Ultra Underdrive @227MHz
 * 625000:	Super Underdrive @625MHz
 * 845000:	Underdrive @845MHz
 * 1066000:	Nominal @1066MHz
 */
enum tpu_pwr_state {
	TPU_OFF = 0,
	TPU_ACTIVE_UUD = 227000,
	TPU_ACTIVE_SUD = 625000,
	TPU_ACTIVE_UD  = 845000,
	TPU_ACTIVE_NOM = 1066000,
};

/*
 * Request codes from firmware
 * Values must match with firmware code base
 */
enum janeiro_reverse_kci_code {
	RKCI_CODE_PM_QOS = RKCI_CHIP_CODE_FIRST + 1,
	RKCI_CODE_BTS = RKCI_CHIP_CODE_FIRST + 2,
};

#define TPU_POLICY_MAX	TPU_ACTIVE_NOM

#define TPU_ACPM_DOMAIN			7

int janeiro_pm_create(struct edgetpu_dev *etdev);

void janeiro_pm_destroy(struct edgetpu_dev *etdev);

void janeiro_pm_set_pm_qos(struct edgetpu_dev *etdev, u32 pm_qos_val);

void janeiro_pm_set_bts(struct edgetpu_dev *etdev, u32 bts_val);

#endif /* __JANEIRO_PM_H__ */
