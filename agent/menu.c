/* The in-headset menu: what can be switched while the game runs, from the controller, drawn in
 * the headset. A release build has the player's settings menu (k_items below, its choices saved
 * to the game's menu-<game>.txt); a debug build the debug menu (the other k_items: every live
 * option of the port, nothing saved).
 *  - L1 + R1 held, then a click of the touchpad: open or close it. The touchpad click goes to the
 *    agent while both shoulder buttons are held.
 *  - Open, the game gets no buttons, sticks or triggers. Up and down choose a line, left and
 *    right change its value (cross too, upwards, wrapping), circle closes it.
 *  - Each line is one of the agent's options or one of the game's integer variables (set in both
 *    threads' copies, as cvars.txt does). A change takes at once; the lines marked * take from
 *    the next start (the frame rate through framerate.txt, as before; the settings menu's others
 *    through its file). Not here: the palm tracker's live start (started mid-game it took the
 *    console down before).
 *  - Reset all puts every line back: the debug menu to its value when the menu was first opened;
 *    the settings menu to the default (the value without the menu's file: the port's own, or
 *    cvars.txt's), and removes its file.
 * The settings file (release builds): /data/summerlessonvr2/menu-<game>.txt, "name=value" lines
 * in cvars.txt's names (agent.hands=0, r.ShadowQuality=2, ...; the eye width as agent.resolution,
 * this file's own name, the payload reads it with resolution.txt), fetched from the payload as the
 * agent's options are read (CMD_MENU, menu_boot: its agent.* lines applied there, the game's
 * variables once the console manager is up, menu_cvars_apply, after cvars.txt's own lines, so
 * the menu's choices win) and written through the payload from the cvars thread within 2 s of
 * a change (menu_save): a line for each setting the player changed from the default, and no
 * file at all when there is none.
 * The picture (MENU_W x MENU_H, RGBA) is drawn here on the CPU into one of two buffers the GPU
 * reads, and copied into both eyes, head-locked a little below straight ahead at about 1.2 m
 * (each eye's copy moved in by that distance's half disparity), by the inset
 * shader's stage 2 (inset.c inset_menu_frame, from vr2.c every frame it is open); the insets give
 * way under it.
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include "vr2.h"
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

enum { M_OPT, M_CVAR, M_CVARF, M_CLOSE, M_RESET, M_VIEW }; /* M_CVARF: a float variable, in hundredths; M_VIEW: reset the view */
struct item {
  const char *label;
  int kind;
  volatile u32 *opt;  /* M_OPT */
  const char *cvar;   /* M_CVAR, M_CVARF */
  int lo, hi, step;
  const char *const *names; /* the values' names from lo, or 0 for the number */
  const u32 *vals;          /* a list: its choices, lo..hi their indices (the line's value its index;
                             * one not on the list, from a file, shown as its number) */
  const char *key;          /* the settings file's line (release builds), 0: none */
};
static const char *const k_offon[] = {"off", "on"};
#ifdef RELEASE
/* the player's settings (what cvars.txt can set that a player would, docs/SETTINGS.md) */
static const char *const k_widths[] = {"game's preset", "960", "1040", "1120", "1200", "1280", "1360", "1440",
                                       "1520", "1600", "1680", "1760", "1840", "1920", "2000"};
static const u32 k_width_vals[] = {0, 960, 1040, 1120, 1200, 1280, 1360, 1440, 1520, 1600, 1680, 1760, 1840, 1920, 2000};
#define WIDTH_DFR_HI 9 /* 1600: inset mode takes an asked width only up to there (multiview.c); the stock
                        * picture up to 2000 (2048 ran out of GPU memory at a scene change, docs/LIMITS.md) */
static const char *const k_shadows[] = {"low", "medium", "high"};
static const u32 k_shadow_vals[] = {2, 3, 5}; /* (the game's own 3, inset mode's 5) */
static const u32 k_dfr_vals[] = {0, 1}, k_hands_vals[] = {0, 60}, k_ssr_vals[] = {0, 2}, k_ao_vals[] = {0, 3},
                 k_bloom_vals[] = {0, 1}; /* (AO: inset mode's 3; the game's own 2 shows as "2") */
static const char *const k_tv[] = {"left eye", "both eyes", "black"};
/* the next start's choices (the file's lines) and the TV's (agent.mirrorboth 1, agent.tvblank 2) */
static volatile u32 g_next_width, g_next_dfr, g_next_hands, g_tv;
static const struct item k_items[] = {
    {"Frame rate*", M_OPT, &g_opt_nextfps, 0, 60, 120, 30, 0, 0, 0}, /* (framerate.txt) */
    {"Eye width*", M_OPT, &g_next_width, 0, 0, 14, 1, k_widths, k_width_vals, "agent.resolution"},
    {"Foveated rendering*", M_OPT, &g_next_dfr, 0, 0, 1, 1, k_offon, k_dfr_vals, "agent.multiview"},
    {"Hand tracking*", M_OPT, &g_next_hands, 0, 0, 1, 1, k_offon, k_hands_vals, "agent.hands"},
    {"Shadows", M_CVAR, 0, "r.ShadowQuality", 0, 2, 1, k_shadows, k_shadow_vals, "r.ShadowQuality"},
    {"Reflections", M_CVAR, 0, "r.SSR.Quality", 0, 1, 1, k_offon, k_ssr_vals, "r.SSR.Quality"},
    {"Ambient occlusion", M_CVAR, 0, "r.AmbientOcclusionLevels", 0, 1, 1, k_offon, k_ao_vals, "r.AmbientOcclusionLevels"},
    {"Bloom", M_CVAR, 0, "r.BloomQuality", 0, 1, 1, k_offon, k_bloom_vals, "r.BloomQuality"},
    {"TV picture", M_OPT, &g_tv, 0, 0, 2, 1, k_tv, 0, "tv"}, /* (two lines: agent.mirrorboth, agent.tvblank) */
    {"Reset view", M_VIEW, 0, 0, 0, 0, 0, 0, 0, 0},
    {"Reset all", M_RESET, 0, 0, 0, 0, 0, 0, 0, 0},
    {"Close", M_CLOSE, 0, 0, 0, 0, 0, 0, 0, 0},
};
#else
static const char *const k_views[] = {"off", "drawn", "shown"};
static const char *const k_grid[] = {"off", "magenta", "green", "cross", "grid", "tints"};
static const char *const k_gfilt[] = {"hold", "smooth"};
static const char *const k_ifx[] = {"none", "bloom", "AO", "bloom+AO", "refl", "bloom+refl", "AO+refl", "all"};
static const char *const k_aa[] = {"none", "FXAA", "TAA"};
static const char *const k_follow[] = {"centre", "gaze"}; /* (the test patterns 2-11 from cvars.txt: agent.insetgaze) */
/* our TAA's variants that are choices (the others, tests and their steps, from cvars.txt: agent.taa) */
static const char *const k_taa[] = {"the engine's", "copy", "velocity fix", "fix + quick new"};
static const u32 k_taa_vals[] = {0, 1, 11, 13};

static const struct item k_items[] = {
    {"Frame rate, next start", M_OPT, &g_opt_nextfps, 0, 60, 120, 30, 0}, /* (framerate.txt) */
    {"Insets", M_OPT, &g_opt_insetviews, 0, 0, 2, 1, k_views},
    {"Inset follows", M_OPT, &g_opt_insetgaze, 0, 0, 1, 1, k_follow},
    {"Inset density", M_OPT, &g_opt_insetppt, 0, 600, 1600, 100, 0},
    {"TAA shader", M_OPT, &g_opt_taa, 0, 0, 3, 1, k_taa, k_taa_vals},
    {"Inset PP from eye", M_OPT, &g_opt_insetpp, 0, 0, 7, 1, k_ifx},
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
    {"TV: headset view", M_OPT, &g_opt_tvhmd, 0, 0, 1, 1, k_offon},
    {"TV: black", M_OPT, &g_opt_tvblank, 0, 0, 1, 1, k_offon},
    {"Reset all (start values)", M_RESET, 0, 0, 0, 0, 0, 0},
    {"Close", M_CLOSE, 0, 0, 0, 0, 0, 0},
};
#endif
#define N_ITEMS (sizeof k_items / sizeof *k_items)

static struct {
  volatile u32 open, sel;
  u32 prev, shoulder_touch; /* the last buttons; the touchpad held with both shoulders (hidden) */
  volatile u32 dirty;       /* the picture to draw again */
  u8 *px[2];                /* the two pictures (GPU-readable) */
  volatile u32 front;       /* the one the GPU copies */
  u64 drawn_at;
  u32 said;
  int start[64]; /* each line's value for Reset all: the debug menu's when it was first opened; the
                  * settings menu's default (menu_boot, menu_cvars_apply) */
  u8 has_start[64], started;
#ifdef RELEASE
  u8 has[64];  /* the settings menu: lines the player chose (from the file, or changed here) */
  int set[64]; /* their values (the file's, until the game's variables can be set) */
  volatile u32 file_dirty; /* the file to write again (menu_save, the cvars thread) */
  u32 cvars_done;          /* the game's variables' defaults taken, the file's values set */
#endif
} g_m;

/* a line's value as it is: the option's or the game's variable's (the list's value, not its index) */
static int raw_of(const struct item *it, int *v) {
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
  return 0; /* (Close, Reset all, Reset view) */
}
/* (1 when it took: the game's variables need the console manager up) */
static int raw_set(const struct item *it, int n) {
  int ok = 1;
  if (it->kind == M_OPT)
    *it->opt = (u32)n;
  else if (it->kind == M_CVAR)
    ok = hooks_cvar_int(it->cvar, 0, &n);
  else if (it->kind == M_CVARF) {
    float f = (float)n / 100.0f;
    ok = hooks_cvar_float(it->cvar, 0, &f);
  }
#ifdef RELEASE
  if (it->opt == &g_tv) /* the TV picture: live (the mirror thread reads both each frame) */
    g_opt_mirrorboth = g_tv == 1, g_opt_tvblank = g_tv == 2;
#endif
  return ok;
}
/* the line's value as the menu steps it: 1 with a list's index, else the value itself; 2 with
 * the value itself when a list's isn't one of the list's (set from a file, or a variable's own,
 * negative too: r.AmbientOcclusionLevels' -1) */
static int value_of(const struct item *it, int *v) {
  if (!raw_of(it, v))
    return 0;
  if (it->vals) {
    u32 o = (u32)*v;
    for (int k = it->lo; k <= it->hi; k++)
      if (it->vals[k] == o) {
        *v = k;
        return 1;
      }
    return 2;
  }
  return 1;
}
/* n: a list's index, else the value */
static void set_value(const struct item *it, int n) {
  int raw = it->vals ? (int)it->vals[n] : n;
#ifdef RELEASE
  u32 i = (u32)(it - k_items);
  if (it->key)
    g_m.set[i] = raw; /* (before the set: the cvars thread's menu_cvars_apply, between the two, would set the old one back) */
#endif
  int ok = raw_set(it, raw);
#ifdef RELEASE
  if (it->key && ok) { /* (set, so the file can have it; the value asked, not read back: cvars_keep could
                        * hand back the old one between the set and the read) */
    g_m.has[i] = !(g_m.has_start[i] && raw == g_m.start[i]); /* (back at its default: the menu holds it no
                                                               * more, so a cvars.txt change while the game
                                                               * runs applies, as with no menu line) */
    __atomic_store_n(&g_m.file_dirty, 1, __ATOMIC_RELEASE); /* (after has and set: the cvars thread reads them) */
  }
#else
  (void)ok; /* (the debug menu saves nothing) */
#endif
}

#ifdef RELEASE
/* the eye width with DFR on: 1600 at most (inset mode takes an asked width only up to there: a
 * wider one left in the file would show while the rate's own ran) */
static void width_fit_dfr(void) {
  if (!g_next_dfr || g_next_width <= k_width_vals[WIDTH_DFR_HI])
    return;
  LOG("menu: eye width %u -> %u (the most with foveated rendering)", g_next_width, k_width_vals[WIDTH_DFR_HI]);
  u32 i = 0;
  while (k_items[i].opt != &g_next_width) i++;
  set_value(&k_items[i], WIDTH_DFR_HI);
}
#endif
static void change(const struct item *it, int dir, int wrap) {
  int v, r = value_of(it, &v);
  if (!r)
    return;
  int was = 0; /* (the list's value before, for the log) */
  if (it->vals)
    raw_of(it, &was);
  if (r == 2) { /* (a value off the list: the list's next one past it) */
    int k = -1;
    for (int q = it->lo; q <= it->hi; q++)
      if (dir > 0 ? (int)it->vals[q] > was && k < 0 : (int)it->vals[q] < was)
        k = q;
    if (k < 0) { /* (none: wrapped to the list's other end, or left as it is) */
      if (!wrap)
        return;
      k = dir > 0 ? it->lo : it->hi;
    }
    set_value(it, k);
    LOG("menu: %s %d -> %u", it->label, was, it->vals[k]);
    return;
  }
  int n = v + dir * it->step, hi = it->hi;
  if (build_newer_engine() && it->kind == M_CVAR && memcmp(it->cvar, "r.PostProcessAAQuality", 23) == 0)
    hi = 3; /* (Chisato, Allison: 4 and above shake the picture, the user worn; hooks.c's inset preset 3 too) */
#ifdef RELEASE
  if (it->opt == &g_next_width && g_next_dfr)
    hi = WIDTH_DFR_HI;
#endif
  if (n > hi)
    n = wrap ? it->lo : hi;
  if (n < it->lo)
    n = wrap ? hi : it->lo;
  if (n == v)
    return;
  set_value(it, n);
  if (it->vals)
    LOG("menu: %s %d -> %u", it->label, was, it->vals[n]);
  else
    LOG("menu: %s %d -> %d", it->label, v, n);
#ifdef RELEASE
  width_fit_dfr();
#endif
}

#ifndef RELEASE
/* each line's value as the menu is first opened, for Reset all */
static void remember_start(void) {
  if (g_m.started)
    return;
  g_m.started = 1;
  for (u32 i = 0; i < N_ITEMS && i < 64; i++)
    g_m.has_start[i] = (u8)raw_of(&k_items[i], &g_m.start[i]);
}
#endif
static void reset_all(void) {
  for (u32 i = 0; i < N_ITEMS && i < 64; i++) {
    const struct item *it = &k_items[i];
    int v;
#ifdef RELEASE
    g_m.has[i] = 0; /* (the file goes: menu_save) */
#endif
    if (!g_m.has_start[i] || !raw_of(it, &v) || v == g_m.start[i])
      continue;
    raw_set(it, g_m.start[i]);
    LOG("menu: %s %d -> %d (reset)", it->label, v, g_m.start[i]);
  }
#ifdef RELEASE
  __atomic_store_n(&g_m.file_dirty, 1, __ATOMIC_RELEASE);
#endif
}

/* the player's controller sample (hooks.c, the game's pad thread) */
void menu_pad(u8 *d) {
  u32 b = *(u32 *)d, pressed = b & ~g_m.prev;
  g_m.prev = b;
  int shoulders = (b & (PAD_L1 | PAD_R1)) == (PAD_L1 | PAD_R1);
  if (shoulders && (pressed & PAD_TOUCH)) {
    g_m.open = !g_m.open, g_m.dirty = 1;
#ifndef RELEASE
    if (g_m.open)
      remember_start();
#endif
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
      else if (it->kind == M_VIEW)
        vr_reset_view("the menu's Reset view");
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

#ifdef RELEASE
/* ---- the settings file (menu-<game>.txt, through the payload: CMD_MENU) ---- */
static char g_file[512]; /* as fetched (the payload keeps it to this) */

/* At the end of cvars_fetch (hooks.c), the agent's options as cvars.txt left them: those are
 * the defaults; then the file's lines over them (agent.hands, the TV's: applied here; the
 * game's variables kept for menu_cvars_apply; agent.multiview and agent.resolution the payload
 * read itself before the boot, the lines say what it did). */
void menu_boot(void) {
  g_next_dfr = g_multiview_rows != 0; /* (the state; the default is what cvars.txt said) */
  g_next_hands = g_opt_hands;
  g_tv = g_opt_tvblank ? 2 : g_opt_mirrorboth ? 1 : 0;
  for (u32 i = 0; i < N_ITEMS; i++)
    if (k_items[i].kind == M_OPT) {
      g_m.has_start[i] = 1;
      g_m.start[i] = k_items[i].opt == &g_opt_nextfps ? 60
                     : k_items[i].opt == &g_next_dfr  ? (int)(g_cvars_multiview != 0)
                                                       : (int)*k_items[i].opt;
    }
  if (agent_call(CMD_MENU, (u64)g_file, sizeof g_file, 0, 0, 3000, 0, 0) != 0)
    g_file[0] = 0; /* (a payload without the command: no file) */
  g_file[sizeof g_file - 1] = 0;
  u32 n = 0, mb = 0, tb = 0, tv_line = 0;
  for (const char *p = g_file; *p;) {
    const char *line = p, *eq;
    while (*p && *p != '\n') p++;
    u32 len = (u32)(p - line);
    if (*p) p++;
    for (eq = line; eq < line + len && *eq != '='; eq++)
      ;
    if (eq >= line + len)
      continue;
    int neg = eq[1] == '-', v = 0;
    for (const char *d = eq + 1 + neg; d < line + len && *d >= '0' && *d <= '9' && v < 100000000; d++)
      v = v * 10 + (*d - '0'); /* (the digits stop short of overflowing: a hand-edited file) */
    if (neg)
      v = -v;
    u32 klen = (u32)(eq - line), i = 0;
    if (klen == 16 && memcmp(line, "agent.mirrorboth", 16) == 0) {
      mb = (u32)v, tv_line = 1;
      continue;
    }
    if (klen == 13 && memcmp(line, "agent.tvblank", 13) == 0) {
      tb = (u32)v, tv_line = 1;
      continue;
    }
    for (; i < N_ITEMS; i++)
      if (k_items[i].key && strlen(k_items[i].key) == klen && memcmp(k_items[i].key, line, klen) == 0)
        break;
    if (i == N_ITEMS || k_items[i].opt == &g_tv) {
      LOG("menu: the file's line '%.*s' is not one of the menu's", (int)len, line);
      continue;
    }
    g_m.has[i] = 1, g_m.set[i] = v, n++;
    if (k_items[i].opt == &g_next_hands)
      g_opt_hands = (u32)v, LOG("menu: agent.hands = %d (the file)", v);
    if (k_items[i].opt == &g_next_dfr)
      g_m.set[i] = v != 0; /* (any number: on, at the rate's own rows; the payload took it) */
    if (k_items[i].kind == M_OPT)
      *k_items[i].opt = (u32)g_m.set[i];
  }
  if (tv_line) {
    u32 i = 0;
    while (k_items[i].opt != &g_tv) i++;
    g_tv = tb ? 2 : mb ? 1 : 0;
    g_m.has[i] = 1, g_m.set[i] = (int)g_tv, n++;
    g_opt_mirrorboth = g_tv == 1, g_opt_tvblank = g_tv == 2;
    LOG("menu: the TV %s (the file)", k_tv[g_tv]);
  }
  if (g_file[0])
    LOG("menu: %u settings from the game's menu file", n);
  width_fit_dfr(); /* (a file saying both: the file put right at the next save) */
}

/* In apply_cvars (hooks.c), after cvars.txt's lines, every time: first the game's variables'
 * defaults (as inset mode and cvars.txt left them), then the menu's choices over them (so they
 * win, and a change of cvars.txt while the game runs leaves them in force) */
void menu_cvars_apply(void) {
  int v;
  if (!hooks_cvar_int("r.ShadowQuality", &v, 0))
    return; /* (the console manager not up yet: as cvars.txt's lines, at the next apply) */
  for (u32 i = 0; i < N_ITEMS; i++) {
    const struct item *it = &k_items[i];
    if (it->kind != M_CVAR)
      continue;
    if (!g_m.cvars_done)
      g_m.has_start[i] = (u8)raw_of(it, &g_m.start[i]);
    if (g_m.has[i]) {
      raw_set(it, g_m.set[i]);
      if (!g_m.cvars_done)
        LOG("menu: %s = %d (the file)", it->key, g_m.set[i]);
    }
  }
  g_m.cvars_done = 1;
}

/* The cvars thread, every 2 s: the file written when a line changed since (within 2 s of the
 * player's presses, not every press): a line for each setting that isn't its default, none
 * at all (the file removed) when there is none. A payload without the command: said once. */
void menu_save(void) {
  static char text[512];
  static u32 said;
  if (!__atomic_exchange_n(&g_m.file_dirty, 0, __ATOMIC_ACQ_REL))
    return;
  u32 o = 0;
  for (u32 i = 0; i < N_ITEMS; i++) {
    const struct item *it = &k_items[i];
    if (!it->key || !g_m.has[i] || (g_m.has_start[i] && g_m.set[i] == g_m.start[i]))
      continue;
    if (it->opt == &g_tv)
      o += (u32)agent_snprintf(text + o, sizeof text - o, "agent.mirrorboth=%u\nagent.tvblank=%u\n",
                               (u32)(g_m.set[i] == 1), (u32)(g_m.set[i] == 2));
    else
      o += (u32)agent_snprintf(text + o, sizeof text - o, "%s=%d\n", it->key, g_m.set[i]);
  }
  if (o >= sizeof text)
    o = sizeof text - 1;
  int rc = agent_call(CMD_MENU, (u64)text, o, 1, 0, 2000, 0, 0);
  if (rc == 0)
    LOG("menu: %s", o ? "the settings saved for the next start" : "no settings: the file removed");
  else if (!said++)
    LOG("menu: the payload did not save the settings (an older payload?)");
}
#else
void menu_boot(void) {}
void menu_cvars_apply(void) {}
void menu_save(void) {}
#endif

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
#ifdef RELEASE
  text(p, 6, 3, "SETTINGS  L1+R1+pad", RGBA(255, 210, 60, 255));
  text(p, 14, MENU_H - 16, "* from the next start", RGBA(180, 180, 180, 255));
#else
  text(p, 6, 3, "DEBUG MENU  L1+R1+pad", RGBA(255, 210, 60, 255));
#endif
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
    int v, r;
    char val[24];
    if (it->kind == M_CLOSE || it->kind == M_RESET || it->kind == M_VIEW)
      val[0] = 0;
    else if (!(r = value_of(it, &v)))
      memcpy(val, "-", 2);
    else if (r == 2)
      fmt_int(val, v); /* (a value off the list) */
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
