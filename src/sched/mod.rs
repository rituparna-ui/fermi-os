//! Preemptive round-robin scheduler with EL0 user tasks and EL1 kernel tasks.
//!
//! Circular run queue, timer-driven preemption, per-task kernel stacks, and
//! context switching via callee-saved register save/restore (switch.S). User
//! tasks get their own TTBR0 page tables (per-task ASID), a user text+stack
//! mapping, and an fd table; a trampoline `eret`s them to EL0. Kernel tasks run
//! at EL1 with no TTBR0 swap. Supports sleep/wake, fork, kill, demand-paged
//! stack growth, and recursive resource teardown on reap.
//!
//! Concurrency (§2.5): the run queue is mutated only with IRQs masked on a
//! single core, so it lives in `static mut` raw pointers, not behind a lock.

#![allow(dead_code)]

pub mod elf;

use crate::exception::{self, TrapFrame};
use crate::fs::vfs::{self, FdTable};
use crate::mm::consts::*;
use crate::mm::{heap, mmu, pmm};
use crate::kprintln;
use crate::sched::elf::ElfImage;
use core::arch::global_asm;
use core::ptr;

global_asm!(include_str!("switch.S"));

extern "C" {
    // context_switch only touches Task.sp (offset 0) and Task.ttbr0 (offset
    // 40), so it's declared over opaque pointers to avoid an FFI-safety lint on
    // the non-repr-transparent Task (which embeds ElfImage).
    fn context_switch(prev: *mut u8, next: *mut u8);
    fn task_trampoline();
    fn kernel_task_trampoline();
    fn fork_return();
}

/// 16 KiB per-task kernel stack.
pub const TASK_STACK_PAGES: u64 = 4;
/// Hard cap for demand-paged user stack growth: 64 pages = 256 KiB.
pub const USER_STACK_PAGES_MAX: u64 = 64;
pub const USER_STACK_GROWN_MAX: usize = (USER_STACK_PAGES_MAX - USER_STACK_PAGES) as usize;

#[derive(Clone, Copy, PartialEq, Eq)]
#[repr(u32)]
pub enum TaskState {
    Ready,
    Running,
    Sleeping,
    Dead,
}

pub type TaskEntry = extern "C" fn();

/// A schedulable task. `#[repr(C)]`; `sp` MUST be at offset 0 and `ttbr0` at
/// offset 40 — switch.S reads them as TASK_SP / TASK_TTBR0.
#[repr(C)]
pub struct Task {
    pub sp: u64,           // 0  : kernel SP (context_switch saves/restores)
    pub pid: u64,          // 8
    pub state: TaskState,  // 16 (+4 pad)
    pub sleep_until: u64,  // 24
    pub stack_phys: u64,   // 32
    pub ttbr0: u64,        // 40 : user page-table base + ASID (0 for EL1 tasks)
    pub user_sp: u64,      // 48
    pub kstack_top: u64,   // 56
    pub ustack_phys: u64,  // 64
    pub exec_image: ElfImage,
    pub name: [u8; 16],
    pub fds: *mut FdTable,
    pub next: *mut Task,
    pub stack_grown_phys: [u64; USER_STACK_GROWN_MAX],
    pub stack_grown_count: u16,
}

// Freeze asm-critical offsets.
const _: () = assert!(core::mem::offset_of!(Task, sp) == 0);
const _: () = assert!(core::mem::offset_of!(Task, ttbr0) == 40);

impl Task {
    fn zeroed() -> Self {
        Self {
            sp: 0,
            pid: 0,
            state: TaskState::Ready,
            sleep_until: 0,
            stack_phys: 0,
            ttbr0: 0,
            user_sp: 0,
            kstack_top: 0,
            ustack_phys: 0,
            exec_image: ElfImage::empty(),
            name: [0; 16],
            fds: ptr::null_mut(),
            next: ptr::null_mut(),
            stack_grown_phys: [0; USER_STACK_GROWN_MAX],
            stack_grown_count: 0,
        }
    }
}

extern "C" {
    static __text_start: u8;
    static __user_text_end: u8;
}

// Scheduler state — single-core, IRQ-masked mutation, hence raw statics. The
// idle task is heap-allocated at init (it's too big for a static initializer
// with the ElfImage + grown-page arrays, and lives forever anyway).
static mut IDLE_TASK: *mut Task = ptr::null_mut();
static mut CURRENT: *mut Task = ptr::null_mut();
static mut NEXT_PID: u64 = 0;
static mut DEAD_LIST: *mut Task = ptr::null_mut();
static mut NEXT_ASID: u16 = 1;

fn copy_name(dst: &mut [u8; 16], src: &str) {
    let mut i = 0;
    for b in src.bytes() {
        if i >= 15 {
            break;
        }
        dst[i] = b;
        i += 1;
    }
}

fn name_str(name: &[u8; 16]) -> &str {
    let len = name.iter().position(|&b| b == 0).unwrap_or(16);
    core::str::from_utf8(&name[..len]).unwrap_or("?")
}

#[inline]
fn irq_save() -> u64 {
    let daif: u64;
    unsafe {
        core::arch::asm!("mrs {}, daif", out(reg) daif, options(nomem, nostack));
        core::arch::asm!("msr daifset, #2", options(nomem, nostack));
    }
    daif
}

#[inline]
fn irq_restore(daif: u64) {
    unsafe { core::arch::asm!("msr daif, {}", in(reg) daif, options(nomem, nostack)) };
}

/// Allocate a fresh ASID in [1, 65535]; flush all TLBs on wraparound.
pub fn asid_alloc() -> u16 {
    // SAFETY (single-core): boot/creation path.
    unsafe {
        let a = NEXT_ASID;
        NEXT_ASID = NEXT_ASID.wrapping_add(1);
        if NEXT_ASID == 0 {
            core::arch::asm!("tlbi vmalle1", "dsb ish", "isb");
            NEXT_ASID = 1;
            kprintln!("[SCHED] ASID space wrapped — flushed all TLBs");
        }
        a
    }
}

/// Allocate a zeroed Task on the heap.
fn alloc_task() -> *mut Task {
    let raw = heap::kmalloc(core::mem::size_of::<Task>()) as *mut Task;
    if !raw.is_null() {
        unsafe { ptr::write(raw, Task::zeroed()) };
    }
    raw
}

/// Initialize the scheduler and register its hooks.
pub fn init() {
    kprintln!("[SCHED] Initializing scheduler");
    // SAFETY (single-core): boot-time init.
    unsafe {
        let idle = alloc_task();
        (*idle).pid = NEXT_PID;
        NEXT_PID += 1;
        // READY (not RUNNING) so schedule() can pick idle as a fallback.
        (*idle).state = TaskState::Ready;
        (*idle).next = idle; // circular
        copy_name(&mut (*idle).name, "idle");
        IDLE_TASK = idle;
        CURRENT = idle;
    }

    exception::set_schedule_hook(schedule_hook);
    exception::timer::set_wake_sleepers(wake_sleepers);

    kprintln!("[SCHED] Initialized! Idle task registered");
}

/// Insert `t` at the tail of the circular run queue (IRQs masked by caller or
/// during boot).
unsafe fn enqueue(t: *mut Task) {
    let current = CURRENT;
    let mut tail = current;
    while (*tail).next != current {
        tail = (*tail).next;
    }
    (*tail).next = t;
    (*t).next = current;
}

/// Create an EL1 kernel-mode task.
pub fn create_kernel_task(name: &str, entry: TaskEntry) -> i64 {
    let t = alloc_task();
    if t.is_null() {
        crate::klib::uart::Uart.errorln("[SCHED] Failed to allocate kernel task struct");
        return -1;
    }
    let kstack_phys = pmm::allocate_pages(TASK_STACK_PAGES);
    if kstack_phys == 0 {
        crate::klib::uart::Uart.errorln("[SCHED] Failed to allocate kernel-task stack");
        heap::kfree(t as *mut u8);
        return -1;
    }
    let kstack_va = phys_to_virt(kstack_phys);
    let kstack_size = TASK_STACK_PAGES * PAGE_SIZE;
    let kstack_top = kstack_va + kstack_size;

    unsafe {
        core::ptr::write_bytes(kstack_va as *mut u8, 0, kstack_size as usize);
        // 160-byte context-switch frame; d8-d15 stay zero.
        let frame = (kstack_top - 160) as *mut u64;
        frame.add(0).write(entry as usize as u64); // x19 = entry
        frame.add(11).write(kernel_task_trampoline as usize as u64); // x30

        (*t).sp = frame as u64;
        (*t).pid = NEXT_PID;
        NEXT_PID += 1;
        (*t).state = TaskState::Ready;
        (*t).stack_phys = kstack_phys;
        (*t).ttbr0 = 0; // EL1: no TTBR0 swap
        (*t).kstack_top = kstack_top;
        copy_name(&mut (*t).name, name);

        let daif = irq_save();
        enqueue(t);
        irq_restore(daif);

        kprintln!(
            "[SCHED] Created EL1 kernel task {} '{}' | kstack: {:#x} | entry: {:#x}",
            (*t).pid,
            name,
            kstack_top,
            entry as usize as u64
        );
        (*t).pid as i64
    }
}

/// Create an EL0 user task running the kernel-shared `entry` image at EL0.
pub fn create_task(name: &str, entry: TaskEntry) -> i64 {
    let uart = crate::klib::uart::Uart;
    let t = alloc_task();
    if t.is_null() {
        uart.errorln("[SCHED] Failed to allocate task struct");
        return -1;
    }

    let kstack_phys = pmm::allocate_pages(TASK_STACK_PAGES);
    if kstack_phys == 0 {
        uart.errorln("[SCHED] Failed to allocate kernel stack");
        heap::kfree(t as *mut u8);
        return -1;
    }
    let kstack_va = phys_to_virt(kstack_phys);
    let kstack_size = TASK_STACK_PAGES * PAGE_SIZE;
    let kstack_top = kstack_va + kstack_size;
    unsafe { core::ptr::write_bytes(kstack_va as *mut u8, 0, kstack_size as usize) };

    let user_l0 = mmu::create_user_tables();
    if user_l0 == 0 {
        uart.errorln("[SCHED] Failed to create user page tables");
        pmm::free_pages(kstack_phys, TASK_STACK_PAGES);
        heap::kfree(t as *mut u8);
        return -1;
    }

    // Map [__text_start, __user_text_end) as a contiguous RO+EL0X window at
    // USER_TEXT_BASE. The entry fn's offset within .text becomes the user entry.
    let entry_pa = virt_to_phys(entry as usize as u64);
    let text_start_pa = virt_to_phys(core::ptr::addr_of!(__text_start) as u64);
    let user_text_end_pa = virt_to_phys(core::ptr::addr_of!(__user_text_end) as u64);

    if entry_pa < text_start_pa || entry_pa >= user_text_end_pa {
        uart.errorln("[SCHED] entry outside [__text_start, __user_text_end)");
        pmm::free_page(user_l0);
        pmm::free_pages(kstack_phys, TASK_STACK_PAGES);
        heap::kfree(t as *mut u8);
        return -1;
    }
    let text_pages = (user_text_end_pa - text_start_pa) / PAGE_SIZE;
    let entry_offset = entry_pa - text_start_pa;
    let text_flags = pte_attridx(1) | PTE_AP_RO_EL0 | PTE_PXN;
    mmu::map_user_range(user_l0, USER_TEXT_BASE, text_start_pa, text_pages, text_flags);
    let user_entry = USER_TEXT_BASE + entry_offset;

    // User stack.
    let ustack_phys = pmm::allocate_pages(USER_STACK_PAGES);
    if ustack_phys == 0 {
        uart.errorln("[SCHED] Failed to allocate user stack");
        mmu::free_user_tables(user_l0);
        pmm::free_pages(kstack_phys, TASK_STACK_PAGES);
        heap::kfree(t as *mut u8);
        return -1;
    }
    unsafe {
        core::ptr::write_bytes(
            phys_to_virt(ustack_phys) as *mut u8,
            0,
            (USER_STACK_PAGES * PAGE_SIZE) as usize,
        );
    }
    let ustack_user_base = USER_STACK_TOP - USER_STACK_PAGES * PAGE_SIZE;
    let stack_flags = pte_attridx(1) | PTE_AP_RW_EL0 | PTE_UXN | PTE_PXN;
    mmu::map_user_range(user_l0, ustack_user_base, ustack_phys, USER_STACK_PAGES, stack_flags);

    unsafe {
        // Context-switch frame: x19 = user entry, x20 = user SP, x30 = trampoline.
        let frame = (kstack_top - 160) as *mut u64;
        frame.add(0).write(user_entry);
        frame.add(1).write(USER_STACK_TOP);
        frame.add(11).write(task_trampoline as usize as u64);

        (*t).sp = frame as u64;
        (*t).pid = NEXT_PID;
        NEXT_PID += 1;
        (*t).state = TaskState::Ready;
        (*t).stack_phys = kstack_phys;
        (*t).ttbr0 = ttbr_pack(user_l0, asid_alloc());
        (*t).user_sp = USER_STACK_TOP;
        (*t).kstack_top = kstack_top;
        (*t).ustack_phys = ustack_phys;
        copy_name(&mut (*t).name, name);

        // Per-task fd table: fd 0/1/2 -> /dev/console.
        (*t).fds = vfs::fd_table_create();
        if !(*t).fds.is_null() {
            vfs::fd_open((*t).fds, "/dev/console");
            vfs::fd_open((*t).fds, "/dev/console");
            vfs::fd_open((*t).fds, "/dev/console");
        }

        let daif = irq_save();
        enqueue(t);
        irq_restore(daif);

        kprintln!(
            "[SCHED] Created EL0 task {} '{}' | kstack: {:#x} | user_entry: {:#x}",
            (*t).pid,
            name,
            kstack_top,
            user_entry
        );
        (*t).pid as i64
    }
}

/// Pick the next READY task and switch to it.
pub fn schedule() {
    reap();
    // SAFETY (single-core): callers mask IRQs or are in IRQ context.
    unsafe {
        let prev = CURRENT;
        let idle = IDLE_TASK;
        let mut next = (*prev).next;
        let mut fallback: *mut Task = ptr::null_mut();

        while next != prev {
            if (*next).state == TaskState::Ready {
                if next == idle {
                    if fallback.is_null() {
                        fallback = next;
                    }
                    next = (*next).next;
                    continue;
                }
                break;
            }
            next = (*next).next;
        }

        if next == prev {
            if fallback.is_null() || (*prev).state == TaskState::Running {
                return; // prev still runnable; keep its timeslice
            }
            next = fallback; // prev dead/blocked — fall back to idle
        }

        if (*prev).state == TaskState::Running {
            (*prev).state = TaskState::Ready;
        }

        // If prev died, unlink it and push onto the dead list.
        if (*prev).state == TaskState::Dead {
            let mut p = prev;
            while (*p).next != prev {
                p = (*p).next;
            }
            (*p).next = (*prev).next;
            (*prev).next = DEAD_LIST;
            DEAD_LIST = prev;
        }

        (*next).state = TaskState::Running;
        CURRENT = next;
        context_switch(prev as *mut u8, next as *mut u8);
    }
}

extern "C" fn schedule_hook() {
    schedule();
}

pub fn r#yield() {
    schedule();
}

/// Terminate the current task.
#[no_mangle]
pub extern "C" fn task_exit() {
    unsafe {
        kprintln!(
            "[SCHED] Task {} '{}' exiting",
            (*CURRENT).pid,
            name_str(&(*CURRENT).name)
        );
        (*CURRENT).state = TaskState::Dead;
    }
    schedule();
}

/// Kill the task with `pid`. Returns 0 on success, -1 otherwise.
pub fn kill_task(pid: u64) -> i64 {
    if pid == 0 {
        crate::klib::uart::Uart.errorln("[SCHED] kill: refusing to kill idle (pid 0)");
        return -1;
    }
    // SAFETY (single-core): traversal + mutation under IRQ mask.
    unsafe {
        let head = IDLE_TASK;
        let mut t = head;
        loop {
            if (*t).pid == pid {
                if (*t).state == TaskState::Dead {
                    return -1;
                }
                if t == CURRENT {
                    task_exit();
                    return 0;
                }
                let daif = irq_save();
                kprintln!("[SCHED] Killing task {} '{}'", pid, name_str(&(*t).name));
                let mut p = t;
                while (*p).next != t {
                    p = (*p).next;
                }
                (*p).next = (*t).next;
                (*t).next = DEAD_LIST;
                DEAD_LIST = t;
                (*t).state = TaskState::Dead;
                irq_restore(daif);
                return 0;
            }
            t = (*t).next;
            if t == head {
                break;
            }
        }
    }
    -1
}

/// Sleep the current task for `ms` milliseconds (>= one tick).
pub fn sleep_ms(ms: u64) {
    let ticks = core::cmp::max(1, ms / exception::timer::TIMER_INTERVAL_MS);
    unsafe {
        (*CURRENT).sleep_until = exception::timer::get_ticks() + ticks;
        (*CURRENT).state = TaskState::Sleeping;
        kprintln!(
            "[SCHED] Task {} '{}' sleeping for {} ms ({} ticks)",
            (*CURRENT).pid,
            name_str(&(*CURRENT).name),
            ms,
            ticks
        );
    }
    schedule();
}

/// Wake sleeping tasks whose deadline passed (called from the timer IRQ).
pub fn wake_sleepers() {
    let now = exception::timer::get_ticks();
    unsafe {
        let idle = IDLE_TASK;
        let mut t = (*idle).next;
        while t != idle {
            if (*t).state == TaskState::Sleeping && now >= (*t).sleep_until {
                (*t).state = TaskState::Ready;
                (*t).sleep_until = 0;
            }
            t = (*t).next;
        }
    }
}

/// Demand-paged user stack growth. Called from the EL0 data-abort path before
/// the fatal kill. Returns true if a page was mapped (resume the task).
pub fn try_grow_stack(t: *mut Task, far: u64) -> bool {
    // SAFETY: t is the current task; ttbr0 nonzero for user tasks.
    unsafe {
        if t.is_null() || (*t).ttbr0 == 0 {
            return false;
        }
        let stack_max_lo = USER_STACK_TOP - USER_STACK_PAGES_MAX * PAGE_SIZE;
        let stack_initial_lo = USER_STACK_TOP - USER_STACK_PAGES * PAGE_SIZE;
        if far < stack_max_lo || far >= stack_initial_lo {
            return false;
        }
        if (*t).stack_grown_count as usize >= USER_STACK_GROWN_MAX {
            crate::klib::uart::Uart.errorln("[STACK] grow refused: USER_STACK_GROWN_MAX hit");
            return false;
        }
        let pa = pmm::allocate_page();
        if pa == 0 {
            crate::klib::uart::Uart.errorln("[STACK] grow refused: PMM exhausted");
            return false;
        }
        core::ptr::write_bytes(phys_to_virt(pa) as *mut u8, 0, PAGE_SIZE as usize);

        let va = far & !(PAGE_SIZE - 1);
        let flags = pte_attridx(1) | PTE_AP_RW_EL0 | PTE_UXN | PTE_PXN;
        let l0 = ttbr_baddr((*t).ttbr0);
        mmu::map_user_range(l0, va, pa, 1, flags);

        let asid = ttbr_asid((*t).ttbr0);
        let arg = ((asid as u64) << TTBR_ASID_SHIFT) | (va >> 12);
        core::arch::asm!("dsb ish", "tlbi vae1, {}", "dsb ish", "isb", in(reg) arg);

        let idx = (*t).stack_grown_count as usize;
        (*t).stack_grown_phys[idx] = pa;
        (*t).stack_grown_count += 1;

        kprintln!(
            "[STACK] grew task {} '{}' by 1 page at {:#x} (now {} dyn pages)",
            (*t).pid,
            name_str(&(*t).name),
            va,
            (*t).stack_grown_count
        );
        true
    }
}

/// Free dead tasks' resources.
pub fn reap() {
    unsafe {
        loop {
            let dead = DEAD_LIST;
            if dead.is_null() {
                break;
            }
            DEAD_LIST = (*dead).next;

            kprintln!("[SCHED] Reaping task {} '{}'", (*dead).pid, name_str(&(*dead).name));

            if (*dead).stack_phys != 0 {
                pmm::free_pages((*dead).stack_phys, TASK_STACK_PAGES);
            }
            if (*dead).ustack_phys != 0 {
                pmm::free_pages((*dead).ustack_phys, USER_STACK_PAGES);
            }
            // Free per-exec ELF regions.
            for i in 0..(*dead).exec_image.region_count {
                let r = (*dead).exec_image.regions[i];
                if r.phys != 0 && r.pages != 0 {
                    pmm::free_pages(r.phys, r.pages);
                }
            }
            // Free demand-grown stack pages.
            for i in 0..(*dead).stack_grown_count as usize {
                if (*dead).stack_grown_phys[i] != 0 {
                    pmm::free_page((*dead).stack_grown_phys[i]);
                }
            }
            // Invalidate the dead task's ASID before freeing its page tables.
            if (*dead).ttbr0 != 0 {
                let asid = ttbr_asid((*dead).ttbr0);
                if asid != 0 {
                    let arg = (asid as u64) << TTBR_ASID_SHIFT;
                    core::arch::asm!("tlbi aside1, {}", "dsb ish", "isb", in(reg) arg);
                }
                mmu::free_user_tables(ttbr_baddr((*dead).ttbr0));
            }
            if !(*dead).fds.is_null() {
                vfs::fd_table_destroy((*dead).fds);
            }
            heap::kfree(dead as *mut u8);
        }
    }
}

pub fn current() -> *mut Task {
    unsafe { CURRENT }
}

/// The task's name as a &str (for logging). The returned slice borrows the
/// task's name buffer; it stays valid as long as the task is alive (the kernel
/// does not free a task mid-syscall on a single core).
pub fn task_name<'a>(t: *mut Task) -> &'a str {
    // SAFETY: caller passes a live task pointer.
    unsafe { name_str(&(*t).name) }
}

pub fn first_task() -> *mut Task {
    unsafe { IDLE_TASK }
}

pub fn state_name(s: TaskState) -> &'static str {
    match s {
        TaskState::Ready => "READY",
        TaskState::Running => "RUNNING",
        TaskState::Sleeping => "SLEEPING",
        TaskState::Dead => "DEAD",
    }
}

/// Fork the calling task. Returns the child pid to the parent; the child, when
/// first scheduled, returns 0 from the same SVC via fork_return.
pub fn fork(parent: *mut Task, frame: *mut TrapFrame) -> i64 {
    let uart = crate::klib::uart::Uart;
    if parent.is_null() || frame.is_null() {
        return -1;
    }
    let t = alloc_task();
    if t.is_null() {
        uart.errorln("[FORK] Failed to allocate child task struct");
        return -1;
    }

    let kstack_phys = pmm::allocate_pages(TASK_STACK_PAGES);
    if kstack_phys == 0 {
        uart.errorln("[FORK] Failed to allocate child kernel stack");
        heap::kfree(t as *mut u8);
        return -1;
    }
    let kstack_va = phys_to_virt(kstack_phys);
    let kstack_size = TASK_STACK_PAGES * PAGE_SIZE;
    let kstack_top = kstack_va + kstack_size;
    unsafe { core::ptr::write_bytes(kstack_va as *mut u8, 0, kstack_size as usize) };

    let user_l0 = mmu::create_user_tables();
    if user_l0 == 0 {
        uart.errorln("[FORK] Failed to allocate child user_l0");
        pmm::free_pages(kstack_phys, TASK_STACK_PAGES);
        heap::kfree(t as *mut u8);
        return -1;
    }

    // Re-map the shared .text + .rodata window (RO at EL0, same physical pages).
    let text_start_pa = virt_to_phys(core::ptr::addr_of!(__text_start) as u64);
    let user_text_end_pa = virt_to_phys(core::ptr::addr_of!(__user_text_end) as u64);
    let text_pages = (user_text_end_pa - text_start_pa) / PAGE_SIZE;
    let text_flags = pte_attridx(1) | PTE_AP_RO_EL0 | PTE_PXN;
    mmu::map_user_range(user_l0, USER_TEXT_BASE, text_start_pa, text_pages, text_flags);

    // Fresh user stack, copied from the parent.
    let ustack_phys = pmm::allocate_pages(USER_STACK_PAGES);
    if ustack_phys == 0 {
        uart.errorln("[FORK] Failed to allocate child user stack");
        mmu::free_user_tables(user_l0);
        pmm::free_pages(kstack_phys, TASK_STACK_PAGES);
        heap::kfree(t as *mut u8);
        return -1;
    }
    unsafe {
        core::ptr::copy_nonoverlapping(
            phys_to_virt((*parent).ustack_phys) as *const u8,
            phys_to_virt(ustack_phys) as *mut u8,
            (USER_STACK_PAGES * PAGE_SIZE) as usize,
        );
    }
    let ustack_user_base = USER_STACK_TOP - USER_STACK_PAGES * PAGE_SIZE;
    let stack_flags = pte_attridx(1) | PTE_AP_RW_EL0 | PTE_UXN | PTE_PXN;
    mmu::map_user_range(user_l0, ustack_user_base, ustack_phys, USER_STACK_PAGES, stack_flags);

    unsafe {
        // Copy the parent's 688-byte trap frame to the top of the child kstack;
        // clobber x0=0 so the child sees 0 from the SVC.
        let child_frame = (kstack_top - 688) as *mut u8;
        core::ptr::copy_nonoverlapping(frame as *const u8, child_frame, 688);
        (child_frame as *mut u64).write(0); // regs[0] = 0

        // 160-byte context-switch frame just below it; x30 = fork_return.
        let switch_frame = (child_frame as u64 - 160) as *mut u64;
        switch_frame.add(0).write(0); // x19
        switch_frame.add(1).write(0); // x20
        switch_frame.add(11).write(fork_return as usize as u64); // x30

        copy_name(&mut (*t).name, name_str(&(*parent).name));
        // Append "+f" if room.
        let nlen = (*t).name.iter().position(|&b| b == 0).unwrap_or(16);
        if nlen + 2 < 16 {
            (*t).name[nlen] = b'+';
            (*t).name[nlen + 1] = b'f';
        }

        (*t).sp = switch_frame as u64;
        (*t).pid = NEXT_PID;
        NEXT_PID += 1;
        (*t).state = TaskState::Ready;
        (*t).stack_phys = kstack_phys;
        (*t).ttbr0 = ttbr_pack(user_l0, asid_alloc());
        (*t).user_sp = (*parent).user_sp;
        (*t).kstack_top = kstack_top;
        (*t).ustack_phys = ustack_phys;

        (*t).fds = vfs::fd_table_create();
        if !(*t).fds.is_null() {
            vfs::fd_open((*t).fds, "/dev/console");
            vfs::fd_open((*t).fds, "/dev/console");
            vfs::fd_open((*t).fds, "/dev/console");
        }

        let daif = irq_save();
        enqueue(t);
        irq_restore(daif);

        kprintln!(
            "[FORK] {} '{}' -> child {} '{}'",
            (*parent).pid,
            name_str(&(*parent).name),
            (*t).pid,
            name_str(&(*t).name)
        );
        (*t).pid as i64
    }
}
