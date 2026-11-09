/* xref EBOOT TEXT_OFFSET TEXT_SIZE TARGET_VA...: the places in a PS4 eboot's code whose 32-bit
 * displacement reaches a target, rip-relative (an instruction ending 0 to 4 bytes after it).
 * Prints "target disp_va k" per candidate (VA = file offset - TEXT_OFFSET); match.py checks
 * each by decoding it. Build: cc -O2 -o tools/ebootmap/xref tools/ebootmap/xref.c
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv) {
  if (argc < 5) {
    fprintf(stderr, "usage: xref EBOOT TEXT_OFFSET TEXT_SIZE TARGET_VA...\n");
    return 2;
  }
  FILE *f = fopen(argv[1], "rb");
  if (!f)
    return perror(argv[1]), 1;
  fseek(f, 0, SEEK_END);
  long n = ftell(f);
  fseek(f, 0, SEEK_SET);
  uint8_t *b = malloc(n);
  if (!b || fread(b, 1, n, f) != (size_t)n)
    return fprintf(stderr, "%s: can't read\n", argv[1]), 1;
  fclose(f);
  long off = strtol(argv[2], 0, 0), size = strtol(argv[3], 0, 0);
  if (off < 0 || size < 0 || off + size > n)
    return fprintf(stderr, "text outside the file\n"), 1;
  for (int t = 4; t < argc; t++) {
    long tg = strtol(argv[t], 0, 0);
    for (long p = 0; p + 4 <= size; p++) {
      int32_t d;
      memcpy(&d, b + off + p, 4);
      for (int k = 0; k <= 4; k++)
        if (p + 4 + k + (long)d == tg)
          printf("%lx %lx %d\n", tg, p, k);
    }
  }
  return 0;
}
