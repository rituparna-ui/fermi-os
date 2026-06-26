#!/usr/bin/env bash
# Build a minimal aarch64 Linux Image for booting under the FermiOS hypervisor
# as an EL1 guest (M28). Produces linux-image/Image (kept OUTSIDE build/ so
# `make clean` cannot wipe it).
#
# Reproduce from the repo root (fetch the tarball on the host — the build
# container has no outbound net tool — then build in the container):
#   mkdir -p linux-image && cp src/hyp/build-linux.sh linux-image/build.sh
#   curl -sL -o linux-image/linux.tar.xz \
#     https://cdn.kernel.org/pub/linux/kernel/v6.x/linux-6.6.58.tar.xz
#   docker run --rm -v "$PWD/linux-image":/img osdev:dev bash /img/build.sh
# Then build the hypervisor with the embedded Image + Linux run hook and boot:
#   docker run --rm -v "$PWD":/work -w /work osdev:dev bash -lc \
#     'make CFLAGS="<base CFLAGS> -DHYP_RUN_LINUX -DHAVE_LINUX_IMAGE" all && make disk'
#   qemu-system-aarch64 -machine virt,gic-version=3,virtualization=on -m 9G \
#     -nographic -cpu max -kernel build/kernel.elf
set -e
export DEBIAN_FRONTEND=noninteractive
export ARCH=arm64
export CROSS_COMPILE=aarch64-linux-gnu-

cd /img
echo "=== deps ==="
apt-get update -qq 2>&1 | tail -1
apt-get install -y -qq bc flex bison cpio xz-utils libssl-dev >/dev/null 2>&1
echo "deps OK"

if [ ! -f src/Makefile ]; then
  echo "=== extract ==="
  rm -rf src linux-6.6.58
  tar -xf linux.tar.xz
  mv linux-6.6.58 src
fi

cd src
echo "=== config: tinyconfig + minimal aarch64-virt boot options ==="
make tinyconfig >/dev/null 2>&1
cat >> .config <<EOF
CONFIG_PRINTK=y
CONFIG_TTY=y
CONFIG_SERIAL_AMBA_PL011=y
CONFIG_SERIAL_AMBA_PL011_CONSOLE=y
CONFIG_SERIAL_EARLYCON=y
CONFIG_OF=y
CONFIG_OF_EARLY_FLATTREE=y
CONFIG_ARM_ARCH_TIMER=y
CONFIG_ARM_GIC_V3=y
CONFIG_ARM_PSCI=y
CONFIG_CMDLINE="earlycon=pl011,0x09000000 console=ttyAMA0"
CONFIG_CMDLINE_FORCE=y
CONFIG_BLK_DEV_INITRD=n
EOF
make olddefconfig >/dev/null 2>&1
echo "config done"

echo "=== build Image (nproc=$(nproc)) ==="
make -j"$(nproc)" Image 2>&1 | tail -6
cp arch/arm64/boot/Image /img/Image
echo "=== IMAGE BUILT ==="
ls -la /img/Image
python3 - /img/Image <<'PY'
import sys,struct
d=open(sys.argv[1],'rb').read(64)
_,_,toff,isz,flags=struct.unpack_from('<IIQQQ',d,0)
print(f"hdr: text_offset={toff:#x} image_size={isz:#x} flags={flags:#x} magic={d[56:60]!r}")
PY
