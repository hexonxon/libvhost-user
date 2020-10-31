/**
 * A small test server for the library
 */

#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/stat.h>

#include <vhost.h>
#include <virtio/blk.h>

#define DIE(fmt, ...) do { \
    fprintf(stderr, fmt "\n", ##__VA_ARGS__); \
    exit(EXIT_FAILURE); \
} while (0);

static int g_fd = -1;

static void usage(void)
{
    fprintf(stderr, "vhost-server socket-path disk-image\n");
}

static int handle_rw(struct virtio_blk* vblk, struct blk_io_request* bio)
{
    uint64_t sector = bio->sector;
    uint32_t total_sectors = bio->total_sectors;
    uint32_t vec_idx = 0;

    while (total_sectors > 0) {
        if (vec_idx >= bio->nvecs) {
            DIE("Not enough iovecs to handle request (have %d)", bio->nvecs);
        }

        struct virtio_iovec* pvec = &bio->vecs[vec_idx];
        uint32_t nsectors = pvec->len >> VIRTIO_BLK_SECTOR_SHIFT;
        if (nsectors > total_sectors) {
            nsectors = total_sectors;
        }

        ssize_t res = 0;
        size_t count = nsectors << VIRTIO_BLK_SECTOR_SHIFT;
        off_t offset = sector << VIRTIO_BLK_SECTOR_SHIFT;

        if (bio->type == BLK_IO_READ) {
            res = pread(g_fd, pvec->ptr, count, offset);
        } else if (bio->type == BLK_IO_WRITE) {
            res = pwrite(g_fd, pvec->ptr, count, offset);
        } else {
            DIE("Unexpected request type %d", bio->type);
        }

        if (res != count) {
            DIE("Read/write failed at offset %lu, size %zu: %d", offset, count, -errno);
        }

        sector += nsectors;
        total_sectors -= nsectors;
        vec_idx++;

        if (pvec->len - count > 0) {
            DIE("Still have space remaining in vector");
        }
    }

    return 0;
}

int process_event(struct virtio_dev* vdev, struct vring* vring)
{
    int error = 0;
    struct virtio_blk* vblk = (struct virtio_blk*) vdev; /* TODO: add a type conversion helper in virtio */

    struct blk_io_request* bio;
    while (true) {
        error = virtio_blk_dequeue_request(vblk, &vring->vq, &bio);
        if (error == -ENOENT) {
            break;
        }

        fprintf(stdout, "Handling request type %d\n", bio->type);

        if (error) {
            fprintf(stderr, "Could not dequeue vblk request: %d\n", error);
            return error;
        }

        if (bio->type == BLK_IO_GET_ID) {
            snprintf(bio->vecs[0].ptr, bio->vecs[0].len, "vhost-blk-0");
            goto complete;
        }

        /*
         * All IO error are reported to guest and not vhost implementation
         */

        error = handle_rw(vblk, bio);
        if (error) {
            fprintf(stderr, "Failed handling bio %p: %d\n", bio, error);
        }

complete:
        virtio_blk_complete_request(vblk, bio, (error ? BLK_IOERROR : BLK_SUCCESS));
    }

    return 0;
}

int main(int argc, char** argv)
{
    if (argc != 3) {
        usage();
        exit(EXIT_FAILURE);
    }

    int error = 0;
    const char* socket_path = argv[1];
    const char* disk_image = argv[2];

    error = access(socket_path, F_OK);
    if (!error) {
        DIE("Socket path %s already exists, refusing to reuse", socket_path);
    }

    error = access(disk_image, F_OK | R_OK);
    if (error) {
        DIE("Disk image path %s does not exist or is not readable", disk_image);
    }

    bool ro = false;
    error = access(disk_image, W_OK);
    if (error) {
        fprintf(stdout, "Disk image %s is not writable - will user readonly device\n", disk_image);
        ro = true;
    }

    g_fd = open(disk_image, O_SYNC | (ro ? O_RDONLY : O_RDWR));
    if (g_fd < 0) {
        DIE("Could not open disk image file %s", disk_image);
    }

    struct stat st;
    error = fstat(g_fd, &st);
    if (error) {
        DIE("Could not fstat disk image %s", disk_image);
    }

    if (st.st_blocks == 0) {
        DIE("Disk image %s has 0 blocks", disk_image);
    }

    uint32_t blocks = st.st_size / VIRTIO_BLK_SECTOR_SIZE;
    fprintf(stdout, "Using disk image %s, %u blocks\n", disk_image, blocks);

    struct virtio_blk vblk;
    vblk.total_sectors = blocks;
    vblk.block_size = VIRTIO_BLK_SECTOR_SIZE;
    vblk.readonly = ro;
    vblk.writeback = false;
    error = virtio_blk_init(&vblk);
    if (error) {
        DIE("Failed to initialize virtio-blk device: %d", error);
    }

    struct vhost_dev dev;
    error = vhost_register_device_server(&dev, socket_path, 1, &vblk.vdev, process_event);
    if (error) {
        DIE("Failed to register device server: %d", error);
    }

    while (1) {
        error = vhost_run();
        if (error) {
            DIE("vhost run failed with %d", error);
        }
    }

    close(g_fd);
    return 0;
}
