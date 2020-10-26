#pragma once

#include <stdint.h>
#include <stdbool.h>

#include "virtio/virtio10.h"
#include "virtio/vdev.h"

struct virtqueue;

/**
 * Simple io buffer
 */
struct virtio_iovec
{
    void* ptr;
    size_t len;
};

/**
 * Supported block request types
 */
enum blk_io_type
{
    BLK_IO_READ = VIRTIO_BLK_T_IN,
    BLK_IO_WRITE = VIRTIO_BLK_T_OUT,
    BLK_IO_FLUSH = VIRTIO_BLK_T_FLUSH,
    BLK_IO_GET_ID = VIRTIO_BLK_T_GET_ID,
};

/**
 * Possible block request result status from the backend
 */
enum blk_io_status
{
    BLK_SUCCESS = VIRTIO_BLK_S_OK,
    BLK_IOERROR = VIRTIO_BLK_S_IOERR,
};

/**
 * In-flight block request
 */
struct blk_io_request
{
    /** Request type */
    enum blk_io_type type;

    /**
     * Fields below are only valid if request is not BLK_IO_FLUSH
     */

    /** Request start sector */
    uint64_t sector;

    /** Total sectors in request */
    uint32_t total_sectors;

    /** Size of the scatter-gather list below */
    uint32_t nvecs;

    /** iovec scatter-gather list with request data */
    struct virtio_iovec vecs[/* nvecs */];
};

/**
 * Virtio-blk emulated device model
 */
struct virtio_blk
{
    /** Generic vdev context */
    struct virtio_dev vdev;

    /*
     * Fields below are set by the client code before calling virtio_blk_init
     * to describe the device layout and options
     */
    
    /** Device capacity in 512-byte sectors */
    uint64_t total_sectors;

    /** Backend optimal block size, must be a multiple of 512 */
    uint32_t block_size;

    /** Device is read-only */
    bool readonly;

    /** Underlying storage supports caching, device needs to expose writeback flush to driver */
    bool writeback;
};

/**
 * Initialize virtio blk device state
 */
int virtio_blk_init(struct virtio_blk* vblk);

/**
 * Dequeue next request from the device's virtqueue.
 * Called by the block backend implementation once we get a guest kick.
 * Takes care of (safely) handling guest-facing request memory and initializes req for the backend.
 */
int virtio_blk_dequeue_request(struct virtio_blk* vblk, struct virtqueue* vq, struct blk_io_request** bio);

/**
 * Complete block request with status
 */
void virtio_blk_complete_request(struct virtio_blk* vblk, struct blk_io_request* bio, enum blk_io_status res);
