#ifndef RMM_MEASURE_H
#define RMM_MEASURE_H

#include <stddef.h>
#include <stdint.h>

/* ---------------------------------------------------------------------------
 * measure.h — SHA-256 + Realm Initial Measurement (RIM)
 *
 * The RMM measures everything loaded into a realm before it runs, building a
 * RIM: a hash chain that cryptographically fixes the realm's initial state.
 * A relying party can later check an attestation token over the RIM to know
 * *exactly* what code/data a realm started with before trusting it with
 * secrets. Pure computation — safe to call at EL2 (MMU off) or EL1.
 * --------------------------------------------------------------------------- */

typedef struct {
  uint32_t h[8];
  uint64_t len; /* total bytes hashed */
  uint8_t buf[64];
  size_t buflen;
} sha256_ctx;

void sha256_init(sha256_ctx *c);
void sha256_update(sha256_ctx *c, const void *data, size_t len);
void sha256_final(sha256_ctx *c, uint8_t out[32]);
void sha256(const void *data, size_t len, uint8_t out[32]);

/* RIM extension: rim = SHA-256( rim(32) || ipa_le64(8) || page(len) ). The
 * same scheme used by the host verifier, so an independently computed RIM
 * over the loaded image must match the RMM's. */
void rim_extend(uint8_t rim[32], uint64_t ipa, const void *page, size_t len);

#endif /* RMM_MEASURE_H */
