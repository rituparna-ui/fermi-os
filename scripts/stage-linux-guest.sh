#!/usr/bin/env bash
#
# stage-linux-guest.sh — fetch the Linux guest assets for the Fermi hypervisor.
#
# Produces (both gitignored):
#   guest/Image              — an SCS-free aarch64 Linux kernel Image
#   guest/initramfs.cpio.gz  — a static-busybox initramfs whose /init drops
#                              to a shell
#
# After running this, `make run` boots Fermi-the-hypervisor with the Linux
# kernel as its second guest (alongside Fermi itself), all the way to a
# BusyBox userspace shell.
#
# Run this on a machine with internet access (it downloads from
# ports.ubuntu.com). It only produces files under guest/; the aarch64
# build/run still happens via `make` in the build container per the README.
#
# Why an older kernel? Ubuntu's recent generic arm64 kernels enable
# CONFIG_SHADOW_CALL_STACK, which faults under this minimal boot. The 5.4
# generic kernel predates that default and boots cleanly.

set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
GUEST="$ROOT/guest"
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT
mkdir -p "$GUEST"

KDEB="linux-image-unsigned-5.4.0-99-generic_5.4.0-99.112_arm64.deb"
KURL="http://ports.ubuntu.com/ubuntu-ports/pool/main/l/linux/${KDEB}"
BBDIR="http://ports.ubuntu.com/ubuntu-ports/pool/main/b/busybox/"

# extract a .deb's data member (handles .xz / .zst / .gz) into $2
deb_extract() {
	local deb="$1" dest="$2" d; d="$(mktemp -d)"
	( cd "$d" && ar x "$deb" )
	mkdir -p "$dest"
	if   [ -f "$d/data.tar.zst" ]; then zstd -dqf "$d/data.tar.zst" -o "$d/data.tar"
	elif [ -f "$d/data.tar.xz"  ]; then xz -dc "$d/data.tar.xz"  > "$d/data.tar"
	elif [ -f "$d/data.tar.gz"  ]; then gzip -dc "$d/data.tar.gz" > "$d/data.tar"
	fi
	tar xf "$d/data.tar" -C "$dest"
	rm -rf "$d"
}

echo "[stage] downloading SCS-free kernel (5.4.0-99-generic)..."
curl -fsSL -o "$WORK/k.deb" "$KURL"
deb_extract "$WORK/k.deb" "$WORK/k"
echo "[stage] extracting Image..."
zcat "$WORK"/k/boot/vmlinuz-* > "$GUEST/Image"
if ! dd if="$GUEST/Image" bs=1 skip=56 count=4 2>/dev/null | grep -q "ARMd"; then
	echo "[stage] ERROR: $GUEST/Image is not a valid arm64 Image" >&2
	exit 1
fi
echo "[stage] guest/Image ready ($(stat -c %s "$GUEST/Image") bytes)"

echo "[stage] downloading static busybox..."
BBDEB="$(curl -fsSL "$BBDIR" | grep -oE 'busybox-static_[0-9][^"]*arm64\.deb' | sort -u | tail -1)"
curl -fsSL -o "$WORK/bb.deb" "${BBDIR}${BBDEB}"
deb_extract "$WORK/bb.deb" "$WORK/bb"
BB="$(find "$WORK/bb" -name busybox -type f | head -1)"
[ -n "$BB" ] || { echo "[stage] ERROR: busybox binary not found" >&2; exit 1; }

echo "[stage] fetching virtio-rng kernel module..."
KDIR="http://ports.ubuntu.com/ubuntu-ports/pool/main/l/linux/"
MODDEB="linux-modules-5.4.0-99-generic_5.4.0-99.112_arm64.deb"
curl -fsSL -o "$WORK/mod.deb" "${KDIR}${MODDEB}"
deb_extract "$WORK/mod.deb" "$WORK/mod"
VRNG_KO="$(find "$WORK/mod" -name 'virtio-rng.ko' | head -1)"
[ -n "$VRNG_KO" ] || { echo "[stage] ERROR: virtio-rng.ko not found" >&2; exit 1; }
VBLK_KO="$(find "$WORK/mod" -name 'virtio_blk.ko' | head -1)"
[ -n "$VBLK_KO" ] || { echo "[stage] ERROR: virtio_blk.ko not found" >&2; exit 1; }

echo "[stage] building initramfs..."
IR="$WORK/ir"
mkdir -p "$IR/bin" "$IR/proc" "$IR/sys" "$IR/dev"
cp "$BB" "$IR/bin/busybox"; chmod +x "$IR/bin/busybox"
cp "$VRNG_KO" "$IR/virtio-rng.ko"
cp "$VBLK_KO" "$IR/virtio_blk.ko"
cat > "$IR/init" <<'INIT'
#!/bin/busybox sh
/bin/busybox mount -t proc proc /proc 2>/dev/null
/bin/busybox mount -t sysfs sys /sys 2>/dev/null
/bin/busybox mount -t devtmpfs dev /dev 2>/dev/null
# Load the virtio drivers (CONFIG_HW_RANDOM_VIRTIO=m, CONFIG_VIRTIO_BLK=m);
# virtio_mmio and the rng core are built into the kernel, so plain insmods are
# enough. They bind to the hypervisor's emulated virtio-mmio devices.
/bin/busybox insmod /virtio-rng.ko 2>/dev/null && echo "[init] loaded virtio-rng driver"
/bin/busybox insmod /virtio_blk.ko 2>/dev/null && echo "[init] loaded virtio_blk driver"
echo ""
echo "==================================================="
echo "  Linux userspace is ALIVE on the Fermi hypervisor!"
echo "==================================================="
/bin/busybox uname -a
echo "guest0 = Fermi OS, guest1 = this Linux, both on Fermi-HV"
echo ""
# Respawn the shell so exiting it (e.g. EOF on the serial console) does not
# kill PID 1 and panic the kernel.
while true; do
	/bin/busybox sh
	echo "[init] shell exited; respawning..."
	/bin/busybox sleep 1
done
INIT
chmod +x "$IR/init"
( cd "$IR" && find . | cpio -o -H newc 2>/dev/null | gzip -9 > "$GUEST/initramfs.cpio.gz" )
echo "[stage] guest/initramfs.cpio.gz ready ($(stat -c %s "$GUEST/initramfs.cpio.gz") bytes)"

echo "[stage] done. Now: make run  (inside the build container)"
