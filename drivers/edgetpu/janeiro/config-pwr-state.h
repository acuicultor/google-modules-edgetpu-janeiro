/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Chip-dependent power configuration and states.
 *
 * Copyright (C) 2021 Google, Inc.
 */

#ifndef __JANEIRO_CONFIG_PWR_STATE_H__
#define __JANEIRO_CONFIG_PWR_STATE_H__

/*
 * TPU Power States:
 * 0:		Off
 * 227000	Ultra Underdrive @227MHz
 * 625000:	Super Underdrive @625MHz
 * 845000:	Underdrive @845MHz
 * 1066000:	Nominal @1066MHz
 */
enum edgetpu_pwr_state {
	TPU_OFF = 0,
	TPU_ACTIVE_UUD = 227000,
	TPU_ACTIVE_SUD = 625000,
	TPU_ACTIVE_UD  = 845000,
	TPU_ACTIVE_NOM = 1066000,
};

#define MIN_ACTIVE_STATE	TPU_ACTIVE_UUD

#define EDGETPU_NUM_STATES 4

extern enum edgetpu_pwr_state edgetpu_active_states[];

extern uint32_t *edgetpu_states_display;

#define TPU_POLICY_MAX	TPU_ACTIVE_NOM

#define TPU_ACPM_DOMAIN	7

#endif /* __JANEIRO_CONFIG_PWR_STATE_H__ */
