/*
 * monitor_ioctl.h — Shared ioctl definitions
 *
 * Included by both monitor.c (kernel) and engine.c (user-space).
 * Defines the commands and data structures used to communicate
 * between the supervisor and the kernel memory monitor.
 */

#ifndef MONITOR_IOCTL_H
#define MONITOR_IOCTL_H

#include <linux/ioctl.h>

#define MONITOR_MAGIC 'M'

/* Register a container PID with soft/hard memory limits */
#define MONITOR_REGISTER   _IOW(MONITOR_MAGIC, 1, struct monitor_request)

/* Unregister a container PID */
#define MONITOR_UNREGISTER _IOW(MONITOR_MAGIC, 2, struct monitor_request)

/* Query current RSS of a registered PID */
#define MONITOR_QUERY      _IOWR(MONITOR_MAGIC, 3, struct monitor_request)

struct monitor_request {
    int   pid;                      /* host PID of the container process    */
    char  container_id[32];         /* human-readable container name        */
    unsigned long soft_limit_bytes; /* soft limit: log warning when exceeded */
    unsigned long hard_limit_bytes; /* hard limit: kill process when exceeded */
    unsigned long rss_bytes;        /* filled by kernel on MONITOR_QUERY    */
    int   state;                    /* 0=ok  1=soft_warned  2=killed        */
};

#endif /* MONITOR_IOCTL_H */
