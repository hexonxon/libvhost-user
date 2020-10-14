#pragma once

#include <stdint.h>
#include <stdbool.h>

#ifndef MAP_FAILED
#   define MAP_FAILED ((void*)-1)
#endif

/**
 * Virtqueue memory table describes mapped guest-physical regions
 * that we can touch when traversing the queue.
 * Client is responsible to fill this out for us.
 */
struct virtio_memory_map
{
    enum {
        VIRTIO_MEMORY_MAX_REGIONS = 16,
    };

    /** Number of memory regions in the table */
    uint32_t num_regions;

    /** Variable-sized array of mapped guest regions, not intersecting, sorted by gpa */
    struct virtio_memory_region {
        /** guest-physical base address */
        uint64_t gpa;

        /** Region length in bytes */
        uint64_t len;

        /** mapped host virtual address */
        void* hva;

        /** Region is read-only */
        bool ro;
    } regions[VIRTIO_MEMORY_MAX_REGIONS];
};

/**
 * Initializer for empty memory map
 */
#define VIRTIO_INIT_MEMORY_MAP ((struct virtio_memory_map) {.num_regions = 0, .regions = {0}})

/**
 * Insert a new region into the map
 */
int virtio_add_guest_region(struct virtio_memory_map* mem, uint64_t gpa, uint64_t len, void* hva, bool ro);

/**
 * Find mapped host address that covers specified guest gpa range.
 * Returns MAP_FAILED if regions is invalid.
 */
void* virtio_find_gpa_range(const struct virtio_memory_map* mem, uint64_t gpa, uint32_t len, bool ro);
