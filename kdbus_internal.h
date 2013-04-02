/*
 * Copyright (C) 2013 Kay Sievers
 * Copyright (C) 2013 Greg Kroah-Hartman <gregkh@linuxfoundation.org>
 * Copyright (C) 2013 Linux Foundation
 *
 * kdbus is free software; you can redistribute it and/or modify it under
 * the terms of the GNU Lesser General Public License as published by the
 * Free Software Foundation; either version 2.1 of the License, or (at
 * your option) any later version.
 */

#ifndef __INTERNAL_KDBUS_H
#define __INTERNAL_KDBUS_H

#include <uapi/linux/major.h>
#include <linux/workqueue.h>
#include <linux/hashtable.h>
#include "kdbus.h"

/* FIXME: move to uapi/linux/major.h */
#define KDBUS_CHAR_MAJOR	222

/* copy the uint64_t "size" value from the userspace-supplied  structure */
#define kdbus_size_user(_s, _b, _t, _m) \
({ \
	u64 __user *_sz = _b + offsetof(_t, _m); \
	get_user(_s, _sz); \
})

/*
 * kdbus namespace
 * - provides a "control" node
 * - owns a major number
 * - owns all created buses
 * - the initial namespace is unnamed and stays around for forver
 * - new namespaces are created by opening the control node and
 *   issuing KDBUS_NS_CREATE
 * - closing the connection destroys the created namespace
 */
struct kdbus_ns {
	unsigned int ref;		/* reference count */
	const char *name;		/* name of the namespace */
	bool disconnected;		/* invalidated data */
	struct kdbus_ns *parent;	/* parent namespace */
	u64 id;				/* global id of this namespace */
	const char *devpath;		/* /dev base directory path */
	int major;			/* device major number for all nodes */
	struct idr idr;			/* map of endpoint minors to buses */
	struct device *dev;		/* control device node, minor == 0 */
	struct mutex lock;		/* ns data lock */
	u64 bus_id_next;		/* next bus id sequence number */
	struct list_head list_entry;
};

/* policy */

struct kdbus_policy_db {
	struct kref kref;
	DECLARE_HASHTABLE(entries_hash, 6);
	struct mutex		entries_lock;
};

struct kdbus_policy_db *kdbus_policy_db_new(void);
void kdbus_policy_db_unref(struct kdbus_policy_db *db);
int kdbus_policy_set_from_user(struct kdbus_policy_db *db,
			       void __user *buf);

/* names registry */
struct kdbus_name_registry {
	struct kref		kref;
	DECLARE_HASHTABLE(entries_hash, 6);
	struct mutex		entries_lock;
};

struct kdbus_name_entry {
	char 			*name;
	u64			flags;
	struct list_head	queue_list;
	struct list_head	conn_entry;
	struct hlist_node	hentry;
	struct kdbus_conn	*conn;
};

struct kdbus_name_registry *kdbus_name_registry_new(void);
void kdbus_name_registry_unref(struct kdbus_name_registry *reg);

int kdbus_name_acquire(struct kdbus_name_registry *reg,
		       struct kdbus_conn *conn,
		       void __user *buf);
int kdbus_name_release(struct kdbus_name_registry *reg,
		       struct kdbus_conn *conn,
		       void __user *buf);
int kdbus_name_list(struct kdbus_name_registry *reg,
		    struct kdbus_conn *conn,
		    void __user *buf);
int kdbus_name_query(struct kdbus_name_registry *reg,
		     struct kdbus_conn *conn,
		     void __user *buf);

struct kdbus_name_entry *kdbus_name_lookup(struct kdbus_name_registry *reg,
					   const char *name, u64 type);
void kdbus_name_remove_by_conn(struct kdbus_name_registry *reg,
			       struct kdbus_conn *conn);

/*
 * kdbus bus
 * - provides a "bus" endpoint
 * - owns additional endpoints
 * - own all bus connections
 * - new buses are created by opening the control node and
 *   issuing KDBUS_BUS_CREATE
 * - closing the connection destroys the created bus
 */
struct kdbus_bus {
	struct kref kref;		/* reference count */
	bool disconnected;		/* invalidated data */
	struct kdbus_ns *ns;		/* namespace of this bus */
	const char *name;		/* bus name */
	u64 id;				/* id of this bus in the namespace */
	struct mutex lock;		/* bus data lock */
	u64 ep_id_next;			/* next endpoint id sequence number */
	u64 conn_id_next;		/* next connection id sequence number */
	u64 msg_id_next;		/* next message id sequence number */
	struct idr conn_idr;		/* map of connection ids */
	DECLARE_HASHTABLE(conn_hash, 6);
	struct kdbus_ep *ep;		/* "bus" default endpoint */
	struct list_head ep_list;	/* endpoints assigned to this bus */
	u64 bus_flags;			/* simple pass-thru flags from userspace to userspace */
	struct kdbus_name_registry *name_registry;
};

/*
 * kdbus endpoint
 * - offers access to a bus, the default device node name is "bus"
 * - additional endpoints can carry a specific policy/filters
 */
struct kdbus_ep {
	struct kref kref;		/* reference count */
	bool disconnected;		/* invalidated data */
	struct kdbus_bus *bus;		/* bus behind this endpoint */
	const char *name;		/* name, prefixed with uid */
	u64 id;				/* id of this endpoint on the bus */
	unsigned int minor;		/* minor of this endpoint in the namespace major */
	struct device *dev;		/* device node of this endpoint */
	umode_t mode;			/* file mode of this endpoint device node */
	uid_t uid;			/* uid owning this endpoint */
	gid_t gid;			/* gid owning this endpoint */
	struct list_head bus_entry;	/* list of endpoints for this bus */
	struct list_head message_list;	/* messages in flight for this endpoint */
	struct list_head connection_list;
	wait_queue_head_t wait;		/* wake up this endpoint */
	struct kdbus_policy_db *policy_db;
};

/*
 * kdbus connection
 * - connection to a control node or an endpoint
 */
enum kdbus_conn_type {
	KDBUS_CONN_UNDEFINED,
	KDBUS_CONN_CONTROL,
	KDBUS_CONN_NS_OWNER,
	KDBUS_CONN_BUS_OWNER,
	KDBUS_CONN_EP,
};

struct kdbus_conn {
	enum kdbus_conn_type type;
	struct kdbus_ns *ns;
	union {
		struct kdbus_ns *ns_owner;
		struct kdbus_bus *bus_owner;
		struct kdbus_ep *ep;
	};
	u64 id;		/* id of the connection on the bus */

	bool active;	/* did the connection say hello yet? */
	bool starter;

	/*
	 * first, horrible cut at messages assigned to connections
	 * odds are, this is going to be slow, but let's measure it first to
	 * see what the real numbers are, and where the bottlenecks are.
	 * Premature optimization and all...
	 */
	struct mutex msg_lock;
	struct list_head msg_list;

	struct hlist_node hentry;

	struct list_head connection_entry;
	struct list_head names_list;
	struct list_head names_queue_list;

	struct work_struct work;
	struct timer_list timer;
};

struct kdbus_kmsg {
	struct kref kref;
	u64 deadline;
	struct kdbus_msg msg;
};

struct kdbus_msg_list_entry {
	struct kdbus_kmsg *kmsg;
	struct list_head list;
};

/* message */
int kdbus_kmsg_new(u64 extra_size, struct kdbus_kmsg **m);
int kdbus_kmsg_new_from_user(void __user *argp, struct kdbus_kmsg **m);
void kdbus_kmsg_unref(struct kdbus_kmsg *kmsg);
int kdbus_kmsg_send(struct kdbus_ep *ep, struct kdbus_kmsg **kmsg);
int kdbus_kmsg_recv(struct kdbus_conn *conn, void __user *buf);

/* kernel generated notifications */
int kdbus_notify_name_change(struct kdbus_conn *conn,
			     struct kdbus_bus *bus, u64 type,
			     u64 old_id, u64 new_id, u64 flags,
			     const char *name);
int kdbus_notify_id_change(struct kdbus_conn *conn,
			   struct kdbus_bus *bus, u64 type,
			   u64 id, u64 flags);
int kdbus_notify_reply_timeout(struct kdbus_ep *ep,
			       const struct kdbus_msg *orig_msg);
int kdbus_notify_reply_dead(struct kdbus_ep *ep,
			    const struct kdbus_msg *orig_msg);

/* main */
extern struct bus_type kdbus_subsys;
void kdbus_release(struct device *dev);
extern struct mutex kdbus_subsys_lock;
extern struct idr kdbus_ns_major_idr;
extern struct kdbus_ns *kdbus_ns_init;

/* namespace */
extern const struct file_operations kdbus_device_ops;
struct kdbus_ns *kdbus_ns_ref(struct kdbus_ns *ns);
void kdbus_ns_disconnect(struct kdbus_ns *ns);
struct kdbus_ns *kdbus_ns_unref(struct kdbus_ns *ns);
int kdbus_ns_new(struct kdbus_ns *parent, const char *name, umode_t mode, struct kdbus_ns **ns);
struct kdbus_ns *kdbus_ns_find(const char *name);

/* bus */
struct kdbus_bus *kdbus_bus_ref(struct kdbus_bus *bus);
void kdbus_bus_unref(struct kdbus_bus *bus);
void kdbus_bus_disconnect(struct kdbus_bus *bus);
int kdbus_bus_new(struct kdbus_ns *ns, const char *name, u64 bus_flags,
		  umode_t mode, uid_t uid, gid_t gid, struct kdbus_bus **bus);
void kdbus_bus_scan_timeout_list(struct kdbus_bus *bus);
struct kdbus_conn *kdbus_bus_find_conn_by_id(struct kdbus_bus *bus, u64 id);

/* endpoint */
struct kdbus_ep *kdbus_ep_ref(struct kdbus_ep *ep);
void kdbus_ep_unref(struct kdbus_ep *ep);

struct kdbus_ep *kdbus_ep_find(struct kdbus_bus *bus, const char *name);
int kdbus_ep_new(struct kdbus_bus *bus, const char *name, umode_t mode,
		 uid_t uid, gid_t gid, struct kdbus_ep **ep);
int kdbus_ep_remove(struct kdbus_ep *ep);
void kdbus_ep_disconnect(struct kdbus_ep *ep);

/* connection */
void kdbus_conn_scan_timeout(struct kdbus_conn *conn);

#endif
