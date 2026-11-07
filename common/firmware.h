/* Firmware-specific addresses, one profile per supported firmware. The payload picks the
 * profile for the running firmware and refuses any other. See docs/FIRMWARE-11.60.md.
 * SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include <stdint.h>

struct astro_lib {
  const char *name;
  unsigned long size;
  const char *sha256;
};
struct av_site {
  uint64_t va; /* 0: not known for this firmware yet */
  uint8_t len;
  uint8_t orig[8], patch[8];
};
struct astro_firmware {
  unsigned version;
  const char *name;
  struct astro_lib libs[5]; /* last entry validates the unmodified VideoOut */
  struct astro_lib hand;    /* libSceVrHand for hand tracking (optional: name 0 = none) */
  unsigned hmd_sdk_plt, hmd_shader_va;
  uint8_t hmd_sdk_orig[6];
  unsigned shader_src_dwords, shader_out_dwords;
  unsigned vo_layout, vo_lookup, vo_release;
  unsigned vo_flags[5]; /* 0: EnhancedAttribute sets no globals on this firmware */
  struct av_site av[2];
  int av_record; /* the AV service records the app as PS VR1 at launch: fix its record (main.c) */
};

static inline const struct astro_firmware *astro_firmware(unsigned version) {
  static const struct astro_firmware profiles[] = {
    {
      /* From HeadsetHero5.50's dumps. Before 9.00 sceVideoOutEnhancedAttribute sets only the
       * port's own fields, so there are no globals to put back (vo_flags 0). The AV service
       * here builds its checks differently: its layout gate is a short je made a jmp, and
       * the video-mode check returns 0 through the function's own stack-guard exit. */
      .version = 0x550, .name = "5.50",
      .libs = {
        {"libSceAgcDriver",139680,"2765597643c1bc6bac755b33c2fb00926b24057660521bd2bdb6510caeb18212"},
        {"libSceAgc",274792,"3e3f0c93d9958dc5b24b74801eb6bdde163ef352c1069d89be44f0360562264f"},
        {"libSceHmd2",1658520,"8cd7c8bdd1956e35b10b9aa4526f448c27942ba56ea46a16c41b98ad28ff58df"},
        {"libSceVrTracker2",1225420,"76b81e8baadbfd28b042d847484e8b2149b651c396c1b43c73bf5fe04abbb8f3"},
        {"libSceVideoOut",224524,"d77c0063198fd5ad74e3733852fa7a5c27c57f560b39cee110b9d1651ee6afaa"},
      },
      .hmd_sdk_plt = 0xf4f20, .hmd_shader_va = 0x16cf00,
      .hmd_sdk_orig = {0xff,0x25,0xa2,0x17,0x07,0},
      .shader_src_dwords = 4524, .shader_out_dwords = 4943,
      .vo_layout = 0x314e4, .vo_lookup = 0xe620, .vo_release = 0xe5e0,
      .av = {
        {0x40d959,2,{0x74,0x54},{0xeb,0x54}},
        {0x429744,5,{0x48,0x89,0xdf,0x4c,0x89},{0x45,0x31,0xe4,0xeb,0x1b}},
      },
      .av_record = 1,
    },
    {
      /* From tvk-source's dumps (the AV control service from the payload's own dump). Its AV
       * service is built like 5.50's: the layout gate is a short je made a jmp, and the
       * video-mode check clears its result (r12d) and leaves through the error path that loads
       * the stack-guard pointer (r14) its exit reads. */
      .version = 0x761, .name = "7.61",
      .libs = {
        {"libSceAgcDriver",140432,"22783082a1b322273e080bd1a0aed45d27fa658956d25dc3a07fcdee4029aa38"},
        {"libSceAgc",308376,"615bfad2eb70b6b31d4fae144999215bad899d4de932d2b2bf0cc19185f6a2ee"},
        {"libSceHmd2",1794280,"587b6038c0b511b2cefe037a2ac1789e9206aa63dd5387440c5cb1b3c5869226"},
        {"libSceVrTracker2",1350876,"d0f35daf457be5071a7a9d9bbfc5acffc57e50d5d0c36762abc0604b762d4cc6"},
        {"libSceVideoOut",242812,"97a82a7053d900f5c7fa2303c0503fd4a10607ce1ae91fce2e6ff0bf0c347b0e"},
      },
      .hmd_sdk_plt = 0x10d8a0, .hmd_shader_va = 0x189000,
      .hmd_sdk_orig = {0xff,0x25,0xb2,0x4c,0x07,0},
      .shader_src_dwords = 3616, .shader_out_dwords = 3861,
      .vo_layout = 0x35624, .vo_lookup = 0xf990, .vo_release = 0xf950,
      .av = {
        {0x415129,2,{0x74,0x26},{0xeb,0x26}},
        {0x42c0c4,5,{0x48,0x89,0xdf,0x4c,0x89},{0x45,0x31,0xe4,0xeb,0x19}},
      },
      .av_record = 1,
    },
    {
      /* From Vorenu5's dumps (the AV control service from a second, decrypted dump). Its AV
       * service's checks are built as on 9.00: the layout gate is a short je made a jmp, and the
       * video-mode check clears its result (r15d) and leaves through an error path that loads the
       * stack-guard pointer (rbx) its exit reads. */
      .version = 0x840, .name = "8.40",
      .libs = {
        {"libSceAgcDriver",140720,"fbb27d5a6bf1e8f29d5e511d1a64ddfbe466c0b40ca2ea380aa60a2abaac3297"},
        {"libSceAgc",306472,"8964b11acc6133d4c7d103706162fce59d47d7593346bc9e5f1bf4f41bde4793"},
        {"libSceHmd2",1854040,"4c33beea9b4d67cb9a908f1c475a1ebb8a78235bf1f1770129f79196b67004ca"},
        {"libSceVrTracker2",1351916,"b2b54ec55a633b824c65b3b2c02245c83f28e2ed86e1752c3bd92cb3dd6e400b"},
        {"libSceVideoOut",243068,"43aa871aae64f46dfe13fda001ab36f8f50e00ed992a226e629e25a3a670561a"},
      },
      .hmd_sdk_plt = 0x117460, .hmd_shader_va = 0x195000,
      .hmd_sdk_orig = {0xff,0x25,0xf2,0x70,0x07,0},
      .shader_src_dwords = 3526, .shader_out_dwords = 3878,
      .vo_layout = 0x35664, .vo_lookup = 0xfb50, .vo_release = 0xfb10,
      .av = {
        {0x4156cd,2,{0x74,0x2e},{0xeb,0x2e}},
        {0x42c6e4,5,{0x4c,0x89,0xef,0x4c,0x89},{0x45,0x31,0xff,0xeb,0x16}},
      },
      .av_record = 1,
    },
    {
      /* From HAKIM's dumps. Hmd2 loads the same reprojection shader as 8.40 and 11.x (same
       * lengths). sceVideoOutEnhancedAttribute sets three globals here, two dwords and a byte, so
       * the byte is listed for all three byte slots. The AV service's checks are built as on 5.50:
       * the layout gate is a short je made a jmp, and the video-mode check clears its result
       * (r15d) and leaves through an error path that loads the stack-guard pointer (rbx) its exit
       * reads. */
      .version = 0x900, .name = "9.00",
      .libs = {
        {"libSceAgcDriver",141248,"03990b838c42e8963b1af126f358a70f3277ddefc5c55dec30e367e2b56a2f03"},
        {"libSceAgc",366488,"b6dd6cb84a3d0f86d38102fbc13d149086be4344e08b887c60769c822ea11782"},
        {"libSceHmd2",1891240,"699a65b251c760fdb088987922e1700cb9f088da677f4ea6e42bb655b15fd357"},
        {"libSceVrTracker2",1352844,"2e939520bcc8238408a6cbf8c49cf2a1134d3805c6aa2a66f2dbfd1f7463764c"},
        {"libSceVideoOut",244484,"155c462473dff342ee1aa8eabf1ea51017cda3133c30acc1f03cbf1970456ad2"},
      },
      .hmd_sdk_plt = 0x118490, .hmd_shader_va = 0x194f00,
      .hmd_sdk_orig = {0xff,0x25,0xc2,0x60,0x07,0},
      .shader_src_dwords = 3526, .shader_out_dwords = 3878,
      .vo_layout = 0x358c4, .vo_lookup = 0x102b0, .vo_release = 0x10270,
      .vo_flags = {0x30018,0x3001c,0x30078,0x30078,0x30078},
      .av = {
        {0x4167dd,2,{0x74,0x2e},{0xeb,0x2e}},
        {0x430964,5,{0x4c,0x89,0xef,0x4c,0x89},{0x45,0x31,0xff,0xeb,0x16}},
      },
      .av_record = 1,
    },
    {
      /* From marrinerwolverine's dumps. Every library has 9.00's size; every offset and both AV
       * sites match 9.00 byte for byte; only the library builds differ. Its libSceVrHand is
       * another, much larger build, not looked at yet, so no hand tracking. */
      .version = 0x960, .name = "9.60",
      .libs = {
        {"libSceAgcDriver",141248,"d452049f96dfb1a267e3ed4c5f6790dd386304efaf440e77240dfeb2cfe43bae"},
        {"libSceAgc",366488,"df486f18acaadaacebf8cee5c4fed7a94dcc8261be569659ba7c4a3992901a3b"},
        {"libSceHmd2",1891240,"846cd8b686457f4631e44c716a47e1d13d5edcf430b4db2ba3ec15e522ddbc87"},
        {"libSceVrTracker2",1352844,"1c5448a9f319123dab31ab4ece6ad6a4002f2a7d34d98360e5d17373297d17bc"},
        {"libSceVideoOut",244484,"d2c69d7ea0cc9a3c819264c51a5c9e09118e83103b1deb5b45eb34db91ba888f"},
      },
      .hmd_sdk_plt = 0x118490, .hmd_shader_va = 0x194f00,
      .hmd_sdk_orig = {0xff,0x25,0xc2,0x60,0x07,0},
      .shader_src_dwords = 3526, .shader_out_dwords = 3878,
      .vo_layout = 0x358c4, .vo_lookup = 0x102b0, .vo_release = 0x10270,
      .vo_flags = {0x30018,0x3001c,0x30078,0x30078,0x30078},
      .av = {
        {0x4167dd,2,{0x74,0x2e},{0xeb,0x2e}},
        {0x430964,5,{0x4c,0x89,0xef,0x4c,0x89},{0x45,0x31,0xff,0xeb,0x16}},
      },
      .av_record = 1,
    },
    {
      /* From starman's dumps (PS5 Pro). Hmd2 has 11.x's shader at 11.x's address; VideoOut's
       * enhanced attribute sets the same three globals as 9.00. The AV service's layout gate is
       * the short je of 9.00 (made a jmp) and its video-mode check is 11.x's (xor eax and a jump
       * to the exit). */
      .version = 0x1001, .name = "10.01",
      .libs = {
        {"libSceAgcDriver",174096,"7aa8b6ebf6cc981bc850ebe58e17b7cb95e16a3d98814d75b8409a717ae03cdf"},
        {"libSceAgc",366584,"6ac34d65e003e8b9d1c0dbf921fe205a42f566480fbee3d5b30061f5cbcbbf47"},
        {"libSceHmd2",1870744,"140604a0ea5483a688511c3796389fd72285e1ca0fb2b6a98b81ca053c721912"},
        {"libSceVrTracker2",1378332,"1cf3c43512126ef2bd1bfb0da848af24a82e045743cba0696696509ca546cb2b"},
        {"libSceVideoOut",244500,"2b21600219b829d98f2346c428ec60d48352d34c59b10f5ac9e9834971625b3a"},
      },
      .hmd_sdk_plt = 0x112fe0, .hmd_shader_va = 0x190f00,
      .hmd_sdk_orig = {0xff,0x25,0xda,0x73,0x07,0},
      .shader_src_dwords = 3526, .shader_out_dwords = 3878,
      .vo_layout = 0x358c4, .vo_lookup = 0x10450, .vo_release = 0x10410,
      .vo_flags = {0x30018,0x3001c,0x30078,0x30078,0x30078},
      .av = {
        {0x417532,2,{0x74,0x2e},{0xeb,0x2e}},
        {0x431b98,7,{0x4c,0x89,0xe7,0x4c,0x89,0xfe,0xe8},{0x31,0xc0,0xe9,0x85,0,0,0}},
      },
      .av_record = 1,
    },
    {
      /* From GametimeUK's dumps. Every offset and both AV sites match 10.01 byte for byte; only
       * the library builds differ. */
      .version = 0x1020, .name = "10.20",
      .libs = {
        {"libSceAgcDriver",174096,"fc032762c63dea429771a9d50b28136328fb73b32a9f2d0fd430e19c766eda4f"},
        {"libSceAgc",366584,"4a8e237cfb418a4046603c5ef327f6e4ecf18ef82932c01f1e58bb782f233aab"},
        {"libSceHmd2",1870744,"6ffd3e348b77985a5c086d0231c76ae69c04f1ac962bfa19267582590d5d4989"},
        {"libSceVrTracker2",1378332,"d89cf984b3a1f0481d7a0992d7230900f0825be8b96b5414ee98f8b230f6c4b1"},
        {"libSceVideoOut",244500,"ec49cbb715c7b3a8abbdf0167ae3fb4c65968d411bb9c59206db2c768c7a3034"},
      },
      .hmd_sdk_plt = 0x112fe0, .hmd_shader_va = 0x190f00,
      .hmd_sdk_orig = {0xff,0x25,0xda,0x73,0x07,0},
      .shader_src_dwords = 3526, .shader_out_dwords = 3878,
      .vo_layout = 0x358c4, .vo_lookup = 0x10450, .vo_release = 0x10410,
      .vo_flags = {0x30018,0x3001c,0x30078,0x30078,0x30078},
      .av = {
        {0x417532,2,{0x74,0x2e},{0xeb,0x2e}},
        {0x431b98,7,{0x4c,0x89,0xe7,0x4c,0x89,0xfe,0xe8},{0x31,0xc0,0xe9,0x85,0,0,0}},
      },
      .av_record = 1,
    },
    {
      /* From Micael Azevedo's dumps. Same layout as 10.01 and 10.20 (every offset and both AV
       * sites checked); only the library builds differ. */
      .version = 0x1060, .name = "10.60",
      .libs = {
        {"libSceAgcDriver",174096,"985b4f7910e806defcb13966c783ce2abe347b1453d17d3076a3d215dd2d8e45"},
        {"libSceAgc",366584,"72c5367fbf73f46a593cc15a8f308f5b1334af35c840d4232e6cd44b38d43587"},
        {"libSceHmd2",1870744,"db68ffb98905b08534ffeb927d9317cc9cd46ba920c5cc61edd69e5b2222b64a"},
        {"libSceVrTracker2",1378332,"828a8201e40124e89a9cb0574f4b2b118982168d37bdf0218d93904d6e48ca91"},
        {"libSceVideoOut",244500,"8564bebf184a96e009dac0eadd628f8bce6973fd3203d6dcdefcb75e6b44b9a6"},
      },
      .hmd_sdk_plt = 0x112fe0, .hmd_shader_va = 0x190f00,
      .hmd_sdk_orig = {0xff,0x25,0xda,0x73,0x07,0},
      .shader_src_dwords = 3526, .shader_out_dwords = 3878,
      .vo_layout = 0x358c4, .vo_lookup = 0x10450, .vo_release = 0x10410,
      .vo_flags = {0x30018,0x3001c,0x30078,0x30078,0x30078},
      .av = {
        {0x417532,2,{0x74,0x2e},{0xeb,0x2e}},
        {0x431b98,7,{0x4c,0x89,0xe7,0x4c,0x89,0xfe,0xe8},{0x31,0xc0,0xe9,0x85,0,0,0}},
      },
      .av_record = 1,
    },
    {
      /* Same layout as 11.60 (every offset below checked in 11.40 dumps from scrdcow), only
       * the library builds differ. */
      .version = 0x1140, .name = "11.40",
      .libs = {
        {"libSceAgcDriver",174016,"bf34d63ead8c61f4ec999bd0ccebe83de6d242a86ee78ac9e8886ea135229dd4"},
        {"libSceAgc",366552,"17b03d5c5c103fd5d6734e727b353f0c8fa6f2d35eb576a9e23720ad7a4e90ad"},
        {"libSceHmd2",1870808,"c058b0e006a7da39e656cfdc8539f4f3e4ee6e02569a06fff88171453f4d86ef"},
        {"libSceVrTracker2",1378556,"4584bafd9a68d46024c9b0130be45ff3ba5d9c81a66bd3f36f9194bcc4334096"},
        {"libSceVideoOut",244964,"c6cb87eccbe0ed14ca9ef40870fe690036f9122ce395c4385bd86f496817397d"},
      },
      .hmd_sdk_plt = 0x112e00, .hmd_shader_va = 0x190f00,
      .hmd_sdk_orig = {0xff,0x25,0xba,0x75,0x07,0},
      .shader_src_dwords = 3526, .shader_out_dwords = 3878,
      .vo_layout = 0x358d4, .vo_lookup = 0x10170, .vo_release = 0x10130,
      .vo_flags = {0x3007c,0x30018,0x30080,0x30084,0x30088},
      .av = {
        {0x41733b,6,{0x0f,0x84,0xfd,0,0,0},{0x66,0x0f,0x1f,0x44,0,0}},
        {0x431f68,7,{0x4c,0x89,0xe7,0x4c,0x89,0xfe,0xe8},{0x31,0xc0,0xe9,0x85,0,0,0}},
      },
      .av_record = 1,
    },
    {
      .version = 0x1160, .name = "11.60",
      .libs = {
        {"libSceAgcDriver",174016,"8314b5d388445a3b9a23f787c1b752f84fd35887b9bb2af4def4d49c77bfab3c"},
        {"libSceAgc",366552,"39a64567eb79fc2006fbb58ba9c218fbfbb790630b8183c77caf7ff6508a3d44"},
        {"libSceHmd2",1870808,"67d16fd692cc19200bb37ed1aca8f76e1a55d4030d23d550c268fb63540f47cc"},
        {"libSceVrTracker2",1378556,"32417df5c8712769a0250a2d051e4720eb8863c3a9a8a81ee494875d590b1bf1"},
        {"libSceVideoOut",244964,"7d8628957d087962f851d779a0a58ee7149691bcbe00f3884cf1b0a67dd6f7a3"},
      },
      .hmd_sdk_plt = 0x112e00, .hmd_shader_va = 0x190f00,
      .hmd_sdk_orig = {0xff,0x25,0xba,0x75,0x07,0},
      .shader_src_dwords = 3526, .shader_out_dwords = 3878,
      .vo_layout = 0x358d4, .vo_lookup = 0x10170, .vo_release = 0x10130,
      .vo_flags = {0x3007c,0x30018,0x30080,0x30084,0x30088},
      .av = {
        {0x41733b,6,{0x0f,0x84,0xfd,0,0,0},{0x66,0x0f,0x1f,0x44,0,0}},
        {0x431f68,7,{0x4c,0x89,0xe7,0x4c,0x89,0xfe,0xe8},{0x31,0xc0,0xe9,0x85,0,0,0}},
      },
      .av_record = 1,
    },
    {
      /* From sagacity's dumps. Every offset and both AV sites match 12.20 and 12.40 byte for
       * byte; only the library builds differ. */
      .version = 0x1200, .name = "12.00",
      .libs = {
        {"libSceAgcDriver",174688,"37a1e102c590349b4d4d4867eb4db1f5b4b84dee3efa6dc80257ba00e147e8fa"},
        {"libSceAgc",366600,"26e58291f23e6a7fb63f4054c47f1a93704d3830b89530b0c060220b3ef7b89e"},
        {"libSceHmd2",853624,"13caf16b7bf77cc375f73fc32aa11cd60bcf980e26addf1d6c329b2a991ea377"},
        {"libSceVrTracker2",1411164,"e38650ac8ae1328f867ccafa9f86ef6c01cf83797746f4ff8ebd68d446f11f1c"},
        {"libSceVideoOut",244964,"cde01da0b70c56c456ce23ca34173d592e85ba815d5bcff9dec0e02049e57626"},
      },
      /* sagacity's dump: 13.60's size, its own build, the same 438 import and export NIDs */
      .hand = {"libSceVrHand",1169572,"91a8d332ccc973724155de4e084e894fbd0cbcbc3e2d02bee502481580220c55"},
      .hmd_sdk_plt = 0x67420, .hmd_shader_va = 0x9cf00,
      .hmd_sdk_orig = {0xff,0x25,0x92,0xce,0x02,0},
      .shader_src_dwords = 3526, .shader_out_dwords = 3878,
      .vo_layout = 0x358d4, .vo_lookup = 0x10470, .vo_release = 0x10430,
      .vo_flags = {0x3007c,0x30018,0x30080,0x30084,0x30088},
      .av = {
        {0x41735b,6,{0x0f,0x84,0xfd,0,0,0},{0x66,0x0f,0x1f,0x44,0,0}},
        {0x431eb8,7,{0x4c,0x89,0xe7,0x4c,0x89,0xfe,0xe8},{0x31,0xc0,0xe9,0x85,0,0,0}},
      },
      .av_record = 1,
    },
    {
      /* From Vince559's dumps. Same layout as 12.00 and 12.20 (every offset and both AV sites
       * checked); only the library builds differ. No libSceVrHand dump yet. */
      .version = 0x1202, .name = "12.02",
      .libs = {
        {"libSceAgcDriver",174688,"2c45cb5861a1c4559e21e88e93a8516a2553787855c1561adf7b3c25beb55208"},
        {"libSceAgc",366600,"30f79a9fc0ae9a26ab9805f7255822903d06d6e131d7bcee9e5bd8eaa148deac"},
        {"libSceHmd2",853624,"3f113fcf2d4fae149d6b71570b71d7fbc12f846d4b90d464077afebc8b2fc0f7"},
        {"libSceVrTracker2",1411164,"d7449f77a777bc03f55381615b1a89f32c9fc2e00499b4ac303c0919157e88d4"},
        {"libSceVideoOut",244964,"521f8b5d286be18cc20a64e000d77465b195c050e6936136c6518dc95fbd999a"},
      },
      /* From Ghost-VR (Vince559's dump, Discord 11-04): VrHand's "12.0.0.0 build 376" with 13.60's size,
       * the same 427 imports and 13 exports as 13.60's; its shaders from the console's own models */
      .hand = {"libSceVrHand",1169572,"e286e08d10e6cbf65a7bc645ea59afbe07a4032616b954fb5dba9f32aa29c51e"},
      .hmd_sdk_plt = 0x67420, .hmd_shader_va = 0x9cf00,
      .hmd_sdk_orig = {0xff,0x25,0x92,0xce,0x02,0},
      .shader_src_dwords = 3526, .shader_out_dwords = 3878,
      .vo_layout = 0x358d4, .vo_lookup = 0x10470, .vo_release = 0x10430,
      .vo_flags = {0x3007c,0x30018,0x30080,0x30084,0x30088},
      .av = {
        {0x41735b,6,{0x0f,0x84,0xfd,0,0,0},{0x66,0x0f,0x1f,0x44,0,0}},
        {0x431eb8,7,{0x4c,0x89,0xe7,0x4c,0x89,0xfe,0xe8},{0x31,0xc0,0xe9,0x85,0,0,0}},
      },
      .av_record = 1,
    },
    {
      /* From Und3adShr3d's dumps. Every offset and both AV sites match 12.40 byte for byte; only
       * the library builds differ. No libSceVrHand dump yet, so no hand tracking. */
      .version = 0x1220, .name = "12.20",
      .libs = {
        {"libSceAgcDriver",174688,"22ce3a3bf5e7d4e106ce9e601cd7e756bc8631262e1c9f839d692d366acb4a5e"},
        {"libSceAgc",366600,"c1b1b49cc211420587f950b7194c46fe63873209f44b52f7bc900748d81ed520"},
        {"libSceHmd2",853624,"767cb7c0181b48764a89d7e483b63f349d567a1ba5b67c0d4bdfb53ce1b3b033"},
        {"libSceVrTracker2",1411164,"552ddb60df7f704cab4153f5ab659a9500bbd1b0780b6d9e91c75d37b89fd60a"},
        {"libSceVideoOut",244964,"728fbabef7356b1200f5752da554a3c2e11328082d112908fcfa9b10d3c03c8e"},
      },
      .hmd_sdk_plt = 0x67420, .hmd_shader_va = 0x9cf00,
      .hmd_sdk_orig = {0xff,0x25,0x92,0xce,0x02,0},
      .shader_src_dwords = 3526, .shader_out_dwords = 3878,
      .vo_layout = 0x358d4, .vo_lookup = 0x10470, .vo_release = 0x10430,
      .vo_flags = {0x3007c,0x30018,0x30080,0x30084,0x30088},
      .av = {
        {0x41735b,6,{0x0f,0x84,0xfd,0,0,0},{0x66,0x0f,0x1f,0x44,0,0}},
        {0x431eb8,7,{0x4c,0x89,0xe7,0x4c,0x89,0xfe,0xe8},{0x31,0xc0,0xe9,0x85,0,0,0}},
      },
      .av_record = 1,
    },
    {
      /* Hmd2 has 13.60's layout but 11.60's reprojection shader; the SDK stub (found through
       * its JMP_SLOT, same GOT slot 0x942b8 as 13.60), VideoOut's port lookup/release (from
       * the enhanced-attribute export) and both AV sites (each found by its log string) moved.
       * The AV service records the game as PS VR1, as on 11.60, so av_record is set. */
      .version = 0x1240, .name = "12.40",
      .libs = {
        {"libSceAgcDriver",174688,"b942a5d9c9100a6d8c009b75792d60ec410c98b0d8f37afcf337376ade3008f3"},
        {"libSceAgc",366600,"2b9a2503287ebc3d1a9b4cdfe3371168e4ebc31bdd85b5e60e4cd6ced08f2867"},
        {"libSceHmd2",853624,"50da7251eb3e566218139bd3a2554fc51c1aff4c82e2c5cf6e54c64e4e0c50fa"},
        {"libSceVrTracker2",1411164,"58d9303ed39faf9b559cd31e4d61337ac13eb1e6db22919037f02a23d71a841c"},
        {"libSceVideoOut",244964,"73fe9115eb261422fff8c72b4649e4ace3d7952e8bbf39d2a8de1075f9f8ff50"},
      },
      /* 13.60's size, its own build: the five sceVrHand exports and every import hand.c and
       * ps5compat.c hook are there, and its shaders and models' are version 0x18 */
      .hand = {"libSceVrHand",1169572,"02aa859dc7c91d1433f2b44e8bc248f0839774c90a36c950fb80f2ae60e55e4f"},
      .hmd_sdk_plt = 0x67420, .hmd_shader_va = 0x9cf00,
      .hmd_sdk_orig = {0xff,0x25,0x92,0xce,0x02,0},
      .shader_src_dwords = 3526, .shader_out_dwords = 3878,
      .vo_layout = 0x358d4, .vo_lookup = 0x10470, .vo_release = 0x10430,
      .vo_flags = {0x3007c,0x30018,0x30080,0x30084,0x30088},
      .av = {
        {0x41735b,6,{0x0f,0x84,0xfd,0,0,0},{0x66,0x0f,0x1f,0x44,0,0}},
        {0x431eb8,7,{0x4c,0x89,0xe7,0x4c,0x89,0xfe,0xe8},{0x31,0xc0,0xe9,0x85,0,0,0}},
      },
      .av_record = 1,
    },
    {
      /* From deadhead's dumps. Every offset and both AV sites match 13.20 and 13.60 byte for
       * byte; only the library builds differ. No libSceVrHand dump yet. */
      .version = 0x1300, .name = "13.00",
      .libs = {
        {"libSceAgcDriver",174736,"6f1736fce45fbbceeda58bd2ff472567ec43e6a013680ad984abb9c2d0c629b1"},
        {"libSceAgc",383048,"35d0faf8f783124ab08540c365b981275c0fc6489879492050379aa6a9ef6286"},
        {"libSceHmd2",853624,"8c4619b9b9b3a9740fbb5b5d2ab7e3c249ba757da18522cac2a5f6e79b8ba9bb"},
        {"libSceVrTracker2",1411164,"68c93be463b75069c4b8ee3c64dec11df067520af6d0544246ce2c9bdfa09b77"},
        {"libSceVideoOut",244964,"3e5539c91df22b2290eff8e31a60b3d9dbaf2a7a65381cd45db2cfb2e8735860"},
      },
      /* From Ghost-VR (fwadfthy's dump, Discord 11-06): 13.60's build with its strings moved (1697
       * bytes differ, all rip-relative string addresses); the same imports and exports */
      .hand = {"libSceVrHand",1169572,"eeb0f5d7a3f7978db239b0e44241c88c0d4fbb8430e25d412a2a836b01fc7cb4"},
      .hmd_sdk_plt = 0x67a30, .hmd_shader_va = 0x9cf00,
      .hmd_sdk_orig = {0xff,0x25,0x82,0xc8,0x02,0},
      .shader_src_dwords = 3501, .shader_out_dwords = 3853,
      .vo_layout = 0x358d4, .vo_lookup = 0x10400, .vo_release = 0x103c0,
      .vo_flags = {0x3007c,0x30018,0x30080,0x30084,0x30088},
      .av = {
        {0x41739b,6,{0x0f,0x84,0xfd,0,0,0},{0x66,0x0f,0x1f,0x44,0,0}},
        {0x431ed8,7,{0x4c,0x89,0xe7,0x4c,0x89,0xfe,0xe8},{0x31,0xc0,0xe9,0x85,0,0,0}},
      },
      .av_record = 1,
    },
    {
      /* From Izzysin's dumps. Every offset and both AV sites match 13.60 byte for byte; only the
       * library builds differ. av_record is set in case its AV service records the game as PS VR1
       * like 12.x (the check finds nothing to change otherwise). */
      .version = 0x1320, .name = "13.20",
      .libs = {
        {"libSceAgcDriver",174736,"88bc6eccae865dd06ca48b29a11c4528f2a7ec124f05349151977d1d01a21253"},
        {"libSceAgc",383048,"deaedd02cbd0df28bdbf3c5c536ccbe19d9e4d9a4b8df844c7ca1b3d48b1f664"},
        {"libSceHmd2",853624,"13b7d3dc068a963936d1692e094ac993bd52602fb5f146a5340b0f0f20775925"},
        {"libSceVrTracker2",1411164,"02bbe0428bfcf7534491dd3b468156b8c5ce09d26f8addbd7b4fa61ab5e724f1"},
        {"libSceVideoOut",244964,"b3de5201f21ed58ae244bcb3e43a411f34626cf7d665286ce3440d103cf54bc8"},
      },
      /* Izzysin's dump: 13.60's size, its own build, the same 438 import and export NIDs */
      .hand = {"libSceVrHand",1169572,"d91547dc56c719d80be06ef11efb8e5d2f7091bb2512c10b4e85948873ce3aa9"},
      .hmd_sdk_plt = 0x67a30, .hmd_shader_va = 0x9cf00,
      .hmd_sdk_orig = {0xff,0x25,0x82,0xc8,0x02,0},
      .shader_src_dwords = 3501, .shader_out_dwords = 3853,
      .vo_layout = 0x358d4, .vo_lookup = 0x10400, .vo_release = 0x103c0,
      .vo_flags = {0x3007c,0x30018,0x30080,0x30084,0x30088},
      .av = {
        {0x41739b,6,{0x0f,0x84,0xfd,0,0,0},{0x66,0x0f,0x1f,0x44,0,0}},
        {0x431ed8,7,{0x4c,0x89,0xe7,0x4c,0x89,0xfe,0xe8},{0x31,0xc0,0xe9,0x85,0,0,0}},
      },
      .av_record = 1,
    },
    {
      /* From damimimi's dumps. Every offset and both AV sites match 13.60 byte for byte; only
       * the library builds differ. */
      .version = 0x1340, .name = "13.40",
      .libs = {
        {"libSceAgcDriver",174736,"1d3c11ad4223ef7a405a3a643996799d07f7b287af41e1e208ccab242965b725"},
        {"libSceAgc",383048,"951b7d52952523acd2e49603ad0b5d4aa25d5b267a5bbdca18ad0bba2c581724"},
        {"libSceHmd2",853624,"fc36c37a5ac0e6cd69ba39d43af2870ad5366ad595687f2ad2c60c1463049e9d"},
        {"libSceVrTracker2",1411164,"815694d038f752c3c22b6b8aff09681e026d82de4f56512733dc1335545a4c2a"},
        {"libSceVideoOut",244964,"51036dd873f8c6f6a63ff45557e454277db0f53d511f2605b2640f5125caf018"},
      },
      /* damimimi's dump: 13.60's size, its own build, the same 438 import and export NIDs */
      .hand = {"libSceVrHand",1169572,"79f6fbd5cfde548d18d71134c8d3bb544192a3eb540a1fe826f6ed734c7ccfac"},
      .hmd_sdk_plt = 0x67a30, .hmd_shader_va = 0x9cf00,
      .hmd_sdk_orig = {0xff,0x25,0x82,0xc8,0x02,0},
      .shader_src_dwords = 3501, .shader_out_dwords = 3853,
      .vo_layout = 0x358d4, .vo_lookup = 0x10400, .vo_release = 0x103c0,
      .vo_flags = {0x3007c,0x30018,0x30080,0x30084,0x30088},
      .av = {
        {0x41739b,6,{0x0f,0x84,0xfd,0,0,0},{0x66,0x0f,0x1f,0x44,0,0}},
        {0x431ed8,7,{0x4c,0x89,0xe7,0x4c,0x89,0xfe,0xe8},{0x31,0xc0,0xe9,0x85,0,0,0}},
      },
      .av_record = 1,
    },
    {
      /* From js's dumps. Every offset and both AV sites match 13.60 byte for byte; only the
       * library builds differ. av_record is set as on 13.20 (the check finds nothing to change
       * if this AV service doesn't record the game as PS VR1). */
      .version = 0x1342, .name = "13.42",
      .libs = {
        {"libSceAgcDriver",174736,"78d5ad5fb70cc874b88841035218963a063199c4513607fa8f87242b66b6277b"},
        {"libSceAgc",383048,"6bae17e07ce4661d306009cc3d83e8524e86a7e8123864340874d5acb2420601"},
        {"libSceHmd2",853624,"823f9556c32ff7743032ae43007e7adda54c640662a4e0b752be0f33acb7fe21"},
        {"libSceVrTracker2",1411164,"9021b8d6d5e01135e96b125e414e97e7ccc81006c3f64c459ad79daa377893ec"},
        {"libSceVideoOut",244964,"975b229324655a2d2a945c755fc0339e7feecb62863efc28ad3b2d91a95ad215"},
      },
      /* js's dump: 13.60's size, its own build, the same import and export NIDs */
      .hand = {"libSceVrHand",1169572,"8d211358c9e9a632d7289d7a5dcbb73ec3193a734cea20caa7ee4111d7295dcc"},
      .hmd_sdk_plt = 0x67a30, .hmd_shader_va = 0x9cf00,
      .hmd_sdk_orig = {0xff,0x25,0x82,0xc8,0x02,0},
      .shader_src_dwords = 3501, .shader_out_dwords = 3853,
      .vo_layout = 0x358d4, .vo_lookup = 0x10400, .vo_release = 0x103c0,
      .vo_flags = {0x3007c,0x30018,0x30080,0x30084,0x30088},
      .av = {
        {0x41739b,6,{0x0f,0x84,0xfd,0,0,0},{0x66,0x0f,0x1f,0x44,0,0}},
        {0x431ed8,7,{0x4c,0x89,0xe7,0x4c,0x89,0xfe,0xe8},{0x31,0xc0,0xe9,0x85,0,0,0}},
      },
      .av_record = 1,
    },
    {
      .version = 0x1360, .name = "13.60",
      .libs = {
        {"libSceAgcDriver",174736,"1e0d8e258364c715672a73955fca24c113d5019c0230be1b723084631b81b0cd"},
        {"libSceAgc",383048,"586444df9ee65eed7b084cd665103bca12ad9eca7c08faa2ad17fa758f3c97c5"},
        {"libSceHmd2",853624,"8279dd7bbb3b6ed1a328ab6db08a4d88eb6d428c237ba6834c109f57af5cfbcf"},
        {"libSceVrTracker2",1411164,"dfa09f5177d510dba3fda2beb9c8ee0efcda29205bea02ea0d3fcd8e6ad9b611"},
        {0,0,0}, /* upstream did not record VideoOut's hash */
      },
      .hand = {"libSceVrHand",1169572,"948fe5defe51eabd053e40e82abcee48ebdc3eb700dd961e247b03af4efae395"},
      .hmd_sdk_plt = 0x67a30, .hmd_shader_va = 0x9cf00,
      .hmd_sdk_orig = {0xff,0x25,0x82,0xc8,0x02,0},
      .shader_src_dwords = 3501, .shader_out_dwords = 3853,
      .vo_layout = 0x358d4, .vo_lookup = 0x10400, .vo_release = 0x103c0,
      .vo_flags = {0x3007c,0x30018,0x30080,0x30084,0x30088},
      .av = {
        {0x41739b,6,{0x0f,0x84,0xfd,0,0,0},{0x66,0x0f,0x1f,0x44,0,0}},
        {0x431ed8,7,{0x4c,0x89,0xe7,0x4c,0x89,0xfe,0xe8},{0x31,0xc0,0xe9,0x85,0,0,0}},
      },
    },
  };
  for (unsigned i = 0; i < sizeof profiles / sizeof profiles[0]; ++i)
    if (profiles[i].version == version)
      return &profiles[i];
  return 0;
}
