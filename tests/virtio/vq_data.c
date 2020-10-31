#include <CUnit/Basic.h>
#include <CUnit/CUnit.h>

#include "vq_data.h"

int vq_init(struct virtqueue* vq, uint16_t qsize, void* base, struct virtio_memory_map* mem)
{
    uint64_t desc_addr = (uint64_t) base;
    uint64_t avail_addr = desc_addr + sizeof(struct virtq_desc) * qsize;
    uint64_t used_addr = VIRTQ_ALIGN_UP(avail_addr + sizeof(uint16_t) * (3 + qsize));

    return virtqueue_start(vq, qsize, desc_addr, avail_addr, used_addr, 0, -1, mem);
}

/* Allocate memory to hold a queue of qsize descriptors and init a virtqueue on top of it */
void* vq_alloc(uint16_t qsize, struct virtio_memory_map* mem, struct virtqueue* vq)
{
    size_t size_bytes = virtq_size(qsize);
    void* base = aligned_alloc(4096, size_bytes);
    CU_ASSERT(base != NULL);
    memset(base, 0, size_bytes);

    int res = vq_init(vq, qsize, base, mem);
    CU_ASSERT(res == 0);
    return base;
}

/* Fill descriptor based on buffer description */
void vq_fill_desc(struct virtq_desc* desc, void* addr, size_t len, uint16_t flags, uint16_t next)
{
    desc->addr = (uintptr_t) addr;
    desc->len = (uint32_t) len;
    desc->flags = flags;
    desc->next = next;
}

void vq_validate_desc(const struct virtq_desc* desc, const struct virtqueue_buffer* buf)
{
    CU_ASSERT_EQUAL(buf->ptr, (void*) desc->addr);
    CU_ASSERT_EQUAL(buf->len, desc->len);
    CU_ASSERT_EQUAL(buf->ro, (desc->flags & VIRTQ_DESC_F_WRITE) == 0);
}

/* Fill descriptor with a given id */
struct virtq_desc* vq_fill_desc_id(
    struct virtqueue* vq,
    uint16_t id,
    void* addr,
    size_t len,
    uint16_t flags,
    uint16_t next)
{
    vq_fill_desc(&vq->desc[id], addr, len, flags, next);
    return &vq->desc[id];
}

void vq_publish_desc_id(struct virtqueue* vq, uint16_t id)
{
    vq->avail->ring[vq->avail->idx] = id;
    vq->avail->idx++;
}
