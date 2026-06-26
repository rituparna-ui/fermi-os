#!/usr/bin/env bash
# Build a minimal aarch64 Linux Image for booting under the FermiOS hypervisor
# as an EL1 guest. Produces linux-image/Image (kept OUTSIDE build/ so
# `make clean` cannot wipe it).
#
#   M28: bare kernel boots and parses our DTB.
#   M29: fuller vGIC -> kernel brings up GICv3 + arch timer and execs init.
#   M30: a BUILT-IN, UNCOMPRESSED initramfs carries a freestanding static
#        /init (no libc) so the kernel reaches USERSPACE, prints from PID 1,
#        and powers the VM off via PSCI SYSTEM_OFF (-> our hyp PSCI handler).
#
# The initramfs is built INTO the Image (CONFIG_INITRAMFS_SOURCE), so the
# hypervisor side needs no extra blob — the embedded Image simply grows.
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

# ---------------------------------------------------------------------------
# M30: freestanding static /init (no libc). It is exec'd by the kernel as PID 1
# directly from the built-in initramfs (the kernel prefers an initramfs /init
# over /sbin/init). It prints from userspace then powers the VM off via the
# reboot(2) POWER_OFF path, which with PSCI firmware (DTB /psci method="hvc")
# issues a PSCI SYSTEM_OFF HVC that traps to the hypervisor's PSCI handler.
# Syscall numbers / reboot magics verified against the on-disk 6.6.58 source:
#   write=64, reboot=142; MAGIC1=0xfee1dead MAGIC2=0x28121969 POWER_OFF=0x4321fedc
# ---------------------------------------------------------------------------
echo "=== build freestanding /init (userspace PID 1) ==="
cat > /img/init.c <<'INITC'
/* /init — freestanding aarch64 PID-1 for the FermiOS hypervisor Linux guest.
   No libc, no dynamic linker: the kernel jumps straight to _start, which must
   never return. Demonstrates a real mainline Linux reaching USERSPACE under
   the hypervisor, then powering off cleanly via PSCI. */
static long syscall5(long n, long a, long b, long c, long d) {
  register long x8 __asm__("x8") = n;
  register long x0 __asm__("x0") = a;
  register long x1 __asm__("x1") = b;
  register long x2 __asm__("x2") = c;
  register long x3 __asm__("x3") = d;
  __asm__ __volatile__("svc #0"
    : "+r"(x0)
    : "r"(x8), "r"(x1), "r"(x2), "r"(x3)
    : "memory", "cc");
  return x0;
}
#define SYS_write  64
#define SYS_reboot 142
static unsigned long slen(const char *s) { unsigned long n = 0; while (s[n]) n++; return n; }
static void put(const char *s) { syscall5(SYS_write, 1, (long)s, (long)slen(s), 0); }
void _start(void) {
  put("\n");
  put("[init] ===============================================================\n");
  put("[init]  Hello from PID 1 - USERSPACE is ALIVE under the FermiOS hyp!\n");
  put("[init]  A real mainline Linux 6.6 kernel booted as an EL1 guest,\n");
  put("[init]  unpacked the built-in initramfs, and exec'd me - a freestanding\n");
  put("[init]  static /init (no libc) - as its first userspace process.\n");
  put("[init] ===============================================================\n");
  put("[init] powering the VM off via PSCI SYSTEM_OFF (traps to the hypervisor)\n");
  /* reboot(MAGIC1, MAGIC2, LINUX_REBOOT_CMD_POWER_OFF, NULL) */
  syscall5(SYS_reboot, 0xfee1deadL, 0x28121969L, 0x4321fedcL, 0);
  /* Unreachable on success. Spin quietly (no console spam) if it ever returns. */
  put("[init] power-off returned unexpectedly - halting\n");
  for (;;) { __asm__ __volatile__("nop"); }
}
INITC
aarch64-linux-gnu-gcc -static -nostdlib -no-pie -ffreestanding \
  -fno-stack-protector -fno-asynchronous-unwind-tables -Os \
  /img/init.c -o /img/init
aarch64-linux-gnu-strip /img/init || true
echo "init built:"; ls -la /img/init
# Sanity: must be a static ET_EXEC with no PT_INTERP (no dynamic linker).
aarch64-linux-gnu-readelf -hl /img/init | grep -E "Type:|INTERP" || true

# gen_init_cpio spec (field order verified vs usr/gen_init_cpio.c):
#   dir  <name> <mode> <uid> <gid>
#   nod  <name> <mode> <uid> <gid> <c|b> <maj> <min>
#   file <name> <location> <mode> <uid> <gid>
# /dev/console (c 5 1) is the device the kernel wires to init's fd 0/1/2.
cat > /img/initramfs.list <<'LIST'
dir /dev 0755 0 0
nod /dev/console 0600 0 0 c 5 1
file /init /img/init 0755 0 0
LIST
echo "initramfs.list:"; cat /img/initramfs.list

cd src
echo "=== config: tinyconfig + minimal aarch64-virt boot + built-in initramfs ==="
make tinyconfig >/dev/null 2>&1
cat >> .config <<EOF
CONFIG_PRINTK=y
CONFIG_TTY=y
CONFIG_BINFMT_ELF=y
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
CONFIG_BLK_DEV_INITRD=y
CONFIG_INITRAMFS_SOURCE="/img/initramfs.list"
CONFIG_INITRAMFS_COMPRESSION_NONE=y
EOF
make olddefconfig >/dev/null 2>&1
echo "config done; key options:"
grep -E "^CONFIG_(BINFMT_ELF|BLK_DEV_INITRD|INITRAMFS_SOURCE|INITRAMFS_COMPRESSION_NONE)=" .config || true

echo "=== build Image (nproc=$(nproc)) ==="
make -j"$(nproc)" Image 2>&1 | tail -8
cp arch/arm64/boot/Image /img/Image
echo "=== IMAGE BUILT ==="
ls -la /img/Image
python3 - /img/Image <<'PY'
import sys,struct
d=open(sys.argv[1],'rb').read(64)
_,_,toff,isz,flags=struct.unpack_from('<IIQQQ',d,0)
print(f"hdr: text_offset={toff:#x} image_size={isz:#x} flags={flags:#x} magic={d[56:60]!r}")
PY
