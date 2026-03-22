#include "pci/pci.h"
#include "rng/rng.h"
#include "uart/uart.h"
#include "utils/utils.h"

void kernel_main() {
  uart_init();

  uart_println("Fermi OS - Booting Up...");
  print_current_el();

  pci_enumerate_bus();

  pci_virtio_rng_init();

  while (1) {
    uart_putc(uart_getc());
  }
}
