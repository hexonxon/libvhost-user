/**
 * Vhost server connection handling
 */

#include <assert.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdint.h>

#include <unistd.h>
#include <sys/queue.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>

#include "vhost.h"
#include "vhost-protocol.h"

/*
 * Vhost global event loop.
 * We have separate event loops for vhost protocols event and actual device queue events.
 */

struct event_loop* g_vhost_evloop;

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

/*
 * Device state management
 */

/* Global device list */
LIST_HEAD(, vhost_dev) g_vhost_dev_list;

static void vhost_on_connect(struct vhost_dev* dev)
{
    /* we don't allow more that 1 active connections */
    if (dev->connfd >= 0) {
        return;
    }

    dev->connfd = accept4(dev->listenfd, NULL, NULL, SOCK_CLOEXEC);
    assert(dev->connfd >= 0);

    vhost_evloop_add_fd(dev->connfd, &dev->server_cb);
}

static void vhost_on_disconnect(struct vhost_dev* dev)
{
    vhost_drop_connection(dev);
}

static void vhost_on_read_avail(struct vhost_dev* dev)
{
    assert(dev->connfd >= 0);

    int res = 0;
    struct vhost_user_message msg;
    int fds[VHOST_USER_MAX_FDS];

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
    if (res < 0) {
        /*
         * Master is required to send full messages.
         * We can terminate connection if that is not the case
         */
        vhost_drop_connection(dev);
        return;
    }

    fprintf(stdout, "received %d bytes (%zu + %zu)\n", res, sizeof(msg), sizeof(msghdr));
    assert(res == sizeof(msg) + sizeof(msghdr));

    struct cmsghdr* cmsg = CMSG_FIRSTHDR(&msghdr);
    if (!cmsg ||
        cmsg->cmsg_level != SOL_SOCKET ||
        cmsg->cmsg_type != SCM_RIGHTS ||
        cmsg->cmsg_len > CMSG_LEN(sizeof(fds))) {
        /*
         * Master is required to send only a single control header
         * with auxillary file desciptors.
         */
        vhost_drop_connection(dev);
        return;
    }

    memcpy(fds, CMSG_DATA(cmsg), cmsg->cmsg_len);
    vhost_handle_message(dev, &msg, fds, cmsg->cmsg_len / sizeof(*fds));
}

void vhost_send_reply(struct vhost_dev* dev, const struct vhost_user_message* msg)
{
    assert(dev);
    assert(msg);
    assert(dev->connfd >= 0);

    struct iovec iov[1];
    iov[0].iov_base = (void*) msg;
    iov[0].iov_len = sizeof(*msg);

    struct msghdr msghdr = {0};
    msghdr.msg_iov = iov;
    msghdr.msg_iovlen = sizeof(iov) / sizeof(*iov);

    ssize_t res = sendmsg(dev->connfd, &msghdr, 0);
    if (res < 0) {
        vhost_drop_connection(dev);
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
            vhost_on_connect(dev);
        }
    } else if (fd == dev->connfd) {
        assert((events & ~(uint32_t)(EPOLLIN | EPOLLHUP)) == 0);

        /* Handle reads first */
        if (events & EPOLLIN) {
            vhost_on_read_avail(dev);
        }
        
        if (events & EPOLLHUP) {
            vhost_on_disconnect(dev);
        }
    } else {
        assert(0);
    }
}

int vhost_register_device_server(struct vhost_dev* dev, const char* socket_path)
{
    assert(dev);
    assert(socket_path);

    vhost_reset_dev(dev);

    int error = 0;

    /*
     * Create and configure listening socket
     */

    struct sockaddr_un addr;
    addr.sun_family = AF_UNIX;
    if (snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", socket_path) > sizeof(addr.sun_path)) {
        return -ENOSPC;
    }

    dev->listenfd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (dev->listenfd < 0) {
        return -errno;
    }

    error = bind(dev->listenfd, &addr, sizeof(addr));
    if (error) {
        error = -errno;
        goto error_out;
    }

    error = listen(dev->listenfd, 1);
    if (error) {
        error = -errno;
        goto error_out;
    }

    /*
     * Register listen socket with the global vhost event loop
     */

    dev->server_cb = (struct event_cb){ EPOLLIN | EPOLLHUP, dev, handle_server_event };
    vhost_evloop_add_fd(dev->listenfd, &dev->server_cb);

    /*
     * Insert device into devices list and return
     */

    LIST_INSERT_HEAD(&g_vhost_dev_list, dev, link);
    return 0;

error_out:
    close(dev->listenfd);
    return error;
}

void vhost_drop_connection(struct vhost_dev* dev)
{
    assert(dev->connfd >= 0);

    vhost_evloop_del_fd(dev->connfd);
    close(dev->connfd);

    vhost_reset_dev(dev);
}
