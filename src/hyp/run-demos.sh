#!/usr/bin/env bash
# Full M1-M25 hypervisor regression: build the HYP_RUN_DEMOS image and run the
# whole self-test suite in one QEMU boot, then check every milestone passed.
#
# Run inside the osdev:dev container (QEMU 8.2.2 + aarch64 gcc), from the repo
# root, e.g.:
#   docker run --rm -v "$PWD":/work -w /work osdev:dev bash src/hyp/run-demos.sh
set -u

CF="-ffreestanding -g -nostdlib -nostartfiles -Wall -Wextra -O0 -mstrict-align \
-fno-pic -MMD -MP -DHYP_RUN_DEMOS -Isrc/lib -Isrc -Isrc/exception \
-Isrc/pci/virtio -Isrc/syscall -Isrc/fs -Isrc/devices"

make clean >/dev/null 2>&1
make CFLAGS="$CF" all >/dev/null 2>&1 || { echo "BUILD FAILED"; exit 1; }
make disk >/dev/null 2>&1
echo "build OK; booting full M1-M25 suite ..."

LOG=$(mktemp)
printf 'x\n' | timeout 90 qemu-system-aarch64 \
  -machine virt,gic-version=3,virtualization=on -m 8G -nographic -cpu max \
  -netdev user,id=n0 -device virtio-net-pci,netdev=n0,disable-legacy=on \
  -drive file=build/disk.img,if=none,format=raw,id=d0 \
  -device virtio-blk-pci,drive=d0,disable-legacy=on \
  -kernel build/kernel.elf > "$LOG" 2>&1

# Each expected marker -> the milestone it proves.
declare -a CHECKS=(
  "Current Exception Level: Hyper Space|M1 boot at EL2 (VHE)"
  "self-test OK: HVC routed to EL2|M2 EL2 trap plumbing"
  "M3 smoke test: PASS|M3 world-switch + stage-2"
  "M4 demo: PASS|M4 EL2-timer time-slicing"
  "M9 demo: PASS|M9 multi-guest round-robin"
  "psci test: post-reset beats=7 -> PASS|M11 PSCI reset"
  "M15: PASS|M15 inter-VM shared memory + doorbell"
  "M16: PASS|M16 interrupt-driven doorbell"
  "M17: done|M17 HVC hypercall ABI"
  "M19: done|M19 paravirt block"
  "M20: done|M20 paravirt net"
  "M21: PASS|M21 VM lifecycle (no leak)"
  "ncpus=2 bootargs=|M24 OS-grade DTB consumed by foreign guest"
  "M22: done|M22 foreign (non-FermiOS) guest"
  "M23: done|M23 SMP guest"
  "M25: PASS|M25 fault isolation"
)

fail=0
for c in "${CHECKS[@]}"; do
  pat="${c%%|*}"; name="${c##*|}"
  if grep -qF -- "$pat" "$LOG"; then
    echo "  PASS  $name"
  else
    echo "  FAIL  $name   (missing: $pat)"
    fail=1
  fi
done

rm -f "$LOG"
if [ "$fail" -eq 0 ]; then
  echo "ALL MILESTONES PASS (M1-M25)"
else
  echo "REGRESSION DETECTED"
  exit 1
fi
