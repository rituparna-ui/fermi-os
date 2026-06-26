//! A synthetic arm64 "Linux" Image — the smallest thing the Fermi hypervisor's
//! Linux-slot detector will accept and boot.
//!
//! It carries a valid 64-byte arm64 boot-protocol header (the magic
//! `0x644d_5241` = "ARM\x64" at byte +56) so `hyp_create_linux_guest` detects it
//! and enters per the boot protocol (PC = Image base, x0 = DTB). The code then
//! writes a recognizable banner to the PL011 UART at IPA 0x09000000 — the device
//! the guest's stage-2 maps — and spins, so the EL2 preemption tick still slices
//! it. This stands in for a real Linux Image to exercise the M11–M13 detect +
//! boot-protocol-entry path with no external download.
//!
//! Built flat (no ELF) via `guest/build-synthimage.sh`, then staged into the
//! slot with QEMU's `-device loader` at IPA 0x40200000.

#![no_std]
#![no_main]

use core::arch::global_asm;
use core::panic::PanicInfo;

global_asm!(
    r#"
.section .text._start
.global _start
_start:
    // --- arm64 Image header (Documentation/arm64/booting.rst) ---
    b      entry            // code0  @ +0 : branch over the header to real code
    .long  0                // code1  @ +4
    .quad  0                // text_offset @ +8
    .quad  0                // image_size  @ +16
    .quad  0                // flags       @ +24
    .quad  0                // res2        @ +32
    .quad  0                // res3        @ +40
    .quad  0                // res4        @ +48
    .long  0x644d5241       // magic       @ +56  ("ARM\x64")
    .long  0                // res5 (PE)   @ +60
    // --- entry (offset 64). Runs at EL1, stage-1 MMU off; x0 = DTB IPA. ---
entry:
    movz   x1, #0x0900, lsl #16   // x1 = 0x09000000 (PL011 base, IPA)
    add    x3, x1, #0x18          // x3 = FR (flag register)
    adr    x4, banner             // x4 = PC-relative address of the banner
1:
    ldrb   w5, [x4], #1           // next byte, post-increment
    cbz    w5, 3f                 // NUL => done printing
2:
    ldr    w6, [x3]               // wait while TX FIFO full (FR.TXFF = bit 5)
    tbnz   w6, #5, 2b
    str    w5, [x1]               // UART_DR <- byte
    b      1b
3:
    // Pace + stay alive; the EL2 preemption tick interrupts this busy loop.
    movz   x9, #0x800, lsl #16
4:
    subs   x9, x9, #1
    b.ne   4b
    adr    x4, banner             // reprint each quantum so it's visibly periodic
    b      1b

banner:
    .asciz "SYNTH-LINUX: arm64 Image booted by Fermi hyp\n"
"#
);

#[panic_handler]
fn panic(_: &PanicInfo) -> ! {
    loop {}
}
