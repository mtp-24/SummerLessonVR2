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

/* ---- Hikari 1.00's engine (CUSA07434, UE 4.12) ---- */
#define TM_PROCESS 0xcb1620u /* FRCPassPostProcessTonemap::Process(this, Context) */
#define TM_REST 0xcb1634u    /* after its first 20 bytes */
/* the film tonemapper's (r.TonemapperFilm, the one this game runs: pp1-rec1 never reached the
 * other; made by AddTonemapper 0xc67d00, vtable 0x4473ce8; inputs 0 the scene, 1 the combined
 * bloom, 2 eye adaptation, 3 the combined LUTs) */
#define TMF_PROCESS 0xcaebb0u
#define TMF_REST 0xcaebc4u
#define AO_PROCESS 0xbf3b40u /* FRCPassPostProcessAmbientOcclusion::Process(this, Context) */
#define AO_REST 0xbf3b51u    /* after its first 17 bytes */
#define AO_SETUP_AS_INPUT 0xdcu /* its bAOSetupAsInput: 0 for the last pass (writes ScreenSpaceAO) */
#define E_ALLOC_CHUNK 0x22b980u /* FMemStackBase::AllocateNewChunk(this, MinSize) */
#define E_SET_BSS 0x18e7910u    /* SetGlobalBoundShaderState(cl, level, bss, decl, vs, ps, gs) */
#define E_DRAW_RECT 0xd2acc0u   /* DrawRectangle(cl, x, y, w, h, u, v, su, sv, target, tex, vs, flags) */
#define E_SET_RT 0x8fa350u      /* SetRenderTarget(cl, texture, depth, bWritableBarrier) */
#define E_FIND_FREE 0xd22600u   /* FRenderTargetPool::FindFreeElement(pool, cl, desc, out, name, barrier) */
#define E_SCENE_CTX 0xd2d6b0u   /* FSceneRenderTargets::Get(cl) */
#define E_SHADER_RHI 0x882f00u  /* FShaderResource's lazy RHI shader creation */
#define E_BLEND 0x92b8a0u       /* TStaticBlendState<>::GetRHI */
#define E_RASTER 0x8f6ed0u      /* TStaticRasterizerState<>::GetRHI */
#define E_DEPTH 0x8f7270u       /* TStaticDepthStencilState<false, CF_Always>::GetRHI */
#define E_SAMP_BILINEAR 0x8e0ee0u /* TStaticSamplerState<SF_Bilinear, clamp x3>::GetRHI */
#define E_SAMP_POINT 0x9075c0u  /* TStaticSamplerState<SF_Point, clamp x3>::GetRHI */
#define G_POOL 0x4a04bc0u       /* GRenderTargetPool */
#define G_FILTER_DECL 0x4a050c0u /* GFilterVertexDeclaration (+0x30 its RHI declaration) */
#define G_CACHE_ON 0x466d854u   /* the command list's state filtering on (an int) */
#define G_WHITE 0x46c0440u      /* FLinearColor::White (the blend factor the engine passes) */
#define G_DRAWRECT_UB 0x4a05130u /* FDrawRectangleParameters' struct (DrawRectangle's VS uniform buffer) */
#define T_SCREEN_VS 0x4af8a48u  /* FScreenVS::StaticType */
#define T_SCREEN_PS 0x4af88e8u  /* FScreenPS::StaticType */
/* render commands' execute functions (each FRHICommand: +0 Next, +8 the function, then fields) */
#define X_VIEWPORT 0x8ae110u /* 0x28: minx miny minz(f) maxx maxy maxz(f) */
#define X_BLEND 0x8ae5e0u    /* 0x28: state, blend factor (FLinearColor) */
#define X_RASTER 0x8ae620u   /* 0x18: state (filtered through the list's +0x98) */
#define X_DEPTH 0x8ae600u    /* 0x20: state, stencil ref (filtered through +0xa0) */
#define X_PS_TEXTURE 0x256b0u /* 0x28: pixel shader, index, texture */
#define X_PS_SAMPLER 0x25960u /* 0x28: pixel shader, index, sampler */
#define X_RESOLVE 0x8fbc30u  /* 0x48: FResolveParams, source, dest, bKeepOriginalSurface */
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
} g_e;

static void (*o_tm)(u64, u64), (*o_tmf)(u64, u64);
static void (*o_ao)(u64, u64);
u64 g_tm_rest, g_tmf_rest, g_ao_rest; /* game base + the rests (the asm below jumps there) */
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
const u32 k_insetpp_at[3] = {TM_PROCESS, AO_PROCESS, TMF_PROCESS}; /* (the film one's first 14 bytes are k_insetpp_tm's too) */

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
/* FRCPassPostProcessTonemap::Process: the eyes' bloom at the insets' pixels into F before each
 * eye's runs; F as each inset's bloom while it runs */
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

/* hooks.c's 10 s report */
void insetpp_report(void) {
  static u32 was[8];
  u32 v[8] = {g_n.eye, g_n.inset, g_n.skipped, g_n.fail, g_ao.done, g_ao.skipped, g_n.released, g_n.calls};
  if (memcmp(v, was, sizeof v) == 0)
    return;
  LOG_DETAIL("insetpp: the eye's bloom: %u resampled, %u into insets, %u skipped, %u no texture (%u "
      "tonemapper passes); the eye's AO: %u into insets, %u skipped (agent.insetpp %u)", v[0] - was[0],
      v[1] - was[1], v[2] - was[2], v[3] - was[3], v[7] - was[7], v[4] - was[4], v[5] - was[5],
      g_opt_insetpp);
  memcpy(was, v, sizeof v);
}

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
  o_tm = mv_tm_orig, o_tmf = mv_tmf_orig, o_ao = mv_ao_orig;
#endif
  g_tm_rest = base + TM_REST, g_tmf_rest = base + TMF_REST, g_ao_rest = base + AO_REST;
}
