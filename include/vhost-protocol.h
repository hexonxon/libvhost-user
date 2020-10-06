#pragma once

#include <stdint.h>

#define VHOST_USER_MAX_FDS 8

/**
 * Vhost user memory region description
 */
struct vhost_user_mem_region
{
    /** guest address of the region */
    uint64_t guest_addr;

    /** size */
    uint64_t size;

    /** user address */
    uint64_t user_addr;

    /** offset where region starts in the mapped memory */
    uint64_t mmap_offset;
};

/**
 * Vhost user message structure as described by the documentation
 */
struct vhost_user_message
{
    uint32_t request;
    uint32_t flags;

    /** Size of the contained payload excluding the header */
    uint32_t size;

    union {

        /** A single 64-bit integer */
        uint64_t u64;

        /** A vring state description */
        struct {
            uint32_t index;
            uint32_t num;
        } vring_state;

        /** A vring address description */
        struct {
            /** a 32-bit vring index */
            uint32_t index;

            /** a 32-bit vring flags */
            uint32_t flags;

            uint64_t size;

            /** a 64-bit ring address of the vring descriptor table */
            uint64_t descriptor;

            /** a 64-bit ring address of the vring used ring */
            uint64_t used;

            /** a 64-bit ring address of the vring available ring */
            uint64_t available;

            /** a 64-bit guest address for logging */
            uint64_t log;
        } vring_address;

        /** Memory regions description */
        struct {
            uint32_t num_regions;
            uint32_t padding;
            struct vhost_user_mem_region regions[8];
        } mem_regions;

        /** Virtio device config space */
        struct {
            /** offset of virtio device's configuration space */
            uint32_t offset;

            /** configuration space access size in bytes */
            uint32_t size;

            /**
              - 0: Vhost master messages used for writeable fields
              - 1: Vhost master messages used for live migration
            */
            uint32_t flags;

            /** Size bytes array holding the contents of the virtio device's configuration space */
            uint8_t payload[];
        } device_config_space;

        /** Vring area description */
        struct {
            /** vring index and flags */
            uint64_t u64;

            /** size of this area */
            uint64_t size;

            /** offset of this area from the start of the supplied file descriptor */
            uint64_t offset;
        } vring_area;
    };
};
