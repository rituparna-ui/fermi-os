#!/usr/bin/env bash
# Headless QEMU boot smoke-test: build a FAT32 disk, boot the kernel, and assert
# that the key subsystem milestones appear in the output and nothing panics.
# Exits non-zero on any failure. Used by CI and runnable locally.
#
# Usage: ci/smoke-test.sh [path-to-kernel-elf]
set -euo pipefail

KERNEL="${1:-target/aarch64-unknown-none/debug/kernel}"
DISK="$(mktemp /tmp/fermi-ci-disk.XXXXXX.img)"
VCONS="$(mktemp /tmp/fermi-ci-vcons.XXXXXX.txt)"
LOG="$(mktemp /tmp/fermi-ci-boot.XXXXXX.log)"
trap 'rm -f "$DISK" "$VCONS" "$LOG"' EXIT

# --- build a FAT32 disk with HELLO.TXT ---
truncate -s 64M "$DISK"
mkfs.fat -F 32 -n FERMI "$DISK" >/dev/null
printf 'Hello from Fermi OS FAT32!\nThis is HELLO.TXT.\n' \
	| MTOOLS_SKIP_CHECK=1 mcopy -i "$DISK" - ::/HELLO.TXT

# --- boot headless with the full VirtIO device set ---
echo "Booting $KERNEL ..."
timeout 25 qemu-system-aarch64 \
	-machine virt,gic-version=3 -cpu cortex-a72 -m 8G -nographic \
	-netdev user,id=n0 -device virtio-net-pci,netdev=n0,disable-legacy=on \
	-device virtio-rng-pci,disable-legacy=on \
	-drive file="$DISK",if=none,format=raw,id=d0 \
	-device virtio-blk-pci,drive=d0,disable-legacy=on \
	-chardev file,id=vc,path="$VCONS",mux=off \
	-device virtio-serial-pci,disable-legacy=on -device virtconsole,chardev=vc \
	-device virtio-balloon-pci,disable-legacy=on \
	-kernel "$KERNEL" < /dev/null > "$LOG" 2>&1 || true

echo "----- boot log (tail) -----"
tail -n 40 "$LOG"
echo "---------------------------"

# --- assertions ---
fail=0

require() {
	if grep -qF "$1" "$LOG"; then
		echo "  ok: $1"
	else
		echo "  MISSING: $1"
		fail=1
	fi
}

echo "Checking required milestones:"
require "[MMU TEST] MMU Enabled: PASS"
require "[MMU TEST] TTBR1 Upper Half: PASS"
require "[HEAP TEST] coalesce + realloc: PASS"
require "[EXC TEST] Survived BRK"
require "[RNG TEST] got 16 bytes"
require "[BLK TEST] write+read sector 1 round-trip: PASS"
require "[DHCP] Lease ACK"
require "PING reply from 10.0.2.2"
require "[FAT32 TEST] /mnt/fat32/HELLO.TXT"
require "[KERNEL] Ready!"

echo "Checking for failures:"
if grep -qiE 'KERNEL PANIC|RUST PANIC| FAIL' "$LOG"; then
	echo "  PANIC/FAIL detected:"
	grep -iE 'KERNEL PANIC|RUST PANIC| FAIL' "$LOG" | sed 's/^/    /'
	fail=1
else
	echo "  ok: no panics / FAILs"
fi

if [ "$fail" -ne 0 ]; then
	echo "SMOKE TEST FAILED"
	exit 1
fi
echo "SMOKE TEST PASSED"
