# How AstroBotVR2 works

> Written for [AstroBotVR2](https://theghostonline.dev/theghostonline/AstroBotVR2), which this
> port is built on. The mechanism is the same here; what differs for Summer Lesson is in
> [SUMMER-LESSON.md](SUMMER-LESSON.md). The paths below are this port's (`/data/summerlessonvr2`).
> Astro Bot's frame-rate modes (90 and 120 fps, its physics and soccer patches,
> `frametimer.txt`) and section 10 (sharpening, FXAA, anisotropic filtering) are not in this
> port: it runs the game's own 60 fps, and its picture is the game's own (README, Picture).

Astro Bot Rescue Mission is a PS4 game. On a PS5 it runs in backwards
compatibility mode: PS4 system libraries, a PS4 process, and a PS4-mode GPU
context. PS VR2 is driven by PS5-only libraries (libSceHmd2 for the headset
and its reprojection, libSceVrTracker2 for tracking, libSceAgc and
libSceAgcDriver for the GPU). None of them were made to run in a PS4 process.

AstroBotVR2 has two parts:

- **the payload** (`payload/`), a normal PS5 payload that stays resident. It
  prepares the PS5 libraries once, puts the agent into the game when it
  starts, and does the things only a kernel-level process can do (mounting
  files into the game's sandbox, re-pointing import slots, patching the AV
  control service while the game runs).
- **the agent** (`agent/`), freestanding code that runs inside the game. It
  answers the game's PS VR calls and drives PS VR2.

All offsets below are for firmware 13.60 and Astro Bot 1.00 (CUSA12392). Each
supported firmware's own values are in `common/firmware.h`; what differs on
11.60 is in [FIRMWARE-11.60.md](FIRMWARE-11.60.md).

## 1. First run: preparing the PS VR2 libraries

The payload builds four files in `/data/summerlessonvr2/libs` from the console's
own firmware (`payload/prepare.c`):

1. Each library is read from `/system/common/lib` and decrypted with
   idlesauce's self pager.
2. The ELF ABI version is changed from 2 to 3. PS5-only libraries are refused
   in a PS4 process ("ABIVERSION mismatch") otherwise.
3. In libSceHmd2, the PLT stub for `sceKernelGetProsperoCompiledSdkVersion`
   is replaced with code that returns SDK 8.05. A PS4 process has no PS5 SDK
   version, and Hmd2 checks it before anything else.
4. The result is wrapped as a fake-signed SELF (the SDK's `make_fself.py`
   format, written by `fself.c`), which kstuff lets the game load.

Each decrypted library is checked against the SHA-256 of its build for the
running firmware before it is changed, so an unknown firmware fails cleanly
instead of producing a broken file. The prepared files are tagged with the
firmware they were built for, and rebuilt after a firmware change.

The same step translates Hmd2's reprojection shader (see section 5) and
writes it to `/data/summerlessonvr2/hmd2_gcn.bin`.

## 2. Getting into the game

The payload watches for an `eboot.bin` whose image has Astro Bot 1.00's
recentre code at offset 0xc48320 (the same build check AstroQuest uses).
Then it:

- sets the PS VR2 bit (0x400) in the app info attribute3 the kernel keeps for
  the process, so the system treats the game as a PS VR2 app;
- attaches with ptrace as soon as the game starts, while it's still in
  libkernel's start code, and puts a breakpoint on the eboot's entry point. The
  main thread stops there before any of the game's own code has run, which is
  where the agent goes in. So the agent answers the game's very first PS VR
  calls, and the system's "connect PS VR" screen never comes up;
- if the payload was sent with the game already running, the entry point is
  long gone. Then it points the import slots of the game's first PS VR calls
  and of functions it calls all the time at unmapped addresses. The first
  thread that calls one stops on the fault in game code, at a call boundary
  (a thread parked in a system call can't be used: the kernel's restart path
  would undo the register changes). The slots go back, and the call that fired
  carries on into the agent's hook. A slot whose module loads meanwhile is left
  as the linker set it;
- maps the agent, applies its relocations, resolves its imports by NID, and
  points the game's import slots (its GOT) for every hooked function at the
  agent;
- detaches. The game never stops for more than a moment.

Any other copy of Astro Bot (another region or version) gets a notification
saying which one it is, instead of the payload waiting in silence. So do the
other ways it can't start: the wrong firmware, or the libraries not loading.

The output only switches to VR once PS VR2 says it's ready. Switched while
the headset is off, the system puts its own "turn on PS VR2" screen over the
game, so the agent shows a notification and waits instead.

From then on the payload relays the agent's log and serves its requests over
a small shared memory block: mount the libraries into the sandbox, re-point an
import of a loaded library, resolve imports of libraries loaded later. When
the game loads a system module, its imports from that module are bound again
and the payload puts the hooks back.

## 3. Pretending to be PS VR

The game talks to PS VR through libSceHmd, libSceVrTracker, libSceCamera and
libSceHmdSetupDialog. The agent replaces those calls (`agent/hooks.c`):

- The headset is always connected, and its setup dialog closes as soon as it
  opens. A real one the game opened before the agent got in is closed the
  proper way (Close, then wait for it to finish), or the system's "connect PS
  VR" screen would stay up.
- The headset reports PS VR2's field of view (tangents 1.8418 out, 0.9472 in,
  1.3290 up and down) instead of PS VR's, so the game renders exactly what the
  PS VR2 lenses show.
- The camera delivers blank frames, and the tracker answers every request with
  the PS VR2 head pose converted to PS VR's tracking space (section 6).
- `sceHmdReprojectionStart`, where the game hands over each finished frame
  (two eye textures, the head pose it rendered for, and the UV mapping), is
  where the frames are collected for PS VR2.
- The game's main video output asks for PS VR's 119.88 Hz mode. That is only
  pretended: with no PS VR attached, the real call makes the system suspend
  the game.

## 4. The PS VR2 libraries in a PS4 process

Once the game is rendering VR frames, the agent starts a thread that loads
the four libraries. The game's sandbox can't see `/data`, so the payload
mounts `/data/summerlessonvr2/libs` read-only into the sandbox (nullfs) while the
agent loads them, and unmounts it after. Up to 1.0.1 the payload copied each
library into the sandbox and deleted it after loading; the sandbox's files
live in a tmpfs every sandbox shares, and loading from it could panic the
kernel.

Then the libraries need several things to look like they are in a PS5 app
(`agent/ps5compat.c`). Each one is an import of one library pointed at a
wrapper, so the game's own view of the system doesn't change:

- **PS4 or PS5.** libSceVideoOut refuses the VR output bus and the PS5 pixel
  formats to a PS4 process. Its `sceKernelIsPs4Process` and `get_authinfo`
  imports answer "PS5" (and capability bit 62) on the threads the PS VR2
  libraries run on: the agent's VR thread and every thread they create, which
  are tagged through their `scePthreadCreate` import.
- **App info.** Hmd2 reads attribute3 for the PS VR2 bit, as does VideoOut,
  which caches it (data +0x358d4).
- **Foreground.** With the headset in VR mode the system reports the PS4 title
  as running in the background (status byte 5). Both Hmd2 and the game act on
  that (Hmd2 shows black, the game pauses), so the byte is cleared for both.
- **SDK version.** VrTracker2 gives up (0x8a740009) without a PS5 SDK
  version, so it is given 7.00.
- **The AV control service.** SceSysAvControl refuses the PS VR2 output for a
  PS4 title in two places: the layout handler (a branch at 0x41739b) and
  `checkVideoModeForChangeOutput` (0x431ed8). The payload patches both in the
  service's memory while the game runs, and puts the original bytes back when
  the game closes or the payload exits.

## 5. The GPU: running PS5 GPU work in a PS4 context

This is most of the work (`agent/gen1.c`). The game's GPU context is
"gen1", meaning PS4 mode: it runs GCN shader code and reads GCN resource
descriptors, and some RDNA registers are privileged in it. Hmd2 builds its
reprojection as an RDNA compute dispatch and submits it through AgcDriver.
Unchanged, the GPU hangs. Every command buffer Hmd2 submits is rewritten
before it reaches the GPU:

- **The shader.** Hmd2's reprojection shader (3501 dwords of RDNA code) is
  translated to GCN (3853 dwords) by `payload/rdna2gcn.c`. Most instructions
  encode the same way. The rest are rewritten: scalar memory loads (SMEM to
  SMRD, with dword offsets and an SCC-preserving add for register plus offset
  addressing), SDWA to VOP3, literal operands moved into scratch registers,
  `v_cmpx`, `v_lshl_add_u32`, `v_add_nc_u32`, `s_andn1_saveexec`, wait
  counters repacked, and the extra `s_nop` GCN needs after a VALU write to an
  SGPR or EXEC. The dispatch is pointed at the translated code with GCN
  register counts.
- **Descriptors.** Image descriptors (T#) Hmd2 writes in the RDNA layout are
  converted to GCN (format, tiling and pitch). The game's own eye textures
  keep the game's original GCN descriptors, because Hmd2 rewrites their size
  fields in the RDNA layout.
- **Packets.** Profiler markers (writes to privileged registers) become NOPs,
  and RELEASE_MEM and ACQUIRE_MEM are rewritten in their gfx7 form.
- **Headset buffers.** GCN can't address the RDNA swizzle Hmd2 uses for the
  4000x2040 headset buffers, so they are registered linear. VideoOut accepts
  linear display buffers only on a port with the "enhanced" buffer attribute,
  which is set on the VR port alone.
- **Flips.** Hmd2 flips the headset from the GPU with an end-of-pipe
  interrupt, which a gen1 context never delivers. The flip is done from the
  CPU instead, once the GPU has written the frame's label, and Hmd2's wait for
  the GPU is replaced by polling that label.
- **The game's own compute queues.** Once AgcDriver is in the process, the
  PS4 driver can't map a new compute queue any more (`sceGnmMapComputeQueue`
  fails on every pipe; queues mapped before keep working). The tutorial
  videos need one: the game plays them with AvPlayer's Videodec2 decoder,
  which maps its GPU queue when a video starts, so they stayed blue. Two of
  those decoder queues are made before the PS5 libraries load (pipe 2) and
  lent to AvPlayer when it asks for one.

## 6. Tracking and timing

**Head.** VrTracker2 gives the headset pose in its own room space. The agent
converts it to PS VR's tracking space (metres, +Y up, the player seated 1.5 m
in front of where the camera would be, facing it), rotating only around the
vertical axis so that "forward" is where you faced when the game started. The
tracker's first answers are default poses, so tracking only counts as live
once the pose changes on 8 samples in a row. The seat is set again when the
tracker relocalises (its pose jumps further than a head can move in 100 ms),
2 s after the headset's attachment sensor says it was put on, on the first
press of X, when OPTIONS is held for a second, and when the game itself
recalibrates the headset. Each time, the game is sent the ResetVrPosition
system event a console sends when the player resets the view, so it moves
its own origin to the head too (the game only reads events while
`sceSystemServiceGetStatus` counts some, so the status counts it).

The head's speed goes with every pose, worked out from one pose to the next
(AstroQuest does the same): the game only counts a head pushing into
something as a headbutt when it comes at some speed. A frame the game drew
just before the seat changed was drawn from the old seat, so for 250 ms
after a change the render pose is mapped back with whichever seat puts it
nearer the head.

With L1 held, a flick of the right stick turns the view 30 degrees: the seat
turns about the head, so to the game the player has turned where they stand
(AstroQuest's TurnView). The game isn't told.

That matters for the game's calibration screens, which a PS VR player sees
in front of the camera. "Fit roughly inside the silhouette" wants the head
within 0.2 m sideways of the camera and at least 0.5 m from it, for 1 s
(calibration.xml SilhouetteAreaWidth, SilhouetteAreaMinZ,
SilhouetteDetectionTime). "Put the controller inside the floating shape"
wants it within 8 cm of a point 17 cm below and 50 cm in front of the game's
origin (Position, PadConnectionRange). A seat taken with the headset lying
on a table failed the first; a game origin left from an older seat failed
the second.

**Controller.** The game registers its DualShock 4 with the PS VR tracker.
Its orientation comes from the pad's own fused orientation (the game turns
the pad's tilt correction on, so only the heading can drift). Holding
OPTIONS makes wherever it points straight ahead; any other seat change keeps
it pointing where it points in the room. Its position is held 17 cm below
and 50 cm in front of the head, following the head's position but not its
turn, and gliding there over 0.3 s rather than jumping (AstroQuest's rule);
a new seat puts it there at once. With L1 held the d-pad, circle and square
move that spot and triangle switches back to the standard one. After
`sceVrTrackerRecalibrate` a device reports "calibrating" for 200 ms, as a
console's tracker does; the game from 1.01 on waits for that on its
silhouette screen.

**Headset off.** PS VR2's attachment sensor (Hmd2 device information +20)
is read every half second. While the headset is off, the game's system
status says the system UI is over it (`isSystemUiOverlaid`), which is what
the game pauses for: it hands over a black picture until the headset is
back on. `sceHmdGetDeviceInformation` reports the same in `hmuMount`.

**Eye distance.** The game asks `sceHmdGet2DEyeOffset` once, before Hmd2 is
up, so the lens separation Hmd2 reports (+21, in mm) is kept in
`/data/summerlessonvr2/ipd` and given to the game from the next start on (63 mm
until then).

**Frames.** Each frame from the game is handed to Hmd2 as a type 0x10 layer:
its two eye textures (the game renders into three fixed pairs, registered
once as the VR port's buffers), the pose it was rendered for, its field of
view and the part of the texture it covers (all of it: the game draws its
scene at a size that follows the GPU load, then copies it into the 1440x1536
eye pictures). Hmd2 reprojects it to the latest head pose: at 120 and 90 fps
each frame once, at the game's own 60 (`framerate.txt` saying `60`) each
frame twice.

**Pacing.** On PS VR the game timed itself on 120 Hz vblank events from its
main video output, which the PS VR reprojection flipped. On a PS5 that
output is the 60 Hz TV, which held the game to 30 fps. The agent replaces
that vblank event with its own 120 Hz event and reports matching flip status,
with each frame shown for two refreshes, so the game runs at 60 fps. Once
Hmd2 is up the event follows the headset's own refreshes (each flip Hmd2
makes), so the game runs at exactly half the headset's 119.88 Hz. A clock
of its own at 120 Hz drifted from the headset, which dropped a frame every
several seconds.

**90 fps** (Astro Bot only). The game goes on to its next frame after two of those events. At
90 fps (`framerate.txt` says `90`; `60` for the above) Hmd2 is put in its
90 Hz mode (`sceHmd2ReprojectionEnableVrMode` takes two modes, `0x2000f` for
120 Hz and `0x2000c` for 90), its render config at full rate, and the event
is raised twice each refresh, the second half way, so the game draws a frame
for every refresh. Its time, counted in sixtieths, would then run half as
fast again: each frame the agent writes the time its frames take where the
game keeps its frame rate, time step and step in microseconds (`0x16688a8`,
`0x16688b0`, `0x16688b8` from its image base), exactly one refresh where frames
come steadily, as AstroQuest's PC version does. The payload also patches the
game's physics step (its bodies are stepped with the time step they were sent
with) and the soccer robots' frame counting (in the console's sixtieths), both
as AstroQuest found them. If Hmd2 refuses the 90 Hz mode and takes the 120 Hz
one, the agent goes back to 60 fps and the game's own sixtieth.

**120 fps (Astro Bot's default; not in this port).** At a frame for every refresh of the headset's own
120 Hz, three things the game does for PS VR stood in the way. Each was found
by measuring on the console (a timeline of every game thread's waits, the
GPU's work per frame, which frame each refresh showed) rather than guessed,
and each is changed in `hooks.c`:

- *The frame timer.* The game's hand-over thread (`Hmd::ReproThread`) re-arms a
  3 ms timer on its VR event queue at every wake, and its draw thread submits a
  frame's GPU work on the second fire after the one before: the first fire
  begins the frame on the main thread, the second submits it. On PS VR that
  gave two fires per 16.7 ms frame; at 120 Hz it threw away 3 ms of every
  8.34 before the GPU started, and the game's GPU work takes 6-7 ms in busy
  scenes. The agent hooks the timer call (`sceKernelAddHRTimerEvent`) and gives
  the delay itself: 300 us until the first fire after a submit, and for the
  arm after that first fire the delay that has the frame done 1.5 ms before
  the first refresh it can make, from the longest GPU time of its last 16 frames
  (first command buffer to GPU-done event) and the time the frame in flight has
  taken so far; about five arms a frame in all. Busy scenes start at once;
  light scenes start late enough that no frame is done early, which would be
  two for one refresh and one never shown. The same busy spot went from 80-107
  fps with 250-400 repeated refreshes every 10 s to 119-120 fps with 3-10, at
  the same GPU and CPU load; with the same four minutes of play replayed on the
  console, 0 frames shown twice or never over 21,580 refreshes in a busy stage.
  `frametimer.txt` saying `0` keeps the game's own timer.
- *The wakes.* The game hands a finished frame over only when woken by a
  refresh event, and needs two wakes after a refresh to do it (the first sees
  the frame shown). It gets one wake right after each flip, in its next wait,
  and one when its own GPU-done event (its graphics EOP, on the same queue)
  arrives, so a frame is handed over within microseconds of being drawn. No
  other events: a grid of them re-armed the timer.
- *The count.* The game paces on the refresh count in the flip status. Only
  the flip's own event advances the count, time and timestamp; the two wakes
  above are events alone. The vblank thread itself is woken by Hmd2's flip
  (`gen1.c`) instead of polling, so the flip's event follows it within
  microseconds, and the hand-off thread (`vr2.c`) is woken by the game's
  handover, so a frame reaches Hmd2 within 0.1 ms, and passes frames in order,
  one a refresh: Hmd2 builds each refresh
  just after the flip before, from the newest frame passed by then, so a frame
  passed too late for a refresh is held back until that build and the frame
  after next is aimed a refresh later, which has frames passed before their
  flips again (without that, every frame after the first late one was held, a
  refresh of latency for good). The TV mirror flips at the TV's own 60 a second. The time step snaps to a
  whole number of refreshes (up to three) within 10% while the game's time
  debt is small, else follows the frames.

Every 10 s the log says how many frames the game handed over and how many
Hmd2 showed, showed again or never showed; the aim is 1199, 0, 0.

## 7. The TV

With PS VR, the reprojection drew the headset picture into the game's main
video port (buffers 2 and 3) and flipped it, and the processor unit put it
on the TV, unless the game opened the social screen's "separate mode" and
sent the TV a picture of its own on bus 5. With PS VR2 in VR mode the
console drives two outputs, the headset over USB-C and the TV over HDMI, and
for a PS4 title the HDMI side shows its main port. Only the main port (0x0)
and Hmd2's own port (0x200) are display targets in the kernel log
(`dce-ft ... opened`); bus 5 isn't.

So the agent puts the headset view on the main port itself
(`agent/mirror.c`). It registers three linear 1920x1080 display buffers of
its own there (indexes 4 to 6, next to the game's four), and for the newest
finished frame at each TV refresh it runs a small GCN compute shader (`agent/mirror.s`,
assembled by `tools/gcn_asm.py`) on Hmd2's GPU queue. The shader samples an
80-degree-wide crop around the centre of the left eye picture, through the
game's own descriptor with the sRGB conversion turned off (the display
buffer is sRGB-encoded too), and writes the pixels; a label shows when it's
done, and the buffer is flipped with the frame's flip argument, as the PS VR
reprojection did.

## 8. Sound

The game opens its main audio port (7.1), an AUX port (type 127, the mix PS
VR sent to the TV for whoever watches), a music port and the controller's
speaker, and records from the microphone for blowing at things. On a PS5 the
main port goes to headphones only (the PS VR2's earphones), the AUX port goes
to the TV, and the music port goes to both. PS VR2 games leave the TV quiet,
so the agent does the same: the AUX port plays silence, and the music port is
opened as a second main port, which only the headphones get. (AstroQuest
mutes the AUX port too.)

## 9. Hand tracking

PS VR2 hand tracking is `libSceVrHand`. It's a PS5-only library like Hmd2:
the payload builds a fake-signed copy on first run (on 12.00, 12.02, 12.40, 13.00, 13.20, 13.40, 13.42 and 13.60), and
the agent loads it in the game with the other four as PS VR2 starts (`agent/hand.c`).

It is started later, once a session: when the headset picture has run for two
seconds and the headset has then stayed on your head for 5 seconds with no system
screen up. A system screen shows as the game's status (`isSystemUiOverlaid` or
`isInBackgroundExecution`) or as its controller held by the system (libScePad's intercepted
bit). Watched through a fresh restart's safety notice and play-area setup, the controller was
held and the background flag set from the notice until 3 s after it finished; the overlay flag
came up only for the PS menu. Its start takes
the headset cameras. PS VR2's safety notice and play-area setup use the same
cameras, and Allison Snow's first two starts took the console down when the palm
tracker started during them. Once started it keeps running when the headset
comes off and goes back on, since the system starts no camera session of its own
then. A start that fails isn't tried again. With `agent.handwatch=1` the check runs all
session and logs each change of the system status, the controller and the system's events to
the game, each with the game's time, but hand tracking never starts. `tools/testrun.sh` writes
`<log>.ticks` beside it: where the agent's log and the kernel log had got to every 2 s, to line
those changes up with the kernel log's safety-notice and play-area lines.

It needs a few things a PS4 process doesn't have:

- **Its models.** It asks VrTracker2 for `handCenter_00` and `handPose_00`
  from `/system/priv/vr_hand_ro`, which the game's sandbox can't read. Its
  import of that call is answered by the agent, and the payload copies the
  system's files into the game's buffer.
- **The headset cameras.** Hmd2 opens `/dev/hmd2_image` and
  `/dev/hmd2_leddet` for it, which an app is refused. The payload lends the
  game its own capabilities while VrHand starts, then gives the game its own
  back.
- **App info.** VrHand checks the app's attribute3 for the hand tracking bit,
  through libkernel's sized app info call, which the agent answers with the
  bit set.
- **Its GPU work.** The networks are 27 RDNA compute shaders in the model
  files. Each one goes through the same translator as Hmd2's shader when
  VrHand creates it (`sceAgcCreateShader`), and its register list is pointed at
  the translation. 8 of them are wave32, which a PS4-mode GPU can't run: they
  run as wave64 with only the low 32 lanes on, each wave32 of a group in its
  own wave64, with the thread ids worked out at the start; their lane masks
  stay 32-bit, and the SGPR they use for spare scalars (VCC_HI) moves to an
  SGPR of its own. Buffer descriptors get a gfx7 dword 3 for each access and
  their own one back after. Command buffers get the same gen1 packet forms as
  Hmd2's.
  VrHand waits on a GPU event a gen1 queue never raises, so its end-of-buffer
  writes a fresh label value every submit and the wait polls it.

The translation was checked by running each of the 27 shaders twice from
memory captured in the game, once as VrHand's RDNA code and once as the
translation, with one set of instruction semantics: all 175 dispatches of a
frame come out the same. The detection network was also checked against
what the console's GPU wrote, one dispatch at a time.

VrHand runs at 60 Hz, the most it takes, and its result is looked at every
4 ms. When both palms are seen 5 to 32 cm apart, the controller sits between
them, 1.5 cm up and 3.5 cm ahead, and its heading follows the line between
your hands (AstroQuest's rule). Its place is smoothed as a One Euro filter:
35 ms while it is still or slow, down to 8 ms as it moves faster, so a move
trails less and a still controller doesn't jitter. When the palms are lost (out
of the cameras' view, or one of them unseen a moment at its edge), it stays
where they last had it for 2 seconds, or as long as it is held still, then
glides to its place before you; seen again, it glides back into your hands.
With one hand on the controller VrHand keeps losing track of it, so one hand
isn't used yet.

## 10. The picture (AstroBotVR2 only)

Two optional improvements in AstroBotVR2, each off unless its file is there. Neither is in this
port: the sharpening filter was removed for the release (it never ran here: the game hands its
frames over before its GPU has finished them), and there is no anisotropic filtering.

**Sharpening and FXAA** (AstroBotVR2's `agent/filter.c`). For every frame the game hands
over, one compute buffer goes on Hmd2's GPU queue, the one the TV mirror uses,
before the frame's flip: for each eye a copy of the eye picture into a picture
of the agent's own, then the filter from that copy back into the eye picture.
Hmd2's own work for the refreshes that show the frame is submitted after the
flip, so on the same queue it comes after the filter; the buffer ends by
holding the queue until its last stores are in memory. The shader is C
(AstroBotVR2's `tools/gpu/filter.c`), compiled for the PS4's GPU (gfx701) by
`tools/gcn_cc.py`, and tested on the host by `tools/gpu/test.c`. It is FXAA
3.11, in its console form or its PC quality form (which follows an edge up
to 12 pixels each way), then AMD's FidelityFX CAS, whose weights come from the
unfiltered pixels. The pictures are read and written as UNORM, their sRGB
values as they are. It costs about 0.6 ms of GPU time a frame.

**Anisotropic filtering** (AstroBotVR2's `aniso.c`). The game sets every texture's
sampler through one routine, its constant update engine's setSamplers. At the
entry point the payload replaces the routine's first 15 bytes (six pushes, a
`movsxd` and a `test`, checked against the stock build) with a jump to the
agent's `aniso_entry`, which copies the samplers to its stack with the
mipmapped bilinear ones made anisotropic and calls the routine with the copy,
so the game's own sampler state is never written. Samplers for shadow maps
(a depth compare), unnormalized coordinates or min/max filtering are left
alone, and so is every sampler's mipmap filter: blending mipmaps that the game
takes one at a time made the console go down while a level loaded.

## Where the time went

The hard part wasn't the PS VR emulation, which AstroQuest had already mapped
out. It was getting Hmd2 to output anything at all from inside a PS4 process.
The first frame needed the AV control patch, the five PS5-library fixes, and
the whole GPU translation layer, all working at once. Before that, the only
signs were a black headset, a queue that never drained, or a GPU fault.
