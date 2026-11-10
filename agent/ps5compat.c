/* The PS5 VR libraries (AgcDriver, Agc, Hmd2, VrTracker2) in a PS4 process. Each fix is an
 * import of one library pointed at a wrapper here; the game's own view of the system stays as it
 * was. Offsets into libSceVideoOut come from the firmware profile the injector selected
 * (common/firmware.h).
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include "vr2.h"
#include "../common/firmware.h"

/* ---------------------------------------------------------------- PS5-side threads */
/* VideoOut refuses a PS4 process the native output bus Hmd2 opens (0x200) and the PS5 pixel
 * formats it registers. Its sceKernelIsPs4Process and get_authinfo imports answer "PS5" on the
 * threads the PS5 libraries run on (the VR thread and every thread they create); the game's
 * threads and its own TV port keep the real answers. */
static void *volatile g_ps5_threads[48];
static volatile u32 g_nps5;

void ps5compat_mark_thread(void) {
  u32 i = __atomic_fetch_add(&g_nps5, 1, __ATOMIC_SEQ_CST);
  if (i < 48)
    g_ps5_threads[i] = scePthreadSelf();
}

static int is_ps4_for_thread(void) {
  void *self = scePthreadSelf();
  for (u32 i = 0; i < g_nps5 && i < 48; i++)
    if (g_ps5_threads[i] == self)
      return 0;
  return sceKernelIsPs4Process();
}

/* authinfo capability bit 62 gives a port the PS5 pixel formats (VideoOut sub_1c7e0) */
static int authinfo_for_thread(int pid, void *info) {
  int rc = get_authinfo(pid, info);
  if (rc == 0 && info && !is_ps4_for_thread())
    *(u64 *)((u8 *)info + 8) |= 1ull << 62;
  return rc;
}

static struct {
  void *(*entry)(void *);
  void *arg;
} g_tramp[48];
static volatile u32 g_ntramp;

static void *ps5_thread_entry(void *p) {
  ps5compat_mark_thread();
  return g_tramp[(u64)p].entry(g_tramp[(u64)p].arg);
}

/* Thread attributes a PS5 library sets that a PS4 process may refuse: failures are logged. */
static int attr_affinity(void *attr, u64 mask) {
  int rc = scePthreadAttrSetaffinity(attr, mask);
  if (rc != 0)
    LOG("ps5 thread: affinity 0x%lx refused (0x%x)", mask, rc);
  return rc;
}
static int attr_sched(void *attr, const s32 *param) {
  int rc = scePthreadAttrSetschedparam(attr, param);
  if (rc != 0)
    LOG("ps5 thread: priority %d refused (0x%x)", param ? *param : -1, rc);
  return rc;
}
static int attr_inherit(void *attr, int inherit) {
  int rc = scePthreadAttrSetinheritsched(attr, inherit);
  if (rc != 0)
    LOG("ps5 thread: inheritsched %d refused (0x%x)", inherit, rc);
  return rc;
}

/* VrHand's one-time GPU toolkit setup (its sub_769e0): Agc's init for toolkit version 0xd answers
 * 0x8a6c001f (shader already made) because Hmd2 ran the same init in this Agc first, so the
 * toolkit's shaders are there: that is success. */
static int hand_mprotect(void *addr, size_t len, int prot) {
  int rc = sceKernelMprotect(addr, len, prot);
  if (rc != 0)
    LOG("hand: mprotect(%p, 0x%lx, 0x%x) -> 0x%x", addr, (unsigned long)len, prot, rc);
  return rc;
}
static int hand_tk_a(void *p, u32 v) {
  int rc = sceAgcToolkitA ? sceAgcToolkitA(p, v) : -1;
  if ((u32)rc == 0x8a6c001f)
    rc = 0;
  if (rc != 0)
    LOG("hand: Agc toolkit init (0x%x) -> 0x%x", v, rc);
  return rc;
}
static int hand_tk_b(void *p) {
  int rc = sceAgcToolkitB ? sceAgcToolkitB(p) : -1;
  if (rc != 0)
    LOG("hand: Agc toolkit setup -> 0x%x", rc);
  return rc;
}

static int ps5_pthread_create(void *thr, const void *attr, void *(*entry)(void *), void *arg,
                              const char *name) {
  u32 i = __atomic_fetch_add(&g_ntramp, 1, __ATOMIC_SEQ_CST);
  if (i >= 48)
    return scePthreadCreate(thr, attr, entry, arg, name);
  g_tramp[i].entry = entry;
  g_tramp[i].arg = arg;
  int rc = scePthreadCreate(thr, attr, ps5_thread_entry, (void *)(u64)i, name);
  if (rc != 0)
    LOG("ps5 thread: %s not created (0x%x)", name ? name : "?", rc);
  return rc;
}

/* ---------------------------------------------------------------- app info, status, SDK */
/* Hmd2 refuses an app whose param.json attribute3 lacks 0x400 (PS VR2); a PS4 title has none. */
static int appinfo_with_vr2(int pid, u8 *info) {
  int rc = sceKernelGetAppInfo(pid, info);
  if (rc == 0 && info) {
    *(u32 *)(info + 0x4c) |= 0x400;
    if (info[0x4b] == 1)
      info[0x4b] = 0;
  }
  return rc;
}

/* VrHand reads the app info through libkernel's sized variant (d7xu4QDJw8Q, 0x98 bytes) and refuses
 * an app without attribute3 0x04000000 (hand tracking) with 0x8a87001e. */
static int appinfo_with_hands(int pid, u8 *info, size_t size) {
  int rc = sceKernelGetAppInfoSized(pid, info, size);
  if (rc == 0 && info && size >= 0x50) {
    *(u32 *)(info + 0x4c) |= 0x04000400;
    if (info[0x4b] == 1)
      info[0x4b] = 0;
  }
  return rc;
}

/* With the headset in VR mode the system reports this PS4 title as running in the background
 * (status byte 5), and Hmd2 then shows black. Hmd2's view clears it. */
static int status_foreground(u8 *st) {
  int rc = sceSystemServiceGetStatus ? sceSystemServiceGetStatus(st) : -1;
  if (rc == 0 && st)
    st[5] = 0;
  return rc;
}

/* A PS4 process has no PS5 SDK version, and VrTracker2 gives up without one. */
static int sdk_version(u32 *out) {
  int rc = sceKernelGetProsperoCompiledSdkVersion ? sceKernelGetProsperoCompiledSdkVersion(out) : -1;
  if (rc != 0 && out) {
    *out = 0x07000000;
    rc = 0;
  }
  return rc;
}

/* libSceVideoOut caches whether param.json lets the app change the output layout (attribute3
 * 0x400; data +0x358d4). EnableVrMode needs it. */
void ps5compat_allow_vr_layout(void) {
  const struct astro_firmware *fw = astro_firmware(astro_hdr.firmware);
  u64 base = vr2_module_base(vr2_find_module("libSceVideoOut.sprx"));
  if (base && fw)
    *(volatile u32 *)(base + fw->vo_layout) = 1;
}

/* ---------------------------------------------------------------- linear headset buffers */
/* GCN can't address the RDNA 64KB_R_X swizzle Hmd2 uses for the headset buffers, so they are
 * registered linear (VideoOut tiling 1). VideoOut takes linear display buffers only on a port
 * with the devkit "Enhanced Display Buffer Attribute" (port +0x704), which the export
 * Wu+K43K3ToE(port) sets; it also sets five process-wide flags (data +0x3007c, +0x30018,
 * +0x30080, +0x30084, +0x30088), which are put back so the game's own port is unaffected. */
static void set_buffer_attr2(u32 *attr, u64 format, u32 tiling, u32 w, u32 h, u64 option, u32 r0,
                             u64 r1) {
  if (w == 4000 && h == 2040)
    tiling = 1;
  sceVideoOutSetBufferAttribute2(attr, format, tiling, w, h, option, r0, r1);
}

static void enhance_port(int handle) {
  const struct astro_firmware *fw = astro_firmware(astro_hdr.firmware);
  u64 vo = vr2_module_base(vr2_find_module("libSceVideoOut.sprx"));
  if (!vo || !fw || !sceVideoOutEnhancedAttribute)
    return;
  if (!fw->vo_flags[0]) { /* 8.40 and older: it only sets the port's own fields */
    sceVideoOutEnhancedAttribute(handle);
    return;
  }
  volatile u8 *g = (volatile u8 *)vo;
  u32 a = *(volatile u32 *)(g + fw->vo_flags[0]), b = *(volatile u32 *)(g + fw->vo_flags[1]);
  u8 c = g[fw->vo_flags[2]], d = g[fw->vo_flags[3]], e = g[fw->vo_flags[4]];
  sceVideoOutEnhancedAttribute(handle);
  *(volatile u32 *)(g + fw->vo_flags[0]) = a, *(volatile u32 *)(g + fw->vo_flags[1]) = b;
  g[fw->vo_flags[2]] = c, g[fw->vo_flags[3]] = d, g[fw->vo_flags[4]] = e;
}

static int register_buffers2(int handle, int set, int start, void **bufs, int num, const void *attr,
                             int category, const void *option) {
  int linear = attr && ((const u32 *)attr)[1] == 1;
  if (linear)
    enhance_port(handle);
  int rc = sceVideoOutRegisterBuffers2(handle, set, start, bufs, num, attr, category, option);
  if (rc == 0 && linear) {
    /* the pitch VideoOut chose is in its copy of the attribute: port +0x248 + set * 0x108,
     * +0xa0 attribute, +0xb4 pitch (ports looked up and released with sub_10400/sub_103c0) */
    u64 vo = vr2_module_base(vr2_find_module("libSceVideoOut.sprx"));
    const struct astro_firmware *fw = astro_firmware(astro_hdr.firmware);
    void *(*lookup)(int) = fw ? (void *(*)(int))(vo + fw->vo_lookup) : 0;
    void (*release)(void *) = fw ? (void (*)(void *))(vo + fw->vo_release) : 0;
    void *wr = vo && lookup ? lookup(handle) : 0;
    if (wr) {
      const u32 *rec = (const u32 *)(*(u8 **)wr + 0x248 + (u64)set * 0x108 + 0xa0);
      if (rec[5] >= rec[3] && rec[5] <= 8192)
        g_display_pitch = rec[5];
      release(wr);
    }
    LOG("vr2: headset buffers linear, pitch %u", g_display_pitch);
  }
  return rc;
}

/* ---------------------------------------------------------------- the libraries' messages */
volatile u64 g_hand_lib_camera_at;
static const char *strstr_lib(const char *s, const char *k) {
  size_t kl = strlen(k);
  for (; *s; s++)
    if (*s == k[0] && memcmp(s, k, kl) == 0)
      return s;
  return 0;
}
static void lib_line(const char *text, size_t n) {
  char buf[400];
  if (n >= sizeof buf)
    n = sizeof buf - 1;
  memcpy(buf, text, n);
  while (n && (buf[n - 1] == '\n' || buf[n - 1] == '\r'))
    n--;
  buf[n] = 0;
  /* VrHand's camera manager and image lock speaking: its camera stream is up (hand.c keeps the
   * game's credentials raised until then) */
  if (n > 8 && (strstr_lib(buf, "[VrHand][CameraManager]") || strstr_lib(buf, "[VrHand][ImageLock]")))
    g_hand_lib_camera_at = sceKernelGetProcessTime();
  /* the libraries' own messages: the first 40, then a count every 500 (the tracker's sensor
   * notices came at ~3 a second) */
  static volatile u32 lines;
  u32 k = n ? __atomic_fetch_add(&lines, 1, __ATOMIC_RELAXED) : 0;
  if (n && k < 40)
    LOG("lib: %s", buf);
  else if (n && k % 500 == 499)
    LOG("lib: %u messages from the PS VR2 libraries so far (the first 40 logged)", k + 1);
}

static int lib_printf(const char *fmt, ...) {
  char buf[400];
  __builtin_va_list ap;
  __builtin_va_start(ap, fmt);
  int n = agent_vsnprintf(buf, sizeof buf, fmt, ap);
  __builtin_va_end(ap);
  lib_line(buf, strlen(buf));
  return n;
}

static int lib_fprintf(void *f, const char *fmt, ...) {
  (void)f;
  char buf[400];
  __builtin_va_list ap;
  __builtin_va_start(ap, fmt);
  int n = agent_vsnprintf(buf, sizeof buf, fmt, ap);
  __builtin_va_end(ap);
  lib_line(buf, strlen(buf));
  return n;
}

static int lib_puts(const char *s) {
  lib_line(s, strlen(s));
  return 0;
}

static size_t lib_fwrite(const void *p, size_t size, size_t count, void *f) {
  (void)f;
  lib_line(p, size * count);
  return count;
}

/* ---------------------------------------------------------------- */
void ps5compat_install(const int h[NLIBS]) {
  for (int i = 0; i < NLIBS; i++) {
    vr2_override(h[i], NID_scePthreadCreate, (void *)ps5_pthread_create);
    vr2_override(h[i], "hcuQgD53UxM", (void *)lib_printf);
    vr2_override(h[i], "fffwELXNVFA", (void *)lib_fprintf);
    vr2_override(h[i], "YQ0navp+YIc", (void *)lib_puts);
    vr2_override(h[i], "MpxhMh8QFro", (void *)lib_fwrite);
  }
  vr2_override(h[LIB_HMD2], NID_sceKernelGetAppInfo, (void *)appinfo_with_vr2);
  vr2_override(h[LIB_HMD2], NID_sceSystemServiceGetStatus, (void *)status_foreground);
  vr2_override(h[LIB_HMD2], NID_sceVideoOutSetBufferAttribute2, (void *)set_buffer_attr2);
  vr2_override(h[LIB_HMD2], NID_sceVideoOutRegisterBuffers2, (void *)register_buffers2);
  vr2_override(h[LIB_VRTRACKER2], NID_sceKernelGetProsperoCompiledSdkVersion,
               (void *)sdk_version);
  int vo = vr2_find_module("libSceVideoOut.sprx");
  if (vo >= 0) {
    vr2_override(vo, NID_sceKernelIsPs4Process, (void *)is_ps4_for_thread);
    vr2_override(vo, NID_get_authinfo, (void *)authinfo_for_thread);
  }
}

/* libSceVrHand (hand.c): its threads are PS5-side, its messages go to the log, it gets an SDK
 * version and the foreground status as the others do. */
void ps5compat_install_hand(int h) {
  vr2_override(h, NID_scePthreadCreate, (void *)ps5_pthread_create);
  vr2_override(h, "hcuQgD53UxM", (void *)lib_printf);
  vr2_override(h, "fffwELXNVFA", (void *)lib_fprintf);
  vr2_override(h, "YQ0navp+YIc", (void *)lib_puts);
  vr2_override(h, "MpxhMh8QFro", (void *)lib_fwrite);
  vr2_override(h, NID_sceKernelGetProsperoCompiledSdkVersion, (void *)sdk_version);
  vr2_override(h, NID_sceSystemServiceGetStatus, (void *)status_foreground);
  vr2_override(h, NID_sceKernelGetAppInfoSized, (void *)appinfo_with_hands);
  vr2_override(h, NID_scePthreadAttrSetaffinity, (void *)attr_affinity);
  vr2_override(h, NID_scePthreadAttrSetschedparam, (void *)attr_sched);
  vr2_override(h, NID_scePthreadAttrSetinheritsched, (void *)attr_inherit);
  vr2_override(h, NID_sceKernelMprotect, (void *)hand_mprotect);
  vr2_override(h, NID_sceAgcToolkitA, (void *)hand_tk_a);
  vr2_override(h, NID_sceAgcToolkitB, (void *)hand_tk_b);
}
