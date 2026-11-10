# Settings and controls

The settings menu in the headset has what a player would change; the files below in
`/data/summerlessonvr2`, read as the game starts, have the rest. `cvars.txt` is read
again every 2 s while the game runs: the game's `r.*` variables and `agent.gcwait`
change at once; the other `agent.` options at the next start.

**One game's own settings:** `cvars-hikari.txt`, `cvars-chisato.txt` and
`cvars-allison.txt` are read after `cvars.txt`, for that game only, and their lines win
(the same format). `resolution-hikari.txt` (and the others) is used instead of
`resolution.txt` for that game when it is there. Hikari 1.02 uses Hikari's files.

## The settings menu

Hold **L1 + R1** and click the **touchpad**: the menu opens in the headset, a little
below straight ahead, and the same closes it. While it is open the game gets no input:
**up/down** choose a line, **left/right** (or **cross**) change it, **circle** closes
it. The lines marked `*` take from the next start of the game; the others at once.

- **Frame rate\*:** 60, 90 or 120 (saved to `framerate.txt`, for every game).
- **Eye width\*:** the game's preset, or a width from 960 up: to 1600 with foveated
  rendering (DFR takes an asked width only up to there), to 2000 with the stock
  picture (2048 ran out of GPU memory). The preset is each game's own width per frame
  rate (the README's table), or `resolution.txt`'s.
- **Foveated rendering\*:** on (the default) or off, the stock picture
  (`agent.multiview`).
- **Hand tracking\*:** on (the default) or off (`agent.hands`). Never started or stopped
  mid-game: the next start.
- **Shadows:** low, medium or high (`r.ShadowQuality` 2, 3 or 5: the game's own is
  medium, DFR's high).
- **Reflections:** off (the default) or on (`r.SSR.Quality` 0 or 2).
- **Ambient occlusion:** off or on (`r.AmbientOcclusionLevels` 0 or 3, DFR's; the stock
  picture starts at the game's own 2, shown as "2").
- **Bloom:** off or on (`r.BloomQuality` 0 or the game's 1).
- **TV picture:** the left eye (the default), both eyes side by side, or black
  (`agent.mirrorboth`, `agent.tvblank`).
- **Reset view:** as holding OPTIONS (below).
- **Reset all:** every line back to its default and the menu's saved settings removed
  (the frame rate to 60).
- **Close.**

The choices are kept in `menu-hikari.txt` (`-chisato`, `-allison`; Hikari 1.02 shares
Hikari's), written by the menu alone within 2 s of a change: one `name=value` line for
each setting that differs from its default, in `cvars.txt`'s names (the eye width as
`agent.resolution`, this file's own), and no file when none does. It is read after `cvars.txt` and the game's own file, so the menu's choices
win over theirs; "default" is what those files (or the port) would give without it. A
line in `cvars.txt` is never rewritten. The debug build has the debug menu instead and
leaves the file be (it says so in its log).

## `cvars.txt`

One setting a line:

- `agent.multiview=0`: the stock picture instead of dynamic foveated rendering (DFR: the
  area you look at drawn at full sharpness, following your gaze; "inset mode" in
  [MULTIVIEW.md](MULTIVIEW.md)). DFR is `agent.multiview=600`, the default (the sharp
  area's rows; 540 at 90 fps, 480 at 120); keep it (a larger sharp area or wider eyes run
  out of GPU time in the heaviest scenes).
- `agent.hands=0`: hand tracking off (the controller then stays in front of you). The
  default, `60`, starts it once the headset has been on for 5 seconds with no system
  screen up and the system's own use of the headset cameras (the safety notice, a
  play-area setup, the tracking-lost screen, its bring-up as the game starts) is 12
  seconds behind ([HOW-IT-WORKS.md](HOW-IT-WORKS.md) 9); `30` runs it at 30 Hz.
- `agent.recentre=1`: the first press of circle also takes your seat. Off by default.
- `agent.tvblank=1`: the TV stays black. `agent.mirrorboth=1`: the TV shows both eyes
  side by side.
- `agent.gcwait=N` (0 to 3600, changes at once): the seconds between the engine's garbage
  collections, each a dropped frame of about 40 ms. Chisato and Allison default to 600
  (the game's own is about once a minute); `0` keeps the game's own. Hikari 1.00 keeps
  its own by default. Hikari 1.02 turns its timed collections off itself, and the agent
  leaves that alone (the line does nothing there).
- `agent.cap=N` (debug builds only): the engine's frame-rate cap, in thousandths (the
  default is the headset's own 59.94, 89.91 or 119.88 at each rate:
  [MULTIVIEW.md](MULTIVIEW.md) 29); a release build ignores the line.
- `name=value` sets one of the game's Unreal console variables, e.g.
  `r.ShadowQuality=3`; `name` alone logs its value. DFR sets shadows 5, AA quality 6
  (3 on Chisato, Allison and Hikari 1.02) and ambient occlusion 3
  (`r.AmbientOcclusionLevels`, the game's own is 2) itself; a line here overrides them,
  and the settings menu's choice overrides the line. The values set (DFR's, this file's,
  the menu's) are kept even when the game resets them at a scene change.

A line for an option this build doesn't have is logged as unknown and ignored.

## `resolution.txt`

The eye width: in the stock picture (`agent.multiview=0`) 960 to 2048; without it, 2000.
DFR uses it only when it is 1600 or less, at any frame rate; without it, each game's
own width ([README](../README.md), Features). The settings menu's "Eye width" comes
before both files. Keep the stock picture at 2000 or
less: at 2048 the game ran out of GPU memory at the start of day 2 (going to Hikari's
room), where 2000 played through three days with memory to spare; 2000 can still drop
the odd frame in the heaviest scenes.

## `framerate.txt`

`60` (the default, without the file), `90` or `120`. With DFR, 90 and 120 run the game's
own native 90 and 120 fps modes, with smaller eyes (the table in the README) and a sharp
area of 540 or 480 rows ([MULTIVIEW.md](MULTIVIEW.md) 28). The rate is fixed at the
game's start. The stock picture stays at 60 and says so in the log. The settings menu's
"Frame rate" line (the debug menu's "Frame rate, next start") writes the file for the
next start.

## Controls

On top of the game's own (every button still reaches the game, but for the menu's):

- **L1 + R1, then a click of the touchpad**: the settings menu (above). The click, and
  the touchpad while both shoulder buttons are held, stay with the menu; while it is
  open the game gets no input.
- **Hold OPTIONS for a second**: reset the view. Where you sit and where you look become
  the centre.
- **First press of circle**, with `agent.recentre=1`: takes your seat where you are then.

## Debug builds

A debug build ([BUILDING.md](BUILDING.md)) adds the tools used to develop the port, off
unless switched on:

- **The debug menu**, in the settings menu's place (opened and used the same way). It
  has the frame rate for the next start, DFR's settings (the
  sharp area on/off, gaze following, its density, the TAA shader, the eye's
  bloom/AO/reflections in it, a debug grid), the gaze smoothing, the game's graphics settings
  (shadows, reflections, AO, bloom, tonemapper, motion blur, depth of field, AA type and
  quality, occlusion culling, LOD, translucency lighting), the TV views, and a reset to
  the start values. Nothing of it is saved, and the settings menu's file is ignored.
- **L3+R3** (both sticks in) marks a moment in the log with the 10 seconds before and 5
  after, one line a second; the controller buzzes once.
- A test mode that replays a recorded session with nobody wearing the headset
  ([TESTING.md](TESTING.md)), and measuring options in `cvars.txt` (`agent.rtime`,
  `agent.tvhmd`, `agent.capture`, `agent.probe`, `agent.taadump`, `agent.gaze`, and
  DFR's tuning: `agent.insetpp`, `agent.insetppt`, `agent.insetsize`,
  `agent.taa`, `agent.insetgaze`, `agent.gazefilter` and the rest). What each does, and
  what was measured with it, is in [MULTIVIEW.md](MULTIVIEW.md) and
  [SUMMER-LESSON.md](SUMMER-LESSON.md). An `agent.elf` in `/data/summerlessonvr2`
  replaces the built-in agent (a debug payload only).
