#!/bin/bash

set -e

QEMU=$(realpath ${QEMU:-qemu-system-x86_64})
VHOST_SOCK=$(realpath ${VHOST_SOCK:-/tmp/vhost.sock})

$QEMU \
    -machine pc,accel=kvm \
    -cpu host \
    -m 512 \
    -object memory-backend-file,id=mem0,size=512M,mem-path=/dev/shm/vhost,share=on \
    -numa node,memdev=mem0 \
    -smp 2 \
    -display none \
    -chardev socket,id=char0,reconnect=1,path=$VHOST_SOCK \
    -device vhost-user-blk-pci,packed=on,chardev=char0,num-queues=1 \

