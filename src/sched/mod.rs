//! Preemptive round-robin scheduler (EL1 tasks) with tick-based sleep.
//!
//! Port of the early `src/sched/sched.c` (EL1-only). Per-task kernel stacks,
//! a circular run queue, timer-driven preemption via `schedule()`, voluntary
//! `yield`/`sleep_ms`, and dead-task reaping. EL0/userspace, per-task TTBR0,
//! and demand paging are added at the syscall/userspace milestone.

use crate::exception::timer;
use crate::kprintln;
use crate::mm::mmu::{self, phys_to_virt};
use crate::mm::pmm::{self, PAGE_SIZE};
use crate::mm::heap::{kfree, kmalloc};
use crate::sync::Racy;
use crate::uart;
use core::arch::global_asm;

global_asm!(include_str!("switch.S"));
global_asm!(include_str!("user_prog.S"));

pub const TASK_STACK_PAGES: u64 = 4; // 16 KiB

pub const TASK_READY: u32 = 0;
pub const TASK_RUNNING: u32 = 1;
pub const TASK_SLEEPING: u32 = 2;
pub const TASK_DEAD: u32 = 3;

pub type TaskEntry = extern "C" fn();

#[repr(C)]
pub struct Task {
    pub sp: u64,        // 0  — kernel SP (switch.S saves/restores here)
    pub ttbr0: u64,     // 8  — packed TTBR0 (0 = kernel task, no swap)
    pub pid: u64,       // 16
    pub state: u32,     // 24
    _pad: u32,
    pub sleep_until: u64, // 32
    pub stack_phys: u64,  // 40 — kernel stack phys
    pub user_sp: u64,     // 48
    pub ustack_phys: u64, // 56 — user stack phys (for freeing)
    pub user_l0: u64,     // 64 — user L0 table phys (for freeing)
    pub utext_phys: u64,  // 72 — user text page phys (for freeing)
    pub name: [u8; 16],   // 80
    pub next: u64,        // 96 — *mut Task, 0 = null
    pub fds: u64,         // 104 — *mut FdTable, 0 = none
    pub exec_phys: [u64; 4],   // ELF PT_LOAD region phys bases (for reap)
    pub exec_pages: [u64; 4],
    pub exec_count: u32,
    pub stack_grown: [u64; 16],  // demand-paged stack pages (for reap)
    pub stack_grown_count: u32,
    pub ticks: u64,              // timer ticks accumulated while RUNNING
}

extern "C" {
    fn context_switch(prev: *mut Task, next: *mut Task);
    fn task_trampoline();
    fn user_trampoline();
    fn fork_return();
    static user_prog_start: u8;
    static user_prog_end: u8;
}

struct Sched {
    idle: Task,
    current: *mut Task,
    next_pid: u64,
    dead_list: *mut Task,
    ctxt_switches: u64,
}

static SCHED: Racy<Sched> = Racy::new(Sched {
    idle: Task {
        sp: 0,
        ttbr0: 0,
        pid: 0,
        state: TASK_RUNNING,
        _pad: 0,
        sleep_until: 0,
        stack_phys: 0,
        user_sp: 0,
        ustack_phys: 0,
        user_l0: 0,
        utext_phys: 0,
        name: [0; 16],
        next: 0,
        fds: 0,
        exec_phys: [0; 4],
        exec_pages: [0; 4],
        exec_count: 0,
        stack_grown: [0; 16],
        stack_grown_count: 0,
        ticks: 0,
    },
    current: core::ptr::null_mut(),
    next_pid: 0,
    dead_list: core::ptr::null_mut(),
    ctxt_switches: 0,
});

fn copy_name(dst: &mut [u8; 16], src: &str) {
    let bytes = src.as_bytes();
    let n = core::cmp::min(bytes.len(), 15);
    dst[..n].copy_from_slice(&bytes[..n]);
    dst[n] = 0;
}

fn name_str(t: &Task) -> &str {
    let len = t.name.iter().position(|&b| b == 0).unwrap_or(16);
    core::str::from_utf8(&t.name[..len]).unwrap_or("?")
}

pub fn init() {
    uart::println("[SCHED] Initializing scheduler");
    let s = unsafe { SCHED.get() };
    let idle = &mut s.idle as *mut Task;
    unsafe {
        (*idle).pid = s.next_pid;
        (*idle).state = TASK_RUNNING;
        (*idle).stack_phys = 0;
        (*idle).next = idle as u64;
        copy_name(&mut (*idle).name, "idle");
    }
    s.next_pid += 1;
    s.current = idle;
    uart::println("[SCHED] Initialized! Idle task registered");
}

/// Total context switches since boot.
pub fn context_switches() -> u64 {
    unsafe { SCHED.get() }.ctxt_switches
}

/// Pointer to the currently running task.
pub fn current() -> *mut Task {
    unsafe { SCHED.get() }.current
}

/// Allocate and initialize an EL1 kernel task WITHOUT linking it into core 0's
/// run queue. Used by the secondary core to build its own task set. `pid` is
/// caller-chosen to avoid racing core 0's pid counter.
pub fn make_kernel_task(name: &str, pid: u64, entry: TaskEntry) -> *mut Task {
    let t = kmalloc(core::mem::size_of::<Task>()) as *mut Task;
    if t.is_null() {
        return t;
    }
    unsafe { core::ptr::write_bytes(t as *mut u8, 0, core::mem::size_of::<Task>()) };
    let stack_phys = pmm::allocate_pages(TASK_STACK_PAGES);
    if stack_phys == 0 {
        kfree(t as usize);
        return core::ptr::null_mut();
    }
    let stack_va = phys_to_virt(stack_phys);
    let stack_top = stack_va + TASK_STACK_PAGES * PAGE_SIZE;
    unsafe {
        core::ptr::write_bytes(stack_va as *mut u8, 0, (TASK_STACK_PAGES * PAGE_SIZE) as usize);
        let frame = (stack_top - 160) as *mut u64;
        *frame.add(0) = entry as usize as u64; // x19
        *frame.add(11) = task_trampoline as usize as u64; // x30
        (*t).sp = frame as u64;
        (*t).pid = pid;
        (*t).state = TASK_READY;
        (*t).stack_phys = stack_phys;
        copy_name(&mut (*t).name, name);
    }
    t
}

/// Raw context switch (for the secondary core's own scheduler).
pub unsafe fn raw_context_switch(prev: *mut Task, next: *mut Task) {
    context_switch(prev, next);
}

/// Allocate a bare (zeroed) Task struct (e.g. a core's idle context).
pub fn alloc_bare_task() -> *mut Task {
    let t = kmalloc(core::mem::size_of::<Task>()) as *mut Task;
    if !t.is_null() {
        unsafe { core::ptr::write_bytes(t as *mut u8, 0, core::mem::size_of::<Task>()) };
    }
    t
}

pub fn create_task(name: &str, entry: TaskEntry) -> i64 {
    let s = unsafe { SCHED.get() };
    let t = kmalloc(core::mem::size_of::<Task>()) as *mut Task;
    if t.is_null() {
        uart::errorln("[SCHED] Failed to allocate task struct");
        return -1;
    }
    unsafe { core::ptr::write_bytes(t as *mut u8, 0, core::mem::size_of::<Task>()) };

    let stack_phys = pmm::allocate_pages(TASK_STACK_PAGES);
    if stack_phys == 0 {
        uart::errorln("[SCHED] Failed to allocate task stack");
        kfree(t as usize);
        return -1;
    }
    let stack_va = phys_to_virt(stack_phys);
    let stack_size = TASK_STACK_PAGES * PAGE_SIZE;
    let stack_top = stack_va + stack_size;
    unsafe { core::ptr::write_bytes(stack_va as *mut u8, 0, stack_size as usize) };

    // Initial context_switch frame (160 bytes): x19 = entry, x30 = trampoline.
    let frame = (stack_top - 160) as *mut u64;
    unsafe {
        *frame.add(0) = entry as usize as u64; // x19
        *frame.add(11) = task_trampoline as usize as u64; // x30
        (*t).sp = frame as u64;
        (*t).pid = s.next_pid;
        (*t).state = TASK_READY;
        (*t).stack_phys = stack_phys;
        (*t).fds = crate::fs::vfs::fd_table_create() as u64;
        copy_name(&mut (*t).name, name);

        // Tail-insert into the circular queue.
        let cur = s.current;
        let mut tail = cur;
        while (*tail).next != cur as u64 {
            tail = (*tail).next as *mut Task;
        }
        (*tail).next = t as u64;
        (*t).next = cur as u64;
    }
    s.next_pid += 1;
    let pid = unsafe { (*t).pid };
    kprintln!(
        "[SCHED] Created task {} '{}' | stack: {:#x} - {:#x}",
        pid,
        name,
        stack_va,
        stack_top
    );
    pid as i64
}

/// Create an EL0 user task running the embedded user program. Sets up a
/// per-task TTBR0 with the user text mapped RX at USER_TEXT_BASE and a user
/// stack mapped RW below USER_STACK_TOP, then arranges an eret to EL0.
pub fn create_user_task(name: &str) -> i64 {
    let s = unsafe { SCHED.get() };
    let t = kmalloc(core::mem::size_of::<Task>()) as *mut Task;
    if t.is_null() {
        uart::errorln("[SCHED] user: alloc task failed");
        return -1;
    }
    unsafe { core::ptr::write_bytes(t as *mut u8, 0, core::mem::size_of::<Task>()) };

    // Kernel stack for the task.
    let kstack_phys = pmm::allocate_pages(TASK_STACK_PAGES);
    if kstack_phys == 0 {
        kfree(t as usize);
        return -1;
    }
    let kstack_top = phys_to_virt(kstack_phys) + TASK_STACK_PAGES * PAGE_SIZE;
    unsafe { core::ptr::write_bytes(phys_to_virt(kstack_phys) as *mut u8, 0, (TASK_STACK_PAGES * PAGE_SIZE) as usize) };

    // Per-task user page tables.
    let l0 = mmu::create_user_tables();
    if l0 == 0 {
        pmm::free_pages(kstack_phys, TASK_STACK_PAGES);
        kfree(t as usize);
        return -1;
    }

    // User text page: copy the embedded program and map RX at USER_TEXT_BASE.
    let utext_phys = pmm::allocate_page();
    let prog_len = unsafe {
        (&user_prog_end as *const u8 as usize) - (&user_prog_start as *const u8 as usize)
    };
    unsafe {
        core::ptr::copy_nonoverlapping(
            &user_prog_start as *const u8,
            phys_to_virt(utext_phys) as *mut u8,
            prog_len,
        );
    }
    // EL0 read+execute, kernel no-execute (PXN), Normal memory.
    mmu::map_user_range(
        l0,
        mmu::USER_TEXT_BASE,
        utext_phys,
        1,
        mmu::PTE_AP_RO_EL0 | mmu::PTE_PXN | mmu::pte_attridx(1),
    );

    // User stack: USER_STACK_PAGES below USER_STACK_TOP, RW, non-exec.
    let ustack_phys = pmm::allocate_pages(mmu::USER_STACK_PAGES);
    let ustack_size = mmu::USER_STACK_PAGES * PAGE_SIZE;
    let ustack_base = mmu::USER_STACK_TOP - ustack_size;
    mmu::map_user_range(
        l0,
        ustack_base,
        ustack_phys,
        mmu::USER_STACK_PAGES,
        mmu::PTE_AP_RW_EL0 | mmu::PTE_UXN | mmu::PTE_PXN | mmu::pte_attridx(1),
    );

    // Kernel stack frame: x19 = user entry, x20 = user SP, x30 = user_trampoline.
    let frame = (kstack_top - 160) as *mut u64;
    let pid = s.next_pid;
    unsafe {
        *frame.add(0) = mmu::USER_TEXT_BASE; // x19
        *frame.add(1) = mmu::USER_STACK_TOP; // x20
        *frame.add(11) = user_trampoline as usize as u64; // x30
        (*t).sp = frame as u64;
        (*t).ttbr0 = mmu::ttbr_pack(l0, pid as u16);
        (*t).pid = pid;
        (*t).state = TASK_READY;
        (*t).stack_phys = kstack_phys;
        (*t).user_sp = mmu::USER_STACK_TOP;
        (*t).ustack_phys = ustack_phys;
        (*t).user_l0 = l0;
        (*t).utext_phys = utext_phys;
        (*t).fds = crate::fs::vfs::fd_table_create() as u64;
        copy_name(&mut (*t).name, name);

        let cur = s.current;
        let mut tail = cur;
        while (*tail).next != cur as u64 {
            tail = (*tail).next as *mut Task;
        }
        (*tail).next = t as u64;
        (*t).next = cur as u64;
    }
    s.next_pid += 1;
    kprintln!("[SCHED] Created EL0 task {} '{}' (ttbr0 l0={:#x})", pid, name, l0);
    pid as i64
}

/// Create an EL0 task by loading an ELF image into a fresh address space.
pub fn spawn_elf(name: &str, elf_bytes: &[u8]) -> i64 {
    let s = unsafe { SCHED.get() };
    let t = kmalloc(core::mem::size_of::<Task>()) as *mut Task;
    if t.is_null() {
        return -1;
    }
    unsafe { core::ptr::write_bytes(t as *mut u8, 0, core::mem::size_of::<Task>()) };

    let kstack_phys = pmm::allocate_pages(TASK_STACK_PAGES);
    if kstack_phys == 0 {
        kfree(t as usize);
        return -1;
    }
    let kstack_top = phys_to_virt(kstack_phys) + TASK_STACK_PAGES * PAGE_SIZE;
    unsafe { core::ptr::write_bytes(phys_to_virt(kstack_phys) as *mut u8, 0, (TASK_STACK_PAGES * PAGE_SIZE) as usize) };

    let l0 = mmu::create_user_tables();
    if l0 == 0 {
        pmm::free_pages(kstack_phys, TASK_STACK_PAGES);
        kfree(t as usize);
        return -1;
    }
    let img = match crate::elf::load(elf_bytes, l0) {
        Some(i) => i,
        None => {
            mmu::free_user_tables(l0);
            pmm::free_pages(kstack_phys, TASK_STACK_PAGES);
            kfree(t as usize);
            return -1;
        }
    };

    // User stack.
    let ustack_phys = pmm::allocate_pages(mmu::USER_STACK_PAGES);
    let ustack_size = mmu::USER_STACK_PAGES * PAGE_SIZE;
    let ustack_base = mmu::USER_STACK_TOP - ustack_size;
    mmu::map_user_range(l0, ustack_base, ustack_phys, mmu::USER_STACK_PAGES,
        mmu::PTE_AP_RW_EL0 | mmu::PTE_UXN | mmu::PTE_PXN | mmu::pte_attridx(1));

    let frame = (kstack_top - 160) as *mut u64;
    let pid = s.next_pid;
    unsafe {
        *frame.add(0) = img.entry;            // x19 = ELF entry
        *frame.add(1) = mmu::USER_STACK_TOP;  // x20 = user SP
        *frame.add(11) = user_trampoline as usize as u64;
        (*t).sp = frame as u64;
        (*t).ttbr0 = mmu::ttbr_pack(l0, pid as u16);
        (*t).pid = pid;
        (*t).state = TASK_READY;
        (*t).stack_phys = kstack_phys;
        (*t).user_sp = mmu::USER_STACK_TOP;
        (*t).ustack_phys = ustack_phys;
        (*t).user_l0 = l0;
        (*t).fds = crate::fs::vfs::fd_table_create() as u64;
        for i in 0..img.region_count {
            (*t).exec_phys[i] = img.regions[i].phys;
            (*t).exec_pages[i] = img.regions[i].pages;
        }
        (*t).exec_count = img.region_count as u32;
        copy_name(&mut (*t).name, name);
        let cur = s.current;
        let mut tail = cur;
        while (*tail).next != cur as u64 {
            tail = (*tail).next as *mut Task;
        }
        (*tail).next = t as u64;
        (*t).next = cur as u64;
    }
    s.next_pid += 1;
    kprintln!("[SCHED] Spawned ELF task {} '{}' entry={:#x}", pid, name, img.entry);
    pid as i64
}

/// exec() — replace the current EL0 task's image with a freshly-loaded ELF.
/// On success this does not "return" in the usual sense: it rewrites the trap
/// frame so the syscall epilogue erets into the new program. Returns -1 on
/// failure (caller keeps running its old image).
pub fn exec_image(frame: *mut crate::exception::TrapFrame, elf_bytes: &[u8]) -> i64 {
    let s = unsafe { SCHED.get() };
    let cur = s.current;
    if cur.is_null() || frame.is_null() {
        return -1;
    }
    // Build the new address space first; keep the old one until we succeed.
    let l0_new = mmu::create_user_tables();
    if l0_new == 0 {
        return -1;
    }
    let img = match crate::elf::load(elf_bytes, l0_new) {
        Some(i) => i,
        None => {
            mmu::free_user_tables(l0_new);
            return -1;
        }
    };
    let ustack_new = pmm::allocate_pages(mmu::USER_STACK_PAGES);
    let ustack_size = mmu::USER_STACK_PAGES * PAGE_SIZE;
    let ustack_base = mmu::USER_STACK_TOP - ustack_size;
    mmu::map_user_range(l0_new, ustack_base, ustack_new, mmu::USER_STACK_PAGES,
        mmu::PTE_AP_RW_EL0 | mmu::PTE_UXN | mmu::PTE_PXN | mmu::pte_attridx(1));

    unsafe {
        // Free the OLD image (kernel runs in TTBR1, so this is safe).
        if (*cur).user_l0 != 0 {
            mmu::free_user_tables((*cur).user_l0);
        }
        for i in 0..(*cur).exec_count as usize {
            if (*cur).exec_phys[i] != 0 {
                pmm::free_pages((*cur).exec_phys[i], (*cur).exec_pages[i]);
            }
        }
        if (*cur).utext_phys != 0 {
            pmm::free_page((*cur).utext_phys);
            (*cur).utext_phys = 0;
        }
        if (*cur).ustack_phys != 0 {
            pmm::free_pages((*cur).ustack_phys, mmu::USER_STACK_PAGES);
        }

        // Install the new image on the current task.
        (*cur).user_l0 = l0_new;
        (*cur).ttbr0 = mmu::ttbr_pack(l0_new, (*cur).pid as u16);
        (*cur).ustack_phys = ustack_new;
        (*cur).user_sp = mmu::USER_STACK_TOP;
        (*cur).exec_count = img.region_count as u32;
        for i in 0..img.region_count {
            (*cur).exec_phys[i] = img.regions[i].phys;
            (*cur).exec_pages[i] = img.regions[i].pages;
        }

        // Switch TTBR0 to the new address space now.
        crate::msr!(ttbr0_el1, (*cur).ttbr0);
        core::arch::asm!("dsb ish", "isb", "tlbi vmalle1", "dsb ish", "isb");

        // Reset SP_EL0 and rewrite the trap frame to eret into the new entry.
        crate::msr!(sp_el0, mmu::USER_STACK_TOP);
        let f = &mut *frame;
        for r in f.regs.iter_mut() {
            *r = 0;
        }
        f.elr = img.entry;
        f.spsr = 0; // EL0t
    }
    kprintln!("[EXEC] pid {} replaced image, entry={:#x}", unsafe { (*cur).pid }, img.entry);
    0
}

/// fork() — duplicate the calling EL0 task. Child gets its own copy of the
/// user text + stack, a fresh address space, and resumes inside the same SVC
/// with x0 = 0. Returns the child pid to the parent, -1 on failure.
pub fn fork(frame: *mut crate::exception::TrapFrame) -> i64 {
    let s = unsafe { SCHED.get() };
    let parent = s.current;
    if parent.is_null() || frame.is_null() {
        return -1;
    }
    // Only EL0 tasks with a user image can be forked in this model.
    let (p_utext, p_ustack, p_user_sp) = unsafe {
        ((*parent).utext_phys, (*parent).ustack_phys, (*parent).user_sp)
    };
    if p_utext == 0 || p_ustack == 0 {
        uart::errorln("[FORK] caller has no user image");
        return -1;
    }

    let t = kmalloc(core::mem::size_of::<Task>()) as *mut Task;
    if t.is_null() {
        return -1;
    }
    unsafe { core::ptr::write_bytes(t as *mut u8, 0, core::mem::size_of::<Task>()) };

    let kstack_phys = pmm::allocate_pages(TASK_STACK_PAGES);
    if kstack_phys == 0 {
        kfree(t as usize);
        return -1;
    }
    let kstack_va = phys_to_virt(kstack_phys);
    let kstack_top = kstack_va + TASK_STACK_PAGES * PAGE_SIZE;
    unsafe { core::ptr::write_bytes(kstack_va as *mut u8, 0, (TASK_STACK_PAGES * PAGE_SIZE) as usize) };

    let l0 = mmu::create_user_tables();
    if l0 == 0 {
        pmm::free_pages(kstack_phys, TASK_STACK_PAGES);
        kfree(t as usize);
        return -1;
    }

    // Child's own copy of the user text page (RO+EL0X), mapped at USER_TEXT_BASE.
    let utext_child = pmm::allocate_page();
    unsafe {
        core::ptr::copy_nonoverlapping(
            phys_to_virt(p_utext) as *const u8,
            phys_to_virt(utext_child) as *mut u8,
            PAGE_SIZE as usize,
        );
    }
    mmu::map_user_range(l0, mmu::USER_TEXT_BASE, utext_child, 1,
        mmu::PTE_AP_RO_EL0 | mmu::PTE_PXN | mmu::pte_attridx(1));

    // Child's own copy of the user stack (RW), mapped below USER_STACK_TOP.
    let ustack_child = pmm::allocate_pages(mmu::USER_STACK_PAGES);
    let ustack_size = mmu::USER_STACK_PAGES * PAGE_SIZE;
    unsafe {
        core::ptr::copy_nonoverlapping(
            phys_to_virt(p_ustack) as *const u8,
            phys_to_virt(ustack_child) as *mut u8,
            ustack_size as usize,
        );
    }
    let ustack_base = mmu::USER_STACK_TOP - ustack_size;
    mmu::map_user_range(l0, ustack_base, ustack_child, mmu::USER_STACK_PAGES,
        mmu::PTE_AP_RW_EL0 | mmu::PTE_UXN | mmu::PTE_PXN | mmu::pte_attridx(1));

    // Copy the 288-byte trap frame to the child kstack; child sees x0 = 0.
    let child_frame = (kstack_top - 288) as *mut u8;
    unsafe {
        core::ptr::copy_nonoverlapping(frame as *const u8, child_frame, 288);
        *(child_frame as *mut u64) = 0; // regs[0] = 0 in child
    }
    // context_switch frame (160B) below it; x30 = fork_return.
    let switch_frame = (child_frame as usize - 160) as *mut u64;
    let pid = s.next_pid;
    unsafe {
        core::ptr::write_bytes(switch_frame as *mut u8, 0, 160);
        *switch_frame.add(11) = fork_return as usize as u64; // x30
        (*t).sp = switch_frame as u64;
        (*t).ttbr0 = mmu::ttbr_pack(l0, pid as u16);
        (*t).pid = pid;
        (*t).state = TASK_READY;
        (*t).stack_phys = kstack_phys;
        (*t).user_sp = p_user_sp;
        (*t).ustack_phys = ustack_child;
        (*t).utext_phys = utext_child;
        (*t).user_l0 = l0;
        (*t).fds = crate::fs::vfs::fd_table_create() as u64;
        // name = parent + "+f"
        copy_name(&mut (*t).name, name_str(&*parent));
        let mut nlen = 0;
        while nlen < 16 && (*t).name[nlen] != 0 { nlen += 1; }
        if nlen + 2 < 16 {
            (*t).name[nlen] = b'+';
            (*t).name[nlen + 1] = b'f';
            (*t).name[nlen + 2] = 0;
        }
        let cur = s.current;
        let mut tail = cur;
        while (*tail).next != cur as u64 {
            tail = (*tail).next as *mut Task;
        }
        (*tail).next = t as u64;
        (*t).next = cur as u64;
    }
    s.next_pid += 1;
    kprintln!("[FORK] parent {} -> child {}", unsafe { (*parent).pid }, pid);
    pid as i64
}

pub fn schedule() {
    let s = unsafe { SCHED.get() };
    let prev = s.current;
    if prev.is_null() {
        return;
    }
    unsafe {
        let mut next = (*prev).next as *mut Task;
        while next != prev {
            if (*next).state == TASK_READY {
                break;
            }
            next = (*next).next as *mut Task;
        }
        if next == prev {
            return; // no other runnable task
        }

        if (*prev).state == TASK_RUNNING {
            (*prev).state = TASK_READY;
        }

        // If prev is dead, unlink and defer cleanup.
        if (*prev).state == TASK_DEAD {
            let mut p = prev;
            while (*p).next != prev as u64 {
                p = (*p).next as *mut Task;
            }
            (*p).next = (*prev).next;
            (*prev).next = s.dead_list as u64;
            s.dead_list = prev;
        }

        (*next).state = TASK_RUNNING;
        s.current = next;
        s.ctxt_switches += 1;
        context_switch(prev, next);
    }
}

pub fn r#yield() {
    schedule();
}

#[no_mangle]
pub extern "C" fn task_exit() {
    let s = unsafe { SCHED.get() };
    unsafe {
        let cur = &*s.current;
        kprintln!("[SCHED] Task {} '{}' exiting", cur.pid, name_str(cur));
        (*s.current).state = TASK_DEAD;
    }
    schedule();
}

pub fn sleep_ms(ms: u64) {
    let s = unsafe { SCHED.get() };
    let mut ticks_needed = ms / timer::TIMER_INTERVAL_MS;
    if ticks_needed == 0 {
        ticks_needed = 1;
    }
    unsafe {
        (*s.current).sleep_until = timer::get_ticks() + ticks_needed;
        (*s.current).state = TASK_SLEEPING;
        let cur = &*s.current;
        kprintln!(
            "[SCHED] Task {} '{}' sleeping for {} ms ({} ticks)",
            cur.pid,
            name_str(cur),
            ms,
            ticks_needed
        );
    }
    schedule();
}

/// Charge one timer tick to the currently-running task (CPU-time accounting).
pub fn account_tick() {
    let s = unsafe { SCHED.get() };
    if !s.current.is_null() {
        unsafe { (*s.current).ticks += 1 };
    }
}

pub fn wake_sleepers() {
    let s = unsafe { SCHED.get() };
    if s.current.is_null() {
        return; // scheduler not initialised yet
    }
    let now = timer::get_ticks();
    let idle = &mut s.idle as *mut Task;
    unsafe {
        let mut t = (*idle).next as *mut Task;
        while t != idle {
            if (*t).state == TASK_SLEEPING && now >= (*t).sleep_until {
                (*t).state = TASK_READY;
                (*t).sleep_until = 0;
            }
            t = (*t).next as *mut Task;
        }
    }
}

/// Demand-paged user-stack growth: on a translation fault in the stack-growth
/// zone, map a fresh RW page and let the faulting instruction resume.
pub fn try_grow_stack(far: u64) -> bool {
    let s = unsafe { SCHED.get() };
    let t = s.current;
    unsafe {
        if t.is_null() || (*t).ttbr0 == 0 {
            return false;
        }
        let stack_max_lo = mmu::USER_STACK_TOP - mmu::USER_STACK_PAGES_MAX * PAGE_SIZE;
        let stack_init_lo = mmu::USER_STACK_TOP - mmu::USER_STACK_PAGES * PAGE_SIZE;
        if far < stack_max_lo || far >= stack_init_lo {
            return false;
        }
        if (*t).stack_grown_count as usize >= 16 {
            uart::errorln("[STACK] grow refused: cap hit");
            return false;
        }
        let pa = pmm::allocate_page();
        if pa == 0 {
            return false;
        }
        core::ptr::write_bytes(phys_to_virt(pa) as *mut u8, 0, PAGE_SIZE as usize);
        let va = far & !(PAGE_SIZE - 1);
        let l0 = mmu::ttbr_baddr((*t).ttbr0);
        mmu::map_user_range(l0, va, pa, 1,
            mmu::PTE_AP_RW_EL0 | mmu::PTE_UXN | mmu::PTE_PXN | mmu::pte_attridx(1));
        // Invalidate just this (VA, ASID) entry.
        let asid = mmu::ttbr_asid((*t).ttbr0) as u64;
        let arg = (asid << 48) | (va >> 12);
        core::arch::asm!("dsb ish", "tlbi vae1, {}", "dsb ish", "isb", in(reg) arg);
        let i = (*t).stack_grown_count as usize;
        (*t).stack_grown[i] = pa;
        (*t).stack_grown_count += 1;
        kprintln!("[STACK] grew task {} by 1 page at {:#x} ({} dyn)", (*t).pid, va, (*t).stack_grown_count);
    }
    true
}

/// Kill a task by pid. Returns 0 on success, -1 if not found / not killable.
/// Render the run-queue as a /proc/tasks table.
pub fn render_tasks() -> alloc::string::String {
    use core::fmt::Write;
    let s = unsafe { SCHED.get() };
    let mut out = alloc::string::String::new();
    let _ = writeln!(out, "PID  STATE     TICKS    NAME");
    unsafe {
        let idle = &mut s.idle as *mut Task;
        let mut t = idle;
        loop {
            let state = match (*t).state {
                TASK_READY => "READY",
                TASK_RUNNING => "RUNNING",
                TASK_SLEEPING => "SLEEPING",
                _ => "DEAD",
            };
            let _ = writeln!(out, "{:<4} {:<9} {:<8} {}", (*t).pid, state, (*t).ticks, name_str(&*t));
            t = (*t).next as *mut Task;
            if t == idle {
                break;
            }
        }
    }
    let _ = writeln!(out, "ctxt-switches: {}", s.ctxt_switches);
    out
}

pub fn kill(pid: u64) -> i64 {
    let s = unsafe { SCHED.get() };
    unsafe {
        let idle = &mut s.idle as *mut Task;
        if pid == (*idle).pid {
            return -1; // never kill idle
        }
        if pid == (*s.current).pid {
            (*s.current).state = TASK_DEAD;
            schedule();
            return 0;
        }
        // Find and unlink the target from the circular queue.
        let mut prev = s.current;
        let mut t = (*prev).next as *mut Task;
        while t != s.current {
            if (*t).pid == pid {
                (*prev).next = (*t).next;
                (*t).state = TASK_DEAD;
                (*t).next = s.dead_list as u64;
                s.dead_list = t;
                return 0;
            }
            prev = t;
            t = (*t).next as *mut Task;
        }
    }
    -1
}

pub fn reap() {
    let s = unsafe { SCHED.get() };
    unsafe {
        while !s.dead_list.is_null() {
            let dead = s.dead_list;
            s.dead_list = (*dead).next as *mut Task;
            kprintln!("[SCHED] Reaping task {} '{}'", (*dead).pid, name_str(&*dead));
            if (*dead).stack_phys != 0 {
                pmm::free_pages((*dead).stack_phys, TASK_STACK_PAGES);
            }
            if (*dead).ustack_phys != 0 {
                pmm::free_pages((*dead).ustack_phys, mmu::USER_STACK_PAGES);
            }
            if (*dead).utext_phys != 0 {
                pmm::free_page((*dead).utext_phys);
            }
            if (*dead).user_l0 != 0 {
                mmu::free_user_tables((*dead).user_l0);
            }
            if (*dead).fds != 0 {
                crate::fs::vfs::fd_table_destroy((*dead).fds as *mut crate::fs::vfs::FdTable);
            }
            for i in 0..(*dead).exec_count as usize {
                if (*dead).exec_phys[i] != 0 {
                    pmm::free_pages((*dead).exec_phys[i], (*dead).exec_pages[i]);
                }
            }
            for i in 0..(*dead).stack_grown_count as usize {
                if (*dead).stack_grown[i] != 0 {
                    pmm::free_page((*dead).stack_grown[i]);
                }
            }
            kfree(dead as usize);
        }
    }
}
