#pragma once

#include <stdint.h>

#define VHOST_USER_MAX_FDS 8

/**
 * Feature bits
 */
#define VHOST_USER_F_PROTOCOL_FEATURES  30

/**
 * Protocol feature bits
 */
#define VHOST_USER_PROTOCOL_F_MQ                    0
#define VHOST_USER_PROTOCOL_F_LOG_SHMFD             1
#define VHOST_USER_PROTOCOL_F_RARP                  2
#define VHOST_USER_PROTOCOL_F_REPLY_ACK             3
#define VHOST_USER_PROTOCOL_F_MTU                   4
#define VHOST_USER_PROTOCOL_F_SLAVE_REQ             5
#define VHOST_USER_PROTOCOL_F_CROSS_ENDIAN          6
#define VHOST_USER_PROTOCOL_F_CRYPTO_SESSION        7
#define VHOST_USER_PROTOCOL_F_PAGEFAULT             8
#define VHOST_USER_PROTOCOL_F_CONFIG                9
#define VHOST_USER_PROTOCOL_F_SLAVE_SEND_FD        10
#define VHOST_USER_PROTOCOL_F_HOST_NOTIFIER        11
#define VHOST_USER_PROTOCOL_F_INFLIGHT_SHMFD       12
#define VHOST_USER_PROTOCOL_F_RESET_DEVICE         13
#define VHOST_USER_PROTOCOL_F_INBAND_NOTIFICATIONS 14
#define VHOST_USER_PROTOCOL_F_CONFIGURE_MEM_SLOTS  15
#define VHOST_USER_PROTOCOL_F_STATUS               16

/**
 * Master message ids
 */
#define VHOST_USER_GET_FEATURES             1
#define VHOST_USER_SET_FEATURES             2
#define VHOST_USER_SET_OWNER                3
#define VHOST_USER_RESET_OWNER              4
#define VHOST_USER_SET_MEM_TABLE            5
#define VHOST_USER_SET_LOG_BASE             6
#define VHOST_USER_SET_LOG_FD               7
#define VHOST_USER_SET_VRING_NUM            8
#define VHOST_USER_SET_VRING_ADDR           9
#define VHOST_USER_SET_VRING_BASE           10
#define VHOST_USER_GET_VRING_BASE           11
#define VHOST_USER_SET_VRING_KICK           12
#define VHOST_USER_SET_VRING_CALL           13
#define VHOST_USER_SET_VRING_ERR            14
#define VHOST_USER_GET_PROTOCOL_FEATURES    15
#define VHOST_USER_SET_PROTOCOL_FEATURES    16
#define VHOST_USER_GET_QUEUE_NUM            17
#define VHOST_USER_SET_VRING_ENABLE         18
#define VHOST_USER_SEND_RARP                19
#define VHOST_USER_NET_SET_MTU              20
#define VHOST_USER_SET_SLAVE_REQ_FD         21
#define VHOST_USER_IOTLB_MSG                22
#define VHOST_USER_SET_VRING_ENDIAN         23
#define VHOST_USER_GET_CONFIG               24
#define VHOST_USER_SET_CONFIG               25
#define VHOST_USER_CREATE_CRYPTO_SESSION    26
#define VHOST_USER_CLOSE_CRYPTO_SESSION     27
#define VHOST_USER_POSTCOPY_ADVISE          28
#define VHOST_USER_POSTCOPY_LISTEN          29
#define VHOST_USER_POSTCOPY_END             30
#define VHOST_USER_GET_INFLIGHT_FD          31
#define VHOST_USER_SET_INFLIGHT_FD          32
#define VHOST_USER_GPU_SET_SOCKET           33
#define VHOST_USER_RESET_DEVICE             34
#define VHOST_USER_VRING_KICK               35
#define VHOST_USER_GET_MAX_MEM_SLOTS        36
#define VHOST_USER_ADD_MEM_REG              37
#define VHOST_USER_REM_MEM_REG              38
#define VHOST_USER_SET_STATUS               39
#define VHOST_USER_GET_STATUS               40

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

struct vhost_user_message_header
{
    uint32_t request;
    uint32_t flags;

    /** Size of the contained payload excluding the header */
    uint32_t size;
};

/**
 * Vhost user message structure as described by the documentation
 */
struct vhost_user_message
{
    struct vhost_user_message_header hdr;

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
