#include "exception.h"
#include "mm/mmu/mmu.h"
#include "panic/panic.h"
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

static void dump_trap_frame(uint64_t type, trap_frame_t *frame) {
  uart_println("");
  uart_println("========== EXCEPTION ==========");

  uart_puts("  Type : ");
  uart_println(exception_type_str(type));

  uint64_t ec = ESR_EC(frame->esr);
  uart_puts("  Class: ");
  uart_puts(esr_class_str(ec));
  uart_puts(" (EC=0x");
  uart_puthex(ec);
  uart_println(")");

  uart_puts("  ESR_EL1 : ");
  uart_puthex(frame->esr);
  uart_println("");

  uart_puts("  ELR_EL1 : ");
  uart_puthex(frame->elr);
  uart_println("");

  uart_puts("  FAR_EL1 : ");
  uart_puthex(frame->far);
  uart_println("");

  uart_puts("  SPSR_EL1: ");
  uart_puthex(frame->spsr);
  uart_println("");

  uart_println("  Registers:");
  for (int i = 0; i < 31; i++) {
    uart_puts("    x");
    uart_putdec((uint64_t)i);
    uart_puts(" = ");
    uart_puthex(frame->regs[i]);
    uart_println("");
  }

  uart_println("===============================");
}

void exception_dispatch(uint64_t type, trap_frame_t *frame) {
  uint64_t ec = ESR_EC(frame->esr);

  switch (type) {
  case EXCEPTION_SYNC:
    switch (ec) {
    case EC_SVC_AARCH64:
      // syscall handling
      // svc number in ESR_EL1 bits [15:0]
      uart_println("[EXCEPTION] SVC call (not implemented)");
      dump_trap_frame(type, frame);
      break;

    case EC_DATA_ABORT_CUR:
    case EC_DATA_ABORT_LO:
      dump_trap_frame(type, frame);
      kernel_panic("Data abort");
      break;

    case EC_INST_ABORT_CUR:
    case EC_INST_ABORT_LO:
      dump_trap_frame(type, frame);
      kernel_panic("Instruction abort");
      break;

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

  case EXCEPTION_IRQ:
    // GIC interrupt
    uart_println("[EXCEPTION] IRQ received (not implemented)");
    break;

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

  uint64_t vbar = (uint64_t)vector_table;
  uart_puts("[EXCEPTION] VBAR_EL1 = ");
  uart_puthex(vbar);
  uart_println("");

  __asm__ __volatile__("msr vbar_el1, %0" ::"r"(vbar));
  __asm__ __volatile__("isb");

  uart_println("[EXCEPTION] Vector table installed!");
}

void exceptions_init_upper(void) {
  uart_println("[EXCEPTION] Relocating vector table to upper half");

  uint64_t vbar = PHYS_TO_VIRT((uint64_t)vector_table);
  uart_puts("[EXCEPTION] VBAR_EL1 = ");
  uart_puthex(vbar);
  uart_println("");

  __asm__ __volatile__("msr vbar_el1, %0" ::"r"(vbar));
  __asm__ __volatile__("isb");

  uart_println("[EXCEPTION] Vector table relocated!");
}
