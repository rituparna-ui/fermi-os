#include "utils.h"
#include "uart/uart.h"

uint8_t get_current_el_number() {
  // CurrentEL is a 64 bit register
  uint64_t CurrentEL = -1;

  // move to general purpose register from system register
  __asm__ __volatile__("mrs %0, CurrentEL" : "=r"(CurrentEL));

  // EL is stored in bits 2 and 3
  uint8_t el = (CurrentEL >> 2) & 0b11;
  return el;
}

char *get_el_name(uint8_t el) {
  switch (el) {
  case 0:
    return "User Space";
  case 1:
    return "Kernel Space";
  case 2:
    return "Hyper Space";
  case 3:
    return "Secure Monitor/Firmware";
  default:
    return "Invalid Exception Level";
  }
}

void print_current_el() {
  uint8_t el = get_current_el_number();
  char *el_name = get_el_name(el);

  uart_puts("Current Exception Level: ");
  uart_println(el_name);
  return;
}

void dsb_sy() {
  __asm__ volatile("dsb sy" ::: "memory");
  return;
}