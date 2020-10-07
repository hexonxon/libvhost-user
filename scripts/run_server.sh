#!/bin/bash

set -e

SERVER=$(realpath ${SERVER:-build-x86/server/vhost-server})
VHOST_SOCK=$(realpath ${VHOST_SOCK:-/tmp/vhost.sock})

[ -S $VHOST_SOCK ] && rm -f $VHOST_SOCK

$SERVER $VHOST_SOCK
