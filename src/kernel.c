#include "pmm/pmm.h"
#include "uart/uart.h"
#include "utils/utils.h"
#include <stdint.h>

void kernel_main() {
  uart_init();

  uart_println("Fermi OS - Booting Up...");
  print_current_el();

  pmm_init(MEM_START, MEM_SIZE);

  uintptr_t addr1 = pmm_allocate_page();
  uart_puthex(addr1);
  uart_println("");

  uintptr_t addr2 = pmm_allocate_page();
  uart_puthex(addr2);
  uart_println("");
  pmm_free_page(addr2);

  uintptr_t addr3 = pmm_allocate_page();
  uart_puthex(addr3);
  uart_println("");
  // not double free... addr3 got reassigned to addr2
  pmm_free_page(addr2);
  pmm_free_page(addr1);
  // error here
  pmm_free_page(addr3);

  while (1) {
    uart_putc(uart_getc());
  }
}
