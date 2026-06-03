# Toolchain
CROSS_COMPILE = aarch64-linux-gnu-

CC := $(CROSS_COMPILE)gcc
AS := $(CROSS_COMPILE)as
LD := $(CROSS_COMPILE)ld

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

.PHONY: all run debug clean gdb tmux disk dump_dts compile_commands.json


all: $(TARGET)

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
	@$(CC) -ffreestanding -nostartfiles -nostdlib -fno-pic -static \
		-Wl,-Ttext=0x400000 -Wl,-e,_start -Wl,--build-id=none \
		-o $@ $<

# C user programs link with crt0.o which provides _start and the
# main->SYS_EXIT trampoline. crt0 is the FIRST input so the linker
# places .text._start at the start of the output — flat binaries are
# loaded at USER_TEXT_BASE = 0x400000 and the kernel jumps straight to
# that address on exec.
$(USER_CRT0): $(USER_DIR)/lib/crt0.S
	@echo "USER AS $<"
	@mkdir -p $(dir $@)
	@$(CC) -ffreestanding -nostartfiles -nostdlib -fno-pic -c -o $@ $<

$(USER_DIR)/%.elf: $(USER_DIR)/%.c $(USER_CRT0)
	@echo "USER GCC $<"
	@$(CC) -ffreestanding -nostartfiles -nostdlib -fno-pic -static \
		-Wall -Wextra -O0 -g $(USER_INCLUDE) \
		-Wl,-Ttext=0x400000 -Wl,-e,_start -Wl,--build-id=none \
		-o $@ $(USER_CRT0) $<

# objcopy step removed — the kernel now loads real ELF files. Keeping the
# rule scaffolding here would reintroduce confusion about which artifact
# ships on disk.

user_bins: $(USER_BINS)


# Run QEMU
run: all disk
	@$(QEMU_BASE) $(QEMU_FLAGS_RUN)

debug: all disk
	@$(QEMU_BASE) $(QEMU_FLAGS_DEBUG)

disk: $(DISK_IMG)

$(DISK_IMG): $(USER_BINS)
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
	@for bin in $(USER_BINS); do \
		name=$$(basename $$bin .elf | tr '[:lower:]' '[:upper:]').ELF; \
		echo "  + $$name <- $$bin"; \
		MTOOLS_SKIP_CHECK=1 mcopy -i $@ $$bin ::/$$name; \
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
	@rm -f $(USER_DIR)/*.elf $(USER_DIR)/*.bin $(USER_DIR)/lib/*.o
