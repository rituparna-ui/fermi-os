#!/usr/bin/env bash
# Boot the kernel in QEMU headless with the full VirtIO device set, capture
# output, and exit after a timeout. Usage: ./test-boot.sh [timeout_secs]
set -euo pipefail

TIMEOUT="${1:-8}"
KERNEL="target/aarch64-unknown-none/debug/kernel"
DISK="/tmp/fermi_disk.img"
VCONS="/tmp/fermi_vcons.txt"

[ -f "$DISK" ] || truncate -s 64M "$DISK"
rm -f "$VCONS"

timeout "$TIMEOUT" qemu-system-aarch64 \
	-machine virt,gic-version=3 -cpu cortex-a72 -m 8G -nographic \
	-netdev user,id=n0 -device virtio-net-pci,netdev=n0,disable-legacy=on \
	-device virtio-rng-pci,disable-legacy=on \
	-drive file="$DISK",if=none,format=raw,id=d0 \
	-device virtio-blk-pci,drive=d0,disable-legacy=on \
	-chardev file,id=vc,path="$VCONS",mux=off \
	-device virtio-serial-pci,disable-legacy=on -device virtconsole,chardev=vc \
	-device virtio-balloon-pci,disable-legacy=on \
	-kernel "$KERNEL" < /dev/null 2>&1 || true
