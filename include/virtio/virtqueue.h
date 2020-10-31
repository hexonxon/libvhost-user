#pragma once

#include <stdint.h>
#include <stdbool.h>

#include "virtio/virtio10.h"

/**
 * Buffer described by a virtq descriptor and mapped to host address space.
 *
 * Working with this buffer is safer than accessing raw virtio descriptors.
 * Queue implementation makes sure to sanitize descriptor data before mapping the buffer.
 */
struct virtqueue_buffer
{
    /** Mapped host address */
    void* ptr;

    /** Size of mapped buffer in bytes */
    size_t len;

    /** Read only flag */
    bool ro;
};

/**
 * Virtqueue descriptor chain iterator.
 * Allows device types to traverse a single descriptor chain in a safe manner.
 */
struct virtqueue_buffer_iter
{
    /** Containing virtqueue (to mark broken if we don't like something) */
    struct virtqueue* vq;

    /** Original descriptor chain head id within the virtqueue descriptor table */
    uint16_t head;

    /** Current descriptor id within the table (can be indirect) */
    uint16_t cur;

    /** Current desciptor table to resolve next pointers */
    struct virtq_desc* ptbl;

    /** Number of descriptors in current table */
    uint16_t tbl_size;

    /** Current descriptor table is an indirect one */
    bool is_indirect;

    /** Total number of descriptors we've seen, to detect loops */
    uint32_t nseen;
};

/**
 * Get next buffer from descriptor chain.
 *
 * Will return false once there are no more buffers available or if we detected bad input.
 * Returned buffers are sanitized for access on behalf of the guest that owns the queue.
 */
bool virtqueue_next_buffer(struct virtqueue_buffer_iter* iter, struct virtqueue_buffer* buf);

/**
 * Tell if next call to virtqueue_next_buffer will return false
 */
bool virtqueue_has_next_buffer(struct virtqueue_buffer_iter* iter);

/**
 * Release the chain buffers by moving head id to used ring.
 *
 * @nwritten    Optional total number of bytes written by the device when handling the chain.
 *              This is an optional hint to provide to the guest driver so that only this much data
 *              is zeroed-out when reusing the buffer.
 */
void virtqueue_release_buffers(struct virtqueue_buffer_iter* iter, uint32_t nwritten);

/**
 * Virtqueue tracking struct
 */
struct virtqueue
{
    /** Mapped guest memory available for this virtqueue */
    struct virtio_memory_map* mem;

    /**
     * These point directly to virtq memory
     */
    struct virtq_desc* desc;
    struct virtq_avail* avail; 
    struct virtq_used* used; 

    /** Size of the queue in descriptor count */
    uint16_t qsize;

    /** Shadow copy of an avail->idx value we've last seen */
    uint16_t last_seen_avail;

    /** queue is broken by the guest and cannot be safely handled further */
    bool is_broken;

    /** Eventfd to sent driver notifications when must */
    int callfd;
};

/**
 * Start virtqueue with given arguments
 */
int virtqueue_start(struct virtqueue* vq,
                    uint16_t qsize,
                    uint64_t desc_addr,
                    uint64_t avail_addr,
                    uint64_t used_addr,
                    uint16_t avail_base,
                    int callfd,
                    struct virtio_memory_map* mem);

/**
 * Dequeue next buffer chain from the queue.
 *
 * Returns false if there are no new buffer chains available.
 * Also can mark the virtqueue broken if we encountered a bad chain.
 */
bool virtqueue_dequeue_avail(struct virtqueue* vq, struct virtqueue_buffer_iter* out_iter);

/**
 * Enqueue descriptor chain head into used ring
 *
 * @desc_id     Id of a head decriptor that starts a buffer chain.
 * @nwritten    Optional total number of bytes written by the device when handling the chain.
 *              This is an optional hint to provide to the guest driver so that only this much data
 *              is zeroed-out when reusing the buffer.
 */
void virtqueue_enqueue_used(struct virtqueue* vq, uint16_t desc_id, uint32_t nwritten);

/**
 * Tell if virtqueue is broken by invalid guest data.
 * Broken virtqueue cannot be used until completely reinitialized.
 */
bool virtqueue_is_broken(struct virtqueue* vq);
