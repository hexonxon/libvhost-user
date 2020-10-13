#include "platform.h"
#include "virtio/virtqueue.h"

int virtqueue_init(struct virtqueue* vq, void* base, uint16_t qsize)
{
    VHOST_VERIFY(vq);
    VHOST_VERIFY(base);

    /*
     * 2.4 Virtqueues: "Queue size is always a power of 2"
     * Note: we know that qsize is not 0 at this point.
     */

    if (!qsize || qsize & (qsize - 1)) {
        return -EINVAL;
    }

    if (qsize > VIRTQ_MAX_SIZE) {
        return -EINVAL;
    }

    /*
     * Refer to "2.4 Virtqueues" in virtio 1.0 spec for reasoning
     * behind alignment and size calculations below
     */

    struct virtq_desc* pdesc = (struct virtq_desc*) base;
    if (!VIRTQ_IS_ALIGNED_PTR(pdesc, 16)) {
        return -EINVAL;
    }

    base += 16 * qsize;
    struct virtq_avail* pavail = (struct virtq_avail*) base;
    if (!VIRTQ_IS_ALIGNED_PTR(pavail, 2)) {
        return -EINVAL;
    }

    /* Align up to qalign to skip the padding */
    base += (6 + 2 * qsize);
    base = VIRTQ_ALIGN_UP_PTR(base);
    struct virtq_used* pused = (struct virtq_used*) base;
    if (!VIRTQ_IS_ALIGNED_PTR(pused, 4)) {
        return -EINVAL;
    }

    vq->desc = pdesc;
    vq->avail = pavail;
    vq->used = pused;
    vq->qsize = qsize;
    vq->last_seen_avail = 0;
    vq->is_broken = false;

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

static void start_desc_chain(struct virtqueue_desc_chain* iter, struct virtqueue* vq, uint16_t head)
{
    iter->vq = vq;
    iter->head = head;
    iter->cur = head;
    iter->ptbl = vq->desc;
    iter->tbl_size = vq->qsize;
    iter->is_indirect = false;
    iter->nseen = 0;
}

bool virtqueue_next_buffer(struct virtqueue_desc_chain* iter, struct virtqueue_buffer* buf)
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

        /* Continue the chain inside indirect decriptor table */
        iter->is_indirect = true;
        iter->ptbl = host_address(pcur->addr);
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

    /* On x86 things cannot be write-only, so we have to ignore the exact virtio definition here */
    buf->ro = ((pcur->flags & VIRTQ_DESC_F_WRITE) == 0);
    buf->ptr = host_address(pcur->addr);
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

void virtqueue_release_buffers(struct virtqueue_desc_chain* iter, uint32_t bytes_written)
{
    uint16_t used_idx = read_used_idx(iter->vq);
    iter->vq->used[get_index(iter->vq, used_idx)] = (struct virtq_used) { iter->head, bytes_written };

    write_used_idx(iter->vq, used_idx + 1);
}

bool virtqueue_dequeue(struct virtqueue* vq, struct virtqueue_desc_chain* chain)
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
