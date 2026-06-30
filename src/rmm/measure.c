#include "measure.h"

/* ---------------------------------------------------------------------------
 * measure.c — compact, dependency-free SHA-256 (FIPS 180-4) + RIM extend.
 * Used by both the RMM (EL2) to measure realm contents and the host (EL1)
 * verifier to recompute the expected RIM.
 * --------------------------------------------------------------------------- */

static const uint32_t K[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1,
    0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
    0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786,
    0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147,
    0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
    0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b,
    0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a,
    0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
    0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};

#define ROTR(x, n) (((x) >> (n)) | ((x) << (32 - (n))))

static void sha256_block(sha256_ctx *c, const uint8_t *p) {
  uint32_t w[64];
  for (int i = 0; i < 16; i++)
    w[i] = ((uint32_t)p[i * 4] << 24) | ((uint32_t)p[i * 4 + 1] << 16) |
           ((uint32_t)p[i * 4 + 2] << 8) | (uint32_t)p[i * 4 + 3];
  for (int i = 16; i < 64; i++) {
    uint32_t s0 = ROTR(w[i - 15], 7) ^ ROTR(w[i - 15], 18) ^ (w[i - 15] >> 3);
    uint32_t s1 = ROTR(w[i - 2], 17) ^ ROTR(w[i - 2], 19) ^ (w[i - 2] >> 10);
    w[i] = w[i - 16] + s0 + w[i - 7] + s1;
  }
  uint32_t a = c->h[0], b = c->h[1], cc = c->h[2], d = c->h[3];
  uint32_t e = c->h[4], f = c->h[5], g = c->h[6], h = c->h[7];
  for (int i = 0; i < 64; i++) {
    uint32_t S1 = ROTR(e, 6) ^ ROTR(e, 11) ^ ROTR(e, 25);
    uint32_t ch = (e & f) ^ ((~e) & g);
    uint32_t t1 = h + S1 + ch + K[i] + w[i];
    uint32_t S0 = ROTR(a, 2) ^ ROTR(a, 13) ^ ROTR(a, 22);
    uint32_t maj = (a & b) ^ (a & cc) ^ (b & cc);
    uint32_t t2 = S0 + maj;
    h = g; g = f; f = e; e = d + t1;
    d = cc; cc = b; b = a; a = t1 + t2;
  }
  c->h[0] += a; c->h[1] += b; c->h[2] += cc; c->h[3] += d;
  c->h[4] += e; c->h[5] += f; c->h[6] += g; c->h[7] += h;
}

void sha256_init(sha256_ctx *c) {
  c->h[0] = 0x6a09e667; c->h[1] = 0xbb67ae85;
  c->h[2] = 0x3c6ef372; c->h[3] = 0xa54ff53a;
  c->h[4] = 0x510e527f; c->h[5] = 0x9b05688c;
  c->h[6] = 0x1f83d9ab; c->h[7] = 0x5be0cd19;
  c->len = 0;
  c->buflen = 0;
}

void sha256_update(sha256_ctx *c, const void *data, size_t len) {
  const uint8_t *p = (const uint8_t *)data;
  c->len += len;
  while (len) {
    size_t n = 64 - c->buflen;
    if (n > len)
      n = len;
    for (size_t i = 0; i < n; i++)
      c->buf[c->buflen + i] = p[i];
    c->buflen += n;
    p += n;
    len -= n;
    if (c->buflen == 64) {
      sha256_block(c, c->buf);
      c->buflen = 0;
    }
  }
}

void sha256_final(sha256_ctx *c, uint8_t out[32]) {
  uint64_t bits = c->len * 8; /* capture length BEFORE padding */
  uint8_t pad = 0x80;
  sha256_update(c, &pad, 1);
  uint8_t zero = 0;
  while (c->buflen != 56)
    sha256_update(c, &zero, 1);
  uint8_t lenb[8];
  for (int i = 0; i < 8; i++)
    lenb[i] = (uint8_t)(bits >> (56 - 8 * i));
  sha256_update(c, lenb, 8);
  for (int i = 0; i < 8; i++) {
    out[i * 4] = (uint8_t)(c->h[i] >> 24);
    out[i * 4 + 1] = (uint8_t)(c->h[i] >> 16);
    out[i * 4 + 2] = (uint8_t)(c->h[i] >> 8);
    out[i * 4 + 3] = (uint8_t)(c->h[i]);
  }
}

void sha256(const void *data, size_t len, uint8_t out[32]) {
  sha256_ctx c;
  sha256_init(&c);
  sha256_update(&c, data, len);
  sha256_final(&c, out);
}

void rim_extend(uint8_t rim[32], uint64_t ipa, const void *page, size_t len) {
  sha256_ctx c;
  sha256_init(&c);
  sha256_update(&c, rim, 32);
  uint8_t ipab[8];
  for (int i = 0; i < 8; i++)
    ipab[i] = (uint8_t)(ipa >> (8 * i)); /* little-endian IPA */
  sha256_update(&c, ipab, 8);
  sha256_update(&c, page, len);
  sha256_final(&c, rim);
}
