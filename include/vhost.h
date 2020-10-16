#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <sys/queue.h>

#include "evloop.h"
#include "vhost-protocol.h"

#include "virtio/memory.h"

#define PAGE_SIZE 4096ull

struct vhost_user_message;

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
    struct virtio_memory_map memory_map;

    /** Device-specific config space buffer */
    uint8_t config_space[VHOST_USER_MAX_CONFIG_SIZE];

    LIST_ENTRY(vhost_dev) link;
};

int vhost_register_device_server(struct vhost_dev* dev, const char* socket_path, uint8_t num_queues);
void vhost_reset_dev(struct vhost_dev* dev);

/**
 * Run main vhost event loop
 */
int vhost_run();
