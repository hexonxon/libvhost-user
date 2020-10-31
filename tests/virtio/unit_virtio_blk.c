/**
 * virtio-blk unit tests
 */

#include <stdlib.h>
#include <stdint.h>

#include <CUnit/Basic.h>
#include <CUnit/CUnit.h>

#include "virtio/blk.h"

#include "vq_data.h"

/* Default memory map just allows the entire address space for rw access.
 * Specific tests might use something more restictive. */
static struct virtio_memory_map g_default_memory_map = {
    .num_regions = 1,
    .regions = {
        { 0, UINTPTR_MAX, NULL, false },
    },
};

enum {
    VBLK_TEST_DEV_SECTORS = 1024,
    VBLK_TEST_DEV_BSIZE = 4096,
    VBLK_TEST_DEV_QUEUE_SIZE = 1024,
    VBLK_TEST_DEV_MAX_QUEUES = 16,
};

struct vblk_queue
{
    struct virtqueue vq;
    void* data;
};

/** Test virtio-blk device context */
struct vblk_test_dev
{
    struct virtio_blk vblk;
    struct vblk_queue queues[16];
    uint32_t num_queues;
};

/** Helper to initialize a test virtio-blk device and its queues */
static void vblk_init(struct vblk_test_dev* dev, uint64_t sectors, uint32_t bsize, bool ro, bool wb, uint32_t num_queues)
{
    dev->vblk.total_sectors = sectors;
    dev->vblk.block_size = bsize;
    dev->vblk.readonly = ro;
    dev->vblk.writeback = wb;
    CU_ASSERT_EQUAL(0, virtio_blk_init(&dev->vblk));

    CU_ASSERT_FATAL(num_queues < VBLK_TEST_DEV_MAX_QUEUES);
    dev->num_queues = num_queues;

    for (uint32_t i = 0; i < num_queues; ++i) {
        dev->queues[i].data = vq_alloc(VBLK_TEST_DEV_QUEUE_SIZE, &g_default_memory_map, &dev->queues[i].vq);
    }
}

static void vblk_init_default(struct vblk_test_dev* dev)
{
    vblk_init(dev, VBLK_TEST_DEV_SECTORS, VBLK_TEST_DEV_BSIZE, false, false, 1);
}

static void vblk_free(struct vblk_test_dev* dev)
{
    for (uint32_t i = 0; i < dev->num_queues; ++i) {
        free(dev->queues[i].data);
    }
}

static struct virtq_desc* fill_req_hdr(struct virtqueue* vq, uint16_t desc, struct virtio_blk_req* hdr, uint16_t next)
{
    return vq_fill_desc_id(vq, desc, hdr, sizeof(*hdr), VIRTQ_DESC_F_NEXT, next);
}

static struct virtq_desc* fill_req_buf(struct virtqueue* vq, uint16_t desc, void* ptr, uint32_t len, bool ro, uint16_t next)
{
    return vq_fill_desc_id(vq, desc, ptr, len, VIRTQ_DESC_F_NEXT | (ro ? 0 : VIRTQ_DESC_F_WRITE), next);
}

static struct virtq_desc* fill_req_status(struct virtqueue* vq, uint16_t desc, uint8_t* ptr)
{
    return vq_fill_desc_id(vq, desc, ptr, sizeof(*ptr), VIRTQ_DESC_F_WRITE, 0);
}

struct vblk_req_data
{
    struct virtio_blk_req hdr;
    uint8_t status;
    struct virtqueue_buffer buffers[UINT8_MAX];
    uint8_t num_buffers;
};

static struct virtq_desc* vblk_enqueue_req(struct vblk_test_dev* dev, uint32_t qidx, struct vblk_req_data* req, uint16_t desc_head)
{
    CU_ASSERT_FATAL(qidx < dev->num_queues);

    struct virtqueue* vq = &dev->queues[qidx].vq;
    uint16_t desc = desc_head;

    struct virtq_desc* phead = fill_req_hdr(vq, desc, &req->hdr, desc + 1);
    
    ++desc;
    for (uint8_t i = 0; i < req->num_buffers; ++i, ++desc) {
        fill_req_buf(vq, desc, req->buffers[i].ptr, req->buffers[i].len, req->buffers[i].ro, desc + 1);
    }

    fill_req_status(vq, desc, &req->status);
    vq_publish_desc_id(vq, desc_head);

    return phead;
}

static struct blk_io_request* vblk_dequeue_and_verify(struct vblk_test_dev* dev,
                                                      uint32_t qidx,
                                                      const struct vblk_req_data* req)
{
    struct blk_io_request* bio = NULL;
    CU_ASSERT_TRUE(0 == virtio_blk_dequeue_request(&dev->vblk, &dev->queues[0].vq, &bio));

    CU_ASSERT_EQUAL(req->hdr.type, bio->type);
    CU_ASSERT_EQUAL(req->hdr.sector, bio->sector);

    CU_ASSERT_EQUAL(req->num_buffers, bio->nvecs);

    uint32_t sectors = 0;
    for (uint8_t i = 0; i < req->num_buffers; ++i) {
        sectors += req->buffers[i].len >> VIRTIO_BLK_SECTOR_SHIFT;
        CU_ASSERT_EQUAL(req->buffers[i].ptr, bio->vecs[i].ptr);
        CU_ASSERT_EQUAL(req->buffers[i].len, bio->vecs[i].len);
    }

    CU_ASSERT_EQUAL(sectors, bio->total_sectors);
    return bio;
}

/**
 * Positive/negative vblk init test
 */
static void init_test(void)
{
    struct virtio_blk good;
    good.total_sectors = 1024;
    good.block_size = 4096;
    good.readonly = false;
    good.writeback = false;

    struct virtio_blk bad;

    /* Good init */
    CU_ASSERT(0 == virtio_blk_init(&good));

    /* Bad block size (not a multiple of 512) */
    bad = good;
    bad.block_size -= 1;
    CU_ASSERT(0 != virtio_blk_init(&bad));
    bad.block_size = 0;
    CU_ASSERT(0 != virtio_blk_init(&bad));

    /* Bad total sectors */
    bad = good;
    bad.total_sectors = 0;
    CU_ASSERT(0 != virtio_blk_init(&bad));
}

/**
 * Build simple read request chain, enqueue it,
 * let virtio-blk implementation parse and dequeue it for us,
 * verify dequeued bio correctness and commit it as completed
 */
static void rw_request_test(void)
{
    struct vblk_test_dev dev;
    vblk_init_default(&dev);

    struct vblk_req_data req = {
        .hdr = { VIRTIO_BLK_T_IN, 0 },
        .buffers = {
            { (void*) 0x1000, 0x1000, false },
            { (void*) 0x4000, 0x2000, false },
        },
        .num_buffers = 2,
        .status = -1,
    };

    vblk_enqueue_req(&dev, 0, &req, 0);
    struct blk_io_request* bio = vblk_dequeue_and_verify(&dev, 0, &req);

    /* Completion status should reach back our status variable through virtq_desc in the request chain */
    virtio_blk_complete_request(&dev.vblk, bio, BLK_SUCCESS);
    CU_ASSERT_EQUAL(req.status, BLK_SUCCESS);

    /* vq we used for request should be empty now */
    struct virtqueue_buffer_iter unused;
    CU_ASSERT_FALSE(virtqueue_dequeue_avail(&dev.queues[0].vq, &unused));

    vblk_free(&dev);
}

/**
 * Attempt to send a write request to a read-only device
 */
static void write_request_for_ro_device(void)
{
    struct vblk_test_dev dev;
    vblk_init(&dev, VBLK_TEST_DEV_SECTORS, VBLK_TEST_DEV_BSIZE, true, false, 1);

    struct vblk_req_data req = {
        .hdr = { VIRTIO_BLK_T_OUT, 0 },
        .buffers = {
            { (void*) 0x1000, 0x1000, true },
        },
        .num_buffers = 1,
    };

    vblk_enqueue_req(&dev, 0, &req, 0);

    struct blk_io_request* bio = NULL;
    CU_ASSERT(0 != virtio_blk_dequeue_request(&dev.vblk, &dev.queues[0].vq, &bio));

    vblk_free(&dev);
}

/**
 * Submit a request that does not allow writes to status buffer
 */
static void read_only_status_buffer(void)
{
    struct vblk_test_dev dev;
    vblk_init(&dev, VBLK_TEST_DEV_SECTORS, VBLK_TEST_DEV_BSIZE, true, false, 1);

    struct virtio_blk_req hdr = { VIRTIO_BLK_T_OUT, 0 };
    uint8_t status;

    vq_fill_desc_id(&dev.queues[0].vq, 0, &hdr, sizeof(hdr), VIRTQ_DESC_F_NEXT, 1);
    vq_fill_desc_id(&dev.queues[0].vq, 1, (void*) 0x1000, 0x1000, VIRTQ_DESC_F_NEXT, 2);
    vq_fill_desc_id(&dev.queues[0].vq, 2, &status, sizeof(status), 0 /* Missing VIRTQ_DESC_F_WRITE */, 0);
    vq_publish_desc_id(&dev.queues[0].vq, 0);

    struct blk_io_request* bio = NULL;
    CU_ASSERT(0 != virtio_blk_dequeue_request(&dev.vblk, &dev.queues[0].vq, &bio));

    vblk_free(&dev);
}

/**
 * Submit a request with an incorrect status buffer size
 */
static void incorrect_status_buffer_size(void)
{
    struct vblk_test_dev dev;
    vblk_init(&dev, VBLK_TEST_DEV_SECTORS, VBLK_TEST_DEV_BSIZE, true, false, 1);

    struct virtio_blk_req hdr = { VIRTIO_BLK_T_OUT, 0 };
    uint8_t status;

    vq_fill_desc_id(&dev.queues[0].vq, 0, &hdr, sizeof(hdr), VIRTQ_DESC_F_NEXT, 1);
    vq_fill_desc_id(&dev.queues[0].vq, 1, (void*) 0x1000, 0x1000, VIRTQ_DESC_F_NEXT, 2);
    vq_fill_desc_id(&dev.queues[0].vq, 2, &status, sizeof(status) + 1, VIRTQ_DESC_F_WRITE, 0);
    vq_publish_desc_id(&dev.queues[0].vq, 0);

    struct blk_io_request* bio = NULL;
    CU_ASSERT(0 != virtio_blk_dequeue_request(&dev.vblk, &dev.queues[0].vq, &bio));

    vblk_free(&dev);
}

/**
 * Submit a request with bad header buffer size
 */
static void incorrect_header_buffer_size(void)
{
    struct vblk_test_dev dev;
    vblk_init(&dev, VBLK_TEST_DEV_SECTORS, VBLK_TEST_DEV_BSIZE, true, false, 1);

    struct virtio_blk_req hdr = { VIRTIO_BLK_T_OUT, 0 };
    uint8_t status;

    vq_fill_desc_id(&dev.queues[0].vq, 0, &hdr, sizeof(hdr) + 1, VIRTQ_DESC_F_NEXT, 1);
    vq_fill_desc_id(&dev.queues[0].vq, 1, (void*) 0x1000, 0x1000, VIRTQ_DESC_F_NEXT, 2);
    vq_fill_desc_id(&dev.queues[0].vq, 2, &status, sizeof(status), VIRTQ_DESC_F_WRITE, 0);
    vq_publish_desc_id(&dev.queues[0].vq, 0);

    struct blk_io_request* bio = NULL;
    CU_ASSERT(0 != virtio_blk_dequeue_request(&dev.vblk, &dev.queues[0].vq, &bio));

    vblk_free(&dev);
}

/**
 * Submit a request with missing data buffers
 */
static void no_data_buffers(void)
{
    struct vblk_test_dev dev;
    vblk_init(&dev, VBLK_TEST_DEV_SECTORS, VBLK_TEST_DEV_BSIZE, true, false, 1);

    struct virtio_blk_req hdr = { VIRTIO_BLK_T_OUT, 0 };
    uint8_t status;

    vq_fill_desc_id(&dev.queues[0].vq, 0, &hdr, sizeof(hdr), VIRTQ_DESC_F_NEXT, 1);
    vq_fill_desc_id(&dev.queues[0].vq, 1, &status, sizeof(status), VIRTQ_DESC_F_WRITE, 0);
    vq_publish_desc_id(&dev.queues[0].vq, 0);

    struct blk_io_request* bio = NULL;
    CU_ASSERT(0 != virtio_blk_dequeue_request(&dev.vblk, &dev.queues[0].vq, &bio));

    vblk_free(&dev);
}

/**
 * Submit a request with missing data and status buffers
 */
static void no_data_or_status_buffers(void)
{
    struct vblk_test_dev dev;
    vblk_init(&dev, VBLK_TEST_DEV_SECTORS, VBLK_TEST_DEV_BSIZE, true, false, 1);

    struct virtio_blk_req hdr = { VIRTIO_BLK_T_OUT, 0 };

    vq_fill_desc_id(&dev.queues[0].vq, 0, &hdr, sizeof(hdr), 0, 1);
    vq_publish_desc_id(&dev.queues[0].vq, 0);

    struct blk_io_request* bio = NULL;
    CU_ASSERT(0 != virtio_blk_dequeue_request(&dev.vblk, &dev.queues[0].vq, &bio));

    vblk_free(&dev);
}

/**
 * Submit a request with 0 data size
 */
static void zero_data_size(void)
{
    struct vblk_test_dev dev;
    vblk_init(&dev, VBLK_TEST_DEV_SECTORS, VBLK_TEST_DEV_BSIZE, true, false, 1);

    struct virtio_blk_req hdr = { VIRTIO_BLK_T_OUT, 0 };
    uint8_t status;

    vq_fill_desc_id(&dev.queues[0].vq, 0, &hdr, sizeof(hdr), VIRTQ_DESC_F_NEXT, 1);
    vq_fill_desc_id(&dev.queues[0].vq, 1, (void*) 0x1000, 0, VIRTQ_DESC_F_NEXT, 2);
    vq_fill_desc_id(&dev.queues[0].vq, 2, &status, sizeof(status), VIRTQ_DESC_F_WRITE, 0);
    vq_publish_desc_id(&dev.queues[0].vq, 0);

    struct blk_io_request* bio = NULL;
    CU_ASSERT(0 != virtio_blk_dequeue_request(&dev.vblk, &dev.queues[0].vq, &bio));

    vblk_free(&dev);
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
    CU_add_test(suite, "rw_request_test", rw_request_test);
    CU_add_test(suite, "write_request_for_ro_device", write_request_for_ro_device);
    CU_add_test(suite, "read_only_status_buffer", read_only_status_buffer);
    CU_add_test(suite, "incorrect_status_buffer_size", incorrect_status_buffer_size);
    CU_add_test(suite, "incorrect_header_buffer_size", incorrect_header_buffer_size);
    CU_add_test(suite, "no_data_buffers", no_data_buffers);
    CU_add_test(suite, "no_data_or_status_buffers", no_data_or_status_buffers);
    CU_add_test(suite, "zero_data_size", zero_data_size);

    /* run tests */
    CU_basic_set_mode(CU_BRM_VERBOSE);
    CU_basic_run_tests();
    int res = CU_get_error() || CU_get_number_of_tests_failed();

    CU_cleanup_registry();
    return res;
}
