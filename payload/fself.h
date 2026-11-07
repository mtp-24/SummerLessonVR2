/* fself: fake-signed SELF writer (format of ps5-payload-sdk make_fself.py).
 * SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include <stddef.h>
#include <stdint.h>

#define FSELF_PTYPE_FAKE 1

/* Size needed when out is 0; bytes written otherwise (0 = bad ELF or out too small). */
size_t fself_make(const uint8_t *elf, size_t elf_len, uint64_t paid, uint64_t ptype, uint8_t *out,
                  size_t out_cap);
void fself_sha256(const void *data, size_t len, uint8_t out[32]);
