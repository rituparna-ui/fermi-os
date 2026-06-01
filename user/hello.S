// user/hello.S
//
// Tiny user-mode program loaded from /mnt/fat32/HELLO.BIN by SYS_EXEC.
//
// Linked at USER_TEXT_BASE (0x400000) — the same VA the Fermi kernel maps
// for every EL0 task. Uses only inline `svc #0` calls with the AAPCS64
// calling convention (x0–x7 = args, x8 = syscall number).
//
// Syscalls used:
//   SYS_WRITE = 1   (fd, buf, len)  → bytes written
//   SYS_EXIT  = 4

.section .text
.global _start
_start:
	// sys_write(1, msg, msg_len)
	mov	x0, #1                  // fd = stdout (/dev/console)
	adr	x1, msg                 // PC-relative — works because we're loaded at 0x400000
	mov	x2, #(msg_end - msg)
	mov	x8, #1                  // SYS_WRITE
	svc	#0

	// sys_exit() — never returns
	mov	x8, #4                  // SYS_EXIT
	svc	#0

	// Should not reach here.
1:	b	1b

msg:
	.ascii	"Hello from /mnt/fat32/HELLO.BIN! Loaded into a fresh user_l0 by SYS_EXEC.\n"
msg_end:
