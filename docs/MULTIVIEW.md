# Multiple views: inset mode (a gaze inset over a lower-resolution periphery)

Inset mode is what the README calls dynamic foveated rendering (DFR).

## Where it stands (2026-11-07, kept up since)

Built, and in the release build: **inset mode**, the default in all three games and Hikari
1.02 (`agent.multiview=600` without a line; `agent.multiview=0` in `cvars.txt` for the stock
picture).
- **The layout:** the eyes 1440 to 1600 wide per game at 60 fps, 1200 to 1320 at 90, 960 to
  1080 at 120 (`multiview.c` `k_rate_eye`, from the sweeps: SUMMER-LESSON.md, "The sweep with
  the palm tracker on"; `resolution.txt`'s width instead when it asks for 1600 or less), two
  1200x600 insets side by side in 600 rows below them (1080x540 at 90, 960x480 at 120: 28), at
  1200 pixels a unit of tangent (about +-27 by +-14 degrees at 60: 12, 17, 20). They follow the
  gaze (PS VR2's eye tracking, smoothed: 15, 16) and go to Hmd2 as layer 1 with its window test
  (19, 20).
- **The game's look in the insets:** each inset its own view state (13; the game's fades, its
  dimming and the yellow outline round a chosen item reach them: 21), the left eye's exposure
  (7) and translucency lighting volume (23), the eye's own bloom, AO and reflections (22, 27,
  `agent.insetpp=7`), its own eye adaptation passes left out (24), its eye's occlusion-culling
  answers and LOD choice (26), and the engine's TAA in every view, patched so the insets'
  history follows the window as the gaze moves it and new pixels converge quickly (`agent.taa=13`,
  the window in 2-pixel steps: 30). Shadows 5, AO 3 and AA quality 6 (3 on the newer engine:
  Chisato, Allison, Hikari 1.02), kept against the game's resets at scene changes (24).
- **24 patches** to the game's code at 60 fps, 26 at the native 90 and 120 (28);
  `AGENT_PATCH_MAX` is 26 in the agent and the payload.
- **GPU:** per game and rate in the sweeps (SUMMER-LESSON.md); on Hikari 1.00, 12.1-12.3 ms a
  frame at 60 fps in steady play, 13.4-13.9 the longest frame of a 10 s window (24).
- **The TV mirror's black frames:** fixed (25).
- **Open:** see "Still open" at the end.

The rest of this page is the dated record, in the order things were found: the scope (an offline
reading, 2026-11-06), the checks, the build's steps and items 1-30. Some of its options were
removed for the release (SUMMER-LESSON.md, "Options removed"); where one is described as current,
it is marked so.

## The scope (2026-11-06)

Check 3 of the multi-view question (docs/FOVEATION.md, "The runs"): what it would take to make
Summer Lesson (Unreal Engine 4.12, compiled into the eboot) draw each eye twice. One view is wide
and at low resolution; the other is a small one at full resolution around the gaze. Hmd2 would
compose them as two layers. This is an offline reading of Hikari 1.00's eboot (2026-11-06); nothing
in it had run then.

## In short

- **Possible, with an engine patch of moderate size.** The two eye views come from one loop,
  `UGameViewportClient::Draw` (0x15d3a90). It runs twice, and the game's stereo plugin places
  and projects each view through four virtual functions the agent can replace. Unreal's renderer
  already draws four views in one family for four-player split-screen. What's new is stereo
  code meeting more than one left/right pair.
- **The design that touches the least**: run the loop four times with the passes left, right,
  left, right. The second pair are the insets, told apart by the plugin functions the agent
  supplies. No new pass values reach the engine. Three changes are needed besides the plugin
  functions:
  1. a per-view state (TAA history) for each inset, which the engine picks per pass at one
     place;
  2. no hidden-area mask for the insets;
  3. the hand-over: two Hmd2 layers per eye.
- **Its unknowns**, none of which this reading can settle:
  - whether Hmd2 reprojects a second eye layer with its own angles. Ghost-VR's second layer is
    an overlay, which Hmd2 doesn't reproject;
  - whether the seam between the two layers shows;
  - what the renderer's 42 pass checks do with two pairs;
  - the fixed cost of each extra view, on the GPU and on the render thread.
- **Expected**, with the periphery at half density and a ±20° inset at today's centre sharpness:
  about 31 % of today's pixels, so roughly 8.3 ms less shading at 2048, less the extra views'
  fixed cost (measured in check 1: at most ~2 ms for an instanced pair; more for the insets as
  mono views). About 4-6 ms net.
- **Before any code**:
  - check 1: two cvar-only runs at small eye sizes, for the fixed cost of a pair of views
    (**done 2026-11-06**: about 1.9 ms; see "Check 1" below);
  - check 2: an agent-only Hmd2 inset-layer test, which needs the player to judge the seam;
  - `vr.InstancedStereo`, read once in a run (**done: it is 1, instanced stereo is on**, which
    changes the design: see "Check 1").

## The view loop: `UGameViewportClient::Draw` (0x15d3a90)

```
15d44af  call 0x1971d00          ; GEngine->IsStereoscopic3D(Viewport) -> al
15d44b4  mov  %al, 0xb0(%rsp)    ; stereo
15d44be  inc  %eax               ; views = stereo + 1
15d44c6  mov  %eax, 0xac(%rsp)
15d44d0  xor  %r14d, %r14d       ; i = 0
15d44e0  cmp  $1, %r14d          ; pass = stereo ? (i < 1 ? 1 : 2) : 0
15d44e4  mov  $1, %eax           ;   (eax = 2 - carry; cmove 0 without stereo)
15d4511  sbb  $-1, %eax
15d451c  cmove %ecx, %eax
15d4527  mov  %eax, (%rsp)       ; the pass, the 7th argument
15d452a  call 0x16de770          ; ULocalPlayer::CalcSceneView(..., pass)
15d4a78  inc  %r14d              ; next view
```

For four views:
- the count at 0x15d44be becomes stereo × 3 + 1;
- the pass becomes `(i & 1) + 1`;
- i is written where the agent can read it (the round: periphery or inset).

That is about 30 bytes of new code in a cave, entered at 0x15d44e0, patched by the payload at the
entry point as the frame-rate stores are (OTHER-GAMES.md, Method).

## Each view's state: `CalcSceneViewInitOptions` (0x16dd810)

```
16dd97f  lea  0x98(%rbx), %rax   ; LocalPlayer->ViewState
16dd986  lea  0xc0(%rbx), %rcx   ; LocalPlayer->StereoViewState
16dd98d  cmp  $2, %r12d          ; the pass
16dd991  cmove %rcx, %rax        ; the right eye's own
16dd9db  mov  %r12d, 0x100(%r14) ; ViewInitOptions.StereoPass
```

The view state holds the TAA history, eye adaptation, occlusion history and the previous frame's
matrices. A view sharing one with another view of another size and projection would smear both.
The insets need two more `FSceneViewStateReference`s (0x28 bytes each), made the way the engine
makes its own: `ViewState.Allocate()`, the renderer module's `AllocateViewState`. Their address
is still to be found. A cave at 0x16dd97f picks among four by the pass and the round.

## The plugin's stereo functions (vtable 0x465a258; the stereo interface at 0x465a570, `this` - 8)

The engine calls these through `GEngine`+0x968 (`StereoRenderingDevice`), at the interface's
slots:

| function | plugin | slot | called from | for the insets |
|---|---|---|---|---|
| `AdjustViewRect(pass, &x, &y, &w, &h)` | 0x2f8ef50: w /= 2, x += w for pass 2 | +0x18 | `GetProjectionData` 0x16dda80 (at 0x16de10e) | each view's rect in the texture: the periphery pair, then the insets beside or below them |
| `CalculateStereoViewOffset` | (thunk to the plugin's) | +0x28 | 0x17abc9e | the same eye offset as the periphery |
| `GetStereoProjectionMatrix(pass, fov)` | 0x2f8f540 | +0x30 | 0x16de67c | an off-axis projection: the gaze window's tangents |
| `CalculateRenderTargetSize` | 0x2f8fb20 | (primary +0x2c0) | | a texture holding all four |
| `NeedReAllocateViewportRenderTarget` | 0x2f8fc40 | (primary +0x2c8) | | its size compared with it |
| `DrawHiddenAreaMesh_RenderThread` | 0x2f90890 | primary +0x180 | 0x93f0a9 (the prepass) | nothing for an inset (else PS VR1's corners are cut out of it) |
| `DrawVisibleAreaMesh_RenderThread` | 0x2f908f0 | primary +0x188 | 0xd2b801 | nothing for an inset |

The agent replaces vtable entries in the eboot's read-only data the same way the payload patches
code at the entry point. Its functions receive the pass, and they read the round from the loop's
cave.

## The renderer's special cases

`FSceneView` is 0xd10 bytes. Its pass is at +0x420, copied from the options' +0x100 in its
constructor (0x17aa463). In the renderer's range (0x8e0000-0xd40000), 42 functions compare it:
17 with 0 (not stereo), 32 with 1 (left), 8 with 2 (right). Most of the left-eye comparisons sit
around the depth prepass (0x93a000-0x940000), which points at instanced stereo: one draw for both
eyes, from the left view, assuming exactly one pair. It is on in this game (`vr.InstancedStereo` = 1,
read in check 1), so the insets can't be a second left/right pair: see "Check 1" below. Every one of the 42 needs reading for an assumption that the views are one left/right
pair; the right-eye ones mostly reuse the left eye's work through the view state or the family's
first view:

```
0x8f7820 0x8f7ca0 0x928150 0x9342a0 0x93acb0 0x93ade0 0x93b170 0x93b510 0x93b780 0x93bf70
0x93c3a0 0x93fb70 0x93fd10 0x93ff90 0x9402f0 0x9482d0 0x9c6230 0xa8f100 0xaa0a60 0xaaa3c0
0xaebf80 0xb48ee0 0xb4cd10 0xb4cda0 0xb4cf00 0xb4d070 0xb4d100 0xb4d190 0xb53c50 0xb66ac0
0xb67980 0xb689e0 0xb69a40 0xb6aaa0 0xb6b7c0 0xb9d0a0 0xc08040 0xc0f770 0xc71e60 0xcaebb0
0xcb1620 0xcde500
```

The offset is common in other structures too, so some of these won't be views at all. Outside the
renderer, a further 57 functions compare a +0x420 field the same way; most of them are other
structures.

## The hand-over and Hmd2

The game hands each frame over as one texture, two eyes, and their uv (`sceHmdReprojectionStart`).
The agent builds Hmd2's layer from that anyway (vr2.c `game_loop`). With insets it builds two
layers per frame:
- layer 0: the periphery rects with the full field of view;
- layer 1: the inset rects with the window's tangents and the same pose.

Hmd2 takes several layers. The `param.count` of the `SetParam` block and the VR port's buffer sets
registered with two layers are Ghost-VR's. But Ghost-VR's second layer is an overlay (+0x9c = 1),
which Hmd2 leaves out of reprojection (its notes: libSceHmd2 0xb1b1, 0xc978, 0xcb38). Call of the
Mountain sends a second eye layer too, and whether Hmd2 reprojects that one with its own pose and
angles, and how it joins the two, is unknown (+0x98 = 100, which every user of the layer sets, may
be an opacity). Check 2 answers it.

## TAA, the gaze and the seam

- Each inset's projection moves with the gaze every frame. Unreal's TAA reprojects its history
  with the previous frame's matrices (kept in the view state), projection included, so a steady
  gaze is like a steady camera. After a saccade the inset is somewhere new and its history is
  useless for a few frames: aliasing then, mostly hidden by saccadic suppression.
- The seam: an edge between the periphery's density and the inset's. Without blending in Hmd2,
  the inset needs a soft edge made in the agent: its outer band drawn over the periphery with
  falling alpha, if Hmd2 blends by alpha; or the periphery sharpened near the edge.

## What it costs and saves

- Pixels at 2048: the periphery at half density over the whole field is 1024x1152 an eye. An
  inset of ±20° at today's centre density (734 pixels per unit of tangent) is about 535x535 an
  eye. That's 2.93 Mpx in all against 9.44: 31 %, about 8.8 ms less at 1.36 ms per Mpx. The inset
  can be sharper than today's centre, since it's a texture of its own beyond the 4096-wide limit:
  at 1.5 times today's density it's 0.64 Mpx more.
- The fixed cost of a pair of views: the fit says 1.6 ms for everything that doesn't follow the
  pixels, Hmd2's reprojection and the TV mirror included. An extra pair adds at most about that
  on the GPU (culling, shadow depths per view, per-view passes), plus the render thread's work for
  two more views. At 60 fps there's room: the game held 120 fps at 1248 eyes. At 90 (11.1 ms) it's
  to be measured.
- Net: about 5-7 ms at 2048 at 60, with the same centre sharpness. Or the same 14 ms with a
  centre about 1.5 times sharper than now.

## Before any code

1. **The fixed cost of a pair** (no code; two replays): `resolution.txt` at 960 and at 1440
   (eyes 960x1080, 1440x1620). The two points and today's 2048 give the line; its intercept is
   the cost that would come again with a second pair.
2. **An inset layer in Hmd2** (agent only): every frame handed over twice, layer 0 as now, layer 1
   the centre third of each eye's picture with its narrower tangents. Correct, the headset shows
   the same picture. A second layer Hmd2 doesn't reproject would swim against the first when the
   head turns. A visible edge means a seam to hide. It needs the player wearing the headset to
   judge. It also settles whether a non-overlay second layer is taken at all.
3. **`vr.InstancedStereo`**: a `cvars.txt` line `vr.InstancedStereo` (read only) in any run.

If 1 shows a pair of views costing under about 2 ms and 2 shows a reprojected inset without a
hard edge, the patch is worth writing. In order:
1. the plugin functions (rects, projections, sizes, no masks) with two views as now, the inset
   rects empty, a no-change run;
2. the loop and the view states (four views, the insets at the centre, fixed);
3. the two layers;
4. the gaze;
5. the 42 functions read one by one as each step shows trouble.

Several weeks, each step a run of its own. Most failures would end the game rather than the
console, since this is the game's own CPU-side code. The two exceptions are the Hmd2 layers and
render targets of a new size, the kind of thing that has hung the GPU before.

## Check 1 (2026-11-06): the fixed cost, and instanced stereo is on

Two replays of `tests/rec1` (280 s, `replay worn`, the claude/dfr agent with every foveation option
off), `resolution.txt` 960 and 1440, `cvars.txt` + `vr.InstancedStereo` and `r.ScreenPercentage`
(read only); logs `logs/mv1-960.log`, `logs/mv1-1440.log`. Both ran clean (kernel log without a
panic, the game killed and gone at the end); `resolution.txt` back to 2048, `cvars.txt` the
console's own, `agent.elf` and `test.txt` deleted after. The 2048 point is 2026-11-05's plain replay
of the same recording (`logs/fov1-plain.log`). The game's GPU time a frame, play windows only
(`tools/fov/report.py`, scene loads left out):

| eyes | Mpx (both) | GPU ms a frame, average | most |
|---|---|---|---|
| 960x1080 | 2.07 | 4.58 | 6.9 |
| 1440x1620 | 4.67 | 7.74 | 9.8 |
| 2048x2304 | 9.44 | 13.96 | 17.8 |

- **The line**: 1.87 ms + 1.28 ms per Mpx, within 0.1 ms of all three points. The two small points
  alone give 2.05 + 1.22. So the cost that doesn't follow the pixels is about 1.9-2.0 ms (the
  earlier estimate was 1.6 + 1.36).
- **For a second pair of views** that is an upper bound. It holds the whole frame's fixed work,
  and part of that comes once a frame whatever the views: the shadow depths with one light, the
  agent's mirror, the frame's own start and end. So a second pair costs at most ~2 ms on the GPU.
  At the threshold this scope set ("under about 2 ms"), not clearly under it.
- **The saving recomputed** with the measured slope: the periphery at half density plus a ±20°
  inset, 2.93 Mpx against 9.44, is about 8.3 ms less shading. Less up to ~2 ms is **about 6.3 ms
  net at 2048 at best**, before instanced stereo below.
- **`vr.InstancedStereo` = 1** (game and render thread). The game draws both eyes in one
  instanced draw from the left view; the right view's base and depth passes are skipped (the 32
  left-eye comparisons around the prepass are that). The design above, which repeats the pair as
  L, R, L, R, doesn't survive it. Unreal 4.12's instanced path takes the family's first two views
  as the pair, so a second left/right pair would instance its left view's draws into the wrong
  rect, and its right view would draw nothing. Two ways remain:
  1. **Insets as mono views** (pass 0, `eSSP_FULL`) after the instanced periphery pair. The
     instanced pair stays as the game has it, and the insets draw one view at a time like a
     split-screen view. But `GetProjectionData` calls the plugin's `AdjustViewRect` and
     projection only for stereo passes, so the insets' rects and projections must come from the
     loop's cave (`CalcSceneView`'s options) instead. And the 17 functions that compare the pass
     with 0 need reading for stereo-family assumptions. The insets lose instancing, so their
     draw calls (render thread, command processor) are a mono view's each: two more views' worth
     of draws.
  2. **Instanced stereo off** (`vr.InstancedStereo` is read-only at startup; it would be a patch
     to its value before the renderer starts) and four plain views. Every view's draws on the
     render thread then, four times today's left view's: likely too much on the CPU side at 60.
     It also depends on whether the cooked shaders have the non-instanced vertex shaders at all
     (with instanced stereo on, 4.12 compiles the base pass for it; unknown whether a plain
     variant is cooked).

  The first is the way. It raises the fixed cost of the insets above the measured bound, since
  ~2 ms was an instanced pair's. The render thread's time for two mono views is unmeasured.

**What check 1 changes.** The gain is now about 4-6 ms at 2048 at 60, not 5-7, and the patch is
larger: the inset views made in the loop's cave as mono views, not through the plugin's stereo
functions. It's still worth it only if check 2 shows Hmd2 reprojecting a second layer without a
hard seam. Whether two mono views fit on the CPU at 60 was the cheaper next measure: they do
(below).

## The game's threads (2026-11-06): plenty of CPU room for more views

`agent.rtime=1` (agent/rtime.c, debug only) samples each game thread's own CPU clock at every
hooked call it makes (the submits, the hand-over, the pad read, its waits) and logs every 10 s each
thread's CPU time a frame, by its name. Two replays of `tests/rec1`, 280 s, 2048 and 960 eyes
(`logs/mv3-rt-2048.log`, `logs/mv3-rt-960.log`; both clean, the console restored after). Averages
over the 27 reports of each run, in us of CPU a frame (16 667 at 60):

| thread | 2048 | 960 | most (2048) | what it is |
|---|---|---|---|---|
| eboot.bin | 3983 | 3743 | 4597 | the game thread |
| RenderThread 3 | 1442 | 1416 | 2095 | Unreal's render thread: the views, their visibility; hands frames over |
| TaskGraphThreadHP 7 | 1147 | 1079 | 1604 | high-priority tasks (the render thread's parallel work) |
| nusc | 1087 | 1039 | 1300 | (not Unreal's; likely the sound middleware) |
| RHIThread | 948 | 928 | 1066 | translates the render thread's commands into Gnm; one submit a frame |
| TaskGraphThreadHP 6 | 822 | 799 | 1153 | high-priority tasks |
| TaskGraphThreadHP 5, 4 | 285, 177 | 276, 170 | | high-priority tasks |
| TaskGraphThreadNP 0-3 | 300-380 each | the same | ~700 | normal-priority tasks |
| SubmitGPUCommandsAsyncT | 158 | 166 | 187 | hands the command buffers to the GPU (6-7 submits a frame) |
| FAsyncLoadingThread | 1751 (in 6 reports) | | 5970 | the scene loads |

- **This game build has an RHI thread and an async submit thread.** The first runs
  (`logs/mv2-rt-*.log`), which timed only the submitting threads, saw the RHI thread and the
  submit thread: ~1 ms a frame between them.
- **The eye size changes nothing on the CPU** (2048 against 960 within 2-6 %), so no thread
  spins waiting for the GPU: these are the threads' work.
- **The busiest thread uses about a quarter of its frame** (the game thread, 4.0 ms, 4.6 at the
  most). The render-side work for today's instanced pair, shadows and post-processing is about
  1.4 ms on the render thread, 2.4 ms over the high-priority task threads and 0.95 ms on the RHI
  thread, each on its own core.
- **Two inset views as mono views** would add their visibility, draw setup and translation. Even
  if each cost as much as the whole of today's render side (pessimistic: a mono view draws once
  where the instanced pair draws once for two eyes, and shadows and post-processing come once a
  frame), the render thread would be at ~4.3 ms, the RHI thread at ~2.8 ms and the high-priority
  tasks at ~2.4 ms a thread. That's far under 16.7 ms, and under 11.1 ms (90) too. **The CPU is
  not what limits multi-view**; the GPU's fixed cost per view (check 1) and Hmd2's second layer
  (check 2) are.
- The process's CPU-time clock (FreeBSD's `CLOCK_PROCESS_CPUTIME_ID`, 15) read as the wall
  time here (16 666 us a frame, exactly the frame period, in every report): it doesn't count the
  process's CPU on this console, so the per-thread sums are the figures to use. The threads seen
  add up to ~13 ms a frame over ~32 threads.

## Check 2 (2026-11-06): Hmd2 takes a second layer and reprojects it with the first

**Hmd2 read offline first** (libSceHmd2 12.40; evidence in the side notes `hmd2-layers.md`):
- **What it takes:** up to 5 layers a frame. Each is reprojected with its own pose when +0x9c is 0
  (+0x9c = 1 is a head-locked overlay, Ghost-VR's).
- **How they combine:** layer 0 is drawn opaque, and each later layer goes over it in its blend
  mode, +0x98: 0 straight alpha, 1 premultiplied, 2 additive. 100 is allowed on layer 0 only.
- **No clipping:** a plain type-0x10 layer is **not clipped to its window**. Hmd2 samples it over
  the whole eye, and only its alpha (or a clamp-to-border sampler, off the texture) hides it.
- **Where it can come from:** a layer may use the same registered texture as layer 0 (+0x04 = 0).
- **Its translation on the PS4 side:** the shader's layer loop, which had never run on this
  console, was translated on the host with the payload's rdna2gcn. Its only new scalar forms
  decode right: the SGPR + offset loads with SCC kept around the add, the loop's compare and
  branches. Its scratch s66/s67 are unused by Hmd2. Its only new vector form is a plain
  `v_mad_f32`.

**The run** (`logs/mv4-inset.log`; agent 6f6aadb; one replay of `tests/rec1`, 240 s, 2048 eyes,
the headset on the table, the user at the TV):
- 0-90 s: `agent.tvhmd=1`, the TV showing Hmd2's own output (both eyes, the lenses' warp in it);
- 90-150 s: `agent.inset=1`, layer 1 = the centre third of each eye of the same texture
  (682x768 at 683,768 and 2731,768), its angles cut from those pixels, straight alpha;
- 150-210 s: `agent.inset=2`, the same layer additive;
- 210-240 s: `agent.inset` off.

Results:
- **Accepted, no hang:** every frame handed over was shown, no SetParam error, the kernel log
  clean, the game closed normally.
- **The headset's picture on the TV works** (`agent.tvhmd`) and costs no GPU time: 0-90 s
  matches the run before, window by window, within 0.6 ms.
- **The TV:** "normal, then brighter, over-exposed". With the additive layer every pixel got its
  own value added again. A layer reprojected otherwise than layer 0 would have shown the picture
  twice: the replay's head moves against a headset lying still, so the reprojection is large,
  and the two copies would sit far apart. The user saw no second picture (the over-exposure
  hides fine sharpness). So Hmd2 reprojects layer 1 with layer 0.
- **Its cost:** the game's GPU time a frame rose about 1 ms while the layer was on (15.1-15.7 ms
  against 14.0-15.0 in the same 10 s windows of `logs/mv3-rt-2048.log`). Hmd2 samples the extra
  layer over the whole of both eyes at every refresh, two a frame, on the GPU the game shares.

**What this means for multi-view**:
- **The inset needs its own texture** with alpha 0 outside the inset and an alpha ramp at its
  edge for the seam: an agent pass over the game's inset rect, or a clamp-to-border transparent
  sampler on a texture that is only the inset. It is blended in straight-alpha mode.
- **The seam is still untested**: that needs the inset as such a texture.
- **About 1 ms of Hmd2's per-refresh work** comes off the 4-6 ms estimated after check 1 (the
  layer is sampled over the whole eye whatever its size): **about 3-5 ms net at 2048**.
- **Nothing found so far rules multi-view out.** What's left is the engine patch above (weeks).

## The build (decided 2026-11-06: multi-view, for native 90 and for 60's heavy scenes)

**The aim.**
- **At 60:** 2048 averages ~14 ms of GPU time a frame but its heaviest scene peaks at 17.8,
  over the 16.7 a frame has: those are the frames shown three times. Multi-view's 3-5 ms take
  the peaks under.
- **At 90 (11.1 ms a frame):** today's native 90 holds eyes of only ~1700. Multi-view would hold
  a centre as sharp as 2048's, or sharper.

**The layout** (2048-class centre):
- **The periphery pair** at half density, 1024x1152 an eye, side by side: 2048x1152, drawn as
  today's instanced pair.
- **The two insets** as mono views below it: ±20° at today's centre density (734 pixels a unit of
  tangent) is ~535x535 an eye, or ~800x800 at 1.5 times. That gives a texture of 2048x1687 (or
  2048x1952 at 1.5x), well under Hmd2's 4096.
- **To Hmd2:** layer 0 is the periphery; layer 1 is the insets, straight alpha, transparent
  outside them with an alpha ramp at their edge (Hmd2 doesn't clip a layer: check 2).

**The budget** (the check-1 line, 1.87 ms + 1.28 ms a Mpx, plus what checks 1 and 2 add):

| | Mpx | ms |
|---|---|---|
| periphery pair | 2.36 | 3.0 |
| insets at 1x (1.5x) | 0.57 (1.28) | 0.7 (1.6) |
| fixed | | 1.9 |
| two mono views' own fixed cost (at most) | | ~2 |
| Hmd2's second layer (two refreshes a frame at 60, one at 90) | | ~1 at 60, ~0.5 at 90 |
| **total** | | **~8.6 (9.5) at 60, ~8.1 (9.0) at 90** |

That is against 14 ms today. At 90 there's ~2 ms left even with the sharper inset.

**The steps**, each a console run of its own:
1. **The engine sites (offline):** the view loop's cave for two mono views after the instanced
   pair; their rect, off-axis projection and eye offset; two more view states; the renderer's
   pass checks with mono views in a stereo family; anything that assumes the views tile the
   target.
2. **The layout without insets:** the target 2048x1687, the periphery pair at half density in
   its top, layer 0 from it. Softer everywhere; checks the sizes and the hand-over.
3. **The two inset views,** fixed at the centre, drawn into the bottom. Seen with
   `agent.mirrorboth=1` (the whole texture on the TV).
4. **The inset as layer 1:** its alpha (an agent pass writing the ramp), straight alpha. Seen
   with `agent.tvhmd=1`, then worn, for the seam.
5. **The gaze** (eye tracking, worn).
6. **With the native 90 build**, and 60 measured again for its drops.

## Step 2 done (2026-11-06): the layout without insets

Agent ebdf5f0 with the payload's patch table (98783ca), `resolution.txt` 1024,
`agent.multiview=544`, a 180 s replay (`logs/mv6-step2b.log`; the first try, `mv5-step2.log`,
drew the eyes 608 rows high, fixed). Results:
- **The patches:** "3 patches to the game's code in".
- **The texture:** 2048x1696, the eyes' viewports 1024x1152 at its top. The census's largest
  scissors are 2048x1152 at 0,0, so nothing draws into the 544 rows below.
- **GPU time:** 4.9-5.7 ms a frame in play, as the check-1 line predicts for 2.36 Mpx.
- **Clean:** the kernel log clean, the game closed normally.

## Step 3 built (2026-11-06), not run yet: the two inset views

`agent/multiview.c`, 8 more entries in the payload's patch table (11 in all), only with
`agent.multiview=N`; the views only with `agent.insetviews=1`, which follows `cvars.txt` live.
- **Three calls into the agent,** each through a 14-byte `jmp [rip]` in dead padding after a
  function's `ret` (0x15d3a82, 0x15d3912, 0x16df982: nothing refers to them; the agent is mapped
  too far for a rel32):
  - Draw's view count (0x15d44bb) -> `mv_count_stub`: 4 views when stereo and the insets are on,
    else 2 (1 without stereo), as before;
  - Draw's call of `CalcSceneView` (0x15d452a) -> `mv_cs_wrap`: the third and fourth views (pass
    2 again from Draw) go in as pass 0, the plain per-view path, so the instanced pair stays
    Views[0]/Views[1]. A view the engine makes past the inset's rect is set back to it.
  - `CalcSceneView`'s call of `CalcSceneViewInitOptions` (0x16de89c) -> `mv_opt_wrap`: keeps each
    eye's options; an inset gets that eye's origin and rotation, that eye's projection cropped to
    a window straight ahead (`M[2][0..1]/M[2][3]`) at twice the periphery's pixels a tangent, its
    rect below that eye (544x544 at 0,1080 and 960,1080 with eyes 960x1080), and no view state:
    no TAA history or eye adaptation of its own yet (else it shares the left eye's). An eye with
    no options that frame gets no inset (Draw takes a null view as none).
- **The final pass's whole-target clear** (tonemap 0xcb1a3d, upscale 0xcde69e) only for pass 1
  (`ja` -> `jne`), so an inset drawn after the periphery doesn't wipe it.
- **Host test** (`tools/fov/multiview_test.c`): every entry on the stock bytes, the calls land on
  the pads and the pads hold the agent's three entries; a frame through the wrappers with
  PS VR2-like asymmetric eyes: the rects, no view state, pass 0, each eye's origin, straight ahead
  at the inset's centre and twice the periphery's density across and up; a refused left eye gives
  no left inset; a fifth view is refused.
- **The run:** `resolution.txt` 960 (r.ScreenPercentage exactly 100, so no scaling of the insets'
  rects whatever percentage the engine applies to pass 0), `tools/fov/runs/mv-step3.txt` from the
  launch and `mv-step3-on.txt` (+ `agent.insetviews=1`) from 60 to 150 s. The TV
  (`agent.mirrorboth=1`) shows the whole 1920x1624 texture: the two sharp windows should appear
  below the eyes. The log has each view's rects for the first frames, and the census the
  scissors drawn.
- **Not known until it ran:** what the game's `FinalizeViews` and the plugin's per-view
  functions do with four views; whether other post passes clear the whole target; the GPU cost.

## Step 3 run (2026-11-06 22:10, `logs/mv7-step3.log`): the insets drawn, clean

Agent bcc57d7 with the resident payload (no swap), `resolution.txt` 960, the insets on from 60 to
150 s of a 180 s replay.
- **The patches:** "11 patches to the game's code in".
- **Every frame:** both inset views made, 0 left out (8192 in 90 s); the game held 60 fps; no
  game or console trouble (the kernel log's one GPU reset is the switch to Neo mode at the
  launch, in every run).
- **Where they look:** straight ahead lands at NDC x = ±0.32 in each eye (towards the nose: PS VR2's
  eyes are asymmetric), so the windows are centred there, ±0.28 x ±0.25 of the eye's NDC.
- **The rects:** the engine's rects for every inset went past the inset's own and were set back
  (the log shows them after; which of ViewRect, UnscaledViewRect and UnconstrainedViewRect, and
  by how much, is to log next). The census's largest scissors became 1920x1624 at 0,0 with the
  insets on: something covers the whole target (likely the bounding-box canvas fill, section 1
  of the engine notes).
- **GPU time:** 3.8-3.9 ms a frame before (960 eyes), 6.1-7.5 (about 6.8) with the insets, 4.6-5.0
  after (a later scene). About +2.9 ms for two 544x544 views: their pixels (~0.8 ms by the
  check-1 line) and their own fixed cost (~2 ms), as the budget had it. Draws per 10 s went from
  ~250k to ~656k: each inset draws its own passes (shadows, prepass, base) as a plain view.
- **Not seen yet:** whether the insets look right (sharp, straight ahead, the same exposure and
  time as the eyes): the TV showed the whole texture (`agent.mirrorboth=1`).

**The rects, logged (2026-11-06 23:2x, `logs/mv8-step3b.log`, agent f0540ee, same run
settings):** only `ViewRect` goes past the inset's, by one pixel up (and one left for the right
inset): 0,1079-544,1624 and 959,1079-1504,1624 for 0,1080-544,1624 and 960,1080-1504,1624;
`UnscaledViewRect` and `UnconstrainedViewRect` are the inset's. That is the engine's own rounding
of the scaled rect's minimum (the right periphery eye's `ViewRect` starts at 959 too, stock), so
setting it back changes nothing but keeping the inset off the eyes' last row. Everything else as
in the first run: both insets every frame (10240), 0 left out, 60 fps, about 6.1-7.8 ms of GPU
time with them against 3.6-3.9 before, the kernel log clean.

Next: look at the picture (the TV with `agent.mirrorboth=1`), then step 4 (layer 1 with its
alpha).

## Step 4 built (2026-11-06), not run yet: the insets as Hmd2's layer 1

With `agent.insetviews=2` (live; 1 is the views alone):
- **No texture of its own:** layer 1 is the game's texture again (registered layer 0), its
  viewports the insets' rects, its angles the insets' windows as the game drew them (from each
  eye's projection in `mv_opt_wrap`: `tan = (ndc w - M[2][c]) / M[c][c]`), straight alpha (mode
  0), reprojected with layer 0's pose. Hmd2 samples a layer wherever the eye looks (no window
  test, clamped at the texture's edge), so the texture's alpha is what limits it.
- **The alpha** (`agent/inset.c`, shader `tools/gpu/inset.c`): one compute pass a frame on Hmd2's
  queue, as the filter's, at the hand-over: every texel's alpha alone written (an image store of
  the alpha component, dmask 0x8; the colour neither read nor written), 0 everywhere but the
  insets, rising from 0 on their outermost texels to 1 at 40 texels in (smoothstep). Layer 0 reads no alpha. The game's
  texture is 8_8_8_8 (an alpha of 8 bits); another format, or a frame whose pass didn't go,
  goes as layer 0 alone. Hmd2 refusing the two layers three frames running turns the layer off
  (`agent.insetviews` 2 -> 1).
- **Host tests:** `tools/fov/inset_test.c` (the shader over a 1920x1624 texture: the colour
  unchanged, alpha 0 outside and on the edge, 1 from 40 in, rising); `multiview_test.c` (layer 1's
  angles: twice the eye's pixels a unit of tangent, centred on the axis).
- **The run:** `resolution.txt` 960, `tools/fov/runs/mv-step4-off.txt` (`agent.tvhmd=1`: Hmd2's
  own output on the TV) from the launch, `mv-step4-views.txt` from 30 s, `mv-step4-layer.txt`
  from 90 s, off again from 160 s of 180. On the TV the centre of each eye should turn sharper
  from 90 s, with no edge to see if the two match (exposure, TAA: the insets have neither of
  their own yet).

**First run (2026-11-06 23:3x, `logs/mv9-step4.log`, a7a85f0): layer 1 never went.** The pass
waited up to 2 ms for the game's frame label, as the filter does, and found it unwritten for
every frame (600 of 600 each 10 s): that label marks later GPU work, as vr2.c found on
2026-11-04 (the picture is complete at the hand-over). Otherwise clean: the views every frame, the
layer's settings and the pass set up, the kernel log clean. Changed: no wait, and the pass stores
the alpha alone, so it can't touch the colour even if the game's GPU were still on the frame.
(The filter, `sharpen.txt`, waits on the same label and likely never sharpens: a follow-up.
The filter was removed for the release.)

**Worn run (2026-11-06 23:39, `logs/mv10-step4-worn.log`, cad68d6, the user playing, no
replay; the layer on at 60-100, 120-160, 180-220, 240-300 s):** layer 1 went with every frame
(the alpha pass ~30-37 us of GPU a frame, Hmd2 never refused the two layers), the game's GPU
~4.9 ms a frame without the insets and ~7.3 with them and the layer, clean (the game closed
normally, the kernel log clean). **The picture is wrong:** the user saw black masks in the
headset, and the TV (`agent.tvhmd=1`) showed the same, with no sharp patch to be found. The
layer's angles match the game's projection (left eye: tangents out 1.8418, in 0.9472, up and
down 1.3290; projection M[2][0] 0.3208 = (out - in) / (out + in); the insets' ±0.3951 x ±0.3347
around the axis). Likely the insets' own pictures are dark or black (never looked at yet; they
have no view state, so no eye adaptation of their own); to check with `agent.mirrorboth=1`.

## Still to find (2026-11-06; since then)

- `FSceneViewStateReference::Allocate`, to make the insets' view states: not needed, the view
  states are made another way (13).
- The render thread's time per view, measured as a whole frame's (above): small enough that a
  per-view figure isn't needed before check 2.
- What 0x2f8f640, 0x2f8f780 and the rest of the plugin's stereo slots do (the canvas and the
  render-thread eye parameters), in case they assume two views: nothing found wrong with four
  views in the runs since.
- The same sites in Chisato's and Allison's eboots (OTHER-GAMES.md, Method): still open (see
  "Still open").

## Captures (2026-11-07, `agent.capture`, `logs/captures/`)

- **`mv12` (step 4 build, the replay):** the whole texture at 75 s: the insets in their rects,
  the right part of the scene (straight ahead), but about 5 times darker than the eyes (mean 11
  against 59): no view state, so no eye adaptation. Hmd2's output at 150 s (layer 1 on) and at
  165 s (insets off) both show the black masks: the top of each eye showing the inset rows,
  black wedges between. So they come from step 2's taller texture, not from layer 1.
- **`mv13` (no multi-view, same replay):** Hmd2's output clean, the two eyes round and full.
- **The fix:** layer 0 tells Hmd2 the texture ends below the eyes (`+0x0c`/`+0x14` = the
  texture's height less the inset rows; Hmd2 sets the T#'s height from them: the same rows and
  layout), layer 1 the whole height. To check with `tools/fov/runs/mv-fix-*.txt`.
- **`mv14` (that fix alone):** still wrong, differently (cones and bulges): gen1.c's
  `restore_game_tsharp` puts the game's own GCN T# (1624 rows) over the one Hmd2 patched from the
  layer, so Hmd2's maths (1080) and the GPU's texture (1624) disagreed. Before the fix both said
  1624, and below the eyes Hmd2 sampled the inset rows instead of the eyes' clamped edge (the
  dark strips and black wedges). Now the restored T# keeps Hmd2's height when it is the smaller
  (multi-view only): layer 0 is the eyes' 1080 rows to the GPU too, layer 1 the whole texture.
- **`mv15` (2026-11-07, that fix and `agent.insetstate`):** the replay's captures of Hmd2's
  output are warped wherever the replayed head looks away from the real headset on the table
  (Hmd2 corrects for it, as check 2 found): the cones and bulges with layer 1 off are that, not
  a fault. With layer 1 on (and with the view states, made on the game thread without trouble)
  large black areas and magnified picture cover the view: layer 1 isn't transparent outside
  the insets. Hmd2's layer path for type 0x10 mode 0 does read alpha (its samples at
  0x2208/0x2308/0x2404 take dmask 0xf), and the game's T# reads alpha from the texture (dst_sel
  w = W). So either the alpha pass's writes don't last until Hmd2 reads the texture (the game's
  GPU writing the frame after the hand-over), or the blend's translation is wrong:
  `agent.insetdebug=1` paints everything outside the insets magenta (alpha 0) to tell.
- **`mv17` (2026-11-07, `agent.insetdebug=1`):** no magenta anywhere, in Hmd2's output or in
  the texture as the TV mirror reads it once the frame is drawn: the pass ran (~35 us a frame) but
  the game's drawing went over it. At the hand-over the game has only submitted the frame (the
  mirror waits ~7 ms more for its "drawn" labels). The pass now goes from the mirror's thread
  after that wait, on Hmd2's queue as before; layer 1 goes while it keeps up.

## Step 4 working (2026-11-07 night, `logs/mv22`-`mv37`, `logs/captures/`)

What it took, in order (each found with `agent.capture` pictures of the TV, `agent.tvhmd=1`):
1. **Writes to the game's texture:** its T# has word 3 bit 26 set (read-only memory type);
   stores through it are dropped without a fault. The pass's copy clears it (self-test: 0 of
   4096 texels written before, all after).
2. **When:** at the hand-over. Hmd2 reads a frame from its next refresh, ~13 ms before the
   game's "drawn" labels; the picture is complete at the hand-over (vr2.c, 2026-11-04). The
   frame's PS VR label is the reprojection's to write, never the game's.
3. **What:** each texel read and written back whole with its alpha; a store of the alpha alone
   wrote the colour as 0 (the headset's picture went black).
4. **Layer 0's height:** Hmd2's maths and the restored T# (gen1.c) both the eyes' 1080 rows;
   with the texture's 1624 Hmd2 showed the inset rows at the top of each eye (the black masks).
5. **Layer 1's place:** Hmd2 shows layer 1 ~0.4 of tangent lower than the angles say, in both
   eyes, with either viewport form, with the real head's pose too (crosshairs,
   `agent.insetdebug=3`). Moved up 0.4 (`agent.insetdy`, 400 by default; 0 since 8, removed
   for the release) the crosshairs meet and the scene runs on across the inset's edge. The
   cause in Hmd2 is not found (the real cause, not Hmd2's: 8).
6. **Placement:** the right inset at the band's right edge (1376) so neither eye sees the
   other's; layer 1's viewports from row 0 with the upward angle extended.

7. **Exposure** (`logs/mv39-eye.log`, 2026-11-07 morning): an inset has no view state, so the
   shaders that apply the exposure (each asks `FViewInfo::HasValidEyeAdaptation`, 0xb4d100, then
   `GetEyeAdaptation`, 0xb4cd90, else a white texture: an exposure of 1) drew it about 3.4 times
   darker. Both getters now go to the agent (multiview.c, their first 14 bytes a jump), which
   hands an inset (Views[2]/[3], pass 0, no state) the left eye's view, as the engine's own
   `GetEffectiveViewState` (0xb4cd10) does for the right eye. Only the readers: the eye adaptation
   passes are added only for a view with a state (0xc6274f, `View.State != 0`), so the insets
   never write the eyes' adaptation. Measured in the texture (mean of the inset against the
   periphery's same window): 110 against 120 and 108 against 117 with it, 36 against 122 and
   35 against 117 with `agent.inseteye=0`. In Hmd2's output the inset no longer shows as a dark
   box (`logs/captures/mv39-hmd.png`: both eyes with it, mv37 without); a faint edge is left.
   (`agent.insetstate=1`, each inset its own view state, had crashed the game on its render
   thread: `logs/mv26-layer.log`, a null write at 0x58, eboot 0x1cb867a; left off. Fixed in 13;
   on in inset mode since 21.)

8. **The periphery was squashed, the real cause of 5 and of the lower ghost**
   (`logs/mv40-grid.log`-`mv42-fixed.log`, `agent.insetdebug=4`: lines every 0.1 of tangent in
   the periphery and the insets): across, the insets' lines fell on the periphery's; down, the
   insets' +-0.3 fell on its +-0.5, and the bottom third of each eye showed the inset rows (the
   lower ghost, with the green grid in it). gen1.c read Hmd2's height for each layer's T# as 16
   bits; it is 14, with other bits above (layer 0's 1080 came out 50232), so the rule keeping
   Hmd2's smaller height (4) never applied: Hmd2's maths had layer 0's 1080 rows, the GPU read
   the texture's 1624. The periphery was squashed to 1080/1624 of its height, its axis 0.44 of
   tangent high, which the 0.4 of 5 had matched. Read as 14 bits: 1080 kept for layer 0, 1624
   for layer 1; each eye whole again, the two grids on each other with no correction
   (`agent.insetdy` 0 by default now), and in the plain picture the scene runs straight across
   the inset's edges in both eyes (`logs/captures/mv42-*-zoom.png`).

9. **Each inset from its eye's place** (`logs/mv46-pos.log`): the user saw the insets' near
   things off sideways (the left eye's to the left, the right eye's to the right) and felt
   cross-eyed. The two insets were the same picture (their best match with each other at no
   shift, the eyes' at 12-18 texels): `FSceneView::UpdateViewMatrix` (0x17abc20), which the
   engine runs after Draw (the plugin's head update, 0x2f914b0), makes the origin again from the
   view's location and adds the eye offset only for pass 1 and 2; the insets are pass 0. The
   agent's function stands in for it (multiview.c `mv_uvm`): an inset is its eye's pass during
   the call. Now the insets' shift is twice the eyes' (30-42 inset texels against 12-24), as
   their density is; 0 again with `agent.insetpos=0`.
10. **What the game draws over the eyes** (`logs/mv44-probe.log`, `agent.probe=1`: the eyes'
   centres and the insets' brightness four times a second from the TV's picture of the texture):
   the game's fades to black are drawn over the eyes after the scene, never into the insets
   (eyes 0, insets ~120 for up to 2 s, five times in 170 s); its UI icons likewise were cut at
   the inset's edge. The alpha pass now compares each 4x4 block of an inset with the periphery's
   texels in the same directions and gives way (alpha to 0 between a brightness difference of
   0.06 and 0.18) where they differ: the periphery, with what was drawn over it, shows
   (`agent.insetmatch`, on; `agent.insetdebug=5` paints where it gives way; off from 11, and
   `agent.insetmatch` removed for the release). Host test: same
   scene alpha 1, a faded periphery 0, an icon 0 under it and 1 away from it; in `mv45` the one
   capture in a fade had the inset dark too.

11. **Brightness and anti-aliasing, off for now** (`logs/mv47`-`mv49`, worn 2026-11-07): the
   block-by-block giving way blurred the inset in patches (now `agent.insetmatch=2` only);
   `agent.insetmatch=1` scales each inset to the periphery's brightness, from samples taken
   once the game has drawn the frame (its fades come after the hand-over) and again on a frame
   handed over twice (its loading screens); `agent.insetaa=1` is a cut-down FXAA into a texture
   of ours before the pass. Worn: the brightness went subtly up and down, and the FXAA looked
   dithered: both off by default, to come back to at the end. Without them the game's fades
   leave the insets bright and their edges are jagged (no TAA without a view state). (Both
   options removed for the release: the view states, 13 and 21, did both jobs.)

12. **The saving and the panel's density** (`logs/mv50-meas-a/b`, `mv51-ppt`): on the replay,
   stock 2048 takes 14.7 ms of GPU a frame (worst windows 15.8-17.8), multi-view (960 + insets at
   twice its density) 7.1 ms (8.0-9.0). PS VR2's panel shows ~1200 pixels a unit of tangent at the
   centre, both ways (0.1 of tangent ~120 of Hmd2's output pixels in the grid captures); stock 2048
   has 734 across, 867 up. The insets' density is now set in those units (`agent.insetppt`, 1200
   by default). Side by side, each eye mustn't see the other's inset through its layer: at 1200
   that caps the window at +-11 degrees (480x480, `agent.multiview=480`, the other inset from 1.0
   of tangent, past the eye's inner 0.947). User's choice: that, widened later by a lower density
   if too small (1100: +-14, 1000: +-17); +-20 at 1200 would need a 1920x4048 texture, never
   registered with the VR port. Worn and in the grid: aligned (lines on each other within a
   capture pixel); the two insets' stereo shift ~3x the eyes' (3.5 expected).

13. **The insets' own view states: the game's TAA** (`agent.insetstate=1`, `logs/ts1-state.log`,
   2026-11-07). The crash in `mv26` was not the render thread's own: the klog's backtrace (game
   base 0x400000) is GameThread_UpdateMIParameter's render task (0x1c94660 -> 0x1c94430 ->
   0x1cb8670, InvalidateUniformExpressionCache) given a null FMaterialInstanceResource. A view
   state makes material instances for the post-process blendables (FSceneViewState vtable
   0x44684f0: slot 6 GetReusableMID, 0xb1e2b0, a pool at +0x630/+0x638 reused by parent, new
   ones from UMaterialInstanceDynamic::Create, 0x1c97800, in the transient package); only its
   AddReferencedObjects (slot 2, 0xb1e1b0) keeps them, and the engine calls that only for the local
   player's two states (ULocalPlayer::AddReferencedObjects, 0x16e1290: +0x98, +0xc0). The
   collector freed the insets' instances (Resources[0] null) while the pools kept them. The agent
   stands in for 0x16e1290 (a 15th patch; the engine's function from the agent's copy of its
   first 20 bytes) and adds the insets' states. Exposure: with a state an inset would adapt to its
   own window, so the redirect (7) now covers insets with a state too, except the histogram eye
   adaptation pass's own target (0xc56910 gets it through the same getter, return address
   0xc56931); the basic pass (0xc57840) uses the getters' bodies, so an inset's adaptation never
   writes the eyes'. Run: 240 s of the replay at 960 with the states from the launch (off 62-74
   s): 60 fps, the game's GPU 7.2-8.0 ms a frame (about the same as without), the kernel log
   clean; worn, the inset's TAA looks good (its edges no longer jagged). 1:1 crops
   (`agent.mirrorboth=2`, `agent.capture=3xx`): `logs/captures/ts1-30x.png`; with
   `agent.inseteye=0` (the inset's own exposure, `ts1-307`) it looks normally exposed too. The
   engine collected only 4 times in that run, all at loading with the pools empty, so the
   collector's side was checked in a second run (`logs/ts2-gc.log`, 210 s, the states from the
   launch, `gc.CollectGarbageEveryFrame=1` for 6 s at 130 s, where `mv26` crashed, and at 180 s):
   the pools held 1 then 2 material instances each from early on (natural collections at about
   20 and 40 s kept them), about 1400 reference passes in the two bursts with 2 and 2 in the pools,
   no crash, 60 fps outside the bursts, the kernel log clean.


14. **The wine bottle's flicker** (worn, `logs/mv55`-`mv57`, parked; its colour found in 23): in the cafe the glass bottle
   flickers in brightness inside the inset as the head moves, never outside it (seen from the
   side, in the periphery, it is steady). Not the shadows (on/off, with and without the insets'
   TAA), not the insets' TAA, not their density (600-1200: so not Hmd2 resampling the dense
   layer), and none of the debug menu's lines (reflections, AO, bloom, motion blur, depth of
   field, the eyes' AA). Left: something the engine does per view for the inset's narrow,
   zoomed frustum: the translucency lighting volume (`r.TranslucentLightingVolume` and
   `r.TranslucencyLightingVolumeDim` are ref variables the agent can't set), level-of-detail
   choice (`r.ForceLOD` isn't in this build; `r.StaticMeshLODDistanceScale=0.01` tried at the end,
   not judged), or refraction offsets in screen space.
15. **The insets following the gaze** (`agent.insetgaze=1` with `agent.gaze=1`; 2 sweeps, 3-6
   hold left/right/up/down for tests; `agent.multiview=960 agent.insetsize=480`): the two
   insets one above the other on a diagonal of the 960 inset rows (`agent.insetlayout=0`: the
   right eye's on top at the right, the left eye's below at the left; `=1` the other diagonal;
   this stacked layout, replaced by the window test in 19-20, and `agent.insetlayout` were
   removed for the release).
   Hmd2's type 0x10 layer has no window test, so each eye's layer samples the texture far
   outside its window: the other eye's inset showed as a skewed patch (~1.2 of tangent across,
   0.4 up) in the eye whose inset is the lower one (`logs/mv59`-`mv62`, `agent.insetdebug=6`, now 5,
   tints each inset). Each eye's T# is cut at its own inset's right and bottom edges (vr2.c),
   past which the border sampler is transparent. The cut width first didn't hold: gen1.c
   converted a T# a second time when another dispatch shared the constants, read the GCN
   width bits as gfx10's (a 480 as ~1918, a 1440 as ~5760, the texture's 1920 put back), so the
   GPU sampled the whole width while Hmd2's maths had the cut one: the eye's inset 1/4 or 3/4
   as wide and the other inset in view (the height sits in the same bits in both, so height
   cuts always held). gen1.c now leaves a T# alone that is already the game's GCN one.
   Checked on the replay (`logs/mv65-ct`, `mv64-gr`, `mv67-fx`): both layouts, centre and
   the four held positions, both eyes: no other-eye inset, each eye's inset the same size,
   the grid's lines continuous across every inset edge. `agent.insetcut` (debug): 1 not cut,
   2 the old cut at the other inset's edge (removed for the release; history).
16. **Worn with the gaze, and what followed** (2026-11-07, `logs/mv68`-`mv85`):
   - *The window shook* with the tracker's scatter: smoothed by its speed (`agent.gazefilter=1`,
     the "1 euro" filter: 0.1 Hz at rest, +1 Hz a tangent a second; tuned on a simulation of
     +-0.02 of tangent scatter: ~0.3 degrees of wander at rest), a jump past twice
     `agent.gazehold` taken at once. A hold (still within 0.05 of tangent) had moved it in
     steps when a look was held while the head tilted.
   - *The picture flashed as the window moved:* Hmd2 had the newest window the game thread
     made, the frame shown one or more behind it. Each frame's window is now kept with the head
     pose it was made with (the game thread's tracker result), and the hand-over finds its own by
     the pose it carries: the render thread's late pose, asked ~6 ms after the game thread's
     while that frame is the newest (`logs/mv71-fp`: the hand-over 3 frames behind the newest
     views). `logs/mv72-fp` and the worn runs: every lookup found its frame, none the newest.
   - *The inset darker toward its edges:* each inset view had the engine's lens vignette of its
     own (FinalPostProcessSettings.VignetteIntensity, FSceneView +0xa48, from
     EndFinalPostprocessSettings 0x17af4a0). Off in every view now (`agent.vignette`).
   - *TAA ghosted* in the moving inset: the insets' anti-aliasing is the engine's FXAA
     (`agent.insetaam`, AntiAliasingMethod at +0xb0c; without a view state the engine had already
     made their TAA its FXAA).
   - *The delay before the inset is sharp after a fast look:* the game's pipeline (the window is
     fixed as a frame starts, shown ~3 frames, ~50 ms, later) and the tracker's latency; big
     jumps are taken in a frame. 90/120 fps would shorten it.
17. **The window's size** (`logs/mv76`-`mv79`, replay): 600x600 (+-14 degrees, `agent.multiview=1200
   agent.insetsize=600`) and 960x600 (`agent.insetwidth=960`, +-22 by +-14 degrees at 1200 a
   tangent: eyes move sideways most, and the rows are what the texture has least of): no
   other-eye inset at five gaze positions in both layouts, the grid continuous across every edge.
   The panel shows ~1200 pixels a tangent at the centre (13 above): more adds no detail.
18. **The balance at 60 fps** (`logs/mv80`-`mv85`, replay, the window moving): the game's GPU a
   frame: 960 periphery with 480x480 insets 7.2 ms, 600x600 7.8, 960x600 8.3; 1080 periphery with
   1080x544 8.9; 1200 with 1200x476 9.0; stock 2048 13.8. All held 60 with no frame lost. The
   limit is the texture's height (the eyes and two insets one above the other, the console's
   largest 2304), not the GPU. On the headset 599 of 600 frames stayed two refreshes; the rest
   were scene loads (one ~98 ms GPU frame, in stock too) and one frame a 10 s window, first
   blamed on the agent's own 10 s report made in the game's hand-over (moved to the agent's cvars
   thread, though that one frame stayed: 19). The "three or more refreshes after" count of the hand-overs (~1.5%, in stock too)
   is the hand-overs against the agent's refresh count, not what the headset shows.
19. **The insets' layer with Hmd2's window test** (`agent.insetwin`, 2026-11-07, run the same day
   below; 3 in inset mode; side-session note `hmd2-type11.md`): type 0x11 is a mono, head-locked quad (one texture
   for both eyes, not reprojected), no use for stereo insets. A type 0x10 record registered
   with byte +0x44 = 1 is: SetParam (0x202c2) copies it into the descriptor's +0xcd, the
   compositor (0xaddc) sets the layer's flag bit 0, and the shader then tests each channel's
   window uv against 0..1 (0x2b4c), skips the lanes outside, maps u and v through two 1D lookup
   tables an eye and applies the viewport. With an identity table (RGBA16F, 1024 texels, each
   its own centre: exact) that is type 0x10 with a window: each eye's inset layer draws only
   inside its own window, so the insets can sit side by side in one band of rows
   (`agent.multiview` = the inset's height), no per-eye cuts, half the rows. Registration
   (VideoOut 0x11920 -> Hmd2 0x1b7a0) checks only counts, free slots, non-zero bases and zero
   bytes; a refused one keeps its set and buffer slots, so the fallback registers without the
   record on the next set and buffers. The translation of 0x2b4c (SDWA compares to SGPRs, 1D
   samples) is in rdna2gcn.c's rules but has never run. Stages (tools/fov/insetwin_test.c
   checks the table, its T# through gen1's conversion and the record offline): 1 registered
   only; 2 the layer on it with both windows off view (the tables never sampled); 3 in use,
   960x600 insets side by side (`agent.multiview=600`), tints and grid.
   Run 2026-11-07 14:09-14:22 (`logs/mv86`-`mv89`, replay): all four clean (no crash), the
   registration with the record accepted each time, Hmd2 took the layer on it. Side by side with
   the window test: no other-eye inset in either eye at five gaze positions (the only red, a
   ceiling lamp's colour fringe in the periphery), the grid continuous across every edge, 60 fps
   with no frame lost, the game's GPU 7.4 ms a frame (8.3 stacked: a 1680-row texture). The one
   frame a 10 s window held one refresh stayed after the 10 s report left the hand-over (the
   report takes ~20 us): its cause is elsewhere.
20. **A sharper periphery with the window test** (2026-11-07, `logs/mv9x-*`, `mv9y-*`, replay):
   at 1200 and 1440 (screen percentage 125 and 150) the insets first went inside the eyes'
   rows and no wider than 960: the options' rects are the engine's unscaled ones (every earlier
   run was at 960, scale 1). Fixed (0b4a9ba): the insets in the texture's pixels, the inset
   views' rects set as the engine sets an eye's. Then, 1200x600 insets side by side
   (`agent.multiview=600 agent.insetsize=600 agent.insetwidth=1200 agent.insetwin=3`, +-27 by
   +-14 degrees at 1200 a tangent): no other-eye inset at either resolution, the grid
   continuous, 60 fps with no frame lost; the game's GPU 9.9 ms a frame with a 1200 periphery
   (2400x1950 texture), 11.4 ms with 1440 (2880x2220; the periphery 516/610 a tangent, 70% of
   stock 2048's), stock 2048 13.8. (The 1080/1200 figures in 18 came from the misplaced
   insets.)

21. **The game's fades: post-process materials, which need a view state** (2026-11-07,
   `logs/mv97-fade`, `mv98-fade`, `mv99-state`, replay, inset mode from `agent.multiview=600`,
   `agent.probe=1`). The insets kept their brightness through the game's fades to black and
   back (the TV's probe: the inset ~182 while the eyes went 167, 122, 76, 30, 0; worn too).
   - Not drawn after the hand-over: `agent.insetfade` (inset.c, `fadefit.h`, host test
     `tools/fov/fade_test.c`; all three removed for the release) samples the periphery at the hand-over and again once drawn and
     fits after = m before + b; in 8400 frames of the replay it found none (the two the same
     bytes in every fade). Kept, off by default (removed for the release; history).
   - Not the camera's fade: the views' OverlayColor (CalcSceneViewInitOptions, 0x16dd810,
     copies the camera manager's FadeColor +0x3ac and FadeAmount +0x3bc into the options'
     +0xe0/+0xec when bEnableFading, +0x1488 bit 1; logged by `mv_opt_wrap` on a change) stayed
     0 through the run.
   - Post-process materials: the engine gives a view the blendables only with a view state
     (`UMaterialInterface::OverrideBlendableSettings` returns at `!View.State`), and the insets
     had none. With `agent.insetstate=1` (13: each inset its own, its material instances kept
     from the collector) the probe during fades: inset/eye median 1.000, worst 0.973, none of
     29 off by 10% (without: median 1.18, worst 9.9, 15 of 27); steady 1.002; 60 fps, frames
     held as before, the game's GPU ~0.3 ms more a frame, no crash. On by default in inset mode
     now, its anti-aliasing still the engine's FXAA (`agent.insetaam=1`; TAA only with 2). The
     game's yellow outline round a chosen item is one too: worn, it shows in the insets with
     the view states.
   - Still different: stretches where the left eye's probe reads black and the right's inset is
     ~1.5x its periphery (104-112 probes a run, both with and without): something drawn over the
     eyes alone, likely the game's UI; to look at in captures.

22. **The eye's own bloom and AO in the insets** (`agent.insetpp`, 2026-11-07, `agent/insetpp.c`;
   on the console: runs 1-2, the edges and the lights below). Unreal makes both per view from what that view sees, so
   an inset's were its own: the glow round a light stopped at the inset's edge (bloom is a blur
   over the whole view, and the inset can't see a light outside it), and its kernels and AO
   radius were sized to its own narrow view (11; sizing them to the eye's angles, `agent.insetfx`,
   still left the cutout; removed for the release). Inset mode had both off. Now the inset takes the eye's own, at each of
   its pixels' directions:
   - *How 4.12 wires them* (read in the eboot). The tonemapper (`FRCPassPostProcessTonemap::Process`,
     0xcb1620; three inputs: the scene, the combined bloom and lens flares at half the buffer's
     size, the eye adaptation) samples input 1 at the scene's UV (the place in the buffer over the
     buffer's size) and adds it times the view's BloomIntensity (ColorScale1 from view +0x888)
     before exposure and tonemapping; its pixel shader takes each input's size from the pooled
     target's own description. SSAO: `FCompositionLighting::ProcessAfterBasePass` (0xbed520), view
     by view, runs the AO passes (0xbedcb0 adds them; the last one, its `bAOSetupAsInput` +0xdc at 0,
     writes `ScreenSpaceAO`, SceneContext +0xd8: G8, the buffer's size, each view its own rect),
     then the base pass AO (vtable 0x446c258) and the ambient cubemap; the sky light and
     reflections read it later, each pixel its own texel. The views go 0-3 in each pass: both
     eyes before the insets.
   - *Bloom:* before each eye's tonemapper runs, the agent draws that eye's bloom texture (its
     input 1) sampled at each inset pixel's direction into F, a texture the buffer's size (fp16,
     from the engine's render-target pool), at the inset's own pixels and a one-texel ring (not
     over the other inset); before each inset's, F becomes its input 1's pooled target for the
     call (put back after it returns, before the graph lets its inputs go). The inset's
     tonemapper then reads F's own texel at each pixel: the eye's tonemapper's bilinear sample at
     that direction, with the eye's exposure (7) and the eye's BloomIntensity (left alone: the
     agent's sizing and strength options don't touch an inset's bloom while bit 1 is on). F goes
     back to the pool after the right inset's tonemapper. Written into the inset's own half-size
     bloom texture instead, the values were interpolated a second time by the inset's tonemapper:
     up to 4% of the bloom off round a small bright light (the host test's first version).
   - *AO:* after an inset's last AO pass, the agent draws the eye's rect of `ScreenSpaceAO`,
     sampled at each inset pixel's direction, over the inset's rect, before the base pass AO and
     the rest read it.
   - *The mapping:* an inset's projection is its eye's cropped (mv_opt_wrap): NDC_eye = s NDC_inset
     + c each way, read from the two views' matrices (FViewInfo +0x70) that frame. The renderer
     adds the TAA jitter into an eye's matrix and keeps it at +0x1e4/+0x1e8 (0xb54648): bloom is
     made from the TAA's output, so its mapping leaves the jitter out; AO is made from the frame
     as drawn, so its mapping keeps it (0.22 of a texel here).
   - *The draws are the engine's own,* recorded into the pass's command list as its passes record
     theirs: SetRenderTarget (0x8fa350, with its writable barrier), the viewport, its static blend,
     rasterizer and depth states (the list's state filters kept, +0x98/+0xa0),
     SetGlobalBoundShaderState (0x18e7910) with FScreenVS/FScreenPS from the global shader map
     (types 0x4af8a48/0x4af88e8, the map's hash walked as the engine's inline code does), the
     pixel shader's texture and sampler, DrawRectangle (0xd2acc0) and CopyToResolveTarget; F from
     `FRenderTargetPool::FindFreeElement` (0xd22600). The RHI keeps every GPU state and
     transition as for its own passes; nothing of the agent's runs on the GPU here.
   - *Exactness* (`tools/fov/insetpp_test.c`: the hooks against a fake engine, then the GPU's
     sampling simulated with the draws they recorded, lights of 2-300 texels inside and across the
     windows' edges): the inset's bloom is the eye's at its pixels to within fp16's half step
     (0.049%; with the texture unit's 8-bit filter weights, 0.18%, its own precision); its AO to
     within half of G8's step. Nothing outside the insets' rects and rings is written; the eyes'
     own AO is untouched.
   - *Cost:* F is the buffer's size times 8 bytes (48 MB at 2880x2220), held each frame from the
     left eye's tonemapper to the right inset's; two draws of ~0.7 Mpx for bloom and two for AO
     (run 1c: nothing measurable). The insets' own bloom and AO still run (their
     textures carry the eye's: the inset's own bloom texture is where F stands in, the AO pass
     writes the rect the agent then covers).
   - *Patches:* the hooks make 19 (both tonemappers: the game runs 4.12's film tonemapper,
     `r.TonemapperFilm`, Process 0xcaebb0, made by AddTonemapper 0xc67d00 with inputs scene, bloom,
     eye adaptation, combined LUTs; the older one, 0xcb1620, only without it), 20 with the insets'
     eye adaptation skip (24); `AGENT_PATCH_MAX` is 20 in the agent and the payload, so inset mode needs the new payload (an older one refuses
     the whole table: inset mode off).
   - *Run 1* (`logs/pp1-rec1.log`, rec1 replay 240 s, with only the older tonemapper hooked): no
     crash, 60 fps, the game's GPU 12.4-12.7 ms a frame; the 18 patches in; FScreenVS/FScreenPS
     found (DrawRectangle's parameters bound); AO from the eyes ~1200 draws a 10 s window (two
     insets at 60), none skipped; bloom never reached (0 resampled, 0 skipped): the game runs the
     film tonemapper, hooked since. Probe, inset/eye: with the eyes' AO (bloom still the insets'
     own) median 0.998, 80% within 0.991-1.009, 73 of 1063 off by 5%; the insets' own bloom and AO
     median 1.016, the top tenth up to 1.71.
   - *Run 1b* (`logs/pp2-rec1.log`, both tonemappers hooked): clean; the film tonemapper reached
     each view, but its bloom input is 180x139, a 16th of the buffer (the game's
     `r.BloomQuality` 1 stops at a small stage), and the agent only took a half-size one: every
     bloom skipped. Now any power-of-two fraction is taken (the mapping never assumed one; the
     host test runs both). The game's GPU with its own bloom and AO back on: ~12.7 ms a frame in
     steady play (both runs), against 9.3-9.5 with them off (item 20's runs since d6f1e1f).
   - *Run 1c* (`logs/pp3-rec1.log`, 7e526ab): working. Both eyes' bloom (180x139, a 16th) taken
     into F every frame and F given to both insets (~1200 of each a 10 s window, none skipped), the
     AO likewise; no crash, 60 fps, frames two refreshes each on the headset as before. Probe,
     inset/eye luma, leaving out the stretches where one eye's probe reads black (drawn over one
     eye alone, 21: ~100 samples a run, there with and without): with the eye's bloom and AO
     median 0.999, 5-95% 0.988-1.008, 16 of 950 off by 2%, 3 by 5% (worst 0.904); with the insets'
     own (agent.insetpp=0, the game's bloom 1 and AO 2) median 1.013, 5-95% 0.995-1.032, 87 of 284
     off by 2%. The game's GPU 12.6 ms a frame in steady play with insetpp 3, 12.9 with 0: the
     agent's draws cost nothing measurable here.
   - *Run 2* (`logs/pp4-hold.log`, the cafe's held view, the headset's picture on the TV): clean;
     the eye's and the insets' own alike there (captures within the same-setting differences).
   - *The edges* (`logs/pp5-edge.log`, rec1, `agent.probe=2`: each inset's four bands from 70% of
     its size out to its fade edge against the eye at the same directions, one-eye-black
     stretches left out): with the eye's bloom and AO, medians 0.998 (left), 1.000 (right), 0.999
     (top), 0.999 (bottom), 5-95% within 0.978-1.021; with the insets' own, 1.011, 1.028, 1.009,
     1.019, the right edge's 5-95% 1.002-1.069 (56 of 246 off by 5%). A few outliers up to 2.3
     in both (not bloom or AO).
   - *Lights at the edge* (`tests/rec4-lights`: the user held the headset, the inset centred
     (`agent.insetgaze=0`), and moved the cafe's lamp and the window across each edge for 150 s,
     with the eye's bloom and AO and the headset's picture on the TV: the edge couldn't be told
     apart on the TV; `logs/pp8-rec4.log`. Replayed with the setting 3 and 0 in turn every 15 s,
     `logs/pp9-rec4ab.log`): samples off by 5%, inset/eye, the eye's against the insets' own:
     middle 1 of 506 against 17 of 204; left edge 7 of 484 against 95 of 200 (its 5-95%
     0.911-1.123); right 11 of 496 against 31 of 202; top 17 of 491 against 46 of 202; bottom 27
     of 498 against 25 of 202. Medians with the eye's 0.997-0.999 everywhere. The few left with
     the eye's (worst 1.68 top, 2.17 bottom) are there as often with the insets' own: not bloom
     or AO. (An earlier recording, `tests/rec3-lights`, had the bloom level changed from the debug
     menu during it, which its replay repeats: not used.)
   - *Settings:* `agent.insetpp` (live, debug builds only since the release; 3 in inset mode: 1
     bloom, 2 AO; 0 the insets' own, for an A/B), in the debug menu as "Inset bloom/AO: the
     eye's". With a bit on, inset mode leaves that effect's cvar at the game's value
     (`r.BloomQuality`, `r.AmbientOcclusionLevels`: no longer set to 0, nor kept there), and
     `agent.insetfx`/`insetbloom`/`insetflare` left that effect of the insets alone (those three
     removed for the release; history). The 10 s report: "insetpp: the eye's bloom: N resampled, N into insets...".
   - *Confirmed on the console* (runs 1-1c and 2, the edges, the lights): the FScreenVS/FScreenPS
     line, the eyes' bloom format (a 16th of the buffer, 1b) and F, the draws and the AO line;
     inset/eye ~1.000 with the eye's bloom and AO, at the insets' edges too and with lights
     across them, which bears out the tonemapper sampling input 1 at the scene's UV and the AO
     readers taking the pixel's own texel; the GPU time (1c). Still unverified: that the game has
     no bloom dirt mask (sampled at the view's own UV, it would be the inset's); none has been
     seen.

23. **The wine bottle's colour: the left eye's translucency lighting volume** (`agent.insettlv`,
   2026-11-07, `logs/tv1-bottle`, `mv100`-`mv103`, `tests/rec2-bottle`, `rec2-bottle-hold`).
   Glass is lit from a volume the engine fits to each view's frustum
   (`FViewInfo::CalcTranslucencyLightingVolumeBounds`, 0xbc0bf0); its caller already gives the
   right eye the left eye's, an inset fitted its own to its narrow off-axis window. The bottle in
   the cafe was browner in the inset; the eye's own the same with the insets on or off (TV
   captures 103/104). The agent's `mv_tlv` gives an inset Views[0]'s bounds. The held view
   (`tests/rec2-bottle-hold`: rec2-bottle's head held from 56.5 s), on, off, on, off, three
   captures each: inset/eye 1.06 1.02 0.92 with it, 0.82 0.73 0.67 without (the eye's 97 68 38
   throughout). Worn: fixed. (A single capture can show the frame before a setting changed:
   `mv100`'s two read the wrong way round.)

24. **Optimisation** (2026-11-07). The inset pass covers only the insets' rows with Hmd2's window
   test (layer 1 shows nothing outside the windows, layer 0 reads no alpha): 75-231 us of GPU a
   frame against 238-837 (`mv101`-`mv103` against `mv99`). The sweep on `tests/rec1` (bloom and
   AO off, `logs/opt-a`-`opt-c`), the game's GPU in steady play, average / the 10 s windows'
   longest: 1440 eyes with 1200x600 insets 9.5-9.7 / 11.7-12.8 ms; 1600 eyes 10.8-11.0 / 12.8-13.8
   (+1.2); 1440x720 insets 10.3-10.4 / 11.9-13.8 (+0.7); 60 fps and the same held frames in all,
   no memory refused beyond the game's usual CPU-pool retry. With bloom and AO back (22) the game
   took ~12.6-13.0 ms at 1440/600, nearly all the game's own bloom and AO in four views. The
   insets' own eye adaptation passes are now left out (`agent.insetea`, on by default: the
   insets take the left eye's exposure, 7 and 13, so their own adaptation was never shown; a 20th
   patch, `mv_ea_skip`): 0.1-0.2 ms. With both, on `tests/rec1` (`logs/opt-d`-`opt-f`, the
   eye's bloom and AO): 1440/600 12.1-12.3 ms in steady play (13.4-13.9 the longest frame of a
   window; 14.9 in the heaviest scene); 1600 eyes 13.7-13.9 (16.8-17.1 in the heaviest scene);
   1440x720 insets 13.1-13.3 (16.5 there). Either step up leaves no room in that scene: the
   defaults stay 1440 and 600. Inset mode sets r.ShadowQuality=5 and r.PostProcessAAQuality=6
   (the highest) and r.AmbientOcclusionLevels=3 (the game's 2; worn 2026-11-08, 1 showed the
   AO's dither in the shadows, 3 the smoothest), and r.BloomQuality and
   r.AmbientOcclusionLevels to 0 only when `agent.insetpp` leaves them off (`apply_cvars`). The game resets r.BloomQuality,
   r.ShadowQuality, r.AmbientOcclusionLevels and r.PostProcessAAQuality at scene changes: the
   agent puts back what it set within 0.5 s (`cvars_keep`; the menu's and cvars.txt's too).

25. **The TV mirror's black frames** (2026-11-07, `logs/tv-fix`-`tv-fix4`, `agent.probe=1`). The
   raw view (`agent.mirrorboth=1`) showed the left eye's upper part black in some captures
   (`mv102`, `mv103`), and the probe read the left eye black on the TV in 55-108 of its samples.
   The mirror picked a frame, waited for the TV's buffer (up to a refresh), then copied that
   frame, by then a frame late: in the late part of a slow beat the game had begun drawing that
   texture again, and caught its black 1920x1080 fill. Now the mirror takes the newest frame
   after that wait, copied once the labels reach its count - 1 (complete: docs/FRAME-COPY.md),
   and holds back a copy with a sudden area of exact black for up to half a second: 0 of 669
   probe samples black (`tv-fix4`). The headset was never affected.

26. **Objects popping in the insets as they move** (2026-11-08, worn on Allison and Hikari). As
   the gaze moved the insets, the curtains flashed black and objects on the desk popped; with
   the LOD scale above 0.1 far objects popped in from the periphery. Two per-view decisions
   the engine makes from an inset's own view:
   - **Occlusion culling** (gone with `r.AllowOcclusionQueries=0`, worn). Since 21 each inset
     has a view state, so the engine keeps its occlusion history: what was hidden in the
     inset's last frames is skipped, the answers arriving a frame or two late. The window moves
     with the gaze, which the engine does not see as a camera move (its reset needs
     `bCameraCut`, FSceneView +0x758, or a large turn), so objects outside the old window were
     skipped for a frame or two. PreVisibilityFrameSetup (0xb53f20) asks DoOcclusionQueries
     (0xb4d530) per view and, told no, sets FViewInfo +0x17e0 bits 0x4|0x8 (ignore the old
     answers, send no new tests): the agent answers no for an inset (`agent.insetocc`,
     `mv_occ_skip` through a pad, the view at r13 + r15). `bCameraCut` itself was not used:
     16 functions read it (the post-process chain, the view's uniform buffer), which would
     have set the insets apart from their eyes.
   - **LOD** (ComputeLODForMeshes, 0x17a7770). A mesh's level comes from its screen size in
     the view, PI r^2 / the view rect's area with r from max(W M00, H M11) / 2, clamped to 1,
     against each level's threshold times `r.StaticMeshLODDistanceScale`: a fraction of the
     view, not pixels, so resolution doesn't change it, the field of view does. An inset's
     window is about 1/27 of its eye's angular area: a finer level in the inset than around it,
     switching as it passed. Scaling the LOD scale for the insets would not be exact (the
     clamp); the insets are drawn from their eyes' places with their rotations, so the agent
     answers an inset's call with its eye's view (`agent.insetlod`, Views[2] -> [0], [3] -> [1]):
     the eye's choice exactly, its dithered transitions too.
   23 patches (AGENT_PATCH_MAX 24, the payload's too); the sites mapped in all three games.
   Host-tested (`tools/fov/multiview_test.c`: each game's stock bytes, the call on its pad, the
   eye per inset). First run (`logs/ssr1-bottle`): both logged, no crash.

27. **The eye's reflections in the insets** (`agent.insetpp` bit 4, 2026-11-08). Screen-space
   reflections (`r.SSR.Quality`, the menu's Reflections; the game's own is 0, off) trace each
   view on its own (FRCPassPostProcessScreenSpaceReflections::Process 0xd4da30, from
   ScreenSpaceReflections 0xd4f3a0 in the reflection composite's loop over the views, each view
   an output of its own the buffer's size, PF_FloatRGBA): an inset reflected only what its own
   window held. Their cost on the café's held view (`logs/ssr1-bottle`, 10 s windows): 12.7-12.8
   ms a frame off, 14.0-14.1 at quality 2, 14.1 at 4 (worst 15.3-15.7 against 13.9-14.3), no frame
   lost. As AO (22): after an eye's trace, the block of its texels its inset's pixels fall on (a
   texel round it) is copied 1:1 into S, a target of the agent's held per eye (its size rounded
   up to 64: 2 MB); after the inset's trace, S is drawn over the inset's rect of its output,
   bilinear at each pixel's direction in the jittered frame. Holding each eye's whole output for
   its inset would keep two more 51 MB targets in the pool. Host test (`insetpp_test.c`): the
   eye's own value at every inset pixel (0.016% one rounding step off, at float boundaries), no
   unwritten texel read, S kept from frame to frame and let go with the bit off. 24 patches
   (AGENT_PATCH_MAX 24: full). On the console (`logs/ssr2-eye`, the café's held view, quality 2):
   S 512x320 for a 450x268 block, every inset drawn from it (1234 of 1234 in 10 s); 14.2-14.3 ms a
   frame with the eye's, 14.1-14.3 with the insets' own, 12.8-12.9 with reflections off, none
   lost. The edge probe (`logs/ssr3-probe`, `agent.probe=2`) reads the same with either (median
   step 0.7%, none over 5%): this view's surfaces at the insets' edges reflect too little to tell;
   to be worn where a glossy surface crosses an inset's edge.

28. **Inset mode at the native 90 and 120** (2026-11-08, `logs/inset-90-120/`, replays of
   `tests/rec1`, headset on the table, Hikari; each run a whole replay, ~4 minutes with its
   heavy scene 60-150 s in and its scene loads; `-hold`: `tests/rec2-bottle-hold`, 150 s; the
   gaze sweep: `agent.insetgaze=2`, the insets moving every 2 s). Judged by the headset's own counts (each frame
   one refresh at a native rate; held two = a repeat, never shown = a drop), not the game's GPU
   time, which stops meaning anything once the game is GPU-bound (it read 2-3 ms then).
   - *How it runs:* the plugin's two stores of its frame sequence (docs/SUMMER-LESSON.md, "Frame
     rate") are in the agent's own patch table (`multiview.c` `build_fps`), applied by the
     payload with the rest at the entry point (the payload then leaves its own NATIVE patch
     out: both would leave the table on bytes not the stock ones). Two slots of their own:
     `AGENT_PATCH_MAX` 26 in the agent and the payload (24 at 60, so an older payload still
     takes 60's table; 26 at 90 and 120 needs this payload). The agent paces natively
     (`frame_pace` 1, `refresh_hz`) only once the stores read back from the game's code after
     the boot (`multiview_native_confirm`): a refused table leaves 60. The rate is the payload's
     `want_fps` (framerate.txt, given before the agent's boot). The host test
     (`multiview_test.c`, `native_check`) runs all three games at both rates.
     (The measurements below ran before that, on the resident payload of the day with 24 slots:
     the stores then took the older tonemapper's and the reflections' slots, and the rate came in
     `agent.multiview=N` from 2048. No NATIVE payload was sent for them: its send, a swap, took
     the console down at 05:13 that day, before any game; its start-up is the normal one's code,
     so the swap itself again.)
   - *Choosing the rate:* `framerate.txt` (60, 90 or 120), or the debug menu's first line, "Frame
     rate, next start": the rate can't change while the game runs (the eyes, the insets' rows
     and the plugin's frame sequence are fixed at its start), so the agent's cvars thread asks
     the payload to save the choice (`CMD_FRAMERATE`, 13) and the payload says so in a PS5
     notification; it applies from the next start. A payload before this command never answers
     (the agent says so in its log). The settings menu's "Frame rate" line (release builds) is
     the same.
   - *Per-rate defaults* (`multiview.c` `k_rate_presets`, the payload's `MULTIVIEW_DEFAULT_*`):
     60: 1440 eyes, 600 rows (1200x600 insets); 90: 1200 eyes, 540 rows (1080x540); 120: 960
     eyes, 480 rows (960x480); AO 3, shadows 5, AA quality 6 at all three (3 on the newer
     engine). (The eyes per game since the sweep with the palm tracker on, `k_rate_eye`:
     SUMMER-LESSON.md; these stay Hikari 1.00's.) `resolution.txt` of
     1600 or less still sets the eyes at any rate, `cvars.txt` any of the rest. The insets'
     density stays 1200 a tangent: their windows +-27 x +-14 degrees at 60, +-24 x +-12 at 90,
     +-22 x +-11 at 120 (`agent.insetppt` trades that for sharpness at no GPU cost).
   - *The engine's cap at the headset's refresh, 119.88 / 89.91* (hooks.c): at 120.0 / 90.0 the
     game made one frame more than PS VR2 showed every ~1100 frames (9.4 s at 120, 12 s at 90),
     two handed over in one refresh and the older never shown, with the hand-off thread on time
     (`n90-b`: 11.9 ms since the last pass at each). At 89.91: none in 21 play windows (`n90-c`).
     The same 1.001 at 60 (the game's own 60.0 bound, the headset's 59.94) was 18-19's "one frame
     a 10 s window": the cap is 59.94 there too since 086599b (`logs/cap1-5994`, `cap2-60`).
   - *What holds* (play windows; the scene loads, the game's own, left out):

   | rate | eyes | rows (insets) | runs | play frames on 1 refresh | held 2 | never shown |
   |---|---|---|---|---|---|---|
   | 120 | 960 | 480 (960x480) | `n120-f`, `n120-g` (gaze sweep), `n120-hold` | 25523, 25568, 13516 | 3, 3, 3 | 0, 0, 0 |
   | 120 | 1080 | 480 (960x480) | `n120-a`*, `-c`, `-d`*, `-e` | 30411, 24458, 24528, 30395 | 0, 536, 518, 3 | 26*, 3, 8*, 0 |
   | 120 | 1080 | 600 (960x480 .. 1200x600) | `n120-b`* | 22673 | 1489 | 2 |
   | 90 | 1200 | 540 (1080x540) | `n90-b`*, `n90-c`, `n90-e` (gaze sweep), `n90-hold` | 23692, 18257, 20077, 10148 | 1, 0, 0, 1 | 22*, 0, 0, 0 |
   | 90 | 1320 | 540 (1080x540) | `n90-a`* | 18165 | 52 (heavy scene) | 18* |
   | 90 | 1320 | 480 (960x480) | `n90-d` | 19201 | 8 (7 in the heavy scene) | 0 |

   (* the cap at 120.0 / 90.0: those never-shown frames were the cap's, one each ~1100 frames.)
   1080 eyes at 120 is the edge: two runs held, two fell behind in the heavy scene and mostly
   stayed behind after it (the hand-over 4-6 ms after the refresh against 2-3, frames held
   two refreshes ~2%); 960 held in both runs. AO's cost at 120 (`n120-e`, alternated every
   30 s): ~0.5-0.9 ms (AO 0 6.5-7.1 ms, AO 3 7.3-8.0; AO 1 no cheaper than 3). Rows the insets
   don't use still cost (the buffer is that much taller: `n120-b`).
   - *Memory:* every run (scene loads, both rates) kept flexible memory at 295 MB free and took
     no GPU block from the extra segment; the buffers are smaller than 60's (2400x1890 at 90,
     1920x1560 at 120, against 2880x2220), which ran 66 minutes and more (`logs/play-b2ac6a7`).
     No session with the day change was replayed at 90 or 120 (no such recording).
29. **60 fps at the headset's half rate** (2026-11-08, `logs/cap1-5994`, `cap2-60`, `cap3-default`).
   PS VR2 refreshes at 119.88 Hz, so the game's own cap of 60.0 made frames a little faster than
   the headset took them: one or two frames each 10 s window were shown for one refresh instead
   of two (a short frame, a judder; the "one a window" of 18-19). The engine's cap (GEngine
   +0x634) is now 60000/1001 = 59.94 at 60 (`agent.cap=N` overrides it, N thousandths from
   1000, at any rate): on the café's held view 0 short frames in 12 of 12 windows and 15 of 16
   (against 1-2 in every window at 60.0), none lost, the time step 16.684 ms and steadier (sd
   68-123 us against 124-131), the GPU as before. Found at 90 and 120 by the 90/120 work
   (`claude/inset-90-120-ca1a49`: 119.88 and 89.91 there).

30. **The insets' anti-aliasing: TAA in every view, our own blend** (2026-11-08, `logs/aa1-hold`,
   `aa2-dump`, `aa3-code`, `aa4-block`, `aa5-ours`, `aa6-walk`). The user: FXAA in the insets is too
   jagged, their TAA ghosted (16, `mv74`), the two must not differ; no ghosting, blur, jaggedness or
   shimmer as far as can be. Inset mode now gives the insets the eyes' TAA (`agent.insetaam=2`) and
   runs our TAA in all four views (`agent.taa=2`); the menu has "Inset AA" and "TAA shader".
   - *The engine follows the window.* PreVisibilityFrameSetup (0xb53f20) keeps each view state's
     previous matrices (+0x1a0) as the last frame's, the inset's crop included (Prev = Pending,
     Pending = this frame's: 0xb53fa4, 0xb54039; the view's copy +0x17f0, its PrevViewProj
     +0x1970 from the previous projection); the velocity pass draws every view
     (RenderVelocitiesInner 0xbe3af0). A check on the console (`agent.insettaa`'s 10 s line): with
     FXAA (no jitter) the insets' previous projection was the frame before's in 1212 of 1212
     frames. The window moving is a camera turn to the TAA. The `mv74` ghosting came with the
     insets' occlusion and LOD popping (fixed in 26, which TAA smears into trails) and the stepped
     gaze hold (16).
   - *The engine's TAA shader, read from the game's memory* (`agent.taadump`: the RHI pixel shader's
     CPU block, its code at +0xa4, its Gnm::PsShader at +0x34 with the stage registers at +0x3c;
     `tools/fov/taadump.py`, `tools/gcn7dis.py`, a gfx7 disassembler whose tables are LLVM's own,
     checked on 820 instructions): FPostProcessTemporalAAPS<1,0> (static type 0x49f0430; Chisato
     and Allison 0x4a94570), 2380 bytes, 64 VGPRs, the user data a depth T#, its sampler and a table
     at s[12:13] (the view uniform buffer read at 1728 bytes: ClipToPrevClip). It dilates the
     velocity to the nearest depth of a neighbourhood, takes the velocity buffer where something
     moved and the camera's reprojection elsewhere, tests off-screen and clamps the history's
     place against the *viewport* (so an inset's history never reads past its rect: the band of
     `agent.insettaa`, below, never reached the picture; it is now off), filters the history
     with a 5-tap Catmull-Rom, clamps it to a min/max box of the neighbours in YCoCg with HDR
     weighting, and blends **a fixed 4% of the new frame whatever the motion** (0.25 in the
     responsive variant, <1,1>), then adds dither. What the box lets through trails for ~25
     frames, and a moving picture goes through the history filter again every frame (blur).
   - *Our TAA* (`agent.taa`, insetpp.c; the patch made by `tools/gen_taa_patch.py` with llvm-mc into
     `agent/taa_patch.h`): the agent finds the shader by its type, checks its stage registers and
     its code's FNV-1a, copies the code into its own CPU+GPU memory (the reserve), patches the copy
     and points the PsShader's PGM_LO/HI at it (RSRC1 to 68 VGPRs a frame before the code that needs
     them, back the other way round; the stock code untouched). The patch: at the place the
     history's position is known (0x3e4 in the block), a branch to code after the shader's end that
     puts the blend's weight in v64 from the pixel's motion in pixels, 0.04 + min(0.004 x motion,
     0.16) (UE 4.15's later curve: the engine's 4% when still, 20% at 40 pixels a frame), and the four
     blends (0x8d4, 0x90c, 0x934, 0x954) take v64 instead of 0.04. The added code is arithmetic
     only. Every view, eyes and insets, runs the same shader: the same AA in both. Values: 0 the
     engine's, 1 the copy as is, 2 motion-weighted (inset mode's default), 3 no history (a test).
   - *On the console* (`aa5-ours`, the cafe's held view, the window circling, 1:1 crops): every
     switch in a frame, no crash; with 3 the crop aliases (high-frequency energy 10.98 against
     ~5), so the copy is what the GPU runs; with the window still, ours and the engine's the same
     picture (mean difference 0.45 of 255: the same 4% when nothing moves); the game's GPU 12.9-13.5
     ms a frame in every mode (nothing measurable). `aa6-walk` (rec1 with the new defaults, the
     engine's and FXAA in between): no crash, 60 fps, the GPU as with the engine's. The edge probe
     reads brightness only and can't tell the AA settings apart (`aa1`).
   - *The ghosting worn with the gaze* (2026-11-08, the user: fine centred, ghosting with the
     gaze on, on 3D objects only (Hikari, the calendar's date cards, the clock hands), never the
     background; the engine's TAA and ours alike). Their play log (`logs/play-aa`): the window moved
     10-21 inset pixels a frame (the tracker's scatter through the smoothing, rms 0.03-0.045 of
     tangent, 12-41 jumps a 10 s window); the insets' previous projections right every frame.
     Replicated with a made-up gaze through the same smoothing (`agent.insetgaze=9`: fixations,
     jumps, the scatter; `=10` the same about one place), measured with `tools/fov/ghost.py` (1:1
     crops aligned to a still window's; the RMS luma left; two still crops 0.7 apart):
     - the window moved by fractions of a pixel blurred everything a little (the history taken
       between texels by its Catmull-Rom each frame): moved by whole inset pixels (`agent.insetsnap`)
       the board's painted text and the wall match a still window (0.5-0.8, aa8);
     - the date cards stayed 12-22 off (aa8-aa10): with the velocity buffer left out (`agent.taa=4`)
       0.5-0.9 (aa9). A probe in the shader (`agent.taa=10`: one pixel's numbers stored to the
       agent's memory each frame, aa13) read the cause: in the frames where only the window moved,
       a still object's velocity is 0, its error the whole move. The engine writes an object's
       velocity with this frame's projection for its last place too, so a change of projection (the
       inset's window, never an eye's) is not in it; the background's reprojection (ClipToPrevClip,
       from the previous matrices) has it.
     - *The fix* (`agent.taa=11`, inset mode's default; `insetpp.c`, `tools/gen_taa_patch.py`): the
       window moves by 2-pixel steps (`agent.insetsnap=2`); each view's TAA reads its projection's
       M[2][0..1] (the view buffer's ViewToClip, dword 104), one pixel of it stores them in a
       two-slot ring of the agent's memory by the frame's parity (the dither's frame counter), every
       pixel reads last frame's back past the caches, and the difference rounded to 2 pixels (the
       jitters, under a pixel apart, taken out; 0 for an eye, whose projection never changes; 0
       past 150 steps, 300 pixels) is added to the velocity. aa14: the date cards under the made-up gaze 0.5-0.8,
       a still window's (from the engine's 15-21); `rec1` with Hikari (aa15): no crash, the GPU as
       before (12.5-13.0 ms). A first try (7-9) read last frame's projection from the view buffer
       at a slot that holds something else (the probe read -2 and 0 there) and did nothing.
     - *Ours against the engine's*: the fix needs the patched shader. Its blend: the engine's 4%
       (11) left the cards at the floor; the motion-weighted one (12) at 0.8-1.8, as it counts the
       window's move, now reprojected exactly, as motion and takes in more of the raw frame (more
       shimmer in motion). 11 is the default; 2 (motion-weighted without the fix) is not kept as one.
   - *The pop-in* (the user, worn with 11: Hikari's hair "pops in" as the gaze comes back to her):
     where an inset's history is off the view (what the window had left and comes back over), the
     shader takes the current frame alone and then blends 4% a frame, so that first frame's aliasing
     (one jitter's sample) fades by 0.96 a frame: 70% left after 8 frames, 36% after 25. *The fix*
     (`agent.taa=13`, inset mode's default): each pixel's blend weight kept in the history's alpha
     (which the engine blends as a fourth channel and nothing after the TAA reads), 1, 1/2, 1/3 ...
     down to 0.04 from the frame a pixel comes into view: the average of every frame since, an
     8-sample average after 8 frames. *Measured* (pop3, the cafe replay held, its calendar; a look
     away and back every 3 s, `agent.insetgaze=11`; a burst of four 1:1 crops 3, 10, 30 and 120
     pictures after one look back, `agent.capafter=1000`; `tools/fov/pop.py`, RMS from the burst's
     settled crop, the floor 0.5-0.8; three bursts each):

     | pictures after the look back | 11 (the engine's blend) | 13 |
     |---|---|---|
     | 3 | 4.5, 6.9, 7.5 | 4.5, 3.2, 4.1 |
     | 10 | 2.6, 5.0, 5.1 | 1.4, 1.4, 1.4 |
     | 30 | 1.2, 2.1, 2.2 | 0.8, 0.8, 0.8 (the floor) |

     The settled crops of 11 and 13 the same within the floor (0.49-0.77: the alpha is used by
     nothing after the TAA; pop5, rec1's rooms, the loading screen, the UI and subtitles under 13
     as before). The weight is kept negative (the code review): what another shader left in the
     alpha (a scene's alpha when 13 takes over, the responsive TAA's pixels) reads as the engine's
     0.04, so switching to 13 resets nothing; pop4 the same numbers as pop3 with it; under the made-up gaze (`agent.insetgaze=10`) 11 and 13 both at a still
     window's (0.50-0.75; the engine's 14-21, aa14): the hardened velocity fix holds. Hikari herself
     moves too much in the rec1 replays to measure on her (pop1, pop2: a held head and the pad's
     script cut at 213 s, she still walks and the scene fades); what comes back is converged the
     same way whatever it shows. Runs logs/pop1-pop3, run files tools/fov/runs/pop*.
   - *The pop-in, its cause* (the user, worn with 13: still a pop, "why only on Hikari and the
     calendar date": the velocity objects, never the background). pop3/pop4's crops had held only
     the board's painted text (no velocity). The velocity fix dropped the window's move past 75
     two-pixel steps (its constant 0x42960000, 75.0: 150 pixels), so on the frame the window jumped
     back (720 pixels with `agent.insetgaze=11`, any saccade past 150) the velocity objects took
     their history from the wrong place in the old window; the clamp let part of it through and it
     faded for ~30 frames. Lifted (any move within a view, still rounded to 2 pixels; the slot's
     key and frame counter refuse stale or first frames). Measured (pop7 capped, pop8 uncapped, the
     cafe replay, bursts of 128x144 crops on the "火" card, `agent.capdx=-76 agent.capdy=-78`; RMS
     from the burst's settled crop at 2, 3, 10, 30 pictures after the look back, the first one
     the window is back on the TV at 2; a box on the card, one on the painted 曜 beside it):

     | | card | 曜 (static) |
     |---|---|---|
     | 13 capped (three bursts) | 19.0-19.4, 13.0-14.6, 8.0-8.1, 3.7-3.9 | 11.3-15.6, 3.1-7.0, 1.4-2.2, 0.6-0.8 |
     | 13 uncapped (two) | 9.5-10.6, 4.2-4.8, 1.6-1.7, 1.1-1.2 | 11.5-13.1, 3.0-4.4, 1.5-1.6, 0.8-1.0 |
     | 14, start 1/4 (two) | 7.0-9.3, 5.4-6.3, 2.3-2.6, 1.1 | 9.8-13.9, 7.7-10.1, 3.3-4.4, 1.4-1.6 |
     | 15, start 1/8 (two) | 7.5-12.1, 6.4-9.8, 3.4-5.0, 1.6-2.2 | 10.9-16.7, 9.5-14.4, 5.4-8.0, 2.3-3.5 |

     Uncapped, the card settles as the static text does (the crops: capped, a doubled card at 2
     and 3 pictures; uncapped, the card in place from the first frame back). The made-up gaze
     (`agent.insetgaze=10`, moves under 150 pixels) the same with both (the cards at the still
     floor, 0.68-0.70). Runs logs/pop7, pop8.
   - *Variants 14, 15* (`agent.taa=14`, 15): 13 with the pixels that come back started from a soft
     picture of the current frame at a weight of 1/4 or 1/8 instead of the raw frame at 1: four
     bilinear taps at +-1/2 pixel of the shader's own current-frame texture (a 3x3 tent, about the
     eye's softness at 0.6 of the inset's density) in the shader's space (YCoCg, the HDR weight),
     for the waves with a pixel off the view only. The eye's own converged picture (a T# for its
     TAA output written by the agent) was weighed and left: that target is the engine's pool's and
     changes every frame, so a T# the CPU writes ahead of the GPU can name a target given back or
     freed (a GPU page fault: the console at risk); a wider history band cannot reach below the
     insets, which sit in the buffer's last rows. Measured on top of the uncapped fix (above): the
     frame-to-frame steps smaller (the biggest 4.2-6.1 with 14, 2.2-7.0 with 15, against 9.7-12.5
     with 13: the first frame's jump), but what comes back stays soft for longer (the static text
     3.3-4.4 from settled at 10 pictures with 14 against 1.5-1.6, 1.4-1.6 at 30 against 0.8-1.0).
     Not the default: to judge worn (smoother but softer for half a second). Their returning pixels
     also come back about 2% too bright (the code review, pop8: the card's crop at 2-3 pictures +1.1
     to +2.1 levels in R, G and B alike, fading over ~10 frames; 13 within 0.2): the bilinear taps
     average the frame's HDR colours before the shader's HDR weight, which the shader's own taps
     apply per texel, and that average is brighter on high-contrast detail. To fix before either
     becomes a default: point taps at texel centres (the four taps then wait once, not one by one).
     With the cap gone, a live change of the insets' density or size (the menu, cvars) is taken as
     a window move for its one frame: one frame's smear on the velocity objects, accepted.
   - *Still to do / judge worn*: Hikari, the clock hands and the edge in the headset with the gaze;
     the first frame back is still the raw frame with 13 (14 and 15 start it soft instead, measured
     above); variance clipping in place of the box
     clamp, for what trails at 4% when still. The code review hardened the fix's ring: a
     slot holds the view's key (its viewport centre) and frame counter with the projection, and a
     view takes the move only from its own slot of the frame before (the counter one less, mod 8)
     and only if it is an inset (an eye's projection never moves); before, a slot was taken
     whatever wrote it (another draw of the shader centred in an eye's quarter would have moved
     that eye's history; an inset missing a frame read a slot two frames old). The pixels that
     store are those within a pixel of the view's middle in pixels (it was 1/600 and 1/300 of the
     view, none in a view under 300 rows). With `agent.taa=11` to 15 the window moves by 2-pixel
     steps whatever `agent.insetsnap` says.
   - *Chisato and Allison* (2026-11-08 evening, branch `claude/taa-chisato-allison`): until now the
     patch engaged in Hikari only (their shader failed its checks; the user, worn, saw the gaze
     ghosting and the pop-in on the velocity objects there too). Their newer engine draws
     FPostProcessTemporalAAPS40, the "faster" TAA, at r.PostProcessAAQuality 3 exactly (0xca4a7a),
     which inset mode caps them at (their PS10 shimmers at 4+); PS40 is the shader to patch there
     (static type 0x4a94af0 in both games; the same code in both: their PS10 blocks too differ only
     in their hashes). Read from `logs/pop10`'s dump (`tools/fov/taadump.py`, now finding the code
     by its `s_mov_b32 s107` start and disassembling with `gcn7dis.py`): the code at +0xa0, 2048
     bytes, 32 VGPRs and 64 SGPRs, FNV 0x5670e5eb; the same shape as Hikari's PS10 with other
     registers, and three differences that matter: the history's 5 Catmull-Rom taps read RGB only
     (dmask 7), the output's alpha is the constant 0 (0x888), and the three blends (0x760, 0x7cc,
     0x838, `v_mac_f32 vD, s7, vX`, in YCoCg) take r.TemporalAACurrentFrameWeight (s7; the games set
     0.15, not the engine's 0.04); no dither, so no frame counter is read, and the shader binds the
     View uniform buffer (s[0:3] at 0x1b0, ClipToPrevClip; ViewToClip M[2][0..1] at dword 104 as in
     Hikari) and its own parameters only, not the Frame buffer whose FrameNumber Hikari's dither
     reads (both engines' View buffer has the same member list, read from the eboots' name
     strings: no counter in it). The sites: the view buffer's load 0x1c0, the velocity test 0x2f0
     (`v_cmp_lt_f32 vcc, 0, v0`, the same encoding; v1 the decoded y, v0 the raw x, v22 and v10
     the pixel's screen place, ScreenPosToPixel in s48-s51), the history's place 0x318, the
     history off the view 0x468 (into s[0:1]), the first history tap 0x450, the clamped history's
     Y 0x74c. `tools/gen_taa_patch.py` now takes several bases (a Base per dumped block with its
     own sites, Hikari's code kept as it was and checked byte for byte against the committed
     header; the velocity fix's body shared and renamed per base: v64-v74 become v32-v42, s76 on
     stay, the shader then 37-41 VGPRs and 96 SGPRs), and the agent picks the base by build
     (`k_taa_base[]`, `taa_var()`; a variant the base has no code for is built as another: 2 and 6
     as 1, 5 as 4, 8 and 9 as 7, 12-15 as 11). PS40's own variants: 1, 3 (every history taken as
     off the view, and R and G swapped so a capture shows the copy draws the view: taa40-4), 4,
     10 (a probe that dumps the view buffer's first 960 dwords each frame and the history's T#),
     7 and 11. *11, the velocity fix*: the view buffer has the frame before's jittered projection
     at dword 284 (PrevViewToClip, taa40-1's probe), so the window's move is this frame's
     ViewToClip M[2][0..1] less it, rounded to the window's 2-pixel steps, in the insets only (the
     view told by the pixel's place, as the ring's form does): no slots, no counter. *13, the
     quick convergence*, is not built here: it keeps its weight in the history's alpha, and
     taa40-4's probe read the history's T# as data format 6, R11G11B10, which has none (13 would
     read 1 and give the engine's own weight: 11 with one more texture read); 13 is built as 11.
     *7, the ring form, for comparison*: each view's last projection in the agent's slots, with
     a frame counter of the agent's own. *The frame counter*: a dword of
     the agent's own after the views' slots, raised by one each frame by the left eye's TAA draw
     (its one middle pixel, the pixel whose column and row the viewport's centre falls in, loads it
     and stores it one more); the insets' draws come after the eyes' in the frame (each view's
     post-processing in turn, the engine's barriers between), so every inset pixel reads the same
     count for the frame, one more than the count its view stored with last frame's slot, and
     Hikari's check (the slot's counter one less, mod 8) holds as it is. (The jitter's Halton
     index, decodable from ViewToClip M[2][0] in pixels in principle, was not used: the inset's
     projection carries the eye's own off-centre fraction of a pixel, 0.76 in Hikari's left
     inset, which the shader cannot tell from the jitter without learning it.) Host tests:
     `insetpp_test` runs each base's block (`TAA_BLOCK`, `TAA_BLOCK40`). Measured so far (Chisato,
     taa40-1 to -4, the palm tracker off, all clean): the copy draws the view (3's R and G
     swapped in the capture); after a look back the inset's crop (a wall corner, 128x144) is
     within about 1 luma step RMS of the settled one in every burst, ours and the engine's alike
     (two settled captures differ by 1.05), with a little more detail in ours in the first
     pictures (0.96-0.98 of the settled one's against 0.92-0.94): no harm, no clear gain in that
     view. Allison (taa40-5 to -9: her PS40 the same code, FNV 0x5670e5eb, the copy drawing her
     view; her replay's still stretch 235-300 s, ours and the engine's in two runs): within about
     2 luma steps of settled for 11, 1-4 for the engine's, against a floor of 2.1-3.0 (her scene
     keeps moving): no clear difference. Hikari 1.02 (taa40-10: its PS40 the same code, FNV
     0x5670e5eb, every variant run clean; taa40-14: the cafe replay re-timed for 1.02's menu,
     `tests/rec2-bottle-hold-102`, the calendar's 月 card, `agent.insetgaze=11`, a look away and
     back every 3 s, two bursts each, the floor 0.48), RMS from the burst's settled crop at 2, 3,
     10 and 30 pictures after the look back: 11 8.8, 9.1, 4.8, 2.0; the engine's 15.1, 10.5, 3.4,
     1.9 (each pair of bursts the same within 0.01). So 11 halves the pop on the window's return
     but settles a little slower at 10 pictures; 13's quick convergence, which won on Hikari 1.00
     throughout, has no alpha to keep its weight in here. *13 on PS40, its weight in the agent's
     memory* (`fresh_buf` in tools/gen_taa_patch.py): a byte per 2x2 block of an inset's pixels,
     two planes a view written and read in turn by the parity of a frame counter the left eye's
     middle pixel raises (the insets drawn after the eyes); last frame's byte read at the history's
     pixel (0x334, v8 and v13), w = it within s7 and 1, the next w / (1 + w) or 0.5 off the view
     stored at the output; the eyes keep s7 and store nothing. Its V# ends at the last plane, so a
     load or store past it is dropped by the hardware. 1 MB more for the copies on the newer
     engine only (in the game's range: the reserve is full; Hikari 1.00's 72 KB as before).
     Measured (taa40-15, the same calendar crops, the floor 0.48): 13 7.0, 5.4, 1.8, 1.4; the
     engine's 16.7, 11.6, 4.6, 1.9; 11 10.8, 7.0, 3.6, 2.2: under half the engine's pop on the
     return and settled sooner throughout. Run clean on Chisato and Allison too (taa40-16, the
     window away and back every 3 s, 150 s each). So 13 is inset mode's default in every build
     (agent.h `taa_default()`).
   - *Hikari 1.02 (build 4, the newer engine; docs/OTHER-GAMES.md)* takes base 1 too, its PS40 at
     its own static type 0x49e09b0 (1.00's types less 0x10000, found by their names). Its shaders
     are in the game's data, not the eboot, so whether its PS40 is Chisato's code is told on the
     console: `taa_init` checks the FNV and the registers and leaves a different shader alone. Its
     PS10 showed Chisato's FNV (0x9c2c094d) in the first play (2026-11-08), so the same shader
     cache is likely. `insetpp_test` runs the PS40 block as build 4 too. (Run since: taa40-10 and
     taa40-14 above, its PS40 the same code; 13 its default too.)

Still open:
- **The UI over the eyes** (21's last point; the icons cut at the inset's edge, 10): the
  stretches where one eye's probe reads black, likely the game's UI drawn over the eyes alone.
  The probe reads the TV's picture, so some of them may have been the mirror's black frames
  (25): to check with the fixed mirror.
- **A doubled outline at an inset's edge** (her hair, ~3 capture pixels, right eye, seen before
  the eye's bloom and AO and the window test): not looked at since; worn after 22 the inset's
  edge was no longer visible.
- **Allison's bloom above 3**: a flashing light with `r.BloomQuality` 4-5 from the menu (the
  game's own is 1, inset mode leaves it): not looked at yet.
