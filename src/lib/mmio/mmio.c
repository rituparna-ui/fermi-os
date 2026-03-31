#include "mmio.h"
#include "mm/mmu/mmu.h"

uintptr_t mmio_va_offset = 0;

void mmio_switch_to_upper() {
  mmio_va_offset = KERNEL_VA_OFFSET;
  return;
}

void mmio_write32(uintptr_t addr, uint32_t value) {
  *(volatile uint32_t *)(addr + mmio_va_offset) = value;
  return;
}

uint32_t mmio_read32(uintptr_t addr) {
  uint32_t value = *(volatile uint32_t *)(addr + mmio_va_offset);
  return value;
}

void mmio_write16(uintptr_t addr, uint16_t value) {
  *(volatile uint16_t *)(addr + mmio_va_offset) = value;
  return;
}

uint16_t mmio_read16(uintptr_t addr) {
  uint16_t value = *(volatile uint16_t *)(addr + mmio_va_offset);
  return value;
}

void mmio_write8(uintptr_t addr, uint8_t value) {
  *(volatile uint8_t *)(addr + mmio_va_offset) = value;
  return;
}

uint8_t mmio_read8(uintptr_t addr) {
  uint8_t value = *(volatile uint8_t *)(addr + mmio_va_offset);
  return value;
}
