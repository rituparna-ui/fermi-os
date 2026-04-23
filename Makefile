# Toolchain
CROSS_COMPILE = aarch64-linux-gnu-

CC := $(CROSS_COMPILE)gcc
AS := $(CROSS_COMPILE)as
LD := $(CROSS_COMPILE)ld

# Directories
SRC_DIR := src
BUILD_DIR := build
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

# Flags
CFLAGS := -ffreestanding -g -nostdlib -nostartfiles -Wall -Wextra -O0 -mstrict-align \
					-I $(SRC_DIR)/lib \
					-I $(SRC_DIR) \
					-I $(SRC_DIR)/exception \
					-I $(SRC_DIR)/pci/virtio \
					-I $(SRC_DIR)/syscall \
					-I $(SRC_DIR)/fs
ASFLAGS := -g
LDFLAGS := -nostdlib -g -T linker.ld

# QEMU Config
DISK_IMG := $(BUILD_DIR)/disk.img
DISK_SIZE := 1G

QEMU_CPU := cortex-a72
QEMU_MACHINE := virt,gic-version=3 -m 8G
QEMU_DEVICES := -nic none \
	-device virtio-rng-pci,disable-legacy=on \
	-drive file=$(DISK_IMG),if=none,format=raw,id=d0 \
	-device virtio-blk-pci,drive=d0,disable-legacy=on
# QEMU_MACHINE := virt,gic-version=3,virtualization=on -m 8G
# QEMU_MACHINE := virt,gic-version=3,virtualization=on,secure=on -m 8G
QEMU_BASE := qemu-system-aarch64 -machine $(QEMU_MACHINE) -nographic -cpu $(QEMU_CPU) $(QEMU_DEVICES)

QEMU_FLAGS_RUN   := -kernel $(TARGET)
QEMU_FLAGS_DEBUG := -kernel $(TARGET) -s -S

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
	@$(AS) $(ASFLAGS) $< -o $@

# Run QEMU
run: all disk
	@$(QEMU_BASE) $(QEMU_FLAGS_RUN)

debug: all disk
	@$(QEMU_BASE) $(QEMU_FLAGS_DEBUG)

disk: $(DISK_IMG)

$(DISK_IMG):
	@mkdir -p $(BUILD_DIR)
	@echo "Creating $(DISK_IMG) ($(DISK_SIZE) sparse, FAT32)"
	@truncate -s $(DISK_SIZE) $@
	@mkfs.fat -F 32 -n FERMI $@ > /dev/null
	@printf 'Hello from Fermi OS!\nThis is HELLO.TXT on a FAT32 volume.\n' \
		| MTOOLS_SKIP_CHECK=1 mcopy -i $@ - ::/HELLO.TXT
	@printf '\336\255\276\357\312\376\272\276' \
		| MTOOLS_SKIP_CHECK=1 mcopy -i $@ - ::/DATA.BIN

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

clean:
	@echo "Cleaning up..."
	@rm -rf $(BUILD_DIR)
