#! /bin/sh

# -g for gdb debugging symbols
aarch64-linux-gnu-as -g src/boot.S -o build/boot.o

aarch64-linux-gnu-gcc -g -ffreestanding \
  -nostdlib -nostartfiles \
  -Wall -Wextra -O0 -mstrict-align \
  -c src/kernel.c -o build/kernel.o

aarch64-linux-gnu-ld -nostdlib -g -T linker.ld build/boot.o build/kernel.o -o build/kernel.elf

qemu-system-aarch64 -machine virt -nographic -cpu cortex-a72 -kernel build/kernel.elf -s -S
# gdb-multiarch ./build/kernel.elf -ex "target remote :1234" -ex "layout split"
