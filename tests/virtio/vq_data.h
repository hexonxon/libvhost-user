/**
 * Common virtqueue handling for unit tests
 */

#pragma once

#include <stdlib.h>
#include <stdint.h>

#include "virtio/virtqueue.h"
#include "virtio/memory.h"

/** Init an allocated virtqueue */
int vq_init(struct virtqueue* vq, uint16_t qsize, void* base, struct virtio_memory_map* mem);

/** Allocate memory to hold a queue of qsize descriptors and init a virtqueue on top of it */
void* vq_alloc(uint16_t qsize, struct virtio_memory_map* mem, struct virtqueue* vq);

/** Fill descriptor based on buffer description */
void vq_fill_desc(struct virtq_desc* desc, void* addr, size_t len, uint16_t flags, uint16_t next);

/** Fill descriptor with a given id */
struct virtq_desc* vq_fill_desc_id(
    struct virtqueue* vq,
    uint16_t id,
    void* addr,
    size_t len,
    uint16_t flags,
    uint16_t next);

/** Publish specified desc id in available ring */
void vq_publish_desc_id(struct virtqueue* vq, uint16_t id);
