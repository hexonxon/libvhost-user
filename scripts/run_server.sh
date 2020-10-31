#!/bin/bash

set -e

SERVER=$(realpath ${SERVER:-build-x86/tools/server/vhost-server})
VHOST_SOCK=$(realpath ${VHOST_SOCK:-/tmp/vhost.sock})
DISK_IMAGE=$(realpath $1)

[ -S $VHOST_SOCK ] && rm -f $VHOST_SOCK

$SERVER $VHOST_SOCK $DISK_IMAGE
