// Position-independent EL0 program copied to USER_TEXT_BASE. Exercises the
// full process API: SYS_FORK=12, SYS_EXEC=13, SYS_WRITE=1, SYS_EXIT=4.
// After fork the parent prints a line and exits; the child exec()s the ELF
// binary on the FAT32 disk (which then runs as a fresh image).

.section .text.user
.globl user_prog_start
.globl user_prog_end

user_prog_start:
    mov x8, #12                 // SYS_FORK
    svc #0
    cbz x0, child               // x0 == 0 -> child

parent:
    mov x8, #1                  // SYS_WRITE
    mov x0, #1
    adr x1, pmsg
    mov x2, #(pmsg_end - pmsg)
    svc #0
    b done

child:
    mov x8, #13                 // SYS_EXEC("/mnt/fat32/HELLO.ELF")
    adr x0, path
    svc #0
    // Only reached if exec failed:
    mov x8, #1
    mov x0, #1
    adr x1, emsg
    mov x2, #(emsg_end - emsg)
    svc #0

done:
    mov x8, #4                  // SYS_EXIT
    mov x0, #0
    svc #0
1:  b 1b

pmsg:
    .ascii "    [user] parent: forked a child, which will exec()\n"
pmsg_end:
emsg:
    .ascii "    [user] child: exec() failed\n"
emsg_end:
path:
    .asciz "/mnt/fat32/HELLO.ELF"

user_prog_end:
