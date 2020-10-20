#include "platform.h"

#include "virtio/memory.h"
#include "virtio/virtqueue.h"

int virtqueue_start(struct virtqueue* vq,
                    uint16_t qsize,
                    uint64_t desc_gpa,
                    uint64_t avail_gpa,
                    uint64_t used_gpa,
                    uint16_t avail_base,
                    struct virtio_memory_map* mem)
{
    if (!vq) {
        return -EINVAL;
    }

    if (!mem) {
        return -EINVAL;
    }

    /*
     * 2.4 Virtqueues: "Queue size is always a power of 2"
     */

    if (!qsize || qsize & (qsize - 1)) {
        return -EINVAL;
    }

    if (qsize > VIRTQ_MAX_SIZE) {
        return -EINVAL;
    }

    uint32_t desc_size = sizeof(struct virtq_desc) * qsize;
    struct virtq_desc* pdesc = virtio_find_gpa_range(mem, desc_gpa, desc_size, false);
    if (pdesc == MAP_FAILED || !VIRTQ_IS_ALIGNED_PTR(pdesc, VIRTQ_DESC_ALIGNMENT)) {
        return -EINVAL;
    }

    uint32_t avail_size = sizeof(struct virtq_avail) + sizeof(uint16_t) * qsize + 2 /* for used_event */;
    struct virtq_avail* pavail = virtio_find_gpa_range(mem, avail_gpa, avail_size, false);
    if (pavail == MAP_FAILED || !VIRTQ_IS_ALIGNED_PTR(pavail, VIRTQ_AVAIL_ALIGNMENT)) {
        return -EINVAL;
    }

    uint32_t used_size = sizeof(struct virtq_used) + sizeof(struct virtq_used_elem) * qsize + 2 /* for avail_event */;
    struct virtq_used* pused = virtio_find_gpa_range(mem, used_gpa, used_size, false);
    if (pused == MAP_FAILED || !VIRTQ_IS_ALIGNED_PTR(pused, VIRTQ_USED_ALIGNMENT)) {
        return -EINVAL;
    }

    vq->desc = pdesc;
    vq->avail = pavail;
    vq->used = pused;
    vq->qsize = qsize;
    vq->last_seen_avail = avail_base;
    vq->is_broken = false;
    vq->mem = mem;

    return 0;
}

static inline void mark_broken(struct virtqueue* vq)
{
    vq->is_broken = true;
}

bool virtqueue_is_broken(struct virtqueue* vq)
{
    return vq->is_broken;
}

/* Find mapped address of a buffer described by desc within vq's memory map.
 * Not that on failure we return MAP_FAILED and not NULL */
static void* map_buffer(struct virtqueue* vq, const struct virtq_desc* desc)
{
    return virtio_find_gpa_range(vq->mem, desc->addr, desc->len, (desc->flags & VIRTQ_DESC_F_WRITE) == 0);
}

static inline uint16_t get_index(const struct virtqueue* vq, uint16_t idx)
{
    /*
     * Since qsize is always a power-of-two, we can do the bit-twiddling thing here
     */
    return idx & (vq->qsize - 1);
}

/*
 * Not on the memory barriers for avail/used indexes.
 *
 * Driver updates avail idx after storing buffer heads in avail ring. On x86 stores and not reordered
 * with other stores, thus we don't need a memory barrier, and if we did, that would be on the driver's side.
 * Same logic applies for device updating the used ring/idx.
 *
 * Device reads used idx and then writes to it. Again, on x86 loads and stored are not reordered when they
 * reference the same memory object. Thus, device doesn't need a memory barrier on x86 when accessing avail idx.
 *
 * TODO: this will probably change once we get to notification flags.
 */

static inline uint16_t read_avail_idx(const struct virtqueue* vq)
{
    return vq->avail->idx;
}

static inline uint16_t read_used_idx(const struct virtqueue* vq)
{
    return vq->used->idx;
}

static inline void write_used_idx(const struct virtqueue* vq, uint16_t idx)
{
    vq->used->idx = idx;
}

static void start_desc_chain(struct virtqueue_buffer_iter* iter, struct virtqueue* vq, uint16_t head)
{
    iter->vq = vq;
    iter->head = head;
    iter->cur = head;
    iter->ptbl = vq->desc;
    iter->tbl_size = vq->qsize;
    iter->is_indirect = false;
    iter->nseen = 0;
}

bool virtqueue_next_buffer(struct virtqueue_buffer_iter* iter, struct virtqueue_buffer* buf)
{
    VHOST_VERIFY(iter);
    VHOST_VERIFY(buf);

    if (virtqueue_is_broken(iter->vq)) {
        return false;
    }

    if (iter->cur == VIRTQ_INVALID_DESC_ID) {
        return false;
    }

    struct virtq_desc* pcur = &iter->ptbl[iter->cur];

    while (pcur->flags & VIRTQ_DESC_F_INDIRECT) {

        /*
         * 2.4.5.3.1 Driver Requirements: Indirect Descriptors:
         * The driver MUST NOT set the VIRTQ_DESC_F_INDIRECT flag within an indirect descriptor
         * (ie. only one table per descriptor).
         */

        if (iter->is_indirect) {
            goto mark_broken;
        }

        /*
         * 2.4.5.3.1 Driver Requirements: Indirect Descriptors:
         * A driver MUST NOT set both VIRTQ_DESC_F_INDIRECT and VIRTQ_DESC_F_NEXT in flags
         */

        if (pcur->flags & VIRTQ_DESC_F_NEXT) {
            goto mark_broken;
        }

        /*
         * 2.4.5.3 Indirect Descriptors:
         * The first indirect descriptor is located at start of the indirect descriptor table (index 0),
         * additional indirect descriptors are chained by next. An indirect descriptor without a valid next
         * (with flags&VIRTQ_DESC_F_NEXT off) signals the end of the descriptor.
         *
         * We read the above as "an indirect descriptor chain ends with a descriptor without F_NEXT set",
         * which is impossible if there are no descriptors at all. So we consider this a broken virtq.
         */

        if ((pcur->len / sizeof(*pcur)) == 0) {
            goto mark_broken;
        }

        void* hva = map_buffer(iter->vq, pcur);
        if (hva == MAP_FAILED) {
            goto mark_broken;
        }

        /* Continue the chain inside indirect decriptor table */
        iter->is_indirect = true;
        iter->ptbl = hva;
        iter->tbl_size = pcur->len / sizeof(*pcur);
        iter->cur = 0;
        iter->nseen++;

        pcur = &iter->ptbl[0];
    }

    /*
     * 2.4.5.3.1 Driver Requirements: Indirect Descriptors:
     * A driver MUST NOT create a descriptor chain longer than the Queue Size of the device.
     */

    iter->nseen++;
    if (iter->nseen > iter->vq->qsize) {
        /* Loop detected */
        goto mark_broken;
    }

    /*
     * Spec does not say anything about how we should treat 0-length descriptors.
     * We choose to break things immediately.
     */
    if (pcur->len == 0) {
        goto mark_broken;
    }

    void* hva = map_buffer(iter->vq, pcur);
    if (hva == MAP_FAILED) {
        goto mark_broken;
    }

    /* On x86 things cannot be write-only, so we have to ignore the exact virtio definition here */
    buf->ro = ((pcur->flags & VIRTQ_DESC_F_WRITE) == 0);
    buf->ptr = hva;
    buf->len = pcur->len;

    if (pcur->flags & VIRTQ_DESC_F_NEXT) {
        if (pcur->next >= iter->tbl_size) {
            goto mark_broken;
        }

        iter->cur = pcur->next;
    } else {
        iter->cur = VIRTQ_INVALID_DESC_ID;
    }

    return true;

mark_broken:
    mark_broken(iter->vq);
    iter->cur = VIRTQ_INVALID_DESC_ID;
    return false;
}

bool virtqueue_has_next_buffer(struct virtqueue_buffer_iter* iter)
{
    if (!iter) {
        return false;
    }

    return (iter->cur != VIRTQ_INVALID_DESC_ID);
}

void virtqueue_release_buffers(struct virtqueue_buffer_iter* iter, uint32_t nwritten)
{
    virtqueue_enqueue_used(iter->vq, iter->head, nwritten);
}

bool virtqueue_dequeue_avail(struct virtqueue* vq, struct virtqueue_buffer_iter* chain)
{
    if (virtqueue_is_broken(vq)) {
        return false;
    }

    if (vq->last_seen_avail != read_avail_idx(vq)) {
        uint16_t head = vq->avail->ring[get_index(vq, vq->last_seen_avail)];
        start_desc_chain(chain, vq, head);

        vq->last_seen_avail++;
        return true;
    }

    return false;
}

void virtqueue_enqueue_used(struct virtqueue* vq, uint16_t desc_id, uint32_t nwritten)
{
    uint16_t used_idx = read_used_idx(vq);
    vq->used[get_index(vq, used_idx)] = (struct virtq_used) { desc_id, nwritten };
    write_used_idx(vq, used_idx + 1);
}
