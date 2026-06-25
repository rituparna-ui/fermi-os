#!/usr/bin/env bash
# Boot the kernel in QEMU headless with the full VirtIO device set, capture
# output, and exit after a timeout. Usage: ./test-boot.sh [timeout_secs]
set -euo pipefail

TIMEOUT="${1:-8}"
KERNEL="target/aarch64-unknown-none/debug/kernel"
DISK="/tmp/fermi_disk.img"
VCONS="/tmp/fermi_vcons.txt"

# Build a FAT32 disk with a couple of test files if one doesn't exist (or if
# REFORMAT=1). The FAT32 mount + file-read test in kmain expects HELLO.TXT.
if [ ! -f "$DISK" ] || [ "${REFORMAT:-0}" = "1" ]; then
	rm -f "$DISK"
	truncate -s 64M "$DISK"
	if command -v mkfs.fat >/dev/null 2>&1 && command -v mcopy >/dev/null 2>&1; then
		mkfs.fat -F 32 -n FERMI "$DISK" >/dev/null 2>&1
		printf 'Hello from Fermi OS FAT32!\nThis is HELLO.TXT.\n' \
			| MTOOLS_SKIP_CHECK=1 mcopy -i "$DISK" - ::/HELLO.TXT
		MTOOLS_SKIP_CHECK=1 mmd -i "$DISK" ::/SUBDIR 2>/dev/null || true
		printf 'Inside a subdirectory.\n' \
			| MTOOLS_SKIP_CHECK=1 mcopy -i "$DISK" - ::/SUBDIR/INFO.TXT 2>/dev/null || true
	fi
fi
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
