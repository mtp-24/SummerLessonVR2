# Firmware profiles

Everything that depends on the firmware is in one place, `common/firmware.h`:
a profile per firmware, picked by the payload at start. Any other firmware is
refused. There are 22: 5.50, 7.40, 7.61, 8.40, 9.00, 9.60, 10.01, 10.20, 10.60, 11.40,
11.60, 12.00, 12.02, 12.20, 12.40, 12.60, 12.70, 13.00, 13.20, 13.40, 13.42 and 13.60.

**What is tested with this port:** only 12.40 has run Summer Lesson (the console
this port was developed on). 20 of the other 21 profiles are carried over from
the projects credited in the README, where they ran their own games or were
worked out from dumps people sent; 12.70 was made here from a 12.70 console's
files (below); none of the 21 has been tried with Summer Lesson. The profiles
were worked out on 13.60 and 11.60 first, and the notes below are organised
around those two; the walkthrough in [HOW-IT-WORKS.md](HOW-IT-WORKS.md) gives
12.40's offsets.

11.40 is the same as 11.60 here: its libraries are different builds (their own
hashes in the profile) but every offset in the 11.60 profile matches, checked in
11.40 dumps of the five libraries and the AV control service.

12.40 mixes the two: libSceHmd2 has 13.60's size and layout but 11.60's
reprojection shader. Its AV service records the game as PS VR1, so the AV record
fix below runs as on 11.60. Its offsets are in the table below, each found in
12.40 dumps rather than carried over.

A profile holds:

| | 11.60 | 12.40 | 13.60 |
|---|---|---|---|
| libSceHmd2 SDK-version PLT stub | `0x112e00` | `0x67420` | `0x67a30` |
| Hmd2 reprojection shader (RDNA dwords -> GCN dwords) | `0x190f00` (3526 -> 3878) | `0x9cf00` (3526 -> 3878) | `0x9cf00` (3501 -> 3853) |
| libSceVideoOut port lookup / release | `0x10170` / `0x10130` | `0x10470` / `0x10430` | `0x10400` / `0x103c0` |
| libSceVideoOut layout permission, enhanced-attribute flags | same as 13.60 | same as 13.60 | `0x358d4`, `0x3007c` ... |
| AV service layout-handler branch | `0x41733b` | `0x41735b` | `0x41739b` |
| AV service `checkVideoModeForChangeOutput` exit | `0x431f68` | `0x431eb8` | `0x431ed8` |

and the size and SHA-256 of each library on that firmware (libSceVrHand's too,
where the profile has one: hand tracking needs it, [LIMITS.md](LIMITS.md)). On
11.60 and 12.40 libSceVideoOut's hash is checked too, since the agent writes into
it. The AV patches check the original bytes before writing.

The prepared libraries in `/data/summerlessonvr2/libs` are tagged with the
firmware (`/data/summerlessonvr2/prepared`, the cache's format version in the
tag), so they are rebuilt once after a firmware change or a change of the cache's
format. libSceAgcDriver's command-buffer base (the one `movabs` the driver reads
it from) is found by its bytes on any firmware, with no profile field
([SUMMER-LESSON.md](SUMMER-LESSON.md), run 5).

The agent learns the firmware through its header: the padding word after
`ipd_um` holds it.

## Older firmware: 5.50, 7.40, 7.61, 8.40, 9.00, 10.01

The same profile, worked out from dumps people sent to the original port. 5.50,
7.61 and 9.00 have run a game there (reported by the people who sent them); 8.40
and 10.01 haven't; none has run Summer Lesson.

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
- 12.70 (from a 12.70 console's eight decrypted files, every one 12.40's size)
  matches 12.40 the same way, and more closely: its libSceHmd2 and
  libSceAgcDriver are 12.40's code with only the SDK version, build path and
  build id changed (42 and 28 bytes), so the SDK stub (its JMP_SLOT) and the
  reprojection shader (the same 3526 dwords; the translator, run on the host,
  gives the same 3878) are 12.40's. libSceVideoOut's offsets were found anew
  from its enhanced-attribute export (the five flags it sets, the port lookup
  and release it calls) and the attribute3 `0x400` cache. Both AV sites hold
  12.40's bytes inside the functions their log strings name, and the layout
  handler still tests the PS VR1 flag, so the AV record fix stays on. Its
  libSceVrHand is 12.40's build with one string moved (1648 bytes: the string
  pool, the rip-relative addresses into it, the SDK version and build id); the
  same 439 symbols at the same addresses. Not run with this port.
- 13.42, 13.40, 13.20 and 13.00 match 13.60 the same way.
- 12.60 has 13.40's layout: the same shader address, VideoOut layout and flags, and both AV
  sites 0x40 and 0x20 earlier with the same bytes around them; its own Hmd2 SDK stub, VideoOut
  lookup and release, and a 3526-dword Hmd2 shader as 9.60's. Its libSceVrHand has 13.40's
  size and the same 438 NIDs, so hand tracking is on. 12.70's profile (made here from 12.40's
  files) matches 12.60's at every offset and AV site, as the original port's own 12.70 does.
- 7.40 matches 7.61 byte for byte at every offset, the Hmd2 shader and both AV sites; only the
  library builds differ. No libSceVrHand dump: no hand tracking.
- 9.60 matches 9.00 byte for byte at every offset and both AV sites; only the
  library builds differ.
- 10.20 and 10.60 match 10.01 byte for byte at every offset and both AV sites;
  only the library builds differ.
- 7.40, 9.60, 12.00, 12.02, 12.60 and 13.40 came from later releases of the original port
  (and Ghost-VR 0.9.4-0.9.8), with the hand tracker's library for 12.00,
  12.60 and 13.40; Ghost-VR 0.9.6 adds it for 12.02 and 13.00 (13.60's size and imports);
  12.70's is in its profile here.
  Hand tracking on 10.01 and 10.20 needs more than a profile (an older
  libSceVrHand: its shaders inside it, `buffer_atomic_cmpswap`, a wave32 VCCZ fix
  in the translator, the unsized `sceKernelGetAppInfo`): not here.
- 8.40's AV service is built like 9.00's (`xor r15d, r15d` and a short jump to
  the error path that loads `rbx`). Its first dump came encrypted, so the
  profile had no AV sites until a decrypted copy came in.
- On any firmware without a profile, the payload doesn't start: it decrypts the
  five libraries and `/SceSysAvControl.elf` into
  `/data/summerlessonvr2/firmware-<version>` (for example `firmware-12.40`) and
  says so in a notification, so the files can be sent over.

## The game's image base

On 13.60 a game's `eboot.bin` was always mapped at `0x400000`. On 11.60 it can
be mapped elsewhere (seen at `0x43be8000`), and a build check at a fixed
address then reads zeros and reports an unknown build. The payload takes the
base from the loader's record for the game (dynlib handle 0) for the build
check, the import slots, the entry breakpoint and the agent's own reads of the
game's statics (`payload/main.c` `eboot_mapbase`).

## The AV service's app record

On 11.60 the game ended up on the cinematic floating screen in the headset,
although every Hmd2 and VideoOut call succeeded. The kernel log shows why. The AV
control service keeps a record of each app, with flags that include its VR
type, and builds it once, when the app launches. In those flags `0x200000`
means a PS VR1 title and `0x10` a PS VR2 title. A PS4 PS VR game is recorded as
PS VR1 (`VR:1` in the service's `-- app[...]` lines). The attribute3 bit the
payload sets comes too late: the service doesn't read it again. With a PS VR1
app in front, the service keeps the headset on the single-output USB layout
(cinematic) and never switches to the dual layout (`HDMI_USB`, "2HEADS") that
PS VR2 games get.

So, once the game runs, the payload finds the game's record in the service's
memory (pid, appid, flags) and makes it a PS VR2 record: `0x200000` cleared,
`0x10` set (`payload/main.c` `av_mark_vr2_record`). The service keeps two copies
of the record, and nothing is written unless every match is the same record. The
record goes away with the game. The kernel log then shows
`layout:USB -> HDMI_USB`, `vrType(2)` and `gen2vr=True`, as for a PS VR2 game.
This runs on every profile but 13.60's (`av_record`); on 12.40 it is what puts
Summer Lesson on the dual layout. On 13.60 marking attribute3 was already enough,
so that profile skips it.

## Brightness

Not firmware-specific: the game's eye textures are sRGB, and sampled as sRGB
(decoded to linear) by Hmd2 the headset picture was much too dark. The agent
gives Hmd2 their descriptors as UNORM (`agent/gen1.c`), and the TV mirror reads
them the same way (`agent/mirror.c`).
