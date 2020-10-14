/**
 * libvirtqueue unit tests
 */

#include <stdlib.h>
#include <stdint.h>

#include <CUnit/Basic.h>
#include <CUnit/CUnit.h>

#include "virtio/virtqueue.h"
#include "virtio/memory.h"

/* Default memory map just allows the entire address space for rw access.
 * Specific tests might use something more restictive. */
static struct virtio_memory_map g_default_memory_map = {
    .num_regions = 1,
    .regions = {
        { 0, UINTPTR_MAX, NULL, false },
    },
};

/* Allocate memory to hold a queue of qsize descriptors and init a virtqueue on top of it */
static void* vq_alloc(uint16_t qsize, struct virtio_memory_map* mem, struct virtqueue* vq)
{
    size_t size_bytes = virtq_size(qsize);

    void* queue_mem = aligned_alloc(4096, size_bytes);
    CU_ASSERT(queue_mem != NULL);
    memset(queue_mem, 0, size_bytes);

    int res = virtqueue_init(vq, queue_mem, qsize, mem);
    CU_ASSERT(res == 0);

    return queue_mem;
}

/* Fill descriptor based on buffer description */
static void vq_fill_desc(struct virtq_desc* desc, void* addr, size_t len, uint16_t flags, uint16_t next)
{
    desc->addr = (uintptr_t) addr;
    desc->len = (uint32_t) len;
    desc->flags = flags;
    desc->next = next;
}

static void vq_validate_desc(const struct virtq_desc* desc, const struct virtqueue_buffer* buf)
{
    CU_ASSERT_EQUAL(buf->ptr, (void*) desc->addr);
    CU_ASSERT_EQUAL(buf->len, desc->len);
    CU_ASSERT_EQUAL(buf->ro, (desc->flags & VIRTQ_DESC_F_WRITE) == 0);
}

/* Fill descriptor with a given id */
static struct virtq_desc* vq_fill_desc_id(
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

static void vq_publish_desc_id(struct virtqueue* vq, uint16_t id)
{
    vq->avail->ring[vq->avail->idx] = id;
    vq->avail->idx++;
}

static void vq_dequeue_and_verify(struct virtqueue* vq, struct virtq_desc** chain, uint16_t chain_len)
{
    struct virtqueue_buffer buf;
    struct virtqueue_desc_chain iter;
    CU_ASSERT_TRUE(virtqueue_dequeue(vq, &iter));

    for (uint16_t i = 0; i < chain_len; ++i) {
        CU_ASSERT_TRUE(virtqueue_next_buffer(&iter, &buf));
        if (chain) {
            vq_validate_desc(chain[i], &buf);
        }
    }

    CU_ASSERT_FALSE(virtqueue_next_buffer(&iter, &buf));
}

static void vq_dequeue_and_walk(struct virtqueue* vq, uint16_t expected_len)
{
    /* Just walk the chain and do nothing - we expect queue to [not] turn broken */
    vq_dequeue_and_verify(vq, NULL, expected_len);
}

/*
 * Tests
 */

/* Queue init test */
static void init_test(void)
{
    const uint16_t qsize = 1024;

    struct virtqueue vq;
    void* qdata = vq_alloc(qsize, &g_default_memory_map, &vq);

    /* Check parsed queue layout after init */
    CU_ASSERT_EQUAL(vq.desc, qdata);
    CU_ASSERT_EQUAL(vq.avail, ((void*)vq.desc + sizeof(struct virtq_desc) * qsize));
    CU_ASSERT_EQUAL(vq.used, VIRTQ_ALIGN_UP_PTR((void*)vq.avail + sizeof(uint16_t) * (3 + qsize)));
    CU_ASSERT_EQUAL(
        qdata + virtq_size(qsize),
        VIRTQ_ALIGN_UP_PTR((void*)vq.used + sizeof(uint16_t) * 3 + sizeof(struct virtq_used_elem) * qsize));

    free(qdata);
}

/* Test direct descriptors enqueue and dequeue */
static void dequeue_test(void)
{
    const uint16_t qsize = 1024;

    struct virtqueue vq;
    void* mem = vq_alloc(qsize, &g_default_memory_map, &vq);

    /*
     * Push a chain of direct descriptors of max length.
     */

    struct virtq_desc* chain[qsize];
    for (uint16_t i = 0; i < qsize; ++i) {
        /* Index descriptors in reverse to make things more interesting */
        uint16_t id = qsize - i - 1;
        chain[i] = vq_fill_desc_id(&vq, id, (void*)(uintptr_t) (i * 0x1000), 0x10, VIRTQ_DESC_F_NEXT, id - 1);
    };

    /* Patch last descriptor's next flag */
    chain[qsize - 1]->flags &= ~VIRTQ_DESC_F_NEXT;
    vq_publish_desc_id(&vq, qsize - 1);

    /*
     * Check what we get back from the virtqueue
     */

    vq_dequeue_and_verify(&vq, chain, qsize);
    CU_ASSERT_FALSE(virtqueue_is_broken(&vq));

    free(mem);
}

/* Test indirect descriptors enqueue and dequeue */
static void dequeue_indirect_test(void)
{
    const uint16_t qsize = 1024;

    struct virtqueue vq;
    void* mem = vq_alloc(qsize, &g_default_memory_map, &vq);

    /*
     * Create a table with qsize - 1 indirect descriptors and push 1 direct one to describe it.
     * qsize - 1 is to account for maximum chain length, which will include the indirect table pointer descriptor.
     */

    const uint16_t chain_len = qsize - 1;
    struct virtq_desc itbl[chain_len];
    struct virtq_desc* chain[chain_len];
    for (uint16_t i = 0; i < chain_len; ++i) {
        /* Index descriptors in reverse to make things more interesting,
         * except for first one which must be 0. */
        uint16_t id, next;
        if (i == 0) {
            id = 0;
            next = chain_len - 1;
        } else {
            id = chain_len - i;
            next = id - 1;
        }

        vq_fill_desc(&itbl[id], (void*)(uintptr_t) (i * 0x1000), 0x10, VIRTQ_DESC_F_NEXT, next);
        chain[i] = &itbl[id];
    };

    /* Patch last descriptor's next flag */
    chain[chain_len - 1]->flags &= ~VIRTQ_DESC_F_NEXT;

    vq_fill_desc_id(&vq, 42, itbl, sizeof(itbl), VIRTQ_DESC_F_INDIRECT, 0);
    vq_publish_desc_id(&vq, 42);

    /*
     * Check what we get back from the virtqueue
     */

    vq_dequeue_and_verify(&vq, chain, chain_len);
    CU_ASSERT_FALSE(virtqueue_is_broken(&vq));

    free(mem);
}

/* Test a combined direct/indirect chain of maximum possible size */
static void dequeue_combined_test(void)
{
    const uint16_t qsize = VIRTQ_MAX_SIZE;
    const uint16_t qsize_half = qsize >> 1;

    struct virtqueue vq;
    void* mem = vq_alloc(qsize, &g_default_memory_map, &vq);

    /*
     * Maximum combined chain length is still qsize, according to virtio spec.
     * Build a chain that will use qsize_half - 1 direct, followed by 1 table desc,
     * which points to qsize_half indirect.
     */

    /* Chain will contain all direct and indirect data descriptors, excluding the indirect table one */
    struct virtq_desc* chain[qsize - 1];
    struct virtq_desc itbl[qsize_half];

    uint16_t id = 0;
    for (; id < qsize_half - 1; ++id) {
        chain[id] = vq_fill_desc_id(&vq, id, (void*)(uintptr_t)(id * 0x1000), 0x10, VIRTQ_DESC_F_NEXT, id + 1);
    }

    vq_fill_desc_id(&vq, qsize_half - 1, itbl, sizeof(itbl), VIRTQ_DESC_F_INDIRECT, 0);

    for (uint16_t i = 0; i < qsize_half; ++i, ++id) {
        vq_fill_desc(&itbl[i], (void*)(uintptr_t)(id * 0x1000), 0x10, VIRTQ_DESC_F_NEXT, i + 1);
        chain[id] = &itbl[i];
    }

    /* Patch last descriptor's next flag */
    itbl[qsize_half - 1].flags &= ~VIRTQ_DESC_F_NEXT;
    vq_publish_desc_id(&vq, 0);

    /*
     * Check what we get back from the virtqueue
     */

    vq_dequeue_and_verify(&vq, chain, qsize - 1);
    CU_ASSERT_FALSE(virtqueue_is_broken(&vq));

    free(mem);
}

/* Test multiple dequeues */
static void dequeue_many_test(void)
{
    const uint16_t qsize = VIRTQ_MAX_SIZE;

    struct virtqueue vq;
    void* mem = vq_alloc(qsize, &g_default_memory_map, &vq);

    /*
     * Fill the queue with directs descriptor chains of length 1
     */

    struct virtq_desc* chain[qsize];
    for (uint16_t i = 0; i < qsize; ++i) {
        chain[i] = vq_fill_desc_id(&vq, i, (void*)(uintptr_t)(i * 0x1000), 0x10, 0, i + 1);
        vq_publish_desc_id(&vq, i);
    }

    /*
     * Check what we get back from the virtqueue
     */

    for (uint16_t i = 0; i < qsize; ++i) {
        vq_dequeue_and_verify(&vq, &chain[i], 1);
        CU_ASSERT_FALSE(virtqueue_is_broken(&vq));
    }

    free(mem);
}

/* Test multiple dequeues of indirect descriptors */
static void dequeue_many_indirect_test(void)
{
    const uint16_t qsize = 1024;

    struct virtqueue vq;
    void* mem = vq_alloc(qsize, &g_default_memory_map, &vq);

    /*
     * Fill the queue with descriptors pointing to the same indirect table.
     */

    const uint16_t chain_len = qsize - 1; /* Account for max chain length to include table pointer desc */
    struct virtq_desc itbl[chain_len];
    struct virtq_desc* chain[chain_len];
    for (uint16_t i = 0; i < chain_len; ++i) {
        vq_fill_desc(&itbl[i], (void*)(uintptr_t)(i * 0x1000), 0x10, VIRTQ_DESC_F_NEXT, i + 1);
        chain[i] = &itbl[i];
    }

    /* Patch last descriptor's next flag */
    itbl[chain_len - 1].flags &= ~VIRTQ_DESC_F_NEXT;

    for (uint16_t i = 0; i < qsize; ++i) {
        vq_fill_desc_id(&vq, i, itbl, sizeof(itbl), VIRTQ_DESC_F_INDIRECT, 0);
        vq_publish_desc_id(&vq, i);
    }

    /*
     * Check what we get back from the virtqueue
     */

    for (uint16_t i = 0; i < qsize; ++i) {
        vq_dequeue_and_verify(&vq, chain, chain_len);
        CU_ASSERT_FALSE(virtqueue_is_broken(&vq));
    }

    free(mem);
}

/* Break virtqueue init */
static void init_negative_test(void)
{
    size_t size_bytes = virtq_size(VIRTQ_MAX_SIZE);
    void* mem = aligned_alloc(4096, size_bytes);

    struct virtqueue vq;

    /* invalid qsize */
    CU_ASSERT_TRUE(0 != virtqueue_init(&vq, mem, 0, &g_default_memory_map));
    CU_ASSERT_TRUE(0 != virtqueue_init(&vq, mem, VIRTQ_MAX_SIZE + 1, &g_default_memory_map));
    CU_ASSERT_TRUE(0 != virtqueue_init(&vq, mem, VIRTQ_MAX_SIZE - 1 /* Within limits but not a power-of-2 */,
                                       &g_default_memory_map));

    /* base memory not aligned */
    CU_ASSERT_TRUE(0 != virtqueue_init(&vq, mem + 1, VIRTQ_MAX_SIZE, &g_default_memory_map));

    free(mem);
}

/* Try to dequeue from empty queue */
static void dequeue_empty_test(void)
{
    const uint16_t qsize = 1024;

    struct virtqueue vq;
    void* mem = vq_alloc(qsize, &g_default_memory_map, &vq);

    struct virtqueue_desc_chain iter;
    CU_ASSERT_FALSE(virtqueue_dequeue(&vq, &iter));
    CU_ASSERT_FALSE(virtqueue_is_broken(&vq));

    free(mem);
}

/* Try to use descriptor chain that is overflowing the limit */
static void descriptor_chain_too_long_test(void)
{
    const uint16_t qsize = 1024;

    struct virtqueue vq;
    void* mem = vq_alloc(qsize, &g_default_memory_map, &vq);

    /*
     * To actually have a chain long enough we have to use indirect descriptors
     */

    struct virtq_desc itbl[qsize];
    for (uint16_t i = 0; i < qsize; ++i) {
        vq_fill_desc(&itbl[i], (void*)0x1000, 0x10, VIRTQ_DESC_F_NEXT, i + 1);
    }

    /* Patch last descriptor's next flag */
    itbl[qsize - 1].flags &= ~VIRTQ_DESC_F_NEXT;

    /* Adding a table pointer descriptor will overflow the chain when we try to traverse it. */
    vq_fill_desc_id(&vq, 0, itbl, sizeof(itbl), VIRTQ_DESC_F_INDIRECT, 0);
    vq_publish_desc_id(&vq, 0);

    /* The last one should fail and mark the queue broken, so we expect 1 less to walk */
    vq_dequeue_and_walk(&vq, qsize - 1);
    CU_ASSERT_TRUE(virtqueue_is_broken(&vq));

    free(mem);
}

/* Create desciptor chain with several indirect descriptors (which is not allowed) */
static void several_indirect_descriptors_test(void)
{
    const uint16_t qsize = 1024;

    struct virtqueue vq;
    void* mem = vq_alloc(qsize, &g_default_memory_map, &vq);

    struct virtq_desc itbl[1];
    vq_fill_desc(&itbl[0], (void*)0x1000, 0x10, 0, 0);

    /* Having both next and indirect flags is not allowed,
     * thus it should not be possible to create more than 1 indirect descriptor */
    vq_fill_desc_id(&vq, 0, itbl, sizeof(itbl), VIRTQ_DESC_F_INDIRECT | VIRTQ_DESC_F_NEXT, 0);
    vq_fill_desc_id(&vq, 1, itbl, sizeof(itbl), VIRTQ_DESC_F_INDIRECT, 0);
    vq_publish_desc_id(&vq, 0);

    /* This should encounter bad indirect + next descriptor and fail */
    vq_dequeue_and_walk(&vq, 0);
    CU_ASSERT_TRUE(virtqueue_is_broken(&vq));

    free(mem);
}

/* Try to use empty indirect descriptors table */
static void empty_indirect_table_test(void)
{
    const uint16_t qsize = 1024;

    struct virtqueue vq;
    void* mem = vq_alloc(qsize, &g_default_memory_map, &vq);

    struct virtq_desc itbl[1];
    vq_fill_desc_id(&vq, 0, itbl, sizeof(struct virtq_desc) - 1 /* Empty */, VIRTQ_DESC_F_INDIRECT, 0);
    vq_publish_desc_id(&vq, 0);

    /* This should encounter bad table and fail */
    vq_dequeue_and_walk(&vq, 0);
    CU_ASSERT_TRUE(virtqueue_is_broken(&vq));

    free(mem);
}

/* Chain with invalid next indirect descriptor id */
static void invalid_next_descriptor_id(void)
{
    const uint16_t qsize = 1024;

    struct virtqueue vq;
    void* mem = vq_alloc(qsize, &g_default_memory_map, &vq);

    vq_fill_desc_id(&vq, 0, (void*) 0x1000, 0x10, VIRTQ_DESC_F_NEXT, qsize);
    vq_publish_desc_id(&vq, 0);

    vq_dequeue_and_walk(&vq, 0);
    CU_ASSERT_TRUE(virtqueue_is_broken(&vq));

    free(mem);
}

/* Chain with invalid next descriptor id */
static void invalid_next_indirect_descriptor_id(void)
{
    const uint16_t qsize = 1024;

    struct virtqueue vq;
    void* mem = vq_alloc(qsize, &g_default_memory_map, &vq);

    struct virtq_desc itbl[1];
    vq_fill_desc(&itbl[0], (void*) 0x1000, 0x10, VIRTQ_DESC_F_NEXT, sizeof(itbl) / sizeof(itbl));

    vq_fill_desc_id(&vq, 0, itbl, sizeof(itbl), VIRTQ_DESC_F_INDIRECT, qsize);
    vq_publish_desc_id(&vq, 0);

    vq_dequeue_and_walk(&vq, 0);
    CU_ASSERT_TRUE(virtqueue_is_broken(&vq));

    free(mem);
}

/* Check that nested indirect descriptors are not allowed */
static void nested_indirect_descriptor_test(void)
{
    const uint16_t qsize = 1024;

    struct virtqueue vq;
    void* mem = vq_alloc(qsize, &g_default_memory_map, &vq);

    struct virtq_desc itbl[1];
    vq_fill_desc(&itbl[0], (void*) 0x1000, 0x10, VIRTQ_DESC_F_INDIRECT, 0);
    vq_fill_desc_id(&vq, 0, itbl, sizeof(itbl), VIRTQ_DESC_F_INDIRECT, qsize);
    vq_publish_desc_id(&vq, 0);

    vq_dequeue_and_walk(&vq, 0);
    CU_ASSERT_TRUE(virtqueue_is_broken(&vq));

    free(mem);
}

/* Check that descriptor loops are detected (eventually) */
static void descriptor_loop_test(void)
{
    const uint16_t qsize = 1024;

    struct virtqueue vq;
    void* mem = vq_alloc(qsize, &g_default_memory_map, &vq);

    vq_fill_desc_id(&vq, 0, (void*) 0x1000, 0x10, VIRTQ_DESC_F_NEXT, 1);
    vq_fill_desc_id(&vq, 1, (void*) 0x2000, 0x20, VIRTQ_DESC_F_NEXT, 0);
    vq_publish_desc_id(&vq, 0);

    struct virtqueue_buffer buf;
    struct virtqueue_desc_chain iter;
    CU_ASSERT_TRUE(virtqueue_dequeue(&vq, &iter));

    /* We don't know _when_ exactly the loop will be detected - we just know that it _should_ be.
     * So this test becomes an infinite loop if loop detection is not working at all */
    while (virtqueue_next_buffer(&iter, &buf)) {
    }

    CU_ASSERT_TRUE(virtqueue_is_broken(&vq));

    free(mem);
}

/* Check that indirect descriptor loops are detected (eventually) */
static void indirect_descriptor_loop_test(void)
{
    const uint16_t qsize = 1024;

    struct virtqueue vq;
    void* mem = vq_alloc(qsize, &g_default_memory_map, &vq);

    struct virtq_desc itbl[2];
    vq_fill_desc(&itbl[0], (void*) 0x1000, 0x10, VIRTQ_DESC_F_NEXT, 1);
    vq_fill_desc(&itbl[1], (void*) 0x1000, 0x10, VIRTQ_DESC_F_NEXT, 0);

    vq_fill_desc_id(&vq, 0, itbl, sizeof(itbl), VIRTQ_DESC_F_INDIRECT, 0);
    vq_publish_desc_id(&vq, 0);

    struct virtqueue_buffer buf;
    struct virtqueue_desc_chain iter;
    CU_ASSERT_TRUE(virtqueue_dequeue(&vq, &iter));

    /* We don't know _when_ exactly the loop will be detected - we just know that it _should_ be.
     * So this test becomes an infinite loop if loop detection is not working at all */
    while (virtqueue_next_buffer(&iter, &buf)) {
    }

    CU_ASSERT_TRUE(virtqueue_is_broken(&vq));

    free(mem);
}

/*
 * 2.4.5.3.2 Device Requirements: Indirect Descriptors:
 * The device MUST ignore the write-only flag (flags&VIRTQ_DESC_F_WRITE)
 * in the descriptor that refers to an indirect table.
 */
static void ignore_write_only_for_indirect_descriptor_test(void)
{
    const uint16_t qsize = 1024;

    struct virtqueue vq;
    void* mem = vq_alloc(qsize, &g_default_memory_map, &vq);

    struct virtq_desc itbl[1];
    vq_fill_desc(&itbl[0], (void*) 0x1000, 0x10, 0, 0);
    vq_fill_desc_id(&vq, 0, itbl, sizeof(itbl), VIRTQ_DESC_F_INDIRECT | VIRTQ_DESC_F_WRITE, 0);
    vq_publish_desc_id(&vq, 0);

    /* Traversal should succeed entirely */
    vq_dequeue_and_walk(&vq, 1);
    CU_ASSERT_FALSE(virtqueue_is_broken(&vq));

    free(mem);
}

/* Attempt to dequeue a zero-length descriptor */
static void zero_length_descriptor_test(void)
{
    const uint16_t qsize = 1024;

    struct virtqueue vq;
    void* mem = vq_alloc(qsize, &g_default_memory_map, &vq);

    vq_fill_desc_id(&vq, 0, (void*) 0x1000, 0, VIRTQ_DESC_F_WRITE, 0);
    vq_publish_desc_id(&vq, 0);

    vq_dequeue_and_walk(&vq, 0);
    CU_ASSERT_TRUE(virtqueue_is_broken(&vq));

    free(mem);
}

/* Attempt to dequeue a zero-length indirect descriptor */
static void zero_length_indirect_descriptor_test(void)
{
    const uint16_t qsize = 1024;

    struct virtqueue vq;
    void* mem = vq_alloc(qsize, &g_default_memory_map, &vq);

    struct virtq_desc itbl[1];
    vq_fill_desc(&itbl[0], (void*) 0x1000, 0, 0, 0);
    vq_fill_desc_id(&vq, 0, itbl, sizeof(itbl), VIRTQ_DESC_F_INDIRECT, 0);
    vq_publish_desc_id(&vq, 0);

    vq_dequeue_and_walk(&vq, 0);
    CU_ASSERT_TRUE(virtqueue_is_broken(&vq));

    free(mem);
}

int main(int argc, char** argv)
{
    if (CUE_SUCCESS != CU_initialize_registry()) {
        return CU_get_error();
    }

    CU_pSuite suite = CU_add_suite(VHOST_TEST_SUITE_NAME, NULL, NULL);
    if (NULL == suite) {
        CU_cleanup_registry();
        return CU_get_error();
    }

    CU_add_test(suite, "init_test", init_test);
    CU_add_test(suite, "dequeue_test", dequeue_test);
    CU_add_test(suite, "dequeue_indirect_test", dequeue_indirect_test);
    CU_add_test(suite, "dequeue_combined_test", dequeue_combined_test);
    CU_add_test(suite, "dequeue_many_test", dequeue_many_test);
    CU_add_test(suite, "dequeue_many_indirect_test", dequeue_many_indirect_test);

    CU_add_test(suite, "init_negative_test", init_negative_test);
    CU_add_test(suite, "dequeue_empty_test", dequeue_empty_test);
    CU_add_test(suite, "descriptor_chain_too_long_test", descriptor_chain_too_long_test);
    CU_add_test(suite, "several_indirect_descriptors_test", several_indirect_descriptors_test);
    CU_add_test(suite, "empty_indirect_table_test", empty_indirect_table_test);
    CU_add_test(suite, "invalid_next_descriptor_id", invalid_next_descriptor_id);
    CU_add_test(suite, "invalid_next_indirect_descriptor_id", invalid_next_indirect_descriptor_id);
    CU_add_test(suite, "nested_indirect_descriptor_test", nested_indirect_descriptor_test);
    CU_add_test(suite, "descriptor_loop_test", descriptor_loop_test);
    CU_add_test(suite, "indirect_descriptor_loop_test", indirect_descriptor_loop_test);
    CU_add_test(suite, "ignore_write_only_for_indirect_descriptor_test", ignore_write_only_for_indirect_descriptor_test);
    CU_add_test(suite, "zero_length_descriptor_test", zero_length_descriptor_test);
    CU_add_test(suite, "zero_length_indirect_descriptor_test", zero_length_indirect_descriptor_test);

    /* run tests */
    CU_basic_set_mode(CU_BRM_VERBOSE);
    CU_basic_run_tests();
    int res = CU_get_error() || CU_get_number_of_tests_failed();

    CU_cleanup_registry();
    return res;
}
