/* The payload's translator (../payload/rdna2gcn.c) built into the freestanding agent: core.c has
 * these.
 * SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include <stddef.h>

void *memcpy(void *d, const void *s, size_t n);
void *memset(void *d, int c, size_t n);
