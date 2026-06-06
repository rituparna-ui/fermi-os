#ifndef EXCEPTION_H
#define EXCEPTION_H

#include <stdint.h>

// Trap frame, must match the layout in vector.S
typedef struct trap_frame {
  uint64_t regs[31];
  uint64_t elr;  // exception link register (return address)
  uint64_t spsr; // saved processor state
  uint64_t esr;  // exception syndrome register
  uint64_t far;  // fault address register
} trap_frame_t;

// ESR_EL1 Exception Class (bits [31:26])
// https://esr.arm64.dev
#define ESR_EC_SHIFT 26
#define ESR_EC_MASK (0x3FULL << ESR_EC_SHIFT)
#define ESR_EC(esr) (((esr) >> ESR_EC_SHIFT) & 0x3F)

// Exception classes
#define EC_UNKNOWN 0x00
#define EC_WF_TRAPPED 0x01
#define EC_SVC_AARCH64 0x15
#define EC_HVC_AARCH64 0x16
#define EC_SMC_AARCH64 0x17
#define EC_INST_ABORT_LO 0x20
#define EC_INST_ABORT_CUR 0x21
#define EC_PC_ALIGN 0x22
#define EC_DATA_ABORT_LO 0x24
#define EC_DATA_ABORT_CUR 0x25
#define EC_SP_ALIGN 0x26
#define EC_FP_AARCH64 0x2C
#define EC_SERROR 0x2F
#define EC_BRK 0x3C

// ESR_EL1 ISS layout for Data/Instruction Abort (when EC = 0x20/0x21/0x24/0x25)
#define ESR_ISS_DFSC(esr) ((esr) & 0x3F)        // [5:0]   Data Fault Status Code
#define ESR_ISS_WNR(esr)  (((esr) >> 6) & 0x1)  // [6]     Write not Read (data abort only)
#define ESR_ISS_CM(esr)   (((esr) >> 8) & 0x1)  // [8]     Cache maintenance
#define ESR_ISS_S1PTW(esr) (((esr) >> 7) & 0x1) // [7]     Stage-1 fault on stage-2 walk
#define ESR_ISS_EA(esr)   (((esr) >> 9) & 0x1)  // [9]     External Abort

// Exception types
#define EXCEPTION_SYNC 0
#define EXCEPTION_IRQ 1
#define EXCEPTION_FIQ 2
#define EXCEPTION_SERROR 3

void exceptions_init(void);
void exceptions_init_upper(void);

#endif
