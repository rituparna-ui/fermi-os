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
}

extern "C" {
    fn context_switch(prev: *mut Task, next: *mut Task);
    fn task_trampoline();
    fn user_trampoline();
    static user_prog_start: u8;
    static user_prog_end: u8;
}

struct Sched {
    idle: Task,
    current: *mut Task,
    next_pid: u64,
    dead_list: *mut Task,
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
    },
    current: core::ptr::null_mut(),
    next_pid: 0,
    dead_list: core::ptr::null_mut(),
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

/// Pointer to the currently running task.
pub fn current() -> *mut Task {
    unsafe { SCHED.get() }.current
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

pub fn wake_sleepers() {
    let s = unsafe { SCHED.get() };
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

/// Kill a task by pid. Returns 0 on success, -1 if not found / not killable.
/// Render the run-queue as a /proc/tasks table.
pub fn render_tasks() -> alloc::string::String {
    use core::fmt::Write;
    let s = unsafe { SCHED.get() };
    let mut out = alloc::string::String::new();
    let _ = writeln!(out, "PID  STATE     NAME");
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
            let _ = writeln!(out, "{:<4} {:<9} {}", (*t).pid, state, name_str(&*t));
            t = (*t).next as *mut Task;
            if t == idle {
                break;
            }
        }
    }
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
            kfree(dead as usize);
        }
    }
}
