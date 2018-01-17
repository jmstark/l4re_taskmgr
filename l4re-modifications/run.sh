#!/bin/sh
QEMU_OPTIONS="-net socket,listen=:8010" \
MAC="52:54:00:00:00:01" \
make qemu E=dom0_server_only &

sleep 1

QEMU_OPTIONS="-net socket,connect=127.0.0.1:8010" \
MAC="52:54:00:00:00:02" \
make qemu E=dom0_server_and_client &
