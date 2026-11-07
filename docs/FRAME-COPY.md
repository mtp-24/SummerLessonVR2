# The frame copy for the native 90 and 120 modes (plan, 2026-11-04)

**Removed from the code (2026-11-04 evening).** The cause of the native modes' shake, ghosting
and black lines turned out to be the hand-off, not the game drawing over a texture Hmd2 still
showed: frames passed oldest-first left a frame of backlog that never drained. Passing the newest
frame (`agent.newest`, below) fixed it with no copy, measured and worn. The copy (`compose.c`,
`copy.s`, `agent.copy`) was off by default since, and is gone from the code; this page stays as
the record of how the cause was found. Its findings that still hold: image_store through a
texture's descriptor never reaches memory on this console (buffer stores do), and
`tools/gcn_asm.py` refuses branches it would mis-encode.

The native modes (framerate.txt 90 or 120, test payload `make -C payload NATIVE=1`) ran at full
rate but the view shook, ghosted and showed black lines; 60 is clean. Hmd2 shows the game's own
eye textures, and the game draws into each of them again three frames on (pairs used in turn,
0,1,2). At 60 that is 50 ms on; at 120 it is 25 ms, and the GPU starts on it sooner, so a texture
Hmd2 still reads for a refresh can be drawn over. The plan: hand Hmd2 copies of our own, made
from the finished picture, which the game never draws into.

## Version 2 (after the two console crashes): the copies as the game's own textures

Version 1 (below, kept as the record) registered linear copies as VR port buffers 8-10 in one
more VideoOut set; with it the console went down twice, cause not found (the section at the end).
Version 2 leaves nothing of that shape: what VideoOut and Hmd2 see is what the stable build gives
them.

- **Registered in the game's textures' place**: the copies are set 0, buffers 0 to 2, three of
  them (the game's three), the registration the stable build makes. The game's textures move up
  to indexes 3-5, unregistered; their frames are copied. Late pairs (2D screens) from 6, as before.
- **The game's descriptor**: each copy's T# is the game texture's (its size, pitch, tiling, format
  and channel select), only its base differs. Hmd2's descriptors are restored to it (gen1.c) as to
  the game's.
- **The copy shader** (`copy.s`, 21 words, no branch): each texel read from the game's texture and
  written into the copy through the same descriptor, both as 32-bit UINT with identity channels
  (bit for bit, the texture unit's own tiled addressing), masked to the texture's size. Its memory
  is in the game's range (since step B, below), the tiled extent over-estimated (rows to 256,
  plus 2 MB).
- **Hmd2's first frames** show the third copy, cleared black: its GPU queue (where copies run) comes
  with its first frame.
- **A frame not copied** (no copy free, a copy not done in 10 ms, another texture) is not shown,
  its frame number given to the next (Hmd2's numbers stay consecutive). 30 in a row, or a copy not
  done in 1 s, stop the copies: the game's textures are then registered (indexes 3-5, a set of
  their own, as a late pair is) and frames go as before; refused, their frames are never passed.
- **60 fps** (copy off): the stable path, line for line (only `begin_frame` now a function).
- **Limits**: copies only when the first 3 s show exactly three eye textures alike, each one
  texture for both eyes (else none, said in the log); late pairs (a 2D screen, eye textures remade
  at another size) have indexes 6 and 7 only (more would mean VR port buffers 8 and up again);
  "no copy free" (Hmd2 not building: the headset dark) skips frames without giving up.
- **Found in review**: tools/gcn_asm.py turned an unresolved branch offset (llvm-mc's `A,A`) into
  hex: a branch in copy.s would have jumped 2570 words off. Version 1's shader (mirror.s) has no
  branch, so it is not the crashes' cause; copy.s has none, and the tool now refuses one.
- Memory (the over-estimate, three copies): 114 MB for 4000x2250 (60 with `agent.copy=1`, as
  measured), about 54 MB at 1248 (120 fps) and about 90 MB at 1728 (90 fps), of the game range's
  225 MB kept for PS VR2, with 72 MB left for the palm tracker (since step B).

## What Ghost-VR does, and what fits here (version 1)

Ghost-VR (`ps4/agent/compose.c`, `compose.s`, `vr2.c` `copy_ready`, `register_many`, `NCOPY`)
composes frames it can't pass as they are (overlays, side-by-side eyes, 2D VR) into two RGBA16F
eye buffers per frame (one per eye, at the eye window's size), on Hmd2's GPU queue, the CPU
waiting for the pass's label before the frame goes to Hmd2. RGBA16F is there for blending
layers; it is twice the memory of the game's 8-bit texture.

Here a frame is one side-by-side 8_8_8_8 texture, plain eyes, no layers. So:

- **The copy has the game texture's exact geometry and bytes**: one linear 8_8_8_8 buffer of the
  same width and height, the eyes where they were. The hand-off (`vr2.c game_loop`) keeps its
  viewports, tangents and uv maths; only the T# and the VR port buffer index change. The TV
  mirror and the fingerprints read the same coordinates and still compare.
- **The copy is the TV mirror's shader** (`mirror.s`, on the console since 2026-11-01): a point
  sample at each texel centre, written as B,G,R,A bytes, read back through a T# whose channel
  select swaps them (exact: UNORM8 -> float -> x255 + 0.5 is the identity). Alpha comes out 1
  (Ghost-VR writes 1 too; Hmd2's base layer doesn't blend). No new shader, no new packets: the
  mirror's command buffer (ACQUIRE_MEM, one dispatch, RELEASE_MEM label).
- **Three copies** (NCOPY 3), buffer indexes 8-10 of the VR port, registered as one more set the
  way the game's pairs are (`pairs_register`: over 3840 wide, at one eye's width: registering a
  4000-wide buffer at its full width brought the console down on 2026-11-01).
- **When**: right before a frame is passed (after the hold that waits for the last pass to be
  built, before Hmd2's BeginFrame), on Hmd2's queue; the CPU waits for the label (up to 10 ms).
  The copies are made once Hmd2's queue is up and the eye texture has kept one size for 120
  frames (not a size the game is about to remake), and only for frames of that size. A copy not done in time,
  a frame of another size, a flat picture, or no slot free: that frame goes as today (the game's
  own texture, still registered). A copy still not done after 1 s: copies off for the session.
- **A slot is written again only once Hmd2 has built a later frame** (gen1_last_built > the
  slot's frame): with the hold, two later frames have been passed and one built by then.
- **Default**: on in the native modes, off at 60 (60 unchanged). `agent.copy=1` in cvars.txt
  turns it on at 60 too (A/B, and a first console test in the mode known to be stable),
  `agent.copy=0` off everywhere.

## Memory

From the logs of 2026-11-04 (`manual2`, the 23-minute 2048 session; `res2048`):

| where | size | holds |
|---|---|---|
| the game's range, hidden from it (DMEM_KEEP) | 224 MB | palm tracker 62 MB; the rest (160 MB free 10 s in) is taken by the game's GPU pool's first two 74 MB blocks |
| the reserve (extra segment top) | 160 MB | PS VR2's buffers: 153 MB used, 7 MB free |
| the spare (extra segment below the reserve) | 352 MB | the game's refused blocks: at 2048, 3-5 x 74 MB plus 8-36 MB blocks; the 23-minute session ran it out |

At 1920/60 a 17-minute session needed nothing beyond the game's own pool.

A copy is the texture's pitch (width rounded to 64) x height (rounded to 8) x 4:

| eyes | texture | one copy | three copies |
|---|---|---|---|
| 1248 (the game's own; 120) | 2496x1404 | 14 MB | 42 MB |
| 1500 (120) | 3000x1688 | 20 MB | 61 MB |
| 1728 (90) | 3456x1944 | 27 MB | 81 MB |
| 2000 (60 default) | 4000x2250 | 36 MB | 109 MB |
| 2048 | 4096x2304 | 38 MB | 113 MB |

(Superseded after step B, below: the copies are now in the game's range.) The copies go at the
**top of the spare**, just below the reserve (allocated there in one piece,
mapped CPU and GPU read/write as the reserve is, `spare_hi` lowered past them). Nothing else is
moved: the palm tracker stays in the game's range (its memory in the extra segment hung the
game), the reserve keeps PS VR2's other buffers. The native modes run at 1248-1728, where the
game's GPU pool grows far less than at 2048; 42-81 MB of the 352 MB spare leaves ~270-310 MB for
it. At 60 the copy is off unless asked for; at 2000-2048 with `agent.copy=1` it takes a third of
the spare, enough for short tests but not for a long session at 2048 (which already ran out
without it). If the allocation fails, there is no copy and frames go as today.

GPU cost: a copy reads and writes the texture once, 14-27 MB each way at the native sizes:
around 0.1-0.3 ms of the GPU a frame, shared with the game. The CPU wait is the copy's GPU time
plus the poll (measured in the 10 s report).

## Risks (console safety first)

- **Kernel and filesystem**: nothing new. The memory calls are the reserve's (allocate and map
  in the extra segment, as step 3 already does for the game's blocks); no kernel writes, no
  storage writes.
- **GPU**: the same shader and command-buffer form the mirror submits every frame on the same
  queue. The dispatch writes only through a V# bounded to the copy buffer, reads only through the
  game's T#. A copy that never finishes is never written over (its slot stays pending) and stops
  copies after 1 s; the frames then go as today.
- **VR port**: one more registration, buffer indexes 8-10 (Ghost-VR's copies use 8-9; VideoOut's
  limit is 16). Refused: no copy, frames as today. Over 3840 wide it is registered at one eye's
  width, as the game's textures are.
- **The main port** (whose flips must never stop) is untouched: the mirror still copies from the
  game's texture as before.
- **The game's memory**: the copies shrink the spare the game's refused blocks come from. At the
  native modes' sizes that leaves most of it; at 60 with `agent.copy=1` and 2048 eyes a long
  session would run out sooner (the game quits; the console is not at risk).
- **The picture**: the copy reads the game's texture at the pass, about where Hmd2's first build
  read it before. The fingerprints measured the texture finished by then at 60 (1196-1200 of
  1200); at 120 that is what the measurement will show (a copy made too early would show up as
  stale or torn refreshes).

## Reviews (2026-11-04)

Three review passes over the diff, and one independent one: nothing found that could panic the
kernel, hang the console, damage storage or leave the main port without flips. Fixed from them:
room for the copies' T#s in gen1.c's table (it held 16, sized for 8 pairs), the copy before
Hmd2's BeginFrame (and BeginFrame where it was for frames not copied: 60 exactly as before), the
copies sized only after the eye texture has kept one size for 120 frames, a failed map giving its
memory back, a build record newer than the frame not trusted, `agent.copy` in the README.

Open, for the measurements: the copy reads the game's texture at the pass, up to a refresh before
Hmd2's own build used to. If the fingerprints show stale or torn copies, the fix is a bounded wait
for the frame's label before the copy (safe now: nothing draws into a copy). The first log also
shows whether VideoOut takes one more buffer set on the VR port (refused: frames as before).

## Automatic half rate: not taken

Ghost-VR switches Hmd2 to half rate (each frame shown twice) when a game draws under 85 fps at
120 Hz. It does not fit here:

- The hand-off here passes frames **in order**, each once the last one is built. At half rate
  Hmd2 builds a new frame every other refresh, 60 a second: a game drawing 95 would back the
  four-frame ring up to its oldest frame (25 ms more latency at 120) and lose ~35 frames a second
  unevenly. Ghost-VR passes the newest frame, which is why it works there; changing that changes
  the hold logic measured clean at 60.
- The game's own 60 mode *is* half rate with the game paced to it (its pose prediction set for
  it). A game that can't hold 120 is better at 90 or 60, or at a smaller eye size, than at a
  native 120 sequence shown at 60.
- The crossed test of 2026-11-01 (native 120 with Hmd2 at half rate) was the shakiest of all.

Revisit only if the copy makes native 120 clean when it holds 120 and the drops are what is left.

## Measurement plan (each run only with the user's go)

With the test payload (`make -C payload NATIVE=1`), `agent.fingerprint=1`, the replay
(`"replay worn"`), ~60 s each, the kernel log streamed:

1. 60 / 1920 with `agent.copy=1`: the copy itself in the stable mode (fingerprints: all finished;
   no errors; the copy's wait).
2. 120 / 1248 with `agent.copy=0`: today's direct path, the baseline verdicts at 120.
3. 120 / 1248 (copy on by default).
4. 90 / 1728 with `agent.copy=0`, then with the copy.

Stop at the first failure. Worn A/B (the shake itself) only after these are clean.

## Console runs (2026-11-04): two console crashes, stopped

- **Run 1** (04:02, the NATIVE payload sent by the runner's swap, 60 / 2000, `agent.copy=1`,
  fingerprints, replay): the whole console went down. The kernel log ends right after PS VR2's
  VR mode switch, no panic text; the agent's log copy was lost (the runner overwrote it with an
  empty fetch: fixed in testrun.sh).
- **Manual run** (the user, the same payload and agent, 60 / 2000, `agent.copy=1`): the console
  went down again. The agent's log ends right after the three eye textures were seen (inside the
  3 s before the game's pairs are registered); no `copy:` line; no kernel log kept.
- Not settled: a crash loses the last seconds of the logs, and the copy's set-up comes 3-5 s
  after the VR switch, in the same seconds as the palm tracker's start. Two crashes in two runs
  with this build, against a clean stable run at the same settings 20 minutes before: the copy is
  the suspect. If it is, the likely parts are the VR port registration of buffers 8-10 in one more
  set (index 10 never used before; the kernel's VideoOut) or Hmd2 given 4000-wide linear textures
  (the game's are tiled; the PS5 side reads the descriptor its own way).
- The console was cleaned (agent.elf and cvars.txt removed: payloads inject their built-in agent).
- Next, only with the user's go and not in a crash cluster: this agent with `agent.copy=0` (the
  copy path never runs) tells the two apart. Designs that avoid both suspects: copies that look
  like the game's own textures (its tiling), or copies registered in place of the game's.

## Version 2 on the console (2026-11-04): step A clean, step B a console crash

- **Step A** (the first send after a fresh jailbreak, this agent, 60 / 2000, `agent.copy=0`,
  fingerprints): clean. 601 and 600 frames shown in the 10 s windows, none never shown; 1200 and
  1199 refreshes finished, none stale or torn; `copy: off`, the game's pairs registered as set 0 as
  in the stable build. So version 2 with the copy off is the stable path, as built.
- **Step B** (the same, `agent.copy=1`, no swap): the copies were set up and registered as
  planned (114 MB at the spare's top, three copies of 4000x2250, set 0, buffers 0-2, accepted) and
  ran for about 2 s; the three fingerprinted refreshes read "other" (13+13, 16+8, 8+10 of 30-38
  differing points: the fingerprint's "three frames before" is a game texture's, not a copy's, so
  this is not yet a verdict on the picture). Then the palm tracker loaded (`hand: load
  libSceVrHand.sprx`, 2 s into the output) and the console went down. The kernel log ends with
  VrTracker2's "First time tracking stable", the line three crashes on 2026-11-01 ended on: the
  palm tracker's start, the game's credentials raised for the headset cameras.
- **Reading**: all three copy crashes (version 1 twice, version 2 once) had a new GPU allocation
  (type 3) at the top of the extra segment's spare before the palm tracker started; copy off,
  never. The palm tracker's own memory in the extra segment already stopped the game in its start
  (reserve step 2b), and Ghost-VR's first palm-tracker start from there took its console down.
  The game's own blocks in the spare (type 3, 74 MB at 2048) are harmless: in every stable log
  they all come after the palm tracker has started. The copies' GPU work is the other difference,
  less likely: the TV mirror's dispatches run on the same queue through the palm tracker's start
  every time, and the palm tracker submits on its own queues.
- **The fix** (`vr2_game_alloc`): the copies' memory in the game's range, in the part kept from
  the game for PS VR2 (225 MB free there when PS VR2 starts), where the palm tracker's own memory
  is, only when 72 MB stay free in one piece after them (the palm tracker's 62 MB, which starts 2 s
  later). Nothing of the copies is in the extra segment any more; `vr2_spare_alloc` and its
  release helper are gone. At 2000 that is 114 of the 225 MB, at 1248 about 54, at 1728 about
  90. The game's later blocks (all after the palm tracker's start in the stable logs) then come
  from the spare sooner, which is untouched (352 MB): the game's total room is the same.
- Not settled: this removes the one difference that matches what is known about the extra
  segment; it does not prove it. `agent.copy=1` with `agent.hands=0` (no palm tracker) would
  separate the allocation from the copies' GPU work. Three crashes with the copy on: no more runs
  that day unless the user decides otherwise.
- The console was cleaned again after the reboot (agent.elf and cvars.txt removed).

## Copies with the palm tracker off (2026-11-04 12:21): no crash, the copy taken too early

- **Run** (`ffe635d`, Hikari 60 / 2000, `agent.copy=1 agent.hands=0`, fingerprints, the replay,
  60 s, no swap): the console stayed up and the game closed normally. The copies took 114 MB in
  the game's range (222 MB free in one piece before), every frame was copied (377, then 596 a
  window; none skipped; the copy's own wait 1.4 ms on average, 3.8 at most), 600 of 600 frames
  shown a window, none never shown.
- **The picture**: the fingerprints read 1199 of 1199 refreshes "other", with 0 points of the
  finished frame and 0 of the old one. The reference is the game's texture once drawn; the copy
  is taken at the hand-over, and the game hands a frame over once its drawing is queued, not once
  its GPU is done. The copy, on Hmd2's queue, read the texture half drawn. (Hmd2 itself reads at
  its build, later, when the frame is done: the stable fingerprints.)
- **Fix**: the copy waits for the game's "drawn" label (`hooks_frame_drawn`, as the TV mirror,
  30 ms at most; not drawn by then: the frame is not shown, a skip). The 10 s report gives the
  wait. Untested.
- So far: palm tracker off, copies on, no crash. The palm tracker's start with copies (in the
  game's range now) is still untested.
- **Priorities (the user, 2026-11-04)**: the copy first, palm tracking fixed later, no copy design
  bent around the palm tracker. So: with the copies on, the palm tracker is not started (the log
  says so) until its start is fixed; the copies' memory keeps only a general 32 MB margin in the
  game's range (the 72 MB kept for the palm tracker is gone). At 2000: 114 MB of 222 free.
- The drawn wait's cost: a frame goes to Hmd2 once drawn and copied, not at the hand-over: at 60
  (half rate) inside its two refreshes; at 120 about a refresh later than before (the game's GPU
  7-8 ms, the copy 1.4). The 10 s report gives both waits.

## The artifacts measured at 90 and 120 (2026-11-04 12:43-12:48, copy off)

The 90/120 test payload (`make -C payload NATIVE=1`, swapped in by the runner: the game closed,
the resident instance asked to quit, 15 s, the kernel log streamed; clean), `agent.copy=0`,
`agent.hands=0`, fingerprints, the replay, 60 s each. Per 10 s window (headset refreshes judged):

| run | GPU a frame | handed over after its refresh | finished | torn | other |
|---|---|---|---|---|---|
| 120 / 1248, window 1 (menu) | 3.4 ms | | 425 | 0 | 0 |
| 120 / 1248, window 3 | 5.0 ms | 4.1 ms | 813 | 0 | 386 |
| 120 / 1248, window 4 | 5.1 ms | 3.7 ms | 0 | 4 | 1195 |
| 120 / 1248, window 5 | 5.6 ms | | 519 | 0 | 373 |
| 60 / 1248 (control), windows 3-4 | 5.0 ms | | 1199, 1199 | 0 | 0 |
| 90 / 1728, window 1 (menu) | 5.4 ms | 2.9 ms | 273 | 7 | 0 |
| 90 / 1728, window 3 | 8.3 ms | 6.1 ms | 718 | 0 | 179 |
| 90 / 1728, window 4 | 8.3 ms | 4.4 ms | 0 | 0 | 899 |
| 90 / 1728, window 5 | 8.9 ms | 4.2 ms | 287 | 0 | 292 |

- "Other": the refresh read a picture that is neither the finished frame nor the one three before:
  of the points that differ between the two, some of each and the rest of neither. A frame read
  while it is still being drawn reads like that (the texture still holds the frame three before
  where the game has not drawn yet, the new frame where it has, and passes not yet finished in
  between).
- At 60 the same payload and size is clean; at both native modes the "other" refreshes come in
  stretches, when the game's GPU time is longer than the time from the hand-over to the next
  refresh (Hmd2 builds a native-mode frame at the refresh after its hand-over: 120 at 1248, 8.3 ms
  refreshes, hand-over ~4 ms in, GPU 5 ms; 90 at 1728, 11.1 ms refreshes, hand-over 4-6 ms in,
  GPU 8.3 ms). At 60 the build comes a refresh later, after the frame is drawn.
- So the artifact caught is Hmd2 reading a frame before the game's GPU has finished it, not the
  game drawing over a frame Hmd2 still shows (texture reuse, which the copy was meant for). The
  copy runs at 60 read "other" for the same reason before the copy waited for the frame to be
  drawn. Not fixed yet (the user: capture first).

## The timing at 120 (2026-11-04 12:58, `agent.trace=1`, copy off): the wait for "drawn" is not the fix

Per 10 s window at 120 / 1248, times after the game's hand-over (the trace's frame lines):

| window | the game's submit | drawn label | passed to Hmd2 | Hmd2's build | fingerprints |
|---|---|---|---|---|---|
| 4 | ~5 ms | ~10 ms | ~8.5-9 ms | ~16.6 ms | 1199 finished |
| 6 | ~7.5 ms | ~14 ms | ~8.5 ms | ~16.6 ms | 508 finished, 377 other |
| 5 | ~7.5 ms | ~12.6 ms | ~19 ms | ~26.9 ms | 53 finished, 1142 other |

- The bad refreshes are the ones Hmd2 builds *late* (27 ms after the hand-over), not early: a
  build at ~16.6 ms reads the finished frame, one at ~26.9 ms reads neither. The earlier result
  at 60 says the same (the drawn wait, `ea237df`, made 14 % of refreshes "neither": built later).
- The texture is not good for three frames: about one frame after it is drawn the next frame's GPU
  work changes it (at 120 the next frame's work comes ~8 ms after its own). The trace's
  "reuse" margin (the frame three on) does not catch that.
- So waiting for the drawn label before the pass would push the builds later and make it worse:
  not done. What fits: a copy taken as soon as the frame is drawn (it takes 0.13 ms), which Hmd2
  can read whenever it builds; or builds kept inside the frame's short good window. The copy runs
  at 60 read "other" even with the copy taken at the drawn label, inside that window: the copy's
  content is wrong for another reason, still to be found.

## The fix (2026-11-04 13:20-13:30): the newest frame to the headset, not the oldest

An independent analysis of the code and the traces found the cause of the late builds: the
hand-off (`vr2.c game_loop`) passed frames in order, `last + 1`, one into Hmd2 at a time. At 60
there is a free refresh every frame, so it never fell behind. At 90 and 120 the game hands over
one frame a refresh and Hmd2 takes one a refresh: a frame of backlog (two hand-overs in one
refresh, a repeat after a hitch) never drained, and every frame then reached Hmd2 a refresh later
(two frames of backlog: built ~27 ms after the hand-over, after the game had begun drawing the
frame three on into that texture). The counters show it: windows where more frames were handed
over than shown turned bad, a hitch window (repeats) reset it. The picture handed over with count
c is complete once the drawn labels reach c - 1 and drawn over soon after c + 1 (why the old wait
for c, a frame late, made 60 worse).

Fix (`agent.newest`, on by default, native modes only): each pass takes the newest frame handed
over; frames between are counted as never shown, as PS VR took the newest. Same calls as before,
no memory, no GPU work. The frame copy is off by default (`agent.copy=1` for it): not needed.
(`agent.copy` and the copy were removed since: see the top of this page.)

Results (the replay, 60 s each, fingerprints; `agent.hands=0`; copy off):

| run | finished | torn | other | never shown a window | on the headset after the hand-over |
|---|---|---|---|---|---|
| 120 / 1248 before (run2, run5) | 0-1199 | 0-38 | 386-1195 a window | 0 | 15-34 ms |
| 120 / 1248 newest (run12, run14) | 1199-1200 in steady play | 0-1 (loading) | 0 | 1 | 11.6 ms |
| 90 / 1728 before (run4) | 0-718 | 0-114 | 179-899 a window | 0 | |
| 90 / 1728 newest (run13) | 897-900 in steady play | 0-1 (loading) | 0 | 0-2 | 14 ms |
| 60 / 2000 (run15, defaults) | 1199 | 0 | 0 | 0 | unchanged |

Also found on the way: `image_store` through the texture's descriptor never reached memory on
this console; the copy (when asked for) now copies the texture's bytes with buffer loads and
stores (a4e3cfa), and every read-back check matched. Not yet done: a worn test at 90 and 120 (the
shake was felt, the fingerprints measure the picture, not the feel).

## Worn (2026-11-04 afternoon): the fix felt, TAA's ghosting, what each mode holds

- 120 / 1248 worn with the fix: no tearing or artifacts seen; every refresh a new, finished
  frame (fingerprints 1195-1197 of 1200 finished, 0 torn, 0 other, no repeats). The ghosting left
  on quick head turns is the game's TAA (`r.DefaultFeature.AntiAliasing=2`, the game's default,
  quality raised to 3 by the game after its start): `r.PostProcessAAQuality=0` live took it away
  (jagged), back to 3 brought it back; FXAA (`r.DefaultFeature.AntiAliasing=1`, quality 6) live:
  no ghosting, softer edges than TAA. TAA settings this build has: `r.TemporalAASamples` (8),
  `r.TemporalAASharpness` (1.0), `r.Tonemapper.Sharpen` (0); not `r.TemporalAACurrentFrameWeight`.
- cvars.txt now applies live (every 2 s): the game's variables, `agent.newest`, `agent.predict`.
- What holds, in the scenes played (heavier than the replay): 120 / 1500 FXAA 6: no drops in play,
  GPU 7.1-8.0 ms of 8.3. 120 / 2000: ~84 fps (360 repeats a 10 s). 90 / 1800 TAA: a steady 77 fps
  (127 repeats a 10 s; ~13 ms a frame); 90 would hold at about 1700.
- The user's choice: 60 (at the highest resolution), keeping the fixes. At 60, 2048 already ran the
  full replay without drops (the reserve, 2026-11-04 morning): GPU 14.5 ms average, 17.8 at most
  of 16.7: the limit there is the GPU, not memory.
