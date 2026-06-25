/* Fermi OS linker script (Rust port) — higher-half kernel.
 * Kernel is linked at the upper-half VA (KERNEL_VA) but loaded at the
 * physical RAM base (KERNEL_PA). Before the MMU is on, boot.S converts the
 * (absolute) high symbol addresses to physical by subtracting KERNEL_VA_OFFSET.
 * After the MMU + higher-half jump, the kernel runs entirely from TTBR1, so
 * TTBR0 is free for per-task EL0 user mappings.
 */
ENTRY(_start)

KERNEL_PA = 0x40000000;
KERNEL_VA = 0xFFFF000040000000;

PHDRS
{
    text   PT_LOAD FLAGS(5);  /* R + X */
    rodata PT_LOAD FLAGS(4);  /* R     */
    data   PT_LOAD FLAGS(6);  /* R + W */
}

SECTIONS
{
    . = KERNEL_VA;

    .text ALIGN(4096) : AT(KERNEL_PA) {
        __text_start = .;
        KEEP(*(.text.boot))
        . = ALIGN(2048);
        KEEP(*(.text.vectors))
        *(.text .text.*)
    } :text

    .rodata ALIGN(4096) : {
        *(.rodata .rodata.*)
    } :rodata

    /* Upper bound of pages that may be exposed to EL0. Everything past here
     * (.data, .bss) is kernel-private and must never be mapped into a user
     * TTBR0. */
    . = ALIGN(4096);
    __user_text_end = .;

    .data ALIGN(4096) : {
        *(.data .data.*)
    } :data

    .bss ALIGN(4096) (NOLOAD) : {
        __bss_start = .;
        *(.bss .bss.*)
        *(COMMON)
        __bss_end = .;
    } :data

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
