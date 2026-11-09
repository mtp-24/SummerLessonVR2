# Firmware 11.60

> Written for [AstroBotVR2](https://theghostonline.dev/theghostonline/AstroBotVR2), which this
> port is built on. The mechanism is the same here; what differs for Summer Lesson is in
> [SUMMER-LESSON.md](SUMMER-LESSON.md).

AstroBotVR2 runs on 11.60 as well as 13.60: immersive in the headset, with
head tracking, on a console on 11.60 with Astro Bot CUSA12392 v1.00. This page
lists what differs from the 13.60 walkthrough in [HOW-IT-WORKS.md](HOW-IT-WORKS.md).

11.40 is the same as 11.60 here: its libraries are different builds (their own
hashes in the profile) but every offset in the 11.60 profile matches, checked in
11.40 dumps of the five libraries and the AV control service.

12.40 mixes the two: libSceHmd2 has 13.60's size and layout but 11.60's
reprojection shader. The AV service still records Astro Bot as PS VR1 there, so
the AV record fix runs as on 11.60. Its offsets are in the table below, each
found in 12.40 dumps rather than carried over.

## Firmware profiles

Everything that depends on the firmware is in one place, `common/firmware.h`:
a profile per firmware, picked by the payload at start. Any other firmware is
refused as before. There are 19: 5.50, 7.61, 8.40, 9.00, 9.60, 10.01, 10.20,
10.60, 11.40, 11.60, 12.00, 12.02, 12.20, 12.40, 13.00, 13.20, 13.40, 13.42 and
13.60. This port (Summer Lesson) has run on 12.40 only. A profile holds:

| | 11.60 | 12.40 | 13.60 |
|---|---|---|---|
| libSceHmd2 SDK-version PLT stub | `0x112e00` | `0x67420` | `0x67a30` |
| Hmd2 reprojection shader (RDNA dwords -> GCN dwords) | `0x190f00` (3526 -> 3878) | `0x9cf00` (3526 -> 3878) | `0x9cf00` (3501 -> 3853) |
| libSceVideoOut port lookup / release | `0x10170` / `0x10130` | `0x10470` / `0x10430` | `0x10400` / `0x103c0` |
| libSceVideoOut layout permission, enhanced-attribute flags | same as 13.60 | same as 13.60 | `0x358d4`, `0x3007c` ... |
| AV service layout-handler branch | `0x41733b` | `0x41735b` | `0x41739b` |
| AV service `checkVideoModeForChangeOutput` exit | `0x431f68` | `0x431eb8` | `0x431ed8` |

and the size and SHA-256 of each library on that firmware. On 11.60 and 12.40
libSceVideoOut's hash is checked too, since the agent writes into it. The AV
patches check the original bytes before writing, as before.

The prepared libraries in `/data/summerlessonvr2/libs` are tagged with the
firmware (`/data/summerlessonvr2/prepared`), so they are rebuilt once after a
firmware change or a change of the cache's format.

The agent learns the firmware through its header: the padding word after
`ipd_um` now holds it. The header's size and layout are unchanged.

## Older firmware: 5.50, 7.61, 8.40, 9.00, 10.01

The same profile, worked out from dumps people sent. 5.50, 7.61 and 9.00 have
run on a console (reported by the people who sent them); 8.40 and 10.01 haven't
yet.

| | 5.50 | 7.61 | 8.40 | 9.00 | 10.01 |
|---|---|---|---|---|---|
| libSceHmd2 SDK-version PLT stub | `0xf4f20` | `0x10d8a0` | `0x117460` | `0x118490` | `0x112fe0` |
| Hmd2 reprojection shader | `0x16cf00` (4524 -> 4943) | `0x189000` (3616 -> 3861) | `0x195000` (3526 -> 3878) | `0x194f00` (3526 -> 3878) | `0x190f00` (3526 -> 3878) |
| libSceVideoOut port lookup / release | `0xe620` / `0xe5e0` | `0xf990` / `0xf950` | `0xfb50` / `0xfb10` | `0x102b0` / `0x10270` | `0x10450` / `0x10410` |
| libSceVideoOut layout permission | `0x314e4` | `0x35624` | `0x35664` | `0x358c4` | `0x358c4` |
| libSceVideoOut enhanced-attribute flags | none | none | none | `0x30018`, `0x3001c`, `0x30078` | as 9.00 |
| AV service layout-handler branch | `0x40d959` | `0x415129` | `0x4156cd` | `0x4167dd` | `0x417532` |
| AV service video-mode check | `0x429744` | `0x42c0c4` | `0x42c6e4` | `0x430964` | `0x431b98` |

- Before 9.00, `sceVideoOutEnhancedAttribute` sets only the port's own fields,
  with no globals to put back, so `vo_flags` is 0 there. 9.00 sets three
  (two dwords and a byte) where 11.40 and later set five; the byte is listed
  in all three byte slots of its profile.
- 9.00's AV service is built like 5.50's: its layout check is a short `je`
  (made a `jmp`), and the video-mode check keeps its result in `r15d` and
  leaves through a shared exit that reads the stack-guard pointer from `rbx`.
  The patch is `xor r15d, r15d` and a short jump to an error path that loads
  `rbx` and goes to that exit, so the check returns 0.
- 7.61's shader has one image sample in the gfx10 NSA form (address registers
  anywhere). The translator copies those addresses into its scratch VGPRs and
  writes a plain gfx7 MIMG. It also needed `s_cmp_eq_i32` and `s_cmp_lg_i32`.
  The 13.60, 11.x and 8.40 shaders come out byte for byte as before.
- 5.50's AV service is built differently. Its layout check is a short `je`
  (made a `jmp`), and the video-mode check returns through its own exit, which
  loads the stack-guard pointer the epilogue reads: `xor r12d, r12d` and a short
  jump there.
- 7.61's AV service is built like 5.50's (same registers, `xor r12d, r12d` and
  a short jump). 10.01 mixes the two: 9.00's short `je` for the layout check and
  11.x's `xor eax, eax; jmp` for the video-mode check.
- 12.20, 12.02 and 12.00 match 12.40 byte for byte at every offset and both AV
  sites; only the library builds differ.
- 13.42, 13.40, 13.20 and 13.00 match 13.60 the same way.
- 9.60 matches 9.00 byte for byte at every offset and both AV sites; only the
  library builds differ.
- 10.20 and 10.60 match 10.01 byte for byte at every offset and both AV sites;
  only the library builds differ.
- 9.60, 12.00, 12.02 and 13.40 came from AstroBotVR2 1.1.5-1.1.7 (and Ghost-VR
  0.9.4-0.9.8), with the hand tracker's library for 12.00 and 13.40; Ghost-VR
  0.9.6 adds it for 12.02 and 13.00 (13.60's size and imports). None has run here
  (this console is on 12.40). Hand tracking on 10.01 and 10.20 needs more than a
  profile (an older libSceVrHand: its shaders inside it, `buffer_atomic_cmpswap`,
  a wave32 VCCZ fix in the translator, the unsized `sceKernelGetAppInfo`): not
  here yet.
- 8.40's AV service is built like 9.00's (`xor r15d, r15d` and a short jump to
  the error path that loads `rbx`). Its first dump came encrypted, so the
  profile had no AV sites until a decrypted copy came in.
- On any firmware without a profile, the
  payload doesn't start: it decrypts the five libraries and
  `/SceSysAvControl.elf` into `/data/summerlessonvr2/firmware-<version>` (for
  example `firmware-12.40`) and says so in a notification, so the files can be
  sent over.

## The game's image base

On 13.60 the game's `eboot.bin` was always mapped at `0x400000`. On 11.60 it
can be mapped elsewhere (seen at `0x43be8000`), and the build check then read
zeros and reported an unknown build of Astro Bot. The payload now takes the
base from the loader's record for the game (dynlib handle 0) for the build
check, the import slots and the entry breakpoint.

## The AV service's app record

On 11.60 the game ended up on the cinematic floating screen in the headset,
although every Hmd2 and VideoOut call succeeded. The kernel log shows why. The AV
control service keeps a record of each app, with flags that include its VR
type, and builds it once, when the app launches. In those flags `0x200000`
means a PS VR1 title and `0x10` a PS VR2 title. Astro Bot is recorded as
PS VR1 (`VR:1` in the service's `-- app[...]` lines). The attribute3 bit the
payload sets comes too late: the service doesn't read it again. With a PS VR1
app in front, the service keeps the headset on the single-output USB layout
(cinematic) and never switches to the dual layout (`HDMI_USB`, "2HEADS") that
PS VR2 games get.

So on 11.60, once the game runs, the payload finds the game's record in the
service's memory (pid, appid, flags) and makes it a PS VR2 record: `0x200000`
cleared, `0x10` set. The service keeps two copies of the record, and nothing
is written unless every match is the same record. The record goes away with
the game. The kernel log then shows `layout:USB -> HDMI_USB`, `vrType(2)` and
`gen2vr=True`, as for a PS VR2 game.

On 13.60, marking attribute3 was already enough, so this step is skipped there.

## Brightness

Not firmware-specific: the game's eye textures are sRGB, and the headset
picture was much too dark. See the commit "Sample the game's sRGB eye textures
as UNORM for the headset".
