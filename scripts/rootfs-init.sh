#!/bin/busybox sh
# /sbin/init for the ext4 ROOT filesystem on /dev/vda. Reached via switch_root
# from the initramfs (which loaded the virtio drivers before pivoting). The
# fact that this runs at all proves Linux is running from the hypervisor's
# emulated virtio-blk disk as its root device.
/bin/busybox mount -t proc proc /proc 2>/dev/null
/bin/busybox mount -t sysfs sys /sys 2>/dev/null
/bin/busybox mount -t devtmpfs dev /dev 2>/dev/null
echo ""
echo "==================================================="
echo "  ROOTFS_ON_VDA: running from the ext4 root on /dev/vda"
echo "==================================================="
/bin/busybox uname -a
/bin/busybox cat /etc/motd
echo ""
# Respawn the shell so exiting it does not kill PID 1 and panic the kernel.
while true; do
	/bin/busybox sh
	/bin/busybox sleep 1
done
