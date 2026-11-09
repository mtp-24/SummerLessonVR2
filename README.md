# SummerLessonVR2

Play **Summer Lesson** (the PS4 PS VR games Hikari Miyamoto, Chisato Shinjo and
Allison Snow) on **PlayStation VR2**, on a jailbroken PS5.

The game itself is not modified. A payload hooks the running PS4 game, answers its
PS VR calls, and sends the frames it renders to PS VR2 through the PS5's own VR
libraries, with full head tracking. The game runs at its own 60 fps, each frame
reprojected by PS VR2 to two of its 120 Hz refreshes, as it ran on PS VR.

This is a port of [AstroBotVR2](https://theghostonline.dev/theghostonline/AstroBotVR2)
by theghostonline to another game, maintained separately: report problems with this
port here, not to AstroBotVR2 or Ghost VR.

> Homebrew. Not affiliated with or endorsed by Sony or Bandai Namco. You need your
> own copy of the game; nothing from it is included here.

> **Warning: experimental.** During development the console hard-hung many times
> (CE-108262-9), mostly as the game started or as head tracking came up, and once
> its storage was damaged (CE-100016-8) and it had to be reset to factory defaults.
> Back up your saves, and don't use it on a console you can't afford to reset.

## What it does

- **Inset mode** (the default, in all three games): the eyes 1440 wide, with two sharp 1200x600
  insets that follow your gaze (PS VR2's eye tracking), shown as a second PS VR2
  layer. The insets keep the eye's own look: bloom and ambient occlusion, the game's
  fades, its outline round a chosen item, and glass and other translucent objects lit
  as around them, and reflections when they are switched on. About 13 ms of GPU time a
  frame at 60 fps
  ([docs/MULTIVIEW.md](docs/MULTIVIEW.md)).
- **The stock picture** (`agent.multiview=0`):
  2000x2250 an eye by default (the game's own is 1248x1404), up to 2048.
- Head tracking (rotation and position) from PS VR2's own tracking, and the
  headset's real field of view.
- The controller's position from PS VR2 hand tracking, on firmware 12.00, 12.02,
  12.40, 13.00, 13.20, 13.40, 13.42 and 13.60.
- The TV shows the left eye's view, like PS VR did. Works with the PS5's video
  output at 1080p or 4K.
- No PS VR, PS Camera or setup dialog needed: the game sees a connected PS VR.

## Known limits

- **Version 1.00** of the Asian English releases (on the disc, or installed without
  updates), and Hikari Miyamoto's update **1.02**:

  | Game | Title id | |
  |---|---|---|
  | Hikari Miyamoto 1.00 | CUSA07434 | tested |
  | Hikari Miyamoto 1.02 | CUSA07434 | played, and recorded-session runs |
  | Chisato Shinjo | CUSA11682 | short sessions and recorded-session runs |
  | Allison Snow | CUSA11674 | one long session and recorded-session runs |

  Inset mode runs in all of them. Hikari 1.02, Chisato and Allison share a newer version
  of the game's engine than Hikari 1.00 ([docs/OTHER-GAMES.md](docs/OTHER-GAMES.md)).
- **Hikari 1.02:** a bloom flicker in the right eye was seen once on entering the café,
  until the AA quality was changed and back; not reproduced yet.
- Tested on firmware **12.40** only. The payload has profiles for 19 firmware
  versions (5.50 to 13.60, from AstroBotVR2 and Ghost VR), but no other has been
  tried with this game ([docs/FIRMWARE-11.60.md](docs/FIRMWARE-11.60.md)).
- **60 fps by default.** In inset mode, `framerate.txt` with `90` or `120` (in a debug build also the debug
  menu's "Frame rate, next start" line, from the next start) runs the game's own 90 or 120 fps with
  smaller eyes: 1200 wide at 90 and 960 at 120, against 1440 at 60.
  Measured on replays with no frame lost, not yet worn
  ([docs/MULTIVIEW.md](docs/MULTIVIEW.md) 28). It needs a payload from that change on; the
  stock game (`agent.multiview=0`) stays at 60.
- **Don't turn on PS VR2's see-through view (passthrough) while the game runs**: switched
  on as Allison loaded, it hung the console.
- **Long sessions:** two inset sessions (19 and 25 minutes) ended right after a save, the
  game out of CPU memory. Inset mode now gives the game's memory pool its full size; since
  then a 66-minute session has run with its free memory flat. Save before long breaks.
- If ps-patch-system's `patch-server` is running, the payload stays out of the game
  and says so (in Ghost VR it crashed the console). Stop it and start the game again.
- Don't send the payload again while the game is running: in testing, that hung the
  console.

## Requirements

- A PS5 on firmware 12.40, jailbroken, with kstuff and an ELF payload loader
  (Payload Manager, etaHEN or elfldr on port 9021)
- One of the games above, v1.00 (or Hikari Miyamoto at 1.02)
- PS VR2, connected and set up

## Install and play

1. Build `payload/summerlessonvr2.elf` (see Building) and send it to your payload
   loader with the game closed. It stays in the background and
   waits for the game.
2. The first time, it builds the PS VR2 libraries and its shader from your
   console's own firmware into `/data/summerlessonvr2` (a few seconds, once).
3. Put the headset on and start the game from the home screen. Hikari starts in
   inset mode; nothing to set.

Your seat (where the game thinks you sit, and which way is forward) is taken from
where your head is when the game starts.

A log of each session is written to `/data/summerlessonvr2/agent.log`: if something
doesn't work, look there first.

## Controls

On top of the game's own (every button still reaches the game):

- **Hold OPTIONS for a second**: reset the view. Where you sit and where you look
  become the centre.
- **First press of circle**, with `agent.recentre=1` in `cvars.txt`: takes your seat
  where you are then. Off by default.

## Settings (files in `/data/summerlessonvr2`)

Read as the game starts. `cvars.txt` is read again every 2 s while the game runs:
the game's `r.*` variables and `agent.predict` change at once; the other `agent.`
options at the next start.

**One game's own settings:** `cvars-hikari.txt`, `cvars-chisato.txt` and
`cvars-allison.txt` are read after `cvars.txt`, for that game only, and their lines win
(the same format). `resolution-hikari.txt` (and the others) is used instead of
`resolution.txt` for that game when it is there.

**`cvars.txt`**, one setting a line:

- `agent.multiview=0`: the stock picture instead of inset mode (Hikari). Inset mode
  is `agent.multiview=600`, the default; keep 600 (larger insets or eyes run out of
  GPU time in the heaviest scenes).
- `agent.hands=0`: hand tracking off (the controller then stays in front of you).
  The default, `60`, starts it once the headset has been on for 5 seconds with no
  system screen up; `30` runs it at 30 Hz.
- `agent.recentre=1`: the first press of circle also takes your seat.
- `agent.tvblank=1`: the TV stays black. `agent.mirrorboth=1`: the TV shows both eyes
  side by side.
- `agent.gcwait=N` (0 to 3600, changes at once): the seconds between the engine's garbage collections, each
  a dropped frame of about 40 ms. Chisato, Allison and Hikari 1.02 default to 600 (the game's
  own is about once a minute); `0` keeps the game's own. Hikari 1.00 keeps its own by default.
- `agent.predict=N` (1 to 60): the head's pose for each frame predicted N ms ahead
  instead of the game's own.
- `name=value` sets one of the game's Unreal console variables, e.g.
  `r.ShadowQuality=3`; `name` alone logs its value. Inset mode sets shadows 5, AA
  quality 6 (3 on Chisato, Allison and Hikari 1.02) and ambient occlusion 3 (`r.AmbientOcclusionLevels`, the game's own is 2)
  itself; a line here overrides them. The values set (inset mode's, this
  file's, the debug menu's) are kept even when the game resets them at a scene change.

**`resolution.txt`**, the stock picture's eye width (`agent.multiview=0`), 960 to
2048; without it, 2000; `stock` for the game's own 1248. Inset mode uses it only when
it is 1600 or less. Keep 2000 or less: at 2048 the game ran out of GPU memory at the
start of day 2 (going to Hikari's room), where 2000 played through three days with
memory to spare; 2000 can still drop the odd frame in the heaviest scenes.

## Building

Needs the [PS5 payload SDK](https://github.com/ps5-payload-dev/sdk), LLVM (clang and
ld.lld) and Python 3:

    export PS5_PAYLOAD_SDK=~/ps5sdk/opt/ps5-payload-sdk
    make            # a debug build (the tools below)
    make RELEASE=1  # the one to hand out

The output is `payload/summerlessonvr2.elf`; the in-game part (`agent/`) is built
first and embedded in it. A release build keeps only the player's settings above, a
short log, and none of the debug tools. The GPU shaders' headers (`agent/*_gcn.h`)
are checked in; rebuilding them needs LLVM's AMDGPU tools (Homebrew's LLVM has them).

- `agent/` runs inside the game: the PS VR emulation (`hooks.c`, `vr.c`), the PS VR2
  output and tracking (`vr2.c`), the GPU compatibility layer (`gen1.c`, `acb.c`), the
  PS5-library fixes (`ps5compat.c`), the TV picture (`mirror.c`), hand tracking
  (`hand.c`), inset mode (`multiview.c`, `inset.c`, `insetpp.c`), eye tracking
  (`fov.c`) and the debug menu (`menu.c`).
- `payload/` is the loader: it builds the libraries and the shader on first run,
  puts the agent into the game and applies inset mode's patches.
- `tools/` holds the test runner ([docs/TESTING.md](docs/TESTING.md)), the address
  finder for other builds of the game ([docs/OTHER-GAMES.md](docs/OTHER-GAMES.md))
  and the host tests.

### Debug builds

A debug build adds the tools used to develop the port, off unless switched on:

- **The debug menu**: hold **L1 + R1** and click the **touchpad**. While it is open
  the game gets no input: **up/down** choose a line, **left/right** (or **cross**)
  change it, **circle** closes it. It has inset mode's settings (insets on/off, gaze
  following, density, width, AA, the eye's bloom/AO, vignette, exposure, a debug
  grid), the gaze smoothing, the game's graphics settings (shadows, reflections, AO,
  bloom, tonemapper, motion blur, depth of field, AA type and quality, LOD,
  translucency lighting), prediction, the TV views and the brightness probe.
- **L3+R3** (both sticks in) marks a moment in the log with the 10 seconds before and
  5 after, one line a second; the controller buzzes once.
- A test mode that replays a recorded session with nobody wearing the headset
  ([docs/TESTING.md](docs/TESTING.md)), and measuring options in `cvars.txt`
  (`agent.fingerprint`, `agent.trace`, `agent.waits`, `agent.rtime`, `agent.tvhmd`,
  `agent.capture`, `agent.probe`, `agent.gaze`, and inset mode's tuning). An
  `agent.elf` in `/data/summerlessonvr2` replaces the built-in agent.

What each option does, and what was measured with it, is in the docs.

## How it works

The PS4 game runs in PS4 mode, with a PS4 GPU context, and the PS VR2 libraries are
PS5-only. The payload loads them into the game anyway, makes them believe they are in
a PS5 VR app, translates their GPU work into a form a PS4-mode context can run, and
sits between the game's PS VR calls and PS VR2's reprojection. PS VR2's buffers live
in a memory reserve beyond the game's own (after Ghost VR's).

- [docs/HOW-IT-WORKS.md](docs/HOW-IT-WORKS.md): the general mechanism (written for
  AstroBotVR2)
- [docs/SUMMER-LESSON.md](docs/SUMMER-LESSON.md): what is particular to Summer Lesson
  (its PS VR calls, frame loop, resolution and memory), with measurements
- [docs/MULTIVIEW.md](docs/MULTIVIEW.md): inset mode, its design and measurements
- [docs/FRAME-COPY.md](docs/FRAME-COPY.md), [docs/FOVEATION.md](docs/FOVEATION.md):
  the frame hand-off and the foveation study that led to inset mode

## Credits

- theghostonline, for [AstroBotVR2](https://theghostonline.dev/theghostonline/AstroBotVR2),
  which this is built on, and [PS5VR](https://theghostonline.dev/theghostonline/PS5VR),
  where the hand tracking comes from; and for Ghost VR, where the memory reserve, the
  exit and wait logging, the offline leaderboards and the `patch-server` check come
  from.
- [shadPS4](https://github.com/shadps4-emu/shadPS4), for the PS VR structure layouts.
- John Törnblom, for the [PS5 payload SDK](https://github.com/ps5-payload-dev/sdk),
  `pt.c` and the fake-SELF format.
- idlesauce, for [ps5-self-pager](https://github.com/idlesauce/ps5-self-pager).

## Licence

GPL-3.0-or-later. See [LICENSE](LICENSE) and [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).
