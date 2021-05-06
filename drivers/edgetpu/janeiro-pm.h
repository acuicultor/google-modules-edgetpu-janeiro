/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Power management header for Janeiro.
 *
 * Copyright (C) 2020 Google, Inc.
 */
#ifndef __JANEIRO_PM_H__
#define __JANEIRO_PM_H__

int janeiro_pm_create(struct edgetpu_dev *etdev);

void janeiro_pm_destroy(struct edgetpu_dev *etdev);

#endif /* __JANEIRO_PM_H__ */
