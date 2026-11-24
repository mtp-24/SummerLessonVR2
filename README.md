# SummerLessonVR2

Play **Summer Lesson** (the PS4 PS VR games Hikari Miyamoto, Chisato Shinjo and
Allison Snow) on **PlayStation VR2**, on a jailbroken PS5. The game itself is not
modified: a payload hooks the running PS4 game, answers its PS VR calls, and sends the
frames it renders to PS VR2 through the PS5's own VR libraries, with full head tracking.
The Asian English releases at these versions: Hikari Miyamoto CUSA07434 01.00 and its
01.02 update, Chisato Shinjo CUSA11682 01.02, Allison Snow CUSA11674 01.01
([docs/LIMITS.md](docs/LIMITS.md)).

> Homebrew. Not affiliated with or endorsed by Sony or Bandai Namco. You need your
> own copy of the game; nothing from it is included here.

> **Warning: experimental.** During development the console hard-hung many times
> (CE-108262-9), and once its storage was damaged (CE-100016-8) and it had to be reset
> to factory defaults. Back up your saves, and don't use it on a console you can't
> afford to reset. The known limits are in [docs/LIMITS.md](docs/LIMITS.md).

## Features

- **Dynamic foveated rendering (DFR)** with PS VR2's eye tracking, the default: the area
  you look at is drawn at the headset's full sharpness and follows your gaze, the rest at
  a lower resolution. The sharp area keeps the eye's own look: bloom, ambient occlusion,
  reflections, the game's fades, its outline round a chosen item, glass lit as around it
  ([docs/MULTIVIEW.md](docs/MULTIVIEW.md), where it is "inset mode"). The width each eye
  is drawn at, per game and frame rate:

  | Game | 60 fps | 90 fps | 120 fps |
  |---|---|---|---|
  | Hikari Miyamoto 1.00 | 1440 | 1200 | 960 |
  | Hikari Miyamoto 1.02 | 1520 | 1320 | 1080 |
  | Chisato Shinjo | 1440 | 1320 | 1080 |
  | Allison Snow | 1600 | 1320 | 1080 |

- **60, 90 or 120 fps.** 60 by default (the game's own rate, each frame reprojected to
  two of PS VR2's 120 Hz refreshes, as on PS VR); `framerate.txt` with `90` or `120`
  runs the game's own native 90 or 120 fps modes with smaller eyes, with DFR.
- **The stock picture** (`agent.multiview=0`, DFR off): 2000x2250 an eye by default (the
  game's own is 1248x1404), at 60 fps.
- **Head tracking** (rotation and position) from PS VR2's own tracking, and the
  headset's real field of view.
- **Hand tracking**: the controller's position from PS VR2's hand tracking, on firmware
  12.00, 12.02, 12.40, 12.60, 12.70, 13.00, 13.20, 13.40, 13.42 and 13.60. Its start waits for the
  headset cameras to be free of the system's own use (the safety notice, a play-area
  setup, a game switch), read from the kernel log, so a game started with the headset
  on no longer takes the console down ([docs/HOW-IT-WORKS.md](docs/HOW-IT-WORKS.md) 9).
- **The TV** shows the left eye's view, like PS VR did (or both eyes, or black), at the
  PS5's 1080p or 4K output.
- **A settings menu in the headset** (L1 + R1, then the touchpad): the frame rate, eye
  width, foveated rendering and hand tracking for the next start; shadows, reflections,
  ambient occlusion, bloom and the TV picture at once; a view reset. Saved per game
  ([docs/SETTINGS.md](docs/SETTINGS.md)).
- **No PS VR, PS Camera or setup dialog needed**: the game sees a connected PS VR.
- A **release build** for playing and a **debug build** with a debug menu in the
  settings menu's place, a test mode that replays a recorded session, and measuring
  options ([docs/BUILDING.md](docs/BUILDING.md), [docs/TESTING.md](docs/TESTING.md)).

## Requirements

- A PS5 on firmware 12.40 (the only one tested; the payload has profiles for 22,
  [docs/LIMITS.md](docs/LIMITS.md)), jailbroken, with kstuff and an ELF payload loader
  (Payload Manager, etaHEN or elfldr on port 9021)
- One of the games above, at a version listed there
- PS VR2, connected and set up

## Install and play

1. Build `payload/summerlessonvr2.elf` ([docs/BUILDING.md](docs/BUILDING.md)) and send
   it to your payload loader with the game closed. It stays in the background and waits
   for the game.
2. The first time, it builds the PS VR2 libraries and its shader from your console's own
   firmware into `/data/summerlessonvr2` (a few seconds, once).
3. Put the headset on and start the game from the home screen. It starts with DFR on;
   nothing to set.

Your seat (where the game thinks you sit, and which way is forward) is taken from where
your head is when the game starts. **Hold OPTIONS for a second** to reset the view. The
settings files and the other controls: [docs/SETTINGS.md](docs/SETTINGS.md).

A log of each session is written to `/data/summerlessonvr2/agent.log`: if something
doesn't work, look there first.

## Documentation

- [docs/SETTINGS.md](docs/SETTINGS.md): the settings menu, the settings files
  (`cvars.txt`, `resolution.txt`, `framerate.txt`, one game's own), the controls and the
  debug menu
- [docs/LIMITS.md](docs/LIMITS.md): the game versions, firmware, memory and the known
  problems
- [docs/BUILDING.md](docs/BUILDING.md): building, release and debug builds, the source
  layout
- [docs/HOW-IT-WORKS.md](docs/HOW-IT-WORKS.md): the mechanism: the PS VR2 libraries in a
  PS4 process, the GPU translation, tracking, the TV, hand tracking
- [docs/SUMMER-LESSON.md](docs/SUMMER-LESSON.md): what is particular to Summer Lesson
  (its PS VR calls, frame loop, resolution and memory), with measurements
- [docs/MULTIVIEW.md](docs/MULTIVIEW.md): DFR ("inset mode" there), its design and
  measurements
- [docs/OTHER-GAMES.md](docs/OTHER-GAMES.md): the three games' builds and how their
  addresses were found
- [docs/FIRMWARE.md](docs/FIRMWARE.md): the firmware profiles
- [docs/TESTING.md](docs/TESTING.md): testing without a player
- [docs/FRAME-COPY.md](docs/FRAME-COPY.md), [docs/FOVEATION.md](docs/FOVEATION.md): the
  frame hand-off and the earlier foveation experiment that led to DFR

## Credits

- theghostonline, for [AstroBotVR2](https://theghostonline.dev/theghostonline/AstroBotVR2),
  which this is built on, and [PS5VR](https://theghostonline.dev/theghostonline/PS5VR),
  where the hand tracking comes from; and for Ghost VR, where the memory reserve, the
  exit logging and the `patch-server` check come from.
- AstroQuest, for how a PS VR game is seated and its controller placed: the seat taken
  from the head, the controller held between the palms, and where an unseen controller
  rests.
- [shadPS4](https://github.com/shadps4-emu/shadPS4), for the PS VR structure layouts.
- John Törnblom, for the [PS5 payload SDK](https://github.com/ps5-payload-dev/sdk),
  `pt.c` and the fake-SELF format.
- idlesauce, for [ps5-self-pager](https://github.com/idlesauce/ps5-self-pager).

## Licence

GPL-3.0-or-later. See [LICENSE](LICENSE) and [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).

https://ko-fi.com/mtp24

https://buymeacoffee.com/mtp24
