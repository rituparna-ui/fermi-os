#!/usr/bin/env bash
# Build the synthetic arm64 "Linux" Image (guest/synthimage.rs) into a flat
# binary that QEMU's generic loader can stage into the Fermi hypervisor's Linux
# slot. Pure Rust toolchain — rustc + rust-lld + llvm-objcopy, no GCC/binutils.
#
# Usage: guest/build-synthimage.sh [out.bin]   (default: build/synth-Image)
set -euo pipefail
DIR="$(cd "$(dirname "$0")" && pwd)"
OUT="${1:-$DIR/../build/synth-Image}"
mkdir -p "$(dirname "$OUT")"

ELF="$(mktemp /tmp/synthimage.XXXXXX.elf)"
trap 'rm -f "$ELF"' EXIT

rustc --target aarch64-unknown-none -O -C panic=abort \
	-C link-arg=-T"$DIR/synthimage.ld" -C relocation-model=static \
	-o "$ELF" "$DIR/synthimage.rs"

# Flatten ELF -> raw binary (what the arm64 boot protocol / QEMU loader want).
OBJCOPY="$(find "$(rustc --print sysroot)" -name llvm-objcopy | head -1)"
[ -n "$OBJCOPY" ] || { echo "llvm-objcopy not found in the Rust sysroot"; exit 1; }
"$OBJCOPY" -O binary "$ELF" "$OUT"

echo "built $OUT ($(wc -c < "$OUT") bytes)"
echo "magic @ +56:"
od -An -tx1 -j56 -N4 "$OUT" | tr -d ' '   # expect 41524d64 (LE 0x644d5241)
echo "stage with: -device loader,file=$OUT,addr=0x240200000,force-raw=on"
