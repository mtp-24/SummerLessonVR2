/* Multi-view step 4 (agent/inset.c, docs/MULTIVIEW.md): the alpha that makes the two inset views
 * a layer of their own. Hmd2's second layer is the same texture as the first (the game's), its
 * viewports the insets' rects, in straight alpha: Hmd2 never clips a layer to its window and
 * samples it wherever the eye looks (clamped at the texture's edge), so the texture's alpha has
 * to be 0 everywhere but the insets, and within them rise from 0 at their outermost texels to 1
 * `ramp` texels in (smoothstep), for an edge that fades into the periphery under it. Layer 0
 * (the periphery) reads no alpha. Each texel is read and written back whole with its new alpha:
 * a store of the alpha alone (dmask 0x8) turned the whole picture black on the console
 * (2026-11-07, captures mv25/mv26: the colour written as 0), so the colour goes back as read
 * (UNORM, exact). The pass runs once the picture is complete (agent/inset.c).
 *
 * One thread a texel over the whole texture. Built for the PS4's GPU (gfx701) by tools/gcn_cc.py
 * (GCN_ENTRY=inset_main) into agent/inset_gcn.h; tools/fov/inset_test.c runs it on the host.
 * SPDX-License-Identifier: GPL-3.0-or-later */
typedef int int4 __attribute__((ext_vector_type(4)));
typedef int int8 __attribute__((ext_vector_type(8)));

/* The constants for one dispatch, written by agent/inset.c (struct consts there, the same layout).
 * The pads hold the places of fields since removed: the offsets stay as they were. */
struct inset_k {
  int8 tex;    /* T#: the game's texture, read and written (UNORM) */
  int4 rect[2]; /* the insets: x0, y0, x1, y1 (x1, y1 past the last texel) */
  int w, h;    /* the texture's size */
  float ramp;  /* the edge's width in texels (> 0) */
  int debug;   /* agent.insetdebug=1: outside the insets magenta (alpha 0), to see whether the
                * pass's writes reach Hmd2; 2: the insets green, to find them in its output;
                * 3: a crosshair at each eye's axis in the periphery (magenta) and at each
                * inset's centre (green): the same direction, so one cross in Hmd2's output if
                * it places layer 1 right; 4: a grid of lines every 0.1 of tangent from the
                * axis, out to 0.6, in the periphery (magenta) and in the insets (green): where
                * the green lines leave the magenta ones in Hmd2's output gives layer 1's
                * placement and scale; 5: the left inset green, the right red (which one an eye
                * shows) */
  int axis[2][2]; /* each eye's axis (straight ahead) in the periphery, in texels */
  float ppt[4];   /* pixels a unit of tangent: the periphery's across and up, the insets' */
  int pad0;
  int stage;      /* 2: the debug menu (menu_blit) */
  int pad1;
  int oy;         /* the pass: the first row it covers (0, or the insets' rows when Hmd2's window
                   * test keeps layer 1 to them: thread row 0 is texture row oy) */
  int pad6[10];   /* (40 bytes) */
  float gaze[2];  /* agent.gazedot=1: the gaze's tangents (x right, y up) */
  int gazedot;    /* 1: a ring at the gaze, in the periphery and in the insets */
  int pad2[3];
  int pad7[8];    /* (32 bytes) */
  float menu[4];  /* the debug menu's place, in tangent from each eye's axis: x from, to (right),
                   * y from, to (up); the insets give way there (menuon) */
  int menuon;
  float menu_dx; /* each eye's copy moved in by it (tangent): the left eye's +, the right eye's - */
  int pad3[2];
  int8 mtex;      /* stage 2: the menu's picture (linear RGBA, mw x mh) */
  int mat[2][2];  /* stage 2: where its top-left goes in each eye, texels */
  int mw, mh;
  int pad4[2];
  float ictan[4]; /* each inset's window centre in tangent from its eye's axis: left x, y, right x,
                   * y (x right, y up; 0 straight ahead, the gaze's with agent.insetgaze) */
  int pad5[4];
};
_Static_assert(sizeof(struct inset_k) == 352, "inset constants (agent/inset.c)");
_Static_assert(__builtin_offsetof(struct inset_k, ictan) == 320, "inset constants");
_Static_assert(__builtin_offsetof(struct inset_k, mtex) == 256, "inset constants");
_Static_assert(__builtin_offsetof(struct inset_k, menu) == 224, "inset constants");
_Static_assert(__builtin_offsetof(struct inset_k, gaze) == 168, "inset constants");
_Static_assert(__builtin_offsetof(struct inset_k, stage) == 116, "inset constants");
_Static_assert(__builtin_offsetof(struct inset_k, w) == 64, "inset constants");

#ifdef __AMDGPU__
#define K __attribute__((address_space(4))) const struct inset_k
typedef float float4 __attribute__((ext_vector_type(4)));
float4 img_load(int dmask, int x, int y, int8 t, int te, int cp)
    __asm("llvm.amdgcn.image.load.2d.v4f32.i32");
void img_store_4(float4 v, int dmask, int x, int y, int8 t, int te, int cp)
    __asm("llvm.amdgcn.image.store.2d.v4f32.i32");
#define LOAD(t, x, y) img_load(15, (x), (y), (t), 0, 0)
#define STORE_ALL(c, t, x, y) img_store_4((c), 15, (x), (y), (t), 0, 0)
#else
#define K const struct inset_k
float4 LOAD_host(int8 t, int x, int y);
void STORE_ALL_host(float4 c, int8 t, int x, int y);
#define LOAD(t, x, y) LOAD_host((t), (x), (y))
#define STORE_ALL(c, t, x, y) STORE_ALL_host((c), (t), (x), (y))
#endif

static inline __attribute__((always_inline)) float mnf(float a, float b) { return a < b ? a : b; }
static inline __attribute__((always_inline)) float absf(float v) { return v < 0.0f ? -v : v; }
/* on a grid line: (d texels from the axis) within `half` texels of a multiple of 0.1 of tangent
 * (pp texels a unit), out to 0.6 */
static inline __attribute__((always_inline)) int on_grid(float d, float pp, float half) {
  float m = d * 10.0f / pp;
  float n = __builtin_floorf(m + 0.5f);
  return absf(n) <= 6.0f && absf(m - n) * pp * 0.1f <= half;
}
static inline __attribute__((always_inline)) int abs_i(int v) { return v < 0 ? -v : v; }
/* a value the same for every texel, kept in a scalar register: a choice between two of them
 * by a per-texel test would otherwise become a per-texel load from a chosen address (a
 * buffer_load, which tools/gcn_cc.py refuses) */
#ifdef __AMDGPU__
#define UNI(v) __builtin_amdgcn_readfirstlane(v)
/* the same for a float (through its bits) */
#define UNIF(v)                                                                                    \
  ({                                                                                               \
    union { float f; int i; } u_;                                                                  \
    u_.f = (v);                                                                                    \
    u_.i = __builtin_amdgcn_readfirstlane(u_.i);                                                   \
    u_.f;                                                                                          \
  })
#else
#define UNI(v) (v)
#define UNIF(v) (v)
#endif

/* the alpha of one inset at texel (x, y): 0 outside it and on its outermost texels */
static inline __attribute__((always_inline)) float alpha_in(int4 r, float ramp, int x, int y) {
  if (x < r.x || y < r.y || x >= r.z || y >= r.w)
    return 0.0f;
  /* texels from the nearest edge: 0 on the outermost ring */
  float d = mnf(mnf((float)(x - r.x), (float)(r.z - 1 - x)),
                mnf((float)(y - r.y), (float)(r.w - 1 - y)));
  float t = d / ramp;
  t = t > 1.0f ? 1.0f : t;
  return t * t * (3.0f - 2.0f * t);
}

/* the alpha at texel (x, y): 0 outside the insets. Both insets are read as values and their
 * alphas combined (a loop choosing one by the texel became a per-texel load from memory) */
static inline __attribute__((always_inline)) float inset_alpha(K *k, int x, int y) {
  int4 r0 = k->rect[0], r1 = k->rect[1];
  float a0 = alpha_in(r0, k->ramp, x, y), a1 = alpha_in(r1, k->ramp, x, y);
  return a0 > a1 ? a0 : a1;
}

/* An inset's axis point: the texel (fractional, maybe outside the inset) where its eye's straight
 * ahead falls, its window's centre less the window's offset (the gaze's, agent.insetgaze). Every
 * direction maps from there at the insets' density. Both insets' values loaded, then chosen (a
 * choice of address per texel is a buffer load). */
static inline __attribute__((always_inline)) void inset_axis(K *k, int right, float *ax, float *ay) {
  float qx = UNIF(k->ppt[2]), qy = UNIF(k->ppt[3]);
  float t0x = UNIF(k->ictan[0]), t0y = UNIF(k->ictan[1]), t1x = UNIF(k->ictan[2]), t1y = UNIF(k->ictan[3]);
  float c0x = (float)UNI((k->rect[0].x + k->rect[0].z) / 2), c0y = (float)UNI((k->rect[0].y + k->rect[0].w) / 2);
  float c1x = (float)UNI((k->rect[1].x + k->rect[1].z) / 2), c1y = (float)UNI((k->rect[1].y + k->rect[1].w) / 2);
  *ax = right ? c1x - t1x * qx : c0x - t0x * qx;
  *ay = right ? c1y + t1y * qy : c0y + t0y * qy;
}
/* the eyes' rows: the higher inset's top (the insets below the eyes, side by side or one above
 * the other) */
static inline __attribute__((always_inline)) int eye_rows_of(K *k) {
  int a = UNI(k->rect[0].y), b = UNI(k->rect[1].y);
  return a < b ? a : b;
}

/* stage 2: the debug menu (agent/menu.c) over each eye, thread (x, y) of 2mw x mh: its picture's
 * texel x (left eye) or x - mw (right), blended by its alpha; the texel's own alpha kept */
static inline __attribute__((always_inline)) void menu_blit(K *k, int x, int y) {
  int mw = UNI(k->mw), mh = UNI(k->mh);
  if (x >= 2 * mw || y >= mh)
    return;
  int e = x >= mw, mx = e ? x - mw : x;
  int l0x = UNI(k->mat[0][0]), l0y = UNI(k->mat[0][1]), l1x = UNI(k->mat[1][0]), l1y = UNI(k->mat[1][1]);
  int dx = (e ? l1x : l0x) + mx, dy = (e ? l1y : l0y) + y;
  if (dx < 0 || dy < 0 || dx >= k->w || dy >= k->h)
    return;
  float4 m = LOAD(k->mtex, mx, y), c = LOAD(k->tex, dx, dy);
  float a = m.w;
  c.x = c.x + (m.x - c.x) * a, c.y = c.y + (m.y - c.y) * a, c.z = c.z + (m.z - c.z) * a;
  STORE_ALL(c, k->tex, dx, dy);
}

void inset_main(K *k, int gx, int gy, int tx, int ty) {
  int x = gx * 8 + tx, y = gy * 8 + ty;
  if (k->stage == 2) {
    menu_blit(k, x, y);
    return;
  }
  y += k->oy;
  if (x >= k->w || y >= k->h)
    return;
  float a = inset_alpha(k, x, y);
  int4 r0 = k->rect[0], r1 = k->rect[1];
  int in0 = x >= r0.x && x < r0.z && y >= r0.y && y < r0.w;
  int in1 = x >= r1.x && x < r1.z && y >= r1.y && y < r1.w;
  int right = in1; /* which inset, by its rect */
  if (k->menuon && a > 0.0f) { /* the debug menu's place: the periphery's, with the menu on it */
    float iax, iay;
    inset_axis(k, right, &iax, &iay);
    float qx = k->ppt[2], qy = k->ppt[3];
    float tx = ((float)x - iax) / qx, ty = -((float)y - iay) / qy;
    /* (each eye's copy is moved in for its stereo: the give-way place with it, or a strip of the
     * menu's inner side stayed under the inset: worn 2026-11-07, the wide inset over it) */
    float dx = k->menu_dx * (1.0f - 2.0f * (float)right);
    float m0 = k->menu[0] + dx, m1 = k->menu[1] + dx, m2 = k->menu[2], m3 = k->menu[3];
    if (tx >= m0 && tx <= m1 && ty >= m2 && ty <= m3)
      a = 0.0f;
  }
  float4 c = k->debug == 1 && a == 0.0f ? (float4){1.0f, 0.0f, 1.0f, 0.0f} : LOAD(k->tex, x, y);
  if (k->debug == 2 && a > 0.0f) /* the insets green, to find them in Hmd2's output */
    c.x = 0.0f, c.z = 0.0f;
  if (k->debug == 3) {
    /* both eyes' values loaded and blended by arithmetic: a choice between two constants by a
     * per-texel condition became a per-texel load from memory (buffer_load, which the build
     * refuses) */
    int e = x >= k->w / 2, f = in1;
    int a0x = UNI(k->axis[0][0]), a0y = UNI(k->axis[0][1]);
    int a1x = UNI(k->axis[1][0]), a1y = UNI(k->axis[1][1]);
    int ax = e ? a1x : a0x, ay = e ? a1y : a0y;
    float fix, fiy;
    inset_axis(k, f, &fix, &fiy);
    int ix = (int)fix, iy = (int)fiy;
    if (y < eye_rows_of(k) && (abs_i(y - ay) <= 4 || abs_i(x - ax) <= 4))
      c = (float4){1.0f, 0.0f, 1.0f, 0.0f}; /* the periphery's cross */
    else if (a > 0.0f && (abs_i(y - iy) <= 8 || abs_i(x - ix) <= 8))
      c.x = 0.0f, c.y = 1.0f, c.z = 0.0f; /* the inset's */
  }
  if (k->debug == 4) {
    int e = x >= k->w / 2, f = in1;
    int a0x = UNI(k->axis[0][0]), a0y = UNI(k->axis[0][1]);
    int a1x = UNI(k->axis[1][0]), a1y = UNI(k->axis[1][1]);
    int ax = e ? a1x : a0x, ay = e ? a1y : a0y;
    float iax, iay;
    inset_axis(k, f, &iax, &iay);
    float px = k->ppt[0], py = k->ppt[1], qx = k->ppt[2], qy = k->ppt[3];
    if (y < eye_rows_of(k)) {
      float dx = (float)(x - ax), dy = (float)(y - ay);
      if (absf(dx) <= 0.65f * px && absf(dy) <= 0.65f * py &&
          (on_grid(dx, px, 1.5f) || on_grid(dy, py, 1.5f)))
        c = (float4){1.0f, 0.0f, 1.0f, 0.0f};
    } else if (a > 0.0f) {
      float dx = (float)x - iax, dy = (float)y - iay;
      if (on_grid(dx, qx, 3.0f) || on_grid(dy, qy, 3.0f))
        c.x = 0.0f, c.y = 1.0f, c.z = 0.0f;
    }
  }
  if (k->gazedot) { /* agent.gazedot: a yellow ring where the eye tracking says the eye looks */
    /* every value loaded first, chosen after (a choice of address per texel is a buffer load) */
    float gx = k->gaze[0], gy = k->gaze[1];
    float pp0 = k->ppt[0], pp1 = k->ppt[1], pi0 = k->ppt[2], pi1 = k->ppt[3];
    float iax, iay;
    inset_axis(k, right, &iax, &iay);
    int a0x = UNI(k->axis[0][0]), a0y = UNI(k->axis[0][1]);
    int a1x = UNI(k->axis[1][0]), a1y = UNI(k->axis[1][1]);
    int e = x >= k->w / 2, ins = in0 || in1;
    float bx = ins ? iax : (float)(e ? a1x : a0x);
    float by = ins ? iay : (float)(e ? a1y : a0y);
    float sx = ins ? pi0 : pp0, sy = ins ? pi1 : pp1;
    float cx = bx + gx * sx, cy = by - gy * sy, rad = 0.02f * sx;
    float dx = (float)x - cx, dy = (float)y - cy, d2 = dx * dx + dy * dy;
    float r0 = rad * 0.6f;
    if (d2 <= rad * rad && d2 >= r0 * r0)
      c.x = 1.0f, c.y = 1.0f, c.z = 0.0f;
  }
  if (k->debug == 5 && a > 0.0f) { /* each inset its own tint: the left green, the right red */
    if (in1)
      c.x = 1.0f, c.y = 0.15f, c.z = 0.15f;
    else
      c.x = 0.15f, c.y = 1.0f, c.z = 0.15f;
  }
  c.w = a;
  STORE_ALL(c, k->tex, x, y);
}
