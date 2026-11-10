/* fself: wrap an ELF as a fake-signed SELF, the format of the ps5-payload-sdk's make_fself.py
 * (John Törnblom; GPL-3.0). Byte-identical to it for the same input and options.
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include "fself.h"

#include <stdint.h>
#include <string.h>

/* ---- SHA-256 (FIPS 180-4) ---- */
static const uint32_t k_k[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};

#define ROR(x, n) (((x) >> (n)) | ((x) << (32 - (n))))

static void sha256_block(uint32_t h[8], const uint8_t *p) {
  uint32_t w[64];
  for (int i = 0; i < 16; i++)
    w[i] = (uint32_t)p[4 * i] << 24 | (uint32_t)p[4 * i + 1] << 16 | (uint32_t)p[4 * i + 2] << 8 |
           p[4 * i + 3];
  for (int i = 16; i < 64; i++) {
    uint32_t s0 = ROR(w[i - 15], 7) ^ ROR(w[i - 15], 18) ^ (w[i - 15] >> 3);
    uint32_t s1 = ROR(w[i - 2], 17) ^ ROR(w[i - 2], 19) ^ (w[i - 2] >> 10);
    w[i] = w[i - 16] + s0 + w[i - 7] + s1;
  }
  uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
  for (int i = 0; i < 64; i++) {
    uint32_t t1 = hh + (ROR(e, 6) ^ ROR(e, 11) ^ ROR(e, 25)) + ((e & f) ^ (~e & g)) + k_k[i] + w[i];
    uint32_t t2 = (ROR(a, 2) ^ ROR(a, 13) ^ ROR(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
    hh = g, g = f, f = e, e = d + t1, d = c, c = b, b = a, a = t1 + t2;
  }
  h[0] += a, h[1] += b, h[2] += c, h[3] += d, h[4] += e, h[5] += f, h[6] += g, h[7] += hh;
}

void fself_sha256(const void *data, size_t len, uint8_t out[32]) {
  uint32_t h[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                   0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
  const uint8_t *p = data;
  size_t n = len;
  while (n >= 64) {
    sha256_block(h, p);
    p += 64, n -= 64;
  }
  uint8_t tail[128];
  memset(tail, 0, sizeof tail);
  memcpy(tail, p, n);
  tail[n] = 0x80;
  size_t tl = n + 9 <= 64 ? 64 : 128;
  uint64_t bits = (uint64_t)len * 8;
  for (int i = 0; i < 8; i++)
    tail[tl - 1 - i] = (uint8_t)(bits >> (8 * i));
  sha256_block(h, tail);
  if (tl == 128)
    sha256_block(h, tail + 64);
  for (int i = 0; i < 8; i++)
    for (int j = 0; j < 4; j++)
      out[4 * i + j] = (uint8_t)(h[i] >> (24 - 8 * j));
}

/* ---- SELF ---- */
#define PT_LOAD 1
#define PT_SCE_DYNLIBDATA 0x61000000
#define PT_SCE_RELRO 0x61000010
#define PT_SCE_COMMENT 0x6fffff00
#define PT_SCE_VERSION 0x6fffff01
#define BLOCK 0x4000

static size_t up16(size_t x) { return (x + 15) & ~(size_t)15; }
static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static uint32_t rd32(const uint8_t *p) { return (uint32_t)rd16(p) | (uint32_t)rd16(p + 2) << 16; }
static uint64_t rd64(const uint8_t *p) { return (uint64_t)rd32(p) | (uint64_t)rd32(p + 4) << 32; }
static void wr16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v, p[1] = (uint8_t)(v >> 8); }
static void wr32(uint8_t *p, uint32_t v) { wr16(p, (uint16_t)v), wr16(p + 2, (uint16_t)(v >> 16)); }
static void wr64(uint8_t *p, uint64_t v) { wr32(p, (uint32_t)v), wr32(p + 4, (uint32_t)(v >> 32)); }

static int wanted(uint32_t type) {
  return type == PT_LOAD || type == PT_SCE_RELRO || type == PT_SCE_DYNLIBDATA ||
         type == PT_SCE_COMMENT;
}

size_t fself_make(const uint8_t *elf, size_t elf_len, uint64_t paid, uint64_t ptype, uint8_t *out,
                  size_t out_cap) {
  if (elf_len < 64 || memcmp(elf, "\x7f" "ELF", 4) != 0)
    return 0;
  uint64_t phoff = rd64(elf + 0x20);
  uint16_t ehsize = rd16(elf + 0x34), phentsize = rd16(elf + 0x36), phnum = rd16(elf + 0x38);
  if (phentsize != 56 || phoff + (uint64_t)phnum * 56 > elf_len)
    return 0;
  unsigned n = 0;
  int version = -1;
  for (unsigned i = 0; i < phnum; i++) {
    uint32_t t = rd32(elf + phoff + i * 56);
    if (t == PT_SCE_VERSION)
      version = (int)i;
    if (wanted(t))
      n += 2;
  }
  size_t ehdr_region = ehsize > phoff + (size_t)phentsize * phnum ? ehsize
                                                                   : phoff + (size_t)phentsize * phnum;
  size_t header_size = up16(8 + 24 + (size_t)n * 32 + ehdr_region) + 0x40 + 48;
  size_t meta_size = (size_t)n * 80 + 80 + 0x100;
  /* layout of the segment data */
  size_t off = header_size + meta_size, total;
  for (unsigned i = 0; i < phnum; i++) {
    const uint8_t *ph = elf + phoff + i * 56;
    if (!wanted(rd32(ph)))
      continue;
    uint64_t fs = rd64(ph + 0x20);
    off = up16(off + (fs + BLOCK - 1) / BLOCK * 32);
    off = up16(off + fs);
  }
  total = off;
  size_t vlen = 0;
  const uint8_t *vdata = 0;
  if (version >= 0) {
    const uint8_t *ph = elf + phoff + (size_t)version * 56;
    vdata = elf + rd64(ph + 8);
    vlen = rd64(ph + 0x20);
  }
  if (!out)
    return total + vlen;
  if (total + vlen > out_cap)
    return 0;
  memset(out, 0, total + vlen);
  /* common + extended header */
  memcpy(out, "\x4f\x15\x3d\x1d", 4);
  out[4] = 0, out[5] = 1, out[6] = 1, out[7] = 0x12;
  wr32(out + 8, 0x101);
  wr16(out + 12, (uint16_t)header_size);
  wr16(out + 14, (uint16_t)meta_size);
  wr64(out + 16, total);
  wr16(out + 24, (uint16_t)n);
  wr16(out + 26, 0x2 | (2 << 4));
  /* entries */
  uint8_t *e = out + 32;
  off = header_size + meta_size;
  unsigned idx = 0;
  for (unsigned i = 0; i < phnum; i++) {
    const uint8_t *ph = elf + phoff + i * 56;
    if (!wanted(rd32(ph)))
      continue;
    uint64_t fo = rd64(ph + 8), fs = rd64(ph + 0x20);
    uint64_t nb = (fs + BLOCK - 1) / BLOCK;
    /* digests (all zero) */
    wr64(e, (1u << 2) | (1u << 16) | ((uint64_t)((idx + 1) & 0xffff) << 20));
    wr64(e + 8, off);
    wr64(e + 16, nb * 32);
    wr64(e + 24, nb * 32);
    off = up16(off + nb * 32);
    e += 32;
    /* data, in 16 KB blocks */
    wr64(e, (1u << 2) | (1u << 11) | (2u << 12) | ((uint64_t)(i & 0xffff) << 20));
    wr64(e + 8, off);
    wr64(e + 16, fs);
    wr64(e + 24, fs);
    if (fo + fs > elf_len)
      return 0;
    memcpy(out + off, elf + fo, fs);
    off = up16(off + fs);
    e += 32;
    idx += 2;
  }
  /* ELF header and program headers */
  uint8_t *eh = out + 32 + (size_t)n * 32;
  memcpy(eh, elf, 64);
  memcpy(eh + phoff, elf + phoff, (size_t)phentsize * phnum);
  /* extended info: paid, ptype, app version, fw version, digest */
  uint8_t *x = out + up16(32 + (size_t)n * 32 + ehdr_region);
  wr64(x, paid);
  wr64(x + 8, ptype);
  fself_sha256(elf, elf_len, x + 32);
  /* NPDRM control block */
  wr16(x + 0x40, 3);
  /* meta blocks (zero), footer, signature (zero) */
  uint8_t *m = out + header_size;
  wr32(m + (size_t)n * 80 + 48, 0x10000);
  if (vlen)
    memcpy(out + total, vdata, vlen);
  return total + vlen;
}
