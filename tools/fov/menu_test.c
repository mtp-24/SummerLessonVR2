/* agent/menu.c on the host: the controller's presses through menu_pad, the picture as a PNG.
 *   cc -O2 -I../../agent -o /tmp/menutest menu_test.c -lz && /tmp/menutest <out.png>
 *   cc -O2 -DRELEASE -I../../agent -o /tmp/menutest-r menu_test.c -lz && /tmp/menutest-r <out.png>
 * Checks: L1+R1+touchpad opens and closes it, the game sees nothing while it is open (and not
 * the touchpad held with both shoulders), up/down/left/right/cross change the lines' values.
 * Without RELEASE the debug menu; with it the settings menu: its lines in order, each line's
 * values, and the settings file's round trip (menu_boot, menu_save, Reset all) with the payload
 * played by a stub.
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
int agent_snprintf(char *out, size_t n, const char *fmt, ...) { va_list ap; va_start(ap, fmt); int r = vsnprintf(out, n, fmt, ap); va_end(ap); return r < 0 ? 0 : r >= (int)n ? (int)n - 1 : r; }
volatile u32 g_opt_insetocc = 1, g_opt_insetlod = 1, g_opt_insetviews = 2, g_opt_insetstate = 1, g_opt_insetaam = 1, g_opt_gazefilter = 1, g_opt_gazemincut = 10, g_opt_gazebeta = 1, g_opt_insetppt = 1200, g_opt_inseteye = 1,
             g_opt_insetpp, g_opt_insettaa, g_opt_taa, g_opt_insetsnap = 2, g_opt_insetdebug, g_opt_gazedot, g_opt_insetpos = 1,
             g_opt_probe, g_opt_insetgaze, g_opt_gazehold = 50, g_opt_insetwidth;
volatile u32 g_opt_nextfps = 60;
u32 g_opt_tvhmd, g_opt_mirrorboth, g_opt_tvblank, g_opt_hands = 60, g_multiview_rows = 600, g_cvars_multiview = 1;
struct agent_header astro_hdr; /* (its build: 3 Allison, whose AA quality stops at 3) */
static int g_resets;
void vr_reset_view(const char *why) { printf("  reset view (%s)\n", why); g_resets++; }
/* the game's variables: inset mode's values at the start (shadows 5, AO 3; the game's bloom 1, reflections 0) */
static int g_shadow = 5, g_aaq = 6, g_ssr = 0, g_ao = 3, g_bloom = 1;
int hooks_cvar_int(const char *name, int *get, const int *set) {
  int *p = !strcmp(name, "r.ShadowQuality") ? &g_shadow : !strcmp(name, "r.PostProcessAAQuality") ? &g_aaq
         : !strcmp(name, "r.SSR.Quality") ? &g_ssr : !strcmp(name, "r.AmbientOcclusionLevels") ? &g_ao
         : !strcmp(name, "r.BloomQuality") ? &g_bloom : 0;
  if (!p) return 0;
  if (set) *p = *set;
  if (get) *get = *p;
  return 1;
}
static float g_lod = 1.0f;
int hooks_cvar_float(const char *name, float *get, const float *set) {
  if (strcmp(name, "r.StaticMeshLODDistanceScale")) return 0;
  if (set) g_lod = *set;
  if (get) *get = g_lod;
  return 1;
}
/* the payload: the settings file (CMD_MENU) kept in memory */
static char g_payload_file[512];
static int g_payload_has, g_payload_writes;
int agent_call(u32 cmd, u64 a0, u64 a1, u64 a2, u64 a3, u32 wait_ms, u32 k, u64 *out) {
  (void)a3, (void)wait_ms, (void)k, (void)out;
  if (cmd != CMD_MENU) return -1;
  if (a2 == 0) { if (g_payload_has) strncpy((char *)a0, g_payload_file, a1); return 0; }
  g_payload_writes++;
  g_payload_has = a1 != 0;
  memset(g_payload_file, 0, sizeof g_payload_file);
  if (a1) memcpy(g_payload_file, (const char *)a0, a1 < sizeof g_payload_file ? a1 : sizeof g_payload_file - 1);
  return 0;
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
static int bad;
#define CHECK(cond, ...) do { if (!(cond)) printf("FAIL: " __VA_ARGS__), printf("\n"), bad = 1; } while (0)
/* the common checks: the lines fit the picture, the opening, the game sees nothing while open */
static void common_start(void) {
  /* every line's label and its longest value within the picture's width with a gap: the label from
   * x 14, the value right-aligned to MENU_W - 6, 8 pixels a character (the user, worn: labels over
   * their values) */
  for (u32 i = 0; i < N_ITEMS; i++) {
    const struct item *it = &k_items[i];
    u32 v = 1;
    if (it->names)
      for (int k = it->lo; k <= it->hi; k++)
        v = strlen(it->names[k - it->lo]) > v ? (u32)strlen(it->names[k - it->lo]) : v;
    else {
      char b[16];
      u32 a = (u32)snprintf(b, sizeof b, "%d", it->lo), c = (u32)snprintf(b, sizeof b, "%d", it->hi);
      v = a > c ? a : c;
    }
    CHECK(14 + 8 * strlen(it->label) + 8 <= MENU_W - 6 - 8 * v, "the line '%s' with its %u-character value is wider than the menu", it->label, v);
  }
  CHECK(!menu_picture(), "a picture while closed");
  /* the touchpad alone, or with one shoulder: not the menu, and the game sees it */
  memset(pad, 0, sizeof pad); *(u32 *)pad = 0x100000 | 0x400; menu_pad(pad);
  CHECK(!g_m.open && *(u32 *)pad == (0x100000 | 0x400), "L1+touchpad opened it or was hidden");
  press(0);
  /* both shoulders then the touchpad: open; the touchpad held with them hidden from the game */
  memset(pad, 0, sizeof pad); *(u32 *)pad = 0x0c00; menu_pad(pad);
  memset(pad, 0, sizeof pad); *(u32 *)pad = 0x0c00 | 0x100000; pad[4] = 10; menu_pad(pad);
  CHECK(g_m.open, "L1+R1+touchpad didn't open it");
  CHECK(*(u32 *)pad == 0 && pad[4] == 0x80, "the game saw the opening press or a stick");
  press(0);
}
static void common_end(const char *png_path, const char *picture_line) {
  /* the game sees nothing while open */
  memset(pad, 0, sizeof pad); *(u32 *)pad = 0x4000 | 0x80000000u; pad[6] = 3; pad[9] = 200; menu_pad(pad);
  CHECK(*(u32 *)pad == 0x80000000u && pad[6] == 0x80 && pad[9] == 0, "the game saw input while open");
  press(0);
  go(picture_line); /* for the picture */
  const u8 *p = menu_picture();
  CHECK(p, "no picture while open");
  if (p && png_path) png(png_path, p, MENU_W, MENU_H);
  press(0x2000); /* circle closes */
  CHECK(!g_m.open && !menu_picture(), "circle didn't close it");
}

#ifdef RELEASE
/* the value shown for a line, as draw() makes it */
static const char *shown(const char *label) {
  static char val[24];
  u32 i = 0;
  while (strcmp(k_items[i].label, label)) i++;
  const struct item *it = &k_items[i];
  int v, r = value_of(it, &v);
  if (!r) return "-";
  if (r == 2) snprintf(val, sizeof val, "%d", v);
  else if (it->names) return it->names[v - it->lo];
  else snprintf(val, sizeof val, "%d", v);
  return val;
}
static void fresh_start(void) { /* the agent as it starts: the menu's state, the options and variables at their defaults */
  memset(&g_m, 0, sizeof g_m);
  g_opt_hands = 60, g_opt_mirrorboth = g_opt_tvblank = 0, g_opt_nextfps = 60;
  g_shadow = 5, g_ssr = 0, g_ao = 3, g_bloom = 1;
  g_next_width = g_next_dfr = g_next_hands = g_tv = 0;
}
int main(int argc, char **argv) {
  static const char *const k_order[] = {"Frame rate*", "Eye width*", "Foveated rendering*", "Hand tracking*", "Shadows",
                                        "Reflections", "Ambient occlusion", "Bloom", "TV picture", "Reset view", "Reset all", "Close"};
  CHECK(N_ITEMS == 12, "%u lines, not 12", (u32)N_ITEMS);
  for (u32 i = 0; i < N_ITEMS && i < 12; i++)
    CHECK(!strcmp(k_items[i].label, k_order[i]), "line %u is '%s', not '%s'", i, k_items[i].label, k_order[i]);
  /* the start without a file: the defaults */
  fresh_start();
  menu_boot();
  menu_cvars_apply();
  CHECK(!g_payload_writes && g_next_dfr == 1 && g_next_hands == 60 && g_tv == 0 && g_next_width == 0, "the start without a file");
  common_start();
  CHECK(!strcmp(shown("Frame rate*"), "60") && !strcmp(shown("Eye width*"), "game's preset") && !strcmp(shown("Foveated rendering*"), "on") &&
        !strcmp(shown("Hand tracking*"), "on") && !strcmp(shown("Shadows"), "high") && !strcmp(shown("Reflections"), "off") &&
        !strcmp(shown("Ambient occlusion"), "on") && !strcmp(shown("Bloom"), "on") && !strcmp(shown("TV picture"), "left eye"),
        "the defaults shown: %s %s %s %s %s %s %s %s %s", shown("Frame rate*"), shown("Eye width*"), shown("Foveated rendering*"),
        shown("Hand tracking*"), shown("Shadows"), shown("Reflections"), shown("Ambient occlusion"), shown("Bloom"), shown("TV picture"));
  /* the frame rate for the next start: 60, 90, 120, and no further (framerate.txt's own path) */
  go("Frame rate*");
  press(0x20);
  CHECK(g_opt_nextfps == 90, "frame rate %u after one step, not 90", g_opt_nextfps);
  press(0x20), press(0x20);
  CHECK(g_opt_nextfps == 120, "frame rate %u, not held at 120", g_opt_nextfps);
  press(0x80), press(0x80);
  CHECK(g_opt_nextfps == 60, "frame rate %u, not held at 60", g_opt_nextfps);
  press(0x20);
  /* the eye width: with DFR on, up to 1600 (held there) */
  go("Eye width*");
  for (int i = 0; i < 20; i++) press(0x20);
  CHECK(g_next_width == 1600, "eye width %u with DFR on, not held at 1600", g_next_width);
  CHECK(!strcmp(shown("Eye width*"), "1600"), "eye width shown as '%s'", shown("Eye width*"));
  /* DFR off: the width then goes to 2000, no further */
  go("Foveated rendering*");
  press(0x80);
  CHECK(g_next_dfr == 0 && g_multiview_rows == 600, "DFR off for the next start: %u (the rows now %u)", g_next_dfr, g_multiview_rows);
  go("Eye width*");
  for (int i = 0; i < 20; i++) press(0x20);
  CHECK(g_next_width == 2000, "eye width %u with DFR off, not held at 2000", g_next_width);
  for (int i = 0; i < 10; i++) press(0x80);
  CHECK(g_next_width == 1200, "eye width %u after ten steps down from 2000, not 1200", g_next_width);
  /* hands off: for the next start only (the tracker is never started or stopped mid-game) */
  go("Hand tracking*");
  press(0x80);
  CHECK(g_next_hands == 0 && g_opt_hands == 60, "hands off next start: next %u, now %u (must stay 60)", g_next_hands, g_opt_hands);
  /* shadows high -> medium -> low: 5, 3, 2, live; reflections on 2; AO off 0; bloom off 0 */
  go("Shadows");
  press(0x80);
  CHECK(g_shadow == 3, "shadows %d after one step down from high, not 3 (medium)", g_shadow);
  press(0x80), press(0x80);
  CHECK(g_shadow == 2 && !strcmp(shown("Shadows"), "low"), "shadows %d (%s), not 2 (low)", g_shadow, shown("Shadows"));
  go("Reflections");
  press(0x4000);
  CHECK(g_ssr == 2, "reflections %d, not 2 (on)", g_ssr);
  go("Ambient occlusion");
  press(0x80);
  CHECK(g_ao == 0, "AO %d, not 0 (off)", g_ao);
  go("Bloom");
  press(0x80);
  CHECK(g_bloom == 0, "bloom %d, not 0 (off)", g_bloom);
  /* the TV: both eyes, then black (live) */
  go("TV picture");
  press(0x20);
  CHECK(g_opt_mirrorboth == 1 && g_opt_tvblank == 0, "TV both eyes: mirrorboth %u, tvblank %u", g_opt_mirrorboth, g_opt_tvblank);
  press(0x20);
  CHECK(g_opt_mirrorboth == 0 && g_opt_tvblank == 1 && !strcmp(shown("TV picture"), "black"), "TV black: mirrorboth %u, tvblank %u", g_opt_mirrorboth, g_opt_tvblank);
  press(0x20);
  CHECK(g_opt_tvblank == 1, "TV went past black");
  /* reset view: the view reset, the menu stays open */
  go("Reset view");
  press(0x4000);
  CHECK(g_resets == 1 && g_m.open, "reset view: %d resets, open %u", g_resets, g_m.open);
  /* the file: a line for each setting changed from its default (the frame rate not in it) */
  menu_save();
  const char *want = "agent.resolution=1200\nagent.multiview=0\nagent.hands=0\nr.ShadowQuality=2\nr.SSR.Quality=2\n"
                     "r.AmbientOcclusionLevels=0\nr.BloomQuality=0\nagent.mirrorboth=0\nagent.tvblank=1\n";
  CHECK(g_payload_writes == 1 && g_payload_has && !strcmp(g_payload_file, want), "the file saved (%d writes):\n%s", g_payload_writes, g_payload_file);
  menu_save();
  CHECK(g_payload_writes == 1, "saved again with nothing changed");
  printf("  saved:\n%s", g_payload_file);
  common_end(argc > 1 ? argv[1] : 0, "Shadows");
  /* the next start: the file read back (the payload took agent.multiview=0: no rows), its lines
   * over the defaults, the game's variables once they can be set */
  fresh_start();
  g_multiview_rows = 0;
  menu_boot();
  CHECK(g_opt_hands == 0 && g_opt_tvblank == 1 && g_opt_mirrorboth == 0 && g_next_width == 1200 && g_next_dfr == 0 && g_next_hands == 0,
        "the file read back: hands %u, tvblank %u, mirrorboth %u, width %u, DFR %u, next hands %u", g_opt_hands, g_opt_tvblank,
        g_opt_mirrorboth, g_next_width, g_next_dfr, g_next_hands);
  CHECK(g_shadow == 5, "the game's variables set before the console manager is up");
  menu_cvars_apply();
  CHECK(g_shadow == 2 && g_ssr == 2 && g_ao == 0 && g_bloom == 0, "the file's variables: shadows %d, SSR %d, AO %d, bloom %d", g_shadow, g_ssr, g_ao, g_bloom);
  /* cvars.txt changed while the game runs: its lines (shadows 4) applied, then the menu's again */
  g_shadow = 4;
  menu_cvars_apply();
  CHECK(g_shadow == 2, "the menu's shadows after a cvars.txt change: %d, not 2", g_shadow);
  menu_save();
  CHECK(g_payload_writes == 1, "a start with a file wrote it again");
  /* shadows back to high (the default): its line goes; DFR back on: its line goes */
  memset(pad, 0, sizeof pad); *(u32 *)pad = 0x0c00; menu_pad(pad);
  memset(pad, 0, sizeof pad); *(u32 *)pad = 0x0c00 | 0x100000; menu_pad(pad);
  press(0);
  CHECK(g_m.open && !strcmp(shown("Foveated rendering*"), "off") && !strcmp(shown("Shadows"), "low"), "the menu after the restart: DFR %s, shadows %s",
        shown("Foveated rendering*"), shown("Shadows"));
  go("Shadows");
  press(0x20), press(0x20);
  go("Foveated rendering*");
  press(0x20);
  menu_save();
  want = "agent.resolution=1200\nagent.hands=0\nr.SSR.Quality=2\nr.AmbientOcclusionLevels=0\nr.BloomQuality=0\nagent.mirrorboth=0\nagent.tvblank=1\n";
  CHECK(g_payload_writes == 2 && !strcmp(g_payload_file, want), "the file after shadows and DFR back to their defaults:\n%s", g_payload_file);
  /* shadows back at their default: the menu holds them no more (a cvars.txt change while the game
   * runs applies, as with no menu line); reflections, still changed, held */
  g_shadow = 4, g_ssr = 0;
  menu_cvars_apply();
  CHECK(g_shadow == 4 && g_ssr == 2, "a cvars.txt change after shadows went back to their default: shadows %d (not 4), SSR %d (not 2)", g_shadow, g_ssr);
  g_shadow = 5;
  /* Reset all: everything at its default, the file removed */
  go("Reset all");
  press(0x4000);
  menu_save();
  CHECK(g_payload_writes == 3 && !g_payload_has, "Reset all didn't remove the file (%d writes, has %d)", g_payload_writes, g_payload_has);
  CHECK(g_shadow == 5 && g_ssr == 0 && g_ao == 3 && g_bloom == 1 && g_opt_tvblank == 0 && g_opt_mirrorboth == 0 && g_next_width == 0 &&
        g_next_dfr == 1 && g_next_hands == 60 && g_opt_hands == 0 && g_opt_nextfps == 60,
        "Reset all: shadows %d, SSR %d, AO %d, bloom %d, tvblank %u, mirrorboth %u, width %u, DFR %u, hands %u (now %u), fps %u",
        g_shadow, g_ssr, g_ao, g_bloom, g_opt_tvblank, g_opt_mirrorboth, g_next_width, g_next_dfr, g_next_hands, g_opt_hands, g_opt_nextfps);
  printf("  Reset all: the defaults again, the file removed\n");
  press(0x2000);
  /* cvars.txt's own settings are the defaults: agent.multiview=0 and agent.hands=30 there, the
   * menu turning DFR on writes agent.multiview=1 (the rate's own rows); hands shows 30 and steps
   * onto its list; a file line for a variable the menu hasn't is logged and left */
  fresh_start();
  g_multiview_rows = 0, g_cvars_multiview = 0, g_opt_hands = 30;
  strcpy(g_payload_file, "r.MotionBlurQuality=0\n"), g_payload_has = 1;
  menu_boot();
  menu_cvars_apply();
  memset(pad, 0, sizeof pad); *(u32 *)pad = 0x0c00; menu_pad(pad);
  memset(pad, 0, sizeof pad); *(u32 *)pad = 0x0c00 | 0x100000; menu_pad(pad);
  press(0);
  CHECK(!strcmp(shown("Foveated rendering*"), "off") && !strcmp(shown("Hand tracking*"), "30"), "cvars.txt's defaults shown: DFR %s, hands %s",
        shown("Foveated rendering*"), shown("Hand tracking*"));
  go("Foveated rendering*");
  press(0x20);
  go("Hand tracking*");
  press(0x20);
  CHECK(g_next_hands == 60 && !strcmp(shown("Hand tracking*"), "on"), "hands from 30 went right to %u", g_next_hands);
  menu_save();
  CHECK(!strcmp(g_payload_file, "agent.multiview=1\nagent.hands=60\n"), "the file over cvars.txt's own:\n%s", g_payload_file);
  g_cvars_multiview = 1;
  press(0x2000);
  /* a variable's own negative value (r.AmbientOcclusionLevels -1, the engine's "by quality level"):
   * shown as its number, not as a list index; right steps onto the list's first above it */
  fresh_start();
  g_ao = -1;
  menu_boot();
  menu_cvars_apply();
  memset(pad, 0, sizeof pad); *(u32 *)pad = 0x0c00; menu_pad(pad);
  memset(pad, 0, sizeof pad); *(u32 *)pad = 0x0c00 | 0x100000; menu_pad(pad);
  press(0);
  CHECK(!strcmp(shown("Ambient occlusion"), "-1"), "AO -1 shown as '%s'", shown("Ambient occlusion"));
  go("Ambient occlusion");
  press(0x20);
  CHECK(g_ao == 0 && !strcmp(shown("Ambient occlusion"), "off"), "AO from -1 went right to %d (%s), not 0 (off)", g_ao, shown("Ambient occlusion"));
  press(0x20);
  CHECK(g_ao == 3, "AO %d, not 3 (on)", g_ao);
  /* the eye width set wide with DFR off, then DFR on: the width fits DFR (1600), the file says so */
  go("Foveated rendering*");
  press(0x80);
  go("Eye width*");
  for (int i = 0; i < 20; i++) press(0x20);
  CHECK(g_next_width == 2000, "eye width %u with DFR off, not 2000", g_next_width);
  go("Foveated rendering*");
  press(0x20);
  CHECK(g_next_dfr == 1 && g_next_width == 1600, "DFR back on: width %u, not 1600", g_next_width);
  menu_save();
  CHECK(!strcmp(g_payload_file, "agent.resolution=1600\nr.AmbientOcclusionLevels=3\n"), "the file after the width fitted DFR:\n%s", g_payload_file);
  press(0x2000);
  /* a file saying both (hand-edited): the width fitted at the boot, the file put right at the next save */
  fresh_start();
  g_multiview_rows = 600; /* (DFR on this start) */
  strcpy(g_payload_file, "agent.resolution=2000\n"), g_payload_has = 1, g_payload_writes = 0;
  menu_boot();
  CHECK(g_next_width == 1600 && g_next_dfr == 1, "a file's 2000 with DFR on: width %u, DFR %u", g_next_width, g_next_dfr);
  menu_save();
  CHECK(g_payload_writes == 1 && !strcmp(g_payload_file, "agent.resolution=1600\n"), "the file put right (%d writes):\n%s", g_payload_writes, g_payload_file);
  printf(bad ? "FAIL\n" : "ok\n");
  return bad;
}
#else
int main(int argc, char **argv) {
  common_start();
  /* Shadows: left 5 -> 4; Gaze dot: cross off -> on */
  go("Shadows");
  press(0x80);
  CHECK(g_shadow == 4, "shadows %d, not 4", g_shadow);
  go("Gaze dot");
  press(0x4000);
  CHECK(g_opt_gazedot == 1, "gaze dot %u, not 1 (on)", g_opt_gazedot);
  /* LOD scale, a float in hundredths: 1.00 -> 0.90 */
  {
    u32 lod = 0;
    while (strcmp(k_items[lod].label, "LOD scale (x100)")) lod++;
    while (g_m.sel != lod) press(0x40);
    press(0x80);
    CHECK(g_lod >= 0.899f && g_lod <= 0.901f, "LOD scale %f, not 0.90", g_lod);
  }
  /* Reset all (the line before Close): all back to their values when the menu was opened */
  {
    u32 to_reset = (u32)(N_ITEMS - 2);
    while (g_m.sel != to_reset) press(0x10);
    press(0x4000);
    CHECK(g_shadow == 5 && g_opt_gazedot == 0 && g_lod >= 0.999f, "reset gave shadows %d, gaze dot %u, LOD %f (want 5, 0, 1)", g_shadow, g_opt_gazedot, g_lod);
    if (!bad)
      printf("  reset: shadows 5, gaze dot off again\n");
    while (g_m.sel != 1) press(0x10);
  }
  /* the frame rate for the next start: 60, 90, 120, and no further */
  go("Frame rate, next start");
  press(0x20);
  CHECK(g_opt_nextfps == 90, "frame rate %u after one step, not 90", g_opt_nextfps);
  press(0x20), press(0x20);
  CHECK(g_opt_nextfps == 120, "frame rate %u, not held at 120", g_opt_nextfps);
  press(0x80), press(0x80), press(0x80);
  CHECK(g_opt_nextfps == 60, "frame rate %u, not held at 60", g_opt_nextfps);
  /* AA quality: up to 6, but 3 in the newer engine (Chisato, Allison, Hikari 1.02: builds 2-4; 4 and
   * above shake the picture) */
  {
    int ok = 1;
    for (u32 b = 1; b <= BUILD_N; b++) {
      astro_hdr.build = b, g_aaq = 2;
      go("AA quality");
      for (int i = 0; i < 6; i++) press(0x20);
      ok &= g_aaq == (b >= 2 ? 3 : 6);
    }
    astro_hdr.build = 0, g_aaq = 6;
    CHECK(ok, "AA quality not 6 in Hikari 1.00 and 3 in Chisato, Allison and Hikari 1.02 at its highest");
    if (ok)
      printf("  AA quality: up to 6 in Hikari 1.00, in Chisato, Allison and Hikari 1.02 up to 3\n");
  }
  /* the TAA shader: only the choices (0, 1, 11, 13), held at the last; one set from cvars.txt (7, a
   * test) shown as its number and stepped onto the list */
  go("TAA shader");
  g_opt_taa = 0;
  press(0x20);
  CHECK(g_opt_taa == 1, "TAA shader %u after one step, not 1", g_opt_taa);
  press(0x20), press(0x20);
  CHECK(g_opt_taa == 13, "TAA shader %u after three steps, not 13", g_opt_taa);
  press(0x20);
  CHECK(g_opt_taa == 13, "TAA shader went past 13 (%u)", g_opt_taa);
  press(0x80);
  CHECK(g_opt_taa == 11, "TAA shader %u one step back, not 11", g_opt_taa);
  g_opt_taa = 7;
  press(0x20);
  CHECK(g_opt_taa == 11, "TAA shader from 7 (off the list) went right to %u, not 11", g_opt_taa);
  g_opt_taa = 7;
  press(0x80);
  CHECK(g_opt_taa == 1, "TAA shader from 7 (off the list) went left to %u, not 1", g_opt_taa);
  g_opt_taa = 15;
  press(0x20);
  CHECK(g_opt_taa == 15, "TAA shader from 15 (past the list) went right to %u, not left as it was", g_opt_taa);
  press(0x80);
  CHECK(g_opt_taa == 13, "TAA shader from 15 went left to %u, not 13", g_opt_taa);
  g_opt_taa = 0;
  {
    u32 grid = 0;
    while (strcmp(k_items[grid].label, "Inset grid")) grid++;
    CHECK(k_items[grid].hi == 5 && !strcmp(k_items[grid].names[5], "tints"), "the inset grid's top value %d (%s), not 5 (tints)", k_items[grid].hi, k_items[grid].names[k_items[grid].hi]);
  }
  /* Reset view is the settings menu's; the debug menu's cross on a value line steps it */
  CHECK(g_resets == 0, "a view reset from the debug menu");
  common_end(argc > 1 ? argv[1] : 0, "Shadows");
  printf(bad ? "FAIL\n" : "ok\n");
  return bad;
}
#endif
