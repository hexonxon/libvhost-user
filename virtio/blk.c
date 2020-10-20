#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <errno.h>

#include "platform.h"
#include "virtio/virtqueue.h"
#include "virtio/blk.h"

#define VBLK_DEFAULT_FEATURES (\
    (1ull << VIRTIO_BLK_F_BLK_SIZE) | \
    0)

int virtio_blk_init(struct virtio_blk* vblk)
{
    if (!vblk) {
        return -EINVAL;
    }

    if (!vblk->block_size || (vblk->block_size & (VIRTIO_BLK_SECTOR_SIZE - 1))) {
        return -EINVAL;
    }

    if (!vblk->total_sectors) {
        return -EINVAL;
    }

    vblk->features = 0;
    vblk->supported_features = VBLK_DEFAULT_FEATURES;

    if (vblk->readonly) {
        vblk->supported_features |= VIRTIO_BLK_F_RO;
    }

    /*
     * 5.2.5.1 Driver Requirements: Device Initialization:
     * If neither VIRTIO_BLK_F_CONFIG_WCE nor VIRTIO_BLK_F_FLUSH are negotiated, the driver MAY de-
     * duce the presence of a writethrough cache. If VIRTIO_BLK_F_CONFIG_WCE was not negotiated but VIR-
     * TIO_BLK_F_FLUSH was, the driver SHOULD assume presence of a writeback cache.
     */
    if (vblk->writeback) {
        vblk->supported_features |= (1ull << VIRTIO_BLK_F_FLUSH);
    }

    return 0;
}

void virtio_blk_get_config(struct virtio_blk* vblk, struct virtio_blk_config* cfg)
{
    if (!vblk || !cfg) {
        return;
    }

    cfg->capacity = vblk->total_sectors;
    cfg->blk_size = vblk->block_size;
}

int virtio_blk_set_features(struct virtio_blk* vblk, uint64_t features)
{
    if (!vblk) {
        return -EINVAL;
    }

    if (features & ~vblk->supported_features) {
        return -EINVAL;
    }

    vblk->features = features;
    return 0;
}

/**
 * Virtio-blk private bio context
 */
struct virtio_blk_io
{
    struct virtqueue* vq;
    uint8_t* pstatus;
    uint16_t head;
    struct blk_io_request bio;
};

#define VBLK_IO_FROM_BIO(_bio) container_of((_bio), struct virtio_blk_io, bio)

static inline size_t vblk_io_size(uint32_t maxvecs)
{
    return sizeof(struct virtio_blk_io) + sizeof(struct virtio_iovec) * maxvecs;
}

static void complete_blk_request(struct virtio_blk* vblk, struct virtio_blk_io* vblk_io, enum blk_io_status res)
{
    *vblk_io->pstatus = res;
    virtqueue_enqueue_used(vblk_io->vq, vblk_io->head, 0);
    free(vblk_io);
}

static struct virtio_blk_io* blk_rw(struct virtio_blk* vblk,
                                    const struct virtio_blk_req* hdr,
                                    struct virtqueue_buffer_iter* iter)
{
    bool is_read = (hdr->type == VIRTIO_BLK_T_IN);
    uint64_t sector = hdr->sector;
    uint32_t total_sectors = 0;
    uint8_t* pstatus = NULL;

    if (sector >= vblk->total_sectors) {
        return NULL;
    }

    /*
     * Walk descriptor chain expecting a series of data buffers (at least 1)
     * terminated by 1-byte writable status buffer.
     */

    uint32_t nvecs = 0;
    uint32_t maxvecs = 16;
    struct virtio_blk_io* vblk_io = realloc(NULL, vblk_io_size(maxvecs));
    if (!vblk_io) {
        return NULL;
    }

    struct virtqueue_buffer buf;
    while (virtqueue_next_buffer(iter, &buf)) {
        if (!virtqueue_has_next_buffer(iter)) {
            /* The last one is a status descriptor */
            if (buf.len != sizeof(u8) || buf.ro) {
                goto error_out;
            }

            pstatus = buf.ptr;
            break;
        }

        if (!buf.len || (buf.len & (VIRTIO_BLK_SECTOR_SIZE - 1))) {
            goto error_out;
        }

        if (!is_read && buf.ro) {
            goto error_out;
        }

        total_sectors += buf.len >> VIRTIO_BLK_SECTOR_SHIFT;
        if (sector + total_sectors > vblk->total_sectors) {
            goto error_out;
        }

        vblk_io->bio.vecs[nvecs].ptr = buf.ptr;
        vblk_io->bio.vecs[nvecs].len = buf.len;
        nvecs++;

        if (nvecs == maxvecs) {
            maxvecs += 16;
            vblk_io = realloc(vblk_io, vblk_io_size(maxvecs));
            if (!vblk_io) {
                goto error_out;
            }
        }
    }

    /**
     * If we're missing data buffers or status buffer or both - just fail the request
     */
    if (!total_sectors || !pstatus) {
        return NULL;
    }

    vblk_io->vq = iter->vq;
    vblk_io->pstatus = pstatus;
    vblk_io->head = iter->head; /* TODO: be less intrusive here */
    vblk_io->bio.type = (is_read ? BLK_IO_READ : BLK_IO_WRITE);
    vblk_io->bio.sector = sector;
    vblk_io->bio.total_sectors = total_sectors;
    vblk_io->bio.nvecs = nvecs;

    return vblk_io;

error_out:
    free(vblk_io);
    return NULL;
}

static struct virtio_blk_io* handle_blk_request(struct virtio_blk* vblk, struct virtqueue_buffer_iter* iter)
{
    struct virtio_blk_io* vblk_io = NULL;
    struct virtqueue_buffer buf;

    /* First buffer describes request header */
    if (!virtqueue_next_buffer(iter, &buf)) {
        goto drop_request;
    }

    struct virtio_blk_req hdr;
    if (buf.len != sizeof(hdr)) {
        goto drop_request;
    }

    /* Copy request header to avoid TACTOU problems */
    memcpy(&hdr, buf.ptr, sizeof(hdr));

    switch (hdr.type) {
    case VIRTIO_BLK_T_IN:
    case VIRTIO_BLK_T_OUT:
        vblk_io = blk_rw(vblk, &hdr, iter);
        break;
    case VIRTIO_BLK_T_FLUSH:
        break;
    default:
        goto drop_request;
    };

    return vblk_io;

drop_request:
    /*
     * Request data is malformed, we shouldn't try to report status to the driver.
     * Our strategy now is to commit the buffers silently.
     * TODO: We need to look into errfd reporting in the future.
     */
    virtqueue_release_buffers(iter, 0);
    return NULL;
}

int virtio_blk_dequeue_request(struct virtio_blk* vblk, struct virtqueue* vq, struct blk_io_request** bio)
{
    if (!vblk || !vq || !bio) {
        return -EINVAL;
    }

    if (virtqueue_is_broken(vq)) {
        return -ENXIO;
    }

    struct virtqueue_buffer_iter iter;
    if (!virtqueue_dequeue_avail(vq, &iter)) {
        return -ENOENT;
    }

    struct virtio_blk_io* vblk_io = handle_blk_request(vblk, &iter);
    if (!vblk_io) {
        return -EIO;
    }

    *bio = &vblk_io->bio;
    return 0;
}

void virtio_blk_complete_request(struct virtio_blk* vblk, struct blk_io_request* bio, enum blk_io_status res)
{
    if (!vblk || !bio) {
        return;
    }

    complete_blk_request(vblk, VBLK_IO_FROM_BIO(bio), res);
}
