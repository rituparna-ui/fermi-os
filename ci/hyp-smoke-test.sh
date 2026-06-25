#!/usr/bin/env bash
# Hypervisor (EL2) boot smoke-test. Boots the kernel with QEMU's
# `virtualization=on` so the image is entered at EL2: boot.S brings up the
# hypervisor (hyp_init), enables stage-2 translation, and erets down to EL1,
# where the full Fermi kernel runs as a stage-2-translated guest and must reach
# all its usual milestones with no EL2 trap / panic.
#
# Stage-2 translation needs a QEMU with complete TCG stage-2 support. The host
# QEMU 3.1.0 faults at stage-2 level 0 on the eret to EL1 (an emulator
# limitation, not a kernel bug — EL2 bring-up itself succeeds there). So this
# test runs QEMU >= 8 from the `osdev:dev` Docker image when the host QEMU is
# too old. Set HYP_QEMU to point at a new-enough qemu-system-aarch64 to skip
# Docker.
#
# Usage: ci/hyp-smoke-test.sh [path-to-kernel-elf]
set -euo pipefail

KERNEL="${1:-target/aarch64-unknown-none/debug/kernel}"
DOCKER_IMG="${HYP_DOCKER_IMG:-osdev:dev}"

if [ ! -f "$KERNEL" ]; then
	echo "kernel not found: $KERNEL (run: cargo build)"
	exit 1
fi

DISK="$(mktemp /tmp/fermi-hyp-disk.XXXXXX.img)"
LOG="$(mktemp /tmp/fermi-hyp-boot.XXXXXX.log)"
trap 'rm -f "$DISK" "$LOG"' EXIT

# FAT32 disk with HELLO.TXT (same as the EL1 smoke test) so the guest's BLK +
# FAT32 round-trips have a backing store.
truncate -s 64M "$DISK"
mkfs.fat -F 32 -n FERMI "$DISK" >/dev/null
printf 'Hello from Fermi OS FAT32!\nThis is HELLO.TXT.\n' \
	| MTOOLS_SKIP_CHECK=1 mcopy -i "$DISK" - ::/HELLO.TXT

# QEMU args shared by both the direct and Docker paths. virtualization=on is the
# whole point: it makes QEMU enter the image at EL2.
qemu_args() {
	local kernel="$1" disk="$2"
	echo "-machine virt,gic-version=3,virtualization=on -cpu cortex-a72 -m 8G -nographic \
		-netdev user,id=n0 -device virtio-net-pci,netdev=n0,disable-legacy=on \
		-device virtio-rng-pci,disable-legacy=on \
		-drive file=$disk,if=none,format=raw,id=d0 \
		-device virtio-blk-pci,drive=d0,disable-legacy=on \
		-device virtio-balloon-pci,disable-legacy=on \
		-kernel $kernel"
}

# Pick a runner: a new-enough host QEMU, else Docker.
host_qemu_major() {
	command -v qemu-system-aarch64 >/dev/null 2>&1 || { echo 0; return; }
	qemu-system-aarch64 --version 2>/dev/null | sed -n 's/^QEMU emulator version \([0-9]*\).*/\1/p' | head -1
}

echo "Booting $KERNEL under virtualization=on ..."
if [ -n "${HYP_QEMU:-}" ]; then
	echo "  runner: \$HYP_QEMU = $HYP_QEMU"
	# shellcheck disable=SC2046
	timeout 45 "$HYP_QEMU" $(qemu_args "$KERNEL" "$DISK") < /dev/null > "$LOG" 2>&1 || true
elif [ "$(host_qemu_major)" -ge 8 ]; then
	echo "  runner: host qemu-system-aarch64 ($(qemu-system-aarch64 --version | head -1))"
	# shellcheck disable=SC2046
	timeout 45 qemu-system-aarch64 $(qemu_args "$KERNEL" "$DISK") < /dev/null > "$LOG" 2>&1 || true
elif command -v docker >/dev/null 2>&1; then
	echo "  runner: docker $DOCKER_IMG (host qemu too old for stage-2)"
	# shellcheck disable=SC2046
	timeout 90 docker run --rm -v "$PWD":/work -v "$DISK":/disk.img -w /work "$DOCKER_IMG" \
		timeout 45 qemu-system-aarch64 $(qemu_args "$KERNEL" /disk.img) < /dev/null > "$LOG" 2>&1 || true
else
	echo "SKIP: no QEMU >= 8 and no docker; cannot exercise stage-2 translation."
	echo "      (EL2 bring-up itself works on any QEMU; set HYP_QEMU or install docker to test the full guest boot.)"
	exit 0
fi

echo "----- boot log (tail) -----"
tail -n 30 "$LOG"
echo "---------------------------"

fail=0
require() {
	if grep -qF "$1" "$LOG"; then
		echo "  ok: $1"
	else
		echo "  MISSING: $1"
		fail=1
	fi
}

echo "Checking hypervisor + guest milestones:"
require "[HYP] Fermi hypervisor online at EL2"
require "[HYP] stage-2 enabled (HCR_EL2.VM=1), dropping to EL1 guest"
require "[MMU TEST] TTBR1 Upper Half: PASS"
require "[BLK TEST] write+read sector 1 round-trip: PASS"
require "[FAT32 TEST] create+read RUSTW.TXT round-trip: PASS"
require "[FORK STRESS] parent forked 16 children PASS"
require "[ASID WRAP] PASS"
require "[KERNEL] Ready!"

echo "Checking for EL2 traps / panics:"
# An EL2 trap after the "dropping to EL1 guest" line means the guest faulted
# into the hypervisor unexpectedly (e.g. stage-2 hole) — a real failure.
if grep -qiE 'KERNEL PANIC|RUST PANIC|\[HYP\] \*\*\* EL2 trap| FAIL' "$LOG"; then
	echo "  EL2 trap / panic / FAIL detected:"
	grep -iE 'KERNEL PANIC|RUST PANIC|\[HYP\] \*\*\* EL2 trap| FAIL' "$LOG" | sed 's/^/    /'
	fail=1
else
	echo "  ok: no EL2 traps / panics / FAILs"
fi

if [ "$fail" -ne 0 ]; then
	echo "HYP SMOKE TEST FAILED"
	exit 1
fi
echo "HYP SMOKE TEST PASSED"
