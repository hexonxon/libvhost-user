#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <sys/queue.h>

#include "evloop.h"
#include "vhost-protocol.h"

#include "virtio/memory.h"
#include "virtio/virtqueue.h"

#define PAGE_SIZE 4096ull

struct vhost_user_message;
struct virtio_dev;

/**
 * Vring is a vhost name for a virtio virtqueue over shared guest memory
 * and its associated vhost context.
 */
struct vring
{
    /** Owning device */
    struct vhost_dev* dev;

    /** Event fd we wait on for available buffers */
    int kickfd;

    /** Event fd we use to signal used buffers */
    int callfd;

    /** Event fd we use to signal errors */
    int errfd;

    /** Size of the virtqueue (number of descriptors) */
    uint32_t size;

    /** Base index in the available ring */
    uint32_t avail_base;

    /** Addresses of the various parts of virtqueue */
    uint64_t avail_addr;
    uint64_t desc_addr;
    uint64_t used_addr;

    /** Vring is enabled: can pass data to/from the backend device */
    bool is_enabled;

    /** Vring is started: can service incoming buffers */
    bool is_started;

    /** Underlying virtqueue */
    struct virtqueue vq;

    /** Event handler for kickfd */
    struct event_cb kick_cb;
};

/**
 * Reset vring to default state
 */
void vring_reset(struct vring* vring);

/**
 * Start vring.
 * Started vrings can handle guest buffers.
 */
int vring_start(struct vring* vring);

/**
 * Stop vring.
 * Stopped vrings cannot handle guest buffers.
 */
void vring_stop(struct vring* vring);

/**
 * Send guest notification through vring's callfd
 */
void vring_notify(struct vring* vring);

/**
 * Client-provided virtqueue event handler callback.
 *
 * This is implemented by the client to be notified of vring events we see through vhost.
 * It is entirely up to client to talk to its virtio device when handling those events.
 *
 * Non-0 return value will be treated as an error and a reason to reset the device.
 */
typedef int (*vring_event_handler_cb) (struct virtio_dev* vdev, struct vring* vring);

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
    bool has_protocol_features;
    uint64_t negotiated_protocol_features;

    /** We have received VHOST_USER_SET_OWNER */
    bool session_started;

    /** Number of virt queues we support */
    uint8_t num_queues;

    /** Array of vrings for this device, num_queues total */
    struct vring* vrings;

    /** Mapped memory regions for this device */
    struct virtio_memory_map memory_map;

    /** Memory regions as received from guest */
    uint32_t num_regions;
    struct vhost_user_mem_region regions[VHOST_USER_MAX_FDS];

    /** Virtio device we are servicing */
    struct virtio_dev* vdev;

    /** Client handler for device vring events */
    vring_event_handler_cb vring_cb;

    LIST_ENTRY(vhost_dev) link;
};

/**
 * Register and expose vhost server to service virtio device requests
 *
 * @dev             Vhost device to initialize
 * @socket_path     Path of unix domain socket to create and listen on
 * @num_queues      Number of queues for new device
 * @vdev            Virtio device we will be servicing requests for
 */
int vhost_register_device_server(struct vhost_dev* dev,
                                 const char* socket_path,
                                 uint8_t num_queues,
                                 struct virtio_dev* vdev,
                                 vring_event_handler_cb vring_cb);

/**
 * Reset vhost device state and drop master connection if any
 */
void vhost_reset_dev(struct vhost_dev* dev);

/**
 * Run main vhost event loop
 */
int vhost_run();
