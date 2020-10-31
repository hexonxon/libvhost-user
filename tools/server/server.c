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

static int handle_vring_event(struct virtio_dev* vdev, struct vring* vring)
{
    /* TODO: implement me */
    fprintf(stdout, "Got an event on vring %p", vring);
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
    error = vhost_register_device_server(&dev, socket_path, 1, &vblk.vdev, handle_vring_event);
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
