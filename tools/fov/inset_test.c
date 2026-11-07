/* tools/gpu/inset.c (the insets' alpha, multi-view step 4) run on the host over a texture of the
 * step 3 run's size, its colour a pattern:
 *   cc -O2 -o /tmp/insettest inset_test.c && /tmp/insettest
 * Checks: the colour written back as read; alpha 0 outside the insets and on
 * their outermost texels, 1 from `ramp` texels in, rising in between.
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef float float4 __attribute__((ext_vector_type(4)));
#include "../gpu/inset.c"

#define W 1920
#define H 1624
static float4 *g_tex;

float4 LOAD_host(int8 t, int x, int y) {
  (void)t;
  if (x < 0 || y < 0 || x >= W || y >= H)
    abort();
  return g_tex[y * W + x];
}
void STORE_ALL_host(float4 c, int8 t, int x, int y) {
  (void)t;
  if (x < 0 || y < 0 || x >= W || y >= H)
    abort();
  g_tex[y * W + x] = c;
}

int main(void) {
  g_tex = malloc(sizeof *g_tex * W * H);
  for (int i = 0; i < W * H; i++)
    g_tex[i] = (float4){(i % 251) / 255.0f, (i % 241) / 255.0f, (i % 239) / 255.0f, 0.5f};
  struct inset_k k;
  memset(&k, 0, sizeof k);
  k.rect[0] = (int4){0, 1080, 544, 1624};
  k.rect[1] = (int4){960, 1080, 1504, 1624};
  k.w = W, k.h = H, k.ramp = 40.0f;
  for (int gy = 0; gy < (H + 7) / 8; gy++)
    for (int gx = 0; gx < (W + 7) / 8; gx++)
      for (int ty = 0; ty < 8; ty++)
        for (int tx = 0; tx < 8; tx++)
          inset_main(&k, gx, gy, tx, ty);
  int bad = 0;
  for (int y = 0; y < H; y++)
    for (int x = 0; x < W; x++) {
      int i = y * W + x;
      float4 c = g_tex[i];
      if (c.x != (i % 251) / 255.0f || c.y != (i % 241) / 255.0f || c.z != (i % 239) / 255.0f) {
        if (bad++ < 4)
          printf("FAIL: colour changed at %d,%d\n", x, y);
        continue;
      }
      int in = -1;
      for (int r = 0; r < 2; r++)
        if (x >= k.rect[r].x && y >= k.rect[r].y && x < k.rect[r].z && y < k.rect[r].w)
          in = r;
      if (in < 0) {
        if (c.w != 0.0f && bad++ < 4)
          printf("FAIL: alpha %f outside the insets at %d,%d\n", c.w, x, y);
        continue;
      }
      int4 r = k.rect[in];
      int d = x - r.x;
      d = r.z - 1 - x < d ? r.z - 1 - x : d;
      d = y - r.y < d ? y - r.y : d;
      d = r.w - 1 - y < d ? r.w - 1 - y : d;
      if ((d == 0 && c.w != 0.0f) || (d >= 40 && c.w != 1.0f) || (d > 0 && d < 40 && !(c.w > 0 && c.w < 1))) {
        if (bad++ < 4)
          printf("FAIL: alpha %f at %d,%d, %d texels in\n", c.w, x, y, d);
      }
    }
  /* rising along a line into the left inset from its left edge */
  for (int x = 1; x < 60; x++)
    if (g_tex[1350 * W + x].w < g_tex[1350 * W + x - 1].w && bad++ < 4)
      printf("FAIL: alpha falls at %d,1350\n", x);
  printf("  alpha along row 1350 from the left inset's edge: %.3f %.3f %.3f %.3f %.3f (0, 10, 20, 30, 40 in)\n",
         g_tex[1350 * W].w, g_tex[1350 * W + 10].w, g_tex[1350 * W + 20].w, g_tex[1350 * W + 30].w,
         g_tex[1350 * W + 40].w);
  /* the pass over the insets' rows only (oy, with Hmd2's window test): the insets' texels the same
   * as the whole texture's pass gave them, nothing above their rows touched */
  {
    float4 *full = malloc(sizeof *full * W * H);
    memcpy(full, g_tex, sizeof *full * W * H);
    for (int i = 0; i < W * H; i++)
      g_tex[i] = (float4){(i % 251) / 255.0f, (i % 241) / 255.0f, (i % 239) / 255.0f, 0.5f};
    k.oy = 1080;
    for (int gy = 0; gy < (H - 1080 + 7) / 8; gy++)
      for (int gx = 0; gx < (W + 7) / 8; gx++)
        for (int ty = 0; ty < 8; ty++)
          for (int tx = 0; tx < 8; tx++)
            inset_main(&k, gx, gy, tx, ty);
    int diff = 0, above = 0;
    for (int y = 0; y < H; y++)
      for (int x = 0; x < W; x++) {
        int i = y * W + x;
        if (y < 1080)
          above += g_tex[i].w != 0.5f;
        else
          diff += g_tex[i].x != full[i].x || g_tex[i].y != full[i].y || g_tex[i].z != full[i].z ||
                  g_tex[i].w != full[i].w;
      }
    if (diff || above)
      printf("FAIL: the rows-only pass: %d texels of the insets' rows differ, %d above touched\n", diff, above), bad++;
    else
      printf("  the pass over the insets' rows only: the same texels, %d rows of %d\n", H - 1080, H);
    memcpy(g_tex, full, sizeof *full * W * H);
    free(full);
    k.oy = 0;
  }
  /* the debug menu's place (menuon): each eye's give-way place moved in with its copy of the
   * menu (menu_dx: the left eye's right, the right eye's left), windows centred on the axes */
  {
    for (int i = 0; i < W * H; i++)
      g_tex[i].w = 0.5f;
    k.menuon = 1, k.menu_dx = 0.03f;
    k.menu[0] = -0.1f, k.menu[1] = 0.1f, k.menu[2] = -0.2f, k.menu[3] = 0.0f;
    k.ppt[2] = k.ppt[3] = 1000.0f;
    for (int gy = 0; gy < (H + 7) / 8; gy++)
      for (int gx = 0; gx < (W + 7) / 8; gx++)
        for (int ty = 0; ty < 8; ty++)
          for (int tx = 0; tx < 8; tx++)
            inset_main(&k, gx, gy, tx, ty);
    /* centres: left 272,1352, right 1232,1352; 50 below (tangent -0.05), 100 either side */
    float l_in = g_tex[1402 * W + 272 + 120].w, l_out = g_tex[1402 * W + 272 - 120].w;
    float r_in = g_tex[1402 * W + 1232 - 120].w, r_out = g_tex[1402 * W + 1232 + 120].w;
    if (l_in != 0.0f || l_out != 1.0f || r_in != 0.0f || r_out != 1.0f)
      printf("FAIL: the menu's place: left +0.12 %f, -0.12 %f; right -0.12 %f, +0.12 %f\n", l_in,
             l_out, r_in, r_out), bad++;
    else
      printf("  the menu's place moved in per eye: the left inset clear at +0.12 of tangent and drawn at -0.12, the right the other way\n");
  }
  /* agent.insetdebug=5 (the menu's "tints"; 6 before the give-way mode went): the left inset
   * green, the right red, outside them the colour as read */
  {
    for (int i = 0; i < W * H; i++)
      g_tex[i] = (float4){0.5f, 0.5f, 0.5f, 0.5f};
    k.menuon = 0, k.debug = 5;
    for (int gy = 0; gy < (H + 7) / 8; gy++)
      for (int gx = 0; gx < (W + 7) / 8; gx++)
        for (int ty = 0; ty < 8; ty++)
          for (int tx = 0; tx < 8; tx++)
            inset_main(&k, gx, gy, tx, ty);
    float4 l = g_tex[1350 * W + 272], r = g_tex[1350 * W + 1232], o = g_tex[500 * W + 272];
    if (l.y != 1.0f || l.x > 0.2f || r.x != 1.0f || r.y > 0.2f || o.x != 0.5f || o.w != 0.0f)
      printf("FAIL: debug 5: left %f %f, right %f %f, outside %f (alpha %f)\n", l.x, l.y, r.x, r.y,
             o.x, o.w), bad++;
    else
      printf("  debug 5: the left inset green, the right red\n");
    k.debug = 0;
  }
  printf(bad ? "FAIL (%d)\n" : "ok\n", bad);
  return bad != 0;
}
