// SPDX-License-Identifier: GPL-2.0
/*
 * Janeiro EdgeTPU power management support
 *
 * Copyright (C) 2020 Google, Inc,
 */

#include <linux/pm_runtime.h>

#include "edgetpu-internal.h"
#include "edgetpu-pm.h"
#include "janeiro-pm.h"

#include "edgetpu-pm.c"

static int janeiro_pwr_state_init(struct device *dev)
{
	int ret;

	pm_runtime_enable(dev);
	ret = pm_runtime_get_sync(dev);
	if (ret) {
		dev_err(dev, "pm_runtime_get_sync returned %d\n", ret);
		return ret;
	}
	return ret;
}

static int janeiro_pm_after_create(struct edgetpu_pm *etpm)
{
	struct device *dev = etpm->etdev->dev;

	return janeiro_pwr_state_init(dev);
}

static void janeiro_pm_before_destroy(struct edgetpu_pm *etpm)
{
	pm_runtime_disable(etpm->etdev->dev);
}

static struct edgetpu_pm_handlers janeiro_pm_handlers = {
	.after_create = janeiro_pm_after_create,
	.before_destroy = janeiro_pm_before_destroy,
};

int janeiro_pm_create(struct edgetpu_dev *etdev)
{
	return edgetpu_pm_create(etdev, &janeiro_pm_handlers);
}

void janeiro_pm_destroy(struct edgetpu_dev *etdev)
{
	edgetpu_pm_destroy(etdev);
}
