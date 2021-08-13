/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Chip-dependent configuration for mailbox.
 *
 * Copyright (C) 2020 Google, Inc.
 */

#ifndef __JANEIRO_CONFIG_MAILBOX_H__
#define __JANEIRO_CONFIG_MAILBOX_H__

#include <linux/types.h> /* u32 */

#define EDGETPU_NUM_VII_MAILBOXES 7
#define EDGETPU_NUM_P2P_MAILBOXES 0
#define EDGETPU_NUM_EXT_MAILBOXES 4
#define EDGETPU_NUM_MAILBOXES (EDGETPU_NUM_VII_MAILBOXES + EDGETPU_NUM_EXT_MAILBOXES + 1)
/*
 * Mailbox index layout in mailbox manager is like:
 * ---------------------------------------------
 * | KCI X 1 |   VII(s) X 7  | EXT_DSP(s) X 4  |
 * ---------------------------------------------
 */
#define JANEIRO_EXT_DSP_MAILBOX_START (EDGETPU_NUM_VII_MAILBOXES + 1)
#define JANEIRO_EXT_DSP_MAILBOX_END (EDGETPU_NUM_EXT_MAILBOXES + JANEIRO_EXT_DSP_MAILBOX_START - 1)

#define JANEIRO_CSR_MBOX2_CONTEXT_ENABLE 0xa0000 /* starting kernel mb*/
#define JANEIRO_CSR_MBOX11_CONTEXT_ENABLE 0xc0000 /* DSP mailbox */
#define EDGETPU_MBOX_CSRS_SIZE 0x2000 /* CSR size of each mailbox */

#define JANEIRO_CSR_MBOX_CMD_QUEUE_DOORBELL_SET_OFFSET 0x1000
#define JANEIRO_CSR_MBOX_RESP_QUEUE_DOORBELL_SET_OFFSET 0x1800
#define EDGETPU_MBOX_BASE JANEIRO_CSR_MBOX2_CONTEXT_ENABLE

static inline u32 edgetpu_mailbox_get_context_csr_base(u32 index)
{
	u32 base;

	if (index >= 0 && index <= EDGETPU_NUM_VII_MAILBOXES)
		base = JANEIRO_CSR_MBOX2_CONTEXT_ENABLE;
	else
		base = JANEIRO_CSR_MBOX11_CONTEXT_ENABLE;
	return base + (index % JANEIRO_EXT_DSP_MAILBOX_START) * EDGETPU_MBOX_CSRS_SIZE;
}

static inline u32 edgetpu_mailbox_get_cmd_queue_csr_base(u32 index)
{
	u32 base;

	if (index >= 0 && index <= EDGETPU_NUM_VII_MAILBOXES)
		base = JANEIRO_CSR_MBOX2_CONTEXT_ENABLE;
	else
		base = JANEIRO_CSR_MBOX11_CONTEXT_ENABLE;
	return base + JANEIRO_CSR_MBOX_CMD_QUEUE_DOORBELL_SET_OFFSET +
	       ((index % JANEIRO_EXT_DSP_MAILBOX_START) * EDGETPU_MBOX_CSRS_SIZE);
}

static inline u32 edgetpu_mailbox_get_resp_queue_csr_base(u32 index)
{
	u32 base;

	if (index >= 0 && index <= EDGETPU_NUM_VII_MAILBOXES)
		base = JANEIRO_CSR_MBOX2_CONTEXT_ENABLE;
	else
		base = JANEIRO_CSR_MBOX11_CONTEXT_ENABLE;
	return base + JANEIRO_CSR_MBOX_RESP_QUEUE_DOORBELL_SET_OFFSET +
	       ((index % JANEIRO_EXT_DSP_MAILBOX_START) * EDGETPU_MBOX_CSRS_SIZE);
}

#endif /* __JANEIRO_CONFIG_MAILBOX_H__ */
