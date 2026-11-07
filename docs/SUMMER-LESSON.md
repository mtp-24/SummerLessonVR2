# Summer Lesson on PS VR2

What was found porting AstroBotVR2's code to Summer Lesson: Hikari Miyamoto, and what the
port does. The sections below are a dated record, in the order things were found; some of their
conclusions were later found wrong. Where it stands:

## Where it stands (2026-11-07)

- **60 fps, reprojected to 120 Hz, 2000x2250 an eye by default** (PS VR2 takes textures up to
  4096 wide; over 3840 the side-by-side texture is registered at one eye's width). The full
  4-minute replay at 2048 had no dropped frame outside the game's own loading hitches (GPU ~14.5
  ms a frame on average, 17.8 at the most), but a 23-minute session at 2048 aborted: after the
  first scene change the game's CPU pool took the 160 MB its GPU pool had just released (told
  the truth about what was free), every later scene-change peak (3-5 blocks of 74 MB, released
  after) went to the spare, and 20 minutes on a block was refused, 4 MB short. Since 1948abb the
  game is told 1 MB free once PS VR2 runs (its CPU pool takes nothing), its 224 MB retry is
  refused, and the reserve is 154 MB: about 580 MB for the peaks (518 with the palm tracker on) against the 356 MB that
  session needed; at 1920 a 17-minute session needed nothing beyond the game's own pool.
  2026-11-08, played worn: at 2048 the start of day 2 (going to Hikari's room) took five 74 MB
  blocks from the spare at once and the fifth was refused (the game aborted, `logs/stock2048-day2`);
  at 2000 three days played through with three at the most (222 MB of ~358, `logs/stock2000-days`).
  2000 stays the default. A 25-minute inset session (2026-11-07, `logs/manual-1440`) aborted 0.8 s
  after its third save with the game's CPU memory used up: its CPU pool's 224 MB retry is refused
  (above) and its flexible memory (448 MB in all) ran out; the GPU side had ~360 MB to spare. Not
  measured yet: the flexible memory's headroom (a log line for it is the next step). 90 and 120 are switched off (they
  hold only smaller eyes; `make -C payload NATIVE=1` builds a test payload with them, clean since the newest-frame hand-off).
- **Frames go to PS VR2 at the hand-over.** The picture is complete then: measured with the
  fingerprints (`agent.fingerprint=1`), 1196-1200 of 1200 headset refreshes each 10 s show the
  finished picture. The viewport's "drawn" labels mark later GPU work, not the eye textures:
  waiting for them before passing a frame made 14% of refreshes show textures already being
  drawn over (the tearing seen worn), and the counter of frames "shown before drawn" in the
  sections below measured nothing real.
- **Memory**: the game's container holds a 512 MB segment past the 5120 MB it is told. PS VR2's
  buffers (153 MB) live in a 154 MB reserve there, and a block the game is refused in its range
  is given from the rest; the CPU pool's 224 MB retry is refused, as all along (granted, it left
  the GPU pool none at 2000). The palm tracker's memory stays in the game's range: its start
  stopped the game with it in the reserve.
- **Inset mode** (the default on Hikari, `agent.multiview=600`; `=0` for the stock picture): 1440 eyes with two 1200x600 insets that
  follow the gaze, the game's look kept in them, about 12.1-12.3 ms of GPU a frame:
  [MULTIVIEW.md](MULTIVIEW.md).
- **The TV**: the mirror copies the newest frame handed over once it is complete (its labels at
  its count - 1), never into a buffer the TV still shows or is about to (that was the TV's
  tearing), and holds back a copy that caught the game's black fill, for up to half a second
  (MULTIVIEW.md, 25); it never stops flipping the main port (a port left unflipped brought the
  console down, 2026-11-01).
- **The game's settings**: `cvars.txt` is read again every 2 s; the values the agent sets are put
  back when the game resets them at a scene change (README, Settings).
- **The loading hitches** (one frame of 100-180 ms, ~15-25 frames lost, about one a minute) are
  the game's own: unchanged when its refused memory was granted.
- **The other two games**, Chisato Shinjo (CUSA11682) and Allison Snow (CUSA11674), run the
  same program with its parts elsewhere; the payload knows their addresses (found from Hikari's
  code), and each has had its first runs (5 minutes, palm tracker on):
  [OTHER-GAMES.md](OTHER-GAMES.md). Inset mode is Hikari's only.

## The game

- SUMMER LESSON: Hikari Miyamoto, title id CUSA07434, content id
  HP0700-CUSA07434_00-SLHIKA01DLG00000 (the Asian English release), version 01.00 (its
  `param.sfo` in `/system_data/priv/appmeta/CUSA07434/`; the on-disc `sce_sys` has none).
- An Unreal Engine 4.12 build (`trunk_4_12_asia`, project name `closegame`), SDK 2.508. One
  6.5 GB asset pack; `sce_module` holds the game's own copies of Sony's head, face and hand
  tracker libraries (`libSceHeadTracker.prx` imports the camera and the pad itself).
- Its `eboot.bin` (80 MB, decrypted as the console's FTP serves it from
  `/mnt/sandbox/pfsmnt/CUSA07434-app0/` while the game runs) is an ELF with its code at
  virtual address 0 (file offset 0x4000) and its entry at 0x2593d90. The payload tells the
  game by its title id and version, and by the interpreter's name every PS4 eboot begins
  with, which says the image is mapped.

## How it uses PS VR (from its code)

The PS VR plugin (0x2f8d400 on) loads the HMD modules, calls `sceHmdInitialize315`, opens
the setup dialog (retried until it opens; its parameter carries no user id) and polls it to
FINISHED, waits for `sceHmdGetDeviceInformation` to say ready, opens the headset with the
user id that gives, reads the eye offsets, registers the headset with the tracker (retried
until registered) and reads the field of view. Later it asks
`sceHmdGetDeviceInformationByHandle` now and then; a status other than ready is logged as a
disconnection.

The renderer (Unreal's PS4 RHI, `GnmManager.cpp`, 0x33b00 on) opens the main video port,
registers its vblank event on an event queue of its own, `TrackerVblankQueue`, its flip
event on `VideoOutQueue` and a GPU end-of-pipe event on `SubmitEndOfFrameQueue`. Entering
VR it re-registers two display buffers, calls `sceVideoOutModeSetAny_` and
`sceVideoOutConfigureOutputMode_` with the PS VR mode (resolution 0xC1FFFFFF, refresh code
0x0d for its mode 1; 0x23, 90 Hz, for a mode 2 nothing selects), asks the reprojection for
its buffer sizes, initializes it (priority 256, cores 0x3f, pipe 3 queue 3) and sets its
display buffers 0 and 1.

Each frame in VR mode it submits its GPU work without a flip (PS VR's reprojection flipped
the port), waits for **two vblank events** on `TrackerVblankQueue` (reading the event count
between them), and hands the frame over with `sceHmdReprojectionStart`: two separate eye
textures from one of **two pairs**, a frame label from a ring of six, and the pose it drew
from. Flat screens go through `sceHmdReprojectionStart2dVr` instead. Unlike Astro Bot it
never calls `sceHmdReprojectionSetUserEventEnd`: the vblank event is its whole pace.

The tracker (0x2590cd0 on) opens the camera, queries and initializes the tracker, configures
and starts the camera (each retried until it succeeds), and runs a thread that reads a camera
frame, submits it (`sceVrTrackerGpuSubmit`), then calls `sceVrTrackerGpuWaitAndCpuProcess`
(or feeds the controllers' motion sensors with `sceVrTrackerUpdateMotionSensorData` when the
submit failed). Devices are registered with `sceVrTrackerRegisterDevice` (0 the headset, 1 a
DualShock 4, 2 a Move); results are asked with a prediction time counted from
`sceVrTrackerGetTime`. The headset is recentred through `sceVrTrackerRecalibrate`.

The controller is read with `scePadReadState`. The system status is read for its bytes 5
and 8 only, so the byte 4 pause Astro Bot used when the headset is taken off does nothing
here.

## Checked against the console's own PS4 libraries

The console's decrypted PS4-compatibility libraries (`/data/dump/system/common/lib/`, kept
under `dumps/12.40-ps4libs/`) settle two things the hooks rely on:

- `sceVideoOutGetEventCount` reads bits 12 to 15 of the event's `data`
  (`bextr 0x40c`), and `sceVideoOutGetEventData` the bits above 16, so the agent packs its
  refresh event as `(count << 16) | (1 << 12)`. `sceVideoOutGetEventId` takes the event type
  from the top 16 bits of `ident`; the game never asks for it.
- `sceHmdReprojectionQueryOnionBuffSize/Align` and `...GarlicBuffSize/Align` return
  constants (0x810, 0x100, 0x100000, 0x100) with no state behind them, so they are passed
  through to the real library.

## What the independent review found before the first send

Read against the game's code: the game draws **both eyes into one side-by-side texture**
(its uv scale is 0.5 / (f0 + f1), the right eye from u = 0.5), and it reads the field of
view structure as **inner tangent first** (u = 0 of the left eye's picture is at tan -f1, its
outer edge; its projection offsets agree). AstroBotVR2 gave each eye the texture from (0, 0)
and the tangents as outer, inner. Now the agent gives this game the tangents in its order,
and the hand-off puts each eye's viewport where its picture is (from the u of its outer edge,
as wide as its field of view; (0, 0) and (tw/2, 0) at full resolution), in the offsets that
follow each viewport size in Hmd2's layer (as PS5VR fills it for Horizon's side-by-side
buffer). Also from the review: PS VR's 90 Hz refresh code (0x23, the game's unused mode 2) is
pretended like 0x0d; the flip status keeps the real port's pending flips and current buffer
and fakes only the pace (the game flips the port blank and waits for that flip before it
changes the port's buffers); the game's flat screens (`sceHmdReprojectionStart2dVr`, its
title and loading screens) count as frames so PS VR2 is brought up on them, and are shown
as a 16:9 screen fixed before the seat, about 77 degrees wide, the same picture to both eyes;
the hand-off waits for the game's first frame without a deadline and registers a pair it
first sees later as a VR port buffer set of its own.

## What the port changed

- Lives beside AstroBotVR2 on one console: `/data/summerlessonvr2`, control port 9088,
  `summerlessonvr2.elf`, the libraries mounted at `/summerlessonvr2` in the game's sandbox.
- Everything that was Astro Bot's is gone: the resolution tables, the frame-rate triple and
  the 120 fps timer rule, the physics and soccer patches, the scene files, the anisotropic
  jump, the trigger gadgets and adaptive triggers, the R1 swipe, the L1 view gestures and
  the hand-held controller. The game runs at its own 60 fps, each frame reprojected to two
  of the headset's 120 Hz refreshes. `framerate.txt` other than 60 is ignored and said so.
- The agent's 120 Hz refresh event replaces the first vblank event the game registers (on
  the game's own queue), and its vblank thread starts on that. The event's data is packed as
  libSceVideoOut packs it.
- The hand-off registers the eye-texture pairs it sees (up to eight), and says in the log
  when a frame comes in a pair it has not registered.
- New hooks for what this game calls: `sceVrTrackerGpuWaitAndCpuProcess`,
  `sceVrTrackerUpdateMotionSensorData`, `sceVrTrackerGetTime`,
  `sceHmdReprojectionStart2dVr` (the title and loading screens, shown as a 16:9 screen),
  `sceHmdReprojectionUnsetDisplayBuffers`, the reprojection buffer queries (passed through,
  logged), `sceHmdGetDeviceInformationByHandle`, `sceHmdSetupDialogClose`,
  `scePadReadState` (the test mode's recording and replay moved there), `sceVideoOutClose`
  (the game opens the main port once for its display information before its renderer
  does), `sceVideoOutGetEventCount`, `SetFlipRate`, `UnregisterBuffers` and `WaitVblank`.

## Console runs

Each run states its hypothesis, its metric and the outcome. The runner
(`tools/testrun.sh`) asks a resident AstroBotVR2 (port 9087) to quit as well as a resident
SummerLessonVR2 (9088) before it sends, since a payload loaded while another scans the console
has brought it down.

### Run 1 (2026-10-31 21:53): the observing run, the game started by hand

The agent went in at the entry point (74 of 90 hooks found in 942 slots; the 16 not imported
are Astro Bot's). The init went as the code said: the display-information open and close, the
renderer's port, the vblank event on `TrackerVblankQueue` taken over, the setup dialog
answered three times, `sceHmdInitialize315`, the headset ready and opened with the user id
from the device information, eye offsets, the camera and tracker set up, the headset and a
DualShock 4 registered, the field of view read, the reprojection buffers queried (0x810 and
0x100000) and initialized, the blank flip and the buffer re-registration with the real flip
status, `ConfigureOutputMode` with refresh code 0x0d pretended, display buffers 0 and 1 set.
Frames came at the game's pace: a 1x1 black picture first, then frames from two pairs of one
**2496x1404 side-by-side texture** each (8_8_8_8 UNORM, tile mode 14, pitch 2560), uv scale
0.1793 = 0.5 / (f0 + f1), the left eye's offset 0.1698 and the right's 0.8302. That measured
the field-of-view order: 0.1698 = 0.9472 x 0.1793, the first tangent given, so the game puts
the first field at the left eye's left (outer) edge, as Astro Bot does; the review's inner-first
reading was wrong and is reverted. The audio ports opened (the BGM port as a main port). Then
the agent's VR thread mounted the libraries and the game **died inside
`sceKernelLoadStartModule` of `libSceAgcDriver.sprx`**, the first PS5 library, before the call
returned; the payload and the console stayed up. Nothing in the agent's log says why.

### Run 2 (2026-10-31 22:01): launched by the runner; the load refused by my own probe

No crash: `sceKernelLoadStartModule` returned EINVAL (0x80020016) and the log shows why: the
probe before it called `sceKernelGetCompiledSdkVersion` without its output pointer, so it
wrote the version (2.508 00 80 50 02) over the path. My bug, fixed. `/dev/klog` is held by
another reader on this console (EBUSY), so the kernel log is not available this way. The rest
was as run 1; the game drew 600 frames every 10 s with PS VR2 never up (headset and TV black).
The libraries are byte-identical to AstroBotVR2's on this console (174688 bytes each).

### Run 3 (2026-10-31 22:07): the crash caught

The watch logged it: thread 102281, SIGSEGV at 0x8016b5355 writing to 0xfe003a200. That code
is libSceAgcDriver's own (offset 0x5355 in it: a two-instruction helper that stores a PM4 NOP
header at the pointer it is given), and the pointer is in the driver's command-buffer area.
At its start (offset 0x4f0) the driver sets its base to the fixed address **0xfe0000000**,
allocates direct memory (type 0xc, 2 MB or 64 KB) and maps it with
`sceKernelMapNamedDirectMemory` with that address as a hint and flags 0 (no MAP_FIXED), then
uses 0xfe0000000, 0xfe0200000 and 0xfe0300000 as constants elsewhere. So in this game's
process the hint was not honoured (the kernel gave another address) and the driver wrote at
the address it assumes. Astro Bot (SDK 5.508) got the hint; Summer Lesson (SDK 2.508) did not:
either something of the game's sits there already, or an old-SDK process gets a smaller
address space.

### Run 4 (2026-10-31 22:15): my probe killed the game

The probe mapped a page at 0xfe0000000 with MAP_FIXED, which replaces whatever is there, then
unmapped it: the game died before it opened its video port. A mapping of the game's own was
there. The probe became a read-only memory map.

### Run 5 (22:20): the memory map

At the entry point the process already has 0xfe0000000-0xfe0010000 (no access, 64 KB),
0xfe0200000-0xfe0204000 and 0xfe0300000-0xfe0400000 (read/write), nothing above. The driver
(init at 0x4f0) sets its base to 0xfe0000000 and maps its 2 MB command-buffer area there with a
hint; over the reservation the map fails, the driver only prints that to stderr (0x7cc3) and
its first write, a NOP into slot 3 of its mode-0 table (base + 0x3a200, table at 0x10260),
crashed the game. Everything else in the driver reads the base from its variable, so
`prepare.c` moves the one constant (12.40: the movabs immediate at 0x503, checked against the
stock bytes) to 0xfe0400000 (`agc_base_imm` in the firmware profile; cache tag v3; since
2026-11-04 the movabs is found by its bytes, once, on any firmware, with no profile field; cache
tag v4).

### Run 6 (22:26): the libraries load; no direct memory left

All four PS5 libraries load, Hmd2 opens the headset; every PS VR2 allocation fails.

### Run 7 (22:28): the game's direct memory

It takes 8 MB, 256 MB, 3624 MB (type 3, its GPU heap), 200 MB, then asks how much is free and
takes all but 1 MB (997 MB) of the 5120 MB budget.

### Runs 8 and 9 (22:31, 22:39): PS VR2 up

`sceKernelAvailableDirectMemorySize` reports DMEM_KEEP less than is free, so the game's last
pool comes out that much smaller and PS VR2 gets it (153 MB used: tracker 32, display buffers
96, TV mirror 24, small buffers). VR mode at 120 Hz, head tracking live, the TV mirror on, and
from the second 10 s window on every window reads 600 handed over, 600 shown, 599 shown again,
0 never shown. Open: with 256 MB (run 8) or 160 MB (run 9) kept, the game, short in its own
pool, retries a 256 MB allocation several times a second (run 7, stock, never did); whether it
costs anything visible is for the play test. The console went down once in a payload swap
(5 s after the old one's port closed, before the new one's first line): the runner now waits
15 s.

## Frame rate and resolution: what the game's code says (2026-10-31)

**The frame rate.** The game's VR plugin (Unreal 4.12's Morpheus plugin) reads
`PSVRFrameSequence` from its runtime settings (`/Script/MorpheusEditor.MorpheusRuntimeSettings`)
and keeps a frame sequence at +0x2b8 of the plugin: 1, 60 fps reprojected to 120 Hz, the one the
game ships with (setting unset or 0); 2, native 90 Hz (setting 1; refresh code 0x23); 3, native
120 Hz (setting 2; refresh code 0x0d, one vblank event a frame instead of two, the 120 Hz
latency figure). The two stores of 1 are the plugin's constructor (0x2f8d24b) and the default
(0x2f8e564); `framerate.txt` saying `120` changes both to 3 (checked against the stock bytes),
the agent then raises the game's event every refresh and gives Hmd2 full-rate frames. Unreal's
clock follows real time, so unlike Astro Bot nothing of the game's timing needs a patch; what
decides it is the GPU time, which the health line now reports (first command buffer to the
game's GPU-done event, ident 0x40 on its own queue): a frame a refresh needs it under 8.3 ms.

**The resolution.** First read as "the panel size the game is told times its
`ScreenPercentage`": measurement 12 (a 2308x1300 panel reported, the texture still 2496x1404)
showed the panel plays no part — `sceHmdGetDeviceInformation`'s size (+0x280/+0x284 of the
plugin) only reaches the plugin's `GetMonitorInfo` (0x2f8eb00). The eye texture is made by
`CalculateRenderTargetSize` (0x2f8fb20, an `IStereoRendering` slot; the plugin's vtable
address point is 0x465a248): with `bOverrideScreenPercentage` (+0x294, on) it reads the live
`r.ScreenPercentage` cvar (the plugin caches its `TConsoleVariableData` at 0x4bc5978 on the
first call: [0] the game thread's value, [1] the render thread's) into +0x298 and returns
ceil(1920 x SP/100) by ceil(1080 x SP/100), the viewport being the game's 1920x1080 screen;
`UpdatePostProcessSettings` (0x2f8ff60) gives the views the same percentage, so each eye is
960x1080 times it, and `NeedReAllocateViewportRenderTarget` (0x2f8fc40) compares the texture
to that size every frame and remakes the textures when it differs. The game's 130 gives
1248x1404 an eye. `resolution.txt` now gives an eye width (960 to 2400; 2048 at the most later): the percentage is
width/9.6, written into both cvar values by the agent once the game's frames run (the engine
pokes in `gpu_label_thread`), and the agent takes a pair of a new size at a known base as a
new pair. PS VR2's panels are 2000x2040 an eye; Astro Bot draws 2000x2136.

**The plan, one measurement at a time.** 1: GPU time at the shipped mode and size, in the
menus and in play (this build, no settings). 2: `framerate.txt` 120 at the shipped size: the
health line should read about 1199 handed over and shown, 0 again, 0 never shown, and the GPU
time under 8.3 ms. 3: resolution steps (1500, 1728, 1920) at whichever rate holds, each
judged by the same line. Native 90 Hz (mode 2) is the fallback if 120 does not hold; the
agent does not drive Hmd2's 90 Hz mode yet.

## Frame rate measurements (2026-10-31)

- **Measurement 1 (22:51):** shipped settings. The game maps compute pipe 5 queue 0 itself, which
  is why hand tracking's submits there were refused (0x8a6d0103); hand tracking moved to pipe 6
  and then worked (measurement 2 and a worn play test). The GPU-time line counts few frames:
  the game seldom waits on its GPU-done event, so that measure is of little use here.
- **Measurement 2 (22:53):** `framerate.txt` 120: the native 120 sequence is active (one vblank
  wait a frame, no GetEventCount between two) but the game drew exactly 60.
- **Measurement 3 (22:56):** each frame handed over ~4 ms after its refresh (8.4 at most), still a
  refresh apart: a cap. Read in the code: `UEngine::GetMaxTickRate` (0x1975740) returns `t.MaxFPS`
  if set, else, when `IsAllowedFramerateSmoothing` (0x1975670, slot 0x248 of `UGameEngine`'s
  vtable at 0x453fc38) says so, the running average clamped to `SmoothedFrameRateRange` (62 by
  default). The 120 mode now also makes that check return false.
- **Measurement 4 (22:58): the console went down** within ~35 s of the launch, the first run with
  smoothing off (the swap and the launch had succeeded). Cause not known yet; `framerate.txt` is
  to stay off until the log it left is read and the engine's loop is understood.

- **Measurements 5-8 (23:09-23:19):** with frame-rate smoothing's cap raised, then its
  numerator, then the main loop's fixed-rate clamp skipped: still exactly 60, the GPU at
  5-7 ms a frame (timed from the game's own frame labels, viewport static 0x46917f0). The
  engine's fields at run time: flags 0x1b (smoothing force-disabled), `t.MaxFPS` 0. All of
  those patches were to code this build never reaches, and they are gone.
- **Measurement 9 (23:22):** a census of the game's sleeps and equeue waits by call site (the
  census was removed for the release; history): the
  main loop sleeps 600 times a window, ~13.6 ms each, at 0x196f2bb, the tick-rate limiter's own
  sleep. So `GetMaxTickRate` does return 60.
- **Measurement 10 (23:25):** the live engine object's vtable: the slot the loop calls holds
  0x15aea50, `UGameEngine::GetMaxTickRate`, not `UEngine`'s 0x1975740. Read: with `r.VSync`
  on it returns `SmoothedFrameRateRange`'s upper bound (60.0, the engine object's +0x634;
  bound type +0x630, 2 = open) whenever `UEngine`'s gives 0.
- **Measurement 11 (23:29): 120 fps.** The agent writes 125.0 into that bound in the 120 mode
  (no code patch at all): 1202-1210 frames drawn a window, 1199 shown, 0 shown again, 3-9
  never shown (frames drawn faster than the refresh), the GPU 5.4 ms a frame (6.9 at most),
  the limiter's sleep gone. `framerate.txt` 120 = the native 120 sequence (the two stores of
  the frame sequence) + this.
- Measurement 4's console crash was therefore not its patch (a no-op): a sporadic crash
  during the PS VR2 bring-up, with the payload's ptrace watch attached to the game. The watch
  now runs only with `/data/summerlessonvr2/watch` (debug builds).

## Resolution measurements (2026-10-31, all at 120 fps)

- **Measurement 12 (23:42): the panel is not the lever.** `resolution.txt 1500x1690` told the
  game of a 2308x1300 panel; it drew 2496x1404 as before (1199 shown / 0 again / 3-9 never,
  GPU 5.4 ms). The code reading above followed.
- **Measurement 13 (23:46): 1500x1688 an eye** (`r.ScreenPercentage` 156.25, the first eye
  pair already 3000x1688): 1200-1207 drawn a window, 1199 shown, 0 again, 1-8 never shown, the
  GPU 6.8 ms a frame (7.9-9.0 at most).
- **Measurement 14 (23:49): 1728x1944 an eye** (180): 1199 drawn and shown a window, 0 again,
  0 never shown, each frame handed over 6.8 ms after its refresh (7.7 at most): the GPU is now
  the pacer, just inside the 8.3 ms. The health line's GPU figure under-reads here (2.4 ms,
  9.0 at most): it is timed from the frame's first command buffer, which the render thread
  only submits once the frame before is done, so the figure is only honest when the GPU has
  slack.
- **Measurement 15 (23:52): 1920x2160 an eye** (200): GPU-bound, 964-1026 drawn a window,
  170-240 shown twice (12.9 ms at most): not a 120 fps size. It would hold at 60 (the shipped
  reprojected mode, 16.7 ms), and probably at a native 90 the agent does not drive.
- The first window of every run shows ~730 "never shown" frames: the game's start-up (the
  logo and loading screens at 120 fps on the flat picture), not play.
- The console is left at `framerate.txt 120`, `resolution.txt 1728` for the play test; 1500 is
  the size with margin (the start scene at 6.8 ms), 1248 the game's own.

## More performance? Measurements 16-27 (2026-11-01 00:12-00:32)

The question was whether the PS5 has more to give. What the console and the code say:

- **Neo mode is already on.** The title declares PS4 Pro support (`param.sfo` ATTRIBUTE
  0x480c000 has the 0x00800000 bit), is built with SDK 3.508 and asks `sceKernelIsNeoMode` at
  nine sites (memory pools +512 MB, the RHI's Neo settings, the device-profile name); inside
  the game the answer is 1 (measurement 16), so the PS5 runs it in its PS4 Pro configuration.
  `sceKernelIsNeoMode` is `sysctl kern.neomode` (libkernel 0x21720); the console keeps
  `libSceGnmDriverForNeoMode.sprx` beside the base driver. Clocks and CU count are the compat
  configuration's; nothing in user space raises them (`sceKernelSetNeoModeClock` is an ioctl
  on `/dev/mp1` for the system).
- **The CPU is not the limit**: the game thread holds 120 fps and idles (the hand-over is at
  the refresh when the GPU has slack). **Memory** is tight (the game takes its whole 5 GB
  budget and retries a 224 MB pool the agent withholds for PS VR2) but not a frame-time
  limit. **The GPU is the limit**: 8.3 ms a frame at 120 fps, shared with PS VR2's own
  reprojection and the agent's TV mirror.
- **`cvars.txt`** (one line `name` reads a console variable, `name=value` sets it) now reaches
  the game's `IConsoleManager` (static 0x46b4350; `FindConsoleVariable` slot +0x88; the
  variable's `AsVariableInt` +0x38 / `AsVariableFloat` +0x40 give its two-thread data, both
  written). The injector hands the text over by `CMD_CVARS`. The settings read (measurement
  17): shadows quality 2, 1024 px, one cascade; FXAA-level AA (`r.PostProcessAAQuality` 2,
  TAA samples 8; later found to be TAA, its quality raised to 3 after the start: FRAME-COPY.md); AO levels 2; SSR and motion blur off; DOF 2; bloom 1; tonemapper 5; eye
  adaptation 2; `sg.*` 3/3/3/0.
- **What each setting is worth at 1500x1688** (the game's GPU time in the start scene,
  baseline 6.9-7.0 ms, measurement 20): AO off 0 (18); all post-process off (DOF, bloom,
  tonemapper, eye adaptation) 0.2 (19); **dynamic shadows off (`r.ShadowQuality=0`) 1.3 ms,
  to 5.6** (21); everything off 5.4 (22); shadow quality 1, 512 px maps, half distance: 0
  (23, 24, 27) — the shadow cost is the per-pixel projection, not the maps.
- **Scaling**: 1248x1404 5.4 ms, 1500x1688 6.9: about 1.9 ms per million eye pixels over a
  2 ms fixed part, so 1728x1944 lands at the 8.3 ms refresh (measurements 14 and 26 both
  hand frames over 6.9 ms late, with or without shadows: the lateness at 1728 is not the
  game's shadows alone — the compositor's and mirror's share grows with the texture too).
- The runner's `PS5_REUSE=1` keeps the resident payload: a file-only run never swaps.
- The console is left at `resolution.txt 1500`, `cvars.txt` empty: 120 fps with ~1.4 ms of
  slack and the game's own look. For more pixels at the same slack: `1728` with
  `r.ShadowQuality=0` in `cvars.txt` (no dynamic shadows).
- **The second swap crash (00:05)**: the console went down between "sending" and the new
  payload's first line, as in run 9; nothing was left in agent.log. The console's own
  `klogsrv` (port 3232) holds /dev/klog (hence the payload's EBUSY): the runner's next swaps
  are to be made with `nc <console> 3232 > log &` running for the kernel's words.

## Where else is performance lost? Measurements 28-30 (2026-11-01 00:38-00:42)

The agent's own per-frame work, switched off one piece at a time at 1500x1688 (`cvars.txt`
lines `agent.<option>=<value>`, read before PS VR2 starts):

- **Palm tracking off** (`agent.hands=0`, measurement 29): the game's GPU 6.3-7.1 ms against
  the 6.8-7.0 baseline (28) — the tracker's compute runs on its own pipe and costs the game
  nothing measurable. `agent.hands=30` halves its rate if ever wanted.
- **The TV mirror off** (measurement 30) **brought the console down** about a minute into the
  run: the mirror thread also registers and flips the game's main display port (nothing else
  does in VR mode), so the display path lost its flips. The option is gone; the mirror stays
  (it already runs at 60 a second, every other frame).
- The CPU side, from the sleep census: the game's main loop and a worker poll with
  microsecond sleeps (0x3c803 up to 200 k times a window, 0x196f2f9 25 k) — CPU time the
  PS5 has to spare; the frame hand-over is at the refresh whenever the GPU has slack, so no
  CPU path is on the frame's critical path.
- Conclusion: outside the game's own rendering (shadows above all), nothing of the agent's is
  worth a measurable share of the 8.3 ms. The GPU budget is the game's.

**The swap crashes (run 9, 00:05, and measurement 30's is another kind).** Two swaps out of
nine brought the console down before the new payload's first line; the libs mount was
unmounted every time before (logs), so that is not it, and no kernel log was streaming across
either. The cure is to stop swapping: the payload now injects `/data/summerlessonvr2/agent.elf` (debug builds)
when the file is there (else its built-in copy; chosen again before every injection), and
`tools/testrun.sh` uploads `agent/build/agent.elf` every run — an agent change is a file
upload plus a launch with `PS5_REUSE=1`. Only a change to `payload/` itself needs a send, and
after a re-jailbreak the first send is not a swap. For any swap that remains:
`nc <console> 3232 > klog.txt &` across it (the console's own klogsrv).

## Play test 4 (2026-11-01 00:50-01:05): 1728x1944, dynamic shadows off, 120 fps

Two sessions, 68 windows of 10 s (logs/play4.log). Only the start scene held 120 (windows
37-40: 1199 shown, 0 doubled). In play the game ran at 86-105 frames a second with 150-490
frames shown twice a window — **24.5 % of all frames doubled (15 784 of 64 307), 393 hitches
of two refreshes or more**, the GPU 11-13 ms at the most in nearly every window. The start
scene, where every measurement so far was taken, is about half the cost of play: 1728 is not
a 120 fps size in play even without shadows. Scaling the play figure (about 10.5 ms at
3.36 Mpx an eye pair) puts 1500x1688 with shadows near 9-10 ms and the stock 1248x1404 near
7-7.5: the next play tests settle it (1500 with shadows, then 1248).

## Replay benchmarks and diagnostics (2026-11-01 02:00-03:20)

All by replaying `tests/rec1` (a recorded 4-minute session at 1248/120: 1356 controller and
982 head samples) with the headset on the table, `tools/testrun.sh … "replay worn"`;
"play windows" exclude the scene loads (GPU spikes over 25 ms). The replay reproduced the
recording window for window (rep1 against rec1).

| mode | eyes | play windows | shown twice | never shown | GPU avg / max |
|---|---|---|---|---|---|
| 120 | 1248 | 15 | 0 | 0.58 % | 6.4 / 8.9 |
| 120 | 1312 | 15 | 0 | 0.45 % | 6.9 / 9.9 |
| 90 | 1728 | 16 | 0 | 0.90 % | 8.4 / 12.9 |
| 90 | 1824 | 15 | 3.05 % | 0.08 % | (GPU-bound) |
| 90 | 1920 | 15 | 10.9 % | 0.12 % | (GPU-bound) |
| 60 | 1248 | 17 | (each frame twice by design) | 0 | 6.5 / 8.9 |

- "Never shown" frames (0.5-0.9 %) follow GPU spikes over the budget: a late frame lets the
  next start at once and two reach the compositor in one refresh (one animation step skipped).
- **Dynamic resolution through the render thread's value of `r.ScreenPercentage` alone does
  not work** (dyn1: texture 1728, render value 130: 25 % doubled, as a plain 1728). The views
  take their percentage from the plugin's +0x298 (UpdatePostProcessSettings 0x2f8ff60 copies it
  to the view's +0x368), which CalculateRenderTargetSize writes from the game thread's value
  every frame; no spare field in the plugin (+0x25c is used, +0x2a8-0x2b4 too). A working
  dynamic resolution needs a code change (the views' percentage from a field the agent writes,
  the texture fixed) or re-registering eye buffers on each change.
- **Pose diagnostics**: the eye-texture pairs are used in turn (0,1,2: reused after exactly 3
  frames 98-99 %), so the compositor never shows a texture being drawn into; the game's six
  frame labels are written only by the hand-over and read by nothing else in the game. Hmd2
  does correct each frame for its pose at 120 (a 3 deg yaw added to the frame's pose moves its
  reprojection rotation, B+0x40, by sin 3 deg). While the headset was worn (rec1), the pose
  given to the game moved 0.01-0.1 deg an ask, predicted = raw; under hand-held motion the
  pose chain was exact (handed back 0.000-0.03 deg, converted back 0.000 deg / 0.000 mm) and
  the 10 ms prediction 0.05-0.1 deg off (0.5 at most). The shaky view seen in the headset is
  therefore not in the pose given to the game; the per-refresh reprojection rotation in a
  play scene is the next thing to log (agent.cbdump=2 caught the 2D title screen's layout).
- **Crash 03:19**: within 40 s of the first run at 2000 px eyes (60 fps): the first time a
  side-by-side texture wider than 3840 (4000) was registered with the VR port. The agent now
  caps r.ScreenPercentage at 200 (eyes 1920, texture 3840). The 01:39 crash was also a 60 fps
  run ~37 s in, after a run at another rate (the 02:41 60 fps run, also after one, was fine).

## The shake at native 120 (2026-11-01 03:30-04:20, worn)

- 60 fps at 1920x2160 eyes (each frame shown twice, GPU 13 ms): **stable** to wear.
- Crossed tests: A, the game at 60 with Hmd2 at full rate: **stable**; B, the game at native 120
  with Hmd2 at half rate: **shaky**. So the shake follows the game's native-120 sequence, not
  PS VR2's compositor mode.
- Ruled out by measurement or by wearing: the pose chain (exact at rest and under hand-held
  motion), the prediction horizon (agent.predict=10: still shaky), eye-texture reuse (pairs used
  in turn), Hmd2 ignoring the frame's pose (it corrects for it), labelling frames with their
  own camera pose by frame number (run C: still shaky; label and camera 0.08-0.1 deg apart),
  temporal anti-aliasing (run D: still shaky), the palm tracker (run E: still shaky).
- Run D: stable at first, shaky once the game reached steady 120.
- play4 (1728, no shadows: ~95 fps, GPU-bound) felt best, play5 (1500: ~112) unstable on the
  title screen, play6 (1248: 120) extra dizzy: the **same agent build** (242992 bytes); the
  closer to a steady native 120, the worse.
- The game's time step (FApp::DeltaTime, 0x46690d0): at 60 16667-16689 us (sd 0.13 ms); at 120
  with the cap at 125, 8000-9108 us (sd 0.14-0.36 ms): the engine's limiter (8.0 ms) and the
  119.88 Hz vsync beat against each other.
- **Opening the engine's cap (agent.cap=1) hung the console** ~45 s into the run (04:19; no
  kernel message). Never again. Other never-again: agent.mirror off, payload swaps without the
  kernel log, textures wider than 3840.
- Late update: the plugin's render-thread function 0x2f929d0 (IsInGameThread 0x139fe0; game
  pose in +0x80, render pose in +0x100; the apply at vtable +0x268) runs once at start, not
  per frame. The per-frame asks: the game thread's camera (0x2f91980 from 0x2f8d9c0, ~2.6 a
  frame, horizon 35 ms) and the render thread's device update (0x2592070, horizon 30.8 ms; its
  result is the frame's ts). Each ask carries the game's frame number (user_frame_number).

## Back to 60, and the eye split (2026-11-01 04:30-05:10)

- **The engine cap at the refresh** (120 or 90 instead of 125) makes the native modes' time step
  as regular as 60's (8333-8403 us, sd 0.08 ms, 0 frames over 0.5 ms off); now the default in the
  native modes. It did not cure the jitter: worn at 120 (with the cap, then also with
  agent.predict=10, then with agent.layerpose=1: the compositor told a fresh pose so it corrects
  almost nothing), the title stays stable and the menu (New game / Continue / Settings: no
  character there) jitters "as if the view is not locked in place", once with black triangles in
  one eye (the right, perhaps) with the head still. The TV, showing both eyes as drawn
  (agent.mirrorboth=1), showed no triangles. Not found; 60 is the recommended mode.
- **The eye split**: the side-by-side texture is registered with the VR video port at one eye's
  width when it is over 3840 wide (vr2.c pairs_register); the frame descriptors keep the whole
  texture. 2000x2250 eyes (a 4000x2250 texture) and 2048x2304 (4096) run clean at 60 (every frame
  shown twice, GPU 11.2-15.6 ms). 2200 (4400 wide): every frame refused by Hmd2's SetParam
  (0x8a720016, a size check; flip 0x8a720018): **4096 is the compositor's limit**. 2400: the game
  quits ~30 s in, a 106 MB type-3 direct-memory allocation for its render targets refused
  (0x80020023). The agent caps r.ScreenPercentage at 213.33 (eyes 2048).
- The kernel log: the payload's klog thread and klogsrv race for /dev/klog after each jailbreak;
  the runner uses klogsrv when it streams, else the payload's klog.txt (from the run's start,
  copied every 5 s), and launches nothing without one.

## Cleanup, memory, and the mark button (2026-11-01 05:10-11:35)

- The 90/120 experiments that did not help are gone (agent.serialize, labelcam, layerpose,
  sprender, predict (back later as `agent.predict`), posetest, cbdump, wide, the open cap; the parity ring, stack walks and
  late-update counters): 502 lines. Kept: the cap at the refresh (default in the native modes),
  agent.cap=<fps>, agent.hmdrate, agent.mirrorboth, agent.hands, and the health line's time step,
  pose lead and jitter. A 120 regression run after the cleanup: 1199 shown, 0 twice.
- **2000 is out of memory, not GPU**: the full 4-minute replay at 60/2000 ended ~90 s in with the
  game quitting after a 74 MB type-3 (render target) allocation was refused. PS VR2 and the palm
  tracker use 220 of the 224 MB the agent keeps (tracker 32, the reprojection's display buffers
  96, the TV mirror 25, three 8 MB, the palm tracker 60). A long worn 1920 session had no
  refusal. Ways to 2000-2048 later: raise the game's direct-memory budget in the kernel (none
  known to exist; kern.dmem.game_budget_limit, kern.app.dmem_size_pid are candidates), or trim
  PS VR2's own share (the 96 MB display buffers, the 25 MB mirror).
- A console crash at the launch of a plain 60/1920 run (05:20; the kernel log ends in routine
  memory reports): the sporadic launch-time crash, no trigger found.
- **The mark button** (L3+R3; debug builds only since 2026-11-07): a second-by-second flight recorder (hooks.c rec_tick/rec_pad) logs
  the 10 s before and 5 s after each mark; scePadSetVibration buzzes to confirm.
