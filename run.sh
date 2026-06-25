#!/usr/bin/env bash
# Cargo runner: boot the freshly-built kernel ELF in QEMU with the VirtIO
# device set the kernel drives (net, rng, blk, console, balloon).
#
# Usage (via cargo): cargo run
# $1 is the path to the kernel ELF that cargo just built.
set -euo pipefail

KERNEL="${1:?usage: run.sh <kernel.elf>}"

BUILD_DIR="$(dirname "$KERNEL")"
DISK_IMG="${BUILD_DIR}/disk.img"

# Create a small raw disk for virtio-blk if one doesn't exist.
if [ ! -f "$DISK_IMG" ]; then
	truncate -s 64M "$DISK_IMG"
fi

QEMU_CPU=cortex-a72
QEMU_MACHINE="virt,gic-version=3"
QEMU_MEM=8G

VCONS="${BUILD_DIR}/virtio-console.txt"

exec qemu-system-aarch64 \
	-machine "${QEMU_MACHINE}" \
	-cpu "${QEMU_CPU}" \
	-m "${QEMU_MEM}" \
	-nographic \
	-netdev user,id=n0 \
	-device virtio-net-pci,netdev=n0,disable-legacy=on \
	-device virtio-rng-pci,disable-legacy=on \
	-drive file="${DISK_IMG}",if=none,format=raw,id=d0 \
	-device virtio-blk-pci,drive=d0,disable-legacy=on \
	-chardev file,id=vc,path="${VCONS}",mux=off \
	-device virtio-serial-pci,disable-legacy=on \
	-device virtconsole,chardev=vc \
	-device virtio-balloon-pci,disable-legacy=on \
	-kernel "${KERNEL}"
