/* agent/menu.c on the host: the controller's presses through menu_pad, the picture as a PNG.
 *   cc -O2 -I../../agent -o /tmp/menutest menu_test.c && /tmp/menutest <out.png>
 * Checks: L1+R1+touchpad opens and closes it, the game sees nothing while it is open (and not
 * the touchpad held with both shoulders), up/down/left/right/cross change the lines' values.
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>
#undef memcpy
#undef memset
#define memcpy agent_memcpy_unused
#define memset agent_memset_unused
#define memcmp agent_memcmp_unused
#define strlen agent_strlen_unused
#include "vr2.h"
#undef memcpy
#undef memset
#undef memcmp
#undef strlen
void agent_log(const char *fmt, ...) { va_list ap; va_start(ap, fmt); printf("  log: "); vprintf(fmt, ap); printf("\n"); va_end(ap); }
volatile u32 g_opt_insetviews = 2, g_opt_insetstate = 1, g_opt_insetaam = 1, g_opt_gazefilter = 1, g_opt_gazemincut = 10, g_opt_gazebeta = 1, g_opt_insetppt = 1200, g_opt_inseteye = 1,
             g_opt_insetpp, g_opt_insetdebug, g_opt_gazedot, g_opt_insetpos = 1,
             g_opt_probe, g_opt_insetgaze, g_opt_gazehold = 50, g_opt_vignette, g_opt_insetwidth;
u32 g_opt_predict, g_opt_newest = 1, g_opt_tvhmd, g_opt_mirrorboth, g_opt_tvblank;
static int g_shadow = 3;
int hooks_cvar_int(const char *name, int *get, const int *set) {
  if (strcmp(name, "r.ShadowQuality")) return 0;
  if (set) g_shadow = *set;
  if (get) *get = g_shadow;
  return 1;
}
static float g_lod = 1.0f;
int hooks_cvar_float(const char *name, float *get, const float *set) {
  if (strcmp(name, "r.StaticMeshLODDistanceScale")) return 0;
  if (set) g_lod = *set;
  if (get) *get = g_lod;
  return 1;
}
void *vr2_dalloc(size_t size, size_t align) { (void)align; return calloc(1, size); }
static u64 fake_time(void) { static u64 t; return t += 1000; }
u64 (*sceKernelGetProcessTime)(void) = fake_time;
#include "../../agent/menu.c"
static u8 pad[0x78];
static void press(u32 b) {
  memset(pad, 0, sizeof pad); *(u32 *)pad = b; pad[4] = 10; pad[8] = 99; menu_pad(pad);
  memset(pad, 0, sizeof pad); *(u32 *)pad = 0; menu_pad(pad);
}
static void go(const char *label) { /* down to the line with this label */
  for (u32 i = 0; i < 2 * N_ITEMS && strcmp(k_items[g_m.sel].label, label); i++) press(0x40);
}
static void png(const char *path, const u8 *rgba, int w, int h) {
  FILE *f = fopen(path, "wb"); if (!f) return;
  uLongf n = compressBound((uLong)(h * (w * 3 + 1)));
  u8 *raw = malloc((size_t)h * (w * 3 + 1)), *z = malloc(n);
  for (int y = 0; y < h; y++) { raw[y * (w * 3 + 1)] = 0;
    for (int x = 0; x < w; x++) { const u8 *p = rgba + (y * w + x) * 4; float a = p[3] / 255.0f;
      for (int c = 0; c < 3; c++) raw[y * (w * 3 + 1) + 1 + x * 3 + c] = (u8)(p[c] * a + 90 * (1 - a)); } }
  compress(z, &n, raw, (uLong)(h * (w * 3 + 1)));
  const u8 sig[8] = {0x89, 'P', 'N', 'G', 13, 10, 26, 10}; fwrite(sig, 1, 8, f);
  #define BE(v) (u8)((v) >> 24), (u8)((v) >> 16), (u8)((v) >> 8), (u8)(v)
  u8 ihdr[13] = {BE((u32)w), BE((u32)h), 8, 2, 0, 0, 0};
  const char *t[3] = {"IHDR", "IDAT", "IEND"}; const u8 *d[3] = {ihdr, z, 0}; u32 l[3] = {13, (u32)n, 0};
  for (int i = 0; i < 3; i++) { u8 L[4] = {BE(l[i])}; fwrite(L, 1, 4, f); fwrite(t[i], 1, 4, f); if (l[i]) fwrite(d[i], 1, l[i], f);
    uLong c = crc32(0, (const u8 *)t[i], 4); if (l[i]) c = crc32(c, d[i], l[i]); u8 C[4] = {BE((u32)c)}; fwrite(C, 1, 4, f); }
  fclose(f);
}
int main(int argc, char **argv) {
  int bad = 0;
  if (menu_picture()) printf("FAIL: a picture while closed\n"), bad = 1;
  /* the touchpad alone, or with one shoulder: not the menu, and the game sees it */
  memset(pad, 0, sizeof pad); *(u32 *)pad = 0x100000 | 0x400; menu_pad(pad);
  if (g_m.open || *(u32 *)pad != (0x100000 | 0x400)) printf("FAIL: L1+touchpad opened it or was hidden\n"), bad = 1;
  press(0);
  /* both shoulders then the touchpad: open; the touchpad held with them hidden from the game */
  memset(pad, 0, sizeof pad); *(u32 *)pad = 0x0c00; menu_pad(pad);
  memset(pad, 0, sizeof pad); *(u32 *)pad = 0x0c00 | 0x100000; pad[4] = 10; menu_pad(pad);
  if (!g_m.open) printf("FAIL: L1+R1+touchpad didn't open it\n"), bad = 1;
  if (*(u32 *)pad != 0 || pad[4] != 0x80) printf("FAIL: the game saw the opening press or a stick\n"), bad = 1;
  press(0);
  /* Shadows: left 3 -> 2; Inset AA: cross FXAA -> TAA */
  go("Shadows");
  press(0x80);
  if (g_shadow != 2) printf("FAIL: shadows %d, not 2\n", g_shadow), bad = 1;
  go("Inset AA");
  press(0x4000);
  if (g_opt_insetaam != 2) printf("FAIL: inset AA %u, not 2 (TAA)\n", g_opt_insetaam), bad = 1;
  /* LOD scale, a float in hundredths: 1.00 -> 0.90 */
  {
    u32 lod = 0;
    while (strcmp(k_items[lod].label, "LOD scale (x100)")) lod++;
    while (g_m.sel != lod) press(0x40);
    press(0x80);
    if (g_lod < 0.899f || g_lod > 0.901f)
      printf("FAIL: LOD scale %f, not 0.90\n", g_lod), bad = 1;
  }
  /* Reset all (the line before Close): both back to their values when the menu was opened */
  {
    u32 to_reset = (u32)(N_ITEMS - 2);
    while (g_m.sel != to_reset) press(0x10);
    press(0x4000);
    if (g_shadow != 3 || g_opt_insetaam != 1 || g_lod < 0.999f)
      printf("FAIL: reset gave shadows %d, inset AA %u, LOD %f (want 3, 1, 1)\n", g_shadow, g_opt_insetaam, g_lod), bad = 1;
    else
      printf("  reset: shadows 3, inset AA FXAA again\n");
    while (g_m.sel != 1) press(0x10);
  }
  /* the brightness probe's three values, the grid's last one the tints (shader debug mode 5) */
  go("Brightness probe");
  press(0x20), press(0x20);
  if (g_opt_probe != 2) printf("FAIL: brightness probe %u, not 2 (edges)\n", g_opt_probe), bad = 1;
  press(0x20);
  if (g_opt_probe != 2) printf("FAIL: brightness probe went past 2 (%u)\n", g_opt_probe), bad = 1;
  {
    u32 grid = 0;
    while (strcmp(k_items[grid].label, "Inset grid")) grid++;
    if (k_items[grid].hi != 5 || strcmp(k_items[grid].names[5], "tints"))
      printf("FAIL: the inset grid's top value %d (%s), not 5 (tints)\n", k_items[grid].hi, k_items[grid].names[k_items[grid].hi]), bad = 1;
  }
  g_opt_probe = 0;
  /* the game sees nothing while open */
  memset(pad, 0, sizeof pad); *(u32 *)pad = 0x4000 | 0x80000000u; pad[6] = 3; pad[9] = 200; menu_pad(pad);
  if (*(u32 *)pad != 0x80000000u || pad[6] != 0x80 || pad[9] != 0) printf("FAIL: the game saw input while open\n"), bad = 1;
  press(0);
  go("Shadows"); /* for the picture */
  const u8 *p = menu_picture();
  if (!p) printf("FAIL: no picture while open\n"), bad = 1;
  else if (argc > 1) png(argv[1], p, MENU_W, MENU_H);
  press(0x2000); /* circle closes */
  if (g_m.open || menu_picture()) printf("FAIL: circle didn't close it\n"), bad = 1;
  printf(bad ? "FAIL\n" : "ok\n");
  return bad;
}
