/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Chip-dependent configuration for mailbox.
 *
 * Copyright (C) 2020 Google, Inc.
 */

#ifndef __JANEIRO_CONFIG_MAILBOX_H__
#define __JANEIRO_CONFIG_MAILBOX_H__

#include <linux/types.h> /* u32 */

#define EDGETPU_NUM_MAILBOXES 8
#define EDGETPU_NUM_VII_MAILBOXES (EDGETPU_NUM_MAILBOXES - 1)
#define EDGETPU_NUM_P2P_MAILBOXES 0

#define JANEIRO_CSR_MBOX2_CONTEXT_ENABLE 0xa0000	/* starting kernel mb*/
#define JANEIRO_CSR_MBOX2_CMD_QUEUE_DOORBELL_SET 0xa1000
#define JANEIRO_CSR_MBOX2_RESP_QUEUE_DOORBELL_SET 0xa1800
#define EDGETPU_MBOX_CSRS_SIZE 0x2000	/* CSR size of each mailbox */

#define EDGETPU_MBOX_BASE JANEIRO_CSR_MBOX2_CONTEXT_ENABLE
// TODO: check correct values
/* CSR storing mailbox response queue doorbell status */
#define HOST_NONSECURE_INT_SRC_STATUS_REG 0x000f0000
#define HOST_NONSECURE_INT_SRC_CLEAR_REG 0x000f0008

static inline u32 edgetpu_mailbox_get_context_csr_base(u32 index)
{
	return JANEIRO_CSR_MBOX2_CONTEXT_ENABLE +
		index * EDGETPU_MBOX_CSRS_SIZE;
}

static inline u32 edgetpu_mailbox_get_cmd_queue_csr_base(u32 index)
{
	return JANEIRO_CSR_MBOX2_CMD_QUEUE_DOORBELL_SET +
		index * EDGETPU_MBOX_CSRS_SIZE;
}

static inline u32 edgetpu_mailbox_get_resp_queue_csr_base(u32 index)
{
	return JANEIRO_CSR_MBOX2_RESP_QUEUE_DOORBELL_SET +
		index * EDGETPU_MBOX_CSRS_SIZE;
}
#endif /* __JANEIRO_CONFIG_MAILBOX_H__ */
