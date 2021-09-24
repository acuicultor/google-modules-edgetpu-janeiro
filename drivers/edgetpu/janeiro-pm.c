// SPDX-License-Identifier: GPL-2.0
/*
 * Janeiro EdgeTPU power management support
 *
 * Copyright (C) 2020 Google, Inc.
 */

#include <linux/delay.h>
#include <linux/iopoll.h>

#include "edgetpu-config.h"
#include "edgetpu-internal.h"
#include "edgetpu-mobile-platform.h"
#include "mobile-pm.h"

#define TPU_DEFAULT_POWER_STATE		TPU_ACTIVE_NOM

#include "mobile-pm.c"

#define SHUTDOWN_DELAY_US_MIN		20
#define SHUTDOWN_DELAY_US_MAX		20
#define BOOTUP_DELAY_US_MIN		200
#define BOOTUP_DELAY_US_MAX		250
#define SHUTDOWN_MAX_DELAY_COUNT	50

#define EDGETPU_PSM0_CFG 0x1c1880
#define EDGETPU_PSM0_START 0x1c1884
#define EDGETPU_PSM0_STATUS 0x1c1888
#define EDGETPU_PSM1_CFG 0x1c2880
#define EDGETPU_PSM1_START 0x1c2884
#define EDGETPU_PSM1_STATUS 0x1c2888
#define EDGETPU_LPM_CHANGE_TIMEOUT 30000

static void janeiro_lpm_down(struct edgetpu_dev *etdev)
{
	int timeout_cnt = 0;
	u32 val;

	do {
		/* Manually delay 20us per retry till LPM shutdown finished */
		usleep_range(SHUTDOWN_DELAY_US_MIN, SHUTDOWN_DELAY_US_MAX);
		val = edgetpu_dev_read_32_sync(etdev, EDGETPU_REG_LPM_CONTROL);
		if ((val & 0x1000) || (val == 0))
			break;
		timeout_cnt++;
	} while (timeout_cnt < SHUTDOWN_MAX_DELAY_COUNT);
	if (timeout_cnt == SHUTDOWN_MAX_DELAY_COUNT)
		// Log the issue then continue to perform the shutdown forcefully.
		etdev_warn(etdev, "LPM shutdown failure, continuing BLK shutdown\n");
}

static int janeiro_lpm_up(struct edgetpu_dev *etdev)
{
	int ret;
	u32 val;

	edgetpu_dev_write_32_sync(etdev, EDGETPU_PSM0_START, 1);
	ret = readl_poll_timeout(etdev->regs.mem + EDGETPU_PSM0_STATUS, val, val & 0x80, 20,
				 EDGETPU_LPM_CHANGE_TIMEOUT);
	if (ret) {
		etdev_err(etdev, "Set LPM0 failed: %d\n", ret);
		return ret;
	}
	edgetpu_dev_write_32_sync(etdev, EDGETPU_PSM1_START, 1);
	ret = readl_poll_timeout(etdev->regs.mem + EDGETPU_PSM1_STATUS, val, val & 0x80, 20,
				 EDGETPU_LPM_CHANGE_TIMEOUT);
	if (ret) {
		etdev_err(etdev, "Set LPM1 failed: %d\n", ret);
		return ret;
	}

	edgetpu_dev_write_32_sync(etdev, EDGETPU_PSM0_CFG, 0);
	edgetpu_dev_write_32_sync(etdev, EDGETPU_PSM1_CFG, 0);

	return 0;
}

static void janeiro_block_down(struct edgetpu_dev *etdev)
{
	int timeout_cnt = 0;
	int curr_state;

	do {
		/* Delay 20us per retry till blk shutdown finished */
		usleep_range(SHUTDOWN_DELAY_US_MIN, SHUTDOWN_DELAY_US_MAX);
		/* Only poll for BLK status instead of CLK rate */
		curr_state = exynos_acpm_get_rate(TPU_ACPM_DOMAIN, 1);
		if (!curr_state)
			break;
		timeout_cnt++;
	} while (timeout_cnt < SHUTDOWN_MAX_DELAY_COUNT);
	if (timeout_cnt == SHUTDOWN_MAX_DELAY_COUNT)
		etdev_warn(etdev, "%s: blk_shutdown timeout\n", __func__);
}

static void janeiro_firmware_down(struct edgetpu_dev *etdev)
{
	int ret;

	ret = edgetpu_kci_shutdown(etdev->kci);
	if (ret) {
		etdev_err(etdev, "firmware shutdown failed: %d", ret);
		return;
	}
}

static int janeiro_acpm_set_rate(unsigned int id, unsigned long rate)
{
	return exynos_acpm_set_rate(id, rate);
}

int edgetpu_chip_pm_create(struct edgetpu_dev *etdev)
{
	struct edgetpu_mobile_platform_dev *etmdev = to_mobile_dev(etdev);
	struct edgetpu_mobile_platform_pwr *platform_pwr = &etmdev->platform_pwr;

	platform_pwr->lpm_up = janeiro_lpm_up;
	platform_pwr->lpm_down = janeiro_lpm_down;
	platform_pwr->block_down = janeiro_block_down;
	platform_pwr->firmware_down = janeiro_firmware_down;
	platform_pwr->acpm_set_rate = janeiro_acpm_set_rate;

	return mobile_pm_create(etdev);
}
