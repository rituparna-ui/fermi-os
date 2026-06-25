#!/usr/bin/env bash
# Build a user program and copy it onto the FAT32 disk as <NAME>.ELF.
# Usage: user/build.sh user/hello.rs [build/disk.img]
set -euo pipefail
SRC="${1:?usage: build.sh <src.rs> [disk.img]}"
DISK="${2:-build/disk.img}"
NAME="$(basename "${SRC%.rs}" | tr '[:lower:]' '[:upper:]')"
DIR="$(dirname "$0")"
OUT="$(mktemp -d)/$NAME.elf"
rustc --target aarch64-unknown-none -O -C panic=abort \
    -C link-arg=-T"$DIR/user.ld" -C relocation-model=static -o "$OUT" "$SRC"
STRIP="$(find "$(rustc --print sysroot)" -name llvm-strip | head -1)"
[ -n "$STRIP" ] && "$STRIP" --strip-all "$OUT"
MTOOLS_SKIP_CHECK=1 mcopy -o -i "$DISK" "$OUT" "::/$NAME.ELF"
echo "exec /mnt/fat32/$NAME.ELF"
