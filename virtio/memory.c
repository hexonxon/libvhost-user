#include <assert.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include "virtio/memory.h"

static bool region_contains_gpa(const struct virtio_memory_region* mr, uint64_t gpa)
{
    return gpa >= mr->gpa && (mr->gpa + mr->len - 1) >= gpa;
}

static bool are_overlapping_regions(const struct virtio_memory_region* r1, const struct virtio_memory_region* r2)
{
    assert(r1 && r2);

    /* Position regions so that r1 preceeds r2 */
    if (r1->gpa > r2->gpa) {
        const struct virtio_memory_region* tmp = r1;
        r1 = r2;
        r2 = tmp;
    }

    return (r2->gpa <= r1->gpa + r1->len - 1);
}

int virtio_add_guest_region(struct virtio_memory_map* mem, uint64_t gpa, uint64_t len, void* hva, bool ro)
{
    assert(mem);

    if (mem->num_regions == VIRTIO_MEMORY_MAX_REGIONS) {
        return -ENOSPC;
    }

    uint32_t pos;
    for (pos = 0; pos < mem->num_regions; ++pos) {
        if (gpa < mem->regions[pos].gpa) {
            break;
        }
    }

    struct virtio_memory_region mr = {
        .gpa = gpa,
        .len = len,
        .hva = hva,
        .ro = ro,
    };

    struct virtio_memory_region* next = (pos == mem->num_regions ? NULL : &mem->regions[pos]);
    struct virtio_memory_region* prev = (pos == 0 ? NULL : &mem->regions[pos - 1]);

    /* Check that new region is not intersecting previous and next */
    if ((next && are_overlapping_regions(next, &mr)) || (prev && are_overlapping_regions(prev, &mr))) {
        return -EINVAL;
    }

    /* Make space for new region and insert */
    memmove(&mem->regions[pos + 1], &mem->regions[pos], (mem->num_regions - pos) * sizeof(mr));
    mem->regions[pos] = mr;
    mem->num_regions++;

    return 0;
}

/** Lookup a region that has this gpa and return its index. Otherwise return num_regions. */
static size_t find_region(const struct virtio_memory_map* mem, uint64_t gpa)
{
    /* TODO: Since regions are sorted, this can be done by a binary search */
    for (uint32_t i = 0; i < mem->num_regions; ++i) {
        if (region_contains_gpa(&mem->regions[i], gpa)) {
            return i;
        }
    }

    return mem->num_regions;
}

/** Find continously mapped gpa range or return NULL if mapping is invalid */
void* virtio_find_gpa_range(const struct virtio_memory_map* mem, uint64_t gpa, uint32_t len, bool ro)
{
    if (len == 0) {
        return MAP_FAILED;
    }

    uint32_t region_id = find_region(mem, gpa);
    if (region_id == mem->num_regions) {
        return MAP_FAILED;
    }

    const struct virtio_memory_region* mr = &mem->regions[region_id];
    void* res = (void*) ((uintptr_t) mr->hva + gpa - mr->gpa);

    while (len > 0 && region_id < mem->num_regions) {
        mr = &mem->regions[region_id];

        /* Check that we're not breaking read-only restriction, if any */
        if (!ro && mr->ro) {
            break;
        }

        uint64_t remaining_len = mr->len - (gpa - mr->gpa);
        uint64_t consumed_len = (len > remaining_len ? remaining_len : len);

        len -= consumed_len;
        gpa += consumed_len;

        ++region_id;

        /* Go to next region only if they are continous (we will check ro flag on next iteration) */
        if (len && region_id < mem->num_regions) {
            if (mem->regions[region_id].gpa != mr->gpa + mr->len) {
                break;
            }
        }
    }

    return len > 0 ? MAP_FAILED : res;
}
