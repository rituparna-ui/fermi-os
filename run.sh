#!/usr/bin/env bash
# Cargo runner: boot the freshly-built kernel ELF in QEMU.
#
# Usage (via cargo): cargo run
# $1 is the path to the kernel ELF that cargo just built.
set -euo pipefail

KERNEL="${1:?usage: run.sh <kernel.elf>}"

QEMU_CPU=cortex-a72
QEMU_MACHINE="virt,gic-version=3"
QEMU_MEM=8G

exec qemu-system-aarch64 \
	-machine "${QEMU_MACHINE}" \
	-cpu "${QEMU_CPU}" \
	-m "${QEMU_MEM}" \
	-nographic \
	-kernel "${KERNEL}"
