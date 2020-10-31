/**
 * Vhost protocol handling
 */

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
#include <sys/eventfd.h>

#include "platform.h"
#include "vhost.h"
#include "vhost-protocol.h"

#include "virtio/vdev.h"

#define VHOST_SUPPORTED_FEATURES (\
    (1ull << VHOST_USER_F_PROTOCOL_FEATURES) | \
    (1ull << VIRTIO_F_INDIRECT_DESC) | \
    (1ull << VIRTIO_F_VERSION_1))

#define VHOST_SUPPORTED_PROTOCOL_FEATURES (\
    (1ull << VHOST_USER_PROTOCOL_F_MQ) | \
    (1ull << VHOST_USER_PROTOCOL_F_REPLY_ACK) | \
    (1ull << VHOST_USER_PROTOCOL_F_CONFIG) | \
    (1ull << VHOST_USER_PROTOCOL_F_RESET_DEVICE) | \
    0)

static inline bool has_feature(uint64_t features, int fbit)
{
    return (features & (1ull << fbit)) != 0;
}

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
    VHOST_VERIFY(g_vhost_evloop);
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
    VHOST_VERIFY(dev->connfd >= 0);

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
    VHOST_VERIFY(dev->connfd >= 0);

    vhost_evloop_add_fd(dev->connfd, &dev->server_cb);
}

static void on_disconnect(struct vhost_dev* dev)
{
    VHOST_LOG_DEBUG("dev %p: client disconnected", dev);
    vhost_reset_dev(dev);
}

static void on_read_avail(struct vhost_dev* dev)
{
    VHOST_VERIFY(dev->connfd >= 0);

    int res = 0;
    struct vhost_user_message msg;
    int fds[VHOST_USER_MAX_FDS];

    union {
        char buf[CMSG_SPACE(sizeof(fds))];
        struct cmsghdr cmsghdr;
    } u;

    /*
     * Read header and possible control message.
     */

    struct iovec iov[1];
    iov[0].iov_base = &msg;
    iov[0].iov_len = sizeof(msg.hdr);

    struct msghdr msghdr = {0};
    msghdr.msg_iov = iov;
    msghdr.msg_iovlen = sizeof(iov) / sizeof(*iov);
    msghdr.msg_control = u.buf;
    msghdr.msg_controllen = sizeof(u.buf);

    res = recvmsg(dev->connfd, &msghdr, MSG_CMSG_CLOEXEC | MSG_WAITALL);
    if (res != sizeof(msg.hdr)) {
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

        /*
         * We have enough space in aux buffer to copy maximum number of descriptors
         * since we can't actually know from the cmsg header itself how much there is (portably).
         */
        memcpy(fds, CMSG_DATA(cmsg), sizeof(fds));
    }

    /*
     * Recv message body if any
     */

    if (msg.hdr.size) {
        res = recv(dev->connfd, (char*)&msg + sizeof(msg.hdr), msg.hdr.size, MSG_WAITALL);
        if (res != msg.hdr.size) {
            vhost_reset_dev(dev);
            return;
        }
    }

    handle_message(dev, &msg, fds, VHOST_USER_MAX_FDS);
}

static void send_reply(struct vhost_dev* dev, struct vhost_user_message* msg)
{
    VHOST_VERIFY(dev);
    VHOST_VERIFY(msg);
    VHOST_VERIFY(dev->connfd >= 0);

    msg->hdr.flags = 0x1 | (1ul << VHOST_USER_MESSAGE_F_REPLY); /* Set reply flag */

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
    VHOST_VERIFY(dev);

    if (fd == dev->listenfd) {
        /* we don't expect EPOLLHUP on a listening socket */
        VHOST_VERIFY((events & ~(uint32_t)EPOLLIN) == 0);

        if (events & EPOLLIN) {
            on_connect(dev);
        }
    } else if (fd == dev->connfd) {
        VHOST_VERIFY((events & ~(uint32_t)(EPOLLIN | EPOLLHUP | EPOLLERR)) == 0);

        /* Handler disconnects first */
        if (events & (EPOLLHUP | EPOLLERR)) {
            on_disconnect(dev);
        } else {
            if (events & EPOLLIN) {
                on_read_avail(dev);
            }
        }
    } else {
        VHOST_VERIFY(0);
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

int vhost_register_device_server(struct vhost_dev* dev,
                                 const char* socket_path,
                                 uint8_t num_queues,
                                 struct virtio_dev* vdev,
                                 vring_event_handler_cb vring_cb)
{
    VHOST_VERIFY(dev);
    VHOST_VERIFY(socket_path);
    VHOST_VERIFY(num_queues);
    VHOST_VERIFY(vdev);
    VHOST_VERIFY(vring_cb);

    memset(dev, 0, sizeof(*dev));

    dev->listenfd = create_listen_socket(socket_path);
    if (dev->listenfd < 0) {
        return -1;
    }

    dev->connfd = -1;
    dev->server_cb = (struct event_cb){ EPOLLIN | EPOLLHUP, dev, handle_server_event };
    vhost_evloop_add_fd(dev->listenfd, &dev->server_cb);

    dev->num_queues = num_queues;
    dev->vrings = vhost_calloc(num_queues, sizeof(*dev->vrings));
    for (uint8_t i = 0; i < num_queues; ++i) {
        dev->vrings[i].dev = dev;
        vring_reset(dev->vrings + i);
    }

    dev->vdev = vdev;
    dev->vring_cb = vring_cb;

    LIST_INSERT_HEAD(&g_vhost_dev_list, dev, link);
    return 0;
}

/*
 * Vrings
 */

static void vring_close_fd(struct vring* vring, int* fd)
{
    if (*fd == -1) {
        return;
    }

    if (*fd == vring->kickfd) {
        vhost_evloop_del_fd(*fd);
    }

    close(*fd);
    *fd = -1;
}

static void handle_vring_event(struct event_cb* cb, int fd, uint32_t events)
{
    struct vring* vring = cb->ptr;
    struct vhost_dev* dev = vring->dev;

    VHOST_VERIFY(vring);
    VHOST_VERIFY((events & ~(uint32_t)(EPOLLIN | EPOLLHUP | EPOLLERR)) == 0);

    /* Handler disconnects first */
    if (events & (EPOLLHUP | EPOLLERR)) {
        vring_close_fd(vring, &vring->kickfd);
    } else {
        if (events & EPOLLIN) {
            int error = 0;

            /* Consume the input event */
            eventfd_t unused;
            error = eventfd_read(fd, &unused);
            if (error) {
                VHOST_LOG_DEBUG("eventfd_read(%d) failed", fd);
                goto reset_dev;
            }

            /* According to the spec vrings are started when they receive a first kick */
            if (!vring->is_started) {
                error = vring_start(vring);
            } else {
                error = dev->vring_cb(dev->vdev, vring);
            }

            if (error) {
                goto reset_dev;
            }
        }
    }

    return;

reset_dev:
    vhost_reset_dev(dev);
}

void vring_reset(struct vring* vring)
{
    VHOST_VERIFY(vring);

    vring_close_fd(vring, &vring->kickfd);
    vring_close_fd(vring, &vring->callfd);
    vring_close_fd(vring, &vring->errfd);

    /**
     * Vring is enabled when:
     * - if VHOST_USER_F_PROTOCOL_FEATURES has been negotiated -> on VHOST_USER_SET_VRING_ENABLE(1)
     * - otherwise vring is always enabled
     *
     * Since we don't know when VHOST_USER_F_PROTOCOL_FEATURES will be negotiated,
     * we assume vring to be always enabled until negotiation happens.
     */
    vring->is_enabled = !vring->dev->has_protocol_features;
    vring->is_started = false;
}

int vring_start(struct vring* vring)
{
    VHOST_VERIFY(vring);

    /* Sanity-check what we are starting */
    if (vring->size == 0 || vring->kickfd == -1) {
        return -EINVAL;
    }

    if (vring->is_started) {
        VHOST_LOG_DEBUG("vring %p already started", vring);
        return 0;
    }

    int error = virtqueue_start(&vring->vq,
                                vring->size,
                                vring->desc_addr,
                                vring->avail_addr,
                                vring->used_addr,
                                vring->avail_base,
                                &vring->dev->memory_map);

    if (error) {
        return error;
    }

    vring->is_started = true;
    return 0;
}

void vring_stop(struct vring* vring)
{
    VHOST_VERIFY(vring);

    if (!vring->is_started) {
        VHOST_LOG_DEBUG("vring %p already stopped", vring);
        return;
    }

    /* There is nothing to tell the actual virtqueue for now */
    vring->is_started = false;
}

void vring_notify(struct vring* vring)
{
    if (vring->callfd != -1) {
        eventfd_write(vring->callfd, 0);
    }
}

/*
 * Request handling
 */

static bool message_assumes_reply(const struct vhost_user_message* msg)
{
    /* Those message types assume a slave reply by default */
    switch (msg->hdr.request) {
    case VHOST_USER_GET_FEATURES:
    case VHOST_USER_GET_PROTOCOL_FEATURES:
    case VHOST_USER_GET_VRING_BASE:
    case VHOST_USER_SET_LOG_BASE:
    case VHOST_USER_GET_INFLIGHT_FD:
    case VHOST_USER_GET_QUEUE_NUM:
    case VHOST_USER_GET_CONFIG:
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
    msg->u64 |= dev->vdev->supported_features;

    msg->hdr.size = sizeof(msg->u64);
    return 0;
}

static int set_features(struct vhost_dev* dev, struct vhost_user_message* msg, int* fds, size_t nfds)
{
    if (msg->hdr.size < sizeof(msg->u64)) {
        return -1;
    }

    if (has_feature(msg->u64, VHOST_USER_F_PROTOCOL_FEATURES)) {
        dev->has_protocol_features = true;
    }

    /* Devices don't care about vhost protocol features */
    msg->u64 &= ~(1ull << VHOST_USER_F_PROTOCOL_FEATURES);
    return virtio_dev_set_features(dev->vdev, msg->u64);
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
    if (msg->hdr.size < sizeof(msg->u64)) {
        return -1;
    }

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

/* Convert user address (VA mapped into master's space) to gpa */
static uint64_t uva_to_gpa(struct vhost_dev* dev, uint64_t uva)
{
    for (uint32_t i = 0; i < dev->num_regions; ++i) {
        struct vhost_user_mem_region* mr = &dev->regions[i];
        if (mr->user_addr <= uva && uva <= mr->user_addr + mr->size - 1) {
            return mr->guest_addr + (uva - mr->user_addr);
        }
    }

    return (uint64_t)MAP_FAILED;
}

static void reset_memory_map(struct vhost_dev* dev)
{
    /* Unmap mapped regions */
    for (size_t i = 0; i < dev->memory_map.num_regions; ++i) {
        munmap(dev->memory_map.regions[i].hva, dev->memory_map.regions[i].len);
    }

    dev->memory_map = VIRTIO_INIT_MEMORY_MAP;
    dev->num_regions = 0;
}

static int set_mem_table(struct vhost_dev* dev, struct vhost_user_message* msg, int* fds, size_t nfds)
{
    if (msg->mem_regions.num_regions > VHOST_USER_MAX_FDS) {
        return -1;
    }

    /* Drop current memory map */
    reset_memory_map(dev);

    for (size_t i = 0; i < msg->mem_regions.num_regions; ++i) {
        struct vhost_user_mem_region* mr = &msg->mem_regions.regions[i];
        int fd = fds[i];

        /* Zero-sized regions look fishy */
        if (mr->size == 0) {
            goto reset_dev;
        }

        /* We assume regions to be at least page-aligned */
        if ((mr->guest_addr & (PAGE_SIZE - 1)) ||
            (mr->size & (PAGE_SIZE - 1)) ||
            ((mr->user_addr + mr->mmap_offset) & (PAGE_SIZE - 1))) {
            goto reset_dev;
        }

        void* ptr = mmap(NULL, mr->size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, mr->mmap_offset);
        if (ptr == MAP_FAILED) {
            goto reset_dev;
        }

        int error = virtio_add_guest_region(&dev->memory_map, mr->guest_addr, mr->size, ptr, false);
        if (error) {
            goto reset_dev;
        }

        close(fd);
    }

    memcpy(dev->regions, msg->mem_regions.regions, sizeof(*dev->regions) * msg->mem_regions.num_regions);
    dev->num_regions = msg->mem_regions.num_regions;

    return 0;

reset_dev:
    /* device reset will handle unmapping if anything that was potentially mapped */
    return -1;
}

static int get_queue_num(struct vhost_dev* dev, struct vhost_user_message* msg, int* fds, size_t nfds)
{
    msg->u64 = dev->num_queues;
    msg->hdr.size = sizeof(msg->u64);
    return 0;
}

static int get_config(struct vhost_dev* dev, struct vhost_user_message* msg, int* fds, size_t nfds)
{
    /* Since response size must match the master's request, lets check that its enough */
    if (msg->hdr.size < sizeof(msg->device_config_space) - VHOST_USER_MAX_CONFIG_SIZE) {
        return -1;
    }

    /* Safe to access config header now */
    uint32_t size = msg->device_config_space.size;
    uint32_t offset = msg->device_config_space.offset;
    if (size < offset || size > VHOST_USER_MAX_CONFIG_SIZE) {
        return -1;
    }

    uint32_t space_avail = size - offset;
    return virtio_dev_get_config(dev->vdev, msg->device_config_space.data + offset, space_avail);
}

enum {
    VRING_FD_KICK,
    VRING_FD_CALL,
    VRING_FD_ERR
};

static int set_vring_fd(struct vhost_dev* dev, struct vhost_user_message* msg, int* fds, size_t nfds, int fdtype)
{
    if (msg->hdr.size < sizeof(msg->u64)) {
        return -1;
    }

    uint8_t vring_idx = msg->u64 & 0xFF;
    bool invalid_fd = (msg->u64 & (1ul << 8)) != 0;

    if (vring_idx >= dev->num_queues) {
        return -1;
    }

    int* fd;
    switch (fdtype) {
    case VRING_FD_KICK: fd = &dev->vrings[vring_idx].kickfd; break;
    case VRING_FD_CALL: fd = &dev->vrings[vring_idx].callfd; break;
    case VRING_FD_ERR: fd = &dev->vrings[vring_idx].errfd; break;
    default: VHOST_VERIFY(0);
    };

    /* Close fd in case it was open */
    vring_close_fd(&dev->vrings[vring_idx], fd);

    *fd = (invalid_fd ? -1 : fds[0]);
    return 0;
}

static int set_vring_kick(struct vhost_dev* dev, struct vhost_user_message* msg, int* fds, size_t nfds)
{
    int error = set_vring_fd(dev, msg, fds, nfds, VRING_FD_KICK);
    if (error) {
        return error;
    }

    /*
     * Register vring kickfd in event loop.
     * TODO: for now we are using the global vhost event loop.
     */

    struct vring* vring = &dev->vrings[msg->u64 & 0xFF];
    if (vring->kickfd != -1) {
        vring->kick_cb = (struct event_cb){ EPOLLIN | EPOLLHUP, vring, handle_vring_event };
        vhost_evloop_add_fd(vring->kickfd, &vring->kick_cb);
    }

    return 0;
}

static int set_vring_call(struct vhost_dev* dev, struct vhost_user_message* msg, int* fds, size_t nfds)
{
    return set_vring_fd(dev, msg, fds, nfds, VRING_FD_CALL);
}

static int set_vring_err(struct vhost_dev* dev, struct vhost_user_message* msg, int* fds, size_t nfds)
{
    return set_vring_fd(dev, msg, fds, nfds, VRING_FD_ERR);
}

static int set_vring_num(struct vhost_dev* dev, struct vhost_user_message* msg, int* fds, size_t nfds)
{
    if (msg->hdr.size < sizeof(msg->vring_state)) {
        return -1;
    }

    if (msg->vring_state.index > dev->num_queues) {
        return -1;
    }

    if (msg->vring_state.num > VIRTQ_MAX_SIZE) {
        return -1;
    }

    dev->vrings[msg->vring_state.index].size = msg->vring_state.num;
    return 0;
}

static int set_vring_addr(struct vhost_dev* dev, struct vhost_user_message* msg, int* fds, size_t nfds)
{
    if (msg->hdr.size < sizeof(msg->vring_address)) {
        return -1;
    }

    if (msg->vring_state.index > dev->num_queues) {
        return -1;
    }

    /* Currently we don't support logging */
    if (has_feature(msg->vring_address.flags, VHOST_VRING_F_LOG)) {
        return -1;
    }

    dev->vrings[msg->vring_state.index].avail_addr = uva_to_gpa(dev, msg->vring_address.available);
    dev->vrings[msg->vring_state.index].desc_addr = uva_to_gpa(dev, msg->vring_address.descriptor);
    dev->vrings[msg->vring_state.index].used_addr = uva_to_gpa(dev, msg->vring_address.used);

    return 0;
}

static int set_vring_base(struct vhost_dev* dev, struct vhost_user_message* msg, int* fds, size_t nfds)
{
    if (msg->hdr.size < sizeof(msg->vring_state)) {
        return -1;
    }

    if (msg->vring_state.index > dev->num_queues) {
        return -1;
    }

    dev->vrings[msg->vring_state.index].avail_base = msg->vring_state.num;
    return 0;
}

static int get_vring_base(struct vhost_dev* dev, struct vhost_user_message* msg, int* fds, size_t nfds)
{
    if (msg->hdr.size < sizeof(msg->vring_state)) {
        return -1;
    }

    if (msg->vring_state.index > dev->num_queues) {
        return -1;
    }

    /* Sync avail base between vring and underlying virtqueue */
    struct vring* vring = &dev->vrings[msg->vring_state.index];
    vring->avail_base = vring->vq.last_seen_avail;

    /* Strangely enough spec says that we should stop vring on GET_VRING_BASE */
    vring_stop(vring);

    msg->vring_state.num = vring->avail_base;
    return 0;
}

static void handle_message(struct vhost_dev* dev, struct vhost_user_message* msg, int* fds, size_t nfds)
{
    VHOST_VERIFY(dev);
    VHOST_VERIFY(msg);
    VHOST_VERIFY(fds);
    
    static const handler_fptr handler_tbl[] = {
        NULL,           /* */
        get_features,   /* VHOST_USER_GET_FEATURES         */
        set_features,   /* VHOST_USER_SET_FEATURES         */
        set_owner,      /* VHOST_USER_SET_OWNER            */
        reset_owner,    /* VHOST_USER_RESET_OWNER          */
        set_mem_table,  /* VHOST_USER_SET_MEM_TABLE        */
        NULL, /* VHOST_USER_SET_LOG_BASE         */
        NULL, /* VHOST_USER_SET_LOG_FD           */
        set_vring_num,  /* VHOST_USER_SET_VRING_NUM        */
        set_vring_addr, /* VHOST_USER_SET_VRING_ADDR       */
        set_vring_base, /* VHOST_USER_SET_VRING_BASE       */
        get_vring_base, /* VHOST_USER_GET_VRING_BASE       */
        set_vring_kick, /* VHOST_USER_SET_VRING_KICK       */
        set_vring_call, /* VHOST_USER_SET_VRING_CALL       */
        set_vring_err,  /* VHOST_USER_SET_VRING_ERR        */
        get_protocol_features, /* VHOST_USER_GET_PROTOCOL_FEATURES*/
        set_protocol_features, /* VHOST_USER_SET_PROTOCOL_FEATURES*/
        get_queue_num,         /* VHOST_USER_GET_QUEUE_NUM        */
        NULL, /* VHOST_USER_SET_VRING_ENABLE     */
        NULL, /* VHOST_USER_SEND_RARP            */
        NULL, /* VHOST_USER_NET_SET_MTU          */
        NULL, /* VHOST_USER_SET_SLAVE_REQ_FD     */
        NULL, /* VHOST_USER_IOTLB_MSG            */
        NULL, /* VHOST_USER_SET_VRING_ENDIAN     */
        get_config, /* VHOST_USER_GET_CONFIG           */
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

    VHOST_LOG_DEBUG("dev %p: request %u, size %u, flags 0x%x", dev, msg->hdr.request, msg->hdr.size, msg->hdr.flags);

    if (msg->hdr.request == 0 || msg->hdr.request > sizeof(handler_tbl) / sizeof(*handler_tbl)) {
        VHOST_LOG_DEBUG("dev %p: malformed request", dev);
        goto reset;
    }

    int res = 0;
    if (!handler_tbl[msg->hdr.request]) {
        VHOST_LOG_DEBUG("dev %p: unsupported request", dev);
        res = -ENOTSUP;
    } else {
        res = handler_tbl[msg->hdr.request](dev, msg, fds, nfds);
    }

    if (res < 0) {
        VHOST_LOG_DEBUG("dev %p: request failed", dev);
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
    VHOST_VERIFY(dev);

    /* Drop client connection */
    drop_connection(dev);

    dev->has_protocol_features = false;
    dev->negotiated_protocol_features = 0;
    dev->session_started = false;

    /* Reset vrings */
    for (uint8_t i = 0; i < dev->num_queues; ++i) {
        vring_reset(dev->vrings + i);
    }

    reset_memory_map(dev);
}
