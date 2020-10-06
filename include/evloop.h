#pragma once

#include <stdint.h>
#include <sys/epoll.h> /* Pull in epoll event types for client */

/**
 * General-purpose epoll-based event loop context
 */
struct event_loop;

/**
 * Client-registered event callback.
 * Multiple fds can be used with a common callback handler.
 */
struct event_cb
{
    /**
     * epoll event mask we are interested in,
     * currently only EPOLLIN and EPOLLHUP are supported
     */
    uint32_t events;

    /** private caller data */
    void* ptr;

    /** callback handler */
    void (*handler) (struct event_cb* cb, int fd, uint32_t events);
};

struct event_loop* evloop_create(void);
void evloop_free(struct event_loop* evloop);

int evloop_add_fd(struct event_loop* evloop, int fd, struct event_cb* cb);
int evloop_del_fd(struct event_loop* evloop, int fd);

int evloop_run(struct event_loop* evloop);
