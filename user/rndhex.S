// EL0 program: getrandom(16 bytes) via SYS_GETRANDOM, print as hex.
// ABI: x8=syscall, x0..=args. SYS_GETRANDOM=16, SYS_WRITE=1, SYS_EXIT=4.
.section .text
.global _start
_start:
    sub sp, sp, #64
    mov x8, #16            // SYS_GETRANDOM
    mov x0, sp             // buf
    mov x1, #16            // len
    svc #0

    add x9, sp, #16        // hex-output buffer
    mov x2, #0             // byte index
1:  cmp x2, #16
    b.ge 2f
    ldrb w3, [sp, x2]
    lsr w4, w3, #4         // hi nibble
    cmp w4, #10
    add w5, w4, #'0'
    add w6, w4, #('a' - 10)
    csel w5, w5, w6, lt
    lsl x7, x2, #1
    strb w5, [x9, x7]
    and w4, w3, #0xf       // lo nibble
    cmp w4, #10
    add w5, w4, #'0'
    add w6, w4, #('a' - 10)
    csel w5, w5, w6, lt
    add x7, x7, #1
    strb w5, [x9, x7]
    add x2, x2, #1
    b 1b
2:  mov w5, #10            // newline
    strb w5, [x9, #32]
    mov x8, #1             // SYS_WRITE(1, out, 33)
    mov x0, #1
    mov x1, x9
    mov x2, #33
    svc #0
    mov x8, #4             // SYS_EXIT
    mov x0, #0
    svc #0
3:  b 3b
