/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Implements utilities for virtual device group of EdgeTPU.
 *
 * Copyright (C) 2019 Google, Inc.
 */
#ifndef __EDGETPU_DEVICE_GROUP_H__
#define __EDGETPU_DEVICE_GROUP_H__

#include <linux/eventfd.h>
#include <linux/list.h>
#include <linux/mutex.h>
#include <linux/rbtree.h>
#include <linux/refcount.h>
#include <linux/seq_file.h>
#include <linux/spinlock.h>
#include <linux/types.h>

#include "edgetpu-dram.h"
#include "edgetpu-internal.h"
#include "edgetpu-mailbox.h"
#include "edgetpu-mapping.h"
#include "edgetpu.h"

/* entry of edgetpu_device_group#clients */
struct edgetpu_list_client {
	struct list_head list;
	struct edgetpu_client *client;
};

enum edgetpu_device_group_status {
	/* Waiting for new clients to join. */
	EDGETPU_DEVICE_GROUP_WAITING,
	/* Most operations can only apply on a finalized group. */
	EDGETPU_DEVICE_GROUP_FINALIZED,
	/* No operations except member leaving can be performed. */
	EDGETPU_DEVICE_GROUP_DISBANDED,
};

#define EDGETPU_EVENT_COUNT 2

/* eventfds registered for event notifications from kernel for a device group */
struct edgetpu_events {
	rwlock_t lock;
	struct eventfd_ctx *eventfds[EDGETPU_EVENT_COUNT];
};

struct edgetpu_device_group {
	/*
	 * Reference count.
	 * edgetpu_device_group_get() increases the counter by one and
	 * edgetpu_device_group_put() decreases it. This object will be freed
	 * when ref_count becomes zero.
	 */
	refcount_t ref_count;
	uint workload_id;
	/*
	 * List of clients belonging to this group.
	 * The first client is the leader.
	 */
	struct list_head clients;
	uint n_clients;			/* number of clients in the list */
	/*
	 * Array of the clients belonging to this group.
	 * Clients in this field are same as @clients, but this field is
	 * allocated only when a group is finalized. See
	 * edgetpu_device_group_nth_etdev() for more details.
	 */
	struct edgetpu_client **members;
	enum edgetpu_device_group_status status;
	struct edgetpu_dev *etdev;	/* the device opened by the leader */
	struct edgetpu_vii vii;		/* VII mailbox */
	/* matrix of P2P mailboxes */
	struct edgetpu_p2p_mailbox **p2p_mailbox_matrix;
	/* protects clients, n_clients, status, and vii */
	struct mutex lock;
	/* TPU IOVA mapped to host DRAM space */
	struct edgetpu_mapping_root host_mappings;
	/* TPU IOVA mapped to buffers backed by dma-buf */
	struct edgetpu_mapping_root dmabuf_mappings;
	struct edgetpu_events events;
	/* Mailbox attributes used to create this group */
	struct edgetpu_mailbox_attr mbox_attr;
};

/*
 * Returns if the group is waiting for members to join.
 *
 * Must be called with lock held.
 */
static inline bool edgetpu_device_group_is_waiting(
		const struct edgetpu_device_group *group)
{
	return group->status == EDGETPU_DEVICE_GROUP_WAITING;
}

/*
 * Returns if the group is finalized.
 *
 * Must be called with lock held.
 */
static inline bool edgetpu_device_group_is_finalized(
		const struct edgetpu_device_group *group)
{
	return group->status == EDGETPU_DEVICE_GROUP_FINALIZED;
}

/*
 * Returns if the group is disbanded.
 *
 * Must be called with lock held.
 */
static inline bool edgetpu_device_group_is_disbanded(
		const struct edgetpu_device_group *group)
{
	return group->status == EDGETPU_DEVICE_GROUP_DISBANDED;
}

/* Increases ref_count of @group by one and returns @group. */
static inline struct edgetpu_device_group *
edgetpu_device_group_get(struct edgetpu_device_group *group)
{
	WARN_ON_ONCE(!refcount_inc_not_zero(&group->ref_count));
	return group;
}

/*
 * Decreases ref_count of @group by one.
 *
 * If @group->ref_count becomes 0, @group will be freed.
 */
void edgetpu_device_group_put(struct edgetpu_device_group *group);

/*
 * Allocates a device group with @client as the group leader.
 *
 * @client must not already belong to (either as a leader or a member) another
 * group. @client->group will be set as the returned group on success.
 *
 * Call edgetpu_device_group_put() when the returned group is not needed.
 *
 * Returns allocated group, or a negative errno on error.
 * Returns -EINVAL if the client already belongs to a group.
 */
struct edgetpu_device_group *edgetpu_device_group_alloc(
		struct edgetpu_client *client,
		const struct edgetpu_mailbox_attr *attr);

/*
 * Adds a client to the device group.
 *
 * @group->ref_count will be increased by two on success:
 * - @client->group will be set to @group.
 * - @group will be added to @client->etdev->groups.
 *
 * @client must not already belong to another group, otherwise -EINVAL is
 * returned.
 *
 * Returns 0 on success, or a negative errno on error.
 */
int edgetpu_device_group_add(struct edgetpu_device_group *group,
			     struct edgetpu_client *client);

/*
 * Returns the edgetpu_dev opened by the @n-th client in this group, 0-based.
 *
 * This function returns NULL if "and only if" @group is not finalized or @n is
 * invalid.
 *
 * Caller holds the group lock.
 *
 * Returns the pointer to the edgetpu_dev.
 */
static inline struct edgetpu_dev *edgetpu_device_group_nth_etdev(
		struct edgetpu_device_group *group, uint n)
{
	if (!group->members || n >= group->n_clients)
		return NULL;
	return group->members[n]->etdev;
}

/*
 * Let @client leave the group it belongs to. Caller should hold the client's
 * etdev state_lock.
 *
 * If @client is the leader of a group, the group will be marked as "disbanded".
 *
 * If @client is a member of a group, we have two cases depending on whether the
 * group is in status EDGETPU_DEVICE_GROUP_WAITING:
 * 1. If the group is waiting for members to join, this function simply removes
 *    @client from the group, and new members still can join the group.
 * 2. Otherwise, the group will be marked as "disbanded", no operations except
 *    members leaving can apply to the group.
 *
 * @client->group will be removed from @client->etdev->groups.
 * @client->group will be set as NULL.
 */
void edgetpu_device_group_leave_locked(struct edgetpu_client *client);

/* Let @client leave the group. Device should be in good state, warn if not. */
void edgetpu_device_group_leave(struct edgetpu_client *client);

/* Returns whether @client is the leader of @group. */
bool edgetpu_device_group_is_leader(struct edgetpu_device_group *group,
				    const struct edgetpu_client *client);

/*
 * Finalizes the group.
 *
 * A finalized group is not allowed to add new members.
 *
 * Returns 0 on success.
 * Returns -EINVAL if the group is not waiting for new members to join.
 */
int edgetpu_device_group_finalize(struct edgetpu_device_group *group);

/*
 * Maps buffer to a device group.
 *
 * @arg->device_address will be set as the mapped TPU VA on success.
 *
 * Returns zero on success or a negative errno on error.
 */
int edgetpu_device_group_map(struct edgetpu_device_group *group,
			     struct edgetpu_map_ioctl *arg);

/* Unmap a userspace buffer from a device group. */
int edgetpu_device_group_unmap(struct edgetpu_device_group *group,
			       u32 die_index, tpu_addr_t tpu_addr,
			       edgetpu_map_flag_t flags);

/* Sync the buffer previously mapped by edgetpu_device_group_map. */
int edgetpu_device_group_sync_buffer(struct edgetpu_device_group *group,
				     const struct edgetpu_sync_ioctl *arg);

/* Clear all mappings for a device group. */
void edgetpu_mappings_clear_group(struct edgetpu_device_group *group);

/* Return context ID for group MMU mappings, based on VII mailbox index. */
static inline enum edgetpu_context_id
edgetpu_group_context_id(struct edgetpu_device_group *group)
{
	return EDGETPU_CONTEXT_VII_BASE + group->vii.mailbox->mailbox_id - 1;
}

/* dump mappings in @group */
void edgetpu_group_mappings_show(struct edgetpu_device_group *group,
				 struct seq_file *s);

/*
 * Maps the VII mailbox CSR.
 *
 * Returns 0 on success.
 */
int edgetpu_mmap_csr(struct edgetpu_device_group *group,
		     struct vm_area_struct *vma);
/*
 * Maps the cmd/resp queue memory.
 *
 * Returns 0 on success.
 */
int edgetpu_mmap_queue(struct edgetpu_device_group *group,
		       enum mailbox_queue_type type,
		       struct vm_area_struct *vma);

/* Set group eventfd for event notification */
int edgetpu_group_set_eventfd(struct edgetpu_device_group *group, uint event_id,
			      int eventfd);

/* Unset previously-set group eventfd */
void edgetpu_group_unset_eventfd(struct edgetpu_device_group *group,
				 uint event_id);

/* Notify group of event */
void edgetpu_group_notify(struct edgetpu_device_group *group, uint event_id);

/* Is device in any group (and may be actively processing requests) */
bool edgetpu_in_any_group(struct edgetpu_dev *etdev);

/*
 * Enable or disable device group join lockout (as during f/w load).
 * Returns false if attempting to lockout group join but device is already
 * joined to a group.
 */
bool edgetpu_set_group_join_lockout(struct edgetpu_dev *etdev, bool lockout);

/* Notify all device groups of @etdev about a failure on the die */
void edgetpu_fatal_error_notify(struct edgetpu_dev *etdev);

#endif /* __EDGETPU_DEVICE_GROUP_H__ */
