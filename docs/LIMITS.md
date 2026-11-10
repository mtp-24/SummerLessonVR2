# Known limits

## The games

The Asian English releases, at these versions (the package's `VERSION`; the PS5 shows
01.00 for all of them):

| Game | Title id | Version | |
|---|---|---|---|
| Hikari Miyamoto | CUSA07434 | 01.00 | played and swept |
| Hikari Miyamoto | CUSA07434 | 01.02 (its update) | played and swept |
| Chisato Shinjo | CUSA11682 | 01.02 | played and swept |
| Allison Snow | CUSA11674 | 01.01 | played and swept |

The payload checks each game's code, not only its version, before it touches it: a
version not listed here is used only if its code is one of these builds', else left
alone with a notification. Dynamic foveated rendering (DFR, the README's Features) runs
in all of them. Hikari 1.02, Chisato and
Allison share a newer version of the game's engine than Hikari 1.00
([OTHER-GAMES.md](OTHER-GAMES.md)); "swept" is the replayed sweeps of each game at 60,
90 and 120 fps ([SUMMER-LESSON.md](SUMMER-LESSON.md), the sweep with the palm tracker
on).

## Firmware

Tested on firmware **12.40** only. The payload has profiles for 22 firmware versions
(5.50 to 13.60, from the projects credited in the README, and 12.70 checked here against
12.40's files), but no other has been tried with these games ([FIRMWARE.md](FIRMWARE.md)).
Hand tracking needs the firmware's own `libSceVrHand`, which the profiles for 12.00, 12.02,
12.40, 12.60, 12.70, 13.00, 13.20, 13.40, 13.42 and 13.60 have.

## While playing

- **Don't turn on PS VR2's see-through view (passthrough) while the game runs**: switched
  on as Allison loaded, it hung the console.
- **Don't send the payload again while the game is running**: in testing, that hung the
  console.
- If ps-patch-system's `patch-server` is running, the payload stays out of the game and
  says so (in Ghost VR it crashed the console). Stop it and start the game again.
- **Hikari 1.02:** a bloom flicker in the right eye was seen once on entering the café,
  until the AA quality was changed and back; not reproduced yet
  ([SUMMER-LESSON.md](SUMMER-LESSON.md), the café bloom flicker).
- **Garbage collection:** Chisato and Allison collect garbage about once a minute, each
  time a dropped frame of about 40 ms; the agent spaces the collections 600 s apart by
  default (`agent.gcwait`, [SETTINGS.md](SETTINGS.md)). Over 15-minute sessions with that
  spacing their memory stayed flat. Hikari 1.02 turns the timed collection off itself (no
  once-a-minute hitch, its memory flat too); Hikari 1.00 keeps its own.

## Memory and long sessions

- **The stock picture at 2048:** the game ran out of GPU memory at the start of day 2
  (going to Hikari's room); 2000, the default, played through three days with memory to
  spare ([SUMMER-LESSON.md](SUMMER-LESSON.md), Where it stands).
- **Long sessions with DFR:** two early sessions (19 and 25 minutes) ended right after a
  save, the game out of CPU memory. DFR now gives the game's memory pool its full size;
  since then a 66-minute session has run with its free memory flat. Save before long
  breaks.

## Hand tracking

- With one hand on the controller the tracker keeps losing it, so one hand isn't used:
  the controller follows your hands only when both palms are seen.
- The tracker's start takes the headset cameras, which the system's own screens use too.
  Its start waits until the kernel log shows the system's camera work 12 s behind
  ([HOW-IT-WORKS.md](HOW-IT-WORKS.md) 9); if that never clears there is no start, said in
  the log every 30 s, and after 20 s without the kernel log's word the player is told to
  put the headset on again or start the game again.
