#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/epoll.h>
#include <sys/queue.h>

#include "platform.h"
#include "evloop.h"

struct event_ctx
{
    int fd;
    struct event_cb* cb;

    LIST_ENTRY(event_ctx) link;
};

struct event_loop
{
    enum {
        EV_MAX = 32,
    };

    int epollfd;

    /*
     * Head of registered event fds list.
     *
     * We allow N:N fd to event_cb mapping. This list exists to maintain that relationship.
     * Actual epoll events point right to event_ctx structures.
     */
    LIST_HEAD(, event_ctx) ev_map;

    /*
     * Inflight events bookkeeping.
     *
     * We keep track of a number of events we got from last epoll_wait call
     * to make sure we ignore an event for an fd that has been closed by the user
     * from their callback handler.
     */
    size_t ev_pos;
    size_t ev_count;
    struct epoll_event ev_inflight[EV_MAX];
};

struct event_loop* evloop_create(void)
{
    struct event_loop* evloop = vhost_alloc(sizeof(*evloop));
    evloop->epollfd = epoll_create1(EPOLL_CLOEXEC);
    if (evloop->epollfd == -1) {
        return NULL;
    }

    evloop->ev_pos = 0;
    evloop->ev_count = 0;
    LIST_INIT(&evloop->ev_map);

    return evloop;
}

void evloop_free(struct event_loop* evloop)
{
    if (!evloop) {
        return;
    }

    close(evloop->epollfd);

    while (!LIST_EMPTY(&evloop->ev_map)) {
        struct event_ctx* pctx = LIST_FIRST(&evloop->ev_map);
        LIST_REMOVE(pctx, link);
        vhost_free(pctx);
    }

    vhost_free(evloop);
}

int evloop_add_fd(struct event_loop* evloop, int fd, struct event_cb* cb)
{
    VHOST_VERIFY(evloop);
    VHOST_VERIFY(cb);

    struct event_ctx* ctx = vhost_alloc(sizeof(*ctx));
    ctx->fd = fd;
    ctx->cb = cb;

    struct epoll_event ev;
    ev.events = cb->events & (uint32_t)(EPOLLIN | EPOLLHUP);
    ev.data.ptr = ctx;

    int error = epoll_ctl(evloop->epollfd, EPOLL_CTL_ADD, fd, &ev);
    if (error) {
        vhost_free(ctx);
        return -1;
    }

    LIST_INSERT_HEAD(&evloop->ev_map, ctx, link);
    return 0;
}

int evloop_del_fd(struct event_loop* evloop, int fd)
{
    VHOST_VERIFY(evloop);

    int error = epoll_ctl(evloop->epollfd, EPOLL_CTL_DEL, fd, NULL);
    if (error) {
        return -1;
    }

    /*
     * Client may have deleted this fd when handling a EPOLLHUP event (for another fd).
     * Mark it as ignored in current event list.
     */
    for (size_t i = evloop->ev_pos + 1; i < evloop->ev_count; ++i) {
        struct event_ctx* ctx = evloop->ev_inflight[i].data.ptr;
        if (ctx->fd == fd) {
            evloop->ev_inflight[i].events = 0;
        }
    }

    /* Lookup and remove actual entry */
    struct event_ctx* pctx = NULL;
    LIST_FOREACH(pctx, &evloop->ev_map, link) {
        if (pctx->fd == fd) {
            break;
        }
    }

    if (!pctx) {
        return -ENOENT;
    }

    LIST_REMOVE(pctx, link);
    vhost_free(pctx);

    return 0;
}

int evloop_run(struct event_loop* evloop)
{
    VHOST_VERIFY(evloop);
    int nfd;

again:
    nfd = epoll_wait(evloop->epollfd, evloop->ev_inflight, EV_MAX, -1);
    if (nfd < 0) {
        if (errno == EINTR) {
            goto again;
        }

        return -1;
    }

    evloop->ev_count = nfd;
    for (evloop->ev_pos = 0; evloop->ev_pos < evloop->ev_count; ++evloop->ev_pos) {
        /* Event may have been deleted - skip it */
        if (evloop->ev_inflight[evloop->ev_pos].events == 0) {
            continue;
        }

        struct event_ctx* pctx = evloop->ev_inflight[evloop->ev_pos].data.ptr;
        pctx->cb->handler(pctx->cb, pctx->fd, evloop->ev_inflight[evloop->ev_pos].events);
    }

    return 0;
}
