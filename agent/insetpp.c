/* Inset mode (docs/MULTIVIEW.md 22): the insets with the eye's own bloom and ambient occlusion.
 *
 * Unreal makes both per view, from what that view sees: bloom (with the lens flares) is a chain of
 * blurs over the view's whole picture, added to the scene in the tonemapper; screen-space ambient
 * occlusion is a texture the size of the scene buffer that every view writes its own rect of and
 * its lighting reads. An inset made its own from its narrow, dense frustum: the glow round a light
 * stopped at the inset's edge (it can't see a light outside it), its kernels and AO radius were
 * the inset's own (item 11), so inset mode had both off. Here the inset gets the eye's:
 *
 *   - bloom: at each eye's tonemapper (its input 1 is the eye's finished bloom and flares, half
 *     the scene buffer's resolution), the agent draws that texture sampled at each inset pixel's
 *     direction into F, a texture the scene buffer's size, at the inset's own pixels; the inset's
 *     tonemapper then runs with F as its input 1 and takes, at each pixel, F's texel: the value
 *     the eye's tonemapper takes at that direction. With the eye's exposure (mv_ea_*) and the
 *     eye's bloom strength (the view's BloomIntensity, left as the eye's: multiview.c doesn't
 *     scale it while agent.insetpp's bit 1 is on), the inset adds the eye's bloom exactly.
 *   - AO: after the inset's last AO pass (it wrote the inset's rect of the scene's AO texture),
 *     the agent draws the eye's rect of that same texture, sampled at each inset pixel's
 *     direction, over the inset's rect, before anything reads it (the base pass AO, the ambient
 *     cubemap, the sky light and reflections all read it at the pixel's own texel).
 *
 * The draws are the engine's own: render-thread commands recorded into the pass's command list
 * the way the engine's passes record theirs (its SetRenderTarget, its static states, its
 * SetGlobalBoundShaderState, FScreenVS/FScreenPS from the global shader map, its DrawRectangle,
 * its CopyToResolveTarget), so the RHI keeps every GPU state and transition as for its own.
 * The mapping inset -> eye is the inset's projection, a crop of the eye's (multiview.c):
 * ndc_eye = s ndc_inset + c each way, read from the two views' matrices this frame. Bloom is made
 * from the TAA's output (no jitter): the matrices without the jitter the renderer added
 * (FViewInfo +0x1e4/+0x1e8, 0xb54648). AO is made from the jittered frame: the matrices as drawn.
 *
 * The hooks: the two tonemappers' Process (0xcb1620; the film one, 0xcaebb0, which this game
 * runs) and the AO pass's Process (0xbf3b40), each
 * through a jump at its start (multiview.c's patch table); the engine's code runs from the
 * agent's copy of its first instructions, then on in place.
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include "vr2.h"

volatile u32 g_opt_insetpp; /* agent.insetpp: the eye's bloom (1) and AO (2) in the insets, live;
                             * 3 in inset mode (multiview_boot) */

/* ---- the engine (UE 4.12): Hikari 1.00's addresses (CUSA07434), the game's own through mv_at ---- */
#define TM_PROCESS mv_at(0xcb1620u) /* FRCPassPostProcessTonemap::Process(this, Context) */
#define TM_REST mv_at(0xcb1634u)    /* after its first 20 bytes */
/* the film tonemapper's (r.TonemapperFilm, the one this game runs: pp1-rec1 never reached the
 * other; made by AddTonemapper 0xc67d00, vtable 0x4473ce8; inputs 0 the scene, 1 the combined
 * bloom, 2 eye adaptation, 3 the combined LUTs) */
#define TMF_PROCESS mv_at(0xcaebb0u)
#define TMF_REST mv_at(0xcaebc4u)
#define AO_PROCESS mv_at(0xbf3b40u) /* FRCPassPostProcessAmbientOcclusion::Process(this, Context) */
#define AO_REST mv_at(0xbf3b51u)    /* after its first 17 bytes */
#define SSR_PROCESS mv_at(0xd4da30u) /* FRCPassPostProcessScreenSpaceReflections::Process(this, Context) */
#define SSR_REST mv_at(0xd4da44u)    /* after its first 20 bytes */
#define AO_SETUP_AS_INPUT 0xdcu /* its bAOSetupAsInput: 0 for the last pass (writes ScreenSpaceAO) */
#define E_ALLOC_CHUNK mv_at(0x22b980u) /* FMemStackBase::AllocateNewChunk(this, MinSize) */
#define E_SET_BSS mv_at(0x18e7910u)    /* SetGlobalBoundShaderState(cl, level, bss, decl, vs, ps, gs) */
#define E_DRAW_RECT mv_at(0xd2acc0u)   /* DrawRectangle(cl, x, y, w, h, u, v, su, sv, target, tex, vs, flags) */
#define E_SET_RT mv_at(0x8fa350u)      /* SetRenderTarget(cl, texture, depth, bWritableBarrier) */
#define E_FIND_FREE mv_at(0xd22600u)   /* FRenderTargetPool::FindFreeElement(pool, cl, desc, out, name, barrier) */
#define E_SCENE_CTX mv_at(0xd2d6b0u)   /* FSceneRenderTargets::Get(cl) */
#define E_SHADER_RHI mv_at(0x882f00u)  /* FShaderResource's lazy RHI shader creation */
#define E_BLEND mv_at(0x92b8a0u)       /* TStaticBlendState<>::GetRHI */
#define E_RASTER mv_at(0x8f6ed0u)      /* TStaticRasterizerState<>::GetRHI */
#define E_DEPTH mv_at(0x8f7270u)       /* TStaticDepthStencilState<false, CF_Always>::GetRHI */
#define E_SAMP_BILINEAR mv_at(0x8e0ee0u) /* TStaticSamplerState<SF_Bilinear, clamp x3>::GetRHI */
#define E_SAMP_POINT mv_at(0x9075c0u)  /* TStaticSamplerState<SF_Point, clamp x3>::GetRHI */
/* the ones read as the passes run, Hikari's here: the game's own looked up once (insetpp_boot) */
enum { A_POOL, A_FILTER_DECL, A_CACHE_ON, A_WHITE, A_DRAWRECT_UB, A_SCREEN_VS, A_SCREEN_PS,
       A_X_VIEWPORT, A_X_BLEND, A_X_RASTER, A_X_DEPTH, A_X_PS_TEXTURE, A_X_PS_SAMPLER, A_X_RESOLVE, A_N };
static const u32 k_at_hikari[A_N] = {0x4a04bc0u, 0x4a050c0u, 0x466d854u, 0x46c0440u, 0x4a05130u,
                                     0x4af8a48u, 0x4af88e8u, 0x8ae110u,  0x8ae5e0u,  0x8ae620u,
                                     0x8ae600u,  0x256b0u,   0x25960u,   0x8fbc30u};
static u32 g_at[A_N];
#define G_POOL g_at[A_POOL]       /* GRenderTargetPool */
#define G_FILTER_DECL g_at[A_FILTER_DECL] /* GFilterVertexDeclaration (+0x30 its RHI declaration) */
#define G_CACHE_ON g_at[A_CACHE_ON]   /* the command list's state filtering on (an int) */
#define G_WHITE g_at[A_WHITE]      /* FLinearColor::White (the blend factor the engine passes) */
#define G_DRAWRECT_UB g_at[A_DRAWRECT_UB] /* FDrawRectangleParameters' struct (DrawRectangle's VS uniform buffer) */
#define T_SCREEN_VS g_at[A_SCREEN_VS]  /* FScreenVS::StaticType */
#define T_SCREEN_PS g_at[A_SCREEN_PS]  /* FScreenPS::StaticType */
/* render commands' execute functions (each FRHICommand: +0 Next, +8 the function, then fields) */
#define X_VIEWPORT g_at[A_X_VIEWPORT] /* 0x28: minx miny minz(f) maxx maxy maxz(f) */
#define X_BLEND g_at[A_X_BLEND]    /* 0x28: state, blend factor (FLinearColor) */
#define X_RASTER g_at[A_X_RASTER]   /* 0x18: state (filtered through the list's +0x98) */
#define X_DEPTH g_at[A_X_DEPTH]    /* 0x20: state, stencil ref (filtered through +0xa0) */
#define X_PS_TEXTURE g_at[A_X_PS_TEXTURE] /* 0x28: pixel shader, index, texture */
#define X_PS_SAMPLER g_at[A_X_PS_SAMPLER] /* 0x28: pixel shader, index, sampler */
#define X_RESOLVE g_at[A_X_RESOLVE]  /* 0x48: FResolveParams, source, dest, bKeepOriginalSurface */
/* FRenderingCompositePassContext: +0 View, +0x10 Pass, +0x28 RHICmdList, +0x40 FeatureLevel,
 * +0x48 the global shader map. FRHICommandList: +0x8 the last command's link, +0x14 the count,
 * +0x30/+0x38 its memory stack's top and end, +0x98/+0xa0 the last rasterizer / depth command.
 * A pass: vtable +0x10 GetInput(id) -> {source pass, output id}; the source's +0x40 GetOutput(id)
 * -> its output: +0 FPooledRenderTargetDesc (0x50 bytes: +0x14 extent), +0x50 the pooled target
 * (+0x8 TargetableTexture, +0x10 ShaderResourceTexture; vtable 0 AddRef, 1 Release).
 * FViewInfo: +0 Family (+0 Views, +8 their count: the renderer's own FViewInfos), +0x38 ViewRect,
 * +0x70 the projection, +0x1e4/+0x1e8 the TAA jitter added to its M[2][0]/M[2][1], +0x420 pass.
 * FSceneRenderTargets: +0xd8 ScreenSpaceAO (pooled; full size, G8), +0x210 BufferSize. */

static struct {
  u64 base;
  void (*alloc_chunk)(u64 stack, s32 size);
  void (*set_bss)(u64 cl, u32 level, u64 bss, u64 decl, u64 vs, u64 ps, u64 gs);
  void (*draw_rect)(u64 cl, float x, float y, float w, float h, float u, float v, float su, float sv,
                    u64 target_size, u64 tex_size, u64 vs, u32 flags);
  void (*set_rt)(u64 cl, u64 tex, u64 depth, u32 barrier);
  u32 (*find_free)(u64 pool, u64 cl, const void *desc, u64 *out, const u16 *name, u32 barrier);
  u64 (*scene_ctx)(u64 cl);
  void (*shader_rhi)(u64 resource);
  u64 (*blend)(void), (*raster)(void), (*depth)(void), (*samp_bi)(void), (*samp_pt)(void);
  void *(*dalloc)(u64 size); /* CPU and GPU memory (vr2_dalloc_pref) */
} g_e;

static void (*o_tm)(u64, u64), (*o_tmf)(u64, u64);
static void (*o_ao)(u64, u64), (*o_ssr)(u64, u64);
u64 g_tm_rest, g_tmf_rest, g_ao_rest, g_ssr_rest; /* game base + the rests (the asm below jumps there) */
#ifndef MV_HOST_TEST
void mv_tm_orig(u64 pass, u64 ctx);
__asm__(".text\n.p2align 4\n.type mv_tm_orig,@function\nmv_tm_orig:\n"
        "  pushq %rbp\n"
        "  movq %rsp, %rbp\n"
        "  pushq %r15\n"
        "  pushq %r14\n"
        "  pushq %r13\n"
        "  pushq %r12\n"
        "  pushq %rbx\n"
        "  subq $0x1f8, %rsp\n"
        "  jmpq *g_tm_rest(%rip)\n"
        ".size mv_tm_orig, .-mv_tm_orig\n");
void mv_tmf_orig(u64 pass, u64 ctx);
__asm__(".text\n.p2align 4\n.type mv_tmf_orig,@function\nmv_tmf_orig:\n"
        "  pushq %rbp\n"
        "  movq %rsp, %rbp\n"
        "  pushq %r15\n"
        "  pushq %r14\n"
        "  pushq %r13\n"
        "  pushq %r12\n"
        "  pushq %rbx\n"
        "  subq $0x1f8, %rsp\n"
        "  jmpq *g_tmf_rest(%rip)\n"
        ".size mv_tmf_orig, .-mv_tmf_orig\n");
void mv_ssr_orig(u64 pass, u64 ctx);
__asm__(".text\n.p2align 4\n.type mv_ssr_orig,@function\nmv_ssr_orig:\n"
        "  pushq %rbp\n"
        "  movq %rsp, %rbp\n"
        "  pushq %r15\n"
        "  pushq %r14\n"
        "  pushq %r13\n"
        "  pushq %r12\n"
        "  pushq %rbx\n"
        "  subq $0x98, %rsp\n"
        "  jmpq *g_ssr_rest(%rip)\n"
        ".size mv_ssr_orig, .-mv_ssr_orig\n");
void mv_ao_orig(u64 pass, u64 ctx);
__asm__(".text\n.p2align 4\n.type mv_ao_orig,@function\nmv_ao_orig:\n"
        "  pushq %rbp\n"
        "  movq %rsp, %rbp\n"
        "  pushq %r15\n"
        "  pushq %r14\n"
        "  pushq %r13\n"
        "  pushq %r12\n"
        "  pushq %rbx\n"
        "  subq $0x38, %rsp\n"
        "  jmpq *g_ao_rest(%rip)\n"
        ".size mv_ao_orig, .-mv_ao_orig\n");
#endif

/* the first bytes each hook's jump replaces (multiview.c checks them against the game's) */
const u8 k_insetpp_tm[14] = {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56,
                             0x41, 0x55, 0x41, 0x54, 0x53, 0x48};
const u8 k_insetpp_ao[14] = {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56,
                             0x41, 0x55, 0x41, 0x54, 0x53, 0x48};
u32 k_insetpp_at[4]; /* the tonemapper's, the AO pass's, the film tonemapper's and the reflections'
                     * Process (insetpp_boot; the film one's and the reflections' first 14 bytes are
                     * k_insetpp_tm's too) */

/* ---- recording render commands as the engine's inline code does ---- */
static u8 *cmd_new(u64 cl, u32 size, u32 exec) {
  u64 *top = (u64 *)(cl + 0x30);
  u64 p = (*top + 7) & ~7ull;
  if (p + size > *(u64 *)(cl + 0x38)) {
    g_e.alloc_chunk(cl + 0x30, (s32)size + 8);
    p = (*top + 7) & ~7ull;
  }
  *top = p + size;
  (*(u32 *)(cl + 0x14))++;
  **(u64 **)(cl + 0x8) = p;
  *(u64 *)(cl + 0x8) = p;
  memset((void *)p, 0, size);
  *(u64 *)(p + 8) = g_e.base + exec;
  return (u8 *)p;
}
static void cmd_viewport(u64 cl, s32 x0, s32 y0, s32 x1, s32 y1) {
  u8 *c = cmd_new(cl, 0x28, X_VIEWPORT);
  float zmin = 0.0f, zmax = 1.0f;
  *(s32 *)(c + 0x10) = x0, *(s32 *)(c + 0x14) = y0;
  memcpy(c + 0x18, &zmin, 4);
  *(s32 *)(c + 0x1c) = x1, *(s32 *)(c + 0x20) = y1;
  memcpy(c + 0x24, &zmax, 4);
}
static void cmd_states(u64 cl) {
  u8 *c = cmd_new(cl, 0x28, X_BLEND);
  *(u64 *)(c + 0x10) = g_e.blend();
  memcpy(c + 0x18, (const void *)(g_e.base + G_WHITE), 16);
  int filter = *(const s32 *)(g_e.base + G_CACHE_ON) != 0;
  u64 r = g_e.raster(), last = *(u64 *)(cl + 0x98);
  if (!filter || !last || *(u64 *)(last + 0x10) != r) {
    c = cmd_new(cl, 0x18, X_RASTER);
    *(u64 *)(c + 0x10) = r;
    *(u64 *)(cl + 0x98) = (u64)c;
  }
  u64 d = g_e.depth();
  last = *(u64 *)(cl + 0xa0);
  if (!filter || !last || *(u64 *)(last + 0x10) != d || *(u32 *)(last + 0x18) != 0) {
    c = cmd_new(cl, 0x20, X_DEPTH);
    *(u64 *)(c + 0x10) = d;
    *(u64 *)(cl + 0xa0) = (u64)c;
  }
}
static void cmd_ps_bind(u64 cl, u32 exec, u64 ps, u32 index, u64 what) {
  u8 *c = cmd_new(cl, 0x28, exec);
  *(u64 *)(c + 0x10) = ps, *(u32 *)(c + 0x18) = index, *(u64 *)(c + 0x20) = what;
}
static void cmd_resolve(u64 cl, u64 target, u64 srv) {
  u8 *c = cmd_new(cl, 0x48, X_RESOLVE);
  for (int i = 0; i < 4; i++)
    *(s32 *)(c + 0x14 + 4 * i) = -1; /* the whole texture */
  *(u64 *)(c + 0x30) = target, *(u64 *)(c + 0x38) = srv;
}

/* ---- the engine's objects ---- */
static u32 hash_combine(u32 a, u32 c) {
  u32 b = 0x9e3779b9u;
  a += b;
  a -= b; a -= c; a ^= (c >> 13);
  b -= c; b -= a; b ^= (a << 8);
  c -= a; c -= b; c ^= (b >> 13);
  a -= b; a -= c; a ^= (c >> 12);
  b -= c; b -= a; b ^= (a << 16);
  c -= a; c -= b; c ^= (b >> 5);
  a -= b; a -= c; a ^= (c >> 3);
  b -= c; b -= a; b ^= (a << 10);
  c -= a; c -= b; c ^= (b >> 15);
  return c;
}
/* a global shader by its type: TShaderMap's TMap<FShaderType*, TRefCountPtr<FShader>> at +0x30
 * (elements 24 bytes: key, value, next in the hash chain; the hash at +0x70, inline at +0x68,
 * its size +0x78), looked up as the engine's inline code does (PointerHash: key >> 4) */
static u64 global_shader(u64 map, u64 type) {
  if (!map)
    return 0;
  const u8 *data = *(const u8 *const *)(map + 0x30);
  s32 num = *(const s32 *)(map + 0x38), size = *(const s32 *)(map + 0x78);
  const s32 *hash = *(const s32 *const *)(map + 0x70);
  if (!hash)
    hash = (const s32 *)(map + 0x68);
  if (!data || num <= 0 || size <= 0 || (size & (size - 1)))
    return 0;
  s32 id = hash[hash_combine((u32)(type >> 4), 0) & (u32)(size - 1)];
  for (int guard = 0; id >= 0 && id < num && guard < 4096; guard++) {
    const u8 *e = data + 24 * (u64)id;
    if (*(const u64 *)e == type)
      return *(const u64 *)(e + 8);
    id = *(const s32 *)(e + 16);
  }
  return 0;
}
/* an FShader's RHI shader (+0x48 its resource: lazily made at +0x28; +0x38 vertex, +0x40 pixel) */
static u64 shader_rhi(u64 shader, u32 off) {
  u64 res = *(const u64 *)(shader + 0x48);
  if (!res)
    return 0;
  if (!*(const u8 *)(res + 0x28))
    g_e.shader_rhi(res);
  return *(const u64 *)(res + off);
}
static struct {
  u64 map, vs, ps, ps_rhi;
  u32 tex_index, samp_index, ok;
} g_sh;
static u64 g_bss[8]; /* our FGlobalBoundShaderState: one resource per feature level */
static volatile u32 g_said_shaders;
static int shaders_for(u64 map) {
  if (g_sh.map == map)
    return g_sh.ok;
  g_sh.map = map, g_sh.ok = 0;
  u64 vs = global_shader(map, g_e.base + T_SCREEN_VS), ps = global_shader(map, g_e.base + T_SCREEN_PS);
  int ub = 0;
  if (vs) { /* DrawRectangle's parameters must reach the vertex shader (its uniform buffer) */
    const u64 *structs = *(const u64 *const *)(vs + 0x8);
    s32 n = *(const s32 *)(vs + 0x10);
    for (s32 i = 0; structs && i < n && i < 64; i++)
      ub |= structs[i] == g_e.base + G_DRAWRECT_UB;
  }
  u16 ti = ps ? *(const u16 *)(ps + 0xbc) : 0, tn = ps ? *(const u16 *)(ps + 0xbe) : 0;
  u16 si = ps ? *(const u16 *)(ps + 0xc0) : 0, sn = ps ? *(const u16 *)(ps + 0xc2) : 0;
  u64 ps_rhi = ps ? shader_rhi(ps, 0x40) : 0;
  if (!g_said_shaders++)
    LOG("insetpp: FScreenVS 0x%lx (DrawRectangle's parameters %s), FScreenPS 0x%lx (its pixel "
        "shader 0x%lx, texture %u/%u, sampler %u/%u)", vs, ub ? "bound" : "NOT bound", ps, ps_rhi,
        ti, tn, si, sn);
  if (!vs || !ps || !ub || !ps_rhi || !tn || !sn)
    return 0;
  g_sh.vs = vs, g_sh.ps = ps, g_sh.ps_rhi = ps_rhi, g_sh.tex_index = ti, g_sh.samp_index = si;
  g_sh.ok = 1;
  return 1;
}

/* one textured rectangle: the destination's viewport (x0,y0)-(x1,y1) filled from the source's
 * texels (u,v)+(su,sv) of a texture tw x th (DrawRectangle's convention: the viewport's edges on
 * those texel coordinates' edges, so a pixel centre takes the source at its own place); the
 * destination resolved for reading after */
struct pp_draw {
  u64 dst_target, dst_srv, src_srv;
  s32 x0, y0, x1, y1, tw, th;
  float u, v, su, sv;
  u32 point;
};
static void draw(u64 cl, u32 level, const struct pp_draw *d) {
  s32 w = d->x1 - d->x0, h = d->y1 - d->y0;
  g_e.set_rt(cl, d->dst_target, 0, 1); /* (made writable: F and the AO texture were resolved for reading) */
  cmd_viewport(cl, d->x0, d->y0, d->x1, d->y1);
  cmd_states(cl);
  g_e.set_bss(cl, level, (u64)g_bss, *(const u64 *)(g_e.base + G_FILTER_DECL + 0x30), g_sh.vs, g_sh.ps, 0);
  cmd_ps_bind(cl, X_PS_TEXTURE, g_sh.ps_rhi, g_sh.tex_index, d->src_srv);
  cmd_ps_bind(cl, X_PS_SAMPLER, g_sh.ps_rhi, g_sh.samp_index, d->point ? g_e.samp_pt() : g_e.samp_bi());
  g_e.draw_rect(cl, 0.0f, 0.0f, (float)w, (float)h, d->u, d->v, d->su, d->sv,
                (u64)(u32)w | (u64)(u32)h << 32, (u64)(u32)d->tw | (u64)(u32)d->th << 32, g_sh.vs, 0);
  cmd_resolve(cl, d->dst_target, d->dst_srv);
}

/* ---- the views ---- */
struct pp_views {
  u64 v[4];
};
/* the family's four views (the eyes Views[0] and [1], passes 1 and 2; the insets [2] and [3],
 * pass 0), and which one this is (0-3), or -1 */
static int which_view(u64 view, struct pp_views *pv) {
  if (!view)
    return -1;
  u64 fam = *(const u64 *)view;
  if (!fam)
    return -1;
  const u64 *views = *(const u64 *const *)fam;
  if (!views || *(const s32 *)(fam + 8) != 4)
    return -1;
  for (int i = 0; i < 4; i++)
    if (!(pv->v[i] = views[i]))
      return -1;
  if (*(const u32 *)(pv->v[0] + 0x420) != 1 || *(const u32 *)(pv->v[1] + 0x420) != 2 ||
      *(const u32 *)(pv->v[2] + 0x420) != 0 || *(const u32 *)(pv->v[3] + 0x420) != 0)
    return -1;
  for (int i = 0; i < 4; i++)
    if (view == pv->v[i])
      return i;
  return -1;
}
/* ndc_eye = s ndc_inset + c, each way (x, then y), from the two projections: the inset's is the
 * eye's with each row's x' = (x - c w) / s (multiview.c's mv_opt_wrap); without the jitter for
 * bloom (made from the TAA's output), with it for AO (made from the frame as drawn) */
struct pp_map {
  float s[2], c[2];
  s32 er[4], ir[4]; /* the eye's and the inset's rects in the scene buffer */
};
static float jitter(u64 v, u32 off) {
  float j = *(const float *)(v + off);
  return j > -0.02f && j < 0.02f ? j : 0.0f;
}
static int map_of(u64 eye, u64 inset, int unjittered, struct pp_map *m) {
  const float *pe = (const float *)(eye + 0x70), *pi = (const float *)(inset + 0x70);
  float e20 = pe[8], e21 = pe[9], i20 = pi[8], i21 = pi[9];
  if (unjittered) {
    e20 -= jitter(eye, 0x1e4), e21 -= jitter(eye, 0x1e8);
    i20 -= jitter(inset, 0x1e4), i21 -= jitter(inset, 0x1e8);
  }
  float w = pe[11];
  if (!(w > 0.5f && w < 1.5f) || !(pi[11] > 0.5f && pi[11] < 1.5f) || !(pi[0] > 1e-3f) || !(pi[5] > 1e-3f))
    return 0;
  m->s[0] = pe[0] / pi[0], m->s[1] = pe[5] / pi[5];
  m->c[0] = (e20 - m->s[0] * i20) / w, m->c[1] = (e21 - m->s[1] * i21) / w;
  memcpy(m->er, (const void *)(eye + 0x38), 16);
  memcpy(m->ir, (const void *)(inset + 0x38), 16);
  if (!(m->s[0] > 0.01f && m->s[0] <= 1.001f && m->s[1] > 0.01f && m->s[1] <= 1.001f))
    return 0;
  return m->er[2] > m->er[0] + 16 && m->er[3] > m->er[1] + 16 && m->ir[2] > m->ir[0] + 16 &&
         m->ir[3] > m->ir[1] + 16;
}
/* a place in the inset's view (scene buffer pixels, continuous) to the same direction's place in
 * the eye's: axis 0 x, 1 y (NDC y up, the buffer's rows down) */
static double to_eye(const struct pp_map *m, int axis, double p) {
  double i0 = m->ir[axis], i1 = m->ir[axis + 2], e0 = m->er[axis], e1 = m->er[axis + 2];
  double n = 2.0 * (p - i0) / (i1 - i0) - 1.0;
  if (axis)
    n = -n;
  double ne = m->s[axis] * n + m->c[axis];
  if (axis)
    ne = -ne;
  return e0 + (ne + 1.0) * 0.5 * (e1 - e0);
}

/* a pass's input: its pooled target and its output's description (0 if none) */
static u64 pass_input(u64 pass, u32 id, const u8 **desc) {
  u64 (*get_input)(u64, u32) = *(u64 (**)(u64, u32))(*(const u64 *)pass + 0x10);
  u64 ref = get_input(pass, id);
  if (!ref || !*(const u64 *)ref)
    return 0;
  u64 src = *(const u64 *)ref;
  u64 (*get_output)(u64, u32) = *(u64 (**)(u64, u32))(*(const u64 *)src + 0x40);
  u64 out = get_output(src, *(const u32 *)(ref + 8));
  if (!out)
    return 0;
  *desc = (const u8 *)out;
  return *(const u64 *)(out + 0x50);
}
static void ext_of(const u8 *desc, s32 e[2]) {
  e[0] = *(const s32 *)(desc + 0x14), e[1] = *(const s32 *)(desc + 0x18);
}

/* ---- bloom ----
 * F: a texture the scene buffer's size (fp16), held from the left eye's tonemapper to the right
 * inset's. Each eye's tonemapper first draws, into its inset's rect of F (and a ring of one
 * texel round it where the other inset isn't), the eye's own bloom texture sampled at each pixel's
 * direction: exactly what the eye's tonemapper takes at that direction (its bilinear sample of
 * the same texels). Each inset's tonemapper then runs with F in place of its own bloom (its input
 * 1's pooled target, swapped for the call and put back): it samples F at its pixels' centres, F's
 * own texels. (The inset's own bloom texture is half its resolution: written there, the eye's
 * values were interpolated a second time by the inset's tonemapper, up to 4% of the bloom off
 * round a small bright light, tools/fov/insetpp_test.c.) The ring keeps the bilinear footprint's
 * zero-weight neighbours written (an uninitialised texel could hold a NaN); the two insets' rects
 * share an edge, where each one's neighbours are the other's own. */
/* EPixelFormat (PF_G8 is 3: ScreenSpaceAO's). F takes the eye's bloom texture's own format: the
 * eyes' are PF_FloatRGB (R11G11B10, 4 bytes a texel; logged as 9), half of the PF_FloatRGBA F had
 * before (48 MB at 2880x2220, logs/pp2-rec1). Each value is a bilinear mix of the eye's texels, in
 * the same format again: within the format's own rounding of a small additive term. */
#define PF_FLOATRGB 9
#define PF_FLOATRGBA 10
static const u16 k_name[] = {'I', 'n', 's', 'e', 't', 'E', 'y', 'e', 'B', 'l', 'o', 'o', 'm', 0};
static struct {
  u64 rt;        /* F */
  u64 inset[2];  /* the inset views written this frame (0: not yet) */
  s32 b[2];      /* its size, the buffer's */
} g_f;
static struct {
  u32 eye, inset, skipped, fail, released, calls;
} g_n;
static volatile u32 g_said_bloom, g_said_bloom_ok, g_said_fmt;
static void release_f(void) {
  if (!g_f.rt)
    return;
  u64 rt = g_f.rt;
  g_f.rt = 0, g_f.inset[0] = g_f.inset[1] = 0;
  (*(void (**)(u64))(*(const u64 *)rt + 8))(rt);
}
/* the eye's tonemapper: its input 1 (the eye's bloom and flares) at its inset's pixels into F */
static void eye_bloom(u64 pass, u64 ctx, int e, const struct pp_views *pv) {
  if (e == 0)
    release_f(); /* (a new frame: the last one's, if an inset's never came) */
  u64 cl = *(const u64 *)(ctx + 0x28);
  const u8 *d0 = 0, *d1 = 0;
  u64 in0 = pass_input(pass, 0, &d0), in1 = pass_input(pass, 1, &d1);
  struct pp_map m;
  if (!in0 || !in1 || !*(const u64 *)(in1 + 0x10) || !map_of(pv->v[e], pv->v[2 + e], 1, &m)) {
    g_n.skipped++;
    return;
  }
  s32 b[2], e1[2];
  ext_of(d0, b), ext_of(d1, e1);
  /* (input 1 the bloom texture of the same buffer at a power-of-two fraction of its size: half
   * at the highest quality, a 16th here at the game's r.BloomQuality 1, logs/pp2-rec1; the rects
   * inside it) */
  int frac = 0;
  for (s32 f = 1; f <= 64 && !frac; f *= 2)
    frac = e1[0] * f - b[0] >= -f && e1[0] * f - b[0] <= f && e1[1] * f - b[1] >= -f &&
           e1[1] * f - b[1] <= f ? f : 0;
  if (!frac || e1[0] < 8 || e1[1] < 8 || m.ir[0] < 0 || m.ir[1] < 0 || m.ir[2] > b[0] || m.ir[3] > b[1]) {
    if (g_said_bloom++ < 4)
      LOG("insetpp: the eye's tonemapper inputs %dx%d and %dx%d (inset %d,%d-%d,%d): not a scene and "
          "its bloom at a power-of-two fraction: no bloom from the eye", b[0], b[1], e1[0], e1[1], m.ir[0], m.ir[1],
          m.ir[2], m.ir[3]);
    g_n.skipped++;
    return;
  }
  if (g_f.rt && (g_f.b[0] != b[0] || g_f.b[1] != b[1]))
    release_f();
  if (!g_f.rt) {
    u8 desc[0x50];
    memcpy(desc, d1, sizeof desc);
    s32 fmt = *(const s32 *)(d1 + 0x2c); /* (F in the eye's format: desc's +0x2c kept) */
    if (!g_said_fmt++)
      LOG("insetpp: F (the eye's bloom at the insets' pixels) %dx%d in the eyes' bloom format %d "
          "(%u MB)", b[0], b[1], fmt,
          (u32)((u64)b[0] * (u64)b[1] * (fmt == PF_FLOATRGBA ? 8 : fmt == PF_FLOATRGB ? 4 : 0) >> 20));
    *(s32 *)(desc + 0x14) = b[0], *(s32 *)(desc + 0x18) = b[1];
    *(const u16 **)(desc + 0x40) = k_name;
    u64 rt = 0;
    g_e.find_free(g_e.base + G_POOL, cl, desc, &rt, k_name, 1);
    if (!rt || !*(const u64 *)(rt + 8) || !*(const u64 *)(rt + 0x10)) {
      if (rt)
        (*(void (**)(u64))(*(const u64 *)rt + 8))(rt);
      g_n.fail++;
      return;
    }
    g_f.rt = rt, g_f.b[0] = b[0], g_f.b[1] = b[1], g_f.inset[0] = g_f.inset[1] = 0;
  }
  /* the inset's rect with its ring, but not over the other inset's rect */
  const s32 *ir = m.ir, *orr = (const s32 *)(pv->v[3 - e] + 0x38);
  s32 x0 = ir[0] - 1, y0 = ir[1] - 1, x1 = ir[2] + 1, y1 = ir[3] + 1;
  int yo = orr[1] < ir[3] && ir[1] < orr[3]; /* (the other inset level with this one) */
  if (yo && x0 >= orr[0] && x0 < orr[2])
    x0 = ir[0];
  if (yo && x1 - 1 >= orr[0] && x1 - 1 < orr[2])
    x1 = ir[2];
  x0 = x0 < 0 ? 0 : x0, y0 = y0 < 0 ? 0 : y0, x1 = x1 > b[0] ? b[0] : x1, y1 = y1 > b[1] ? b[1] : y1;
  /* F's pixel X (the buffer's) takes the eye's bloom where its centre's direction lands in the
   * eye, in the eye's bloom texels: the eye's tonemapper's own sample there (its UV is the place
   * in the buffer over the buffer's size, for both its inputs) */
  double ux0 = to_eye(&m, 0, x0) * e1[0] / b[0], ux1 = to_eye(&m, 0, x1) * e1[0] / b[0];
  double vy0 = to_eye(&m, 1, y0) * e1[1] / b[1], vy1 = to_eye(&m, 1, y1) * e1[1] / b[1];
  struct pp_draw dr = {*(const u64 *)(g_f.rt + 8), *(const u64 *)(g_f.rt + 0x10), *(const u64 *)(in1 + 0x10),
                       x0, y0, x1, y1, e1[0], e1[1], (float)ux0, (float)vy0, (float)(ux1 - ux0),
                       (float)(vy1 - vy0), 0};
  draw(cl, *(const u32 *)(ctx + 0x40), &dr);
  g_f.inset[e] = pv->v[2 + e];
  g_n.eye++;
  if (g_said_bloom_ok < 2 && ++g_said_bloom_ok)
    LOG("insetpp: eye %d's bloom (%dx%d, 1/%d of a %dx%d buffer) for its inset %d,%d-%d,%d: F's %d,%d-%d,%d "
        "from the eye's texels %.2f,%.2f +%.2fx%.2f (s %.4f %.4f, c %.4f %.4f)", e, e1[0], e1[1],
        frac, b[0], b[1], ir[0], ir[1], ir[2], ir[3], x0, y0, x1, y1, ux0, vy0, ux1 - ux0, vy1 - vy0,
        (double)m.s[0], (double)m.s[1], (double)m.c[0], (double)m.c[1]);
}
/* the inset's tonemapper: F for its input 1's pooled target while it runs (0: not this time) */
static u64 inset_bloom_swap(u64 pass, int i, const struct pp_views *pv, u64 *slot) {
  /* (both insets written: F's texels round each are the other's) */
  if (!g_f.rt || g_f.inset[0] != pv->v[2] || g_f.inset[1] != pv->v[3]) {
    g_n.skipped++;
    return 0;
  }
  u64 (*get_input)(u64, u32) = *(u64 (**)(u64, u32))(*(const u64 *)pass + 0x10);
  u64 ref = get_input(pass, 1), out = 0;
  if (ref && *(const u64 *)ref) {
    u64 src = *(const u64 *)ref;
    out = (*(u64 (**)(u64, u32))(*(const u64 *)src + 0x40))(src, *(const u32 *)(ref + 8));
  }
  const u8 *d0 = 0;
  s32 b[2] = {0, 0};
  if (pass_input(pass, 0, &d0))
    ext_of(d0, b);
  /* (F is read at the inset's pixels over its scene input's size: the buffer's, as the eyes') */
  if (!out || !*(const u64 *)(out + 0x50) || b[0] != g_f.b[0] || b[1] != g_f.b[1]) {
    g_n.skipped++;
    return 0;
  }
  (void)i;
  u64 was = *(const u64 *)(out + 0x50);
  *slot = out + 0x50;
  *(u64 *)(out + 0x50) = g_f.rt;
  g_n.inset++;
  return was;
}
/* ---- the insets' TAA (agent.insetaam=2): the band round each inset's history ----
 * The engine's TAA pass (FRCPassPostProcessTemporalAA::Process, 0xca4260) clears its whole
 * output to black but the view's rect (its Clear with the rect excluded, 0x8fbc10 at 0xca4400),
 * then stores that output as the view state's history for the next frame (state +0x670, or
 * +0x668 without r.TemporalAAPauseCorrect: 0xca537f; PreVisibilityFrameSetup moves +0x670 to
 * +0x668 at the next frame's start, 0xb53fc3). The next frame's TAA reprojects each pixel into it
 * with the previous frame's matrices (the view state's, crop and all: they follow the window,
 * PreVisibilityFrameSetup 0xb53fa4/0xb54039, Prev = Pending, Pending = this frame's). An eye's
 * rect is its whole field of view, so what lies past it is never looked at; an inset's edge is in
 * the middle of the view and its window moves with the gaze, so pixels near its leading edge
 * reproject past the old rect: the history filter's footprint there (and the whole strip, if the
 * shader takes history up to the buffer's edge rather than the viewport's) reads that black,
 * which the neighbourhood clamp pulls to the darkest neighbour. So after each inset's TAA, the
 * band of its new history round its rect is drawn from its eye's TAA output of the same frame,
 * sampled at each texel's direction (the inset's pixel grid carried on past its rect, the
 * matrices without jitter: the TAA pass takes the jitter back out before it returns, 0xca5403):
 * the eye's converged picture there, which is what the inset's history should have held had its
 * window been that much bigger. Drawn at the inset's tonemapper (after its TAA in the post-process
 * chain, before the next frame's TAA reads it), into the inset's own history target only. */
volatile u32 g_opt_insettaa; /* agent.insettaa=1: the band drawn (off by default: the engine's TAA shader
                             * takes no history past the viewport, logs/aa4-block, so the band never
                             * reaches the picture; kept for a shader that reads it); live */
#define TAA_BAND 64 /* texels round the rect: a window move a frame is a few texels, the filter 2 */
#define VIEW_STATE 0xd10u  /* FViewInfo: its FSceneViewState */
#define VIEW_AAM_B 0xb0cu  /* FViewInfo: FinalPostProcessSettings.AntiAliasingMethod (a byte; 2 TAA) */
#define VIEW_PREV 0x17f0u  /* FViewInfo: PrevViewMatrices (the projection first; its jitter at +0x174) */
#define VMAT_JITTER 0x174u /* FViewMatrices: the TAA jitter added to M[2][0], M[2][1] (NDC) */
#define VS_TAA_PENDING 0x670u /* FSceneViewState: PendingTemporalAAHistoryRT */
#define VS_TAA_HISTORY 0x668u /* FSceneViewState: TemporalAAHistoryRT */
static struct {
  u32 drawn, skipped, noaa;
  /* the previous frame's matrices each view's TAA reprojects with, against what the agent saw that
   * view made with the frame before (views 0-3): the eyes a check of the check */
  float last[4][16];
  u32 have[4];
  u32 frames[2], moved[2], ok[2], reset[2], other[2];
  float move_px[2], worst_px[2];
} g_tc;
static volatile u32 g_said_taa;
/* the view's TAA output this frame (its state's history for the next), or 0 */
static u64 taa_output(u64 view) {
  u64 st = *(const u64 *)(view + VIEW_STATE);
  if (!st)
    return 0;
  u64 rt = *(const u64 *)(st + VS_TAA_PENDING);
  if (!rt)
    rt = *(const u64 *)(st + VS_TAA_HISTORY);
  return rt && *(const u64 *)(rt + 8) && *(const u64 *)(rt + 0x10) ? rt : 0;
}
/* the previous frame's projection the view's TAA uses against the one the agent saw it made with
 * then (each axis in pixels of the view's rect); called after the view's TAA (its own matrices
 * without jitter by then) */
#ifndef RELEASE
static void taa_matrices(u64 v, int k) {
  const float *m = (const float *)(v + 0x70), *prev = (const float *)(v + VIEW_PREV);
  const float *pj = (const float *)(v + VIEW_PREV + VMAT_JITTER);
  /* (both without their own jitter: by the tonemapper the engine has not always taken the
   * current one out, logs/aa1-hold: the eyes' projections moved by up to half a pixel a frame) */
  float cur[16];
  memcpy(cur, m, 64);
  cur[8] -= jitter(v, 0x1e4), cur[9] -= jitter(v, 0x1e8);
  const s32 *r = (const s32 *)(v + 0x38);
  float hw = (float)(r[2] - r[0]) * 0.5f, hh = (float)(r[3] - r[1]) * 0.5f;
  if (!(cur[11] > 0.5f && cur[11] < 1.5f) || hw < 8.0f || hh < 8.0f)
    return;
  int g = k >= 2;
  if (g_tc.have[k]) {
    const float *last = g_tc.last[k];
    float p8 = prev[8] - pj[0], p9 = prev[9] - pj[1];
    float mv = __builtin_fabsf(cur[8] - last[8]) * hw, mvy = __builtin_fabsf(cur[9] - last[9]) * hh;
    mv = mvy > mv ? mvy : mv;
    float el = __builtin_fabsf(p8 - last[8]) * hw, ely = __builtin_fabsf(p9 - last[9]) * hh;
    el = ely > el ? ely : el;
    el += (__builtin_fabsf(prev[0] - last[0]) + __builtin_fabsf(prev[5] - last[5])) * hw; /* (the scale: the same window size) */
    float ec = __builtin_fabsf(p8 - cur[8]) * hw, ecy = __builtin_fabsf(p9 - cur[9]) * hh;
    ec = ecy > ec ? ecy : ec;
    g_tc.frames[g]++;
    if (mv > 0.25f) { /* (the window, or the eye's projection, moved this frame) */
      g_tc.moved[g]++, g_tc.move_px[g] += mv;
      if (el < 0.05f)
        g_tc.ok[g]++;
      else if (ec < 0.05f)
        g_tc.reset[g]++;
      else
        g_tc.other[g]++;
      if (el > g_tc.worst_px[g])
        g_tc.worst_px[g] = el;
    }
  }
  memcpy(g_tc.last[k], cur, 64);
  g_tc.have[k] = 1;
}
#endif
static void inset_taa_band(u64 ctx, int k, const struct pp_views *pv) {
  u64 v = pv->v[k], eye = pv->v[k - 2];
  if (*(const u8 *)(v + VIEW_AAM_B) != 2 || *(const u8 *)(eye + VIEW_AAM_B) != 2) {
    g_tc.noaa++;
    return;
  }
  u64 cl = *(const u64 *)(ctx + 0x28);
  u64 dst = taa_output(v), src = taa_output(eye), sc = g_e.scene_ctx(cl);
  struct pp_map m;
  if (!dst || !src || dst == src || !sc || !map_of(eye, v, 1, &m)) {
    g_tc.skipped++;
    return;
  }
  s32 bw = *(const s32 *)(sc + 0x210), bh = *(const s32 *)(sc + 0x214);
  const s32 *ir = m.ir;
  if (bw < 64 || bh < 64 || ir[0] < 0 || ir[1] < 0 || ir[2] > bw || ir[3] > bh) {
    g_tc.skipped++;
    return;
  }
  s32 X0 = ir[0] - TAA_BAND, Y0 = ir[1] - TAA_BAND, X1 = ir[2] + TAA_BAND, Y1 = ir[3] + TAA_BAND;
  X0 = X0 < 0 ? 0 : X0, Y0 = Y0 < 0 ? 0 : Y0, X1 = X1 > bw ? bw : X1, Y1 = Y1 > bh ? bh : Y1;
  /* above, below (the full width), left, right (the rect's rows) */
  const s32 band[4][4] = {{X0, Y0, X1, ir[1]}, {X0, ir[3], X1, Y1}, {X0, ir[1], ir[0], ir[3]},
                          {ir[2], ir[1], X1, ir[3]}};
  u32 level = *(const u32 *)(ctx + 0x40), n = 0;
  for (int b = 0; b < 4; b++) {
    const s32 *q = band[b];
    if (q[2] <= q[0] || q[3] <= q[1])
      continue;
    double u0 = to_eye(&m, 0, q[0]), u1 = to_eye(&m, 0, q[2]), v0 = to_eye(&m, 1, q[1]), v1 = to_eye(&m, 1, q[3]);
    struct pp_draw dr = {*(const u64 *)(dst + 8), *(const u64 *)(dst + 0x10), *(const u64 *)(src + 0x10),
                         q[0], q[1], q[2], q[3], bw, bh, (float)u0, (float)v0, (float)(u1 - u0),
                         (float)(v1 - v0), 0};
    draw(cl, level, &dr);
    n++;
  }
  g_tc.drawn++;
  if (g_said_taa < 2 && ++g_said_taa)
    LOG("insettaa: inset %d's TAA history (0x%lx) round its rect %d,%d-%d,%d from its eye's TAA output "
        "(0x%lx): %u bands within %d,%d-%d,%d (s %.4f %.4f, c %.4f %.4f)", k - 2, dst, ir[0], ir[1],
        ir[2], ir[3], src, n, X0, Y0, X1, Y1, (double)m.s[0], (double)m.s[1], (double)m.c[0], (double)m.c[1]);
}

/* ---- Our TAA: the engine's main TAA pixel shader, patched (agent.taa) ----
 * The engine's TAA (FPostProcessTemporalAAPS<1,0>, read from the game's memory with agent.taadump
 * and tools/gcn7dis.py, docs/MULTIVIEW.md 30) blends a fixed 4% of the new frame into its history
 * whatever the motion, after a min/max clamp of the history to the neighbourhood: what the clamp
 * lets through trails for ~25 frames, and a moving picture is resampled through the history
 * filter frame after frame (blur). The same shader draws every view (both eyes, both insets),
 * so a patch to it changes all four alike. tools/gen_taa_patch.py makes agent/taa_patch.h from the
 * shader as dumped: a branch at the place the history's position is known to code added after
 * the shader's end that puts a per-pixel weight in v64, and the four blends taking it instead of
 * 0.04. The RHI shader object (+0x30) holds the shader's CPU block: its code at +0xa4, its
 * Gnm::PsShader at +0x34 with the stage registers at +0x3c (PGM_LO, PGM_HI, RSRC1, ...), which the
 * RHI hands the GPU at each bind. The agent copies the stock code, patches the copy in memory of
 * its own (CPU and GPU, the reserve), and points PGM_LO/HI at it (the larger VGPR and SGPR counts
 * of the two codes first, a frame before the new code; the new code's own a frame after). The
 * stock code stays where it is, untouched.
 *   agent.taa: 0 the engine's shader, 1 its copy unchanged (the relocation alone), 2 the
 *   motion-weighted blend (0.04 still, up to 0.2 at 40 pixels a frame), 3 no history (a test:
 *   the picture aliases if the patch is in), 4 the velocity buffer left out (the camera's
 *   reprojection for every pixel), 5 2 and 4, 6 a picture of the velocity buffer's error (where a
 *   pixel takes its velocity: red and green its x and y less the camera's reprojection's, x 20),
 *   7 the velocity fixed (the window's move added back: tools/gen_taa_patch.py), 8 2 and 7, 9 7's fix
 *   the other way (a check), 10 a probe of one pixel's numbers, 11 the velocity fixed with each
 *   view's last projection kept on the GPU (7 took it from a wrong place; the insets only, from a
 *   slot that holds the same view's frame before), 12 2 and 11, 13 11 with newly uncovered pixels
 *   converged as a running average (their weight in the history's alpha: 1, 1/2, 1/3 ... down to
 *   0.04; tools/gen_taa_patch.py), 14 13 with those pixels started from a soft picture of the
 *   current frame (a 3x3 tent, in the shader's own space) at a weight of 1/4, 15 the same at 1/8:
 *   no first frame of one jitter's aliasing, the detail coming in over several frames instead of
 *   at once; live. */
#include "taa_patch.h"
_Static_assert(TAA_NVAR == AGENT_TAA_N, "agent.h's AGENT_TAA_N (the options, the menu) is taa_patch.h's TAA_NVAR");
_Static_assert(TAA_NBASE == 2, "taa_patch.h's bases: Hikari 1.00's PS10, the newer engine's PS40");
static int plausible(u64 p) { return p >= 0x100000000ull && p < 0x1000000000ull && !(p & 7); }
volatile u32 g_opt_taa;
/* the shader patched, by build: Hikari 1.00's FPostProcessTemporalAAPS10 (the main TAA at AA
 * quality 4-6, inset mode's 6); the newer engine's PS40 (Chisato, Allison, Hikari 1.02), the
 * "faster" TAA it takes at AA quality 3 exactly (0xca4a7a), which inset mode caps them at (their
 * PS10 shimmers at 4+); each static type found by its name's registration (taa_patch.h's base 0
 * and 1; Hikari 1.02's PS40 taken as Chisato's code, its FNV checked before anything is written) */
static const u32 k_taa_type[BUILD_N + 1] = {0, 0x49f0430u, 0x4a94af0u, 0x4a94af0u, 0x49e09b0u};
static struct {
  u32 *regs;     /* the PsShader's PGM_LO, PGM_HI, RSRC1 */
  u32 lo0, hi0, rsrc0;
  u8 *buf;       /* the variants, 0x1000 each (variant v at buf + 0x1000 v), then the views' slots' page and the probe's */
  const struct taa_base *base; /* the shader's tables (by build) */
  u32 cur, ok, failed, tries;
  u32 asked;        /* the variant asked for last frame (a velocity fix starts a frame after it is asked) */
  u32 isize, wsize; /* PS40's 13: the inset's size now (width << 16 | height) and the one its weights were cleared for */
  u16 rect_at[TAA_NVAR + 1][2]; /* by variant: where its copy holds the eyes' height and the left eye's right edge (0: none; 10 on, or built as one) */
} g_taa;
/* variant v's patches and registers for the base in use: another variant's where the base has no
 * code of its own for it (taa_patch.h's alias: the newer engine's PS40 has the copy, the
 * no-history and no-velocity tests, the probe, the velocity fix 11, its ring form 7 and 13 with
 * the weights in the agent's memory; 2 and 6 are the copy, 5 is 4, 8 and 9 are 7, 12 is 11, 14
 * and 15 are 13) */
static const struct taa_variant *taa_var(u32 v) {
  const struct taa_variant *tv = &g_taa.base->var[v >= 1 && v <= TAA_NVAR ? v : 0];
  return tv->alias ? &g_taa.base->var[tv->alias] : tv;
}
#define TAA_SLOTS_PAGE (TAA_NVAR + 1) /* the views' last projections (the ring: 11-15 on PS10, 7 on PS40), at +0x200 */
#define TAA_PROBE_PAGE (TAA_NVAR + 2) /* variant 10's numbers */
static u32 fnv1a(const u8 *p, u32 n) {
  u32 h = 0x811c9dc5u;
  for (u32 i = 0; i < n; i++)
    h = (h ^ p[i]) * 0x01000193u;
  return h;
}
/* the patched code of a variant into dst (the stock code at src): the variant's patches over it */
static float g_taa_probe_px[6] = {9.0f, 9.0f, 0.0f, 0.0f, 1e9f, 1e9f}; /* the probe's pixel (variant 10): its screen place, the
                                                        * tolerance; then the eyes' height and the left eye's right edge
                                                        * (10 on: which view, from its viewport's centre) */
static void taa_build(u8 *dst, const u8 *src, u32 v) {
  const struct taa_variant *tv = taa_var(v);
  memset(dst, 0, 0x1000); /* (the placeholders are looked for over the whole page: nothing else in it) */
  memcpy(dst, src, g_taa.base->code_len);
  for (u32 i = 0; v >= 1 && v <= TAA_NVAR && i < tv->n; i++)
    memcpy(dst + tv->p[i].at, tv->p[i].bytes, tv->p[i].len);
  u32 built = v >= 1 && v <= TAA_NVAR && g_taa.base->var[v].alias ? g_taa.base->var[v].alias : v; /* (the variant whose code this is) */
  /* the placeholders (none in the stock code or the plain variants: the scan finds nothing): the
   * probe's memory (10: its numbers, a page; the velocity fix's ring, 11-15 on PS10, 7 on PS40:
   * each view's last projection, two slots a view, at +0x200 of the slots'
   * page, their frame counter at +0x280), the probe's pixel */
  u64 probe = built == 10 ? (u64)(g_taa.buf + 0x1000 * TAA_PROBE_PAGE) : (u64)(g_taa.buf + 0x1000 * TAA_SLOTS_PAGE) + 0x200;
  u32 fill[8];
  fill[0] = (u32)probe, fill[1] = (u32)(probe >> 32) & 0xffff;
  memcpy(&fill[2], &g_taa_probe_px[0], 24);
  for (u32 o = 0; o + 4 <= 0x1000; o += 4)
    for (u32 k = 0; k < 8; k++)
      if (*(u32 *)(dst + o) == 0xd1a90001u + k) {
        *(u32 *)(dst + o) = fill[k];
        if (k >= 6) /* (kept: taa_rect_check sets them again if the eye's rect changes) */
          g_taa.rect_at[v][k - 6] = (u16)o;
      }
}
/* the eyes' height and the left eye's right edge the copies tell the views apart by, against the
 * left eye's rect now: set again in variants 10 on if it changed (each a single 32-bit store into
 * the copy, in use or not: a view drawn meanwhile reads the old or the new number, never a torn
 * one) */
static void taa_rect_check(u64 eye0) {
  const s32 *r = (const s32 *)(eye0 + 0x38);
  float y1 = (float)r[3], x1 = (float)r[2];
  if (y1 == g_taa_probe_px[4] && x1 == g_taa_probe_px[5])
    return;
  static u32 said;
  if (said < 8 && ++said) /* (a few: a rect that changes every frame would fill the log) */
    LOG("taa: the left eye's rect %d,%d-%d,%d: the views told apart by height %.0f and right edge %.0f "
        "(were %.0f, %.0f)", r[0], r[1], r[2], r[3], (double)y1, (double)x1, (double)g_taa_probe_px[4],
        (double)g_taa_probe_px[5]);
  g_taa_probe_px[4] = y1, g_taa_probe_px[5] = x1;
  for (u32 v = 1; v <= TAA_NVAR; v++)
    for (u32 k = 0; k < 2; k++)
      if (g_taa.rect_at[v][k]) {
        u32 bits;
        memcpy(&bits, &g_taa_probe_px[4 + k], 4);
        __atomic_store_n((u32 *)(g_taa.buf + 0x1000 * v + g_taa.rect_at[v][k]), bits, __ATOMIC_RELEASE);
      }
}
static int taa_init(u64 map, u64 inset0, u64 eye0) {
  if (eye0) { /* (the views told apart by their viewport's centre: the left eye's height and right edge) */
    const s32 *r = (const s32 *)(eye0 + 0x38);
    g_taa_probe_px[4] = (float)r[3], g_taa_probe_px[5] = (float)r[2];
  }
  if (inset0) { /* the probe's pixel (variant 10): the left inset's centre less 105, 60 (the cafe's
                 * calendar's "fire" card in the 1:1 captures), its centre */
    const s32 *r = (const s32 *)(inset0 + 0x38);
    float hw = (float)(r[2] - r[0]) * 0.5f, hh = (float)(r[3] - r[1]) * 0.5f;
    /* (the screen place, x right and y up, from -1 to 1 over the view: within 0.6 of a pixel) */
    g_taa_probe_px[0] = -105.0f / hw, g_taa_probe_px[1] = 60.0f / hh;
    g_taa_probe_px[2] = 0.6f / hw, g_taa_probe_px[3] = 0.6f / hh;
  }
  u32 b = astro_hdr.build;
  const struct taa_base *base = &k_taa_base[build_newer_engine()];
  u64 sh = b >= 1 && b <= BUILD_N ? global_shader(map, g_e.base + k_taa_type[b]) : 0;
  u64 res = sh && plausible(sh) ? *(const u64 *)(sh + 0x48) : 0;
  u64 rhi = res && plausible(res) ? *(const u64 *)(res + 0x40) : 0;
  u64 blk = rhi && plausible(rhi) ? *(const u64 *)(rhi + 0x30) : 0;
  if (!plausible(blk - 0x10)) {
    if (g_taa.tries == 0 || g_taa.tries == 600) /* (the first try and the last: tried each frame till then) */
      LOG("taa: the engine's TAA shader not found (shader 0x%lx, resource 0x%lx, RHI 0x%lx, block 0x%lx)%s", sh,
          res, rhi, blk, g_taa.tries ? ": left alone" : "");
    return 0;
  }
  u32 *regs = (u32 *)(blk + TAA_PSSHADER_OFF + 8);
  const u8 *code = (const u8 *)(blk + base->code_off);
  u32 h = fnv1a(code, base->code_len);
  if (regs[2] != base->rsrc1 || regs[3] != 0x1c || regs[5] != 4 || regs[6] != base->inputs || h != base->fnv ||
      regs[1] > 0xff) {
    LOG("taa: the engine's TAA shader is not the one the patch is for (RSRC1 0x%x RSRC2 0x%x colour %u "
        "inputs 0x%x, code FNV 0x%08x, PGM 0x%x 0x%x; %s: RSRC1 0x%x inputs 0x%x FNV 0x%08x): left alone",
        regs[2], regs[3], regs[5], regs[6], h, regs[0], regs[1], base->name, base->rsrc1, base->inputs, base->fnv);
    return -1;
  }
  /* (the variants, then the views' slots' page and the probe's; on the newer engine also its 13's
   * weights, right after: the shader's V# from the slots' page + 0x200 reaches them at +0x1e00) */
  u64 wsize = build_newer_engine() ? TAA_WEIGHTS40 : 0;
  _Static_assert(0x1000 * (TAA_NVAR + 1) + 0x200 + TAA_WEIGHTS_OFF40 == 0x1000 * (TAA_NVAR + 3), "base 1's weights follow the probe's page");
  u8 *buf = g_e.dalloc(0x1000 * (TAA_NVAR + 3) + wsize);
  if (!buf || ((u64)buf & 0xff)) {
    LOG("taa: no memory for the shader's copies");
    return -1;
  }
  g_taa.buf = buf, g_taa.base = base;
  memset(buf + 0x1000 * TAA_SLOTS_PAGE, 0, 0x2000 + wsize); /* (the views' slots, none from the frame before yet, the probe's page, the weights: 0 reads as the engine's own) */
  for (u32 v = 1; v <= TAA_NVAR; v++)
    taa_build(buf + 0x1000 * v, code, v);
  g_taa.regs = regs, g_taa.lo0 = regs[0], g_taa.hi0 = regs[1], g_taa.rsrc0 = regs[2];
  char aliases[160]; /* (taa_patch.h's: the variants this base builds as another) */
  u32 ao = 0;
  aliases[0] = 0;
  for (u32 v = 1; v <= TAA_NVAR; v++)
    if (base->var[v].alias && ao < sizeof aliases - 12)
      ao += (u32)agent_snprintf(aliases + ao, sizeof aliases - ao, " %u as %u", v, base->var[v].alias);
  if (!ao)
    agent_snprintf(aliases, sizeof aliases, " none");
  LOG("taa: the engine's TAA shader, %s (block 0x%lx, code at 0x%lx, FNV 0x%08x): copies at %p (1 as is, 2 "
      "the motion-weighted blend, 3 no history, 4 the velocity buffer left out, 5 2 and 4, 6 a picture "
      "of the velocity's error, 7 the velocity fixed, 8 2 and 7, 9 7 reversed, 10 a probe, 11 the velocity fixed "
      "(2), 12 2 and 11, 13 11 with new pixels converged quickly, 14 13 with them started soft at 1/4, 15 at 1/8; built as another here:%s)",
      base->name, blk, (u64)regs[0] << 8, h, buf,
      aliases);
  return 1;
}
#ifndef RELEASE
/* Chisato's and Allison's probe (variant 10 of their shader): the left inset's view uniform
 * buffer, its first 960 dwords as the GPU read them last frame (the probe's page; +0xf00 the
 * ViewToClip M[2][0..1], ScreenPosToPixel and the storing pixel's place), against the frame
 * before's: the dwords that changed, with the CPU's own M[2][0..1] of the view, for the first
 * frames (which dword counts frames, which holds the previous projection); the whole buffer
 * now and then */
static void taa_probe40(u64 inset0) {
  static u32 prev[960], frames;
  u32 n = frames++;
  if (!(n < 40 || (n + 1) % 300 <= 2)) /* (read only for the frames that log and the one before each) */
    return;
  const volatile u32 *pg = (const volatile u32 *)(g_taa.buf + 0x1000 * TAA_PROBE_PAGE);
  static u32 now[976]; /* (the render thread's stack spared) */
  for (u32 i = 0; i < 976; i++)
    now[i] = pg[i];
  const float *m = (const float *)(inset0 + 0x70);
  float f[8];
  memcpy(f, now + 960, 32);
  if (n < 40 || n % 300 == 0) {
    char line[480];
    u32 o = 0, k = 0;
    line[0] = 0;
    for (u32 i = 0; i < 960; i++) /* (every change counted; listed while the line has room) */
      if (now[i] != prev[i]) {
        float v;
        memcpy(&v, &now[i], 4);
        if (k++ < 24 && o < sizeof line - 48)
          o += (u32)agent_snprintf(line + o, sizeof line - o, " %u:%08x>%08x(%.5g)", i, prev[i], now[i], (double)v);
      }
    LOG("taaprobe40: frame %u: %u dwords changed:%s", n, k, line);
    LOG("taaprobe40: GPU M20 %.6f M21 %.6f, ScreenPosToPixel %.1f %.1f %.1f %.1f, at %.4f %.4f; CPU now M20 %.6f M21 %.6f; "
        "the history's T# %08x %08x %08x %08x %08x %08x %08x %08x (data format %u)",
        (double)f[0], (double)f[1], (double)f[2], (double)f[3], (double)f[4], (double)f[5], (double)f[6], (double)f[7],
        (double)m[8], (double)m[9], now[968], now[969], now[970], now[971], now[972], now[973], now[974], now[975],
        (now[969] >> 20) & 0x3f);
  }
  if (n % 300 == 1)
    for (u32 i = 0; i < 960; i += 32) {
      char line[400];
      u32 o = 0;
      for (u32 j = 0; j < 32; j++)
        o += (u32)agent_snprintf(line + o, sizeof line - o, " %08x", now[i + j]);
      LOG("taaprobe40: view buffer dwords %u-%u:%s", i, i + 31, line);
    }
  memcpy(prev, now, sizeof prev);
}
#endif
/* the variant the engine's TAA shader runs now (0: its own, or ours not there) */
u32 taa_active(void) { return g_taa.ok && !g_taa.failed ? g_taa.cur : 0; }
/* the inset window in the velocity fix's 2-pixel steps: while a fix variant is asked for (from the
 * moment it is, once ours is found: the projections are made a frame ahead of the shader's switch)
 * or still running; not while the engine's own runs (ours refused or not found yet) */
int taa_snap(void) { return taa_velfix(taa_active()) || (g_taa.ok && !g_taa.failed && taa_velfix(g_opt_taa)); }
/* once a frame (the left eye's tonemapper, render thread): towards agent.taa, a step a frame */
static void taa_step(u64 map, u64 inset0, u64 eye0) {
  u32 want = g_opt_taa > TAA_NVAR ? 0 : g_opt_taa;
  if (want >= 13 && build_newer_engine() && inset0) {
    /* PS40's 13 (and 14, 15 built as it): an inset larger than a plane of its weights (2x2 blocks)
     * runs 11, the velocity fix without them; one whose size changed while 13 runs has its weights
     * cleared (they are laid out by its width) */
    const s32 *r = (const s32 *)(inset0 + 0x38);
    if (r[2] > r[0] && r[3] > r[1]) /* (a rect that isn't one leaves the size as it was) */
      g_taa.isize = (u32)(r[2] - r[0]) << 16 | (u32)(r[3] - r[1]);
    if (g_taa.ok && g_taa.cur && taa_var(g_taa.cur) == &g_taa.base->var[13] && g_taa.wsize != g_taa.isize) {
      memset(g_taa.buf + 0x1000 * (TAA_NVAR + 3), 0, TAA_WEIGHTS40); /* (a frame in flight may still write
                                    * a few of the old layout's: they fade as any weight does) */
      g_taa.wsize = g_taa.isize;
    }
    u64 blocks = (u64)((r[2] - r[0] + 1) / 2) * (u64)((r[3] - r[1] + 1) / 2);
    if (r[2] > r[0] && r[3] > r[1] && blocks > TAA_WEIGHTS_PLANE40) {
      static u32 said;
      if (!said++)
        LOG("taa: the inset %dx%d is larger than 13's weights hold (%lu 2x2 blocks, %u a plane): 11 in its place",
            r[2] - r[0], r[3] - r[1], blocks, (u32)TAA_WEIGHTS_PLANE40);
      want = 11;
    }
  }
  if (g_taa.failed || (!g_taa.ok && !want))
    return;
  if (!g_taa.ok) {
    int r = taa_init(map, inset0, eye0);
    if (r < 0 || (r == 0 && ++g_taa.tries > 600))
      g_taa.failed = 1;
    if (r <= 0)
      return;
    g_taa.ok = 1;
    return; /* (the first switch a frame later: the window's 2-pixel steps, taa_snap, start a frame before it) */
  }
  if (eye0)
    taa_rect_check(eye0);
  u32 *regs = g_taa.regs;
#ifndef RELEASE
  if (g_taa.cur == 10 && inset0 && !build_newer_engine()) { /* the probe's numbers (the newest frame the GPU
                                    * drew), beside the left inset's own projections as the CPU has them now */
    static u32 n;
    const volatile float *pr = (const volatile float *)(g_taa.buf + 0x1000 * TAA_PROBE_PAGE);
    const float *m = (const float *)(inset0 + 0x70), *pm = (const float *)(inset0 + 0x17f0);
    const float *j = (const float *)(inset0 + 0x1e4), *pj = (const float *)(inset0 + 0x17f0 + 0x174);
    if (n++ % 6 == 0)
      LOG("taaprobe: velocity x raw %.6f (decoded %.6f) y %.6f, camera %.6f %.6f, uses velocity %.0f, at "
          "%.1f %.1f; ViewToClip M20 %.6f M21 %.6f, PrevViewToClip %.6f %.6f | CPU now: M20 %.6f M21 %.6f "
          "(jitter %.6f %.6f), prev %.6f %.6f (jitter %.6f %.6f); s40-43 %.2f %.2f %.2f %.2f", (double)pr[0],
          (double)(pr[0] * 4.008016f - 2.004008f), (double)pr[1], (double)pr[2], (double)pr[3], (double)pr[4],
          (double)pr[5], (double)pr[6], (double)pr[7], (double)pr[8], (double)pr[9], (double)pr[10], (double)m[8],
          (double)m[9], (double)j[0], (double)j[1], (double)pm[8], (double)pm[9], (double)pj[0], (double)pj[1],
          (double)pr[11], (double)pr[12], (double)pr[13], (double)pr[14]);
  }
  if (g_taa.cur == 10 && inset0 && build_newer_engine())
    taa_probe40(inset0);
#endif
  /* into a velocity-fix variant: a frame later than asked, so the window's 2-pixel steps (taa_snap,
   * from the request) are in the projections the fix's first frame draws */
  if (taa_velfix(want) && !taa_velfix(g_taa.cur) && g_taa.asked != want) {
    g_taa.asked = want;
    return;
  }
  g_taa.asked = want;
  /* the VGPRs the code in use and the code wanted need: the more of them while either runs */
  u32 rs_want = want ? taa_var(want)->rsrc1 : g_taa.rsrc0;
  u32 rs_cur = g_taa.cur ? taa_var(g_taa.cur)->rsrc1 : g_taa.rsrc0;
  if (want == g_taa.cur) {
    if (regs[2] != rs_want) /* (a frame after the code changed: its own count) */
      regs[2] = rs_want;
    return;
  }
  /* (the larger of each count, VGPRs bits 0-5 and SGPRs 6-9, a frame before the code needing it) */
  u32 vg = (rs_want & 0x3f) > (rs_cur & 0x3f) ? rs_want & 0x3f : rs_cur & 0x3f;
  u32 sg = (rs_want & 0x3c0) > (rs_cur & 0x3c0) ? rs_want & 0x3c0 : rs_cur & 0x3c0;
  u32 rs_both = (rs_want & ~0x3ffu) | sg | vg;
  if ((regs[2] & 0x3f) < vg || (regs[2] & 0x3c0) < sg) {
    regs[2] = rs_both;
    return;
  }
  u64 at = want ? (u64)(g_taa.buf + 0x1000 * want) : ((u64)g_taa.hi0 << 40 | (u64)g_taa.lo0 << 8);
  /* PS40's 13 (14, 15 as it) coming in: its weights cleared first (what a time before left there,
   * laid out for another inset size, reads as the engine's own weight); nothing else uses them */
  const struct taa_variant *q13 = &g_taa.base->var[13];
  if (build_newer_engine() && want && taa_var(want) == q13 && (!g_taa.cur || taa_var(g_taa.cur) != q13))
    memset(g_taa.buf + 0x1000 * (TAA_NVAR + 3), 0, TAA_WEIGHTS40), g_taa.wsize = g_taa.isize;
  __atomic_store_n(&regs[1], (u32)(at >> 40), __ATOMIC_RELEASE);
  __atomic_store_n(&regs[0], (u32)(at >> 8), __ATOMIC_RELEASE);
  LOG("taa: agent.taa %u -> %u (the shader's code at 0x%lx)", g_taa.cur, want, at);
  g_taa.cur = want;
}

#ifndef RELEASE
/* agent.taadump=1 (a debug tool): the engine's main TAA pixel shaders (FPostProcessTemporalAAPS
 * <1,0> and <1,1>: the static types 0x49f0430, 0x49f0590, "MainTemporalAAPS", registered at
 * 0xca5e76/0xca5ed9; all three builds, k_taa_types) into the log once: each FShader's and its resource's first
 * bytes, the resource's RHI pixel shader object's, and the bytecode (the resource's first TArray
 * of bytes that looks like one) in base64, "taadump <shader> <line> <base64>", a few lines a
 * frame. To read the shader itself: where the history is taken, what its off-screen test is
 * against, its clamp, for our own TAA. */
volatile u32 g_opt_taadump;
/* FPostProcessTemporalAAPS10, 11, 20, 30, 40, 00 by build (Hikari, Chisato, Allison, Hikari 1.02: each type found
 * from its name's registration): the main TAA and its responsive variant, types 2 and 3, then 40,
 * the "faster" main TAA FRCPassPostProcessTemporalAA::Process takes at r.PostProcessAAQuality 3
 * exactly (0xca4a7a: cmp $3, sete; 10 at 4-6), and 00 */
#define TD_N 6
static const u32 k_taa_types[BUILD_N][TD_N] = {{0x49f0430u, 0x49f0590u, 0x49f06f0u, 0x49f0850u, 0x49f09b0u, 0x49f02d0u},
                                         {0x4a94570u, 0x4a946d0u, 0x4a94830u, 0x4a94990u, 0x4a94af0u, 0x4a94410u},
                                         {0x4a94570u, 0x4a946d0u, 0x4a94830u, 0x4a94990u, 0x4a94af0u, 0x4a94410u},
                                         {0x49e0430u, 0x49e0590u, 0x49e06f0u, 0x49e0850u, 0x49e09b0u, 0x49e02d0u}};
static struct {
  u32 started, shader, pos, len;
  const u8 *code;
} g_td;
static void td_b64(char *out, const u8 *in, u32 n) {
  static const char k[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  u32 o = 0;
  for (u32 i = 0; i < n; i += 3) {
    u32 v = (u32)in[i] << 16 | (i + 1 < n ? (u32)in[i + 1] << 8 : 0) | (i + 2 < n ? in[i + 2] : 0);
    out[o++] = k[v >> 18], out[o++] = k[(v >> 12) & 63];
    out[o++] = i + 1 < n ? k[(v >> 6) & 63] : '=';
    out[o++] = i + 2 < n ? k[v & 63] : '=';
  }
  out[o] = 0;
}
static void td_hex(const char *what, u32 s, u64 p, u32 n) {
  char b[4 * 0x40 + 8];
  for (u32 off = 0; off < n; off += 0x40) {
    u32 o = 0;
    for (u32 i = 0; i < 0x40 / 8; i++)
      o += (u32)agent_snprintf(b + o, sizeof b - o, " %016lx", *(const u64 *)(p + off + 8 * i));
    LOG("taadump %u %s +0x%03x:%s", s, what, off, b);
  }
}
/* the next shader's dump started, or a few lines of the one under way */
static void taa_dump_step(u64 map) {
  if (!g_opt_taadump || astro_hdr.build < 1 || astro_hdr.build > BUILD_N)
    return;
  if (!g_td.len) {
    if (g_td.started >= TD_N)
      return;
    u32 s = g_td.started++;
    u32 type = k_taa_types[astro_hdr.build - 1][s];
    u64 sh = global_shader(map, g_e.base + type);
    LOG("taadump %u: FPostProcessTemporalAAPS (type 0x%x, build %u) FShader 0x%lx", s, type, astro_hdr.build, sh);
    if (!plausible(sh))
      return;
    td_hex("shader", s, sh, 0x100);
    u64 res = *(const u64 *)(sh + 0x48);
    if (!plausible(res))
      return;
    td_hex("resource", s, res, 0xc0);
    u64 rhi = *(const u64 *)(res + 0x40);
    if (plausible(rhi)) {
      td_hex("rhi", s, rhi, 0x100);
      /* the GCN code (aa2: the bytecode is freed once the RHI shader is made; the resource says
       * 183 instructions). The RHI object's +0x30 looks like a Gnm::PsShader: ShaderCommonData
       * (its first dword's low 23 bits the code's size), then the stage registers,
       * SPI_SHADER_PGM_LO_PS and _HI_PS (the code's GPU address >> 8) at +8 and +12 */
      u64 ps = *(const u64 *)(rhi + 0x30);
      if (plausible(ps - 0x10)) {
        td_hex("psshader", s, ps, 0x80);
        /* (aa3: not a bare Gnm::PsShader: its first 16 bytes a hash, "Shdr" at +0x24; the RHI
         * object's pointers all fall in one block from it, 0xac4 bytes on: the shader binary's CPU
         * copy, code and all, by the look of it). The block to the last pointer into it and
         * 0x200 more, from heap memory */
        u64 hi = ps;
        for (u32 off = 0x30; off < 0x100; off += 8) {
          u64 q = *(const u64 *)(rhi + off);
          if (q > hi && q < ps + 0x4000)
            hi = q;
        }
        u32 len = (u32)(hi - ps) + 0x200;
        len = len > 0x4000 ? 0x4000 : len;
        g_td.shader = s, g_td.pos = 0, g_td.len = len, g_td.code = (const u8 *)ps;
        LOG("taadump %u begin %u bytes (the RHI shader's block at 0x%lx)", s, len, ps);
        return;
      }
    }
    /* the bytecode: a TArray<uint8> (data, num, max) in the resource, num within max, 1-256 KB */
    for (u32 off = 0; off + 16 <= 0xc0; off += 8) {
      u64 data = *(const u64 *)(res + off);
      s32 num = *(const s32 *)(res + off + 8), max = *(const s32 *)(res + off + 12);
      if (plausible(data) && num >= 1024 && num <= 262144 && max >= num && max <= 2 * num + 4096) {
        g_td.shader = s, g_td.pos = 0, g_td.len = (u32)num, g_td.code = (const u8 *)data;
        LOG("taadump %u begin %d bytes (the resource's +0x%x)", s, num, off);
        break;
      }
    }
    return;
  }
  for (int i = 0; i < 8 && g_td.len; i++) {
    char line[225 / 3 * 4 + 1];
    u32 n = g_td.len - g_td.pos < 225 ? g_td.len - g_td.pos : 225;
    td_b64(line, g_td.code + g_td.pos, n);
    LOG("taadump %u %u %s", g_td.shader, g_td.pos / 225, line);
    g_td.pos += n;
    if (g_td.pos >= g_td.len) {
      LOG("taadump %u end", g_td.shader);
      g_td.len = 0;
    }
  }
}
#endif

/* FRCPassPostProcessTonemap::Process: the eyes' bloom at the insets' pixels into F before each
 * eye's runs; F as each inset's bloom while it runs; after an inset's, the band round its TAA
 * history */
static void tm_hook(u64 pass, u64 ctx, void (*orig)(u64, u64)) {
  u64 slot = 0, was = 0;
  int k = -1;
  if ((g_opt_insetpp & 1) && ctx && pass) {
    struct pp_views pv;
    k = which_view(*(const u64 *)ctx, &pv);
    if (k >= 0 && shaders_for(*(const u64 *)(ctx + 0x48))) {
      if (k < 2)
        eye_bloom(pass, ctx, k, &pv);
      else
        was = inset_bloom_swap(pass, k - 2, &pv, &slot);
    }
  } else if (g_f.rt) {
    release_f();
    g_n.released++;
  }
  g_n.calls++;
  orig(pass, ctx);
  if (slot)
    *(u64 *)slot = was; /* (before the graph lets its inputs go, after Process returns) */
  if (k == 3)
    release_f(); /* (recorded: the pool may hand it out again for later work) */
  if (ctx && pass) { /* (each view after its TAA: the four views of inset mode only) */
    struct pp_views pv;
    u64 view0 = *(const u64 *)ctx;
    int j = which_view(view0, &pv);
    if (view0 && *(const u32 *)(view0 + 0x420) == 1) /* (the left eye: our TAA, stock or inset mode) */
      taa_step(*(const u64 *)(ctx + 0x48), j == 0 ? pv.v[2] : 0, view0);
#ifndef RELEASE
    if (j == 0 || (j < 0 && *(const u64 *)(ctx + 0x48)))
      taa_dump_step(*(const u64 *)(ctx + 0x48));
#endif
    if (j >= 0) {
#ifndef RELEASE
      taa_matrices(pv.v[j], j); /* (a check, for the log) */
#endif
      if (j >= 2 && g_opt_insettaa && shaders_for(*(const u64 *)(ctx + 0x48)))
        inset_taa_band(ctx, j, &pv);
    }
  }
}
void mv_tm(u64 pass, u64 ctx) { tm_hook(pass, ctx, o_tm); }
void mv_tmf(u64 pass, u64 ctx) { tm_hook(pass, ctx, o_tmf); }

/* ---- AO ---- */
static struct {
  u32 done, skipped;
} g_ao;
static volatile u32 g_said_ao;
/* FRCPassPostProcessAmbientOcclusion::Process: after an inset's last AO pass, the eye's AO over
 * the inset's rect of the scene's AO texture */
void mv_ao(u64 pass, u64 ctx) {
  o_ao(pass, ctx);
  if (!(g_opt_insetpp & 2) || !pass || !ctx || *(const u8 *)(pass + AO_SETUP_AS_INPUT))
    return;
  struct pp_views pv;
  int k = which_view(*(const u64 *)ctx, &pv);
  if (k < 2 || !shaders_for(*(const u64 *)(ctx + 0x48)))
    return;
  u64 cl = *(const u64 *)(ctx + 0x28);
  u64 sc = g_e.scene_ctx(cl);
  u64 ao = sc ? *(const u64 *)(sc + 0xd8) : 0;
  struct pp_map m;
  if (!ao || !*(const u64 *)(ao + 8) || !*(const u64 *)(ao + 0x10) || !map_of(pv.v[k - 2], pv.v[k], 0, &m)) {
    g_ao.skipped++;
    return;
  }
  s32 b[2] = {*(const s32 *)(sc + 0x210), *(const s32 *)(sc + 0x214)};
  s32 x0 = m.ir[0] < 0 ? 0 : m.ir[0], y0 = m.ir[1] < 0 ? 0 : m.ir[1];
  s32 x1 = m.ir[2] > b[0] ? b[0] : m.ir[2], y1 = m.ir[3] > b[1] ? b[1] : m.ir[3];
  if (x1 - x0 < 16 || y1 - y0 < 16) {
    g_ao.skipped++;
    return;
  }
  /* the texture is the buffer's size: its texels are the buffer's pixels */
  double u0 = to_eye(&m, 0, x0), u1 = to_eye(&m, 0, x1), v0 = to_eye(&m, 1, y0), v1 = to_eye(&m, 1, y1);
  struct pp_draw dr = {*(const u64 *)(ao + 8), *(const u64 *)(ao + 0x10), *(const u64 *)(ao + 0x10),
                       x0, y0, x1, y1, b[0], b[1], (float)u0, (float)v0, (float)(u1 - u0),
                       (float)(v1 - v0), 0};
  draw(cl, *(const u32 *)(ctx + 0x40), &dr);
  g_ao.done++;
  if (g_said_ao < 2 && ++g_said_ao)
    LOG("insetpp: inset %d's AO from its eye's (a %dx%d texture): %d,%d-%d,%d from %.2f,%.2f "
        "+%.2fx%.2f", k - 2, b[0], b[1], x0, y0, x1, y1, u0, v0, u1 - u0, v1 - v0);
}

/* ---- reflections (screen-space: r.SSR.Quality above 0; the game's own is 0) ----
 * Each view traces its own (FRCPassPostProcessScreenSpaceReflections, 0xd4da30, from
 * ScreenSpaceReflections 0xd4f3a0 in the reflection composite's loop over the views, each with an
 * output of its own the buffer's size, its rect filled, PF_FloatRGBA), so an inset reflected only
 * what its own small window held: things outside it missing or fading at its edge, where the eye
 * around showed them. As the eye's AO: after an eye's trace, the block of its texels its inset's
 * pixels fall on (with a texel round it) is copied 1:1 into S, a small target of our own held per
 * eye; after the inset's trace, S is drawn over the inset's rect of its output, bilinear at each
 * pixel's direction, the jittered frame's (the reflections are made before the TAA, as AO): the
 * eye's own texels, sampled where its pixels lie. (Holding the eye's whole output for the inset
 * would keep two more 51 MB targets in the pool.) */
static const u16 k_ssr_name[] = {'I', 'n', 's', 'e', 't', 'E', 'y', 'e', 'S', 'S', 'R', 0};
static struct {
  u64 rt;       /* S: the eye's texels for its inset */
  s32 sw, sh;   /* S's size */
  s32 rx, ry;   /* the block's place in the eye's output */
  u64 inset;    /* the inset view it was made for (0: none this frame) */
  s32 fmt;
} g_ssr[2];
static struct {
  u32 eye, inset, skipped, fail;
} g_sn;
static volatile u32 g_said_ssr, g_said_ssr_fmt;
static s32 ifloor(double x) { /* (no libm in the agent) */
  s32 i = (s32)x;
  return x < (double)i ? i - 1 : i;
}
static u64 own_output(u64 pass) {
  return (*(u64 (**)(u64, u32))(*(const u64 *)pass + 0x40))(pass, 0);
}
static void ssr_release(int e) {
  if (!g_ssr[e].rt)
    return;
  u64 rt = g_ssr[e].rt;
  g_ssr[e].rt = 0, g_ssr[e].inset = 0;
  (*(void (**)(u64))(*(const u64 *)rt + 8))(rt);
}
/* the eye's trace done: the block its inset needs into S */
static void ssr_eye(u64 cl, u32 level, int e, const struct pp_views *pv, const u8 *desc, u64 out_rt) {
  struct pp_map m;
  g_ssr[e].inset = 0;
  if (!map_of(pv->v[e], pv->v[2 + e], 0, &m)) {
    g_sn.skipped++;
    return;
  }
  s32 b[2];
  ext_of(desc, b);
  s32 x0 = m.ir[0] < 0 ? 0 : m.ir[0], y0 = m.ir[1] < 0 ? 0 : m.ir[1];
  s32 x1 = m.ir[2] > b[0] ? b[0] : m.ir[2], y1 = m.ir[3] > b[1] ? b[1] : m.ir[3];
  double u0 = to_eye(&m, 0, x0), u1 = to_eye(&m, 0, x1), v0 = to_eye(&m, 1, y0), v1 = to_eye(&m, 1, y1);
  s32 rx0 = ifloor(u0) - 1, ry0 = ifloor(v0) - 1, rx1 = -ifloor(-u1) + 1, ry1 = -ifloor(-v1) + 1;
  rx0 = rx0 < 0 ? 0 : rx0, ry0 = ry0 < 0 ? 0 : ry0;
  rx1 = rx1 > b[0] ? b[0] : rx1, ry1 = ry1 > b[1] ? b[1] : ry1;
  s32 w = rx1 - rx0, h = ry1 - ry0, fmt = *(const s32 *)(desc + 0x2c);
  if (x1 - x0 < 16 || y1 - y0 < 16 || w < 4 || h < 4) {
    g_sn.skipped++;
    return;
  }
  /* S kept from frame to frame, its size rounded up to 64 (the block's moves by a texel as the
   * gaze does: the pool isn't asked for a new size every frame) */
  if (g_ssr[e].rt && (g_ssr[e].sw < w || g_ssr[e].sh < h || g_ssr[e].fmt != fmt))
    ssr_release(e);
  if (!g_ssr[e].rt) {
    u8 d[0x50];
    memcpy(d, desc, sizeof d);
    s32 sw = (w + 63) & ~63, sh = (h + 63) & ~63;
    *(s32 *)(d + 0x14) = sw, *(s32 *)(d + 0x18) = sh;
    *(const u16 **)(d + 0x40) = k_ssr_name;
    u64 rt = 0;
    g_e.find_free(g_e.base + G_POOL, cl, d, &rt, k_ssr_name, 1);
    if (!rt || !*(const u64 *)(rt + 8) || !*(const u64 *)(rt + 0x10)) {
      if (rt)
        (*(void (**)(u64))(*(const u64 *)rt + 8))(rt);
      g_sn.fail++;
      return;
    }
    g_ssr[e].rt = rt, g_ssr[e].sw = sw, g_ssr[e].sh = sh, g_ssr[e].fmt = fmt;
    if (g_said_ssr_fmt++ < 2)
      LOG("insetpp: S (eye %d's reflections for its inset) %dx%d in the eye's format %d (the block "
          "%dx%d of a %dx%d output)", e, sw, sh, fmt, w, h, b[0], b[1]);
  }
  /* 1:1: S's pixel (i, j) the eye's texel (rx0 + i, ry0 + j), point sampled at its centre */
  struct pp_draw dr = {*(const u64 *)(g_ssr[e].rt + 8), *(const u64 *)(g_ssr[e].rt + 0x10),
                       *(const u64 *)(out_rt + 0x10), 0, 0, w, h, b[0], b[1], (float)rx0, (float)ry0,
                       (float)w, (float)h, 1};
  draw(cl, level, &dr);
  g_ssr[e].rx = rx0, g_ssr[e].ry = ry0, g_ssr[e].inset = pv->v[2 + e];
  g_sn.eye++;
}
/* the inset's trace done: the eye's, from S, over its rect */
static void ssr_inset(u64 cl, u32 level, int k, const struct pp_views *pv, const u8 *desc, u64 out_rt) {
  int e = k - 2;
  struct pp_map m;
  if (!g_ssr[e].rt || g_ssr[e].inset != pv->v[k] || !map_of(pv->v[e], pv->v[k], 0, &m)) {
    g_sn.skipped++;
    return;
  }
  g_ssr[e].inset = 0;
  s32 b[2];
  ext_of(desc, b);
  s32 x0 = m.ir[0] < 0 ? 0 : m.ir[0], y0 = m.ir[1] < 0 ? 0 : m.ir[1];
  s32 x1 = m.ir[2] > b[0] ? b[0] : m.ir[2], y1 = m.ir[3] > b[1] ? b[1] : m.ir[3];
  double u0 = to_eye(&m, 0, x0) - g_ssr[e].rx, u1 = to_eye(&m, 0, x1) - g_ssr[e].rx;
  double v0 = to_eye(&m, 1, y0) - g_ssr[e].ry, v1 = to_eye(&m, 1, y1) - g_ssr[e].ry;
  struct pp_draw dr = {*(const u64 *)(out_rt + 8), *(const u64 *)(out_rt + 0x10), *(const u64 *)(g_ssr[e].rt + 0x10),
                       x0, y0, x1, y1, g_ssr[e].sw, g_ssr[e].sh, (float)u0, (float)v0,
                       (float)(u1 - u0), (float)(v1 - v0), 0};
  draw(cl, level, &dr);
  g_sn.inset++;
  if (g_said_ssr < 2 && ++g_said_ssr)
    LOG("insetpp: inset %d's reflections from its eye's: %d,%d-%d,%d from the eye's %.2f,%.2f +%.2fx%.2f",
        e, x0, y0, x1, y1, u0 + g_ssr[e].rx, v0 + g_ssr[e].ry, u1 - u0, v1 - v0);
}
void mv_ssr(u64 pass, u64 ctx) {
  o_ssr(pass, ctx);
  if (!pass || !ctx)
    return;
  if (!(g_opt_insetpp & 4)) {
    ssr_release(0), ssr_release(1);
    return;
  }
  struct pp_views pv;
  int k = which_view(*(const u64 *)ctx, &pv);
  if (k < 0 || !shaders_for(*(const u64 *)(ctx + 0x48)))
    return;
  u64 out = own_output(pass), rt = out ? *(const u64 *)(out + 0x50) : 0;
  if (!rt || !*(const u64 *)(rt + 8) || !*(const u64 *)(rt + 0x10)) {
    g_sn.skipped++;
    return;
  }
  u64 cl = *(const u64 *)(ctx + 0x28);
  u32 level = *(const u32 *)(ctx + 0x40);
  if (k < 2)
    ssr_eye(cl, level, k, &pv, (const u8 *)out, rt);
  else
    ssr_inset(cl, level, k, &pv, (const u8 *)out, rt);
}

/* hooks.c's 10 s report */
void insetpp_report(void) {
  static u32 was[12];
  u32 v[12] = {g_n.eye, g_n.inset, g_n.skipped, g_n.fail, g_ao.done, g_ao.skipped, g_n.released, g_n.calls,
               g_sn.eye, g_sn.inset, g_sn.skipped, g_sn.fail};
  if (memcmp(v, was, sizeof v) == 0)
    return;
  LOG_DETAIL("insetpp: the eye's bloom: %u resampled, %u into insets, %u skipped, %u no texture (%u "
      "tonemapper passes); the eye's AO: %u into insets, %u skipped; the eye's reflections: %u "
      "copied, %u into insets, %u skipped, %u no texture (agent.insetpp %u)", v[0] - was[0],
      v[1] - was[1], v[2] - was[2], v[3] - was[3], v[7] - was[7], v[4] - was[4], v[5] - was[5],
      v[8] - was[8], v[9] - was[9], v[10] - was[10], v[11] - was[11], g_opt_insetpp);
  memcpy(was, v, sizeof v);
  /* the TAA's previous-frame matrices (eyes, then insets) and the insets' history bands */
  if (g_tc.frames[0] || g_tc.frames[1] || g_tc.drawn || g_tc.skipped) {
    LOG_DETAIL("insettaa: previous-frame matrices, eyes: %u frames, %u moved, %u the frame before's, %u "
        "reset, %u other (worst %.2f px); insets: %u frames, %u moved (%.2f px a frame), %u the frame "
        "before's, %u reset, %u other (worst %.2f px); history bands %u drawn, %u skipped, %u not TAA "
        "(agent.insettaa %u, insetaam %u)", g_tc.frames[0], g_tc.moved[0], g_tc.ok[0], g_tc.reset[0],
        g_tc.other[0], (double)g_tc.worst_px[0], g_tc.frames[1], g_tc.moved[1],
        (double)(g_tc.moved[1] ? g_tc.move_px[1] / (float)g_tc.moved[1] : 0.0f), g_tc.ok[1], g_tc.reset[1],
        g_tc.other[1], (double)g_tc.worst_px[1], g_tc.drawn, g_tc.skipped, g_tc.noaa, g_opt_insettaa,
        g_opt_insetaam);
    for (int g = 0; g < 2; g++)
      g_tc.frames[g] = g_tc.moved[g] = g_tc.ok[g] = g_tc.reset[g] = g_tc.other[g] = 0,
      g_tc.move_px[g] = g_tc.worst_px[g] = 0.0f;
    g_tc.drawn = g_tc.skipped = g_tc.noaa = 0;
  }
}

#ifndef MV_HOST_TEST
static void *taa_dalloc(u64 size) { return vr2_dalloc_pref(size, 0x10000, "the TAA shader's copies (agent.taa)"); }
#endif
/* multiview.c's build_step3: the engine's functions, the hooks' returns */
void insetpp_boot(u64 base) {
  g_e.base = base;
#ifndef MV_HOST_TEST
  g_e.alloc_chunk = (void (*)(u64, s32))(base + E_ALLOC_CHUNK);
  g_e.set_bss = (void (*)(u64, u32, u64, u64, u64, u64, u64))(base + E_SET_BSS);
  g_e.draw_rect = (void (*)(u64, float, float, float, float, float, float, float, float, u64, u64,
                            u64, u32))(base + E_DRAW_RECT);
  g_e.set_rt = (void (*)(u64, u64, u64, u32))(base + E_SET_RT);
  g_e.find_free = (u32(*)(u64, u64, const void *, u64 *, const u16 *, u32))(base + E_FIND_FREE);
  g_e.scene_ctx = (u64(*)(u64))(base + E_SCENE_CTX);
  g_e.shader_rhi = (void (*)(u64))(base + E_SHADER_RHI);
  g_e.blend = (u64(*)(void))(base + E_BLEND);
  g_e.raster = (u64(*)(void))(base + E_RASTER);
  g_e.depth = (u64(*)(void))(base + E_DEPTH);
  g_e.samp_bi = (u64(*)(void))(base + E_SAMP_BILINEAR);
  g_e.samp_pt = (u64(*)(void))(base + E_SAMP_POINT);
  o_tm = mv_tm_orig, o_tmf = mv_tmf_orig, o_ao = mv_ao_orig, o_ssr = mv_ssr_orig;
  g_e.dalloc = taa_dalloc;
#endif
  g_tm_rest = base + TM_REST, g_tmf_rest = base + TMF_REST, g_ao_rest = base + AO_REST;
  g_ssr_rest = base + SSR_REST;
  k_insetpp_at[0] = TM_PROCESS, k_insetpp_at[1] = AO_PROCESS, k_insetpp_at[2] = TMF_PROCESS;
  k_insetpp_at[3] = SSR_PROCESS;
  /* the ones read as the passes run, looked up once (one this build's table lacks turns inset mode
   * off at the boot: multiview_boot) */
  for (u32 i = 0; i < A_N; i++)
    g_at[i] = mv_at(k_at_hikari[i]);
}
