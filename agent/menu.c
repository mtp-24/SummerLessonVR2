/* The debug menu (debug builds only): what can be switched while the game runs, from the
 * controller, drawn in the headset.
 *  - L1 + R1 held, then a click of the touchpad: open or close it. The touchpad click goes to the
 *    agent while both shoulder buttons are held.
 *  - Open, the game gets no buttons, sticks or triggers. Up and down choose a line, left and
 *    right change its value (cross too, upwards, wrapping), circle closes it.
 *  - Each line is one of the agent's live options or one of the game's integer variables (set
 *    in both threads' copies, as cvars.txt does): everything the port adds that can change while
 *    the game runs. Not here: what is set at the launch (agent.multiview's layout, the frame rate,
 *    the resolution, eye tracking's start) and the palm tracker (started mid-game it took the
 *    console down before). A change takes at once; cvars.txt changed later sets its own again.
 *  - Reset all puts every line back to its value when the menu was first opened.
 * The picture (MENU_W x MENU_H, RGBA) is drawn here on the CPU into one of two buffers the GPU
 * reads, and copied into both eyes, head-locked a little below straight ahead at about 1.2 m
 * (each eye's copy moved in by that distance's half disparity), by the inset
 * shader's stage 2 (inset.c inset_menu_frame, from vr2.c every frame it is open); the insets give
 * way under it.
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include "vr2.h"

#ifndef RELEASE
#include "font8x12.h"

#define PAD_L1 0x0400u
#define PAD_R1 0x0800u
#define PAD_TOUCH 0x100000u
#define PAD_UP 0x0010u
#define PAD_RIGHT 0x0020u
#define PAD_DOWN 0x0040u
#define PAD_LEFT 0x0080u
#define PAD_CROSS 0x4000u
#define PAD_CIRCLE 0x2000u
#define PAD_INTERCEPTED 0x80000000u

enum { M_OPT, M_CVAR, M_CVARF, M_CLOSE, M_RESET, M_LIST }; /* M_CVARF: a float variable, in hundredths; M_LIST: an
                                                              * option among vals' values (the line's value its index) */
struct item {
  const char *label;
  int kind;
  volatile u32 *opt;  /* M_OPT */
  const char *cvar;   /* M_CVAR */
  int lo, hi, step;
  const char *const *names; /* the values' names from lo, or 0 for the number */
  const u32 *vals;          /* M_LIST: the values, lo..hi their indices */
};
static const char *const k_offon[] = {"off", "on"};
static const char *const k_views[] = {"off", "drawn", "shown"};
static const char *const k_grid[] = {"off", "magenta", "green", "cross", "grid", "tints"};
static const char *const k_iaam[] = {"none", "FXAA", "TAA"};
static const char *const k_gfilt[] = {"hold", "smooth"};
static const char *const k_ifx[] = {"none", "bloom", "AO", "bloom+AO", "refl", "bloom+refl", "AO+refl", "all"};
static const char *const k_vig[] = {"off", "eyes only", "all views"};
static const char *const k_aa[] = {"none", "FXAA", "TAA"};
static const char *const k_follow[] = {"centre", "gaze"}; /* (the test patterns 2-11 from cvars.txt: agent.insetgaze) */
static const char *const k_snap[] = {"off", "1 pixel", "2 pixels"};
/* our TAA's variants that are choices (the others, tests and their steps, from cvars.txt: agent.taa) */
static const char *const k_taa[] = {"the engine's", "copy", "velocity fix", "fix + quick new"};
static const u32 k_taa_vals[] = {0, 1, 11, 13};

static const struct item k_items[] = {
    {"Frame rate, next start", M_OPT, &g_opt_nextfps, 0, 60, 120, 30, 0}, /* (framerate.txt) */
    {"Insets", M_OPT, &g_opt_insetviews, 0, 0, 2, 1, k_views},
    {"Inset follows", M_OPT, &g_opt_insetgaze, 0, 0, 1, 1, k_follow},
    {"Inset density", M_OPT, &g_opt_insetppt, 0, 600, 1600, 100, 0},
    {"Inset AA", M_OPT, &g_opt_insetaam, 0, 0, 2, 1, k_iaam}, /* (the engine's; TAA: a view state) */
    {"TAA shader", M_LIST, &g_opt_taa, 0, 0, 3, 1, k_taa, k_taa_vals},
    {"Inset window steps", M_OPT, &g_opt_insetsnap, 0, 0, 2, 1, k_snap},
    {"Inset PP from eye", M_OPT, &g_opt_insetpp, 0, 0, 7, 1, k_ifx},
    {"Vignette", M_OPT, &g_opt_vignette, 0, 0, 2, 1, k_vig},
    {"Inset grid", M_OPT, &g_opt_insetdebug, 0, 0, 5, 1, k_grid},
    {"Gaze filter", M_OPT, &g_opt_gazefilter, 0, 0, 1, 1, k_gfilt},
    {"Gaze rest cut (1/100 Hz)", M_OPT, &g_opt_gazemincut, 0, 10, 300, 10, 0},
    {"Gaze smooth speed", M_OPT, &g_opt_gazebeta, 0, 0, 30, 1, 0},
    {"Gaze hold / jump (1/1000)", M_OPT, &g_opt_gazehold, 0, 0, 150, 10, 0},
    {"Gaze dot", M_OPT, &g_opt_gazedot, 0, 0, 1, 1, k_offon},
    {"Shadows", M_CVAR, 0, "r.ShadowQuality", 0, 5, 1, 0},
    {"Reflections", M_CVAR, 0, "r.SSR.Quality", 0, 4, 1, 0},
    {"Ambient occlusion", M_CVAR, 0, "r.AmbientOcclusionLevels", -1, 3, 1, 0},
    {"Bloom", M_CVAR, 0, "r.BloomQuality", 0, 5, 1, 0},
    {"Tonemapper (2+: vignette)", M_CVAR, 0, "r.Tonemapper.Quality", 0, 5, 1, 0},
    {"Motion blur", M_CVAR, 0, "r.MotionBlurQuality", 0, 4, 1, 0},
    {"Depth of field", M_CVAR, 0, "r.DepthOfFieldQuality", 0, 2, 1, 0},
    {"AA type", M_CVAR, 0, "r.DefaultFeature.AntiAliasing", 0, 2, 1, k_aa},
    {"AA quality", M_CVAR, 0, "r.PostProcessAAQuality", 0, 6, 1, 0},
    {"Occlusion culling", M_CVAR, 0, "r.AllowOcclusionQueries", 0, 1, 1, k_offon},
    {"LOD fade off", M_CVAR, 0, "r.DisableLODFade", 0, 1, 1, k_offon},
    {"LOD scale (x100)", M_CVARF, 0, "r.StaticMeshLODDistanceScale", 10, 200, 10, 0},
    {"See-through light res", M_CVAR, 0, "r.TranslucencyLightingVolumeDim", 16, 128, 16, 0},
    {"Prediction (ms)", M_OPT, &g_opt_predict, 0, 0, 60, 5, 0},
    {"TV: headset view", M_OPT, &g_opt_tvhmd, 0, 0, 1, 1, k_offon},
    {"TV: black", M_OPT, &g_opt_tvblank, 0, 0, 1, 1, k_offon},
    {"Reset all (start values)", M_RESET, 0, 0, 0, 0, 0, 0},
    {"Close", M_CLOSE, 0, 0, 0, 0, 0, 0},
};
#define N_ITEMS (sizeof k_items / sizeof *k_items)

static struct {
  volatile u32 open, sel;
  u32 prev, shoulder_touch; /* the last buttons; the touchpad held with both shoulders (hidden) */
  volatile u32 dirty;       /* the picture to draw again */
  u8 *px[2];                /* the two pictures (GPU-readable) */
  volatile u32 front;       /* the one the GPU copies */
  u64 drawn_at;
  u32 said;
  int start[64]; /* each line's value when the menu was first opened (Reset all) */
  u8 has_start[64], started;
} g_m;

static int value_of(const struct item *it, int *v) {
  if (it->kind == M_LIST) { /* its index, or -1 - the value when it isn't one of the list's (set from cvars.txt) */
    u32 o = *it->opt;
    *v = -1 - (int)o;
    for (int k = it->lo; k <= it->hi; k++)
      if (it->vals[k] == o)
        *v = k;
    return 1;
  }
  if (it->kind == M_OPT) {
    *v = (int)*it->opt;
    return 1;
  }
  if (it->kind == M_CVAR)
    return hooks_cvar_int(it->cvar, v, 0);
  if (it->kind == M_CVARF) {
    float f;
    if (!hooks_cvar_float(it->cvar, &f, 0))
      return 0;
    *v = (int)(f * 100.0f + (f >= 0 ? 0.5f : -0.5f));
    return 1;
  }
  return 0; /* (Close, Reset all) */
}

static void set_value(const struct item *it, int n) {
  if (it->kind == M_LIST)
    *it->opt = n >= 0 ? it->vals[n] : (u32)(-1 - n);
  else if (it->kind == M_OPT)
    *it->opt = (u32)n;
  else if (it->kind == M_CVAR)
    hooks_cvar_int(it->cvar, 0, &n);
  else if (it->kind == M_CVARF) {
    float f = (float)n / 100.0f;
    hooks_cvar_float(it->cvar, 0, &f);
  }
}

static void change(const struct item *it, int dir, int wrap) {
  int v;
  if (!value_of(it, &v))
    return;
  u32 was = it->kind == M_LIST ? *it->opt : 0; /* (the list's value before, for the log) */
  if (it->kind == M_LIST && v < 0) { /* (a value off the list, from cvars.txt: the list's next one past it) */
    int k = -1;
    for (int q = it->lo; q <= it->hi; q++)
      if (dir > 0 ? it->vals[q] > was && k < 0 : it->vals[q] < was)
        k = q;
    if (k < 0) { /* (none: wrapped to the list's other end, or left as it is) */
      if (!wrap)
        return;
      k = dir > 0 ? it->lo : it->hi;
    }
    set_value(it, k);
    LOG("menu: %s %u -> %u", it->label, was, it->vals[k]);
    return;
  }
  int n = v + dir * it->step, hi = it->hi;
  if (build_newer_engine() && it->kind == M_CVAR && memcmp(it->cvar, "r.PostProcessAAQuality", 23) == 0)
    hi = 3; /* (Chisato, Allison: 4 and above shake the picture, the user worn; hooks.c's inset preset 3 too) */
  if (n > hi)
    n = wrap ? it->lo : hi;
  if (n < it->lo)
    n = wrap ? hi : it->lo;
  if (n == v)
    return;
  set_value(it, n);
  if (it->kind == M_LIST)
    LOG("menu: %s %u -> %u", it->label, was, it->vals[n]);
  else
    LOG("menu: %s %d -> %d", it->label, v, n);
}

/* each line's value as the menu is first opened, for Reset all */
static void remember_start(void) {
  if (g_m.started)
    return;
  g_m.started = 1;
  for (u32 i = 0; i < N_ITEMS && i < 64; i++)
    g_m.has_start[i] = (u8)value_of(&k_items[i], &g_m.start[i]);
}
static void reset_all(void) {
  for (u32 i = 0; i < N_ITEMS && i < 64; i++) {
    const struct item *it = &k_items[i];
    int v;
    if (!g_m.has_start[i] || !value_of(it, &v) || v == g_m.start[i])
      continue;
    set_value(it, g_m.start[i]);
    LOG("menu: %s %d -> %d (reset)", it->label, v, g_m.start[i]);
  }
}

/* the player's controller sample (hooks.c, the game's pad thread) */
void menu_pad(u8 *d) {
  u32 b = *(u32 *)d, pressed = b & ~g_m.prev;
  g_m.prev = b;
  int shoulders = (b & (PAD_L1 | PAD_R1)) == (PAD_L1 | PAD_R1);
  if (shoulders && (pressed & PAD_TOUCH)) {
    g_m.open = !g_m.open, g_m.dirty = 1;
    if (g_m.open)
      remember_start();
    LOG("menu: %s", g_m.open ? "open" : "closed");
  }
  g_m.shoulder_touch = shoulders && (b & PAD_TOUCH);
  if (g_m.open) {
    const struct item *it = &k_items[g_m.sel];
    if (pressed & PAD_UP)
      g_m.sel = (g_m.sel + N_ITEMS - 1) % N_ITEMS, g_m.dirty = 1;
    if (pressed & PAD_DOWN)
      g_m.sel = (g_m.sel + 1) % N_ITEMS, g_m.dirty = 1;
    if (pressed & PAD_LEFT)
      change(it, -1, 0), g_m.dirty = 1;
    if (pressed & PAD_RIGHT)
      change(it, 1, 0), g_m.dirty = 1;
    if (pressed & PAD_CROSS) {
      if (it->kind == M_CLOSE)
        g_m.open = 0, LOG("menu: closed");
      else if (it->kind == M_RESET)
        reset_all();
      else
        change(it, 1, 1);
      g_m.dirty = 1;
    }
    if (pressed & PAD_CIRCLE)
      g_m.open = 0, g_m.dirty = 1, LOG("menu: closed");
  }
  if (g_m.open || g_m.shoulder_touch) { /* the game sees nothing of it */
    *(u32 *)d = b & PAD_INTERCEPTED;
    d[4] = d[5] = d[6] = d[7] = 0x80; /* sticks centred */
    d[8] = d[9] = 0;                  /* triggers */
  }
}

/* ---- the picture ---- */
static void fill(u8 *p, u32 x0, u32 y0, u32 w, u32 h, u32 rgba) {
  for (u32 y = y0; y < y0 + h && y < MENU_H; y++)
    for (u32 x = x0; x < x0 + w && x < MENU_W; x++)
      memcpy(p + (y * MENU_W + x) * 4, &rgba, 4);
}
static void text(u8 *p, u32 x, u32 y, const char *s, u32 rgba) {
  for (; *s && x + 8 <= MENU_W; s++, x += 8) {
    u32 c = (u8)*s;
    if (c < 32 || c > 126)
      c = '?';
    const u8 *g = k_font8x12[c - 32];
    for (u32 r = 0; r < 12; r++)
      for (u32 i = 0; i < 8; i++)
        if (g[r] & (0x80u >> i))
          memcpy(p + ((y + r) * MENU_W + x + i) * 4, &rgba, 4);
  }
}
static void fmt_int(char *o, int v) {
  char t[12];
  int n = 0, neg = v < 0;
  u32 u = neg ? (u32)-v : (u32)v;
  do
    t[n++] = (char)('0' + u % 10), u /= 10;
  while (u);
  if (neg)
    *o++ = '-';
  while (n)
    *o++ = t[--n];
  *o = 0;
}
#define RGBA(r, g, b, a) ((u32)(r) | (u32)(g) << 8 | (u32)(b) << 16 | (u32)(a) << 24)
static void draw(u8 *p) {
  fill(p, 0, 0, MENU_W, MENU_H, RGBA(16, 18, 24, 215));
  fill(p, 0, 0, MENU_W, 2, RGBA(255, 210, 60, 255));
  fill(p, 0, MENU_H - 2, MENU_W, 2, RGBA(255, 210, 60, 255));
  text(p, 6, 3, "DEBUG MENU  L1+R1+pad", RGBA(255, 210, 60, 255));
  /* as many lines as fit, scrolled to keep the chosen one in view */
  u32 rows = (MENU_H - 20) / 11, first = 0;
  if (g_m.sel >= rows / 2)
    first = g_m.sel - rows / 2;
  if (first + rows > N_ITEMS)
    first = N_ITEMS > rows ? N_ITEMS - rows : 0;
  if (first)
    text(p, MENU_W - 14, 3, "^", RGBA(255, 210, 60, 255));
  if (first + rows < N_ITEMS)
    text(p, MENU_W - 14, MENU_H - 14, "v", RGBA(255, 210, 60, 255));
  for (u32 i = first; i < N_ITEMS && i < first + rows; i++) {
    const struct item *it = &k_items[i];
    u32 y = 18 + (i - first) * 11, sel = i == g_m.sel;
    u32 col = sel ? RGBA(255, 230, 90, 255) : RGBA(235, 235, 235, 255);
    if (sel)
      fill(p, 2, y - 1, MENU_W - 4, 12, RGBA(70, 60, 20, 230));
    text(p, 4, y, sel ? ">" : " ", col);
    text(p, 14, y, it->label, col);
    int v;
    char val[24];
    if (it->kind == M_CLOSE || it->kind == M_RESET)
      val[0] = 0;
    else if (!value_of(it, &v))
      memcpy(val, "-", 2);
    else if (it->kind == M_LIST && v < 0)
      fmt_int(val, -1 - v); /* (a variant off the list, from cvars.txt) */
    else if (it->names && v >= it->lo && v <= it->hi)
      memcpy(val, it->names[v - it->lo], strlen(it->names[v - it->lo]) + 1);
    else
      fmt_int(val, v);
    u32 len = (u32)strlen(val);
    text(p, MENU_W - 6 - len * 8, y, val, col);
  }
}

/* the picture to copy while the menu is open (vr2.c each frame), else 0: drawn again when
 * something changed, and every half second for the game's variables */
const u8 *menu_picture(void) {
  if (!g_m.open)
    return 0;
  if (!g_m.px[0]) {
    u8 *m = vr2_dalloc(2 * MENU_W * MENU_H * 4, 0x10000);
    if (!m) {
      if (!g_m.said++)
        LOG("menu: no memory for its picture");
      g_m.open = 0;
      return 0;
    }
    g_m.px[0] = m, g_m.px[1] = m + MENU_W * MENU_H * 4;
    g_m.dirty = 1;
  }
  u64 now = sceKernelGetProcessTime();
  if (g_m.dirty || now - g_m.drawn_at > 500000) {
    g_m.dirty = 0, g_m.drawn_at = now;
    u32 back = !g_m.front;
    draw(g_m.px[back]);
    __atomic_thread_fence(__ATOMIC_SEQ_CST); /* the picture written before it is the front */
    g_m.front = back;
  }
  return g_m.px[g_m.front];
}
#else
void menu_pad(u8 *sample) { (void)sample; }
const u8 *menu_picture(void) { return 0; }
#endif
