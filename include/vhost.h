#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <sys/queue.h>

#include "evloop.h"

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

    LIST_ENTRY(vhost_dev) link;
};

int vhost_register_device_server(struct vhost_dev* dev, const char* socket_path);
void vhost_drop_connection(struct vhost_dev* dev);
void vhost_handle_message(struct vhost_dev* dev, struct vhost_user_message* msg, int* fds, size_t nfds);
void vhost_send_reply(struct vhost_dev* dev, const struct vhost_user_message* msg);
