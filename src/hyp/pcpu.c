#include "pcpu.h"

/* The per-pCPU control blocks. In .bss -> Normal-WB Inner-Shareable once the EL2
 * MMU is on, so cross-core reads (e.g. `online`) are coherent. */
hyp_pcpu_t hyp_pcpus[HYP_MAX_PCPUS];

/* hyp_boot.S indexes hyp_pcpus[] with the hand-coded PCPU_STRUCT_SIZE; keep it
 * honest. */
_Static_assert(sizeof(hyp_pcpu_t) == PCPU_STRUCT_SIZE,
               "PCPU_STRUCT_SIZE out of sync with hyp_pcpu_t");
