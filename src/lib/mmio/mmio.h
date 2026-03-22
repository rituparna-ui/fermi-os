#ifndef LIB_MMIO_H
#define LIB_MMIO_H

#include <stdint.h>

void mmio_write32(uintptr_t addr, uint32_t value);
uint32_t mmio_read32(uintptr_t addr);

void mmio_write16(uintptr_t addr, uint16_t value);
uint16_t mmio_read16(uintptr_t addr);

void mmio_write8(uintptr_t addr, uint8_t value);
uint8_t mmio_read8(uintptr_t addr);

#endif