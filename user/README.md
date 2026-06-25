# User programs

Freestanding EL0 programs loaded from the FAT32 disk and run via the shell's
`exec` builtin (or `SYS_EXEC`). Each is a `no_std` `ET_EXEC` aarch64 binary
linked at `USER_TEXT_BASE = 0x400000` with `_start` as the entry point.

Build (pure Rust — no GCC needed) and place on the disk:

```bash
rustc --target aarch64-unknown-none -O -C panic=abort \
    -C link-arg=-Tuser/user.ld -C relocation-model=static \
    -o hello.elf user/hello.rs
llvm-strip --strip-all hello.elf            # optional: shrink for the disk
mcopy -o -i build/disk.img hello.elf ::/HELLO.ELF

# then in the shell:
#   exec /mnt/fat32/HELLO.ELF arg1 arg2
```

`_start` receives `x0 = argc`, `x1 = argv` (AAPCS64); the kernel builds argv on
the fresh user stack. Talk to the kernel only via `svc #0`.
