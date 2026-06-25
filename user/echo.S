// EL0 program: echo argv[1] (set up by the kernel's argv stack layout).
// aarch64 process start: [sp]=argc, [sp+8]=argv[0], [sp+16]=argv[1], ...
.section .text
.global _start
_start:
    ldr x19, [sp]          // argc
    cmp x19, #2
    b.lt no_args
    ldr x20, [sp, #16]     // argv[1]
    // strlen(argv[1]) -> x2
    mov x2, #0
1:  ldrb w3, [x20, x2]
    cbz w3, 2f
    add x2, x2, #1
    b 1b
2:  mov x8, #1             // SYS_WRITE
    mov x0, #1
    mov x1, x20
    svc #0
    mov x8, #1             // newline
    mov x0, #1
    adr x1, nl
    mov x2, #1
    svc #0
    b done
no_args:
    mov x8, #1
    mov x0, #1
    adr x1, usage
    mov x2, #(usage_end - usage)
    svc #0
done:
    mov x8, #4
    mov x0, #0
    svc #0
3:  b 3b
nl:    .ascii "\n"
usage: .ascii "echo: no argument\n"
usage_end:
