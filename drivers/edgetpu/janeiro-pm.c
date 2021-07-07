// SPDX-License-Identifier: GPL-2.0
/*
 * Janeiro EdgeTPU power management support
 *
 * Copyright (C) 2020 Google, Inc.
 */

#include <linux/atomic.h>
#include <linux/delay.h>
#include <linux/iopoll.h>
#include <linux/module.h>
#include <linux/pm_runtime.h>
#include <soc/google/bcl.h>
#include <linux/version.h>

#include "edgetpu-config.h"
#include "edgetpu-firmware.h"
#include "edgetpu-internal.h"
#include "edgetpu-kci.h"
#include "edgetpu-mailbox.h"
#include "edgetpu-pm.h"
#include "janeiro-platform.h"
#include "janeiro-pm.h"

#include "edgetpu-pm.c"

/* Default power state */
static int power_state = TPU_ACTIVE_NOM;

module_param(power_state, int, 0660);

static struct dentry *janeiro_pwr_debugfs_dir;

static int janeiro_pwr_state_init(struct device *dev)
{
	int ret;
	int curr_state;

	pm_runtime_enable(dev);
	curr_state = exynos_acpm_get_rate(TPU_ACPM_DOMAIN, 0);

	if (curr_state > TPU_OFF) {
		ret = pm_runtime_get_sync(dev);
		if (ret) {
			dev_err(dev, "%s: pm_runtime_get_sync err: %d\n",
				__func__, ret);
			return ret;
		}
	}
	ret = exynos_acpm_set_init_freq(TPU_ACPM_DOMAIN, curr_state);
	if (ret) {
		dev_err(dev, "error initializing tpu ACPM freq: %d\n", ret);
		if (curr_state > TPU_OFF)
			pm_runtime_put_sync(dev);
		return ret;
	}
	return ret;
}

static int janeiro_pwr_state_set_locked(void *data, u64 val)
{
	int ret;
	int curr_state;
	struct edgetpu_dev *etdev = (typeof(etdev))data;
	struct device *dev = etdev->dev;

	curr_state = exynos_acpm_get_rate(TPU_ACPM_DOMAIN, 0);

	dev_dbg(dev, "Power state %d -> %llu\n", curr_state, val);

	if (curr_state == TPU_OFF && val > TPU_OFF) {
		ret = pm_runtime_get_sync(dev);
		if (ret) {
			dev_err(dev, "%s: pm_runtime_get_sync err: %d\n",
				__func__, ret);
			return ret;
		}
	}

	/* TPU_OFF is invalid state */
	if (val != TPU_OFF) {
		ret = exynos_acpm_set_rate(TPU_ACPM_DOMAIN, (unsigned long)val);
		if (ret) {
			dev_err(dev, "error setting tpu power state: %d\n", ret);
			pm_runtime_put_sync(dev);
			return ret;
		}
	}

	if (curr_state != TPU_OFF && val == TPU_OFF) {
		ret = pm_runtime_put_sync(dev);
		if (ret) {
			dev_err(dev, "%s: pm_runtime_put_sync returned %d\n",
				__func__, ret);
			return ret;
		}
	}

	return ret;
}

static int janeiro_pwr_state_get_locked(void *data, u64 *val)
{
	struct edgetpu_dev *etdev = (typeof(etdev))data;
	struct device *dev = etdev->dev;

	*val = exynos_acpm_get_rate(TPU_ACPM_DOMAIN, 0);
	dev_dbg(dev, "current tpu power state: %llu\n", *val);

	return 0;
}

static int janeiro_pwr_state_set(void *data, u64 val)
{
	struct edgetpu_dev *etdev = (typeof(etdev))data;
	struct janeiro_platform_dev *edgetpu_pdev = to_janeiro_dev(etdev);
	struct janeiro_platform_pwr *platform_pwr = &edgetpu_pdev->platform_pwr;
	int ret = 0;

	mutex_lock(&platform_pwr->state_lock);
	platform_pwr->requested_state = val;
	if (val >= platform_pwr->min_state)
		ret = janeiro_pwr_state_set_locked(etdev, val);
	mutex_unlock(&platform_pwr->state_lock);
	return ret;
}

static int janeiro_pwr_state_get(void *data, u64 *val)
{
	struct edgetpu_dev *etdev = (typeof(etdev))data;
	struct janeiro_platform_dev *edgetpu_pdev = to_janeiro_dev(etdev);
	struct janeiro_platform_pwr *platform_pwr = &edgetpu_pdev->platform_pwr;
	int ret;

	mutex_lock(&platform_pwr->state_lock);
	ret = janeiro_pwr_state_get_locked(etdev, val);
	mutex_unlock(&platform_pwr->state_lock);
	return ret;
}

static int janeiro_min_pwr_state_set(void *data, u64 val)
{
	struct edgetpu_dev *etdev = (typeof(etdev))data;
	struct janeiro_platform_dev *edgetpu_pdev = to_janeiro_dev(etdev);
	struct janeiro_platform_pwr *platform_pwr = &edgetpu_pdev->platform_pwr;
	int ret = 0;

	mutex_lock(&platform_pwr->state_lock);
	platform_pwr->min_state = val;
	if (val >= platform_pwr->requested_state)
		ret = janeiro_pwr_state_set_locked(etdev, val);
	mutex_unlock(&platform_pwr->state_lock);
	return ret;
}

static int janeiro_min_pwr_state_get(void *data, u64 *val)
{
	struct edgetpu_dev *etdev = (typeof(etdev))data;
	struct janeiro_platform_dev *edgetpu_pdev = to_janeiro_dev(etdev);
	struct janeiro_platform_pwr *platform_pwr = &edgetpu_pdev->platform_pwr;

	mutex_lock(&platform_pwr->state_lock);
	*val = platform_pwr->min_state;
	mutex_unlock(&platform_pwr->state_lock);
	return 0;
}

DEFINE_DEBUGFS_ATTRIBUTE(fops_tpu_pwr_state, janeiro_pwr_state_get,
			 janeiro_pwr_state_set, "%llu\n");

DEFINE_DEBUGFS_ATTRIBUTE(fops_tpu_min_pwr_state, janeiro_min_pwr_state_get,
			 janeiro_min_pwr_state_set, "%llu\n");

static int janeiro_get_initial_pwr_state(struct device *dev)
{
	switch (power_state) {
	case TPU_ACTIVE_UUD:
	case TPU_ACTIVE_SUD:
	case TPU_ACTIVE_UD:
	case TPU_ACTIVE_NOM:
		dev_info(dev, "Initial power state: %d\n", power_state);
		break;
	case TPU_OFF:
		dev_warn(dev, "Power state %d prevents control core booting",
			 power_state);
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 4, 0)
		fallthrough;
#endif
	default:
		dev_warn(dev, "Power state %d is invalid\n", power_state);
		dev_warn(dev, "defaulting to active nominal\n");
		power_state = TPU_ACTIVE_NOM;
		break;
	}
	return power_state;
}

static void janeiro_power_down(struct edgetpu_pm *etpm);

#define EDGETPU_PSM0_CFG 0x1c1880
#define EDGETPU_PSM0_START 0x1c1884
#define EDGETPU_PSM0_STATUS 0x1c1888
#define EDGETPU_PSM1_CFG 0x1c2880
#define EDGETPU_PSM1_START 0x1c2884
#define EDGETPU_PSM1_STATUS 0x1c2888
#define EDGETPU_LPM_CHANGE_TIMEOUT 30000

static int janeiro_set_lpm(struct edgetpu_dev *etdev)
{
	int ret;
	u32 val;

	edgetpu_dev_write_32_sync(etdev, EDGETPU_PSM0_START, 1);
	ret = readl_poll_timeout(etdev->regs.mem + EDGETPU_PSM0_STATUS, val,
				 val & 0x80, 5, EDGETPU_LPM_CHANGE_TIMEOUT);
	if (ret) {
		etdev_err(etdev, "Set LPM0 failed: %d\n", ret);
		return ret;
	}
	edgetpu_dev_write_32_sync(etdev, EDGETPU_PSM1_START, 1);
	ret = readl_poll_timeout(etdev->regs.mem + EDGETPU_PSM1_STATUS, val,
				 val & 0x80, 5, EDGETPU_LPM_CHANGE_TIMEOUT);
	if (ret) {
		etdev_err(etdev, "Set LPM1 failed: %d\n", ret);
		return ret;
	}

	edgetpu_dev_write_32_sync(etdev, EDGETPU_PSM0_CFG, 0);
	edgetpu_dev_write_32_sync(etdev, EDGETPU_PSM1_CFG, 0);

	return 0;
}

static int janeiro_power_up(struct edgetpu_pm *etpm)
{
	struct edgetpu_dev *etdev = etpm->etdev;
	struct janeiro_platform_dev *edgetpu_pdev = to_janeiro_dev(etdev);
	int ret = 0;

	ret = janeiro_pwr_state_set(
		etpm->etdev, janeiro_get_initial_pwr_state(etdev->dev));

	etdev_info(etpm->etdev, "Powering up\n");

	if (ret)
		return ret;

	janeiro_set_lpm(etdev);

	edgetpu_chip_init(etdev);

	if (etdev->kci) {
		etdev_dbg(etdev, "Resetting KCI\n");
		edgetpu_kci_reinit(etdev->kci);
	}
	if (etdev->mailbox_manager) {
		etdev_dbg(etdev, "Resetting VII mailboxes\n");
		edgetpu_mailbox_reset_vii(etdev->mailbox_manager);
	}

	if (!etdev->firmware)
		return 0;

	/*
	 * Why this function uses edgetpu_firmware_*_locked functions without explicitly holding
	 * edgetpu_firmware_lock:
	 *
	 * edgetpu_pm_get() is called in two scenarios - one is when the firmware loading is
	 * attempting, another one is when the user-space clients need the device be powered
	 * (usually through acquiring the wakelock).
	 *
	 * For the first scenario edgetpu_firmware_is_loading() below shall return true.
	 * For the second scenario we are indeed called without holding the firmware lock, but the
	 * firmware loading procedures (i.e. the first scenario) always call edgetpu_pm_get() before
	 * changing the firmware state, and edgetpu_pm_get() is blocked until this function
	 * finishes. In short, we are protected by the PM lock.
	 */

	if (edgetpu_firmware_is_loading(etdev))
		return 0;

	/* attempt firmware run */
	switch (edgetpu_firmware_status_locked(etdev)) {
	case FW_VALID:
		ret = edgetpu_firmware_restart_locked(etdev);
		break;
	case FW_INVALID:
		ret = edgetpu_firmware_run_locked(etdev->firmware,
						  EDGETPU_DEFAULT_FIRMWARE_NAME,
						  FW_DEFAULT);
		break;
	default:
		break;
	}

	if (ret) {
		janeiro_power_down(etpm);
	} else {
#if IS_ENABLED(CONFIG_GOOGLE_BCL)
		if (!edgetpu_pdev->bcl_dev)
			edgetpu_pdev->bcl_dev = google_retrieve_bcl_handle();
		if (edgetpu_pdev->bcl_dev)
			google_init_tpu_ratio(edgetpu_pdev->bcl_dev);
#endif
	}

	return ret;
}

static void
janeiro_pm_shutdown_firmware(struct janeiro_platform_dev *etpdev,
			     struct edgetpu_dev *etdev)
{
	int ret;

	ret = edgetpu_kci_shutdown(etdev->kci);
	if (ret) {
		etdev_err(etdev, "firmware shutdown failed: %d",
			  ret);
		return;
	}
}

static void janeiro_power_down(struct edgetpu_pm *etpm)
{
	struct edgetpu_dev *etdev = etpm->etdev;
	struct janeiro_platform_dev *edgetpu_pdev = to_janeiro_dev(etdev);
	u64 val;

	etdev_info(etdev, "Powering down\n");

	if (janeiro_pwr_state_get(etdev, &val)) {
		etdev_warn(etdev, "Failed to read current power state\n");
		val = TPU_ACTIVE_NOM;
	}
	if (val == TPU_OFF) {
		etdev_dbg(etdev, "Device already off, skipping shutdown\n");
		return;
	}

	if (etdev->kci && edgetpu_firmware_status_locked(etdev) == FW_VALID) {
		/* Update usage stats before we power off fw. */
		edgetpu_kci_update_usage_locked(etdev);
		janeiro_pm_shutdown_firmware(edgetpu_pdev, etdev);
		edgetpu_kci_cancel_work_queues(etdev->kci);
	}

	janeiro_pwr_state_set(etdev, TPU_OFF);
}

static int janeiro_pm_after_create(struct edgetpu_pm *etpm)
{
	int ret;
	struct edgetpu_dev *etdev = etpm->etdev;
	struct janeiro_platform_dev *edgetpu_pdev = to_janeiro_dev(etdev);
	struct device *dev = etdev->dev;

	ret = janeiro_pwr_state_init(dev);
	if (ret)
		return ret;

	mutex_init(&edgetpu_pdev->platform_pwr.state_lock);

	ret = janeiro_pwr_state_set(etdev,
				     janeiro_get_initial_pwr_state(dev));
	if (ret)
		return ret;
	janeiro_pwr_debugfs_dir =
		debugfs_create_dir("power", edgetpu_fs_debugfs_dir());
	if (IS_ERR_OR_NULL(janeiro_pwr_debugfs_dir)) {
		etdev_warn(etdev, "Failed to create debug FS power");
		/* don't fail the procedure on debug FS creation fails */
		return 0;
	}
	debugfs_create_file("state", 0660, janeiro_pwr_debugfs_dir, etdev,
			    &fops_tpu_pwr_state);
	debugfs_create_file("min_state", 0660, janeiro_pwr_debugfs_dir, etdev,
			    &fops_tpu_min_pwr_state);
	return 0;
}

static void janeiro_pm_before_destroy(struct edgetpu_pm *etpm)
{
	debugfs_remove_recursive(janeiro_pwr_debugfs_dir);
	pm_runtime_disable(etpm->etdev->dev);
}

static struct edgetpu_pm_handlers janeiro_pm_handlers = {
	.after_create = janeiro_pm_after_create,
	.before_destroy = janeiro_pm_before_destroy,
	.power_up = janeiro_power_up,
	.power_down = janeiro_power_down,
};

int janeiro_pm_create(struct edgetpu_dev *etdev)
{
	return edgetpu_pm_create(etdev, &janeiro_pm_handlers);
}

void janeiro_pm_destroy(struct edgetpu_dev *etdev)
{
	edgetpu_pm_destroy(etdev);
}
