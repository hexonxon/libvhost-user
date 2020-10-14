/**
 * virtio memory map unit tests
 */

#include <stdlib.h>
#include <stdint.h>

#include <CUnit/Basic.h>
#include <CUnit/CUnit.h>

#include "virtio/memory.h"

static void init_test(void)
{
    struct virtio_memory_map mem = VIRTIO_INIT_MEMORY_MAP;
    CU_ASSERT_TRUE(mem.num_regions == 0);
}

static void insert_and_query_regions_test(void)
{
    struct virtio_memory_map mem = VIRTIO_INIT_MEMORY_MAP;

    /*
     * Using a series of inserts build the following series of adjacent regions:
     * <r1/rw> <r2/ro> <r3/rw>
     */

    uint64_t len = 0x1000;
    uint64_t gpa1 = 0x1000;
    uint64_t gpa2 = gpa1 + len;
    uint64_t gpa3 = gpa2 + len;

    /* Start from the middle r2 so that we test both pre and post insertions */
    CU_ASSERT_EQUAL(0, virtio_add_guest_region(&mem, gpa2, len, (void*) gpa2, true));
    CU_ASSERT_EQUAL(0, virtio_add_guest_region(&mem, gpa1, len, (void*) gpa1, false));
    CU_ASSERT_EQUAL(0, virtio_add_guest_region(&mem, gpa3, len, (void*) gpa3, false));

    /* Validate map contents: 3 regions are sorted in ascending gpa order */
    CU_ASSERT_EQUAL(3, mem.num_regions);
    CU_ASSERT_TRUE(mem.regions[0].gpa == gpa1);
    CU_ASSERT_TRUE(mem.regions[1].gpa == gpa2);
    CU_ASSERT_TRUE(mem.regions[2].gpa == gpa3);

    /* Query the map by half-len offsets with full length.
     * This way we capture cross-region query (for ro regions). */
    for (uint64_t gpa = gpa1; gpa <= gpa3; gpa += len / 2) {
        CU_ASSERT_EQUAL((void*)gpa, virtio_find_gpa_range(&mem, gpa, len, true));
    }

    /* Query the full covered space */
    CU_ASSERT_EQUAL((void*) gpa1, virtio_find_gpa_range(&mem, gpa1, len * 3, true));
    CU_ASSERT_EQUAL(MAP_FAILED, virtio_find_gpa_range(&mem, gpa1, len * 3, false)); /* rw query should fail */

    /* Query OOB before the very first region */
    CU_ASSERT_EQUAL(MAP_FAILED, virtio_find_gpa_range(&mem, gpa1 - 1, len, true));

    /* Query OOB after the very last region */
    CU_ASSERT_EQUAL(MAP_FAILED, virtio_find_gpa_range(&mem, gpa3, len + 1, true));

    /* Query empty region */
    CU_ASSERT_EQUAL(MAP_FAILED, virtio_find_gpa_range(&mem, gpa3, 0, true));
}

static void overflow_max_regions_test(void)
{
    struct virtio_memory_map mem = VIRTIO_INIT_MEMORY_MAP;

    /*
     * Insert too many regions
     */

    uint64_t gpa = 0;
    const uint64_t len = 0x1000;
    for (uint32_t i = 0; i < VIRTIO_MEMORY_MAX_REGIONS; ++i) {
        CU_ASSERT_EQUAL(0, virtio_add_guest_region(&mem, gpa, len, (void*) gpa, false));
        gpa += len;
    }

    CU_ASSERT_NOT_EQUAL(0, virtio_add_guest_region(&mem, gpa, len, (void*) gpa, false));
}

static void cross_region_query_for_non_continous_space_test(void)
{
    struct virtio_memory_map mem = VIRTIO_INIT_MEMORY_MAP;

    /*
     * Create a map where 2 regions have an unmapped gap between them
     * and query for gpa range that touches them both
     */

    const uint64_t len = 0x1000;
    const uint64_t gpa1 = 0x1000;
    const uint64_t gpa2 = gpa1 + len * 2;

    CU_ASSERT_EQUAL(0, virtio_add_guest_region(&mem, gpa1, len, (void*) gpa1, true));
    CU_ASSERT_EQUAL(0, virtio_add_guest_region(&mem, gpa2, len, (void*) gpa2, true));
    CU_ASSERT_EQUAL(MAP_FAILED, virtio_find_gpa_range(&mem, gpa1, len * 3, true));
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
    CU_add_test(suite, "insert_and_query_regions_test", insert_and_query_regions_test);
    CU_add_test(suite, "overflow_max_regions_test", overflow_max_regions_test);
    CU_add_test(suite, "cross_region_query_for_non_continous_space_test", cross_region_query_for_non_continous_space_test);

    /* run tests */
    CU_basic_set_mode(CU_BRM_VERBOSE);
    CU_basic_run_tests();
    int res = CU_get_error() || CU_get_number_of_tests_failed();

    CU_cleanup_registry();
    return res;
}
