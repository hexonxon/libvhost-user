#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <sys/queue.h>

#include "evloop.h"
#include "vhost-protocol.h"

#define PAGE_SIZE 4096ull

struct vhost_user_message;

/**
 * Mapped memory region shared with us by the master.
 */
struct vhost_mapped_region
{
    /** mmap fd */
    int fd;

    /** mapped host ptr */
    void* ptr;

    /** region description provided by VHOST_USER_SET_MEM_TABLE */
    struct vhost_user_mem_region mr;
};

/**
 * Vhost device.
 * Basic vhost slave per-device context independent of the actual device type.
 */
struct vhost_dev
{
    /** listen socket fd for listening devices */
    int listenfd;

    /** connected slave fd */
    int connfd;

    /** event callback for server events */
    struct event_cb server_cb;

    /** features we negotiated successfully over handshake */
    uint64_t negotiated_features;
    uint64_t negotiated_protocol_features;

    /** We have received VHOST_USER_SET_OWNER */
    bool session_started;

    /** Number of virt queues we support */
    uint8_t num_queues;

    /** Mapped memory regions for this device */
    struct vhost_mapped_region mapped_regions[VHOST_USER_MAX_FDS];

    LIST_ENTRY(vhost_dev) link;
};

int vhost_register_device_server(struct vhost_dev* dev, const char* socket_path, uint8_t num_queues);
void vhost_reset_dev(struct vhost_dev* dev);

/**
 * Run main vhost event loop
 */
int vhost_run();
