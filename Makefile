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

# Build a standalone aarch64 ET_EXEC user binary for the ELF loader to run.
build/hello.elf: user/hello.S
	@mkdir -p build
	clang --target=aarch64-unknown-none -c user/hello.S -o build/hello.o
	ld.lld -e _start --image-base=0x400000 -z max-page-size=0x1000 --no-rosegment build/hello.o -o build/hello.elf

build/echo.elf: user/echo.S
	@mkdir -p build
	clang --target=aarch64-unknown-none -c user/echo.S -o build/echo.o
	ld.lld -e _start --image-base=0x400000 -z max-page-size=0x1000 --no-rosegment build/echo.o -o build/echo.elf

$(DISK): build/hello.elf build/echo.elf
	@mkdir -p build
	dd if=/dev/zero of=$(DISK) bs=1M count=16 status=none
	mkfs.fat -F 32 -n FERMI $(DISK)
	printf 'Hello from FAT32 on Fermi OS (Rust)!\nThis file was read through the VFS.\n' > build/HELLO.TXT
	mcopy -o -i $(DISK) build/HELLO.TXT ::HELLO.TXT
	mcopy -o -i $(DISK) build/hello.elf ::HELLO.ELF
	mcopy -o -i $(DISK) build/echo.elf ::ECHO.ELF
	mmd -o -i $(DISK) ::DOCS
	printf 'Subdirectory file read via nested FAT32 traversal.\n' > build/SUB.TXT
	mcopy -o -i $(DISK) build/SUB.TXT ::DOCS/SUB.TXT

clean:
	cargo clean
	rm -rf build
