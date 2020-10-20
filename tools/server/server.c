/**
 * A small test server for the library
 */

#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/types.h>

#include <vhost.h>
#include <virtio/blk.h>

#define DIE(fmt, ...) do { \
    fprintf(stderr, fmt, ##__VA_ARGS__); \
    fprintf(stderr, "\n"); \
    exit(EXIT_FAILURE); \
} while (0);

enum {
    VBLK_TEST_DEV_SECTORS = 1024,
    VBLK_TEST_DEV_BSIZE = 4096,
};

static void usage(void)
{
    fprintf(stderr, "vhost-server socket-path\n");
}

int main(int argc, char** argv)
{
    if (argc != 2) {
        usage();
        exit(EXIT_FAILURE);
    }

    int error = 0;
    const char* socket_path = argv[1];

    error = access(socket_path, F_OK);
    if (!error) {
        DIE("Socket path %s already exists, refusing to reuse", socket_path);
    }

    struct virtio_blk vblk;
    vblk.total_sectors = VBLK_TEST_DEV_SECTORS;
    vblk.block_size = VBLK_TEST_DEV_BSIZE;
    vblk.readonly = false;
    vblk.writeback = false;
    error = virtio_blk_init(&vblk);
    if (error) {
        DIE("Failed to initialize virtio-blk device: %d", error);
    }

    struct vhost_dev dev;
    error = vhost_register_device_server(&dev, socket_path, 1, &vblk.vdev);
    if (error) {
        DIE("Failed to register device server: %d", error);
    }

    while (1) {
        error = vhost_run();
        if (error) {
            DIE("vhost run failed with %d", error);
        }
    }

    return 0;
}
