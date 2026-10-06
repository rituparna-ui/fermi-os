# Toolchain
CROSS_COMPILE = aarch64-linux-gnu-

CC := $(CROSS_COMPILE)gcc
AS := $(CROSS_COMPILE)as
LD := $(CROSS_COMPILE)ld
# Must be the cross ar: the host's (macOS) tools reject ELF aarch64 members with
# "not a mach-o file". `s` already writes the symbol index, so no separate
# ranlib pass is needed.
AR := $(CROSS_COMPILE)ar

# Directories
SRC_DIR := src
BUILD_DIR := build
USER_DIR  := user
TARGET := $(BUILD_DIR)/kernel.elf

# File discovery - find all .c and .S files in src
S_SOURCES := $(shell find $(SRC_DIR) -name "*.S")
C_SOURCES := $(shell find $(SRC_DIR) -name "*.c")

# Object File Mapping
# src/boot.S      -> build/boot.o
# src/kernel.c  	-> build/kernel.o
S_OBJECTS := $(patsubst $(SRC_DIR)/%.S, $(BUILD_DIR)/%.o, $(S_SOURCES))
C_OBJECTS := $(patsubst $(SRC_DIR)/%.c, $(BUILD_DIR)/%.o, $(C_SOURCES))
OBJECTS := $(S_OBJECTS) $(C_OBJECTS)
DEPS    := $(OBJECTS:.o=.d)

# User-space binaries packaged onto the FAT32 disk and exec()'d at runtime.
# Each user/<name>.S compiles into user/<name>.elf (linked at USER_TEXT_BASE
# = 0x400000, _start as entry) and is then objcopy'd into a flat user/<name>.bin.
USER_S_SOURCES := $(wildcard $(USER_DIR)/*.S)
USER_C_SOURCES := $(wildcard $(USER_DIR)/*.c)
# Disk now ships ELF directly — the kernel parses ET_EXEC + walks PT_LOADs.
USER_BINS      := $(USER_S_SOURCES:.S=.elf) $(USER_C_SOURCES:.c=.elf)
USER_CRT0      := $(USER_DIR)/lib/crt0.o
USER_INCLUDE   := -I $(USER_DIR)/include

# Fermi libc. Every user/lib/*.c compiles with the same freestanding flags as
# user programs and is archived into libc.a, which user binaries link against.
# Deliberately a real archive rather than more header-only statics: a libc
# needs ONE definition per function, and header-only inline code silently
# duplicates state — a header-only malloc counter or errno would become a
# per-translation-unit copy rather than one per process.
USER_LIB_SOURCES := $(wildcard $(USER_DIR)/lib/*.c)
USER_LIB_OBJECTS := $(USER_LIB_SOURCES:$(USER_DIR)/lib/%.c=$(USER_DIR)/lib/%.o)
USER_LIBC       := $(USER_DIR)/lib/libc.a

# A static Linux binary, built by the Linux toolchain and linked against NOTHING
# -- no crt0.o, no libc.a. It speaks Linux syscall numbers and the Linux process
# startup contract, and exists to prove the kernel can host it. Listed apart
# from USER_BINS on purpose: the wildcard rules below would link the Fermi libc
# into it and quietly turn the test into another Fermi binary.
LINUX_DIR       := $(USER_DIR)/linux
LINUX_BIN       := $(LINUX_DIR)/linuxhello.elf
# Stem relative to USER_DIR, because the disk-copy loop builds the path as
# "$(USER_DIR)/$$stem.elf". A stem (not a filename) is what it expects.
LINUX_BIN_REL   := linux/linuxhello

# On-disk name overrides, keyed by user-program stem.
#
# This kernel's FAT32 has NO long-filename support: mcopy stores any name that
# doesn't fit 8.3 under a generated short name ("ERRNOT~1"), and a later lookup
# by the original name then fails. So a user binary whose stem exceeds 8
# characters must be given a short on-disk alias here. Overriding explicitly is
# better than truncating automatically, because the kernel-side exec path has
# to name the same file and a silent mismatch is a confusing runtime failure.
DISKNAME_errno    = ERRNO
DISKNAME_libctest = LIBCT
DISKNAME_stdiotest = STDIO
DISKNAME_proctest = PROCT
# Stem "linuxhello" exceeds 8 characters and this FAT32 has no long-filename
# support, so it ships as LINHELLO.
DISKNAME_linuxhello = LINHELLO

# Every user binary's on-disk name, listed explicitly rather than derived from
# the source name. A derived name is one refactor away from silently breaking
# the kernel-side exec() path, which has to spell the same name.
DISKNAME_hello    = HELLO
DISKNAME_cat      = CAT
DISKNAME_counter  = COUNTER

USER_STEMS     := $(USER_BINS:$(USER_DIR)/%.elf=%)
USER_DISK_PAIRS := $(foreach s,$(USER_STEMS),$(DISKNAME_$s):$s) $(DISKNAME_linuxhello):$(LINUX_BIN_REL)

# Flags shared by user-space compilation (crt0, libc members, user programs).
USER_CFLAGS := -ffreestanding -nostartfiles -nostdlib -fno-pic -static \
               -Wall -Wextra -O0 -g $(USER_INCLUDE)

# Flags
CFLAGS := -ffreestanding -g -nostdlib -nostartfiles -Wall -Wextra -O0 -mstrict-align -fno-pic -MMD -MP \
					-I $(SRC_DIR)/lib \
					-I $(SRC_DIR) \
					-I $(SRC_DIR)/exception \
					-I $(SRC_DIR)/pci/virtio \
					-I $(SRC_DIR)/syscall \
					-I $(SRC_DIR)/fs \
					-I $(SRC_DIR)/devices
# Assemble .S through the C preprocessor so future #include / #define expand correctly
ASFLAGS := -g -MMD -MP
LDFLAGS := -nostdlib -g -T linker.ld

# QEMU Config
DISK_IMG := $(BUILD_DIR)/disk.img
DISK_SIZE := 1G

QEMU_CPU := cortex-a72
QEMU_MACHINE := virt,gic-version=3 -m 8G
QEMU_DEVICES := -netdev user,id=n0 \
	-device virtio-net-pci,netdev=n0,disable-legacy=on \
	-device virtio-rng-pci,disable-legacy=on \
	-drive file=$(DISK_IMG),if=none,format=raw,id=d0 \
	-device virtio-blk-pci,drive=d0,disable-legacy=on \
	-chardev file,id=vc,path=$(BUILD_DIR)/virtio-console.txt,mux=off \
	-device virtio-serial-pci,disable-legacy=on \
	-device virtconsole,chardev=vc \
	-device virtio-balloon-pci,disable-legacy=on
# QEMU_MACHINE := virt,gic-version=3,virtualization=on -m 8G
# QEMU_MACHINE := virt,gic-version=3,virtualization=on,secure=on -m 8G
QEMU_BASE := qemu-system-aarch64 -machine $(QEMU_MACHINE) -nographic -cpu $(QEMU_CPU) $(QEMU_DEVICES)

QEMU_FLAGS_RUN   := -kernel $(TARGET)
QEMU_FLAGS_DEBUG := -kernel $(TARGET) -s -S

.PHONY: all full run debug clean gdb tmux disk dump_dts compile_commands.json libc user_bins


all: $(TARGET)

# Kernel + every user program + the disk image.
#
# `make` alone builds only the kernel, which silently leaves stale user ELFs
# on the disk image: editing user/lib/*.c relinks libc.a, but nothing rebuilds
# the disk unless you ask. The symptom is a fix that appears not to work. The
# `full` target makes the complete tree the default behaviour.
full: all user_bins disk

# TARGET depends on all .o files
$(TARGET): $(OBJECTS)
	@echo "LD  $@"
	@mkdir -p $(dir $@)
	@$(LD) $(LDFLAGS) -o $@ $^

# Compile all .c files
$(BUILD_DIR)/%.o: $(SRC_DIR)/%.c
	@echo "CC $<"
	@mkdir -p $(dir $@)
	@$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/%.o: $(SRC_DIR)/%.S
	@echo "AS  $<"
	@mkdir -p $(dir $@)
	@$(CC) $(CFLAGS) -x assembler-with-cpp -c $< -o $@

# User-mode program build rules. Linked at USER_TEXT_BASE = 0x400000 to match
# what the kernel maps in every EL0 task, then objcopy'd to a flat binary so
# the kernel just memcpy's the bytes into freshly-allocated user pages.
# Assembly user programs are self-contained (their own _start, no crt0).
$(USER_DIR)/%.elf: $(USER_DIR)/%.S
	@echo "USER GCC $<"
	@$(CC) $(USER_CFLAGS) \
		-Wl,-Ttext=0x400000 -Wl,-e,_start -Wl,--build-id=none \
		-o $@ $<

# C user programs link with crt0.o which provides _start and the
# main->SYS_EXIT trampoline. crt0 is the FIRST input so the linker
# places .text._start at the start of the output — binaries are loaded
# at USER_TEXT_BASE = 0x400000 and the kernel jumps straight there.
#
# libc.a comes AFTER the program object so the linker resolves a libc
# symbol only if the program didn't define it itself (which would be an
# error worth hearing about, not silently override).
$(USER_DIR)/%.elf: $(USER_DIR)/%.c $(USER_CRT0) $(USER_LIBC)
	@echo "USER GCC $<"
	@$(CC) $(USER_CFLAGS) \
		-Wl,-Ttext=0x400000 -Wl,-e,_start -Wl,--build-id=none \
		-o $@ $(USER_CRT0) $< $(USER_LIBC)

# The foreign Linux binary.
#
# Flags that differ from USER_CFLAGS, and why each is needed:
#   -fno-tree-loop-distribute-patterns  stops GCC rewriting our byte-copy loops
#                                       into calls to memcpy/memset, which do
#                                       not exist here. Without it the link
#                                       fails on undefined memcpy.
#   -fno-stack-protector               otherwise the canary calls
#                                       __stack_chk_fail, which does not exist.
#   -Wl,-e,_start                      the Linux entry point, not Fermi's crt0.
# No crt0.o and no libc.a on the link line: that is the entire point.
$(LINUX_BIN): $(LINUX_DIR)/main.c $(LINUX_DIR)/start.S
	@echo "LINUX GCC $< (static Linux binary, no Fermi libc)"
	@$(CC) -ffreestanding -nostartfiles -nostdlib -static -fno-pic \
		-fno-stack-protector -fno-tree-loop-distribute-patterns \
		-fno-builtin -fno-asynchronous-unwind-tables \
		-Wall -Wextra -O0 -g \
		-Wl,-Ttext=0x400000 -Wl,-e,_start -Wl,--build-id=none \
		-o $@ $(LINUX_DIR)/start.S $(LINUX_DIR)/main.c

$(USER_CRT0): $(USER_DIR)/lib/crt0.S
	@echo "USER AS $<"
	@mkdir -p $(dir $@)
	@$(CC) -ffreestanding -nostartfiles -nostdlib -fno-pic -c -o $@ $<

# libc members. -MMD/-MP so editing a header rebuilds every object that
# includes it — a stale libc.a silently missing a symbol change is a
# miserable class of bug.
$(USER_DIR)/lib/%.o: $(USER_DIR)/lib/%.c
	@echo "LIBC CC $<"
	@mkdir -p $(dir $@)
	@$(CC) $(USER_CFLAGS) -MMD -MP -c $< -o $@

$(USER_LIBC): $(USER_LIB_OBJECTS)
	@echo "LIBC AR $@"
	@rm -f $@
	@$(AR) rcs $@ $(USER_LIB_OBJECTS)

# objcopy step removed — the kernel now loads real ELF files. Keeping the
# rule scaffolding here would reintroduce confusion about which artifact
# ships on disk.

user_bins: $(USER_BINS) $(LINUX_BIN)

# Build libc.a on its own, for iterating on the libc without relinking users.
libc: $(USER_LIBC)


# Run QEMU
run: all disk
	@$(QEMU_BASE) $(QEMU_FLAGS_RUN)

debug: all disk
	@$(QEMU_BASE) $(QEMU_FLAGS_DEBUG)

disk: $(DISK_IMG)

# Every shipped binary is a prerequisite, including the foreign Linux one.
# Leaving it out lets `make full` report success while the disk image silently
# keeps the previous contents -- the exact failure `full` exists to prevent.
$(DISK_IMG): $(USER_BINS) $(LINUX_BIN)
	@mkdir -p $(BUILD_DIR)
	@echo "Creating $(DISK_IMG) ($(DISK_SIZE) sparse, FAT32)"
	@truncate -s $(DISK_SIZE) $@
	@mkfs.fat -F 32 -n FERMI $@ > /dev/null
	@printf 'Hello from Fermi OS!\nThis is HELLO.TXT on a FAT32 volume.\n' \
		| MTOOLS_SKIP_CHECK=1 mcopy -i $@ - ::/HELLO.TXT
	@printf '\336\255\276\357\312\376\272\276' \
		| MTOOLS_SKIP_CHECK=1 mcopy -i $@ - ::/DATA.BIN
	@MTOOLS_SKIP_CHECK=1 mmd -i $@ ::/SUBDIR
	@printf 'Hello from a subdirectory!\n' \
		| MTOOLS_SKIP_CHECK=1 mcopy -i $@ - ::/SUBDIR/INFO.TXT
	@for pair in $(USER_DISK_PAIRS); do \
		name=$${pair%%:*}; \
		stem=$${pair##*:}; \
		echo "  + $$name.ELF <- $(USER_DIR)/$$stem.elf"; \
		MTOOLS_SKIP_CHECK=1 mcopy -i $@ $(USER_DIR)/$$stem.elf ::/$$name.ELF; \
	done

# GDB Config
GDB := gdb-multiarch
GDB_FLAGS := -ex "target remote :1234" -ex "layout split"
GDB_CMD := $(GDB) $(TARGET) $(GDB_FLAGS)

gdb:
	@$(GDB_CMD)

tmux: all
	tmux new-session -d -s debug \
  "$(QEMU_BASE) $(QEMU_FLAGS_DEBUG)" \; \
  split-window -h '$(GDB_CMD)' \; \
  attach

compile_commands.json: $(C_SOURCES)
	@echo "Generating compile_commands.json..."
	@echo "[" > $@
	@first=true; \
	for src in $(C_SOURCES); do \
		if [ "$$first" = true ]; then first=false; else echo "," >> $@; fi; \
		echo "  {" >> $@; \
		echo "    \"directory\": \"$(CURDIR)\"," >> $@; \
		echo "    \"command\": \"$(CC) $(CFLAGS) -c $$src\"," >> $@; \
		echo "    \"file\": \"$$src\"" >> $@; \
		echo "  }" >> $@; \
	done
	@echo "]" >> $@

dump_dts:
	$(QEMU_BASE) $(QEMU_FLAGS_RUN) -machine dumpdtb=$(BUILD_DIR)/qemu-virt.dtb
	@dtc -I dtb -O dts -o $(BUILD_DIR)/qemu-virt.dts $(BUILD_DIR)/qemu-virt.dtb
	@rm $(BUILD_DIR)/qemu-virt.dtb

# Auto-generated header dependencies (-MMD -MP). Header edits now trigger rebuilds.
-include $(DEPS)


clean:
	@echo "Cleaning up..."
	@rm -rf $(BUILD_DIR)
	@rm -f $(USER_DIR)/*.elf $(USER_DIR)/*.bin
	@rm -f $(USER_DIR)/lib/*.o $(USER_DIR)/lib/libc.a $(USER_DIR)/lib/*.d

-include $(USER_LIB_OBJECTS:.o=.d)
