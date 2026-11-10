# How SummerLessonVR2 works

> The mechanism. What is particular to Summer Lesson (its PS VR calls, frame loop, resolution
> and memory, with the measurements) is in [SUMMER-LESSON.md](SUMMER-LESSON.md); inset mode, the
> dynamic foveated rendering the README calls DFR, is in [MULTIVIEW.md](MULTIVIEW.md).

Summer Lesson is a PS4 game. On a PS5 it runs in backwards
compatibility mode: PS4 system libraries, a PS4 process, and a PS4-mode GPU
context. PS VR2 is driven by PS5-only libraries (libSceHmd2 for the headset
and its reprojection, libSceVrTracker2 for tracking, libSceAgc and
libSceAgcDriver for the GPU). None of them were made to run in a PS4 process.

SummerLessonVR2 has two parts:

- **the payload** (`payload/`), a normal PS5 payload that stays resident. It
  prepares the PS5 libraries once, puts the agent into the game when it
  starts, and does the things only a kernel-level process can do (mounting
  files into the game's sandbox, re-pointing import slots, patching the AV
  control service while the game runs).
- **the agent** (`agent/`), freestanding code that runs inside the game. It
  answers the game's PS VR calls and drives PS VR2.

The firmware offsets below are 12.40's, the firmware this port runs on. Each
supported firmware's own values are in `common/firmware.h`; what differs between
them is in [FIRMWARE.md](FIRMWARE.md). The game's own addresses are
Hikari Miyamoto 1.00's; the other builds' are in [OTHER-GAMES.md](OTHER-GAMES.md).

## 1. First run: preparing the PS VR2 libraries

The payload builds four files in `/data/summerlessonvr2/libs` from the console's
own firmware, and a fifth (libSceVrHand, section 9) where the firmware's profile has one
(`payload/prepare.c`):

1. Each library is read from `/system/common/lib` and decrypted with
   idlesauce's self pager.
2. The ELF ABI version is changed from 2 to 3. PS5-only libraries are refused
   in a PS4 process ("ABIVERSION mismatch") otherwise.
3. In libSceHmd2, the PLT stub for `sceKernelGetProsperoCompiledSdkVersion`
   is replaced with code that returns SDK 8.05. A PS4 process has no PS5 SDK
   version, and Hmd2 checks it before anything else.
4. In libSceAgcDriver, the address of its 2 MB command-buffer area moves from
   0xfe0000000, where the game already has a reservation, to 0xfe0400000.
5. The result is wrapped as a fake-signed SELF (the SDK's `make_fself.py`
   format, written by `fself.c`), which kstuff lets the game load.

Each decrypted library is checked against the SHA-256 of its build for the
running firmware before it is changed, so an unknown firmware fails cleanly
instead of producing a broken file. The prepared files are tagged with the
firmware they were built for, and rebuilt after a firmware change.

The same step translates Hmd2's reprojection shader (see section 5) and
writes it to `/data/summerlessonvr2/hmd2_gcn.bin`.

## 2. Getting into the game

The payload watches for the `eboot.bin` of one of the games (`payload/main.c`
`k_builds`: the title id and the version its `param.sfo` gives, confirmed by the
game's code, which must hold that build's two frame-sequence stores and its
viewport reference at 0x1be91; an update that keeps its version is told apart the
same way: [OTHER-GAMES.md](OTHER-GAMES.md), Hikari Miyamoto 1.02). Then it:

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

Any other copy of the game (another region, or a version whose code is none of
the known builds') gets a notification saying which one it is, instead of the
payload waiting in silence. So do the other ways it can't start: the wrong
firmware, or the libraries not loading.

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
- The game's main video output asks for one of PS VR's output modes (119.88 Hz, or
  90 Hz in its mode 2). That is only pretended: with no PS VR attached, the real call makes the system suspend
  the game.

## 4. The PS VR2 libraries in a PS4 process

Once the game is rendering VR frames, the agent starts a thread that loads
the four libraries. The game's sandbox can't see `/data`, so the payload
mounts `/data/summerlessonvr2/libs` read-only into the sandbox (nullfs) while the
agent loads them, and unmounts it after. (Copying each library into the sandbox
and deleting it after loading, as early versions did, writes into a
tmpfs every sandbox shares, and loading from it could panic the kernel.)

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
  PS4 title in two places: the layout handler (a branch at 0x41735b) and
  `checkVideoModeForChangeOutput` (0x431eb8). The payload patches both in the
  service's memory while the game runs, and puts the original bytes back when
  the game closes or the payload exits. On every firmware but 13.60 the
  service's own record of the app is changed to a PS VR2 title's too
  ([FIRMWARE.md](FIRMWARE.md)).

## 5. The GPU: running PS5 GPU work in a PS4 context

This is most of the work (`agent/gen1.c`). The game's GPU context is
"gen1", meaning PS4 mode: it runs GCN shader code and reads GCN resource
descriptors, and some RDNA registers are privileged in it. Hmd2 builds its
reprojection as an RDNA compute dispatch and submits it through AgcDriver.
Unchanged, the GPU hangs. Every command buffer Hmd2 submits is rewritten
before it reaches the GPU:

- **The shader.** Hmd2's reprojection shader (3526 dwords of RDNA code on
  12.40) is translated to GCN (3878 dwords) by `payload/rdna2gcn.c`; each
  firmware's counts are in its profile. Most instructions
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
  fails on every pipe; queues mapped before keep working). A video the game
  plays needs one: AvPlayer's Videodec2 decoder maps its GPU queue when a video
  starts, and without one the picture stays blue. Two of those decoder queues
  are made before the PS5 libraries load (pipe 2) and lent to AvPlayer when it
  asks for one. The game's own compute queue is on pipe 5, and hand tracking's
  (section 9) on pipe 6.

## 6. Tracking and timing

**Head.** VrTracker2 gives the headset pose in its own room space. The agent
converts it to PS VR's tracking space (metres, +Y up, the player seated 1.5 m
in front of where the camera would be, facing it; `agent/vr.c`), rotating only
around the vertical axis so that "forward" is where you faced when the game
started. The tracker's first answers are default poses, so tracking only counts
as live once the pose changes on 8 samples in a row. The seat is set again when
the tracker relocalises (its pose jumps further than a head can move in 100 ms),
2 s after the headset's attachment sensor says it was put on, when OPTIONS is
held for a second, and, with `agent.recentre=1`, on the first press of circle.
Each time, the game is sent the ResetVrPosition system event a console sends
when the player resets the view, so it moves its own origin to the head too (the
game only reads events while `sceSystemServiceGetStatus` counts some, so the
status counts it).

The head's speed goes with every pose, worked out from one pose to the next. A frame the game drew just before the seat changed
was drawn from the old seat, so for 250 ms after a change the render pose is
mapped back with whichever seat puts it nearer the head.

**Controller.** The game registers its DualShock 4 with the PS VR tracker. Its
orientation comes from the pad's own fused orientation, which the pad only
computes with its motion sensors on: the game never switches them on (it imports
neither call), so the agent does, with the tilt correction, the first time the game
reads the pad; only the heading can then drift. Holding OPTIONS makes wherever it
points straight ahead; any other seat change keeps it pointing where it points
in the room. Its position is held 17 cm below and 50 cm in front of the head,
following the head's position but not its turn, and gliding there over 0.3 s
rather than jumping; a new seat puts it there at once. With
hand tracking (section 9) it sits between your palms instead. After
`sceVrTrackerRecalibrate` a device reports "calibrating" for 200 ms, as a
console's tracker does, so a game waiting for that sees it.

**Headset off.** PS VR2's attachment sensor (Hmd2 device information +20) is
read every half second. While the headset is off, the game's system status
says the system UI is over it (`isSystemUiOverlaid`, byte 4), which is what the
game pauses for: it hands over a black picture until the headset is back on.
`sceHmdGetDeviceInformation` reports the same in its mount flag.

**Eye distance.** The game asks `sceHmdGet2DEyeOffset` once, before Hmd2 is
up, so the lens separation Hmd2 reports (+21, in mm) is kept in
`/data/summerlessonvr2/ipd` and given to the game from the next start on (63 mm
until then).

**Frames.** Each frame from the game is handed to Hmd2 as a type 0x10 layer. The
game hands over (`sceHmdReprojectionStart`) one side-by-side texture, both eyes in
it, from one of two fixed pairs it draws in turn (registered once as the VR port's
buffers, with the game's own GCN descriptors; a texture over 3840 wide is registered
at one eye's width: [SUMMER-LESSON.md](SUMMER-LESSON.md)), with the pose it was
rendered for and its uv mapping (scale 0.5 / (out + in), the left eye from u = 0, the
right from u = 0.5). The layer gets each eye's viewport where its picture is, its
field of view and the pose. The game's flat screens (its title and loading screens,
`sceHmdReprojectionStart2dVr`) count as frames too, shown as a 16:9 screen fixed
before the seat, about 77 degrees wide, the same picture to both eyes. Hmd2
reprojects each frame to the latest head pose: at 60 fps each frame twice, at the
native 90 and 120 (inset mode, [MULTIVIEW.md](MULTIVIEW.md) 28) once. In inset mode
the sharp insets go as a second layer with Hmd2's window test (MULTIVIEW.md 19).

**Pacing.** On PS VR the game timed itself on the 120 Hz vblank events of its
main video output (two a frame, on a queue of its own, `TrackerVblankQueue`),
which the PS VR reprojection flipped. On a PS5 that output is the 60 Hz TV, which
would hold the game to 30 fps. The agent replaces the first vblank event the game
registers with its own 120 Hz event, packed as libSceVideoOut packs it, and
reports a matching flip status (the real port's pending flips and current buffer,
only the pace faked), so the game runs at 60 fps. Once Hmd2 is up the event
follows the headset's own refreshes (each flip Hmd2 makes, `gen1.c` waking the
vblank thread), so the game runs at exactly half the headset's 119.88 Hz; a clock
of its own at 120 Hz drifted from the headset, which dropped a frame every several
seconds. The engine's own frame-rate cap is set to the headset's rate (59.94,
89.91 or 119.88; MULTIVIEW.md 29), else it made one frame more than the headset
took every ~1100. At 90 and 120 the game's own native frame sequence is switched
on by inset mode's patch table and the event is raised every refresh (MULTIVIEW.md
28; the hand-off then passes the newest frame, [FRAME-COPY.md](FRAME-COPY.md)).

Every 10 s the log says how many frames the game handed over and how many
Hmd2 showed, showed again or never showed; at 60 the aim is 600, 600, 599, 0.

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
(`agent/mirror.c`). It registers three linear display buffers of its own there,
of the main port's size (1920x1080 usually; 640x360 to 3840x2160 accepted), indexes 4 to 6 next
to the game's four, and for each new frame the game hands over, once it is drawn (at the 120
pace every other one, as the TV refreshes at 60), it runs a small GCN compute shader (`agent/mirror.s`,
assembled by `tools/gcn_asm.py`) on Hmd2's GPU queue. The shader samples an
80-degree-wide crop around the centre of the left eye picture, through the
game's own descriptor with the sRGB conversion turned off (the display
buffer is sRGB-encoded too), and writes the pixels; a label shows when it's
done, and the buffer is flipped with the frame's flip argument, as the PS VR
reprojection did.

## 8. Sound

The game opens its main audio port, an AUX port (type 127, the mix PS VR sent
to the TV for whoever watches) and a BGM port (type 1). On a PS5 the main port
goes to headphones only (the PS VR2's earphones), the AUX port goes to the TV,
and the BGM port goes to both. PS VR2 games leave the TV quiet, so the agent
does the same: the AUX port plays silence, and the BGM port is opened as a
second main port, which only the headphones get. (Other PS VR ports mute the AUX
port too.)

## 9. Hand tracking

PS VR2 hand tracking is `libSceVrHand`. It's a PS5-only library like Hmd2:
the payload builds a fake-signed copy on first run (on 12.00, 12.02, 12.40, 12.60, 12.70, 13.00, 13.20, 13.40, 13.42 and 13.60), and
the agent loads it in the game with the other four as PS VR2 starts (`agent/hand.c`).

It is started later, once a session: when the headset picture has run for 240
refreshes (two seconds at 120 Hz), the headset has then stayed on your head for 5 seconds with no system
screen up, and, at the same moment, the headset cameras' gate below is clear (the
system's own camera work over for 12 s). A system screen shows as the game's status (`isSystemUiOverlaid` or
`isInBackgroundExecution`) or as its controller held by the system (libScePad's intercepted
bit). Watched through a fresh restart's safety notice and play-area setup, the controller was
held and the background flag set from the notice until 3 s after it finished; the overlay flag
came up only for the PS menu. Its start takes
the headset cameras. PS VR2's safety notice and play-area setup use the same
cameras, and Allison Snow's first two starts took the console down when the palm
tracker started during them. Once started it keeps running when the headset
comes off and goes back on, since the system starts no camera session of its own
then. A start that fails isn't tried again; one the gate turns back waits again, and
if the gate never clears there is no start (the controller stays at its place). (The rule was worked out with a watch-only mode,
`agent.handwatch=1`, since removed: the check ran all session and logged each change of the
system status, the controller and the system's events, but hand tracking never started.)
`tools/testrun.sh` writes
`<log>.ticks` beside it: where the agent's log and the kernel log had got to every 2 s, to line
those changes up with the kernel log's safety-notice and play-area lines.

That rule reads the game's own flags, which say nothing about who has the cameras. The hangs
that followed (Chisato started from the home screen with the headset worn, the Hikari-to-Chisato
switch) were starts about 10 and 7 s after the system's own post-mount camera bring-up (the
kernel log's `Don't need show SafetyNotice`, `VrCamera StateChanged: Stop`), with no system
screen up; every surviving start (206 with that bring-up in their kernel log) had opened the
cameras 10.3 s or more after it, and a running tracker survived everything the system did
afterwards. So the payload polls the kernel message buffer (sysctl `kern.msgbuf`, every 200 ms,
`payload/msgbuf.h`: it consumes nothing, where `/dev/klog` hands each line to one reader only
and a user's klogsrv may be reading it), finding each poll's new text after its own marker line
(`SummerLessonVR2 mark <n> <nonce>`, written into the kernel log once a second the whole time a
game is served, a new nonce a game: unique, so never in doubt where a content anchor could match
a repeated line and skip some; a marker lost from the buffer shuts the gate 15 s and the reading
goes on after the newest marker left, a newest marker not seen for 3 s shuts it; at a new game
and after any such loss the system's state counts as unknown until its own lines say, never by
time: the bring-up the system logs at every game's VR start says it all, in every log so far
before the 12 s floor ends, so this adds no wait; a loss mid-game, which no log comes near with
the 128 KB buffer (the worst 200 ms of kernel log in play is 41 KB, 98 KB at a boot), leaves the
start waiting until the system's lines say again, and after 20 s the player is told to put the
headset on again or start the game again; a payload sent over a running one takes the levels
from the buffer's history once. Two residuals are accepted, both needing such a loss: a camera
job's idle line after it may count the stream as nobody's while the system's open line was among
the lost, and the "system screen focused" flag from the focus lines is cleared by a loss while a
screen is up; the game's own status, which the start rule reads, stays the authority on the
screens, and the 12 s floor runs from every system line either way), and
parses the new lines into the headset cameras' state
(`common/camera.h`: who has the image stream and whether the system's camera job runs, the two
things no API tells, as levels; the notice, a play-area setup and the system's screens as
events only, since the game's own system status says whether one is up; the tracker
relocalising, the headset put on, and when each last happened), written into the agent's shared
block every 100 ms; the agent's gate (`hand.c gate_eval`) is clear only when the buffer is being
read and carries the system's lines, no lines were possibly missed in the last 15 s, the stream
has no system owner, the camera job is idle, no system VR screen has the focus (the focus
lines), the game's own system status is readable and not out of the play area, the headset was
not put on in the last 5 s (the
tracking-lost screen's camera job came 1 s after that in 15 of 31), the tracker has not been
relocalising for 3 s (the lines seen 0-3 s before a tracking-lost camera job) and has either logged
"tracking stable" since its last SLAM reset or that reset is 10 s old, and the last line of any system camera or screen kind is
12 s old. The start comes at the first moment the rule above
and the gate hold together (the rule's 5 s go on while the gate holds, unless its own condition
breaks), the gate is asked once more right before the game's credentials are raised, from a
fresh word of the kernel log (a block the payload wrote after the check began, with a snapshot
taken no more than 0.5 s before the block was written; none within a second, or not clear: no credentials, waiting again, the
rule's settle kept), eye tracking's start and the palm tracker's take turns (one lock), and the
borrowed credentials stay until the camera stream is up (the kernel announcing the game's stream,
`hmd2_image: pid <game>, pri -1 ... mode 4`, seen through the block; VrHand's camera manager or
image lock speaking; or a hand tracked; 3 s at most), since the stream is configured on VrHand's
own thread after the start call returns. A gate that never
clears never starts the tracker; its reason is logged every 30 s, and after 60 s eye tracking
(inset mode) no longer waits for it. The log carries the gate's verdicts (`hand-gate:` lines),
its verdict at the start and right before the credentials, and what it added to the rule's
wait; with `agent.hands=0` the rule's timing and the gate still run, watch only, without a
start. The gate was watched only first (three console runs: the rule alone would have started
5.5 s after the system released the cameras in every start-up setup, the gate held until 12 s
and went not clear 0.7 s before each setup's camera job). `tools/hand/gate_check.py` replays
the gate over the old kernel logs (its constants are options, which is how they were chosen):
every known hang held, the 212 surviving starts 1.5 s later in the median, 3.6 s at p90,
11.4 s at most.

It needs a few things a PS4 process doesn't have:

- **Its models.** It asks VrTracker2 for `handCenter_00` and `handPose_00`/`_02`
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
your hands. Its place is smoothed as a One Euro filter:
35 ms while it is still or slow, down to 8 ms as it moves faster, so a move
trails less and a still controller doesn't jitter. When the palms are lost (out
of the cameras' view, or one of them unseen a moment at its edge), it stays
where they last had it for 2 seconds, or as long as it is held still, then
glides to its place before you; seen again, it glides back into your hands.
With one hand on the controller VrHand keeps losing track of it, so one hand
isn't used yet.

## Where the time went

The hard part wasn't the PS VR emulation, which earlier ports had already mapped
out. It was getting Hmd2 to output anything at all from inside a PS4 process.
The first frame needed the AV control patch, the five PS5-library fixes, and
the whole GPU translation layer, all working at once. Before that, the only
signs were a black headset, a queue that never drained, or a GPU fault. For
this game the next hard parts were its memory (the libraries' command-buffer
address, the game's pools) and the frame loop's pace: SUMMER-LESSON.md.
