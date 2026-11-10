# Testing without a player

The payload and the agent can run the game on the console with nobody at it:
a session is recorded once (the controller and the head), then replayed as often as needed, and
the agent's log says how the frames went. The test mode is off unless
`/data/summerlessonvr2/test.txt` exists, and never in a release build (`make RELEASE=1`,
[BUILDING.md](BUILDING.md)), so the game plays normally otherwise. A replay runs with the
headset on a desk, so dynamic foveated rendering (DFR, the README) draws its sharp area where a
replayed or made-up gaze says (`agent.insetgaze`, [MULTIVIEW.md](MULTIVIEW.md)), not where
anyone looks.

## The control port

The payload listens on port 9088 on every interface. A connection saying (`launch`, `kill` and
`ps` in debug builds only):

- `quit` ends the resident payload (only while no game is served);
- `launch` starts Hikari through the system's launcher, `launch CUSA11682` (or `CUSA11674`)
  Chisato (or Allison); refused while a game is served;
- `kill` closes the running app if it is one of the three games; `kill <pid>` ends any process;
- `ps` lists every process (pid, parent, name): a second `payload.elf` under `elfldr.elf` is a
  stray injector, and two injectors in one game crash it;
- anything else is a probe, answered `yes`.

## Recording and replaying

`test.txt` holds words, read once per game start:

- `record`: the controller goes to the log as `pad:` lines (ms of the game's process time,
  buttons, sticks, triggers, one every 16 ms at most), the head's pose as `head:` lines (one
  every 50 ms at most while it moves);
- `replay`: the controller is made up from `pad.txt` and the head taken from `head.txt` (the
  same lines without their prefix, interpolated between samples); a real controller is ignored
  (the palm tracker still starts, seeing no hands; `agent.hands=0` in `cvars.txt` for a run
  without it);
- `worn`: the headset counts as on a head, so the game does not pause with it on the desk.

No recording is included (a session is the player's own movements): make one with `record`, then
the scripts come from the recording's log: `tools/padscript.sh agent.log pad > pad.txt` and
`tools/padscript.sh agent.log head > head.txt`. Pad and head must come from the same recording.

## The runner

```
tools/testrun.sh <payload.elf> <seconds> <out.log> ["replay worn"] [pad.txt] [head.txt]
```

checks the game is closed, asks the resident payload to leave and waits 15 seconds more
(a payload loaded while one is resident has brought the console down), sends the payload,
checks `ps` shows one, uploads the settings and scripts over FTP (port 2121), launches the
game, waits, kills it, pulls the log and prints its health lines. During the run it copies the
agent's log every 2 s (`<out.log>.partial`, kept if the console goes down) and the kernel log
(`<out.log>.klog`), and writes `<out.log>.ticks`: how far each had come every 2 s, to line the
two up. The test mode is not in a release build (`make RELEASE=1`). `PS5_REUSE=1` keeps the
resident payload when it is the same build. `PS5_HOST` names the console, from the environment
or from `tools/local.env` (not committed), a line `PS5_HOST=<address>`. `PS5_TITLE` picks the
game: `CUSA07434` (Hikari, the default), `CUSA11682` (Chisato) or `CUSA11674` (Allison); the
control port's `launch` takes the same title id. A recording is one game's: record each game's
own.

Settings for one run, without touching the console's own `cvars.txt`:
- `PS5_CVARS=<file>` adds that file's lines to it from the launch.
- `PS5_CVARS_AT="<s>:<file> ..."` (in order) makes the added lines another file's `<s>` seconds
  after the launch (`/dev/null`: none).

The console's own file is saved first and put back when the run ends, however it ends
(`tools/consfile.sh`; a put-back that failed leaves `tools/.cvars.orig` or `.cvars.orig.none`,
which stops the next run until the console's file is put right). The agent reads the file every
2 s, so it applies a change within about 2 s of the upload, and logs `cvars.txt changed`;
`tools/fov/report.py` cuts a log there (docs/FOVEATION.md). The sweeps in `tools/fov/runs/*.sh`
drive the runner the same way.

Never edit the runner while a run executes it: the shell reads it as it goes.

## Reading the log

Every 10 s: `hmd: N frames handed over, N shown, N shown again, N never shown; the game's GPU
up to X ms a frame`. The first window counts the game's start before the headset shows anything. L3+R3 (debug builds) during a run
adds a `mark` with a second-by-second snapshot around it ([SETTINGS.md](SETTINGS.md), Debug
builds).
