#pragma once

#include <stdint.h>

#define VIRTQ_MAX_SIZE          32768
#define VIRTQ_INVALID_DESC_ID   VIRTQ_MAX_SIZE

#define VIRTQ_DESC_ALIGNMENT    16
#define VIRTQ_AVAIL_ALIGNMENT   2
#define VIRTQ_USED_ALIGNMENT    4

#define VIRTQ_ALIGN_MASK                    (4096ull - 1)
#define VIRTQ_ALIGN_UP(_val)                (((_val) + VIRTQ_ALIGN_MASK) & ~VIRTQ_ALIGN_MASK)
#define VIRTQ_ALIGN_UP_PTR(_ptr)            (void*)VIRTQ_ALIGN_UP((uintptr_t)(_ptr))
#define VIRTQ_IS_ALIGNED(_val, _align)      (((_val) & ((__typeof(_val))(_align) - 1)) == 0)
#define VIRTQ_IS_ALIGNED_PTR(_ptr, _align)  VIRTQ_IS_ALIGNED((uintptr_t)(_ptr), _align)

typedef uint8_t  u8;
typedef uint16_t le16;
typedef uint32_t le32;
typedef uint64_t le64;

struct virtq_desc {
    /* Address (guest-physical). */
    le64 addr;
    /* Length. */
    le32 len;

    /* This marks a buffer as continuing via the next field. */
#define VIRTQ_DESC_F_NEXT       1
    /* This marks a buffer as device write-only (otherwise device read-only). */
#define VIRTQ_DESC_F_WRITE      2
    /* This means the buffer contains a list of buffer descriptors. */
#define VIRTQ_DESC_F_INDIRECT   4
    /* The flags as indicated above. */
    le16 flags;
    /* Next field if flags & NEXT */
    le16 next;
};

struct virtq_avail {
#define VIRTQ_AVAIL_F_NO_INTERRUPT      1
    le16 flags;
    le16 idx;
    le16 ring[ /* Queue Size */ ];
};

/* le32 is used here for ids for padding reasons. */
struct virtq_used_elem {
    /* Index of start of used descriptor chain. */
    le32 id;
    /* Total length of the descriptor chain which was used (written to) */
    le32 len;
};

struct virtq_used {
#define VIRTQ_USED_F_NO_NOTIFY  1
    le16 flags;
    le16 idx;
    struct virtq_used_elem ring[ /* Queue Size */];
};

/*
 * Overall virtq structure below is not represantable as a C struct
 *
struct virtq {
    // The actual descriptors (16 bytes each)
    struct virtq_desc desc[ Queue Size ];

    // A ring of available descriptor heads with free-running index.
    struct virtq_avail avail;

    // Only if VIRTIO_F_EVENT_IDX
    le16 used_event;

    // Padding to the next Queue Align boundary.
    u8 pad[ Padding ];

    // A ring of used descriptor heads with free-running index.
    struct virtq_used used;

    // Only if VIRTIO_F_EVENT_IDX
    le16 avail_event;
};
*/

static inline size_t virtq_size(uint16_t qsz)
{
    return VIRTQ_ALIGN_UP(sizeof(struct virtq_desc) * qsz + sizeof(uint16_t) * (3 + qsz))  +
           VIRTQ_ALIGN_UP(sizeof(uint16_t) * 3 + sizeof(struct virtq_used_elem) * qsz);
}

/*
 * virtio-blk specifics
 */

#define VIRTIO_BLK_DEVICE_ID    2
#define VIRTIO_BLK_SECTOR_SHIFT 9
#define VIRTIO_BLK_SECTOR_SIZE  (1ul << VIRTIO_BLK_SECTOR_SHIFT)

/**
 * Feature bits
 */
#define VIRTIO_BLK_F_BARRIER    0
#define VIRTIO_BLK_F_SIZE_MAX   1
#define VIRTIO_BLK_F_SEG_MAX    2
#define VIRTIO_BLK_F_GEOMETRY   4
#define VIRTIO_BLK_F_RO         5
#define VIRTIO_BLK_F_BLK_SIZE   6
#define VIRTIO_BLK_F_SCSI       7
#define VIRTIO_BLK_F_FLUSH      9
#define VIRTIO_BLK_F_TOPOLOGY   10
#define VIRTIO_BLK_F_CONFIG_WCE 11

/**
 * Device configuration layout
 */
struct virtio_blk_config {
    le64 capacity;
    le32 size_max;
    le32 seg_max;
    struct virtio_blk_geometry {
        le16 cylinders;
        u8 heads;
        u8 sectors;
    } geometry;
    le32 blk_size;
    struct virtio_blk_topology {
        // # of logical blocks per physical block (log2)
        u8 physical_block_exp;
        // offset of first aligned logical block
        u8 alignment_offset;
        // suggested minimum I/O size in blocks
        le16 min_io_size;
        // optimal (suggested maximum) I/O size in blocks
        le32 opt_io_size;
    } topology;
    u8 writeback;
};

/**
 * Request types
 */
#define VIRTIO_BLK_T_IN     0
#define VIRTIO_BLK_T_OUT    1
#define VIRTIO_BLK_T_FLUSH  4
#define VIRTIO_BLK_T_GET_ID 8

struct virtio_blk_req {
    le32 type;
    le32 reserved;
    le64 sector;
};

/**
 * Request status codes
 */
#define VIRTIO_BLK_S_OK     0
#define VIRTIO_BLK_S_IOERR  1
#define VIRTIO_BLK_S_UNSUPP 2

struct virtio_blk_req_status {
    u8 status;
};

#define VIRTIO_BLK_ID_BYTES 20  /* ID string length */
