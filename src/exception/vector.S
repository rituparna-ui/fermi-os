// Minimal diagnostic exception vector table (aarch64).
// 16 entries x 128 bytes, table aligned to 2048. Each entry records its
// index in x0 and branches to the common entry, which calls into Rust.
// This is a stripped-down version used to diagnose early faults; the full
// trap-frame-saving version is installed at the exceptions milestone.

.section .text.vectors
.balign 2048
.globl vector_table
vector_table:

.balign 0x80
    mov x0, #0
    b exc_common
.balign 0x80
    mov x0, #1
    b exc_common
.balign 0x80
    mov x0, #2
    b exc_common
.balign 0x80
    mov x0, #3
    b exc_common
.balign 0x80
    mov x0, #4
    b exc_common
.balign 0x80
    mov x0, #5
    b exc_common
.balign 0x80
    mov x0, #6
    b exc_common
.balign 0x80
    mov x0, #7
    b exc_common
.balign 0x80
    mov x0, #8
    b exc_common
.balign 0x80
    mov x0, #9
    b exc_common
.balign 0x80
    mov x0, #10
    b exc_common
.balign 0x80
    mov x0, #11
    b exc_common
.balign 0x80
    mov x0, #12
    b exc_common
.balign 0x80
    mov x0, #13
    b exc_common
.balign 0x80
    mov x0, #14
    b exc_common
.balign 0x80
    mov x0, #15
    b exc_common

.section .text
exc_common:
    bl rust_exception_entry
1:  wfe
    b 1b
