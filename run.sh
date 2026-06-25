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

# Create a FAT32 disk for virtio-blk if one doesn't exist. The kernel mounts it
# at /mnt/fat32 and reads HELLO.TXT.
if [ ! -f "$DISK_IMG" ]; then
	truncate -s 64M "$DISK_IMG"
	if command -v mkfs.fat >/dev/null 2>&1 && command -v mcopy >/dev/null 2>&1; then
		mkfs.fat -F 32 -n FERMI "$DISK_IMG" >/dev/null 2>&1
		printf 'Hello from Fermi OS FAT32!\nThis is HELLO.TXT.\n' \
			| MTOOLS_SKIP_CHECK=1 mcopy -i "$DISK_IMG" - ::/HELLO.TXT 2>/dev/null || true
	fi
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
