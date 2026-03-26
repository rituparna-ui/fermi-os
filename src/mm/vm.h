/*
 * vm.h — Virtual-memory layout constants for higher-half kernel
 *
 * TTBR0_EL1 (lower half):  0x0000_0000_0000_0000 – 0x0000_FFFF_FFFF_FFFF
 *   → identity map (phys == virt), used only during early boot and for
 *     user-space in the future.
 *
 * TTBR1_EL1 (upper half):  0xFFFF_0000_0000_0000 – 0xFFFF_FFFF_FFFF_FFFF
 *   → kernel: VA 0xFFFF_0000_0000_0000 + PA
 *
 * With T0SZ = T1SZ = 16 the hardware uses bit [55] to select the
 * translation regime (0 → TTBR0, 1 → TTBR1).
 */

#ifndef MM_VM_H
#define MM_VM_H

#include <stdint.h>

/* Base virtual address of the kernel (upper half) */
#define KERNEL_VIRT_BASE    0xFFFF000000000000ULL

/* Physical → Virtual and back */
#define PHYS_TO_VIRT(pa)    ((pa) + KERNEL_VIRT_BASE)
#define VIRT_TO_PHYS(va)    ((va) - KERNEL_VIRT_BASE)

/* Same macros but yielding a typed pointer */
#define PA_TO_KVA(pa)       ((void *)((uintptr_t)(pa) + KERNEL_VIRT_BASE))
#define KVA_TO_PA(va)       ((uintptr_t)(va) - KERNEL_VIRT_BASE)

#endif /* MM_VM_H */
