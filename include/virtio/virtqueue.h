#pragma once

#include <stdint.h>
#include <stdbool.h>

#include "virtio/virtio10.h"

extern void* host_address(uint64_t gpa);

/**
 * Buffer described by a virtq descriptor and mapped to host address space.
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
 * Allows device types to traverse a single descriptor chain.
 */
struct virtqueue_desc_chain
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
 * Will return false once there are no more buffers available.
 */
bool virtqueue_next_buffer(struct virtqueue_desc_chain* iter, struct virtqueue_buffer* buf);

/**
 * Release the chain buffers by moving them into used ring.
 */
void virtqueue_release_buffers(struct virtqueue_desc_chain* iter, uint32_t bytes_written);

/**
 * Virtqueue tracking struct
 */
struct virtqueue
{
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
};

/**
 * Init virtqueue from mapped memory
 */
int virtqueue_init(struct virtqueue* vq, void* base, uint16_t size);

/**
 * Dequeue next buffer chain from the queue.
 *
 * Returns false if there are no new buffer chains available.
 * Also can mark the virtqueue broken if we encountered a bad chain.
 */
bool virtqueue_dequeue(struct virtqueue* vq, struct virtqueue_desc_chain* chain);

/**
 * Tell if virtqueue is broken by invalid guest data.
 * Broken virtqueue cannot be used until completely reinitialized.
 */
bool virtqueue_is_broken(struct virtqueue* vq);
