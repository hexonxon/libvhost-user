/**
 * Generic virtio device
 */

#pragma once

#include <stdlib.h>
#include <stdint.h>
#include <errno.h>

#define VIRTIO_DEV_CONFIG_SPACE_SIZE 256

/**
 * This is a generic virtio device, it contains data common for all virtio device types.
 */
struct virtio_dev
{
    /** Features this device can advertise to the driver */
    uint64_t supported_features;

    /** Features this device negotiated with the driver */
    uint64_t features;

    /** Size of the device config structure */
    uint32_t config_size;

    /**
     * Device-specific handler to fill config space buffer.
     *
     * @buffer      Caller buffer to put config into.
     *              Buffer size guaranteed to be >= config_size.
     */
    void (*get_config) (struct virtio_dev* vdev, void* buffer);
};

static inline int virtio_dev_get_config(struct virtio_dev* vdev, void* buffer, uint32_t bufsize)
{
    if (!vdev || !buffer) {
        return -EINVAL;
    }

    if (bufsize < vdev->config_size) {
        return -ENOSPC;
    }

    vdev->get_config(vdev, buffer);
    return 0;
}

static inline int virtio_dev_set_features(struct virtio_dev* vdev, uint64_t features)
{
    if (!vdev) {
        return -EINVAL;
    }

    vdev->features = features;
    return 0;
}
