#include "exception.h"
#include "gic/gic.h"
#include "mm/mmu/mmu.h"
#include "panic/panic.h"
#include "sched/sched.h"
#include "syscall/syscall.h"
#include "timer/timer.h"
#include "uart/uart.h"

extern char vector_table[];

static const char *exception_type_str(uint64_t type) {
  switch (type) {
  case EXCEPTION_SYNC:
    return "Synchronous";
  case EXCEPTION_IRQ:
    return "IRQ";
  case EXCEPTION_FIQ:
    return "FIQ";
  case EXCEPTION_SERROR:
    return "SError";
  default:
    return "Unknown";
  }
}

static const char *esr_class_str(uint64_t ec) {
  switch (ec) {
  case EC_UNKNOWN:
    return "Unknown reason";
  case EC_WF_TRAPPED:
    return "WFI/WFE trapped";
  case EC_SVC_AARCH64:
    return "SVC (AArch64)";
  case EC_HVC_AARCH64:
    return "HVC (AArch64)";
  case EC_SMC_AARCH64:
    return "SMC (AArch64)";
  case EC_INST_ABORT_LO:
    return "Instruction abort (lower EL)";
  case EC_INST_ABORT_CUR:
    return "Instruction abort (current EL)";
  case EC_PC_ALIGN:
    return "PC alignment fault";
  case EC_DATA_ABORT_LO:
    return "Data abort (lower EL)";
  case EC_DATA_ABORT_CUR:
    return "Data abort (current EL)";
  case EC_SP_ALIGN:
    return "SP alignment fault";
  case EC_FP_AARCH64:
    return "Floating point exception";
  case EC_SERROR:
    return "SError interrupt";
  case EC_BRK:
    return "BRK (debug breakpoint)";
  default:
    return "Unrecognized EC";
  }
}

/* DFSC (Data/Instruction Fault Status Code) — ESR_EL1.ISS[5:0] for aborts.
 * Per ARM ARM (DDI 0487, Table D13-9). Sparse table: undefined entries
 * fall through to "Unknown". The L0/L1/L2/L3 suffix tells which level of
 * the page-table walk the MMU reached before the fault — hugely useful
 * when the user passes a wild pointer and you want to know whether even
 * the L1 was unmapped (likely a kernel-pointer dereference) or just the
 * L3 (likely a stack overflow / unmapped user page). */
static const char *dfsc_str(uint8_t dfsc) {
  switch (dfsc) {
  case 0x00: return "Address size fault L0 / TTBR";
  case 0x01: return "Address size fault L1";
  case 0x02: return "Address size fault L2";
  case 0x03: return "Address size fault L3";
  case 0x04: return "Translation fault L0";
  case 0x05: return "Translation fault L1";
  case 0x06: return "Translation fault L2";
  case 0x07: return "Translation fault L3";
  case 0x09: return "Access flag fault L1";
  case 0x0a: return "Access flag fault L2";
  case 0x0b: return "Access flag fault L3";
  case 0x0d: return "Permission fault L1";
  case 0x0e: return "Permission fault L2";
  case 0x0f: return "Permission fault L3";
  case 0x10: return "Synchronous external abort";
  case 0x14: return "Sync ext abort on TT walk L0";
  case 0x15: return "Sync ext abort on TT walk L1";
  case 0x16: return "Sync ext abort on TT walk L2";
  case 0x17: return "Sync ext abort on TT walk L3";
  case 0x21: return "Alignment fault";
  case 0x30: return "TLB conflict abort";
  default:   return "Unknown DFSC";
  }
}

/* Classify a user-space FAR_EL1 against the well-known fixed regions of
 * the EL0 address space. Returns a static string — no allocation. The
 * goal is one-glance diagnosis of typical bugs: NULL deref, stack
 * overflow, kernel-pointer leak, etc. */
static const char *va_classify_user(uint64_t far) {
  /* TTBR1 / kernel-half VAs leaking into a user fault means the user
   * tried to dereference (or jump to) a kernel pointer. */
  if (far >= 0xFFFF000000000000ULL) {
    return "kernel-half VA (kernel-pointer leak?)";
  }

  if (far < 0x1000) {
    return "NULL-page (nullptr deref)";
  }

  /* User code/data window. sched_create_task maps [USER_TEXT_BASE,
   * __user_text_end) RO+EL0X; ELF-loaded tasks have arbitrary PT_LOAD
   * placements but typically start at USER_TEXT_BASE. */
  if (far >= USER_TEXT_BASE && far < (USER_STACK_TOP - USER_STACK_PAGES * 0x1000ULL)) {
    return "user code / data region";
  }

  /* Active stack range: [USER_STACK_TOP - 16 KiB, USER_STACK_TOP). */
  uint64_t stack_lo = USER_STACK_TOP - USER_STACK_PAGES * 0x1000ULL;
  if (far >= stack_lo && far < USER_STACK_TOP) {
    return "user stack (active)";
  }

  /* One page below the stack is the natural "guard" — it's unmapped, so
   * the very-likely cause of a fault here is stack overflow. */
  if (far >= stack_lo - 0x1000ULL && far < stack_lo) {
    return "just below user stack — STACK OVERFLOW likely";
  }

  if (far >= USER_STACK_TOP) {
    return "above user range — wild pointer";
  }

  return "user lower-half (unmapped)";
}

/* Compact, decoded user-fault dump used by both data-abort and
 * instruction-abort lower-EL handlers. Includes the live ASID
 * (TTBR0_EL1[63:48]) so multi-task fault traces are unambiguous. */
static void dump_user_abort(const char *what, task_t *t, trap_frame_t *frame) {
  uint64_t ttbr0;
  __asm__ __volatile__("mrs %0, ttbr0_el1" : "=r"(ttbr0));
  uint16_t asid = (uint16_t)(ttbr0 >> 48);

  uint8_t dfsc = (uint8_t)ESR_ISS_DFSC(frame->esr);
  uint64_t wnr = ESR_ISS_WNR(frame->esr);
  uint64_t cm  = ESR_ISS_CM(frame->esr);
  uint64_t ea  = ESR_ISS_EA(frame->esr);

  uart_printf("[FAULT] %s in task %u '%s' ASID=%u\n",
              what, t->pid, t->name, (uint64_t)asid);
  uart_printf("  ELR=%x  FAR=%x  ESR=%x  SPSR=%x\n",
              frame->elr, frame->far, frame->esr, frame->spsr);
  uart_printf("  cause: %s (DFSC=%x)%s%s%s\n",
              dfsc_str(dfsc), (uint64_t)dfsc,
              wnr ? "  write" : "  read",
              cm  ? "  cache-maint" : "",
              ea  ? "  external-abort" : "");
  uart_printf("  FAR region: %s\n", va_classify_user(frame->far));
  uart_println("  -> killing task");
}

static void dump_trap_frame(uint64_t type, trap_frame_t *frame) {
  uart_println("");
  uart_println("========== EXCEPTION ==========");

  uart_printf("  Type : %s\n", exception_type_str(type));

  uint64_t ec = ESR_EC(frame->esr);
  uart_printf("  Class: %s (EC=%x)\n", esr_class_str(ec), ec);
  uart_printf("  ESR_EL1 : %x\n", frame->esr);
  uart_printf("  ELR_EL1 : %x\n", frame->elr);
  uart_printf("  FAR_EL1 : %x\n", frame->far);
  uart_printf("  SPSR_EL1 : %x\n", frame->spsr);

  uart_println("  Registers:");
  for (int i = 0; i < 31; i++) {
    uart_printf("    x%d = %x\n", (uint64_t)i, frame->regs[i]);
  }

  uart_println("===============================");
}

void exception_dispatch(uint64_t type, trap_frame_t *frame) {
  uint64_t ec = ESR_EC(frame->esr);

  switch (type) {
  case EXCEPTION_SYNC:
    switch (ec) {
    case EC_SVC_AARCH64:
      syscall_dispatch(frame);
      break;

    case EC_DATA_ABORT_CUR:
      dump_trap_frame(type, frame);
      kernel_panic("Data abort (kernel)");
      break;

    case EC_DATA_ABORT_LO: {
      task_t *t = sched_current();
      uint8_t dfsc = (uint8_t)ESR_ISS_DFSC(frame->esr);
      /* Translation fault L1/L2/L3 in the user-stack growth zone =>
       * demand-paged stack growth. Map a page and resume; if anything
       * about the request is suspicious (out of zone, cap exhausted,
       * PMM empty) sched_try_grow_stack returns 0 and we fall through
       * to the fatal kill below. */
      if ((dfsc == 0x05 || dfsc == 0x06 || dfsc == 0x07) &&
          sched_try_grow_stack(t, frame->far)) {
        break;  /* eret resumes the faulting instruction */
      }
      dump_user_abort("data abort", t, frame);
      task_exit();
      break;
    }

    case EC_INST_ABORT_CUR:
      dump_trap_frame(type, frame);
      kernel_panic("Instruction abort (kernel)");
      break;

    case EC_INST_ABORT_LO: {
      task_t *t = sched_current();
      dump_user_abort("instruction abort", t, frame);
      task_exit();
      break;
    }

    case EC_BRK:
      uart_println("[EXCEPTION] Breakpoint hit");
      dump_trap_frame(type, frame);
      // skip 4 bytes to prevent inf loop
      frame->elr += 4;
      break;

    default:
      dump_trap_frame(type, frame);
      kernel_panic("Unhandled synchronous exception");
      break;
    }
    break;

  case EXCEPTION_IRQ: {
    // GIC interrupt
    uint32_t intid = gic_ack_irq();

    if (intid == GIC_INTID_NO_PENDING) {
      break;
    }

    gic_count_irq(intid);


    if (intid == TIMER_PPI_INTID) {
      timer_handle_irq();
    } else {
      uart_printf("[IRQ] INTID %d (not implemented)\n", (uint64_t)intid);
    }

    gic_end_irq(intid);

    // schedule after EOI so GIC can deliver future IRQs
    schedule();
    break;
  }

  case EXCEPTION_FIQ:
    dump_trap_frame(type, frame);
    kernel_panic("Unexpected FIQ");
    break;

  case EXCEPTION_SERROR:
    dump_trap_frame(type, frame);
    kernel_panic("SError (asynchronous abort)");
    break;

  default:
    dump_trap_frame(type, frame);
    kernel_panic("Unknown exception type");
    break;
  }
}

void exceptions_init(void) {
  uart_println("[EXCEPTION] Installing vector table (physical)");

  /* Before MMU: (uint64_t)vector_table is PC-relative → physical */
  uint64_t vbar = (uint64_t)vector_table;
  uart_printf("[EXCEPTION] VBAR_EL1 = %x\n", vbar);

  __asm__ __volatile__("msr vbar_el1, %0" ::"r"(vbar));
  __asm__ __volatile__("isb");

  uart_println("[EXCEPTION] Vector table installed!");
}

void exceptions_init_upper(void) {
  uart_println("[EXCEPTION] Relocating vector table to upper half");

  /* With -fno-pic, &vector_table via adrp+add is PC-relative: physical
     before MMU, upper-half after MMU. We're already in upper half here. */
  uint64_t vbar = (uint64_t)vector_table;
  uart_printf("[EXCEPTION] VBAR_EL1 = %x\n", vbar);

  __asm__ __volatile__("msr vbar_el1, %0" ::"r"(vbar));
  __asm__ __volatile__("isb");

  uart_println("[EXCEPTION] Vector table relocated!");
}
