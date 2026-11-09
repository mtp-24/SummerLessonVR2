# The other two Summer Lesson games

Research (2026-11-04) for supporting the two other PS4 Summer Lesson titles with this payload,
from their files, then the first runs of both games (below), then inset mode in both
(2026-11-08, "Inset mode" below). The files were fetched over FTP while
each game was open (`/mnt/sandbox/pfsmnt/<title>-app0/`, its `eboot.bin` served unlocked) and
are kept under `dumps/` (never committed).

| | Hikari Miyamoto (the reference) | Chisato Shinjo | Allison Snow |
|---|---|---|---|
| Title id | CUSA07434 | CUSA11682 | CUSA11674 |
| Content id | HP0700-CUSA07434_00-SLHIKA01DLG00000 | HP0700-CUSA11682_00-SLTRES01DLG00000 | HP0700-CUSA11674_00-SLALLY01DLG00000 |
| APP_VER (no update installed) | 01.00 | 01.00 | 01.00 |
| SYSTEM_VER (param.sfo) | 4.00 | 4.50 | 4.50 |
| SDK in the eboot's process parameters | 0x03508031 | the same | the same |
| Unreal Engine | 4.12.5 | 4.12.5 | 4.12.5 |
| `eboot.bin` | 80,640,149 bytes | 81,425,995 | 81,425,995 (a different build) |
| Code segment | 0x440407c bytes | 0x44a2e5c | 0x44a2c2c |
| Data segment (VA, memory size) | 0x4668000, 0xba4e90 | 0x470c000, 0xbad010 | the same as Chisato |
| `sce_module/` | 12 libraries | the same 12, byte for byte | the same 12, byte for byte |
| Imports | 959 | 979: all of Hikari's, plus 4 of libSceShareUtility, 15 of libSceJson2, 1 of libc | the same as Chisato |

## Method

Every address our code takes from Hikari's eboot was looked for in the other two by its code, not
by its value:

- **A static** (a variable at a fixed address): every instruction in Hikari's code that reaches
  it (rip-relative) was found, and the 10 instructions from each made into a signature, with
  every rip-relative displacement and rel32 branch masked (those change when code or data
  moves; register use, struct offsets and constants stay). A signature found exactly once in the
  other eboot gives the static's address there from its own instruction. Each static's
  references all voted for one address.
- **A code site**: a signature from its address, lengthened until found exactly once.
- **The functions we rely on** were then compared whole, instruction by instruction.

The tools are in `tools/ebootmap/` (capstone for Python, and `xref.c` built with
`cc -O2 -o tools/ebootmap/xref tools/ebootmap/xref.c`):

    tools/ebootmap/ftpget.py /mnt/sandbox/pfsmnt/CUSA11682-app0 dumps/CUSA11682/app0  # game open
    tools/ebootmap/match.py   dumps/CUSA07434/eboot.bin dumps/CUSA11682/eboot.bin 0x46917f0
    tools/ebootmap/codemap.py dumps/CUSA07434/eboot.bin dumps/CUSA11682/eboot.bin 0x2f8d24b
    tools/ebootmap/fdiff.py   dumps/CUSA07434/eboot.bin dumps/CUSA11682/eboot.bin 0x34a70 0x34a70 0x300
    tools/ebootmap/libs.py    dumps/CUSA11682/eboot.bin BQ

`match.py` maps a static, `codemap.py` a code address, `fdiff.py` compares two stretches of code,
`libs.py` names an import's library, `ftpget.py` fetches a game's files (PS5_HOST from
`tools/local.env`). The same works for a game update: map 1.00's addresses into the new eboot.

## What was found

**Every address maps, unambiguously.** Chisato and Allison share their data addresses; their
code differs from each other by up to 0x400.

Statics (the agent reads them through the game's base, `hooks.c`):

| What | Hikari | Chisato and Allison | References agreeing |
|---|---|---|---|
| The PS4 RHI viewport (`hdr->viewport`; its labels +0x2a8, +0x2b0, the frame count +0x360) | 0x46917f0 | 0x4735970 | 38 of 38 |
| Unreal's IConsoleManager singleton (cvars.txt) | 0x46b4350 | 0x47584d0 | 11 of 11 |
| GEngine | 0x4b186f0 | 0x4bbc850 | 26 of 26 |
| t.MaxFPS's value pointer | 0x4b18cd0 | 0x4bbce30 | 2 of 2 |
| The game's Neo-mode answer / question | 0x4b8ef08 / 0x4b8ef10 | 0x4c33068 / 0x4c33070 | 8 of 8 / 5 of 5 |
| r.ScreenPercentage's data, cached by the plugin (resolution.txt) | 0x4bc5978 | 0x4c71a98 | 2 of 2 |
| FApp::DeltaTime | 0x46690d0 | 0x470d0d0 | 19 of 19 |

Code sites:

| What | Hikari | Chisato | Allison |
|---|---|---|---|
| fps patch 1: the plugin's constructor, `mov dword [rbx+0x2b8], 1` | 0x2f8d24b | 0x2ffc31b | 0x2ffc2db |
| fps patch 2: the setting unset, `mov dword [r14+0x2b8], 1` | 0x2f8e564 | 0x2ffd634 | 0x2ffd5f4 |
| The end of frame (label write, hand-over) | 0x34b70 (0x34c0f, 0x34c97) | the same | the same |
| The GPU-done event (`sceGnmAddEqEvent(queue, 0x40)`) | 0x335ad | the same | the same |
| CalculateRenderTargetSize | 0x2f8fb20 | 0x2ffebf0 | 0x2ffebb0 |
| The plugin's FindConsoleVariable call | 0x2f8e3a1 | 0x2ffd471 | 0x2ffd431 |
| UpdatePostProcessSettings | 0x2f8ff60 | 0x2fff030 | 0x2ffeff0 |
| NeedReAllocateViewportRenderTarget | 0x2f8fc40 | 0x2ffed10 | 0x2ffecd0 |
| GetMonitorInfo | 0x2f8eb00 | 0x2ffdbd0 | 0x2ffdb90 |
| UGameEngine::GetMaxTickRate / UEngine's | 0x15aea50 / 0x1975740 | 0x15ae6a0 / 0x19753a0 | 0x15ae640 / 0x1975340 |
| The console manager's maker | 0x117cc0 | 0x117ec0 | 0x117e60 |
| The pose prediction | 0x2591f38 | 0x2591b98 | 0x2591b38 |
| The frame functions the labels come from | 0x36fa0, 0x34a70 | the same | the same |
| FPlatformProcess::Sleep | 0x2927d0 | 0x292970 | 0x292910 |

**The code we rely on is the same code.** The end-of-frame function (172 instructions), the
GPU-done event's setup, the per-frame label function (100), CalculateRenderTargetSize (152), the
plugin's console-variable lookup (168) and its constructor all match Hikari's instruction for
instruction; only call and branch targets differ, by where things moved. So the struct offsets
the agent uses (the viewport's +0x2a8 / +0x2b0 / +0x360, the console manager's vtable +0x88, a
variable's +0x38 / +0x40, the plugin's +0x2b8) hold in both, and the fps patch sites carry the
same original bytes.

**The system side needs nothing new.** Every function Hikari imports, which is everything the
agent hooks or traps by NID, the other two import too. Their 20 extra imports are the share
menu (libSceShareUtility: InitializeEx2, Terminate, AdvanceInitialize,
AdvanceSetUploadContentData), JSON parsing (libSceJson2) and one libc function: nothing PS VR.
The bundled libraries, the PS VR ones among them (HeadTracker, HandTracker, FaceTracker), are
byte-identical to Hikari's.

## What the files can't tell

These need a first run of each game, and are what the port's early runs would measure:

- **Memory.** Hikari's pools (the 224 MB the CPU pool asks for again, the GPU pool's 74 MB
  blocks at 2000, the 64 KB reservation at 0xfe0000000 that the AgcDriver move works around) are
  behaviour, not addresses. Same engine and SDK make them likely the same; the scenes differ.
- **GPU time.** Different characters, rooms and lighting: 2000 an eye may have more or less room.
- **The game's own eye size** (r.ScreenPercentage from its ini, 130 in Hikari) is in the 8 GB
  data pack, which wasn't fetched.
- **Its frame loop at run time** (two vblank events a frame on its own queue, two eye-texture
  pairs). The code that makes it is identical, so it should be.
- **Anything scene-specific** (menus, the setup dialog's timing).

## Support (2026-11-04: built; Allison Snow and Chisato Shinjo run)

One payload serves all three; Hikari's behaviour is unchanged. The settings in
`/data/summerlessonvr2` (`resolution.txt`, `cvars.txt`, the rest) are shared by the three.
What it took:

In the payload (`main.c`), written for one title before:
- `TITLE_ID` / `k_versions` became `k_builds`: per build its title id, version, name, the
  viewport static and the agent's six others, and the two frame-rate patch sites (`k_fps120`
  holds their bytes, the same in all three);
- `find_game`, `check_other_title`, the control port's `launch [title id]`, `kill`, the sandbox
  path of the PS VR2 libraries (`/mnt/sandbox/<title>_NNN/...`) and the notices take the
  matched build's title;
- the agent's header carries the build's statics (`console_mgr`, `engine`, `max_fps`, `neo`,
  `sp_cache`, `delta_time` after `viewport`; 144 bytes, was 120). An `agent.elf` in
  `/data/summerlessonvr2` whose header is another size (one uploaded by an older build) is not
  used: the payload would write past its header. The built-in agent goes in instead.

In the agent (`hooks.c`): the six statics written as Hikari's numbers before come from the
header; one that is 0 (an unknown build) is not read. Nothing else in the agent reads the
game's memory.

Elsewhere: `tools/testrun.sh` takes `PS5_TITLE`; a recording per game for `docs/TESTING.md`'s
replays is still to be made.

The first run of each is the check of all this: the log's `game pid ... (Summer Lesson: Chisato
Shinjo 01.00)`, `gpu: r.ScreenPercentage 130 ... -> 208.33` (the cache found, the eye size set),
`cvars:` lines answering (the console manager found), the frame labels timing the GPU, and the
memory and GPU time against Hikari's.

## Allison Snow's first runs (2026-11-04)

Four runs on the stable build (logs `allison1` to `allison4`, kept locally):

| Run | Palm tracker | Headset worn | Result |
|---|---|---|---|
| 1 (by hand, her first start ever) | on | yes, at the safety notice | the console down at the setup screen |
| 2 (the runner) | on | yes, at the safety notice | the console down within 2 s of the headset going on |
| 3 | off (`agent.hands=0`) | no | 60 s clean (the game paused, the headset off) |
| 4 | off | yes, through the notice and the play-area setup | 90 s clean |

Everything this port reads from her eboot checked out (run 3): her own `r.ScreenPercentage`
130 -> 208.33 (2000x2250), `t.MaxFPS`, the Neo-mode answer, the cvars. Her memory matches
Hikari's to the megabyte. GPU 9-13 ms a frame at 2000, 600 frames shown a 10 s, none lost.

Both crashes came as the system's safety notice and play-area setup had the headset cameras
(the kernel log's `hmd2_image` opens, by the system's camera process), the palm tracker
starting on its old timer (2 s of headset output) in that time. In Hikari's three kept runs
through the same screens with the palm tracker on, it started during the notice, before the
headset went on (its `hmd2_leddet` open), took the camera images (`hmd2_image`) only once the
setup had finished, and they survived; Allison's crashes came as the headset went on, inside
the setup. Ghost VR saw its first palm-tracker start in a new game take the
console down too (inside sceVrHandInitialize, the credentials just raised). The palm tracker
now waits for the headset worn and no system screen up (HOW-IT-WORKS, hand tracking), tested
on her in runs 5 and 6 below and on by default again.

Run 5 (`allison5`, after a fresh restart, `agent.handwatch=1`: the check watched, the palm
tracker never started) put the check beside the kernel log through the notice and setup:

| Time | Kernel log | The check |
|---|---|---|
| 17:12:30 | the safety notice starts, the system's camera process opens `hmd2_image` | |
| 17:12:32 | | the headset on |
| 17:12:34 | | the controller held by the system, the background flag set |
| 17:12:41-43 | the play-area setup, `success to set playarea`, `SafetyNoticeFinished` | waiting |
| 17:12:46-48 | | the controller the game's again, the background flag clear |
| 17:12:53 | | it would have started |

The overlay flag never came up for the notice or the setup (only, for 0.25 s, for the PS
menu); the background flag is a second signal beside the controller's since then. Four more
starts in the same run (the notice not needed again) were each clear 5 s after the headset went
on.

Run 6 (`allison6`, after a fresh restart, `agent.hands=60`, the real start): the notice
began at 17:23:53, the check held (background flag, then the controller), the setup finished
at 17:24:05 (`SafetyNoticeFinished`), and the palm tracker started at 17:24:11: the game's own
`hmd2_leddet` and `hmd2_image` opens the first after the system's. Both hands tracked (100% in
most 10 s spans, 0 GPU work dropped, 27,482 buffers), 600 of 600 frames a 10 s, 5 minutes, the
game closed by the runner with `VrHandStop`/`VrHandFinalize` 0. The two crashes of runs 1 and 2
did not come back.

## Chisato Shinjo's first run (2026-11-04)

Her first start ever (`chisato1`, after a fresh restart, `agent.hands=60`, 5 minutes). The
first launch was closed by the player 7 s in and the game started again, before PS VR2 had
started (the palm tracker not loaded yet). The notice began at 17:32:35, the check held
(the background flag, then the controller), the setup finished at 17:32:42, and the palm
tracker started at 17:32:49, its `hmd2_image` open the first after the system's. Both hands
tracked (mostly 100% of each 10 s), no GPU work dropped, 600 of 600 frames a 10 s at 2048 a
side (her own `r.ScreenPercentage` 130), GPU 11.2-11.3 ms a frame. Her statics checked out as
Allison's did: the eye size, `t.MaxFPS`, the Neo-mode answer, the cvars.

## Inset mode (2026-11-08)

Inset mode's code (`agent/multiview.c`, `agent/insetpp.c`) names Hikari's addresses; `mv_at`
gives the game's own from a table (multiview.c, after `add()`), the build passed in the agent's
header (k_builds' index + 1, in the word after `vr_persist`). Every one of its 67 addresses was
found as above: each code site by its own code found once (`codemap.py`), the short functions
also by every call that reaches them (all agreeing), the statics and the render commands'
execute functions by the instructions that reference them (`match.py`), the pads by their
neighbours' shift and their bytes. The patch sites' stock bytes are Hikari's in all three except
Draw's call of CalcSceneView, which the agent now makes from the addresses; the payload checks
every entry against the game before writing any. `tools/fov/multiview_test.c` boots the agent as
each build against its eboot: 23 entries on each game's own bytes, the calls on their pads, the
hooks' prologues the same. An unknown build, or an address the table lacks, leaves inset mode off.

Worn: Allison 66 minutes in inset mode, no crash, its 224 MB CPU pool given (as Hikari's:
SUMMER-LESSON.md), its flexible memory flat at 294-295 MB (`logs/play-b2ac6a7`). Chisato: not
run in inset mode yet.

Settings per game: `cvars-<game>.txt` after `cvars.txt` (its lines win) and
`resolution-<game>.txt` instead of `resolution.txt`, `<game>` hikari, chisato or allison.

## Hikari Miyamoto 1.02 (2026-11-08)

Hikari's update, installed as one package (`app.pkg`, 3 GB): its `param.sfo` keeps `APP_VER`
01.00 and says `VERSION` 01.02, so the payload, which read `APP_VER` alone, took it for 1.00 and
gave the agent 1.00's addresses (the first try: the multiview table refused on its bytes, the
game gone a few seconds after its start movie; the console fine). The eboot was fetched with no
payload resident, the game open (`/mnt/sandbox/pfsmnt/CUSA07434-app0/eboot.bin`, the same file as
in `-app0-patch0-union`; 80,571,099 bytes, kept under `dumps/CUSA07434-102/`).

**It is Chisato's and Allison's newer engine.** Its eboot has their newer TAA's variables
(`r.TemporalAACurrentFrameWeight`, `FilterSize`, `CatmullRom`), which 1.00's lacks, and much of
its engine code sits at Allison's very addresses. Code 0x43fbf8c bytes (1.00: 0x440407c), data at
0x4658000 (0x10000 below 1.00's). So the agent treats it as the newer engine
(`build_newer_engine()`: AA quality 3 at most in inset mode's preset and the menu, as in Chisato
and Allison); its settings files are Hikari's (`cvars-hikari.txt`, `resolution-hikari.txt`).

Every address mapped from 1.00 (`tools/ebootmap`), all unambiguous:

| What | 1.00 | 1.02 | |
|---|---|---|---|
| Viewport | 0x46917f0 | 0x46817f0 | 58 of 58 references |
| Console manager | 0x46b4350 | 0x46a4350 | 19 of 19 |
| GEngine | 0x4b186f0 | 0x4b08710 | 31 of 31 |
| t.MaxFPS's value pointer | 0x4b18cd0 | 0x4b08cf0 | 2 of 2 |
| Neo answer | 0x4b8ef08 | 0x4b7ef28 | 8 of 8 |
| r.ScreenPercentage's cache | 0x4bc5978 | 0x4bb4b58 | 2 of 2 |
| FApp::DeltaTime | 0x46690d0 | 0x46590d0 | 19 of 19 |
| fps stores | 0x2f8d24b, 0x2f8e564 | 0x2f8826b, 0x2f89584 | same bytes |
| TAA shader types 10/11/20/30/40/00 | 0x49f0430... | 0x49e0430... (each 0x10000 below) | by each name's registration |
| Inset mode's sites (`multiview.c` `k_at`) | | the fourth column | |

Inset mode's 69 sites: 45 by `codemap.py` (found once); the 18 short ones found more than once
(one-line functions, the render commands' execute functions) by the shift of their mapped
neighbours, each checked over 24 instructions with branch targets and rip-relative operands
masked (one candidate each); the statics by `match.py`; `FSceneViewStateReference`'s vtable by
its base (0x452be28 -> 0x451fe28, 8 of 8). `multiview_test` passes with its eboot as a fifth
argument: the 24 entries on its stock bytes, the calls on the pads, the hooks' prologues Hikari's,
the native 90/120 stores found.

**The payload now tells a build by its code**, not by the version alone (`build_by_code`): the
version picks the first guess, and the game's image must hold that build's two frame-sequence
stores (k_fps120's stock bytes at its `fps120_at`: each of the four eboots has them at its own
places only) and, at 0x1be91 (the same place in all four), `lea rax, [rip + disp]` reaching the
build's viewport, which checks its data addresses too. Otherwise the one other build of the title
whose code it is, else the game is left alone with a notification: an update that keeps its
version can no longer be given another build's addresses.

Not known until it runs: the TAA shader. Our TAA patch is for 1.00's type-10 shader (FNV-checked),
so in 1.02 it is most likely refused and the engine's TAA kept, as in Chisato and Allison (the
log's `taa:` line says). The data pack (shaders, the game's own settings) may differ too.
