# Toolchain
CROSS_COMPILE = aarch64-linux-gnu-

CC := $(CROSS_COMPILE)gcc
AS := $(CROSS_COMPILE)as
LD := $(CROSS_COMPILE)ld

# Directories
SRC_DIR := src
BUILD_DIR := build
USER_DIR  := user
HYP_DIR   := $(SRC_DIR)/hyp
TARGET := $(BUILD_DIR)/kernel.elf       # the GUEST (FermiOS, runs at EL1/EL0)
HYP_TARGET := $(BUILD_DIR)/hyp.elf       # the HYPERVISOR (runs at EL2)

# File discovery — the GUEST is every .c/.S under src EXCEPT src/hyp (the
# hypervisor is a separate image and must never be linked into the guest).
S_SOURCES := $(shell find $(SRC_DIR) -path "$(HYP_DIR)" -prune -o -name "*.S" -print)
C_SOURCES := $(shell find $(SRC_DIR) -path "$(HYP_DIR)" -prune -o -name "*.c" -print)

# The HYPERVISOR image sources (EL2). Exclude the standalone guest subdirs —
# those are built separately to flat blobs, not linked into the hyp.
HYP_GUEST_DIRS := $(HYP_DIR)/guest2 $(HYP_DIR)/ipc $(HYP_DIR)/dom0 $(HYP_DIR)/vmtgt $(HYP_DIR)/crasher $(HYP_DIR)/hangguest $(HYP_DIR)/rngclient
HYP_PRUNE := $(foreach d,$(HYP_GUEST_DIRS),-path "$(d)" -o)
HYP_S_SOURCES := $(shell find $(HYP_DIR) \( $(HYP_PRUNE) -false \) -prune -o -name "*.S" -print)
HYP_C_SOURCES := $(shell find $(HYP_DIR) \( $(HYP_PRUNE) -false \) -prune -o -name "*.c" -print)

# Object File Mapping
# src/boot.S      -> build/boot.o
# src/kernel.c  	-> build/kernel.o
S_OBJECTS := $(patsubst $(SRC_DIR)/%.S, $(BUILD_DIR)/%.o, $(S_SOURCES))
C_OBJECTS := $(patsubst $(SRC_DIR)/%.c, $(BUILD_DIR)/%.o, $(C_SOURCES))
OBJECTS := $(S_OBJECTS) $(C_OBJECTS)

# Hypervisor objects land under build/hyp/ (mirrors src/hyp/).
HYP_S_OBJECTS := $(patsubst $(SRC_DIR)/%.S, $(BUILD_DIR)/%.o, $(HYP_S_SOURCES))
HYP_C_OBJECTS := $(patsubst $(SRC_DIR)/%.c, $(BUILD_DIR)/%.o, $(HYP_C_SOURCES))
HYP_OBJECTS := $(HYP_S_OBJECTS) $(HYP_C_OBJECTS)

DEPS    := $(OBJECTS:.o=.d) $(HYP_OBJECTS:.o=.d)

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

# Hypervisor image: same freestanding flags, its own include dir + linker script.
# -mgeneral-regs-only forbids FP/SIMD in EL2 codegen so the GPR-only world-switch
# trap frame can never silently clobber the guest's caller-saved q-registers.
HYP_CFLAGS  := -ffreestanding -g -nostdlib -nostartfiles -Wall -Wextra -O0 \
               -mstrict-align -fno-pic -mgeneral-regs-only -MMD -MP -I $(HYP_DIR)
HYP_LDFLAGS := -nostdlib -g -T $(HYP_DIR)/linker_hyp.ld

# QEMU Config
DISK_IMG := $(BUILD_DIR)/disk.img
DISK_SIZE := 1G

# EL2 hypervisor mode: '-cpu max' implements FEAT_VHE, stage-2, GICv3 virt and
# CNTVOFF; 'virtualization=on' makes QEMU '-kernel' enter at EL2. RAM is 9 GiB so
# the hyp image at PA 0x250000000 lives ABOVE the guest's 8 GiB (0x40000000..
# 0x240000000) — no hyp/guest collision even with stage-2 off.
QEMU_CPU := max
QEMU_MACHINE := virt,gic-version=3,virtualization=on -m 9G
QEMU_DEVICES := -netdev user,id=n0 \
	-device virtio-net-pci,netdev=n0,disable-legacy=on \
	-device virtio-rng-pci,disable-legacy=on \
	-drive file=$(DISK_IMG),if=none,format=raw,id=d0 \
	-device virtio-blk-pci,drive=d0,disable-legacy=on \
	-chardev file,id=vc,path=$(BUILD_DIR)/virtio-console.txt,mux=off \
	-device virtio-serial-pci,disable-legacy=on \
	-device virtconsole,chardev=vc \
	-device virtio-balloon-pci,disable-legacy=on
QEMU_BASE := qemu-system-aarch64 -machine $(QEMU_MACHINE) -nographic -cpu $(QEMU_CPU) $(QEMU_DEVICES)

# '-kernel' loads the HYPERVISOR (entered at EL2). The GUEST FermiOS image is
# EMBEDDED inside the hyp (build/guest.bin -> .rodata.guest via guest_blob.S)
# and memcpy'd to its physical load base (0x40000000) by the hyp before the
# eret. This makes hyp.elf the ONLY ROM QEMU loads, so the auto-placed DTB at
# 0x40000000 does not collide with a separately-loaded guest ELF.
QEMU_FLAGS_RUN   := -kernel $(HYP_TARGET)
QEMU_FLAGS_DEBUG := -kernel $(HYP_TARGET) -s -S

.PHONY: all run debug clean gdb tmux disk dump_dts compile_commands.json


all: $(TARGET) $(HYP_TARGET)

# Guest (FermiOS, EL1/EL0) — links all non-hyp .o files.
$(TARGET): $(OBJECTS)
	@echo "LD  $@"
	@mkdir -p $(dir $@)
	@$(LD) $(LDFLAGS) -o $@ $^

# Flat guest image embedded into the hypervisor (incbin'd by guest_blob.S).
# objcopy -O binary lays bytes out by LMA starting at guest PA 0x40000000,
# preserving inter-segment gaps, so the hyp's single memcpy to 0x40000000
# reconstructs every guest PT_LOAD.
GUEST_BIN := $(BUILD_DIR)/guest.bin
$(GUEST_BIN): $(TARGET)
	@echo "OBJCOPY $@ (flat guest image)"
	@$(CROSS_COMPILE)objcopy -O binary $(TARGET) $@

# The guest blob object incbin's $(GUEST_BIN); make it an explicit prerequisite.
$(BUILD_DIR)/hyp/guest_blob.o: $(GUEST_BIN)

# VM2: a tiny standalone EL1 guest (heartbeat printer) used to demonstrate the
# multi-VM world switch + per-VM stage-2 isolation. Built directly to a flat
# binary; linked at IPA 0x40000000 (same as VM1 — the hyp maps it to a
# different host PA). Embedded via guest2_blob.S.
GUEST2_BIN := $(BUILD_DIR)/guest2.bin
$(GUEST2_BIN): $(HYP_DIR)/guest2/guest2.S $(HYP_DIR)/guest2/linker_g2.ld
	@echo "GUEST2 $@"
	@mkdir -p $(dir $@)
	@$(CC) -ffreestanding -nostdlib -nostartfiles -fno-pic \
		-Wl,-T,$(HYP_DIR)/guest2/linker_g2.ld -Wl,--build-id=none \
		-o $(BUILD_DIR)/guest2.elf $(HYP_DIR)/guest2/guest2.S
	@$(CROSS_COMPILE)objcopy -O binary $(BUILD_DIR)/guest2.elf $@

$(BUILD_DIR)/hyp/guest2_blob.o: $(GUEST2_BIN)

# IPC demo guest: one standalone EL1 image run by TWO VMs (producer + consumer)
# to demonstrate inter-VM shared memory. Built flat, embedded via ipc_blob.S.
IPC_BIN := $(BUILD_DIR)/ipc.bin
$(IPC_BIN): $(HYP_DIR)/ipc/ipc.S $(HYP_DIR)/ipc/linker_ipc.ld
	@echo "IPCGUEST $@"
	@mkdir -p $(dir $@)
	@$(CC) -ffreestanding -nostdlib -nostartfiles -fno-pic \
		-Wl,-T,$(HYP_DIR)/ipc/linker_ipc.ld -Wl,--build-id=none \
		-o $(BUILD_DIR)/ipc.elf $(HYP_DIR)/ipc/ipc.S
	@$(CROSS_COMPILE)objcopy -O binary $(BUILD_DIR)/ipc.elf $@

$(BUILD_DIR)/hyp/ipc_blob.o: $(IPC_BIN)

# dom0 control guest: a privileged standalone EL1 guest that drives the VMCTL
# management hypercall. Built flat, embedded via dom0_blob.S.
DOM0_BIN := $(BUILD_DIR)/dom0.bin
$(DOM0_BIN): $(HYP_DIR)/dom0/dom0.S $(HYP_DIR)/dom0/linker_dom0.ld
	@echo "DOM0 $@"
	@mkdir -p $(dir $@)
	@$(CC) -ffreestanding -nostdlib -nostartfiles -fno-pic \
		-Wl,-T,$(HYP_DIR)/dom0/linker_dom0.ld -Wl,--build-id=none \
		-o $(BUILD_DIR)/dom0.elf $(HYP_DIR)/dom0/dom0.S
	@$(CROSS_COMPILE)objcopy -O binary $(BUILD_DIR)/dom0.elf $@

$(BUILD_DIR)/hyp/dom0_blob.o: $(DOM0_BIN)

# Migration-target guest: idle stub that a live migration clones a snapshot into.
VMTGT_BIN := $(BUILD_DIR)/vmtgt.bin
$(VMTGT_BIN): $(HYP_DIR)/vmtgt/vmtgt.S $(HYP_DIR)/vmtgt/linker_vmtgt.ld
	@echo "VMTGT $@"
	@mkdir -p $(dir $@)
	@$(CC) -ffreestanding -nostdlib -nostartfiles -fno-pic \
		-Wl,-T,$(HYP_DIR)/vmtgt/linker_vmtgt.ld -Wl,--build-id=none \
		-o $(BUILD_DIR)/vmtgt.elf $(HYP_DIR)/vmtgt/vmtgt.S
	@$(CROSS_COMPILE)objcopy -O binary $(BUILD_DIR)/vmtgt.elf $@

$(BUILD_DIR)/hyp/vmtgt_blob.o: $(VMTGT_BIN)

# crasher guest: deliberately faults, to demonstrate per-VM fault isolation.
CRASHER_BIN := $(BUILD_DIR)/crasher.bin
$(CRASHER_BIN): $(HYP_DIR)/crasher/crasher.S $(HYP_DIR)/crasher/linker_crasher.ld
	@echo "CRASHER $@"
	@mkdir -p $(dir $@)
	@$(CC) -ffreestanding -nostdlib -nostartfiles -fno-pic \
		-Wl,-T,$(HYP_DIR)/crasher/linker_crasher.ld -Wl,--build-id=none \
		-o $(BUILD_DIR)/crasher.elf $(HYP_DIR)/crasher/crasher.S
	@$(CROSS_COMPILE)objcopy -O binary $(BUILD_DIR)/crasher.elf $@

$(BUILD_DIR)/hyp/crasher_blob.o: $(CRASHER_BIN)

# hangguest: livelocks (no fault) to demonstrate the liveness watchdog.
HANG_BIN := $(BUILD_DIR)/hangguest.bin
$(HANG_BIN): $(HYP_DIR)/hangguest/hangguest.S $(HYP_DIR)/hangguest/linker_hang.ld
	@echo "HANGGUEST $@"
	@mkdir -p $(dir $@)
	@$(CC) -ffreestanding -nostdlib -nostartfiles -fno-pic \
		-Wl,-T,$(HYP_DIR)/hangguest/linker_hang.ld -Wl,--build-id=none \
		-o $(BUILD_DIR)/hangguest.elf $(HYP_DIR)/hangguest/hangguest.S
	@$(CROSS_COMPILE)objcopy -O binary $(BUILD_DIR)/hangguest.elf $@

$(BUILD_DIR)/hyp/hangguest_blob.o: $(HANG_BIN)

# rngclient: drives the emulated virtio-mmio entropy device.
RNG_BIN := $(BUILD_DIR)/rngclient.bin
$(RNG_BIN): $(HYP_DIR)/rngclient/rngclient.S $(HYP_DIR)/rngclient/linker_rng.ld
	@echo "RNGCLIENT $@"
	@mkdir -p $(dir $@)
	@$(CC) -ffreestanding -nostdlib -nostartfiles -fno-pic \
		-Wl,-T,$(HYP_DIR)/rngclient/linker_rng.ld -Wl,--build-id=none \
		-o $(BUILD_DIR)/rngclient.elf $(HYP_DIR)/rngclient/rngclient.S
	@$(CROSS_COMPILE)objcopy -O binary $(BUILD_DIR)/rngclient.elf $@

$(BUILD_DIR)/hyp/rngclient_blob.o: $(RNG_BIN)

# Hypervisor (EL2) — separate image, its own linker script.
$(HYP_TARGET): $(HYP_OBJECTS)
	@echo "LD  $@ (hypervisor, EL2)"
	@mkdir -p $(dir $@)
	@$(LD) $(HYP_LDFLAGS) -o $@ $^

# Hypervisor objects (compiled with HYP_CFLAGS). Listed BEFORE the generic
# src rules so make prefers these more-specific patterns for src/hyp/*.
$(BUILD_DIR)/hyp/%.o: $(SRC_DIR)/hyp/%.c
	@echo "CC $< (hyp)"
	@mkdir -p $(dir $@)
	@$(CC) $(HYP_CFLAGS) -c $< -o $@

$(BUILD_DIR)/hyp/%.o: $(SRC_DIR)/hyp/%.S
	@echo "AS  $< (hyp)"
	@mkdir -p $(dir $@)
	@$(CC) $(HYP_CFLAGS) -x assembler-with-cpp -c $< -o $@

# Compile all guest .c files
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
