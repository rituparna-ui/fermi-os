/*
 * user/lib/crt0.S — C runtime stub for Fermi OS user programs.
 *
 * The kernel's SYS_EXEC enters the new image with:
 *   x0 = argc          (already loaded)
 *   x1 = argv          (already loaded; pointer to argv[0] on user stack)
 *   x2 = envp = NULL
 *   sp = 16-byte-aligned slot below argv
 *   pc = USER_TEXT_BASE (= _start)
 *
 * We don't need to read argc/argv off the stack — they're already in
 * x0/x1 per the AAPCS64 entry convention we chose. So _start is just:
 *   1. (Already aligned by kernel; nothing to do for sp.)
 *   2. Call main.
 *   3. SVC SYS_EXIT with main's return code in x0.
 *
 * Linked separately and `gcc -nostdlib`d into every user binary via the
 * Makefile's $(USER_DIR)/%.elf rule.
 */
.section .text._start, "ax", %progbits
.globl _start
.type  _start, %function
_start:
    /* Stack is already 16-byte aligned by the kernel — nothing to fix. */
    bl   main
    /* main's return value is in x0 — pass it untouched as the SYS_EXIT
     * "exit code". Today the kernel ignores the value but we keep the
     * convention so a future SYS_WAIT can surface child status. */
    mov  x8, #4              /* SYS_EXIT */
    svc  #0
    /* Defensive: if SYS_EXIT ever returns, halt forever rather than fall
     * through into garbage. */
1:  wfe
    b    1b
.size _start, . - _start
