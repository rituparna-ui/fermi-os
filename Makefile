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
S_SOURCES := $(shell find $(SRC_DIR) -name *.S)
C_SOURCES := $(shell find $(SRC_DIR) -name *.c)
LINKER := linker.ld

# Object File mapping
# src/boot.S -> build/boot.o
# src/kernel.c -> build/kernel.o
S_OBJECTS := $(patsubst $(SRC_DIR)/%.S, $(BUILD_DIR)/%.o, $(S_SOURCES))
C_OBJECTS := $(patsubst $(SRC_DIR)/%.c, $(BUILD_DIR)/%.o, $(C_SOURCES))
OBJECTS := $(S_OBJECTS) $(C_OBJECTS)

# Flags
ASFLAGS = -g
CFLAGS  = -g -ffreestanding -nostdlib -nostartfiles \
          -Wall -Wextra -O0 -mstrict-align
LDFLAGS = -nostdlib -g -T $(LINKER)

# QEMU Config
QEMU_CPU := cortex-a72
QEMU_MACHINE := virt
QEMU_BASE := qemu-system-aarch64 -machine $(QEMU_MACHINE) -nographic -cpu $(QEMU_CPU)

QEMU_FLAGS_RUN := -kernel $(TARGET)
QEMU_FLAGS_DEBUG := -kernel $(TARGET) -s -S

all: $(TARGET)

# TARGET depends on all .o files
$(TARGET): $(OBJECTS)
	@echo "LD $@"
	@mkdir -p $(dir $@)
	@$(LD) $(LDFLAGS) $^ -o $@

# Compile .c files
$(BUILD_DIR)/%.o: $(SRC_DIR)/%.c
	@echo "CC $<"
	@mkdir -p $(dir $@)
	@$(CC) $(CFLAGS) -c $< -o $@

# Compile .S files
$(BUILD_DIR)/%.o: $(SRC_DIR)/%.S
	@echo "AS $<"
	@mkdir -p $(dir $@)
	@$(AS) $(ASFLAGS) $< -o $@

# Run QEMU
run: all
	@$(QEMU_BASE) $(QEMU_FLAGS_RUN)

debug: all
	@$(QEMU_BASE) $(QEMU_FLAGS_DEBUG)

# GDB Config
GDB := gdb-multiarch
GDB_FLAGS := -ex "target remote :1234" -ex "layout split"
GDB_CMD := $(GDB) $(TARGET) $(GDB_FLAGS)

gdb:
	@$(GDB_CMD)

tmux:
	tmux new-session -d -s debug \
		"$(QEMU_BASE) $(QEMU_FLAGS_DEBUG)" \; \
		split-window -h '$(GDB_CMD)' \; \
		attach

clean:
	@echo "Cleaning up..."
	@rm -rf $(BUILD_DIR)
