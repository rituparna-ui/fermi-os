/* Fermi OS linker script (Rust port).
 * Stage 1: identity-mapped, kernel loaded at QEMU virt RAM base 0x40000000.
 * This mirrors the original C project's first-commit linker.ld. It will be
 * promoted to a higher-half layout when the MMU subsystem is ported.
 */
ENTRY(_start)

SECTIONS
{
    . = 0x40000000;

    .text ALIGN(4096) : {
        KEEP(*(.text.boot))
        *(.text .text.*)
    }

    .rodata ALIGN(4096) : {
        *(.rodata .rodata.*)
    }

    .data ALIGN(4096) : {
        *(.data .data.*)
    }

    .bss ALIGN(4096) (NOLOAD) : {
        __bss_start = .;
        *(.bss .bss.*)
        *(COMMON)
        __bss_end = .;
    }

    . = ALIGN(4096);
    . = . + 0x10000;          /* 64 KiB boot stack */
    __stack_top = .;

    . = ALIGN(4096);
    __kernel_end = .;         /* Everything after this is free for the PMM */

    /DISCARD/ : {
        *(.comment)
        *(.note .note.*)
        *(.eh_frame .eh_frame_hdr)
        *(.ARM.exidx .ARM.exidx.*)
    }
}
