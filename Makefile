# Fermi OS (Rust) — thin wrapper around cargo + QEMU.
# The kernel is pure Rust + aarch64 assembly; cargo drives the actual build
# (see .cargo/config.toml for the target and QEMU runner).

DISK := build/disk.img

.PHONY: all build run debug disk clean

all: build

build:
	cargo build

# `cargo run` uses the runner in .cargo/config.toml, which attaches the
# virtio rng/blk/net/console/balloon devices and the FAT32 disk.
run: disk
	cargo run

# Build a 16 MiB FAT32 disk seeded with HELLO.TXT (needs mkfs.fat + mtools).
disk: $(DISK)

$(DISK):
	@mkdir -p build
	dd if=/dev/zero of=$(DISK) bs=1M count=16 status=none
	mkfs.fat -F 32 -n FERMI $(DISK)
	printf 'Hello from FAT32 on Fermi OS (Rust)!\nThis file was read through the VFS.\n' > build/HELLO.TXT
	mcopy -i $(DISK) build/HELLO.TXT ::HELLO.TXT

clean:
	cargo clean
	rm -rf build
