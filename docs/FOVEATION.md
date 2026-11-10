# Foveated rendering: what is reachable, what it would save, the plan

> **Status (2026-11-07): history.** This is the earlier foveation experiment, a gaze window cutting
> the shadow pass, withdrawn after the runs of 2026-11-05 (shadows cost about 0.1-0.2 ms a frame
> in play, see "The runs"). It is not the dynamic foveated rendering (DFR) the README describes:
> that is inset mode, [MULTIVIEW.md](MULTIVIEW.md). The options here (`agent.census`,
> `agent.fov`, `fovdeg`, `fovps`, `fovhw`) were removed for the release. The eye tracking it
> built (`agent/fov.c`; `agent.gaze`, a debug option, which inset mode turns on itself) is what
> inset mode uses to follow the gaze.

A study (2026-11-04, offline: the code, the docs, the eboot and the 12.40 libraries in `dumps/`,
PS5VR and Ghost-VR, public sources; nothing run on the console) of whether the port can draw the
periphery cheaper, with eye tracking or without, to free GPU time: for steady 60 at 2048 eyes,
or for the native 90/120 modes at bigger eyes. Offsets are Hikari 1.00's eboot and 12.40's
libSceHmd2.

## In short

- **Eye tracking is reachable**: libSceHmd2, which the agent already loads, exports
  `sceHmd2GazeGetResult` and `sceHmd2InternalGazeStart` (both by NID in the 12.40 library), and
  opens the gaze device (`/dev/hmd2_gaze`, the headset's USB interface "PS VR2 Data 3", in our
  kernel logs) in its own start-up. PS5VR uses exactly these two calls. Not yet tried in a PS4
  process.
- **No variable-rate shading** in the game's GPU context (PS4 mode). But the PS4 Pro hardware has
  a **"foveated window"** (context registers 0xeb/0xec, set to the whole screen by the console's
  PS4 Pro driver; the game carries Sony's routine to set it and never calls it). What it does is
  not documented anywhere public.
- **The game already masks 16 % of each eye**: Unreal 4.12's Morpheus plugin draws PS VR1's
  hidden-area mesh into the depth prepass every frame (`vr.HiddenAreaMask`, on by default),
  stretched over PS VR2's field of view.
- **Unreal 4.12 offers no foveation**: one view per eye, one screen percentage for both, no
  multi-resolution or lens-matched shading. Rendering an inset and a periphery as separate views
  means rewriting the engine's view setup in a compiled binary.
- **Hmd2 doesn't render the game's scene**: its layer can carry depth and foveation tables
  (Horizon Call of the Mountain sends them), but those only tell it how to resample a picture
  that was already drawn foveated. They save nothing by themselves.
- **The route that fits this port**: cut individual passes to a window around the gaze in the
  game's own command buffers, which the agent already sees at submit. The pass to start with is
  the shadow projection, the one costly pass measured so far. Expected: about **2 ms at 2048 with
  eye tracking** (about 0.8 ms with a fixed window). Add whatever reshaping the hidden-area mask
  for PS VR2 gives (unknown, probably under 1 ms), and the foveated-window probe, which could be
  worth several ms if the hardware does what its name suggests (unknown).
- **Not in reach**: Unity-style gains (2-3.6x) need the engine to render foveated, which this
  build can't do without hardware help.

## The budget

From the replay's play windows (SUMMER-LESSON.md, FRAME-COPY.md), the game's GPU time is close to
**1.6 ms + 1.36 ms per million eye-pair pixels**: 120/1248 (3.5 Mpx) 6.4 ms, 120/1312 (3.9 Mpx)
6.9, 60/2048 (9.4 Mpx) 14.5 on average, 17.8 at most. At 2048 about 88 % of the time follows the
pixel count, so **10 % fewer pixels shaded at 2048 is about 1.3 ms**. Heavier worn scenes
cost more: 90/1800 TAA ran at about 13 ms a frame.

Where the time would go:

- **60 at 2048 with margin**: the peaks (17.8 ms) need about 2-2.5 ms to stay under 16.7.
  Eyes wider than 2048 aren't possible at any GPU time: Hmd2 refuses textures over 4096 wide, and
  the game draws both eyes into one texture.
- **Native 90**: holds at about 1700 today. 1800 needs about 2.5 ms off the heavy scenes, 1900
  about 3.6, and 2048 about 5.5 (35 %). Native 90 has never been worn: whether it shakes like
  120 decides whether this goal is worth anything.
- **Native 120**: 8.3 ms, a shake that isn't understood yet, and 1250-1500 eyes. Foveation
  doesn't change that picture.

What is known per pass (1500 eyes, start scene, measurements 17-27): dynamic shadows 1.3 of
6.9 ms (19 %), and the cost is the per-pixel projection, not the shadow maps (smaller maps,
quality 1 and half distance saved nothing). All of DOF, bloom, tonemapper and eye adaptation
cost 0.2 ms; AO costs 0. The TAA's cost isn't measured (it stays, by the user's choice). The rest
(prepass, base pass, lighting, translucency, TAA) isn't split; that is what the measurements below
are for.

## 1. Eye tracking from the game's process

**The library.** Gaze is part of libSceHmd2 (no separate library):

- `sceHmd2InternalGazeStart` (0x31760) is one ioctl (0x8004bc08, value 1; `...GazeStop` sends
  2) on Hmd2's main device handle (data 0xb803c). PS5VR: "Eye tracking runs only when asked
  for"; without it every result is invalid.
- `sceHmd2GazeGetResult(param, result)` (0x35110): param is 0x10 bytes (size 0x10, the Hmd2
  handle at +4), result 0x38 bytes (size 0x38). It returns the newest sample from a ring of
  0x168-byte samples that a reader thread fills from the device. With no data, or a sample over
  1 s old (0xf4241 us), the result comes back empty: +4 = -1, +0x10 = 0, +0x1c = -1.0. PS5VR's
  reading of a good one: +4 status (0 ok), +0x10 1 = valid, +0x14 a unit direction in headset
  space (-z forward).
- **The device** is opened by Hmd2's own start-up (0x2aac0 calls 0x316d0, which opens
  `/dev/hmd2_gaze` and keeps its handle at 0xb8a48). The result isn't checked: a refused open
  only prints `/dev/hmd2_gaze Open Error` to stderr, which we don't capture. So we don't know yet
  whether a PS4 process may open it (the camera devices needed the payload's lent credentials for
  the palm tracker). One read of Hmd2's data +0xb8a48 settles it.
- Version gate: one path refuses the result as unsupported (0x8a720003) when a version it reads
  (0x2aab0, apparently the app's SDK) is under 7.00. The prepared Hmd2 already answers 8.05.
- Eye tracking must be on, and calibrated, in the console's PS VR2 settings. The system's own
  eye-tracking setup reads the same device.

**The tracker.** VrTracker2 has no gaze calls in the export tables PS5VR keeps (QueryMemory,
Initialize, Finalize, Register/UnregisterDevice, GetResult); gaze isn't part of the pose
results the agent reads.

**PS5VR** (a native PS5 app) uses Start + GetResult for its "Eyes + pinch" and "Eyes + blink"
modes. It runs on 13.60; on 12.40 its test notes (commit fcb400d) list eye tracking as only started. **Ghost-VR**
does no eye tracking: its OpenXR runtime reports `supportsEyeGazeInteraction = false`.

The cost: none on the GPU that we know of. The gaze arrives from the headset as data (the
library's messages are about transfers, ring buffers and calibration data, not images).

## 2. GPU hardware in the game's context

**Variable-rate shading: no.** The game runs in a PS4-mode ("gen1") context: GCN shaders, GCN
descriptors, PS4 register state. The agent already found RDNA registers privileged there. VRS is
RDNA2 state with no PS4 counterpart, the game's shaders and its Gnm know nothing of it, and Sony
has never said the PS5's GPU has it.

**The PS4 Pro "foveated window": present, unexplained.** The game runs in PS4 Pro (Neo) mode
(measurement 16). Across the console's two PS4 Gnm drivers, the only context registers that the
Neo driver writes and the base driver doesn't are **0xeb and 0xec**. They're written in its
default state (`SET_CONTEXT_REG 0xeb, 0xff00ff00, 0x0000ff00`, in three of its state tables near
file offset 0x14700-0x14b50). AMD's later gfx9 register lists name the same two slots `PA_SC_FOV_WINDOW_LR` and
`PA_SC_FOV_WINDOW_TB`. Hikari's eboot links Sony's two routines for them and never calls either
(no call or pointer to them anywhere):

- 0x259c4e0 writes the default above (the whole screen);
- 0x259c540 writes `LR = a | b<<8 | c<<16 | d<<24` and `TB = e | f<<8` from six byte arguments:
  a left and right bound for each of the two eyes of a side-by-side target, and one top and bottom
  for both.

So the PS4 Pro had a per-eye window in its rasterizer, made for side-by-side VR targets like
this game's, and the default leaves it open. Nothing public says what the hardware does outside
it. It could shade at a coarser rate (that would be foveation with no other change), it could
clip, or it could do nothing unless something else is set too. Also unknown: whether the PS5
implements it in PS4 Pro mode. Section 6 has a probe.

## 3. What Unreal 4.12 offers

- **`r.ScreenPercentage`** sets the eye texture's size through the plugin
  (`CalculateRenderTargetSize` 0x2f8fb20). `UpdatePostProcessSettings` (0x2f8ff60) gives both
  views the same percentage from the plugin's +0x298, so there's no per-view or per-region value
  (SUMMER-LESSON.md, dyn1). It scales the whole eye.
- **Views**: two, one per eye (`eSSP_LEFT_EYE`/`RIGHT_EYE`). There's no monoscopic far field (4.14
  on; its strings aren't in the binary) and no multi-resolution or lens-matched shading (those
  were NVIDIA's own branches). `vr.InstancedStereo` exists, but it saves draw calls, and the CPU
  isn't the limit.
- **The hidden-area and visible-area meshes: in use now.** The Morpheus plugin builds four meshes
  at start-up (0x2f8d3c3; FHMDViewMesh at plugin +0x188/+0x1a8 hidden left/right, 84 vertices
  each, and +0x1c8/+0x1e8 visible, 78). They come from static tables (copied at 0x2f937d0 from
  rodata 0x3619120 on), and `HasHiddenAreaMesh` (vtable 0x465a258 +0x170, 0x2f93fb0) says yes.
  `vr.HiddenAreaMask` defaults to 1 (0x8eada2). So every frame the renderer (0x93ebb0) draws the
  hidden mesh into the depth prepass (`DrawHiddenAreaMesh_RenderThread` 0x2f90890, through vtable
  +0x180), and a later pass (0xd2b801) draws the visible mesh. The hidden mesh is PS VR1's lens
  outline in the eye's 0-1 coordinates: **16.0 % of each eye**, the four corners cut by a curve
  (left eye, `#` = masked):

      ###############..................###############
      ##########.............................#########
      #######...................................######
      ####........................................####
      ##............................................##
      #...............................................
      ................................................
      (the bottom mirrors the top)

  Now that the game draws PS VR2's field of view, that outline covers the corners of PS VR2's
  field-of-view rectangle. Two things we don't know: whether any of it shows in PS VR2's lenses
  (masked pixels would show as dark corners), and how much of what is left falls outside them
  (pixels we could also skip).
- **The post-process chain** (measurement 17, FRAME-COPY.md): TAA (`r.DefaultFeature.AntiAliasing`
  2, quality 3, 8 samples), DOF 2, bloom 1, tonemapper 5, eye adaptation 2, AO 2 levels, SSR and
  motion blur off. Only the TAA is a full-cost pass, and it can't be cut by region: it writes
  every pixel and keeps history.

## 4. Engine-side tricks

The frame-rate and screen-percentage patches were found by reading the code and changing a value
or a store (OTHER-GAMES.md, Method). Foveation needs more than a value. These are the ways in:

- **Reshaping the hidden-area mask** (rodata tables, or the plugin's mesh fields): the same draw
  the game already makes, with PS VR2's outline. It saves prepass and base-pass work in what is
  outside PS VR2's lenses, and nothing more.
- **Radial density masking** (Valve's: skip 2x2 quads in a checkerboard in the periphery, then
  fill them in) through the same mesh, with more triangles, is possible in the mesh. But filling
  the holes needs a pass after the base pass and before lighting and TAA, and the plugin has no
  hook there (its scene-view extension, vtable 0x465a648, runs before the views render). Unfilled,
  the TAA would smear dark holes into the periphery. Rejected.
- **Cutting passes in the command buffers.** The agent hooks `sceGnmSubmitCommandBuffers` and
  `...SubmitAndFlip...` (hooks.c 830), so it sees every command buffer the game sends before the
  GPU does. Unreal sets a viewport and scissor for each pass. Rewriting the *values* of the
  scissor writes before chosen draws (never a packet's size or header) limits those draws to a
  window: outside it, the pass doesn't run. That's the same kind of rewriting gen1.c does to
  Hmd2's buffers, applied to the game's. For the shadow projection, the screen shadow mask Unreal
  clears to "lit" is left unshadowed outside the window. The TAA blends the change over about 8
  frames, so a shadow fades in rather than popping when the gaze moves.
- **Inset and periphery as four views, composed by Hmd2 as two layers**: Hmd2 composes more than
  one layer (Ghost-VR sends a second, as Call of the Mountain does). The engine side means
  patching the view-family loop, the per-view state (TAA history), the stereo-pass handling and
  the plugin's view rectangles and projections, in compiled code. Each added view repeats its
  CPU work and its per-view GPU work (the shadow cascades are per view). It's the only route to
  Unity-style gains in this build, and it's out of proportion to this project.

## 5. Hmd2's layer

The agent fills Hmd2's layer type 0x10 (0x148 bytes) up to +0xb0 (vr2.c game_loop `lp`): the
textures, sampler, each eye's viewport size and offset, the tangents, the pose and time, the
frame, +0x98 = 100 and +0xa0-0xac the viewport sizes again, as Call of the Mountain fills them.
Call of the Mountain's frames also carry depth and foveation tables, which its positional
reprojection (mode 2) works from. Ghost-VR tried mode 2 with a PS4 game's layer (no tables):
red and black eyes. Hmd2 doesn't take gaze in the layer. Its gaze calls are separate, and its
reprojection doesn't render the scene. So nothing in the layer makes the game cheaper. The
tables would matter only if the game drew a non-uniform picture (the foveated window, if it turns
out to work that way, or the four-view route).

## The options

GPU saving at 2048 (14.5 ms average), from the model above and the 1500 measurements. These are
estimates until the first measurements have run.

| option | saving at 2048 | risk | effort |
|---|---|---|---|
| eye tracking (enabler) | 0 (data only) | low-moderate: a new device stream in a PS4 process | small: two imports, a reader |
| shadow projection cut to a gaze window (±30°, 18 % of the eye) | ~2.2 ms if shadows are 19 % in play | moderate: value edits in the game's command buffers; a wrong edit can hang the GPU | medium-high |
| the same, fixed window at the centre (must cover gaze ±20°: ~69 % of the eye) | ~0.8 ms | as above | medium |
| hidden-area mask reshaped for PS VR2 | 0 to ~0.7 ms (unknown outline) | low: the game's own draw, other vertices | small-medium, needs the lens outline |
| PS4 Pro foveated window | 0 if it clips or does nothing; several ms if it coarsens the periphery | moderate-high: undocumented hardware | small to probe; unknown after |
| inset + periphery as four views | ~4-6 ms | high | very high |
| VRS | none reachable | | |
| for comparison: `r.ShadowQuality=0` | ~2.7 ms (all shadows gone) | none | none |
| for comparison: 1900 instead of 2048 eyes | ~1.8 ms | none | none |

The comparison rows set the bar: a foveation scheme is worth having only if it beats shadows off
or a few percent less resolution on quality per millisecond. The gaze window does: it keeps
shadows where the player looks. A fixed window barely does: the eyes move about ±20° in the head,
and on a flat projection most pixels are far from the centre (±30° around the gaze is only 18 % of
the eye's pixels).

## Recommendation

Wire up eye tracking and use it to cut the expensive per-pixel passes to a window around the
gaze, starting with the shadow projection. Do it in the agent, by rewriting values in the game's
own command buffers; no engine code changes. Expect about 2 ms at 2048, or about 15 % (fixed
window: about 0.8). That covers the peaks at 60/2048, or native 90 at about 1800 instead of
1700. In the same command-buffer census, probe the PS4 Pro foveated window: it's the only
reachable thing that could give the large gains, and it can be probed cheaply. Reshape the
hidden-area mask for PS VR2 if the measurements show the lens outline leaves room.

Before building any of it, the measurements below can decide it for free: if shadows turn out to
cost little in play at 2048, the main gain is gone. In that case `r.ShadowQuality=0` or 1900 eyes
is the honest answer.

## The plan

Each step is one console run, and each needs the user's go. The runs in steps 1-2 change no
code. Steps 3-5 add only reads. Only steps 6 and on change what the GPU does.

1. **Shadows in play at 2048** (`cvars.txt` only). Two 60/2048 full replays, one plain and one
   with `r.ShadowQuality=0`. The GPU average and peak difference is the most a gaze window can
   save. If cvars.txt is changed by FTP mid-run instead (live within 2 s), one run does both, at
   the cost of comparing different scenes.
2. **What the existing mask saves** (`cvars.txt` only): a 60/2048 replay with
   `vr.HiddenAreaMask=0`. The difference is what 16 % of the pixels cost in prepass and base pass,
   which calibrates any mask-shaped saving. Worn, with the mask off, the user looks at the four
   corners: if more picture shows than with it on, PS VR1's outline is cutting into what PS VR2
   shows today.
3. **The gaze, read only** (agent: imports for `sceHmd2GazeGetResult` and
   `sceHmd2InternalGazeStart`). Once Hmd2 is up: log Hmd2's gaze handle (its data +0xb8a48 on
   12.40: open or refused) and one `GetResult` without starting (expected: not valid).
   `InternalGazeStart` comes once, and only after the palm tracker's start rule (worn, no system
   screen for 5 s), so it never meets the system's own eye-tracking setup. Then log twice a
   second: the return code, the status, valid or not, the sample's age and the direction. Worn,
   60 s: the player looks left, right, up and down on cue. Before the run: eye tracking on and
   calibrated in the PS VR2 settings.
4. **Command-buffer census, read only** (agent, in the submit hook). Walk each command buffer's
   PM4 packets and accept the walk only if it ends exactly at the buffer's end. Log per frame: the
   packets, the draws, the pixel shader addresses with their draw counts, the viewport and scissor
   writes, every write to 0xeb/0xec (does the driver's default state reach the game's buffers?),
   and the walk's CPU time. 60 s replay. No writes to anything.
5. **Which draws are the shadow projection**: the census run again with `r.ShadowQuality` switched
   0 to 2 and back by `cvars.txt` (live). The draws that come and go with shadows, with the
   scissor writes before them, are the ones to cut. Nothing on the GPU changes.
6. **A fixed window for the shadow projection** (built as `agent.fov=1` with
   `agent.fovps`, live): the scissor values
   before those draws set to a centred window. A/B in one replay run; GPU time, and the TV mirror
   for the look. Values only, after a validated walk; one register's values, nothing else.
7. **The window on the gaze** (needs 3 and 6): moved each frame from the newest gaze sample
   (direction to texture coordinates through the frame's field of view and uv). Worn A/B: can the
   player see the shadows come and go, and what is the GPU time in heavy scenes?
8. **The foveated-window probe** (its own go, only if step 4 found the driver's 0xeb/0xec writes in
   the game's buffers): change those two values in place to a centred window, 30 s, TV mirror
   watched. It shows whether anything changes on screen or in the GPU time. Stop at the first sign
   of trouble. If it coarsens the periphery, it gets its own study (TAA, the window on the gaze).
9. Later, if step 2 shows room: **the hidden-area mask in PS VR2's outline** (the lens outline
   measured first: worn rings, or a coded picture compared with Hmd2's headset buffer), then the
   wear tests at native 90 at the sizes the savings allow. Before any of the native-90 work, the
   still-missing wear test of 90 at 1700 says whether 90 is comfortable at all.

## The implementation (agent/fov.c; run 2026-11-05, see "The runs")

Steps 3 to 8 of the plan were in the agent, each off unless `cvars.txt` asked for it. None of
these options is in a release build; there, eye tracking starts only for inset mode
(`agent.multiview`). All except `agent.gaze` changed live: the agent reads `cvars.txt` every
2 s. (All but `agent.gaze` were removed for the release; this section is history.)

| line in cvars.txt | what it does | plan step |
|---|---|---|
| `agent.census=1` | walks the game's command buffers at submit, read only; every 10 s the log lists the 24 pixel shaders with the most pixels under their scissors (address, draws, how many are 3-4-vertex full-screen draws, the largest rect), the scissor writes by kind, every write of 0xeb/0xec, and the walk's CPU time | 4, 5 |
| `agent.gaze=1` | eye tracking: one result read before the start, then `sceHmd2InternalGazeStart` once the palm tracker's start is over (started or failed) and the headset has then been worn with no system screen for 5 s, then a read every 5 ms; the raw result logged twice a second for 2 minutes, then a summary every 10 s | 3 |
| `agent.fov=1` | the chosen shaders' scissors cut to a window at the centre of each eye | 6 |
| `agent.fov=2` | the same window around the gaze (needs `agent.gaze=1` at the start); nothing is cut while the gaze is lost (no valid sample for 300 ms) | 7 |
| `agent.fov=3` | the same window around a made-up gaze: the centre, 25° left, 25° right, 15° up, 15° down, 2 s each (a run nobody wears) | 7 |
| `agent.fovdeg=N` | the window's half-size, 5 to 80 degrees (default 30; a fixed window needs about 45-50) | 6, 7 |
| `agent.fovps=0x...,0x...` | the pixel shaders to cut (up to 16), as the census prints them | 6, 7 |
| `agent.fovhw=1` | the probe: the values of the game's 0xeb/0xec writes set to the middle half of each eye (`0xbf40bf40`, `0xbf40`) | 8 |

**What a cut changes**: only the values of scissor registers the game itself wrote (screen,
window, generic, viewport 0), never a packet's size or header. A value is cut only when all of
these hold:
- the walk of its buffer ends exactly at the buffer's end;
- every draw under that scissor uses one of the listed shaders;
- each of those draws is a plain colour pass: `CB_COLOR_CONTROL` is not set to a fast-clear
  eliminate, resolve or decompress, and `DB_RENDER_CONTROL` is not set to a depth clear, copy or
  resummarize;
- the window offsets (0x80, 0x8d) are zero; once any non-zero offset is seen, every buffer has
  to write them zero itself. The pass mode and the offsets carry from one buffer to the next as
  the registers do, but only after a submit that was walked to its end;
- the game writes both of its registers again later in the same buffer, so nothing after it
  draws with the cut;
- no packet ran while it was in force that the walk doesn't know to be harmless: only a
  whitelist of opcodes counts as harmless. Indirect buffers, register loads and
  read-modify-writes, `COND_EXEC`, `SET_SH_REG_OFFSET`, predicated packets and unknown opcodes
  all stop the cut, and the census lists the unknown ones;
- it lies within one eye's picture, as the frame's own uv mapping places it (vr2.c's way).

The cut is the scissor intersected with that eye's window. A window that misses it leaves an
empty scissor at 1,1, so that pass doesn't draw there at all. Every cut pair is remembered
(2048 slots, by address):
- a buffer the game submits again is cut from its own values;
- every remembered pair it holds is put back unless it is cut again, whatever the shader list
  says now;
- after the options are switched off, the walk goes on for 1200 more submits just to put cuts
  back.

Unreal records its buffers anew each frame, so this is for safety. Two cases still put nothing
back: a walk switched off for time, and a slot taken over by a newer cut. The probe
(`agent.fovhw`) only replaces the Neo driver's exact default values and puts them back the same
way.

**Safeguards**:
- A buffer is walked only if `sceKernelVirtualQuery` says its whole range is readable by the CPU
  (the answers kept for 2 s).
- The walk checks its time every 4096 packets and gives a buffer up (unchanged) past 2 ms. It
  also switches itself off when one submit takes 4 ms, or when walking takes 5 % of a 10 s
  window. It stays off until `cvars.txt` changes. The render thread reads the buffers on the
  CPU, and if they're in write-combined memory that is slow.
- The gaze counts only when a new sample arrives: a result whose sample time stops changing goes
  stale after 300 ms. A direction that isn't finite, or is beyond about 84 degrees, is ignored.
- Gaze is read on its own thread, marked as a PS5 thread like the palm tracker's. Eye tracking
  starts only after the palm tracker's start is over (`g_hand_settled`, set by hand.c, so the two
  never overlap), and then by the palm tracker's own rule (`hand_clear_why`, shared with
  hand.c), so it never meets PS VR2's own setup screens.

**Review**: two review rounds before the first run. The first, independent, found no likely GPU
hang in the edits, but these bugs, all fixed:
- unknown packets weren't stopping cuts;
- the one-dword NOP was misread;
- edits outlived the options;
- a stalled gaze never counted as lost;
- a NaN gaze gave a bad scissor;
- the window ignored the uv mapping;
- the shader list could be read half-written.

The second found these, all fixed:
- buffers were read without asking whether the CPU can read them;
- eye tracking was timed after the palm tracker's start, not sequenced after it finishing;
- gaze freshness relied on +8 being a time (a changed direction now counts too);
- the window read a frame slot vr.c may be writing (now `vr_frame_copy`);
- the pass mode carried from earlier buffers was assumed harmless;
- cuts weren't put back once the shader list changed;
- the start rule and the draw opcodes were duplicated from hand.c (now shared);
- the release-build rule was spread over hooks.c (now in fov.c alone).

A third round found these, all fixed:
- while vr.c is writing the frame slot, the window was dropped for that buffer, so shadows would
  have flashed on now and then; the last frame's mapping is now used instead;
- a submit skipped while another thread was walking could be forgotten by the carried pass
  state;
- eye tracking would wait forever if the palm tracker's thread couldn't be created;
- the readable-memory answers were kept forever; they are now asked again every 2 s.

**Host test** (removed for the release with the census and the cuts): `tools/fov/test.c` ran the
walk on made-up buffers:
```
make -C agent && cd tools/fov && cc -O2 -I../../agent -o /tmp/fovtest test.c && /tmp/fovtest
```
It covers:
- the windows of both eyes, at the centre and 20° right and up;
- each scissor kind's fields, and the offset-disable bit kept;
- the nine ways a cut must be refused;
- a buffer that doesn't parse, and the one-dword NOP;
- the gaze lost, and an empty window;
- rects across both eyes and outside the texture;
- putting cuts back, including after the list changes and through `fov_submit` once the options
  are off;
- a resolve mode carried from the buffer before;
- an unreadable buffer;
- the gaze samples (new by time or by direction; invalid, NaN and beyond 84° refused);
- the live options' defaults;
- the probe's defaults-only rule;
- the option's list.

**The runs, in order.** Each run needs the user's go. Each line below is one console run, from
the worktree with this agent built (`make -C agent`). The resident payload is kept
(`PS5_REUSE=1`), and the agent is uploaded as `agent.elf`. `R` stands for the recorded session
`tests/rec1` (4 minutes; recordings are not committed), `L` for `logs`.

- `PS5_CVARS` adds the settings file's lines to the console's own `cvars.txt` for the run.
- `PS5_CVARS_AT` switches them at the given seconds.
- The console's own `cvars.txt` is put back at the end, however the run ends.
- Removing a game variable's line doesn't undo it. That is why the files that follow a
  shadows-off stretch set `r.ShadowQuality=2` (the game's own) again.

```
R=tests/rec1; L=logs; . tools/local.env; export PS5_HOST
# 1. shadows in play at 2048, no foveation code involved: plain, then shadows off
PS5_REUSE=1 sh tools/testrun.sh payload/summerlessonvr2.elf 280 $L/fov1-plain.log "replay worn" $R.pad.txt $R.head.txt
PS5_REUSE=1 PS5_CVARS=tools/fov/runs/shadows-off.txt sh tools/testrun.sh payload/summerlessonvr2.elf 280 $L/fov1-noshadow.log "replay worn" $R.pad.txt $R.head.txt
tools/fov/report.py $L/fov1-plain.log $L/fov1-noshadow.log

# 2. the census, then shadows off at 60 s and back at 120 s: the shadow pass's shaders
PS5_REUSE=1 PS5_CVARS=tools/fov/runs/census.txt PS5_CVARS_AT="60:tools/fov/runs/census-shadows-off.txt 120:tools/fov/runs/census-shadows-on.txt" sh tools/testrun.sh payload/summerlessonvr2.elf 180 $L/fov2-census.log "replay worn" $R.pad.txt $R.head.txt
tools/fov/report.py $L/fov2-census.log

# 3. eye tracking started with the headset on the table (counted as worn): does it start in a
# PS4 process, does Hmd2 send anything (with no eyes, samples not valid most likely)
PS5_REUSE=1 PS5_CVARS=tools/fov/runs/gaze.txt sh tools/testrun.sh payload/summerlessonvr2.elf 90 $L/fov3-gaze.log "replay worn" $R.pad.txt $R.head.txt

# 4. the fixed window (the cut from 60 s) and 5. the window on a made-up gaze (agent.fov=3):
# not made (see The runs), and their settings files were never written

# 6. (its own go) the PS4 Pro foveated window, 30 s, the TV watched
PS5_REUSE=1 PS5_CVARS=tools/fov/runs/fovhw.txt sh tools/testrun.sh payload/summerlessonvr2.elf 30 $L/fov6-hw.log "replay worn" $R.pad.txt $R.head.txt

# after the runs: the console's play build back (the runner leaves this agent as agent.elf)
curl -s "ftp://$PS5_HOST:2121/" -Q "DELE /data/summerlessonvr2/agent.elf"
```

What each run decides:
- **Run 1**: is it worth it? A shadow cost under about 1 ms at 2048 makes `r.ShadowQuality=0`
  or 1900 eyes the better answer.
- **Run 2**:
  - the walk's time (it has to be well under a millisecond a buffer);
  - the opcodes the walk doesn't know (a frequent one near the shadow pass needs checking and
    adding to the whitelist);
  - whether the scissor rects are in the eye texture's coordinates (the right eye's starting at
    2048);
  - whether 0xeb/0xec appear;
  - and, through `report.py`, the `agent.fovps` line.
- **Run 3**: does eye tracking start in a PS4 process without trouble, and does Hmd2 send
  anything? With no eyes in the headset it can't say how good the samples are.
- **Run 4** (not made):
  - the GPU time with the cut against without;
  - the cut line (draws under cut scissors, and why others weren't cut);
  - the TV, which should show shadows only in the middle.
- **Run 5** (not made): the gaze window's whole path with a gaze that moves (the TV shows the window move),
  and the GPU time with the window off-centre.
- **Run 6**: whether anything changes on the TV or in the GPU time.

Later, worn (the player needed):
- eye tracking with eyes: looking left, right, up and down on cue, for the samples' rate, how
  stale they are, and their axes;
- `agent.fov=2`: can the player see the shadows come and go? `agent.gaze=1` has to be in the
  settings from the start. (Not made: the approach was withdrawn after the runs. Eye tracking
  was later worn with inset mode: MULTIVIEW.md, 16.)

## The runs (2026-11-05, 00:00-00:23, replays of tests/rec1 at 60/2048, the headset on the table)

All four runs were clean: no crash, every frame shown, the game closed normally. Afterwards the
console was put back: `agent.elf` and `test.txt` deleted, `cvars.txt` as the player's. The
runner had asked the other resident VR payload (port 9087) to quit before the first run, as it
always does.

- **Run 1** (plain, then `r.ShadowQuality=0` from the start: `logs/fov1-plain.log`,
  `fov1-noshadow.log`): 14.0 ms a frame on average and 17.8 at most both times. Shadows were off
  only on the start screen (7.6 against 7.0 ms on average, 12.3 against 7.9 at most). **The game
  sets its shadow quality again at a scene load**, so a setting from the start is undone by the
  first load. In play its own value is 3, not the 2 read at the start (measurement 17).
- **Run 2** (census; shadows off from 130 s to 190 s, in play after the loads:
  `fov2-census.log`):
  - **Dynamic shadows cost about 0.1-0.2 ms a frame in play at 2048**: 13.8-13.9 ms with them
    off against 13.9-14.1 with the game's 3 just before. The read-back at 190 s confirms they
    stayed off. The 1.3 ms of measurement 21 was the start scene at 1500. This replay took a
    different course from run 1 after about 50 s (no load at windows 7-8, lower peaks), so this
    is one scene.
  - **The walk is cheap**: about 17 buffers and 5,600 draws a frame, 10-13 µs a buffer, under
    0.2 ms of CPU a frame, nothing it couldn't parse.
  - It **can't name the shaders**: the game's `SPI_SHADER_PGM_LO_PS` writes carry 0. The
    driver writes the value from the shader's register block (`sceGnmSetPsShader350`, Neo driver
    0x242a), so the real addresses must reach the GPU another way. The likeliest is
    `DMA_DATA` (0x50), about 290 a frame, the only opcode the walk didn't know, which can copy
    memory into registers. It also stops every cut.
  - A non-zero window offset was seen once at the start.
  - 0xeb/0xec are written once a frame with the driver's default.
- **Run 3** (eye tracking, `fov3-gaze.log`): `sceHmd2InternalGazeStart` and `GazeGetResult`
  return 0 in the PS4 process, after the palm tracker's start. The samples come at 120 Hz (the
  time at +8 advances). With no eyes in the headset every one is not valid, direction (0, 0, -1).
  Nothing else changed.
- **Run 6** (the PS4 Pro foveated window set to the middle half of each eye, every frame, from
  130 s to 160 s: `fov6-hw.log`): no crash, no wait timed out, and **no change in the GPU
  time**. With it: 14.1, 14.8 and 14.6 ms. After it: 14.6, 14.5 and 14.4. Run 1 had 14.1, 14.6,
  14.6 and 14.6 at the same windows (this run followed run 1's course). Nobody watched the TV,
  so a visual effect isn't ruled out, but it saves no shading work by itself.
- Runs 4 and 5 (the cuts) were not made: shadows are too cheap to be worth cutting, and the
  walk can't identify the shaders yet.

**What this changes**:
- The recommendation above, a gaze window for the shadow projection, is withdrawn. It would save
  about 0.1-0.2 ms, not about 2.
- The model (1.36 ms per million eye pixels) still says nearly all of the 14 ms follows the
  pixel count, but in passes whose individual costs aren't known yet: the base pass, lighting,
  translucency, TAA.
- What a foveation scheme here could cut is whatever those passes do outside a window, which
  needs per-pass GPU timing first. The obvious next measurement is timestamps written between
  the game's passes. It needs the shader identification solved first (`DMA_DATA`'s
  register writes followed), or markers of its own.
- Without that, the cheaper levers are the eye size and `vr.HiddenAreaMask` (step 2 of the plan:
  what the 16 % corner mask saves, a cvar-only run, which must be set after the loads as shadows
  must).

## Multiple views

A wide view at low resolution plus a small one at full resolution around the gaze, per eye,
composed by Hmd2 as two layers, is the one route left with large gains (about 5-7 ms at 2048, or a
sharper centre than today). Its scope, from the eboot: [MULTIVIEW.md](MULTIVIEW.md). (Built since
as inset mode, `agent.multiview`: on Hikari 1.00, 1440 eyes with 1200x600 insets at 1200 pixels a
unit of tangent, 12.1-12.3 ms a frame in steady play, MULTIVIEW.md 24; the eyes per game and rate
since: README, Features.)

## Open questions

- The shadow projection's share of the GPU time in play at 2048 (step 1); everything here scales
  with it. (Answered: about 0.1-0.2 ms, run 2.)
- Whether a PS4 process may open `/dev/hmd2_gaze`, and how stale the samples are (step 3).
  (Answered: it starts and returns 0, samples at 120 Hz, run 3; used worn by inset mode.)
- What 0xeb/0xec do on this console (step 8).
- PS VR2's visible outline within its field-of-view rectangle (step 9).
- The other two games: the same plugin and renderer, at other addresses (OTHER-GAMES.md, Method).
  (Done since for inset mode, which runs in all three: OTHER-GAMES.md, Inset mode.)
