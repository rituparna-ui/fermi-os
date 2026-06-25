// A tiny position-independent EL0 user program, copied into a user page and
// executed at USER_TEXT_BASE. It uses only the syscall ABI (svc #0, x8 = number,
// x0..x5 = args) and PC-relative data access, so it runs correctly regardless
// of the VA it is mapped at.
//
// Syscalls: SYS_WRITE=1, SYS_SLEEP=6, SYS_GETPID=7, SYS_EXIT=4.

.section .text.user
.globl user_prog_start
.globl user_prog_end

user_prog_start:
    mov x19, #0                 // iteration counter
loop:
    // write(1, msg, msglen)
    mov x8, #1
    mov x0, #1
    adr x1, msg                 // PC-relative -> user VA of msg
    mov x2, #(msg_end - msg)
    svc #0

    // getpid()
    mov x8, #7
    svc #0                      // x0 = pid (ignored here)

    // sleep(400 ms)
    mov x8, #6
    mov x0, #400
    svc #0

    add x19, x19, #1
    cmp x19, #3
    b.lt loop

    // exit(0)
    mov x8, #4
    mov x0, #0
    svc #0
1:  b 1b                        // unreachable

msg:
    .ascii "    [user] hello from EL0 (svc write)\n"
msg_end:

user_prog_end:
