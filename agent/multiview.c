/* Multi-view, inset mode (docs/MULTIVIEW.md, "The build"): the agent's patches to the game's
 * code. The payload calls agent_boot with the game stopped at its entry point, passing N
 * from a cvars.txt line agent.multiview=N; the table returned here is checked entry by entry
 * against the game's stock bytes and written only if all match (payload apply_agent_patches).
 * The addresses below are Hikari 1.00's; mv_at gives Chisato's and Allison's (the table after
 * add()). A build whose bytes differ: nothing is written, and the payload sets g_multiview_rows
 * back to 0, the stock game.
 *
 * Step 2, the layout without insets: N is the number of rows added below the eyes for the insets
 * (none drawn there yet). The Morpheus plugin's
 *   - CalculateRenderTargetSize (0x2f8fb20) makes the texture N rows taller: its store of the
 *     height (0x2f8fc1e) goes through a few bytes in the second AdjustViewRect's room, which
 *     adds N and goes back;
 *   - AdjustViewRect (0x2f8ef50; 0x2f8ef70 is the same code, now a jump to it) halves the width
 *     and moves the right eye over as it did, and holds the eye's height to the eyes' own (the
 *     screen percentage's 1080 rows), so the eyes keep the top of the texture. (The first try took
 *     N rows off the height it was given, 2026-11-06 21:18: that was the eyes' 1152 already, not
 *     the texture's 1696, and the eyes came out 608 rows high.)
 * resolution.txt then gives the periphery's eye width (r.ScreenPercentage, as now).
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include "vr2.h"

#define AGENT_PATCH_MAGIC 0x48435441504c5653ull /* "SVLPATCH" */
#define AGENT_PATCH_MAX 26 /* (payload/main.c's too: raise both together; 24 at 60, 26 with the
                            * native rate's two stores: an older payload's 24 still takes 60's) */
struct a_patch {
  u32 at;
  u8 len, pad[3];
  u8 was[32], now[32];
};
struct a_patches {
  u64 magic;
  u32 n, pad;
  struct a_patch p[AGENT_PATCH_MAX];
};
_Static_assert(sizeof(struct a_patch) == 72, "agent patch ABI (payload/main.c)");

u32 g_multiview_rows; /* agent.multiview=N as the boot was given it (0: off) */
volatile u32 g_opt_insetdebug; /* agent.insetdebug=1: the alpha pass paints magenta outside the insets (inset.c), live */
volatile u32 g_opt_insetstate; /* agent.insetstate=1: the insets with view states of their own, live;
                                * on in inset mode: the engine gives a view the post-process
                                * materials (the game's fades, its dimming) only with one
                                * (UMaterialInterface::OverrideBlendableSettings: !View.State,
                                * none), 2026-11-07 logs/mv98-mv99. Their TAA with
                                * agent.insetaam=2 (inset mode's default since docs/MULTIVIEW.md 30;
                                * it ghosted worn in mv74, before 26's fixes) */
volatile u32 g_opt_insetaam = 1; /* agent.insetaam: the inset views' own anti-aliasing method, 0 none,
                                  * 1 the engine's FXAA, 2 its TAA as the eyes' (inset mode's
                                  * default, multiview_boot; each inset then gets a view state of
                                  * its own, as with agent.insetstate=1), live */
volatile u32 g_opt_gazefilter = 1; /* agent.gazefilter: 0 the gaze held (agent.gazehold), 1 smoothed
                                    * by its speed (inset_gaze), live */
volatile u32 g_opt_gazemincut = 10; /* agent.gazemincut=N: the smoothing's cut-off at rest, N/100 Hz */
volatile u32 g_opt_gazebeta = 1;    /* agent.gazebeta=N: its cut-off's rise, N Hz a tangent a second */
volatile u32 g_opt_probe; /* agent.probe=1: the eyes' and insets' brightness in the log, 2 also the
                           * insets' edge bands (mirror.c) */
volatile u32 g_opt_gazedot; /* agent.gazedot=1: a ring at the gaze (inset.c), live; needs agent.gaze=1 */
volatile u32 g_opt_vignette; /* agent.vignette: the engine's lens vignette, 0 off in every view (by
                              * default), 1 on the eyes only, 2 every view (the insets' own too,
                              * which darkened them toward their edges): mv_cs_wrap, live */
volatile u32 g_opt_gazehold = 50; /* agent.gazehold=N: the window held while the gaze stays within
                                   * N/1000 of tangent of its centre (inset_gaze), live */
volatile u32 g_opt_insetblink; /* agent.insetblink=N: the insets' layer off every other N ms
                                * (vr2.c), live */
volatile u32 g_opt_insetwin; /* agent.insetwin: the insets' layer with Hmd2's window test (vr2.c):
                              * 1 registered only, 2 on it with the windows off view, 3 in use;
                              * registration at the launch, 2/3 live */
volatile u32 g_opt_insetonly; /* agent.insetonly=1|2: only the left / right eye's inset layer (vr2.c), live */
volatile u32 g_gaze_returns;
volatile u32 g_opt_insetgaze; /* agent.insetgaze: 1 the insets' windows follow the gaze (needs
                               * agent.gaze=1), 2 a made-up gaze moving between the centre and
                               * 0.3/0.2 of tangent left, right, up and down every 2 s, 3-6 held
                               * at left, right, up, down (captures), 7 a steady circle of 0.15
                               * of tangent every 4 s (the window moving, head held), 9 a made-up
                               * gaze (fixations, jumps and the tracker's scatter) through the
                               * same smoothing as 1, 10 the same about one fixation straight
                               * ahead (no jumps of its own), 11 a look away and back every 3 s (2.5 s straight ahead,
                               * 0.5 s 0.6 of tangent up); live */
volatile u32 g_opt_insetppt = 1200; /* agent.insetppt=D (inset_window below), live */
volatile u32 g_opt_insetsize; /* agent.insetsize=S: each inset SxS (0: the rows agent.multiview adds),
                               * read as the insets are made */
volatile u32 g_opt_insetwidth; /* agent.insetwidth=W: each inset W wide (0: as tall), at most an
                                * eye's width; live */
volatile u32 g_opt_insetpos = 1; /* agent.insetpos=0: the insets drawn from the head's centre (as
                                  * before; mv_uvm), live; 1 without the line */
volatile u32 g_opt_insetea = 1; /* agent.insetea=0: the insets' own eye adaptation passes run
                                 * (mv_ea_skip), live; 1 without the line: left out */
volatile u32 g_opt_insettlv = 1; /* agent.insettlv=0: each inset its own translucency lighting
                                  * volume (mv_tlv), live; 1 without the line: the left eye's */
volatile u32 g_opt_inseteye = 1; /* agent.inseteye=0: the insets without the eyes' exposure (live; 1
                                  * without the line) */
volatile u32 g_opt_insetsnap = 2; /* agent.insetsnap: the window moved by whole steps of 1 or 2 inset pixels (2 by
                                   * default), 0 by fractions too (mv_opt_wrap), live; 2 whatever it says with
                                   * agent.taa=11-15 (their velocity fix needs it) */
volatile u32 g_opt_insetviews; /* agent.insetviews=1: Draw makes the two inset views (live; read
                                * by mv_count_stub at each frame's view count) */

static struct a_patches g_patches;

static void put32(u8 *p, u32 v) {
  p[0] = (u8)v, p[1] = (u8)(v >> 8), p[2] = (u8)(v >> 16), p[3] = (u8)(v >> 24);
}
static u32 g_patches_over; /* more patches than the table holds: none given (multiview_boot) */
static void add(u32 at, const u8 *was, const u8 *now, u32 len) {
  if (g_patches.n >= AGENT_PATCH_MAX || len > sizeof g_patches.p[0].was) {
    g_patches_over = 1;
    return;
  }
  struct a_patch *e = &g_patches.p[g_patches.n++];
  e->at = at, e->len = (u8)len;
  memcpy(e->was, was, len);
  memcpy(e->now, now, len);
}

/* ---- The builds' addresses ----
 * The code below names Hikari 1.00's addresses (CUSA07434); mv_at gives the game's own. Chisato's
 * (CUSA11682), Allison's (CUSA11674) and Hikari 1.02's (CUSA07434's update, 2026-11-08: Chisato's
 * and Allison's newer engine, its code often at Allison's addresses; the data 0x10000 below 1.00's)
 * were found from Hikari's eboot with tools/ebootmap
 * (docs/OTHER-GAMES.md, "Inset mode"): each function or code site by its code found once, the short
 * ones also by every call that reaches them, the statics and the render commands' execute functions
 * by the instructions that reference them, all agreeing. The patch sites' stock bytes are Hikari's
 * in all three (Draw's call of CalcSceneView aside, made from the addresses), and the payload checks
 * each against the game before any is written. */
static const u32 k_at[][4] = {
    /* multiview.c: Hikari, Chisato, Allison, Hikari 1.02 */
    {0x2f8ef50, 0x2ffe020, 0x2ffdfe0, 0x2f89f70}, /* AdjustViewRect */
    {0x2f8ef70, 0x2ffe040, 0x2ffe000, 0x2f89f90}, /* its copy */
    {0x2f8fc1e, 0x2ffecee, 0x2ffecae, 0x2f8ac3e}, /* CalculateRenderTargetSize's store */
    {0x2f8fc25, 0x2ffecf5, 0x2ffecb5, 0x2f8ac45}, /* its pop rbx after */
    {0x15d44bb, 0x15d410b, 0x15d40ab, 0x15d40ab}, /* Draw: the view count */
    {0x15d452a, 0x15d417a, 0x15d411a, 0x15d411a}, /* Draw: call CalcSceneView */
    {0x16de89c, 0x16de4fc, 0x16de49c, 0x16de48c}, /* CalcSceneView: call CalcSceneViewInitOptions */
    {0x16de770, 0x16de3d0, 0x16de370, 0x16de360}, /* ULocalPlayer::CalcSceneView */
    {0x16dd810, 0x16dd470, 0x16dd410, 0x16dd400}, /* ULocalPlayer::CalcSceneViewInitOptions */
    {0xcb1a3d, 0xcb16dd, 0xcb167d, 0xcb167d},    /* the tonemapper's clear */
    {0xcde69e, 0xcde33e, 0xcde2de, 0xcde2de},    /* the upscale pass's clear */
    {0x15d3a82, 0x15d36d2, 0x15d3672, 0x15d3672}, /* the pads */
    {0x15d3912, 0x15d3562, 0x15d3502, 0x15d3502},
    {0x16df982, 0x16df5e2, 0x16df582, 0x16df572},
    {0x17a9eb0, 0x17a9b10, 0x17a9ab0, 0x17a9aa0}, /* FSceneViewStateReference::Allocate */
    {0x452be38, 0x45c7e38, 0x45c7e38, 0x451fe38}, /* its vtable (+0x10) */
    {0xb4cd90, 0xb4cf90, 0xb4cf30, 0xb4cf30},    /* FViewInfo::GetEyeAdaptation */
    {0xb4cda0, 0xb4cfa0, 0xb4cf40, 0xb4cf40},    /* its body */
    {0xb4d100, 0xb4d300, 0xb4d2a0, 0xb4d2a0},    /* FViewInfo::HasValidEyeAdaptation */
    {0xb4cd10, 0xb4cf10, 0xb4ceb0, 0xb4ceb0},    /* FViewInfo::GetEffectiveViewState */
    {0xc56931, 0xc56b41, 0xc56ae1, 0xc56ae1},    /* the histogram pass's return */
    {0x17abc20, 0x17ab880, 0x17ab820, 0x17ab810}, /* FSceneView::UpdateViewMatrix */
    {0x17abc32, 0x17ab892, 0x17ab832, 0x17ab822},
    {0x16e1290, 0x16e0ef0, 0x16e0e90, 0x16e0e80}, /* ULocalPlayer::AddReferencedObjects */
    {0x16e12a4, 0x16e0f04, 0x16e0ea4, 0x16e0e94},
    {0xbc0bf0, 0xbc0df0, 0xbc0d90, 0xbc0d90},    /* FViewInfo::CalcTranslucencyLightingVolumeBounds */
    {0xbc0c04, 0xbc0e04, 0xbc0da4, 0xbc0da4},
    {0xc62741, 0xc62951, 0xc628f1, 0xc628f1},    /* the eye adaptation gate */
    {0xc62989, 0xc62b99, 0xc62b39, 0xc62b39},
    {0xc6274f, 0xc6295f, 0xc628ff, 0xc628ff},
    {0xb540e4, 0xb542e4, 0xb54284, 0xb54284},    /* PreVisibilityFrameSetup: call DoOcclusionQueries */
    {0xb4d530, 0xb4d730, 0xb4d6d0, 0xb4d6d0},    /* DoOcclusionQueries */
    {0xb4d212, 0xb4d412, 0xb4d3b2, 0xb4d3b2},    /* a pad */
    {0x17a7770, 0x17a73d0, 0x17a7370, 0x17a7360}, /* ComputeLODForMeshes */
    {0x17a7781, 0x17a73e1, 0x17a7381, 0x17a7371},
    /* insetpp.c */
    {0xd4da30, 0xd4d670, 0xd4d610, 0xd4d610}, /* FRCPassPostProcessScreenSpaceReflections::Process */
    {0xd4da44, 0xd4d684, 0xd4d624, 0xd4d624},
    {0xcb1620, 0xcb12c0, 0xcb1260, 0xcb1260}, /* FRCPassPostProcessTonemap::Process */
    {0xcb1634, 0xcb12d4, 0xcb1274, 0xcb1274},
    {0xcaebb0, 0xcae850, 0xcae7f0, 0xcae7f0}, /* the film tonemapper's */
    {0xcaebc4, 0xcae864, 0xcae804, 0xcae804},
    {0xbf3b40, 0xbf3d40, 0xbf3ce0, 0xbf3ce0}, /* FRCPassPostProcessAmbientOcclusion::Process */
    {0xbf3b51, 0xbf3d51, 0xbf3cf1, 0xbf3cf1},
    {0x22b980, 0x22bb80, 0x22bb20, 0x22bb20},    /* FMemStackBase::AllocateNewChunk */
    {0x18e7910, 0x18e7570, 0x18e7510, 0x18e7500}, /* SetGlobalBoundShaderState */
    {0xd2acc0, 0xd2a960, 0xd2a900, 0xd2a900},    /* DrawRectangle */
    {0x8fa350, 0x8fa4f0, 0x8fa490, 0x8fa490},    /* SetRenderTarget */
    {0xd22600, 0xd222a0, 0xd22240, 0xd22240},    /* FRenderTargetPool::FindFreeElement */
    {0xd2d6b0, 0xd2d350, 0xd2d2f0, 0xd2d2f0},    /* FSceneRenderTargets::Get */
    {0x882f00, 0x8830a0, 0x883040, 0x883040},    /* FShaderResource's RHI shader */
    {0x92b8a0, 0x92baa0, 0x92ba40, 0x92ba40},    /* the static states' GetRHI */
    {0x8f6ed0, 0x8f7070, 0x8f7010, 0x8f7010},
    {0x8f7270, 0x8f7410, 0x8f73b0, 0x8f73b0},
    {0x8e0ee0, 0x8e1080, 0x8e1020, 0x8e1020},
    {0x9075c0, 0x907760, 0x907700, 0x907700},
    {0x4a04bc0, 0x4aa8d30, 0x4aa8d30, 0x49f4bf0}, /* GRenderTargetPool */
    {0x4a050c0, 0x4aa9230, 0x4aa9230, 0x49f50f0}, /* GFilterVertexDeclaration */
    {0x466d854, 0x4711854, 0x4711854, 0x465d854}, /* the command list's state filtering */
    {0x46c0440, 0x47645c0, 0x47645c0, 0x46b0440}, /* FLinearColor::White */
    {0x4a05130, 0x4aa92a0, 0x4aa92a0, 0x49f5160}, /* FDrawRectangleParameters' struct */
    {0x4af8a48, 0x4b9cba8, 0x4b9cba8, 0x4ae8a68}, /* FScreenVS::StaticType */
    {0x4af88e8, 0x4b9ca48, 0x4b9ca48, 0x4ae8908}, /* FScreenPS::StaticType */
    {0x8ae110, 0x8ae2b0, 0x8ae250, 0x8ae250},    /* the render commands' execute functions */
    {0x8ae5e0, 0x8ae780, 0x8ae720, 0x8ae720},
    {0x8ae620, 0x8ae7c0, 0x8ae760, 0x8ae760},
    {0x8ae600, 0x8ae7a0, 0x8ae740, 0x8ae740},
    {0x256b0, 0x256b0, 0x256b0, 0x256b0},
    {0x25960, 0x25960, 0x25960, 0x25960},
    {0x8fbc30, 0x8fbdd0, 0x8fbd70, 0x8fbd70},
};
static u32 g_at_missing; /* an address the table doesn't have (multiview_boot: no inset mode) */
static u32 g_mv_fps; /* the native rate inset mode asked at the boot (90, 120), 0 for 60 */
static u32 g_mv_stock_pct, g_mv_pct_set; /* the header's eye size before inset mode set its own */
/* the eye size the stock game was given (resolution.txt's; its bits, 0 for the game's own) when
 * inset mode set its own and then didn't run (the payload refused its table: an older payload,
 * another build's bytes): 1 and *bits; else 0 (hooks.c, as the eye size is written) */
int multiview_stock_pct(u32 *bits) {
  if (!g_mv_pct_set || g_multiview_rows)
    return 0;
  *bits = g_mv_stock_pct;
  return 1;
}
static void build_fps(u32 fps); /* the frame-rate stores (below) */
/* the game's address of Hikari's h (0: not known for this build) */
u32 mv_at(u32 h) {
  u32 b = astro_hdr.build;
  if (b == 1)
    return h;
  for (u32 i = 0; b >= 2 && b <= BUILD_N && i < sizeof k_at / sizeof k_at[0]; i++)
    if (k_at[i][0] == h)
      return k_at[i][b - 1];
  g_at_missing = h;
  return 0;
}

/* Hikari 1.00 (CUSA07434), from its eboot (the game's own through mv_at) */
#define AVR_A mv_at(0x2f8ef50u)  /* AdjustViewRect */
#define AVR_B mv_at(0x2f8ef70u)  /* the same code (the stereo interface's) */
#define CRT_STORE mv_at(0x2f8fc1eu) /* CalculateRenderTargetSize: cvttss2si eax, xmm0; mov [r14], eax */
#define CRT_BACK mv_at(0x2f8fc25u)  /* its pop rbx after */
static const u8 k_avr[32] = {0x55, 0x48, 0x89, 0xe5, 0x41, 0x8b, 0x00, 0xd1, 0xe8, 0x83, 0xfe,
                             0x02, 0x41, 0x89, 0x00, 0x75, 0x02, 0x01, 0x02, 0x5d, 0xc3, 0x66,
                             0x66, 0x2e, 0x0f, 0x1f, 0x84, 0x00, 0x00, 0x00, 0x00, 0x00};
static const u8 k_crt_store[7] = {0xc5, 0xfa, 0x2c, 0xc0, 0x41, 0x89, 0x06};

static void build_step2(u32 rows, u32 eye_h) {
  u8 a[32], b[19], c[7];
  /* AdjustViewRect(this, pass esi, &X rdx, &Y rcx, &SizeX r8, &SizeY r9) */
  static const u8 a0[13] = {0x41, 0x8b, 0x00,  /* mov eax, [r8] */
                            0xd1, 0xe8,        /* shr eax, 1 */
                            0x41, 0x89, 0x00,  /* mov [r8], eax */
                            0x83, 0xfe, 0x02,  /* cmp esi, 2 */
                            0x75, 0x02};       /* jne +2 */
  memcpy(a, a0, 13);
  a[13] = 0x01, a[14] = 0x02;                  /* add [rdx], eax */
  a[15] = 0x41, a[16] = 0x81, a[17] = 0x39;    /* cmp dword [r9], eye_h */
  put32(a + 18, eye_h);
  a[22] = 0x76, a[23] = 0x07;                  /* jbe +7 (not over: as it is) */
  a[24] = 0x41, a[25] = 0xc7, a[26] = 0x01;    /* mov dword [r9], eye_h */
  put32(a + 27, eye_h);
  a[31] = 0xc3;                                /* ret */
  add(AVR_A, k_avr, a, 32);
  /* the second copy: a jump to the first, then the height's detour in its room */
  u32 cave = AVR_B + 2;
  b[0] = 0xeb, b[1] = (u8)(AVR_A - (AVR_B + 2));          /* jmp AVR_A */
  memcpy(b + 2, k_crt_store, 4);                          /* cvttss2si eax, xmm0 (as it was) */
  b[6] = 0x05;                                            /* add eax, rows */
  put32(b + 7, rows);
  b[11] = 0x41, b[12] = 0x89, b[13] = 0x06;               /* mov [r14], eax */
  b[14] = 0xe9;                                           /* jmp CRT_BACK */
  put32(b + 15, CRT_BACK - (cave + 17));
  add(AVR_B, k_avr, b, 19);
  c[0] = 0xe9;                                            /* jmp cave */
  put32(c + 1, cave - (CRT_STORE + 5));
  c[5] = 0x90, c[6] = 0x90;
  add(CRT_STORE, k_crt_store, c, 7);
}

/* ---- Step 3: the two inset views (side notes multiview-engine.md, sections 1-5) ----
 * UGameViewportClient::Draw (0x15d3a90) makes stereo + 1 views, pass 1 then 2. Three calls in the
 * game's code go to the agent, each through a 14-byte jump (jmp [rip]) in dead padding after a
 * function's ret (nothing branches there; the agent is too far for a rel32):
 *   - the view count (0x15d44bb, movzbl al,eax; inc eax): mv_count_stub, 4 when stereo and
 *     agent.insetviews=1 (else 2, or 1 without stereo, as it was);
 *   - Draw's call of ULocalPlayer::CalcSceneView (0x15d452a): mv_cs_wrap, which turns the third
 *     and fourth views (pass 2 again) into pass 0, the plain per-view path, so the instanced pair
 *     stays Views[0] and Views[1] as the renderer needs;
 *   - CalcSceneView's call of CalcSceneViewInitOptions (0x16de89c): mv_opt_wrap, which keeps each
 *     eye's options and gives an inset its eye's origin and rotation, that eye's projection cropped
 *     to a window straight ahead, its rect below that eye in the rows step 2 added, and no view
 *     state (no TAA history or eye adaptation of its own yet: a pass 0 view would otherwise share
 *     the left eye's).
 * The tonemapper and the upscale pass clear the whole target for pass <= 1 when they write the
 * final output (0xcb1a3d, 0xcde69e: ja -> jne), which would wipe the periphery as an inset (pass 0)
 * is drawn after it: only pass 1 clears now. Draw takes a null view from CalcSceneView as no view
 * (0x15d4537), which is how the agent leaves an inset out. */
#define DRAW_COUNT mv_at(0x15d44bbu) /* Draw: movzbl al,eax; inc eax */
#define DRAW_CSV mv_at(0x15d452au)   /* Draw: call CalcSceneView */
#define CSV_OPTS mv_at(0x16de89cu)   /* CalcSceneView: call CalcSceneViewInitOptions */
#define VIEW_VIGNETTE 0xa48u  /* FSceneView: FinalPostProcessSettings.VignetteIntensity */
#define VIEW_AAM 0xb0cu       /* FSceneView: FinalPostProcessSettings.AntiAliasingMethod (a byte) */
/* FSceneView's FinalPostProcessSettings at +0x7a0 (its fields' offsets from the struct's
 * reflection: the UFloatProperty registrations name each with its offset, 0x1ed4000..0x1ed7000:
 * vignette 0x2a8 and AntiAliasingMethod 0x36c land on +0xa48 and +0xb0c, found before) */
#define VIEW_PP 0x7a0u
#define PP_BLOOM_INTENSITY 0xe8u
#define PP_AO_INTENSITY 0x2b4u
#define CSV_FN mv_at(0x16de770u)     /* ULocalPlayer::CalcSceneView */
#define OPTS_FN mv_at(0x16dd810u)    /* ULocalPlayer::CalcSceneViewInitOptions */
#define TONEMAP_JA mv_at(0xcb1a3du)  /* cmpl $1, 0x420(%rbx); ja (no clear for pass > 1) */
#define UPSCALE_JA mv_at(0xcde69eu)  /* cmpl $1, 0x420(%r14); ja */
#define PAD_COUNT mv_at(0x15d3a82u)  /* 14 bytes of nopw after a ret, before Draw */
#define PAD_CSV mv_at(0x15d3912u)    /* the same, before the function before it */
#define PAD_OPTS mv_at(0x16df982u)   /* the same, after CalcSceneView's neighbour */
#define VSR_ALLOCATE mv_at(0x17a9eb0u) /* FSceneViewStateReference::Allocate(this) */
#define VSR_VTABLE mv_at(0x452be38u)   /* FSceneViewStateReference's vtable (0x452be28 + 0x10) */
static const u8 k_pad[14] = {0x66, 0x66, 0x66, 0x66, 0x66, 0x2e, 0x0f,
                             0x1f, 0x84, 0x00, 0x00, 0x00, 0x00, 0x00};
static const u8 k_count[5] = {0x0f, 0xb6, 0xc0, 0xff, 0xc0};
static const u8 k_ja[2] = {0x0f, 0x87}, k_jne[2] = {0x0f, 0x85};

/* ---- The insets' exposure: the eyes' eye adaptation ----
 * Without a view state an inset gets no exposure of its own: the renderer's shaders that apply it
 * (the tonemapper, the bloom setup and the rest: each asks HasValidEyeAdaptation, then
 * GetEyeAdaptation, else takes a white texture, an exposure of 1) drew it about 5 times darker
 * than the eyes. With one (agent.insetstate=1) it would adapt to its own window straight ahead,
 * not to the eye around it. The engine already gives the right eye the left eye's
 * (FViewInfo::GetEffectiveViewState, 0xb4cd10: pass 2 takes the previous view's state when that
 * view is pass 1). The agent's two functions stand in for those two getters and do the same for an
 * inset: Views[2] and Views[3], pass 0, take Views[0]'s (the left eye's), state or not. Only the
 * readers: the eye adaptation passes are added only for a view with a state (0xc6274f: View.State
 * != 0), and they write the view's own state:
 *   - the basic pass (0xc57840) swaps, gets and reads with the getters' bodies (0xb4d070,
 *     0xb4cda0, 0xb4cf00), not through these;
 *   - the histogram pass (0xc56910) gets its render target through GetEyeAdaptation's thunk
 *     (0xc5692c; then 0x8fa350 sets it as the target): from there (return address 0xc56931) the
 *     view's own, so an inset's adaptation never writes the eyes'.
 * Each getter's first 14 bytes become the jump to the agent's:
 *   - GetEyeAdaptation (0xb4cd90: a thunk to 0xb4cda0, which the agent calls);
 *   - HasValidEyeAdaptation (0xb4d100: the effective state's valid flag, +0x628).
 * FViewInfo: +0 Family (Views: data, count), +8 State, +0x420 StereoPass, +0xd10 ViewState. */
#define EA_GET mv_at(0xb4cd90u)      /* FViewInfo::GetEyeAdaptation(RHICmdList): push rbp; ...; jmp 0xb4cda0 */
#define EA_GET_BODY mv_at(0xb4cda0u) /* its body */
#define EA_VALID mv_at(0xb4d100u)    /* FViewInfo::HasValidEyeAdaptation() */
#define EA_EFFECTIVE mv_at(0xb4cd10u) /* FViewInfo::GetEffectiveViewState() */
#define EA_WRITER_RET mv_at(0xc56931u) /* the histogram eye adaptation pass, after its call for its target */
static const u8 k_ea_get[14] = {0x55, 0x48, 0x89, 0xe5, 0x5d, 0xe9, 0x06, 0x00, 0x00, 0x00,
                                0x66, 0x0f, 0x1f, 0x44};
static const u8 k_ea_valid[14] = {0x55, 0x48, 0x89, 0xe5, 0x48, 0x8b, 0x87, 0x10, 0x0d, 0x00,
                                  0x00, 0x83, 0xbf, 0x20};
/* ---- Each inset from its eye's place ----
 * FSceneView::UpdateViewMatrix (0x17abc20) makes a view's origin again from its location: the
 * stereo device's eye offset is added for pass 1 and 2 only. The engine runs it after Draw has
 * made the views (the plugin's head update, 0x2f914b0), so an inset (pass 0) was drawn from the
 * head's centre: the two insets the same picture (no stereo: the left eye's inset's near things
 * left of the periphery's, the right eye's right, crossed eyes; 2026-11-07, the insets' best
 * match with each other at no shift, the eyes' at 12-18 texels). The agent's function stands in
 * for it: an inset (Views[2], Views[3]) is its eye's pass (1, 2) during the call. Its first 14
 * bytes become the jump; the engine's function runs from the agent's copy of its first 18 (push
 * rbp; mov rbp,rsp; push r15/r14/r12/rbx; sub rsp,0x120), then on at 0x17abc32. */
#define UVM mv_at(0x17abc20u)      /* FSceneView::UpdateViewMatrix() */
#define UVM_REST mv_at(0x17abc32u) /* after its first 18 bytes */
static const u8 k_uvm[14] = {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56,
                             0x41, 0x54, 0x53, 0x48, 0x81, 0xec};
u64 g_uvm_rest; /* game base + UVM_REST (the asm below jumps there) */
static void (*o_uvm)(u64);
static volatile u32 g_uvm_logged;
#ifndef MV_HOST_TEST
void mv_uvm_orig(u64 view);
__asm__(".text\n.p2align 4\n.type mv_uvm_orig,@function\nmv_uvm_orig:\n"
        "  pushq %rbp\n"
        "  movq %rsp, %rbp\n"
        "  pushq %r15\n"
        "  pushq %r14\n"
        "  pushq %r12\n"
        "  pushq %rbx\n"
        "  subq $0x120, %rsp\n"
        "  jmpq *g_uvm_rest(%rip)\n"
        ".size mv_uvm_orig, .-mv_uvm_orig\n");
#endif
/* an inset's eye (1 left, 2 right), or 0 for any other view */
static u32 inset_eye(u64 v) {
  if (!g_opt_insetpos || !g_opt_insetviews || !v || *(const u32 *)(v + 0x420) != 0)
    return 0;
  u64 fam = *(const u64 *)v;
  if (!fam)
    return 0;
  const u64 *views = *(const u64 *const *)fam;
  s32 n = *(const s32 *)(fam + 8);
  if (!views || n < 3 || n > 4 || !views[0] || !views[1] ||
      *(const u32 *)(views[0] + 0x420) != 1 || *(const u32 *)(views[1] + 0x420) != 2)
    return 0;
  return v == views[2] ? 1 : n == 4 && v == views[3] ? 2 : 0;
}
void mv_uvm(u64 view) {
  u32 eye = inset_eye(view);
  if (!eye) {
    o_uvm(view);
    return;
  }
  u32 *pass = (u32 *)(view + 0x420);
  *pass = eye;
  o_uvm(view);
  *pass = 0;
  if (!g_uvm_logged) {
    g_uvm_logged = 1;
    LOG("multiview: the insets' view origins from their eyes' places (inset %u's first)", eye);
  }
}

/* ---- The insets' view states kept from the garbage collector ----
 * With agent.insetstate=1 each inset has a view state of its own (TAA history, jitter, its own
 * occlusion), allocated as ULocalPlayer allocates its eyes' (FSceneViewStateReference::Allocate,
 * 0x17a9eb0). A view state with post-process blendables makes material instances for them
 * (FSceneViewState::GetReusableMID, vtable 0x44684f0 slot 6, 0xb1e2b0: its pool at +0x630, count
 * +0x638, in use this frame +0x640; UMaterialInstanceDynamic::Create, 0x1c97800, in the transient
 * package). Only the state's AddReferencedObjects (slot 2, 0xb1e1b0) tells the garbage collector
 * about them, and the engine calls that only for the local player's two states
 * (ULocalPlayer::AddReferencedObjects, 0x16e1290: +0x98 and +0xc0). The insets' instances were
 * collected (their Resources[0] set to null) while their pool kept them: the next frame's
 * parameter update (GameThread_UpdateMIParameter, 0x1c8eb70) sent a null resource to the render
 * thread, which crashed writing it (InvalidateUniformExpressionCache, 0x1cb867a: logs/mv26-layer.log,
 * RenderThread 3, the klog's backtrace 0x1cb8670 <- 0x1c94430 <- 0x1c94660, the task). The agent's
 * function stands in for ULocalPlayer's: the engine's (from the agent's copy of its first 20 bytes,
 * then on at 0x16e12a4), then each inset state's AddReferencedObjects with the same collector.
 * ULocalPlayer's class and the game's own local player class (0x217a2a8) both use it. */
#define LP_ARO mv_at(0x16e1290u)      /* ULocalPlayer::AddReferencedObjects(this, Collector) */
#define LP_ARO_REST mv_at(0x16e12a4u) /* after its first 20 bytes (up to mov rdi, [rbx+0x98]) */
#define VS_ARO_SLOT 2          /* FSceneViewStateInterface::AddReferencedObjects */
#define VS_MIDPOOL_N 0x638     /* FSceneViewState: the material instances in its pool */
static const u8 k_lp_aro[14] = {0x55, 0x48, 0x89, 0xe5, 0x41, 0x56, 0x53,
                                0x48, 0x89, 0xfb, 0x49, 0x89, 0xf6, 0x48};
u64 g_lp_aro_rest; /* game base + LP_ARO_REST (the asm below jumps there) */
static void (*o_lp_aro)(u64, u64);
static volatile u32 g_lp_aro_calls;
#ifndef MV_HOST_TEST
void mv_lp_aro_orig(u64 lp, u64 collector);
__asm__(".text\n.p2align 4\n.type mv_lp_aro_orig,@function\nmv_lp_aro_orig:\n"
        "  pushq %rbp\n"
        "  movq %rsp, %rbp\n"
        "  pushq %r14\n"
        "  pushq %rbx\n"
        "  movq %rdi, %rbx\n"
        "  movq %rsi, %r14\n"
        "  movq 0x98(%rbx), %rdi\n"
        "  jmpq *g_lp_aro_rest(%rip)\n"
        ".size mv_lp_aro_orig, .-mv_lp_aro_orig\n");
#endif

static u64 (*o_ea_get)(u64, u64);
static u64 (*o_ea_effective)(u64);
static volatile u32 g_ea_logged;

/* FSceneViewInitOptions: +0x00 ViewOrigin, +0x10 ViewRotationMatrix, +0x50 ProjectionMatrix,
 * +0x90 ViewRect, +0xa0 ConstrainedViewRect, +0xb8 SceneViewStateInterface*.
 * FSceneView: +0x38 ViewRect, +0x48 UnscaledViewRect, +0x58 UnconstrainedViewRect. */
#define OPT_KEEP 0xb0
/* The insets' density: agent.insetppt=D pixels per unit of tangent across and up (live; 1200 by
 * default, PS VR2's panel at the centre of the view, measured from Hmd2's output: 0.1 of tangent
 * spans ~120 of its pixels both ways, 2026-11-07 mv43 grid; stock 2048 has 734 across and 867
 * up; 0 for twice the periphery's). The window an inset shows is its size (agent.insetsize) over
 * that: 872 at 1200 is +-0.363 of tangent, +-20 degrees. */
static void inset_window(const float *p0, s32 wp, s32 hp, s32 sw, s32 sh, float *sx, float *sy) {
  u32 d = g_opt_insetppt;
  if (!d) {
    *sx = (float)sw / (2.0f * (float)wp), *sy = (float)sh / (2.0f * (float)hp);
    return;
  }
  float dd = (float)(d < 200 ? 200 : d > 4000 ? 4000 : d);
  *sx = (float)sw * p0[0] / (2.0f * dd * p0[11]), *sy = (float)sh * p0[5] / (2.0f * dd * p0[11]);
  /* never wider than the eye (a low density with a large inset): the whole eye at the most */
  *sx = *sx > 1.0f ? 1.0f : *sx, *sy = *sy > 1.0f ? 1.0f : *sy;
}
struct mv_eye {
  u8 opts[OPT_KEEP];
  u32 gen;
};
static struct mv_eye g_eye[2];
static u32 g_gen;         /* left-eye views made (a frame's count) */
static u32 g_view_i;      /* the view's place in the frame's loop */
static s32 g_round = -1;  /* the inset being made (0 left, 1 right), -1 none */
static s32 g_inset_rect[2][4]; /* each inset's rect in the texture (its pixels) */
/* the eyes' screen percentage / 100 (multiview_boot): the options' rects are the engine's unscaled
 * ones, its views drawn at them times this. At 960 it is 1, which every run before 2026-11-07
 * 15:00 used; at 1200 (1.25) the insets went at the unscaled eye's bottom (1080, inside the
 * 1350 rows of the eyes) and no wider than its unscaled 960 (logs/mv9x-g1200). */
static float g_mv_k = 1.0f;
static void rect_unscaled(const s32 *r, s32 *u) {
  for (int i = 0; i < 4; i++)
    u[i] = (s32)((float)r[i] / g_mv_k + 0.5f);
}
static u32 g_made, g_fixed, g_left_out, g_logged, g_out[3];
/* each inset as last made, for Hmd2's layer 1 (multiview_layer) */
struct mv_inset {
  s32 rect[4];
  s32 axis[2]; /* that eye's axis (straight ahead) in the periphery, texels (agent.insetdebug=3) */
  float tan[4]; /* left, right, up, down */
  float ctan[2]; /* the window's centre in tangent from that eye's axis (x right, y up): 0 straight
                  * ahead, the gaze's with agent.insetgaze */
  float ppt[4]; /* pixels a unit of tangent: the periphery's across and up, the inset's (agent.insetdebug=4) */
  volatile u32 gen;
};
static struct mv_inset g_mv_inset[2];
/* Each frame's two insets, by the head pose the game thread made their views with (its last
 * tracker result's time, hooks.c): the hand-over finds its frame's by the pose it carries. The
 * newest insets (the game thread a frame ahead of the hand-over) gave Hmd2 the next frame's
 * window for this frame's picture: still windows never showed it; with the window following the
 * gaze the picture moved against the periphery by each frame's change. */
#define MV_HIST 8
struct mv_frame {
  volatile u32 seq; /* odd while written */
  u32 gen;
  u64 pose;
  struct mv_inset in[2];
};
static struct mv_frame g_mv_hist[MV_HIST];
static volatile u32 g_mv_hist_n;
static u64 g_mv_pose; /* the pose of the frame being made (round 0's) */
static u64 multiview_pose_of_thread(void);
/* the threads' last head poses (the game thread's is the one its views are made with) */
static struct { void *volatile thr; volatile u64 t; } g_pose_by_thread[4];
static volatile u32 g_hist_found, g_hist_lag[3], g_hist_missed;
/* the gaze's scatter about the smoothed window (agent.gazefilter=1): the squared distances of the
 * samples not taken as a jump, and the jumps, since the last report: to set the jump's radius
 * above the tracker's noise (multiview_hist_report) */
static float g_gz_sq;
static u32 g_gz_n, g_gz_jumps, g_gz_hist[6];
/* the tracker's recent head poses, any thread (for the misses' log line) */
static struct { volatile u64 t; void *volatile thr; volatile u32 gen; } g_pose_ring[32];
static volatile u32 g_pose_ring_n;
static u64 g_vsr[2][5] __attribute__((aligned(16))); /* the insets' FSceneViewStateReferences */
static u32 g_vsr_made[2];
static u64 (*o_csv)(u64, u64, u64, u64, u64, u64, u32);
static u32 (*o_opts)(u64, u8 *, u64, u64, u32);

#ifndef MV_HOST_TEST
/* Draw's movzbl al,eax; inc eax is a call here: eax = 1 without stereo, 2 with, 4 with the insets.
 * Draw's caller-saved registers are dead at that point (it sets every argument again before its
 * next call) and its next instruction sets the flags again. */
void mv_count_stub(void);
__asm__(".text\n.p2align 4\n.type mv_count_stub,@function\nmv_count_stub:\n"
        "  movzbl %al, %eax\n"
        "  testl %eax, %eax\n"
        "  je 1f\n"
        "  cmpl $0, g_opt_insetviews(%rip)\n"
        "  je 2f\n"
        "  movl $4, %eax\n"
        "  ret\n"
        "2:\n"
        "  movl $2, %eax\n"
        "  ret\n"
        "1:\n"
        "  movl $1, %eax\n"
        "  ret\n"
        ".size mv_count_stub, .-mv_count_stub\n");
#endif

static int rect_in(const s32 *r, const s32 *box) {
  return r[0] >= box[0] && r[1] >= box[1] && r[2] <= box[2] && r[3] <= box[3] && r[0] < r[2] &&
         r[1] < r[3];
}

/* CalcSceneView(this, family, &location, &rotation, viewport, drawer, pass), from Draw only */
u64 mv_cs_wrap(u64 lp, u64 fam, u64 loc, u64 rot, u64 vp, u64 drawer, u32 pass) {
  if (pass == 1)
    g_view_i = 0, g_gen++;
  else
    g_view_i++;
  s32 round = pass == 2 && g_view_i >= 2 ? (s32)g_view_i - 2 : -1;
  if (round > 1)
    return 0; /* (never: four views at most) */
  int show = g_logged < 2 && g_opt_insetviews;
  if (round < 0) {
    u64 v = o_csv(lp, fam, loc, rot, vp, drawer, pass);
    static u32 vig_said;
    if (v && g_opt_insetviews && vig_said < 1 && ++vig_said)
      LOG("multiview: an eye's view vignette %f (+0xa48)%s", (double)*(const float *)(v + VIEW_VIGNETTE),
          g_opt_vignette ? " (kept)" : " (off)");
    if (v && !g_opt_vignette) { /* (agent.vignette=0: none anywhere, the user's choice worn) */
      float *vig = (float *)(v + VIEW_VIGNETTE);
      if (*vig >= 0.0f && *vig <= 4.0f)
        *vig = 0.0f;
    }
    if (show && v) {
      const s32 *r = (const s32 *)(v + 0x38);
      LOG("multiview: view %u (pass %u): rect %d,%d-%d,%d, unscaled %d,%d-%d,%d", g_view_i, pass,
          r[0], r[1], r[2], r[3], r[4], r[5], r[6], r[7]);
    }
    return v;
  }
  g_round = round;
  u64 v = o_csv(lp, fam, loc, rot, vp, drawer, 0);
  g_round = -1;
  if (!v) {
    g_left_out++;
    return 0;
  }
  /* the view's rects as the engine made them (the screen percentage may have scaled them): inside
   * the inset's own, or set to it (a rect past the texture's edge must never reach the renderer) */
  s32 *r = (s32 *)(v + 0x38);
  /* (set as the engine sets an eye's: the view rect in the texture's pixels, the unscaled and
   * unconstrained rects that divided by the screen percentage; noted where the engine's went
   * past them) */
  const s32 *want = g_inset_rect[round];
  s32 wantu[4];
  rect_unscaled(want, wantu);
  s32 made[12];
  memcpy(made, r, sizeof made);
  int fixed = 0;
  for (int k = 0; k < 3; k++) {
    const s32 *w = k ? wantu : want;
    if (!rect_in(r + 4 * k, w))
      fixed |= 1 << k, g_out[k]++;
    memcpy(r + 4 * k, w, 16);
  }
  g_fixed += fixed != 0;
  /* Each inset view its own lens vignette off (agent.vignette=2 keeps it): the engine darkens
   * every view toward its edges, so each inset was dark around its frame and darker than the eye
   * it sits in, whose vignette is near none that close to its middle (worn, 2026-11-07). The
   * view's FinalPostProcessSettings.VignetteIntensity at +0xa48: EndFinalPostprocessSettings
   * (0x17af4a0, called by CalcSceneView at 0x16deed8 on the view it makes) zeroes it for
   * r.Tonemapper.Quality <= 1 (+0x850 film contrast at 0, +0x818 shadow tint at 2, grain +0xa50
   * at 3, +0xa4c at 4). Written only over a value that looks like one. */
  {
    float *vig = (float *)(v + VIEW_VIGNETTE);
    static u32 said;
    if (said < 2 && ++said)
      LOG("multiview: inset %d's view vignette %f%s", round, (double)*vig,
          g_opt_vignette == 2 ? " (kept)" : " (off)");
    if (g_opt_vignette != 2 && *vig >= 0.0f && *vig <= 4.0f)
      *vig = 0.0f;
  }
  /* The inset view's anti-aliasing method (agent.insetaam): FinalPostProcessSettings'
   * AntiAliasingMethod, the byte at +0xb0c (StartFinalPostprocessSettings 0x17af31f copies
   * r.DefaultFeature.AntiAliasing there when it is 0-2; EndFinalPostprocessSettings 0x17af597
   * clears it under the AA quality's cap; without a view state the engine had already made the
   * insets' TAA its FXAA: logs/mv75-worn read 1). 0 none, 1 FXAA, 2 TAA. */
  {
    u8 *aam = (u8 *)(v + VIEW_AAM);
    static u32 said;
    if (said < 2 && ++said)
      LOG("multiview: inset %d's anti-aliasing method %u, set %u", round, *aam, g_opt_insetaam);
    if (g_opt_insetaam <= 2 && *aam <= 2)
      *aam = (u8)g_opt_insetaam;
  }
  if (fixed && g_fixed <= 4) /* the engine's own, before they were set back */
    LOG("multiview: inset %d as the engine made it: rect %d,%d-%d,%d%s, unscaled %d,%d-%d,%d%s, "
        "unconstrained %d,%d-%d,%d%s (the inset's %d,%d-%d,%d)", round, made[0], made[1], made[2],
        made[3], fixed & 1 ? " (out)" : "", made[4], made[5], made[6], made[7],
        fixed & 2 ? " (out)" : "", made[8], made[9], made[10], made[11], fixed & 4 ? " (out)" : "",
        want[0], want[1], want[2], want[3]);
  if (show) {
    LOG("multiview: inset %d (view %u): rect %d,%d-%d,%d, unscaled %d,%d-%d,%d%s", round, g_view_i,
        r[0], r[1], r[2], r[3], r[4], r[5], r[6], r[7],
        fixed ? " (set to the inset's: the engine's went past it)" : "");
    if (round == 1)
      g_logged++;
  }
  if ((g_made & 1023) == 0)
    LOG_DETAIL("multiview: %u inset views made, %u set back to the inset's rect (rect %u, unscaled "
               "%u, unconstrained %u), %u left out", g_made, g_fixed, g_out[0], g_out[1], g_out[2],
               g_left_out);
  if ((g_made & 1023) == 0)
    multiview_hist_report();
  return v;
}

/* the insets' windows' centre in tangent (x right, y up): agent.insetgaze=1 the gaze (held
 * through a blink: the last one), 2 the made-up sweep, else straight ahead; kept within 1.
 * The gaze held (agent.gazehold=N, N/1000 of tangent, 50 by default): the tracker's samples
 * scatter around a fixed look, and a window following each one shook (worn, 2026-11-07,
 * logs/mv68-gaze). The window stays while the gaze stays within N of its centre; a jump past
 * twice N (a saccade) moves it there at once, a smaller move past N after 4 frames to the
 * smoothed gaze. Once a frame (the left eye's inset first; the game thread only). */
static void inset_gaze(float t[2]) {
  static float last[2], sm[2], c[2];
  static u32 done = ~0u, out;
  u32 m = g_opt_insetgaze;
  if (m == 1 || m == 9 || m == 10) {
    if (done != g_gen) {
      done = g_gen;
      float g[2];
      if (m == 9 || m == 10) {
        /* a made-up gaze through the same smoothing (a test of what follows the window): fixations
         * held 300-800 ms at places within 0.2 of tangent of straight ahead, then a jump; each
         * sample scattered about the fixation as the tracker scatters (an sd of ~0.03 of tangent,
         * the user's play logs/play-aa: the window moved 10-21 inset pixels a frame) */
        static u32 rng = 0x2545f491u;
        static u64 until;
        static float fx[2];
        u64 t_us = sceKernelGetProcessTime();
        rng = rng * 1664525u + 1013904223u;
        if (t_us >= until) { /* (by the clock: the same fixations at 60, 90 and 120 fps) */
          until = t_us + 300000u + (u64)(rng >> 27) * 16667u; /* (300-817 ms) */
          for (int i = 0; i < 2; i++) {
            rng = rng * 1664525u + 1013904223u;
            fx[i] = m == 10 ? 0.0f : ((float)(rng >> 8) / 16777216.0f - 0.5f) * 0.4f; /* (10: one fixation, straight ahead) */
          }
        }
        for (int i = 0; i < 2; i++) {
          float n = 0.0f; /* (three uniforms: near normal, sd 0.5 a unit) */
          for (int k = 0; k < 3; k++) {
            rng = rng * 1664525u + 1013904223u;
            n += (float)(rng >> 8) / 16777216.0f - 0.5f;
          }
          g[i] = fx[i] + n * 0.06f;
        }
        last[0] = g[0], last[1] = g[1];
      } else if (fov_gaze(g))
        last[0] = g[0], last[1] = g[1];
      float r = (float)g_opt_gazehold / 1000.0f;
      u64 now = sceKernelGetProcessTime();
      static u64 was;
      float dt = was ? (float)(now - was) * 1e-6f : 1.0f / 60.0f;
      was = now;
      dt = dt < 0.001f ? 0.001f : dt > 0.1f ? 0.1f : dt;
      if (g_opt_gazefilter == 1) {
        /* Smoothed by its speed (the "1 euro" filter, Casiez et al. 2012): a low cut-off while
         * the gaze rests (the tracker's scatter filtered out), rising with its speed (a look held
         * on a point while the head turns moves the eyes smoothly in the head: the hold moved the
         * window in steps then, worn, logs/mv74-worn). A jump past twice the hold radius (a
         * saccade) is taken at once. Tuned on a simulation (+-0.02 of tangent scatter): 0.1 Hz at
         * rest, +1 Hz a tangent a second, the speed itself at 0.3 Hz: ~0.006 of wander at rest, a
         * 0.35/s drift followed smoothly ~0.035 behind (tools/fov/multiview_test.c). */
        static float f[2], d[2];
        float fc_d = 0.3f, mc = (float)g_opt_gazemincut / 100.0f, beta = (float)g_opt_gazebeta;
        float jx = last[0] - f[0], jy = last[1] - f[1];
        /* (agent.gazehold=0: no jump taken at once, still smoothed: worn, 0 had turned the
         * smoothing off, so its two menu lines did nothing, logs/mv75-worn) */
        if (r > 0.0f && jx * jx + jy * jy > 4.0f * r * r) {
          f[0] = last[0], f[1] = last[1], d[0] = d[1] = 0.0f;
          g_gz_jumps++;
        } else {
          float e2 = jx * jx + jy * jy; /* (in 1/1000 of tangent: <5 <10 <20 <40 <80 more) */
          g_gz_sq += e2, g_gz_n++;
          u32 b = e2 < 25e-6f ? 0 : e2 < 1e-4f ? 1 : e2 < 4e-4f ? 2 : e2 < 16e-4f ? 3 : e2 < 64e-4f ? 4 : 5;
          g_gz_hist[b]++;
          float ad = 1.0f / (1.0f + 1.0f / (6.2831853f * fc_d * dt));
          for (int i = 0; i < 2; i++) {
            float raw_d = (last[i] - f[i]) / dt;
            d[i] += ad * (raw_d - d[i]);
          }
          float speed = __builtin_sqrtf(d[0] * d[0] + d[1] * d[1]);
          float fc = mc + beta * speed;
          float a = 1.0f / (1.0f + 1.0f / (6.2831853f * fc * dt));
          f[0] += a * (last[0] - f[0]), f[1] += a * (last[1] - f[1]);
        }
        c[0] = f[0], c[1] = f[1];
        sm[0] = f[0], sm[1] = f[1], out = 0;
        t[0] = c[0], t[1] = c[1];
        goto clamp;
      }
      sm[0] += 0.35f * (last[0] - sm[0]), sm[1] += 0.35f * (last[1] - sm[1]);
      float dl = (last[0] - c[0]) * (last[0] - c[0]) + (last[1] - c[1]) * (last[1] - c[1]);
      float ds = (sm[0] - c[0]) * (sm[0] - c[0]) + (sm[1] - c[1]) * (sm[1] - c[1]);
      if (r <= 0.0f || dl > 4.0f * r * r) { /* following every sample, or a saccade */
        c[0] = sm[0] = last[0], c[1] = sm[1] = last[1];
        out = 0;
      } else if (ds > r * r) {
        if (++out >= 4)
          c[0] = sm[0], c[1] = sm[1], out = 0;
      } else {
        out = 0;
      }
    }
    t[0] = c[0], t[1] = c[1];
  } else if (m == 11) {
    /* a look away and back (a test of what the insets' TAA does with what comes back into the
     * window: the pop-in, docs/MULTIVIEW.md 30): straight ahead 2.5 s, then 0.6 of tangent up for
     * 0.5 s (past the window's height: nothing of the first place left in it), then back; by the
     * clock, one place a frame for both insets; g_gaze_returns counts the looks back
     * (agent.capafter: a capture a set number of pictures after one) */
    static u32 gen_at = ~0u, away, was_away, said;
    static float c_ahead, c_away, half;
    if (gen_at != g_gen) {
      gen_at = g_gen;
      away = sceKernelGetProcessTime() % 3000000u >= 2500000u;
      /* (the windows made last frame: where the look ahead and the look away put the left one,
       * against its height, as the eye's edge may have stopped it short) */
      float ct = g_mv_inset[0].ctan[1];
      if (was_away)
        c_away = ct;
      else
        c_ahead = ct, half = (g_mv_inset[0].tan[2] + g_mv_inset[0].tan[3]) * 0.5f;
      if (was_away && !away) {
        u32 n = __atomic_add_fetch(&g_gaze_returns, 1, __ATOMIC_RELEASE);
        if (said < 3 || c_away - c_ahead < 2.0f * half) /* (the first few, then any that stayed in the window) */
          said++, LOG("insetgaze 11: look back %u: the window was %.3f of tangent up, its height %.3f: %s", n,
                      (double)(c_away - c_ahead), (double)(2.0f * half),
                      c_away - c_ahead >= 2.0f * half ? "nothing of the first place was left in it" : "PART OF THE FIRST PLACE STAYED IN IT");
      }
      was_away = away;
    }
    t[0] = 0.0f, t[1] = away ? 0.6f : 0.0f;
  } else if (m >= 2 && m <= 6) { /* 2 the sweep; 3-6 held at one of its places (for captures) */
    static const float k_sweep[5][2] = {{0, 0}, {-0.3f, 0}, {0.3f, 0}, {0, 0.2f}, {0, -0.2f}};
    u32 k = m == 2 ? (u32)(sceKernelGetProcessTime() / 2000000) % 5 : m - 2;
    t[0] = k_sweep[k][0], t[1] = k_sweep[k][1];
  } else if (m == 7) {
    /* a circle of 0.15 of tangent round straight ahead every 4 s, smooth and steady (0.24 of
     * tangent a second, ~4.7 inset pixels a frame at 60 and 1200 a tangent): the window moving
     * as a pursuit moves it, for tests of what follows the window (the insets' TAA) with the head
     * held. The angle from the clock (4 s a turn at any frame rate) */
    float a = (float)(sceKernelGetProcessTime() % 4000000u) * (6.2831853f / 4000000.0f);
    float a2 = a * a, s, c;
    /* (sin and cos by their series on [-pi, pi]: the agent has no libm) */
    if (a > 3.14159265f)
      a -= 6.2831853f, a2 = a * a;
    s = a * (1.0f - a2 / 6.0f * (1.0f - a2 / 20.0f * (1.0f - a2 / 42.0f * (1.0f - a2 / 72.0f * (1.0f - a2 / 110.0f)))));
    c = 1.0f - a2 / 2.0f * (1.0f - a2 / 12.0f * (1.0f - a2 / 30.0f * (1.0f - a2 / 56.0f * (1.0f - a2 / 90.0f * (1.0f - a2 / 132.0f)))));
    t[0] = 0.15f * c, t[1] = 0.15f * s;
  } else {
    t[0] = t[1] = 0.0f;
  }
clamp:
  for (int i = 0; i < 2; i++)
    t[i] = t[i] > 1.0f ? 1.0f : t[i] < -1.0f ? -1.0f : t[i];
}

/* CalcSceneViewInitOptions(this, &opts, viewport, drawer, pass) -> bool */
u32 mv_opt_wrap(u64 lp, u8 *o, u64 vp, u64 drawer, u32 pass) {
  u32 ok = o_opts(lp, o, vp, drawer, pass) & 0xff;
  s32 round = g_round;
  if (round < 0) {
    if (ok && (pass == 1 || pass == 2)) {
      memcpy(g_eye[pass - 1].opts, o, OPT_KEEP);
      g_eye[pass - 1].gen = g_gen;
    }
    return ok;
  }
  struct mv_eye *e = &g_eye[round];
  if (!ok || e->gen != g_gen)
    return 0; /* no options of that eye this frame: no inset */
  /* the eye's rect in the texture: the options' (unscaled) times the screen percentage */
  const s32 *eru = (const s32 *)(e->opts + 0x90);
  s32 er[4];
  for (int i = 0; i < 4; i++)
    er[i] = (s32)((float)eru[i] * g_mv_k + 0.5f);
  s32 wp = er[2] - er[0], hp = er[3] - er[1], rows = (s32)g_multiview_rows;
  /* the inset's height s (agent.insetsize) and width sw (agent.insetwidth, the height without
   * it): wider than tall as eyes move sideways most, and the rows are what the texture has least
   * of; at most the eye's width (each eye's inset within its half) */
  s32 s = g_opt_insetsize && (s32)g_opt_insetsize < rows ? (s32)g_opt_insetsize : rows;
  if (s > wp)
    s = wp;
  if (s > hp)
    s = hp;
  s32 sw = g_opt_insetwidth ? (s32)g_opt_insetwidth : s;
  if (sw > wp)
    sw = wp;
  if (wp < 64 || hp < 64 || s < 64 || sw < 64)
    return 0;
  float p[16], p0[16];
  memcpy(p, e->opts + 0x50, 64);
  memcpy(p0, p, 64);
  if (!(p[11] > 0.5f && p[11] < 1.5f))
    return 0; /* not a perspective with w = z (M[2][3] = 1): no inset */
  /* the window, in the eye's NDC: centred where the gaze lands (straight ahead, M[2][0..1] /
   * M[2][3], without agent.insetgaze; a direction (tx, ty) lands at (tx M[0][0] + M[2][0], ty
   * M[1][1] + M[2][1]) / M[2][3]), sw by s pixels at the insets' density (inset_window),
   * kept inside the eye. The same gaze for both eyes (Hmd2's is one direction). */
  float gt[2] = {0.0f, 0.0f};
  inset_gaze(gt);
  float cx = (gt[0] * p[0] + p[8]) / p[11], cy = (gt[1] * p[5] + p[9]) / p[11];
  float sx, sy;
  inset_window(p, wp, hp, sw, s, &sx, &sy);
  if (cx - sx < -1.0f)
    cx = -1.0f + sx;
  if (cx + sx > 1.0f)
    cx = 1.0f - sx;
  if (cy - sy < -1.0f)
    cy = -1.0f + sy;
  if (cy + sy > 1.0f)
    cy = 1.0f - sy;
  /* The window moved by whole inset pixels only (agent.insetsnap, on by default): the TAA
   * reprojects each frame's history by the window's move (a camera turn to it), and a move of a
   * fraction of a pixel lands the history between texels, where its filter (a 5-tap Catmull-Rom)
   * blurs it again; the gaze moves the window 10-21 inset pixels a frame with the tracker's
   * scatter (logs/play-aa), so the insets' TAA blurred and trailed with the window following the
   * gaze, never centred (the user, worn, the engine's TAA and ours alike). By whole pixels the
   * history is taken texel for texel (Catmull-Rom at a texel's centre is that texel). One inset
   * pixel is 2 sx / sw of the eye's NDC across, 2 sy / s up; the window moves at most half of one
   * (0.0004 of tangent at 1200 a tangent). */
  /* (steps of 2 pixels with agent.insetsnap=2: our TAA recovers the window's move from the
   * projections to the nearest 2 pixels, the two frames' jitters under a pixel apart: insetpp.c.
   * So with its velocity fix (agent.taa=11-15) always 2, whatever agent.insetsnap says: by 1
   * pixel or by fractions the fix would put a pixel's error in every moving frame) */
  u32 snap = taa_snap() ? 2 : g_opt_insetsnap; /* (running, or asked once ours is found; taa_snap) */
  if (snap) {
    float q = snap >= 2 ? 2.0f : 1.0f;
    float px = 2.0f * q * sx / (float)sw, py = 2.0f * q * sy / (float)s;
    float kx = cx / px, ky = cy / py;
    kx = (float)(s32)(kx + (kx >= 0.0f ? 0.5f : -0.5f)), ky = (float)(s32)(ky + (ky >= 0.0f ? 0.5f : -0.5f));
    cx = kx * px, cy = ky * py;
    if (cx - sx < -1.0f) cx += px; /* (kept inside the eye, a pixel in from where it was clamped) */
    if (cx + sx > 1.0f) cx -= px;
    if (cy - sy < -1.0f) cy += py;
    if (cy + sy > 1.0f) cy -= py;
  }
  /* clip x' = (clip x - cx clip w) / sx: the window becomes the whole NDC square */
  for (int i = 0; i < 4; i++) {
    p[i * 4 + 0] = (p[i * 4 + 0] - cx * p[i * 4 + 3]) / sx;
    p[i * 4 + 1] = (p[i * 4 + 1] - cy * p[i * 4 + 3]) / sy;
  }
  { /* for Hmd2's layer 1 (vr2.c): the window's angles from the eye's projection, tan = (ndc w
     * - M[2][c]) / M[c][c] with w = M[2][3] (left and down as distances from the axis) */
    float w = p0[11];
    struct mv_inset *mi = &g_mv_inset[round];
    mi->tan[0] = (p0[8] - (cx - sx) * w) / p0[0];
    mi->tan[1] = ((cx + sx) * w - p0[8]) / p0[0];
    mi->tan[2] = ((cy + sy) * w - p0[9]) / p0[5];
    mi->tan[3] = (p0[9] - (cy - sy) * w) / p0[5];
    mi->axis[0] = er[0] + (s32)((p0[8] / w + 1.0f) * 0.5f * (float)wp);
    mi->axis[1] = er[1] + (s32)((1.0f - p0[9] / w) * 0.5f * (float)hp);
    mi->ppt[0] = p0[0] / w * 0.5f * (float)wp, mi->ppt[1] = p0[5] / w * 0.5f * (float)hp;
    mi->ppt[2] = (float)sw / (mi->tan[0] + mi->tan[1]), mi->ppt[3] = (float)s / (mi->tan[2] + mi->tan[3]);
    mi->ctan[0] = (mi->tan[1] - mi->tan[0]) * 0.5f, mi->ctan[1] = (mi->tan[2] - mi->tan[3]) * 0.5f;
  }
  memcpy(o, e->opts, 0x50); /* that eye's origin (with its offset) and rotation */
  memcpy(o + 0x50, p, 64);
  /* below that eye: the left eye's at its left edge, the right eye's at its right edge. Hmd2
   * samples a layer wherever the eye looks (no window), so each eye can see the other inset
   * through layer 1 if they are close: 960 apart (both at their eye's left edge) the right inset
   * showed at the left eye's inner edge (2026-11-07, captures mv25); 1376 apart (2 units of
   * tangent at twice the periphery's density) both are outside the other eye's view. */
  /* Side by side in the rows below the eyes: Hmd2's window test (agent.insetwin=3, vr2.c) keeps
   * each eye's layer to its own inset's window, so neither eye sees the other's inset however
   * close they are (docs/MULTIVIEW.md 19-20). (A layout with the insets one above the other and
   * each eye's layer cut short of the other's inset, for Hmd2 without the window test, was
   * removed: inset mode always registers the window.) */
  s32 x0 = round ? er[2] - sw : er[0];
  s32 y0 = er[3];
  s32 rect[4] = {x0, y0, x0 + sw, y0 + s};
  { /* the options take it unscaled: the engine scales it back to the texture's (mv_cs_wrap then
     * sets it exactly) */
    s32 ru[4];
    rect_unscaled(rect, ru);
    memcpy(o + 0x90, ru, 16);
    memcpy(o + 0xa0, ru, 16);
  }
  memcpy(g_inset_rect[round], rect, 16);
  memcpy(g_mv_inset[round].rect, rect, 16);
  __atomic_store_n(&g_mv_inset[round].gen, g_gen, __ATOMIC_RELEASE);
  if (round == 0)
    g_mv_pose = multiview_pose_of_thread();
  else if (g_mv_inset[0].gen == g_gen && g_mv_pose) { /* both: the frame's insets kept */
    struct mv_frame *h = &g_mv_hist[g_mv_hist_n % MV_HIST];
    __atomic_fetch_add(&h->seq, 1, __ATOMIC_ACQ_REL);
    h->gen = g_gen, h->pose = g_mv_pose;
    memcpy(h->in, g_mv_inset, sizeof h->in);
    __atomic_fetch_add(&h->seq, 1, __ATOMIC_ACQ_REL);
    __atomic_store_n(&g_mv_hist_n, g_mv_hist_n + 1, __ATOMIC_RELEASE);
  }
  /* the inset's view state: none (no TAA history, no exposure of its own: mv_ea_* give it the
   * left eye's), or with agent.insetstate=1 one of its own, as ULocalPlayer makes its eyes': an
   * FSceneViewStateReference (0x28 bytes: vtable, the state, its link in the engine's list of
   * them, which DestroyAll/AllocateAll walk) allocated on this, the game thread, at its first use;
   * the state read from it each time (the engine may make it again). mv_lp_aro reports it to the
   * garbage collector from then on (its material instances). */
  u64 state = 0;
  if (g_opt_insetstate || g_opt_insetaam == 2) { /* (TAA needs one: its history) */
    u64 *ref = g_vsr[round];
    if (!g_vsr_made[round]) {
      ref[0] = astro_hdr.game_base + VSR_VTABLE;
      ((void (*)(u64 *))(astro_hdr.game_base + VSR_ALLOCATE))(ref);
      __atomic_store_n(&g_vsr_made[round], 1, __ATOMIC_RELEASE);
      LOG("multiview: inset %d: a view state of its own (0x%lx), kept from the garbage collector "
          "with the local player's", round, ref[1]);
    }
    state = ref[1];
  }
  *(u64 *)(o + 0xb8) = state;
  if (g_made < 2)
    LOG("multiview: inset %d: %dx%d at %d,%d, the eye's NDC window %f,%f +- %f,%f (%.1fx the "
        "periphery's pixels a tangent across)", round, sw, s, rect[0], rect[1], cx, cy, sx, sy,
        (float)sw / ((float)wp * sx));
  g_made++;
  return 1;
}

/* ULocalPlayer::AddReferencedObjects(this, Collector): the engine's, then the insets' states */
void mv_lp_aro(u64 lp, u64 collector) {
  o_lp_aro(lp, collector);
  s32 mids[2] = {-1, -1};
  for (int i = 0; i < 2; i++) {
    if (!__atomic_load_n(&g_vsr_made[i], __ATOMIC_ACQUIRE))
      continue;
    u64 st = g_vsr[i][1];
    if (!st)
      continue;
    const u64 *vt = *(const u64 *const *)st;
    ((void (*)(u64, u64))vt[VS_ARO_SLOT])(st, collector);
    mids[i] = *(const s32 *)(st + VS_MIDPOOL_N);
  }
  static s32 last[2] = {-1, -1};
  u32 n = ++g_lp_aro_calls;
  int changed = mids[0] != last[0] || mids[1] != last[1];
  last[0] = mids[0], last[1] = mids[1];
  if (mids[0] >= 0 && (n <= 4 || changed || (n & 63) == 0))
    LOG("multiview: the garbage collector's references (%u): the insets' view states hold %d and "
        "%d material instances", n, mids[0], mids[1]);
}

/* the view whose eye adaptation a view reads: an inset's the left eye's, any other its own */
/* an inset view (pass 0, the family's Views[2] or [3] of 3-4, the renderer's FViewInfos: the
 * eyes Views[0] and [1]): 2 or 3, its index, with *n the family's count and *left Views[0]
 * (the left eye, pass 1), or 0 */
static int inset_view(u64 v, s32 *n, u64 *left) {
  if (!g_opt_insetviews || !v || *(const u32 *)(v + 0x420) != 0)
    return 0;
  u64 fam = *(const u64 *)v;
  if (!fam)
    return 0;
  const u64 *views = *(const u64 *const *)fam;
  *n = *(const s32 *)(fam + 8);
  if (!views || *n < 3 || *n > 4 || (v != views[2] && (*n < 4 || v != views[3])))
    return 0;
  *left = views[0];
  if (!*left || *(const u32 *)(*left + 0x420) != 1)
    return 0;
  return v == views[2] ? 2 : 3;
}
static u64 ea_view(u64 v) {
  s32 n;
  u64 eye;
  int k = g_opt_inseteye ? inset_view(v, &n, &eye) : 0;
  if (!k || !*(const u64 *)(eye + 0xd10))
    return v;
  if (!g_ea_logged) {
    g_ea_logged = 1;
    LOG("multiview: the insets take the left eye's exposure (inset view %d of %d)", k, n);
  }
  return eye;
}
/* GetEyeAdaptation called from ret: the histogram pass's target is the view's own */
static u64 g_ea_writer_ret; /* game base + EA_WRITER_RET (build_step3) */
static u64 ea_get_from(u64 view, u64 cmdlist, u64 ret) {
  if (ret == g_ea_writer_ret)
    return o_ea_get(view, cmdlist);
  return o_ea_get(ea_view(view), cmdlist);
}
u64 mv_ea_get(u64 view, u64 cmdlist) {
  return ea_get_from(view, cmdlist, (u64)__builtin_return_address(0));
}
u32 mv_ea_valid(u64 view) {
  u64 st = o_ea_effective(ea_view(view));
  return st && *(const u8 *)(st + 0x628) != 0;
}

/* ---- The insets' translucency lighting volume: the left eye's ----
 * Glass and other translucent surfaces are lit from a volume the engine fits to each view's
 * frustum: FViewInfo::CalcTranslucencyLightingVolumeBounds (0xbc0bf0: view, the cascades' FBoxes,
 * their count; the inner and outer distances, atan of the projection, the field of view rounded).
 * Its one caller (0xb4a987, the view's uniform buffer) already gives the right eye the left
 * eye's (the view before it, if that is pass 1); an inset (pass 0, Views[2] and [3]) fitted its
 * own to its narrow off-axis window, and the wine bottle in the cafe was browner in the inset
 * than around it (worn and on the TV, 2026-11-07, logs/tv1-bottle; the eye's own the same with
 * the insets on or off). The agent's function stands in for it: an inset is given Views[0]'s
 * bounds, as the right eye is. Its first 14 bytes become the jump; the engine's function runs
 * from the agent's copy of its first 20 (push rbp; mov rbp,rsp; push r15/r14/r13/r12/rbx; sub
 * rsp,0x188), then on at 0xbc0c04. */
#define TLV mv_at(0xbc0bf0u)      /* FViewInfo::CalcTranslucencyLightingVolumeBounds(FBox*, int) */
#define TLV_REST mv_at(0xbc0c04u) /* after its first 20 bytes */
static const u8 k_tlv[14] = {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56,
                             0x41, 0x55, 0x41, 0x54, 0x53, 0x48};
u64 g_tlv_rest; /* game base + TLV_REST (the asm below jumps there) */
static void (*o_tlv)(u64, u64, s32);
static volatile u32 g_tlv_logged;
#ifndef MV_HOST_TEST
void mv_tlv_orig(u64 view, u64 boxes, s32 n);
__asm__(".text\n.p2align 4\n.type mv_tlv_orig,@function\nmv_tlv_orig:\n"
        "  pushq %rbp\n"
        "  movq %rsp, %rbp\n"
        "  pushq %r15\n"
        "  pushq %r14\n"
        "  pushq %r13\n"
        "  pushq %r12\n"
        "  pushq %rbx\n"
        "  subq $0x188, %rsp\n"
        "  jmpq *g_tlv_rest(%rip)\n"
        ".size mv_tlv_orig, .-mv_tlv_orig\n");
#endif
/* the view whose translucency volume a view takes: an inset the left eye's, any other its own */
static u64 tlv_view(u64 v) {
  s32 n;
  u64 eye;
  int k = g_opt_insettlv ? inset_view(v, &n, &eye) : 0;
  if (!k)
    return v;
  if (!g_tlv_logged) {
    g_tlv_logged = 1;
    LOG("multiview: the insets take the left eye's translucency lighting volume (inset view %d "
        "of %d)", k, n);
  }
  return eye;
}
void mv_tlv(u64 view, u64 boxes, s32 n) { o_tlv(tlv_view(view), boxes, n); }

/* ---- The insets' eye adaptation passes left out ----
 * The post-process chain adds the eye adaptation passes (histogram or basic) for a view with a
 * state that is not the right eye (0xc62741: cmp [rsp+0xd0] (the view's StereoPass, +0x420), 2;
 * je 0xc62989; then View.State, r15+8). Each inset has a state of its own since 21, so it ran
 * them too, for an exposure no one reads: its readers take the left eye's (ea_view). The 14
 * bytes of that cmp and je become the jump to the agent's gate, which leaves the passes out for
 * an inset as for the right eye, and goes on at 0xc6274f otherwise. */
#define EA_GATE mv_at(0xc62741u)      /* cmp dword [rsp+0xd0], 2; je 0xc62989 */
#define EA_GATE_SKIP mv_at(0xc62989u) /* after the eye adaptation passes */
#define EA_GATE_ON mv_at(0xc6274fu)   /* the state's test, the passes */
static const u8 k_ea_gate[14] = {0x83, 0xbc, 0x24, 0xd0, 0x00, 0x00, 0x00, 0x02,
                                 0x0f, 0x84, 0x3a, 0x02, 0x00, 0x00};
u64 g_ea_gate_skip, g_ea_gate_on;
static volatile u32 g_ea_skip_logged;
/* 1: the view's eye adaptation passes left out (an inset, agent.insetea) */
u32 mv_ea_skip(u64 v) {
  s32 n;
  u64 eye;
  int k = g_opt_insetea ? inset_view(v, &n, &eye) : 0;
  if (!k)
    return 0;
  if (!g_ea_skip_logged) {
    g_ea_skip_logged = 1;
    LOG("multiview: the insets' own eye adaptation passes left out (inset view %d of %d)", k, n);
  }
  return 1;
}
#ifndef MV_HOST_TEST
void mv_ea_gate(void);
/* (reached by a jmp: the frame is the engine's; the scratch registers kept, the flags of the
 * test survive the pops) */
__asm__(".text\n.p2align 4\n.type mv_ea_gate,@function\nmv_ea_gate:\n"
        "  cmpl $2, 0xd0(%rsp)\n"
        "  je 1f\n"
        "  pushq %rax\n  pushq %rcx\n  pushq %rdx\n  pushq %rsi\n  pushq %rdi\n"
        "  pushq %r8\n  pushq %r9\n  pushq %r10\n  pushq %r11\n"
        "  pushq %rbx\n"
        "  movq %rsp, %rbx\n"
        "  andq $-16, %rsp\n" /* (whatever the engine's alignment here) */
        "  movq %r15, %rdi\n"
        "  call mv_ea_skip\n"
        "  movq %rbx, %rsp\n"
        "  popq %rbx\n"
        "  testl %eax, %eax\n"
        "  popq %r11\n  popq %r10\n  popq %r9\n  popq %r8\n  popq %rdi\n"
        "  popq %rsi\n  popq %rdx\n  popq %rcx\n  popq %rax\n"
        "  jnz 1f\n"
        "  jmpq *g_ea_gate_on(%rip)\n"
        "1:\n"
        "  jmpq *g_ea_gate_skip(%rip)\n"
        ".size mv_ea_gate, .-mv_ea_gate\n");
#endif

/* ---- Occlusion culling and LOD in the insets (docs/MULTIVIEW.md 26) ----
 * With a view state of its own (agent.insetstate=1, for the game's fades) each inset keeps the
 * engine's occlusion history: what was hidden in its last frames is skipped, the GPU's answers
 * coming a frame or two late. The inset's window moves with the gaze, which the engine doesn't
 * see as a camera move, so objects that were outside the old window were skipped for a frame or
 * two and popped in (the curtains, the desk; worn 2026-11-08: gone with r.AllowOcclusionQueries=0).
 * PreVisibilityFrameSetup (0xb53f20) asks DoOcclusionQueries for each view (the view at r13 +
 * r15) and, told no, sets its FViewInfo +0x17e0 bits 0x4 and 0x8 (bIgnoreExistingQueries,
 * bDisableQuerySubmissions): the agent answers no for an inset (mv_occ_skip, agent.insetocc), so
 * each inset draws what its frustum holds, the eyes as before.
 *
 * LOD: ComputeLODForMeshes (0x17a7770) picks a mesh's level by its screen size in the view it is
 * given, PI r^2 / the view rect's area with r from max(W M00, H M11) / 2 (clamped to 1), against
 * each level's threshold times r.StaticMeshLODDistanceScale. An inset's window is about 1/27 of
 * its eye's angular area, so the same object was some 27 times "bigger" there: a finer level in
 * the inset than around it, switching as the inset passed (worn: the far objects popping at the
 * periphery, gone at LOD scale 0.1). The insets are drawn from their eyes' places with their
 * rotations, so the eye's view gives the eye's choice exactly, its dithered transitions' history
 * too: an inset (Views[2] or [3]) is answered with its eye's (Views[0] or [1]), agent.insetlod. */
#define OCC_CALL mv_at(0xb540e4u) /* PreVisibilityFrameSetup: call DoOcclusionQueries */
#define OCC_FN mv_at(0xb4d530u)   /* DoOcclusionQueries(.., shader platform) */
#define PAD_OCC mv_at(0xb4d212u)  /* 14 bytes of nopw after a ret */
#define LOD_FN mv_at(0x17a7770u)  /* ComputeLODForMeshes(meshes, view, origin, forced, radius, scale) */
#define LOD_REST mv_at(0x17a7781u) /* after its first 17 bytes */
static const u8 k_lod[14] = {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56,
                             0x41, 0x55, 0x41, 0x54, 0x53, 0x48};
volatile u32 g_opt_insetocc = 1; /* agent.insetocc=0: occlusion culling in the insets as the engine's */
volatile u32 g_opt_insetlod = 1; /* agent.insetlod=0: the insets' own LOD choice */
u64 g_occ_fn, g_lod_rest; /* game base + DoOcclusionQueries / ComputeLODForMeshes's rest (the asm) */
static u32 g_occ_logged, g_lod_logged;
u32 mv_occ_skip(u64 v) {
  s32 n;
  u64 eye;
  if (!g_opt_insetocc || !inset_view(v, &n, &eye))
    return 0;
  if (!g_occ_logged) {
    g_occ_logged = 1;
    LOG("multiview: the insets without occlusion culling (inset view of %d)", n);
  }
  return 1;
}
static u32 (*o_lod)(u64, u64, u64, s32, float, float);
u32 mv_lod(u64 meshes, u64 view, u64 origin, s32 forced, float radius, float scale) {
  s32 n;
  u64 left;
  int k = g_opt_insetlod ? inset_view(view, &n, &left) : 0;
  if (k) {
    const u64 *views = *(const u64 *const *)*(const u64 *)view;
    u64 eye = views[k - 2];
    if (eye && *(const u32 *)(eye + 0x420) == (u32)(k - 1)) {
      if (!g_lod_logged) {
        g_lod_logged = 1;
        LOG("multiview: the insets' LOD chosen as their eyes' (inset view %d of %d)", k, n);
      }
      view = eye;
    }
  }
  return o_lod(meshes, view, origin, forced, radius, scale);
}
#ifndef MV_HOST_TEST
void mv_occ_stub(void);
/* (reached by the engine's call through the pad: the original's arguments kept, its result 0
 * for an inset; the engine's caller-saved registers are free across the call) */
__asm__(".text\n.p2align 4\n.type mv_occ_stub,@function\nmv_occ_stub:\n"
        "  pushq %rdi\n"
        "  pushq %rsi\n"
        "  subq $8, %rsp\n"
        "  leaq (%r13,%r15), %rdi\n"
        "  call mv_occ_skip\n"
        "  addq $8, %rsp\n"
        "  popq %rsi\n"
        "  popq %rdi\n"
        "  testl %eax, %eax\n"
        "  jnz 1f\n"
        "  jmpq *g_occ_fn(%rip)\n"
        "1:\n"
        "  xorl %eax, %eax\n"
        "  ret\n"
        ".size mv_occ_stub, .-mv_occ_stub\n");
u32 mv_lod_orig(u64, u64, u64, s32, float, float);
__asm__(".text\n.p2align 4\n.type mv_lod_orig,@function\nmv_lod_orig:\n"
        "  pushq %rbp\n"
        "  movq %rsp, %rbp\n"
        "  pushq %r15\n"
        "  pushq %r14\n"
        "  pushq %r13\n"
        "  pushq %r12\n"
        "  pushq %rbx\n"
        "  subq $0x28, %rsp\n"
        "  jmpq *g_lod_rest(%rip)\n"
        ".size mv_lod_orig, .-mv_lod_orig\n");
#endif

static void put64(u8 *p, u64 v) {
  put32(p, (u32)v);
  put32(p + 4, (u32)(v >> 32));
}
static void jump_pad(u32 pad, u64 to) {
  u8 j[14] = {0xff, 0x25, 0, 0, 0, 0}; /* jmp [rip+0]; the address after it */
  put64(j + 6, to);
  add(pad, k_pad, j, 14);
}
/* a function's first 14 bytes as the jump itself (nothing branches into them) */
static void jump_at(u32 at, const u8 *was, u64 to) {
  u8 j[14] = {0xff, 0x25, 0, 0, 0, 0};
  put64(j + 6, to);
  add(at, was, j, 14);
}
static void call_to(u32 at, const u8 *was, u32 pad) {
  u8 c[5] = {0xe8};
  put32(c + 1, pad - (at + 5));
  add(at, was, c, 5);
}

static void build_step3(void) {
  o_csv = (u64(*)(u64, u64, u64, u64, u64, u64, u32))(astro_hdr.game_base + CSV_FN);
  o_opts = (u32(*)(u64, u8 *, u64, u64, u32))(astro_hdr.game_base + OPTS_FN);
  jump_pad(PAD_COUNT, (u64)mv_count_stub);
  jump_pad(PAD_CSV, (u64)mv_cs_wrap);
  jump_pad(PAD_OPTS, (u64)mv_opt_wrap);
  call_to(DRAW_COUNT, k_count, PAD_COUNT);
  /* the two calls as they are, from the build's addresses (Hikari's: e8 41 a2 10 00 and e8 6f ef ff
   * ff; Chisato's and Allison's first is e8 51 a2 10 00) */
  u8 csv_call[5] = {0xe8}, opts_call[5] = {0xe8};
  put32(csv_call + 1, CSV_FN - (DRAW_CSV + 5));
  put32(opts_call + 1, OPTS_FN - (CSV_OPTS + 5));
  call_to(DRAW_CSV, csv_call, PAD_CSV);
  call_to(CSV_OPTS, opts_call, PAD_OPTS);
  add(TONEMAP_JA, k_ja, k_jne, 2);
  add(UPSCALE_JA, k_ja, k_jne, 2);
  o_ea_get = (u64(*)(u64, u64))(astro_hdr.game_base + EA_GET_BODY);
  o_ea_effective = (u64(*)(u64))(astro_hdr.game_base + EA_EFFECTIVE);
  jump_at(EA_GET, k_ea_get, (u64)mv_ea_get);
  jump_at(EA_VALID, k_ea_valid, (u64)mv_ea_valid);
  g_uvm_rest = astro_hdr.game_base + UVM_REST;
#ifndef MV_HOST_TEST
  o_uvm = mv_uvm_orig;
#endif
  jump_at(UVM, k_uvm, (u64)mv_uvm);
  g_lp_aro_rest = astro_hdr.game_base + LP_ARO_REST;
#ifndef MV_HOST_TEST
  o_lp_aro = mv_lp_aro_orig;
#endif
  jump_at(LP_ARO, k_lp_aro, (u64)mv_lp_aro);
  g_tlv_rest = astro_hdr.game_base + TLV_REST;
#ifndef MV_HOST_TEST
  o_tlv = mv_tlv_orig;
#endif
  jump_at(TLV, k_tlv, (u64)mv_tlv);
  /* the eye's bloom and AO in the insets (insetpp.c, agent.insetpp): the tonemapper's and the AO
   * pass's Process */
  insetpp_boot(astro_hdr.game_base);
  jump_at(k_insetpp_at[0], k_insetpp_tm, (u64)mv_tm);
  jump_at(k_insetpp_at[1], k_insetpp_ao, (u64)mv_ao);
  jump_at(k_insetpp_at[2], k_insetpp_tm, (u64)mv_tmf); /* (the film tonemapper: the same first bytes) */
  jump_at(k_insetpp_at[3], k_insetpp_tm, (u64)mv_ssr); /* (the reflections' trace: the same too) */
  if (g_mv_fps) /* the native 90 or 120 (framerate.txt): two slots of their own */
    build_fps(g_mv_fps);
  g_ea_gate_skip = astro_hdr.game_base + EA_GATE_SKIP, g_ea_gate_on = astro_hdr.game_base + EA_GATE_ON;
  { /* (the ones read as the game runs, looked up once here: one this build lacks shows now) */
    g_ea_writer_ret = astro_hdr.game_base + EA_WRITER_RET;
    volatile u32 later = VSR_ALLOCATE | VSR_VTABLE;
    (void)later;
  }
#ifndef MV_HOST_TEST
  jump_at(EA_GATE, k_ea_gate, (u64)mv_ea_gate);
#else
  jump_at(EA_GATE, k_ea_gate, (u64)mv_ea_skip);
#endif
  /* no occlusion culling in the insets, and their eyes' LOD */
  g_occ_fn = astro_hdr.game_base + OCC_FN;
#ifndef MV_HOST_TEST
  jump_pad(PAD_OCC, (u64)mv_occ_stub);
#else
  jump_pad(PAD_OCC, (u64)mv_occ_skip);
#endif
  {
    u8 occ_call[5] = {0xe8};
    put32(occ_call + 1, OCC_FN - (OCC_CALL + 5));
    call_to(OCC_CALL, occ_call, PAD_OCC);
  }
  g_lod_rest = astro_hdr.game_base + LOD_REST;
#ifndef MV_HOST_TEST
  o_lod = mv_lod_orig;
#endif
  jump_at(LOD_FN, k_lod, (u64)mv_lod);
}

/* hooks.c (the tracker's GetResult for the headset): the pose time given to this thread */
void multiview_note_pose(u64 t) {
  void *self = scePthreadSelf();
  {
    u32 k = __atomic_fetch_add(&g_pose_ring_n, 1, __ATOMIC_ACQ_REL) % 32;
    g_pose_ring[k].t = t, g_pose_ring[k].thr = self, g_pose_ring[k].gen = g_gen;
  }
  for (int i = 0; i < 4; i++) {
    void *th = g_pose_by_thread[i].thr;
    if (th == self || (!th && __atomic_compare_exchange_n(&g_pose_by_thread[i].thr, &th, self, 0,
                                                          __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))) {
      g_pose_by_thread[i].t = t;
      return;
    }
    if (th == self)
      break;
  }
}
/* the pose this thread was last given; the newest any thread was given if it asks for none (the
 * game's views are made on a thread that may not ask the tracker itself: none found by the first
 * keying, 2026-11-07 logs/mv70-fp) */
static u64 multiview_pose_of_thread(void) {
  void *self = scePthreadSelf();
  for (int i = 0; i < 4; i++)
    if (g_pose_by_thread[i].thr == self)
      return g_pose_by_thread[i].t;
  u32 n = __atomic_load_n(&g_pose_ring_n, __ATOMIC_ACQUIRE);
  return n ? g_pose_ring[(n - 1) % 32].t : 0;
}

/* the insets of the frame made with the head pose at (0: the newest made), copied whole; 0 if
 * none are known. Counted: found (and how many frames behind the newest), not found. */
/* The hand-over's pose is the render thread's late one (it asks the tracker again before
 * drawing, ~6 ms after the game thread asked and made the views), not the one the views were made
 * with: the frame is the one newest when that pose was asked (logs/mv71-fp: each late pose 6 ms
 * before its frame's own, asked while that frame was the newest; the hand-over 3 frames later) */
static u32 gen_of_pose(u64 at) {
  u32 pn = __atomic_load_n(&g_pose_ring_n, __ATOMIC_ACQUIRE);
  for (u32 k = 0; k < 32 && k < pn; k++) {
    u32 j = (pn - 1 - k) % 32;
    if (g_pose_ring[j].t == at)
      return g_pose_ring[j].gen;
  }
  return 0;
}
static int insets_of(u64 at, struct mv_inset out[2]) {
  u32 n = __atomic_load_n(&g_mv_hist_n, __ATOMIC_ACQUIRE);
  u32 gen = at ? gen_of_pose(at) : 0;
  for (u32 k = 0; at && k < MV_HIST && k < n; k++) {
    struct mv_frame *h = &g_mv_hist[(n - 1 - k) % MV_HIST];
    u32 s0 = __atomic_load_n(&h->seq, __ATOMIC_ACQUIRE);
    if ((s0 & 1) || (h->pose != at && (!gen || h->gen != gen)))
      continue;
    memcpy(out, h->in, 2 * sizeof *out);
    if (__atomic_load_n(&h->seq, __ATOMIC_ACQUIRE) != s0)
      continue; /* written over meanwhile */
    __atomic_fetch_add(&g_hist_found, 1, __ATOMIC_RELAXED);
    __atomic_fetch_add(&g_hist_lag[k < 2 ? k : 2], 1, __ATOMIC_RELAXED);
    return 1;
  }
  if (at) {
    u32 m = __atomic_fetch_add(&g_hist_missed, 1, __ATOMIC_RELAXED);
    if (m < 4 || m % 2000 == 0) { /* what the frame's pose is, beside the frames' and the tracker's */
      char b[600];
      int o = agent_snprintf(b, sizeof b, "frames (gen:pose)");
      for (u32 k = 0; k < 4 && k < n; k++) {
        struct mv_frame *h = &g_mv_hist[(n - 1 - k) % MV_HIST];
        o += agent_snprintf(b + o, sizeof b - (u32)o, " %u:%lu", h->gen, h->pose);
      }
      u32 pn = __atomic_load_n(&g_pose_ring_n, __ATOMIC_ACQUIRE);
      o += agent_snprintf(b + o, sizeof b - (u32)o, "; tracker (pose thread gen)");
      for (u32 k = 0; k < 8 && k < pn && o < (int)sizeof b - 40; k++) {
        u32 j = (pn - 1 - k) % 32;
        o += agent_snprintf(b + o, sizeof b - (u32)o, " %lu %lx %u", g_pose_ring[j].t,
                      (u64)g_pose_ring[j].thr & 0xffffff, g_pose_ring[j].gen);
      }
      LOG("multiview: the frame's pose %lu not among its frames' (now gen %u, %lu us): %s", at,
          g_gen, (u64)sceKernelGetProcessTime(), b);
    }
  }
  u32 now = __atomic_load_n(&g_gen, __ATOMIC_ACQUIRE);
  for (int i = 0; i < 2; i++) {
    u32 gen = __atomic_load_n(&g_mv_inset[i].gen, __ATOMIC_ACQUIRE);
    if (!gen || now - gen > 4)
      return 0;
    memcpy(&out[i], &g_mv_inset[i], sizeof out[i]);
  }
  return 1;
}
/* the counts since the last call (hooks.c's 10 s report) */
void multiview_hist_report(void) {
  static u32 was[5];
  u32 v[5] = {g_hist_found, g_hist_lag[0], g_hist_lag[1], g_hist_lag[2], g_hist_missed};
  if (v[0] != was[0] || v[4] != was[4])
    LOG_DETAIL("multiview: frames' insets by their pose: %u found (%u the newest made, %u one behind, %u "
        "more), %u not found (the newest used)", v[0] - was[0], v[1] - was[1], v[2] - was[2],
        v[3] - was[3], v[4] - was[4]);
  memcpy(was, v, sizeof was);
  if (g_gz_n) { /* (the game thread writes these; a report a few frames off is fine) */
    LOG_DETAIL("multiview: the gaze about its smoothed window: rms %.4f of tangent over %u frames (under "
        "0.005 %u, 0.01 %u, 0.02 %u, 0.04 %u, 0.08 %u, more %u), %u jumps taken at once (past %.3f)",
        (double)__builtin_sqrtf(g_gz_sq / (float)g_gz_n), g_gz_n, g_gz_hist[0], g_gz_hist[1],
        g_gz_hist[2], g_gz_hist[3], g_gz_hist[4], g_gz_hist[5], g_gz_jumps,
        (double)g_opt_gazehold / 500.0);
    g_gz_sq = 0, g_gz_n = 0, g_gz_jumps = 0;
    memset(g_gz_hist, 0, sizeof g_gz_hist);
  }
}

/* vr2.c's layer 1: both insets' rects and angles for the frame made with the head pose at (0:
 * the newest), if both were made in the last few frames */
int multiview_layer(s32 rect[2][4], float tan[2][4], u64 at) {
  struct mv_inset in[2];
  if (!insets_of(at, in))
    return 0;
  for (int i = 0; i < 2; i++) {
    memcpy(rect[i], in[i].rect, 16);
    memcpy(tan[i], in[i].tan, 16);
    if (!(tan[i][0] + tan[i][1] > 1e-3f) || !(tan[i][2] + tan[i][3] > 1e-3f))
      return 0;
  }
  return 1;
}

void multiview_centres(float ctan[2][2], u64 at) {
  struct mv_inset in[2];
  if (!insets_of(at, in))
    memset(in, 0, sizeof in);
  for (int i = 0; i < 2; i++)
    ctan[i][0] = in[i].ctan[0], ctan[i][1] = in[i].ctan[1];
}

void multiview_axis(s32 axis[2][2], float ppt[4], u64 at) {
  struct mv_inset in[2];
  if (!insets_of(at, in))
    memset(in, 0, sizeof in);
  for (int i = 0; i < 2; i++)
    axis[i][0] = in[i].axis[0], axis[i][1] = in[i].axis[1];
  memcpy(ppt, in[0].ppt, 16); /* the same for both eyes (mirrored) */
}

/* Inset mode's defaults per frame rate (docs/MULTIVIEW.md 28): the eyes' width (resolution.txt's
 * own when it asks 1600 or less, at any rate: over that it is the stock game's), the quality
 * variables inset mode sets (cvars.txt's lines still win). The insets' rows are the payload's default
 * agent.multiview (MULTIVIEW_DEFAULT*), each inset twice as wide. 60: the game's own pace,
 * reprojected; 90 and 120 the native modes (want_fps, a NATIVE payload). */
static const struct mv_rate_preset k_rate_presets[3] = {
    {60, 1440, 3, 5, 6},
    {90, 1200, 3, 5, 6},
    {120, 960, 3, 5, 6},
};
const struct mv_rate_preset *multiview_rate_preset(void) {
  u32 f = g_mv_fps;
  return &k_rate_presets[f == 120 ? 2 : f == 90 ? 1 : 0];
}

/* The native 90/120 modes from the agent's own table (docs/MULTIVIEW.md 28): the plugin's two
 * stores of its frame sequence (PSVRFrameSequence: 1 = 60 reprojected, 2 = native 90, 3 = native
 * 120; docs/SUMMER-LESSON.md "Frame rate"), the bytes the payload's NATIVE build writes
 * (payload/main.c k_fps120), at each build's place, applied by the payload with the rest at the
 * entry point (which then leaves its own frame-rate patch out). The sequence's value is the last
 * byte of a 32-bit immediate: +6 in the first, +7 in the second. */
static const u32 k_fps_at[BUILD_N][2] = {
    {0x2f8d24b, 0x2f8e564}, {0x2ffc31b, 0x2ffd634}, {0x2ffc2db, 0x2ffd5f4}, {0x2f8826b, 0x2f89584}};
static const u8 k_fps_store0[10] = {0xc7, 0x83, 0xb8, 0x02, 0, 0, 0x01, 0, 0, 0};
static const u8 k_fps_store1[11] = {0x41, 0xc7, 0x86, 0xb8, 0x02, 0, 0, 0x01, 0, 0, 0};
static u8 fps_code(u32 fps) { return fps == 90 ? 2 : 3; }
static void build_fps(u32 fps) {
  u32 b = astro_hdr.build - 1;
  u8 a[10], c[11];
  memcpy(a, k_fps_store0, 10), memcpy(c, k_fps_store1, 11);
  a[6] = c[7] = fps_code(fps);
  add(k_fps_at[b][0], k_fps_store0, a, 10);
  add(k_fps_at[b][1], k_fps_store1, c, 11);
}

/* once the payload has applied the table (after the boot returned, the game not yet running its
 * PS VR): the stores read back from the game's code; only then the agent paces natively (the
 * header's frame_pace and refresh_hz, as the payload's NATIVE build sets them). A refused table
 * leaves the stock bytes: 60 as before. */
static volatile u32 g_booted;
void multiview_booted(void) { g_booted = 1; }
void multiview_native_confirm(void) {
  static volatile u32 done;
  if (!g_mv_fps || !g_booted || done || !__sync_bool_compare_and_swap(&done, 0, 1))
    return;
  u32 b = astro_hdr.build - 1;
  const u8 *g = (const u8 *)astro_hdr.game_base;
  u8 v0 = g[k_fps_at[b][0] + 6], v1 = g[k_fps_at[b][1] + 7], want = fps_code(g_mv_fps);
  if (v0 == want && v1 == want && g_multiview_rows) {
    astro_hdr.refresh_hz = g_mv_fps == 90 ? 90 : 0;
    __atomic_store_n(&astro_hdr.frame_pace, 1, __ATOMIC_RELEASE);
    LOG("multiview: the game's native %u Hz sequence (the agent's table), a frame every refresh",
        g_mv_fps);
  } else {
    LOG("multiview: native %u asked, but the plugin's stores read %u and %u (inset mode %u): the "
        "game's own 60 fps", g_mv_fps, v0, v1, g_multiview_rows);
    g_mv_fps = 0;
  }
}

/* from agent_boot (hooks.c): the table for the payload, or 0 */
u64 multiview_boot(u64 n) {
  if (!n || n > 4096 || !astro_hdr.game_base)
    return 0;
  if (astro_hdr.build < 1 || astro_hdr.build > BUILD_N) {
    LOG("multiview: inset mode off: an unknown build of the game (%u)", astro_hdr.build);
    return 0;
  }
  /* The native rate: the payload's want_fps (framerate.txt 90 or 120). (The measurements of
   * docs/MULTIVIEW.md 28 passed it in agent.multiview=N from 2048, for a payload without
   * want_fps; with the stores in slots of their own such a payload refuses the table anyway.) */
  g_mv_fps = astro_hdr.want_fps == 90 || astro_hdr.want_fps == 120 ? astro_hdr.want_fps : 0;
  if (g_mv_fps)
    LOG("multiview: framerate.txt asks the native %u fps", g_mv_fps);
  /* the eyes' height: the game's 1080 rows times r.ScreenPercentage (resolution.txt, in the
   * header before the boot; 0: the game's own 130), rounded up as the plugin does */
  float pct = 130.0f;
  if (astro_hdr.screen_pct)
    memcpy(&pct, &astro_hdr.screen_pct, 4);
#ifndef MV_HOST_TEST
  /* Inset mode: agent.multiview alone turns on the whole setup (cvars.txt's own lines, read
   * after this, still set any of it otherwise): the eyes 1440 wide (resolution.txt's own only
   * when it asks 1600 or less: a stock 2048 left there doesn't apply), the insets two rows wide
   * (1200x600 side by side at agent.multiview=600, Hmd2's window test), following the gaze (eye
   * tracking on); bloom and AO the eye's own in the insets (agent.insetpp, insetpp.c), shadows
   * and AA quality highest (apply_cvars). Without the
   * line: the stock game at resolution.txt's size. */
  const struct mv_rate_preset *rp = multiview_rate_preset();
  g_mv_stock_pct = astro_hdr.screen_pct, g_mv_pct_set = 0;
  if (!astro_hdr.screen_pct || pct > 1600.0f / 9.6f) {
    g_mv_pct_set = 1;
    LOG("multiview: inset mode at %u fps: the eyes %u wide (resolution.txt's %u is for the stock "
        "game)", rp->fps, rp->eye, (u32)(pct * 9.6f + 0.5f));
    pct = (float)rp->eye / 9.6f;
    memcpy(&astro_hdr.screen_pct, &pct, 4);
  }
  g_opt_insetviews = 2, g_opt_insetgaze = 1, g_opt_insetwin = 3, g_opt_insetwidth = 2 * (u32)n;
  g_opt_insetstate = 1; /* the game's post-process materials: its fades (docs/MULTIVIEW.md 21) */
  g_opt_insetpp = 7;    /* the eye's bloom, AO and reflections in the insets (insetpp.c, docs/MULTIVIEW.md 22, 27) */
  g_opt_insetaam = 2;   /* the insets' AA the eyes' TAA (docs/MULTIVIEW.md 30) */
  g_opt_taa = taa_default(); /* our TAA in every view (13: the engine's, with the insets' velocity fixed and what comes
                              * back into the window converged quickly; agent.h, insetpp.c, docs/MULTIVIEW.md 30) */
  fov_preset_gaze();
#endif
  if (!(pct >= 50.0f && pct <= 400.0f))
    return 0;
  u32 eye_h = (u32)(1080.0f * pct / 100.0f + 0.999f);
  g_multiview_rows = (u32)n;
  g_mv_k = pct / 100.0f;
  memset(&g_patches, 0, sizeof g_patches);
  g_patches.magic = AGENT_PATCH_MAGIC;
  g_patches_over = 0, g_at_missing = 0;
  build_step2((u32)n, eye_h);
  build_step3();
  if (g_at_missing) {
    LOG("multiview: inset mode off: Hikari's 0x%x has no address in this build's table", g_at_missing);
    g_multiview_rows = 0;
    return 0;
  }
  if (g_patches_over) { /* (AGENT_PATCH_MAX is the payload's too: raise both together) */
    LOG("multiview: more patches than the table's %u: none", AGENT_PATCH_MAX);
    g_multiview_rows = 0;
    return 0;
  }
  LOG("multiview: %u patches for the payload (agent.multiview=%u: %u rows for the insets below "
      "eyes %u rows high; the inset views with agent.insetviews=1)", g_patches.n, (u32)n, (u32)n,
      eye_h);
  return (u64)&g_patches;
}
