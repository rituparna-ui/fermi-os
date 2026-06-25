//! SYS_EXEC: replace the calling task's user image with an ELF binary read
//! from the VFS, build argv on a fresh user stack, swap TTBR0, and rewrite the
//! trap frame so the syscall epilogue `eret`s into the new program's entry.

use crate::exception::{TrapFrame, SP_EL0_OFFSET};
use crate::fs::vfs::{self, SEEK_END, SEEK_SET};
use crate::kprintln;
use crate::mm::consts::*;
use crate::mm::{heap, mmu, pmm};
use crate::sched;
use crate::syscall::{user_buf_ok, user_str_ok};

const EXEC_MAX_BYTES: i64 = 1 << 20; // 1 MiB
const EXEC_MAX_ARGC: usize = 32;
const EXEC_ARG_BYTES_MAX: usize = 1024;

/// Returns 0 on success (frame rewritten, new image running on eret), -1 on
/// failure (caller surfaces -1 to the user via x0).
pub fn sys_exec(arg_path: u64, arg_argv: u64, frame: &mut TrapFrame) -> i64 {
    if user_str_ok(arg_path) < 0 {
        return -1;
    }

    let cur = sched::current();
    let fds = unsafe { (*cur).fds };
    if fds.is_null() {
        return -1;
    }

    // Snapshot argv (under the OLD TTBR0) into kernel scratch.
    let mut arg_kbuf = [0u8; EXEC_ARG_BYTES_MAX];
    let mut arg_offsets = [0usize; EXEC_MAX_ARGC];
    let mut argc = 0usize;
    let mut arg_bytes = 0usize;

    if arg_argv != 0 {
        while argc < EXEC_MAX_ARGC {
            let slot_va = arg_argv + argc as u64 * 8;
            if !user_buf_ok(slot_va, 8) {
                return -1;
            }
            let p = unsafe { (slot_va as *const u64).read() };
            if p == 0 {
                break; // NULL terminator
            }
            let len = user_str_ok(p);
            if len < 0 {
                return -1;
            }
            let len = len as usize;
            if arg_bytes + len + 1 > EXEC_ARG_BYTES_MAX {
                return -1;
            }
            arg_offsets[argc] = arg_bytes;
            unsafe {
                core::ptr::copy_nonoverlapping(
                    p as *const u8,
                    arg_kbuf.as_mut_ptr().add(arg_bytes),
                    len + 1,
                );
            }
            arg_bytes += len + 1;
            argc += 1;
        }
    }

    // Resolve path string (still under old TTBR0). Copy into a small kbuf so we
    // can log it after the swap if needed.
    let path_len = user_str_ok(arg_path) as usize;
    let mut path_buf = [0u8; 256];
    let plen = core::cmp::min(path_len, 255);
    unsafe {
        core::ptr::copy_nonoverlapping(arg_path as *const u8, path_buf.as_mut_ptr(), plen);
    }
    let path = match core::str::from_utf8(&path_buf[..plen]) {
        Ok(s) => s,
        Err(_) => return -1,
    };

    // 1. Open + size the binary.
    let fd = vfs::fd_open(fds, path);
    if fd < 0 {
        return -1;
    }
    let size = vfs::fd_seek(fds, fd, 0, SEEK_END);
    vfs::fd_seek(fds, fd, 0, SEEK_SET);
    if size <= 0 || size > EXEC_MAX_BYTES {
        vfs::fd_close(fds, fd);
        return -1;
    }

    // 2. Slurp into a kernel buffer.
    let kbuf = heap::kmalloc(size as usize);
    if kbuf.is_null() {
        vfs::fd_close(fds, fd);
        return -1;
    }
    let got = vfs::fd_read(fds, fd, kbuf, size as usize);
    vfs::fd_close(fds, fd);
    if got != size {
        heap::kfree(kbuf);
        return -1;
    }
    let kslice = unsafe { core::slice::from_raw_parts(kbuf, size as usize) };

    // 3. Fresh user stack.
    let stack_phys = pmm::allocate_pages(USER_STACK_PAGES);
    if stack_phys == 0 {
        heap::kfree(kbuf);
        return -1;
    }
    unsafe {
        core::ptr::write_bytes(
            phys_to_virt(stack_phys) as *mut u8,
            0,
            (USER_STACK_PAGES * PAGE_SIZE) as usize,
        );
    }

    // 4. Fresh user_l0 + load PT_LOADs.
    let new_l0 = mmu::create_user_tables();
    if new_l0 == 0 {
        pmm::free_pages(stack_phys, USER_STACK_PAGES);
        heap::kfree(kbuf);
        return -1;
    }
    let img = match sched::elf::load(kslice, new_l0) {
        Ok(img) => img,
        Err(()) => {
            mmu::free_user_tables(new_l0);
            pmm::free_pages(stack_phys, USER_STACK_PAGES);
            heap::kfree(kbuf);
            return -1;
        }
    };
    heap::kfree(kbuf);

    // 5. Map the user stack into the new user_l0.
    let stack_flags = pte_attridx(1) | PTE_AP_RW_EL0 | PTE_UXN | PTE_PXN;
    let stack_user_base = USER_STACK_TOP - USER_STACK_PAGES * PAGE_SIZE;
    mmu::map_user_range(new_l0, stack_user_base, stack_phys, USER_STACK_PAGES, stack_flags);

    kprintln!(
        "[EXEC] Task {} '{}' loading {} ({} bytes, {} region(s), entry {:#x})",
        unsafe { (*cur).pid },
        sched::task_name(cur),
        path,
        size,
        img.region_count,
        img.entry
    );

    // 6. Swap in the new image (save old refs to free after the TTBR0 switch).
    let (old_ttbr0, old_ustack_phys, old_image);
    unsafe {
        old_ttbr0 = (*cur).ttbr0;
        old_ustack_phys = (*cur).ustack_phys;
        old_image = (*cur).exec_image;
    }

    let new_asid = sched::asid_alloc();
    let new_ttbr0 = ttbr_pack(new_l0, new_asid);
    unsafe {
        (*cur).ttbr0 = new_ttbr0;
        (*cur).ustack_phys = stack_phys;
        (*cur).user_sp = USER_STACK_TOP;
        (*cur).exec_image = img;
        core::arch::asm!("msr ttbr0_el1, {}", "isb", in(reg) new_ttbr0, options(nostack));
    }

    // 7. Rewrite the trap frame: clean registers, new ELR/SPSR/SP_EL0.
    for r in frame.regs.iter_mut() {
        *r = 0;
    }
    frame.elr = img.entry;
    frame.spsr = 0; // EL0t, IRQs unmasked

    let mut new_sp = USER_STACK_TOP;

    if argc > 0 {
        // Build argv on the new stack via its TTBR1 kernel alias.
        let stack_kbase = phys_to_virt(stack_phys);
        let stack_user_lo = USER_STACK_TOP - USER_STACK_PAGES * PAGE_SIZE;

        let strings_user_base = USER_STACK_TOP - arg_bytes as u64;
        unsafe {
            core::ptr::copy_nonoverlapping(
                arg_kbuf.as_ptr(),
                (stack_kbase + (strings_user_base - stack_user_lo)) as *mut u8,
                arg_bytes,
            );
        }

        let argv_user_top = strings_user_base & !15u64;
        let argv_user_base = argv_user_top - (argc as u64 + 1) * 8;
        let argv_kernel = (stack_kbase + (argv_user_base - stack_user_lo)) as *mut u64;
        unsafe {
            for i in 0..argc {
                argv_kernel
                    .add(i)
                    .write(strings_user_base + arg_offsets[i] as u64);
            }
            argv_kernel.add(argc).write(0);
        }

        new_sp = argv_user_base & !15u64;
        frame.regs[0] = argc as u64;
        frame.regs[1] = argv_user_base;
        frame.regs[2] = 0;
    }

    // sp_el0 lives at byte offset 280 in the 688-byte on-stack frame — not a
    // TrapFrame struct field. Poke it via raw pointer.
    unsafe {
        let frame_raw = frame as *mut TrapFrame as *mut u8;
        (frame_raw.add(SP_EL0_OFFSET) as *mut u64).write(new_sp);
    }

    // 8. Free the old image (after the TTBR0 swap).
    if old_ustack_phys != 0 {
        pmm::free_pages(old_ustack_phys, USER_STACK_PAGES);
    }
    for i in 0..old_image.region_count {
        let r = old_image.regions[i];
        if r.phys != 0 && r.pages != 0 {
            pmm::free_pages(r.phys, r.pages);
        }
    }
    if old_ttbr0 != 0 {
        let old_asid = ttbr_asid(old_ttbr0);
        if old_asid != 0 {
            let arg = (old_asid as u64) << TTBR_ASID_SHIFT;
            unsafe { core::arch::asm!("tlbi aside1, {}", "dsb ish", "isb", in(reg) arg) };
        }
        mmu::free_user_tables(ttbr_baddr(old_ttbr0));
    }

    0
}
