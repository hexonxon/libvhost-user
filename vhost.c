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
#include <sys/mman.h>

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

/* Global device list */
LIST_HEAD(, vhost_dev) g_vhost_dev_list;

/* Vhost global event loop. */
struct event_loop* g_vhost_evloop;

/*
 * Vhost global event loop.
 * We have separate event loops for vhost protocols event and actual device queue events.
 */

__attribute__((constructor))
static void libvhost_init(void)
{
    g_vhost_evloop = evloop_create();
    assert(g_vhost_evloop);
}

static void vhost_evloop_add_fd(int fd, struct event_cb* cb)
{
    evloop_add_fd(g_vhost_evloop, fd, cb);
}

static void vhost_evloop_del_fd(int fd)
{
    evloop_del_fd(g_vhost_evloop, fd);
}

int vhost_run(void)
{
    return evloop_run(g_vhost_evloop);
}

/*
 * Communications
 */

static void handle_message(struct vhost_dev* dev, struct vhost_user_message* msg, int* fds, size_t nfds);

static void drop_connection(struct vhost_dev* dev)
{
    assert(dev->connfd >= 0);

    vhost_evloop_del_fd(dev->connfd);
    close(dev->connfd);
    dev->connfd = -1;
}

static void on_connect(struct vhost_dev* dev)
{
    /* we don't allow more that 1 active connections */
    if (dev->connfd >= 0) {
        return;
    }

    dev->connfd = accept4(dev->listenfd, NULL, NULL, SOCK_CLOEXEC);
    assert(dev->connfd >= 0);

    vhost_evloop_add_fd(dev->connfd, &dev->server_cb);
}

static void on_disconnect(struct vhost_dev* dev)
{
    vhost_reset_dev(dev);
}

static void on_read_avail(struct vhost_dev* dev)
{
    assert(dev->connfd >= 0);

    int res = 0;
    struct vhost_user_message msg;
    int fds[VHOST_USER_MAX_FDS];
    size_t nfds = 0;

    struct iovec iov[1];
    iov[0].iov_base = &msg;
    iov[0].iov_len = sizeof(msg);

    union {
        char buf[CMSG_SPACE(sizeof(fds))];
        struct cmsghdr cmsghdr;
    } u;

    struct msghdr msghdr = {0};
    msghdr.msg_iov = iov;
    msghdr.msg_iovlen = sizeof(iov) / sizeof(*iov);
    msghdr.msg_control = u.buf;
    msghdr.msg_controllen = sizeof(u.buf);

    res = recvmsg(dev->connfd, &msghdr, MSG_CMSG_CLOEXEC | MSG_DONTWAIT);
    if (res < 0 || res < sizeof(msg.hdr)) {
        /*
         * Master is required to send full messages.
         * We can terminate connection if that is not the case
         */
        vhost_reset_dev(dev);
        return;
    }

    struct cmsghdr* cmsg = CMSG_FIRSTHDR(&msghdr);
    if (cmsg) {
        if (cmsg->cmsg_level != SOL_SOCKET ||
            cmsg->cmsg_type != SCM_RIGHTS ||
            cmsg->cmsg_len > CMSG_LEN(sizeof(fds))) {
            /*
             * Master is required to send only a single control header
             * with auxillary file desciptors.
             */
            vhost_reset_dev(dev);
            return;
        }

        nfds = cmsg->cmsg_len / sizeof(*fds);
        memcpy(fds, CMSG_DATA(cmsg), cmsg->cmsg_len);
    }

    handle_message(dev, &msg, fds, nfds);
}

static void send_reply(struct vhost_dev* dev, struct vhost_user_message* msg)
{
    assert(dev);
    assert(msg);
    assert(dev->connfd >= 0);

    msg->hdr.flags |= (1ul << VHOST_USER_MESSAGE_F_REPLY); /* Set reply flag */

    struct iovec iov[1];
    iov[0].iov_base = (void*) msg;
    iov[0].iov_len = sizeof(msg->hdr) + msg->hdr.size;

    struct msghdr msghdr = {0};
    msghdr.msg_iov = iov;
    msghdr.msg_iovlen = sizeof(iov) / sizeof(*iov);

    ssize_t res = sendmsg(dev->connfd, &msghdr, 0);
    if (res < 0) {
        vhost_reset_dev(dev);
        return;
    }
}

static void handle_server_event(struct event_cb* cb, int fd, uint32_t events)
{
    struct vhost_dev* dev = cb->ptr;
    assert(dev);

    if (fd == dev->listenfd) {
        /* we don't expect EPOLLHUP on a listening socket */
        assert((events & ~(uint32_t)EPOLLIN) == 0);

        if (events & EPOLLIN) {
            on_connect(dev);
        }
    } else if (fd == dev->connfd) {
        assert((events & ~(uint32_t)(EPOLLIN | EPOLLHUP | EPOLLERR)) == 0);

        /* Handler disconnects first */
        if (events & (EPOLLHUP | EPOLLERR)) {
            on_disconnect(dev);
        } else {
            if (events & EPOLLIN) {
                on_read_avail(dev);
            }
        }
    } else {
        assert(0);
    }
}

static int create_listen_socket(const char* path)
{
    int error = 0;

    struct sockaddr_un addr;
    addr.sun_family = AF_UNIX;
    if (snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", path) > sizeof(addr.sun_path)) {
        return -ENOSPC;
    }

    int sockfd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (sockfd < 0) {
        return -errno;
    }

    error = bind(sockfd, &addr, sizeof(addr));
    if (error) {
        error = -errno;
        goto error_out;
    }

    error = listen(sockfd, 1);
    if (error) {
        error = -errno;
        goto error_out;
    }

    return sockfd;

error_out:
    close(sockfd);
    return error;
}

int vhost_register_device_server(struct vhost_dev* dev, const char* socket_path)
{
    assert(dev);
    assert(socket_path);

    memset(dev, 0, sizeof(*dev));

    dev->listenfd = create_listen_socket(socket_path);
    if (dev->listenfd < 0) {
        return -1;
    }

    dev->connfd = -1;
    dev->server_cb = (struct event_cb){ EPOLLIN | EPOLLHUP, dev, handle_server_event };
    vhost_evloop_add_fd(dev->listenfd, &dev->server_cb);

    LIST_INSERT_HEAD(&g_vhost_dev_list, dev, link);
    return 0;
}

/*
 * Request handling
 */

static inline bool has_feature(uint64_t features, int fbit)
{
    return (features & (1ull << fbit)) != 0;
}

static bool message_assumes_reply(const struct vhost_user_message* msg)
{
    /* Those message types assume a slave reply by default */
    switch (msg->hdr.request) {
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
            has_feature(msg->hdr.flags, VHOST_USER_MESSAGE_F_REPLY_ACK));
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

static int get_features(struct vhost_dev* dev, struct vhost_user_message* msg, int* fds, size_t nfds)
{
    msg->u64 = VHOST_SUPPORTED_FEATURES;
    msg->hdr.size = sizeof(msg->u64);
    return 0;
}

static int set_features(struct vhost_dev* dev, struct vhost_user_message* msg, int* fds, size_t nfds)
{
    if (msg->u64 & ~VHOST_SUPPORTED_FEATURES) {
        /* Master lies about features we can support */
        return -1;
    }

    dev->negotiated_features = msg->u64;
    return 0;
}

static int get_protocol_features(struct vhost_dev* dev, struct vhost_user_message* msg, int* fds, size_t nfds)
{
    /*
     * Note: VHOST_USER_GET_PROTOCOL_FEATURES can be sent by master even if slave
     *       only advertised but not yet negotiated VHOST_USER_F_PROTOCOL_FEATURES,
     *       so we don't check that here.
     */

    msg->u64 = VHOST_SUPPORTED_PROTOCOL_FEATURES;
    msg->hdr.size = sizeof(msg->u64);
    return 0;
}

static int set_protocol_features(struct vhost_dev* dev, struct vhost_user_message* msg, int* fds, size_t nfds)
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

static int set_owner(struct vhost_dev* dev, struct vhost_user_message* msg, int* fds, size_t nfds)
{
    if (dev->session_started) {
        /* Master tries to start the same session again */
        return -1;
    }

    dev->session_started = true;
    return 0;
}

static int reset_owner(struct vhost_dev* dev, struct vhost_user_message* msg, int* fds, size_t nfds)
{
    /* Spec advises to ignore this message */
    return 0;
}

static int set_mem_table(struct vhost_dev* dev, struct vhost_user_message* msg, int* fds, size_t nfds)
{
    if (msg->mem_regions.num_regions >= VHOST_USER_MAX_FDS ||
        msg->mem_regions.num_regions != nfds) {
        return -1;
    }

    for (size_t i = 0; i < nfds; ++i) {
        struct vhost_user_mem_region* mr = &msg->mem_regions.regions[i];
        int fd = fds[i];

        /* Zero-sized regions look fishy */
        if (mr->size == 0) {
            return -1;
        }

        /* We assume regions to be at least page-aligned */
        if ((mr->guest_addr & (PAGE_SIZE - 1)) ||
            (mr->size & (PAGE_SIZE - 1)) ||
            ((mr->user_addr + mr->mmap_offset) & (PAGE_SIZE - 1))) {
            return -1;
        }

        void* ptr = mmap(NULL, mr->size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, mr->mmap_offset);
        if (ptr == MAP_FAILED) {
            /* device reset will handle unmapping if anything that was mapped */
            return -1;
        }

        dev->mapped_regions[i].fd = fd;
        dev->mapped_regions[i].ptr = ptr;
        dev->mapped_regions[i].mr = *mr;
    }

    return 0;
}

static void handle_message(struct vhost_dev* dev, struct vhost_user_message* msg, int* fds, size_t nfds)
{
    assert(dev);
    assert(msg);
    assert(fds);
    
    static const handler_fptr handler_tbl[] = {
        NULL,           /* */
        get_features,   /* VHOST_USER_GET_FEATURES         */
        set_features,   /* VHOST_USER_SET_FEATURES         */
        set_owner,      /* VHOST_USER_SET_OWNER            */
        reset_owner,    /* VHOST_USER_RESET_OWNER          */
        set_mem_table,  /* VHOST_USER_SET_MEM_TABLE        */
        NULL, /* VHOST_USER_SET_LOG_BASE         */
        NULL, /* VHOST_USER_SET_LOG_FD           */
        NULL, /* VHOST_USER_SET_VRING_NUM        */
        NULL, /* VHOST_USER_SET_VRING_ADDR       */
        NULL, /* VHOST_USER_SET_VRING_BASE       */
        NULL, /* VHOST_USER_GET_VRING_BASE       */
        NULL, /* VHOST_USER_SET_VRING_KICK       */
        NULL, /* VHOST_USER_SET_VRING_CALL       */
        NULL, /* VHOST_USER_SET_VRING_ERR        */
        get_protocol_features, /* VHOST_USER_GET_PROTOCOL_FEATURES*/
        set_protocol_features, /* VHOST_USER_SET_PROTOCOL_FEATURES*/
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

    if (msg->hdr.request == 0 || msg->hdr.request > sizeof(handler_tbl) / sizeof(*handler_tbl)) {
        goto reset;
    }

    int res = 0;
    if (!handler_tbl[msg->hdr.request]) {
        res = ENOTSUP;
    } else {
        res = handler_tbl[msg->hdr.request](dev, msg, fds, nfds);
    }

    if (res < 0) {
        goto reset;
    }

    if (message_assumes_reply(msg)) {
        send_reply(dev, msg);
    } else if (must_reply_ack(dev, msg)) {
        msg->u64 = -res;
        msg->hdr.size = sizeof(msg->u64);
        send_reply(dev, msg);
    }

    return;

reset:
    vhost_reset_dev(dev);
}

void vhost_reset_dev(struct vhost_dev* dev)
{
    /* Drop client connection */
    drop_connection(dev);

    /* Unmap mapped regions */
    for (size_t i = 0; i < VHOST_USER_MAX_FDS; ++i) {
        if (dev->mapped_regions[i].ptr == NULL) {
            break;
        }

        munmap(dev->mapped_regions[i].ptr, dev->mapped_regions[i].mr.size);
    }

    memset(dev, 0, sizeof(*dev));
}
