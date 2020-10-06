/**
 * Vhost protocol message handlinh
 */

#include <assert.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>

#include <unistd.h>
#include <sys/queue.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>

#include "vhost.h"
#include "vhost-protocol.h"

#define VHOST_SUPPORTED_FEATURES \
    (1ull << VHOST_USER_F_PROTOCOL_FEATURES) | \
    0

#define VHOST_SUPPORTED_PROTOCOL_FEATURES \
    (1ull << VHOST_USER_PROTOCOL_F_MQ) | \
    (1ull << VHOST_USER_PROTOCOL_F_REPLY_ACK) | \
    (1ull << VHOST_USER_PROTOCOL_F_CONFIG) | \
    (1ull << VHOST_USER_PROTOCOL_F_RESET_DEVICE) | \
    0

static inline bool has_feature(uint64_t features, int fbit)
{
    return (features & (1ull << fbit)) != 0;
}

static bool message_assumes_reply(const struct vhost_user_message* msg)
{
    /* Those message types assume a slave reply by default */
    switch (msg->request) {
    case VHOST_USER_GET_FEATURES:
    case VHOST_USER_GET_PROTOCOL_FEATURES:
    case VHOST_USER_GET_VRING_BASE:
    case VHOST_USER_SET_LOG_BASE:
    case VHOST_USER_GET_INFLIGHT_FD:
        return true;
    default:
        return false;
    }
}

static bool must_reply_ack(const struct vhost_dev* dev, const struct vhost_user_message* msg)
{
    /*
     * if VHOST_USER_PROTOCOL_F_REPLY_ACK has been negotiated
     * and message has VHOST_USER_PROTOCOL_F_REPLY_ACK flag set,
     * then reply is always required.
     */
    return (has_feature(dev->negotiated_protocol_features, VHOST_USER_PROTOCOL_F_REPLY_ACK) &&
            has_feature(msg->flags, VHOST_USER_PROTOCOL_F_REPLY_ACK));
}

/*
 * Request handler type
 *
 * What to reply to master is decided by looking at return value:
 * - =0 command was handled successfully and handler potentially prepared a reply in msg buffer.
 *      prepared reply will be sent to master if command assumes a reply.
 *      if command doesn't assume a reply, but master set REPLY_ACK, caller will sent a zero-value ACK.
 * - >0 we failed to handle request. caller will reply with -res if master set REPLY_ACK
 * - <0 if we didn't like master's request and need to close connection immediately.
 */
typedef int (*handler_fptr) (struct vhost_dev*, struct vhost_user_message*, int*, size_t);

static int vhost_get_features(struct vhost_dev* dev, struct vhost_user_message* msg, int* fds, size_t nfds)
{
    msg->u64 = VHOST_SUPPORTED_FEATURES;
    return 0;
}

static int vhost_set_features(struct vhost_dev* dev, struct vhost_user_message* msg, int* fds, size_t nfds)
{
    if (msg->u64 & ~VHOST_SUPPORTED_FEATURES) {
        /* Master lies about features we can support */
        return -1;
    }

    dev->negotiated_features = msg->u64;
    return 0;
}

static int vhost_get_protocol_features(struct vhost_dev* dev, struct vhost_user_message* msg, int* fds, size_t nfds)
{
    /*
     * Note: VHOST_USER_GET_PROTOCOL_FEATURES can be sent by master even if slave
     *       only advertised but not yet negotiated VHOST_USER_F_PROTOCOL_FEATURES,
     *       so we don't check that here.
     */

    msg->u64 = VHOST_SUPPORTED_PROTOCOL_FEATURES;
    return 0;
}

static int vhost_set_protocol_features(struct vhost_dev* dev, struct vhost_user_message* msg, int* fds, size_t nfds)
{
    /*
     * Note: VHOST_USER_SET_PROTOCOL_FEATURES can be sent by master even if slave
     *       only advertised but not yet negotiated VHOST_USER_F_PROTOCOL_FEATURES,
     *       so we don't check that here.
     */

    if (msg->u64 & ~VHOST_SUPPORTED_PROTOCOL_FEATURES) {
        /* Master lies about protocol features we can support */
        return -1;
    }

    dev->negotiated_protocol_features = msg->u64;
    return 0;
}

static int vhost_set_owner(struct vhost_dev* dev, struct vhost_user_message* msg, int* fds, size_t nfds)
{
    if (dev->session_started) {
        /* Master tries to start the same session again */
        return -1;
    }

    dev->session_started = true;
    return 0;
}

static int vhost_reset_owner(struct vhost_dev* dev, struct vhost_user_message* msg, int* fds, size_t nfds)
{
    /* Spec advises to ignore this message */
    return 0;
}

void vhost_handle_message(struct vhost_dev* dev, struct vhost_user_message* msg, int* fds, size_t nfds)
{
    assert(dev);
    assert(msg);
    assert(fds);
    
    static const handler_fptr handler_tbl[] = {
        NULL,               /* */
        vhost_get_features, /* VHOST_USER_GET_FEATURES         */
        vhost_set_features, /* VHOST_USER_SET_FEATURES         */
        vhost_set_owner,    /* VHOST_USER_SET_OWNER            */
        vhost_reset_owner,  /* VHOST_USER_RESET_OWNER          */
        NULL, /* VHOST_USER_SET_MEM_TABLE        */
        NULL, /* VHOST_USER_SET_LOG_BASE         */
        NULL, /* VHOST_USER_SET_LOG_FD           */
        NULL, /* VHOST_USER_SET_VRING_NUM        */
        NULL, /* VHOST_USER_SET_VRING_ADDR       */
        NULL, /* VHOST_USER_SET_VRING_BASE       */
        NULL, /* VHOST_USER_GET_VRING_BASE       */
        NULL, /* VHOST_USER_SET_VRING_KICK       */
        NULL, /* VHOST_USER_SET_VRING_CALL       */
        NULL, /* VHOST_USER_SET_VRING_ERR        */
        vhost_get_protocol_features, /* VHOST_USER_GET_PROTOCOL_FEATURES*/
        vhost_set_protocol_features, /* VHOST_USER_SET_PROTOCOL_FEATURES*/
        NULL, /* VHOST_USER_GET_QUEUE_NUM        */
        NULL, /* VHOST_USER_SET_VRING_ENABLE     */
        NULL, /* VHOST_USER_SEND_RARP            */
        NULL, /* VHOST_USER_NET_SET_MTU          */
        NULL, /* VHOST_USER_SET_SLAVE_REQ_FD     */
        NULL, /* VHOST_USER_IOTLB_MSG            */
        NULL, /* VHOST_USER_SET_VRING_ENDIAN     */
        NULL, /* VHOST_USER_GET_CONFIG           */
        NULL, /* VHOST_USER_SET_CONFIG           */
        NULL, /* VHOST_USER_CREATE_CRYPTO_SESSION*/
        NULL, /* VHOST_USER_CLOSE_CRYPTO_SESSION */
        NULL, /* VHOST_USER_POSTCOPY_ADVISE      */
        NULL, /* VHOST_USER_POSTCOPY_LISTEN      */
        NULL, /* VHOST_USER_POSTCOPY_END         */
        NULL, /* VHOST_USER_GET_INFLIGHT_FD      */
        NULL, /* VHOST_USER_SET_INFLIGHT_FD      */
        NULL, /* VHOST_USER_GPU_SET_SOCKET       */
        NULL, /* VHOST_USER_RESET_DEVICE         */
        NULL, /* VHOST_USER_VRING_KICK           */
        NULL, /* VHOST_USER_GET_MAX_MEM_SLOTS    */
        NULL, /* VHOST_USER_ADD_MEM_REG          */
        NULL, /* VHOST_USER_REM_MEM_REG          */
        NULL, /* VHOST_USER_SET_STATUS           */
        NULL, /* VHOST_USER_GET_STATUS           */
    };

    if (msg->request == 0 || msg->request > sizeof(handler_tbl) / sizeof(*handler_tbl)) {
        goto drop;
    }

    int res = 0;
    if (!handler_tbl[msg->request]) {
        res = ENOTSUP;
    } else {
        res = handler_tbl[msg->request](dev, msg, fds, nfds);
    }

    if (res < 0) {
        goto drop;
    }

    if (message_assumes_reply(msg)) {
        vhost_send_reply(dev, msg);
    } else if (must_reply_ack(dev, msg)) {
        msg->u64 = -res;
        vhost_send_reply(dev, msg);
    }

    return;

drop:
    vhost_drop_connection(dev);
}

void vhost_reset_dev(struct vhost_dev* dev)
{
    memset(dev, 0, sizeof(*dev));
    dev->connfd = -1;
}
