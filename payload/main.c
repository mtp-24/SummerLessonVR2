/* SummerLessonVR2: puts the agent into Summer Lesson: Hikari Miyamoto (the PS4 PS VR game) and
 * stays resident as its controller. (AstroBotVR2's payload, with that game's patches taken out.)
 *
 * 1. Builds, on the first run, the PS VR2 libraries and the GCN reprojection shader from this
 *    console's own firmware (prepare.c).
 * 2. Waits for the game, then maps the agent (agent/build/agent.elf, embedded) into it with
 *    ptrace, applies its R_X86_64_RELATIVE relocations and resolves its imports by NID in the
 *    game's own modules.
 * 3. Reads the eboot's import relocations from the kernel's dynlib object (dynsec +0x28 symtab,
 *    +0x38 strtab, +0x48 jmprel) and points the GOT slot of every hooked NID at the agent,
 *    saving the old value in the hook's orig.
 * 4. Detaches, then relays the agent's log to klog and /data/summerlessonvr2/agent.log, serves the
 *    agent's requests, and re-points GOT slots that a module load re-bound.
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include <elf.h>
#include <stdarg.h>
#include <stddef.h>
#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/mount.h>
#include <sys/uio.h>
#include <sys/stat.h>
#include <sys/sysctl.h>
#include <sys/wait.h>
#include <dirent.h>
#include <time.h>
#include <machine/reg.h>
#include <errno.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>

#include <ps5/kernel.h>

#include "pt.h"
#include "../common/test.h"

#define OUT_DIR "/data/summerlessonvr2"
/* The games: the three SUMMER LESSON titles of the Asian English release, at 01.00 as sold, by
 * the title id and the APP_VER of their param.sfo (2026-11-04: only 1.00 is supported; Hikari's
 * patch, 01.02, could not be installed to test). The agent hooks their imports by NID, which all
 * three share, but reads a few of a game's own statics (hooks.c) and the frame-rate patch sites
 * are code of its own: those differ per build. Chisato's and Allison's were found from Hikari's
 * code (docs/OTHER-GAMES.md, tools/ebootmap: every static's references agreeing, the code they sit
 * in the same instruction for instruction). Another version or title is refused, with a
 * notification. */
struct game_build {
  const char *title_id, *version, *name;
  /* statics, from the eboot's base: the PS4 RHI viewport (its frames' GPU labels at +0x2a8 and
   * +0x2b0, the frame count at +0x360), and the agent's others (struct agent_header) */
  uint32_t viewport, console_mgr, engine, max_fps, neo, sp_cache, delta_time;
  uint32_t fps120_at[2]; /* the plugin's two stores of its frame sequence (k_fps120) */
};
static const struct game_build k_builds[] = {
    /* HP0700-CUSA07434_00-SLHIKA01DLG00000 */
    {"CUSA07434", "01.00", "Hikari Miyamoto", 0x46917f0, 0x46b4350, 0x4b186f0, 0x4b18cd0, 0x4b8ef08,
     0x4bc5978, 0x46690d0, {0x2f8d24b, 0x2f8e564}},
    /* HP0700-CUSA11682_00-SLTRES01DLG00000 */
    {"CUSA11682", "01.00", "Chisato Shinjo", 0x4735970, 0x47584d0, 0x4bbc850, 0x4bbce30, 0x4c33068,
     0x4c71a98, 0x470d0d0, {0x2ffc31b, 0x2ffd634}},
    /* HP0700-CUSA11674_00-SLALLY01DLG00000: Chisato's statics, its own code */
    {"CUSA11674", "01.00", "Allison Snow", 0x4735970, 0x47584d0, 0x4bbc850, 0x4bbce30, 0x4c33068,
     0x4c71a98, 0x470d0d0, {0x2ffc2db, 0x2ffd5f4}},
};
#define N_BUILDS (int)(sizeof k_builds / sizeof k_builds[0])
#define BUILDS_TEXT "Hikari Miyamoto (CUSA07434), Chisato Shinjo (CUSA11682) and Allison Snow (CUSA11674), 1.00"
static int g_build = -1; /* the game's build: an index into k_builds, -1 before it is found */
static int title_known(const char *tid) {
  for (int i = 0; i < N_BUILDS; i++)
    if (strncmp(tid, k_builds[i].title_id, 9) == 0)
      return 1;
  return 0;
}
static int build_index(const char *tid, const char *ver) {
  for (int i = 0; i < N_BUILDS; i++)
    if (strncmp(tid, k_builds[i].title_id, 9) == 0 && strcmp(ver, k_builds[i].version) == 0)
      return i;
  return -1;
}
#define GAME_NAME (g_build < 0 ? "?" : k_builds[g_build].name)
#define TITLE_NAME "Summer Lesson"
#define TITLE_WORD "summer lesson" /* in the title of any version or region of it */
#include "prepare.h"
static struct gcn_file g_gcn;
static unsigned g_gcn_code[GCN_MAX_DWORDS];
static const struct astro_firmware *g_fw;
#define PAGE 0x4000
#define LOG_SIZE (256 * 1024)
#define AGENT_MAGIC 0x32524f5254534100ull

extern const uint8_t agent_elf[];
extern const uint8_t agent_elf_end[];
/* The agent image that is injected: OUT_DIR/agent.elf when there is one (an upload by
 * tools/testrun.sh or by hand: a new agent without a payload swap, the step that has brought
 * the console down three times; not in a release build), else the copy built in. Read again
 * before every injection. */
static const uint8_t *g_agent = agent_elf;
static size_t g_agent_size;
void say(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static long agent_header_size(const uint8_t *elf, size_t n);
/* a debugging switch file in OUT_DIR (trap_only, watch): there, and not a release build */
static int debug_file(const char *name, struct stat *st) {
#ifdef RELEASE
  (void)name, (void)st;
  return 0;
#else
  char path[96];
  snprintf(path, sizeof path, OUT_DIR "/%s", name);
  return stat(path, st) == 0;
#endif
}

static void choose_agent(void) {
  static uint8_t *file;
  static size_t file_size;
  g_agent = agent_elf;
  g_agent_size = (size_t)(agent_elf_end - agent_elf);
#ifdef RELEASE
  return; /* a release build: always its own agent (an agent.elf there is a test upload) */
#endif
  FILE *f = fopen(OUT_DIR "/agent.elf", "rb");
  if (!f)
    return;
  fseek(f, 0, SEEK_END);
  long n = ftell(f);
  fseek(f, 0, SEEK_SET);
  if (n < 4096 || n > (4 << 20)) {
    fclose(f);
    say("agent.elf: %ld bytes: not an agent, the built-in one used", n);
    return;
  }
  if (!file || file_size < (size_t)n) {
    free(file);
    file = malloc((size_t)n);
    file_size = file ? (size_t)n : 0;
  }
  size_t got = file ? fread(file, 1, (size_t)n, f) : 0;
  fclose(f);
  if (got != (size_t)n || memcmp(file, "\x7f" "ELF", 4) != 0) {
    say("agent.elf: unreadable or not an ELF: the built-in agent used");
    return;
  }
  /* one built with another header (an upload from an older build): the payload would write
   * past its header into the agent */
  long hs = agent_header_size(file, (size_t)n);
  if (hs != agent_header_size(agent_elf, (size_t)(agent_elf_end - agent_elf))) {
    say("agent.elf: its header is %ld bytes, this payload's %ld: the built-in agent used", hs,
        agent_header_size(agent_elf, (size_t)(agent_elf_end - agent_elf)));
    return;
  }
  g_agent = file;
  g_agent_size = (size_t)n;
  say("agent: " OUT_DIR "/agent.elf, %ld bytes (the built-in one is %ld)", n,
      (long)(agent_elf_end - agent_elf));
}
__asm__(".section .rodata\n.global agent_elf\n.global agent_elf_end\n.balign 16\n"
        "agent_elf:\n.incbin \"../agent/build/agent.elf\"\nagent_elf_end:\n.previous\n");

/* Layouts as in agent/agent.h, with game pointers as u64. */
struct a_import {
  uint64_t module, nid, slot;
};
struct a_hook {
  uint64_t nid, hook, orig, slot;
};
struct a_header {
  uint64_t magic;
  uint64_t imports;
  uint32_t nimports, pad0;
  uint64_t hooks;
  uint32_t nhooks, pad1;
  uint64_t shared;
  uint64_t boot;
  uint64_t vr_state; /* AGENT_MAGIC (2) and later */
  uint32_t vr_persist, pad2;
  uint64_t unbound_stub;
  uint64_t gcn; /* struct agent_gcn: the GCN reprojection shader goes here */
  uint32_t ipd_um, firmware;
  uint64_t game_base;
  uint32_t frame_pace, refresh_hz, screen_pct, viewport; /* screen_pct: a float's bits */
  uint32_t console_mgr, engine, max_fps, neo, sp_cache, delta_time; /* the build's statics */
};
struct a_shared_head {
  uint64_t log_wpos;
  uint32_t log_lock, boot_done, cmd, cmd_seq;
  uint64_t cmd_arg[4];
  uint32_t cmd_ack, repatch;
};
_Static_assert(sizeof(struct a_header) == 144, "agent header ABI");
_Static_assert(offsetof(struct a_header, firmware) == 92, "agent firmware field ABI");
_Static_assert(offsetof(struct a_header, frame_pace) == 104, "agent frame pace field ABI");
_Static_assert(offsetof(struct a_header, screen_pct) == 112, "agent screen_pct field ABI");
_Static_assert(offsetof(struct a_header, viewport) == 116, "agent viewport field ABI");
_Static_assert(offsetof(struct a_header, delta_time) == 140, "agent statics ABI");

/* The size of an agent image's header (its astro_hdr symbol), -1 when it can't be told. */
static long agent_header_size(const uint8_t *elf, size_t n) {
  const Elf64_Ehdr *eh = (const Elf64_Ehdr *)elf;
  if (n < sizeof *eh || eh->e_shentsize != sizeof(Elf64_Shdr) || eh->e_shoff > n ||
      (uint64_t)eh->e_shnum * sizeof(Elf64_Shdr) > n - eh->e_shoff)
    return -1;
  const Elf64_Shdr *sh = (const Elf64_Shdr *)(elf + eh->e_shoff);
  for (int i = 0; i < eh->e_shnum; i++) {
    if (sh[i].sh_type != SHT_SYMTAB || sh[i].sh_link >= eh->e_shnum)
      continue;
    const Elf64_Shdr *st = &sh[sh[i].sh_link];
    if (sh[i].sh_offset > n || sh[i].sh_size > n - sh[i].sh_offset || st->sh_offset > n ||
        st->sh_size > n - st->sh_offset)
      return -1;
    const Elf64_Sym *sy = (const Elf64_Sym *)(elf + sh[i].sh_offset);
    const char *str = (const char *)(elf + st->sh_offset);
    for (size_t j = 0; j < sh[i].sh_size / sizeof *sy; j++)
      if (sy[j].st_name < st->sh_size && st->sh_size - sy[j].st_name >= sizeof "astro_hdr" &&
          memcmp(str + sy[j].st_name, "astro_hdr", sizeof "astro_hdr") == 0)
        return (long)sy[j].st_size;
  }
  return -1;
}

static uint64_t g_base;    /* agent base in the game */
static uint8_t *g_mirror;  /* local copy of the agent image */
static size_t g_size;
static FILE *g_logf;
static uint64_t g_hdroff;

#define G2M(a) (g_mirror + ((uint64_t)(a) - g_base))

void say(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void say(const char *fmt, ...) {
  char buf[512];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof buf, fmt, ap);
  va_end(ap);
  printf("summerlessonvr2: %s\n", buf);
  if (g_logf) {
    fprintf(g_logf, "summerlessonvr2: %s\n", buf);
    fflush(g_logf);
  }
}

/* The kernel's log, kept in OUT_DIR/klog.txt: the kernel says there why a process died (a signal,
 * a failed SELF, a loader error), which the agent's own log cannot once the game is gone. A thread
 * reads /dev/klog (each read takes what was written since the last; nobody else reads it here) and
 * appends it, with the time, to the file. */
#define KLOG_MAX (8 << 20) /* bytes in klog.txt before it becomes klog.prev.txt */
static void *klog_thread(void *arg) {
  (void)arg;
  int fd = open("/dev/klog", O_RDONLY);
  if (fd < 0) {
    say("klog: /dev/klog not readable (errno %d)", errno);
    return NULL;
  }
  /* A file per payload start, the one before kept as klog.prev.txt (a crash's kernel log is read
   * after the reboot, when the payload has started again), and the same at KLOG_MAX: it was
   * appended to for good before (1.8 MB in a day, written to the console's storage all along). */
  rename(OUT_DIR "/klog.txt", OUT_DIR "/klog.prev.txt");
  FILE *f = fopen(OUT_DIR "/klog.txt", "w");
  if (!f) {
    close(fd);
    return NULL;
  }
  static char buf[65536];
  for (;;) {
    ssize_t n = read(fd, buf, sizeof buf);
    if (n < 0) {
      if (errno == EINTR)
        continue;
      say("klog: read failed (errno %d)", errno);
      break;
    }
    if (n == 0) {
      usleep(200000);
      continue;
    }
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    fprintf(f, "--- %ld.%03ld\n", (long)ts.tv_sec, (long)(ts.tv_nsec / 1000000));
    fwrite(buf, 1, (size_t)n, f);
    fflush(f);
    if (ftell(f) > KLOG_MAX) {
      fclose(f);
      rename(OUT_DIR "/klog.txt", OUT_DIR "/klog.prev.txt");
      if (!(f = fopen(OUT_DIR "/klog.txt", "w")))
        break;
    }
  }
  if (f)
    fclose(f);
  close(fd);
  return NULL;
}

static void __attribute__((unused)) klog_start(void) { /* (not in a release build) */
  pthread_t t;
  if (pthread_create(&t, NULL, klog_thread, NULL) == 0)
    pthread_detach(t);
}

/* A PS5 notification, so a problem shows on screen and not only in agent.log. */
typedef struct {
  char unused[45];
  char message[3075];
} notify_request_t;
int sceKernelSendNotificationRequest(int device, notify_request_t *req, size_t size, int blocking);

static void notify(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void notify(const char *fmt, ...) {
  static notify_request_t req;
  memset(&req, 0, sizeof req);
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(req.message, sizeof req.message, fmt, ap);
  va_end(ap);
  say("notify: %s", req.message);
  sceKernelSendNotificationRequest(0, &req, sizeof req, 0);
}

/* The agent loads the PS5 libraries from inside the game's sandbox (/summerlessonvr2/... there):
 * the game can't see /data, and giving it the real root let its level loader fail /app0 reads.
 * OUT_DIR/libs is mounted there read-only (nullfs) while they load. 1.0.0 and 1.0.1 copied them
 * into the sandbox and deleted them after: that writes into the /mnt tmpfs every sandbox shares,
 * and loading from it could panic the kernel ("recursing on non recursive lockmgr tmpfs"). */
static const char *const g_libs[] = {"libSceAgcDriver.sprx", "libSceAgc.sprx", "libSceHmd2.sprx",
                                     "libSceVrTracker2.sprx", "libSceVrHand.sprx"};
/* The game's sandbox is /mnt/sandbox/<title id>_NNN, usually _000; the newest one is the
 * running game's. */
static char g_sandbox_libs[96];

static void find_sandbox(void) {
  const char *tid = k_builds[g_build < 0 ? 0 : g_build].title_id;
  snprintf(g_sandbox_libs, sizeof g_sandbox_libs, "/mnt/sandbox/%s_000/summerlessonvr2", tid);
  DIR *d = opendir("/mnt/sandbox");
  struct dirent *e;
  time_t best = 0;
  if (!d)
    return;
  while ((e = readdir(d)) != NULL) {
    char path[96];
    struct stat st;
    if (strncmp(e->d_name, tid, 9) != 0 || e->d_name[9] != '_' || strlen(e->d_name) != 13)
      continue;
    snprintf(path, sizeof path, "/mnt/sandbox/%s", e->d_name);
    if (stat(path, &st) == 0 && st.st_mtime >= best) {
      best = st.st_mtime;
      snprintf(g_sandbox_libs, sizeof g_sandbox_libs, "%s/summerlessonvr2", path);
    }
  }
  closedir(d);
}
#define SANDBOX_LIBS g_sandbox_libs

static int g_libs_mounted;
static char g_mounted_at[96]; /* where they are mounted (SANDBOX_LIBS follows the game served) */

static void unmount_libs(int force);
static int mount_libs(void) {
  if (g_libs_mounted && strcmp(g_mounted_at, SANDBOX_LIBS) == 0)
    return 0;
  if (g_libs_mounted) { /* kept from a game before (an unmount that failed): that game is gone */
    unmount_libs(1);
    if (g_libs_mounted)
      return -1;
  }
  if (mkdir(SANDBOX_LIBS, 0755) != 0 && errno != EEXIST) {
    say("libs: mkdir %s: errno %d", SANDBOX_LIBS, errno);
    return -1;
  }
  const char *target = OUT_DIR "/libs";
  struct iovec iov[] = {
      {(void *)"fstype", 7},  {(void *)"nullfs", 7},
      {(void *)"fspath", 7},  {(void *)SANDBOX_LIBS, strlen(SANDBOX_LIBS) + 1},
      {(void *)"target", 7},  {(void *)target, strlen(target) + 1},
  };
  if (nmount(iov, 6, MNT_RDONLY) != 0) {
    say("libs: nullfs mount of %s on %s failed: errno %d", target, SANDBOX_LIBS, errno);
    return -1;
  }
  g_libs_mounted = 1;
  snprintf(g_mounted_at, sizeof g_mounted_at, "%s", SANDBOX_LIBS);
  say("libs: %s mounted read-only on %s", target, SANDBOX_LIBS);
  return 0;
}

/* force: the game is gone (nothing maps the libraries any more) */
static void unmount_libs(int force) {
  if (!g_libs_mounted)
    return;
  if (unmount(g_mounted_at, force ? MNT_FORCE : 0) == 0) {
    g_libs_mounted = 0;
    rmdir(g_mounted_at);
    say("libs: unmounted from %s", g_mounted_at);
  } else {
    say("libs: unmount %s%s: errno %d (kept)", g_mounted_at, force ? " (forced)" : "", errno);
  }
}

/* the agent's stage request: the library is there already through the mount (delete: nothing to
 * do) */
static void stage_lib(uint64_t i, uint64_t del) {
  if (i >= sizeof g_libs / sizeof g_libs[0] || del)
    return;
  if (mount_libs() != 0)
    return;
  char path[160];
  struct stat st;
  snprintf(path, sizeof path, "%s/%s", SANDBOX_LIBS, g_libs[i]);
  if (stat(path, &st) == 0)
    say("stage: %s (%ld bytes, through the mount)", path, (long)st.st_size);
  else
    say("stage: %s not visible: errno %d", path, errno);
}

/* ---- the eboot's import table, from the kernel's dynlib object ---- */
struct import_slot {
  char nid[16];
  uint64_t slot;
};
static struct import_slot *g_slots;
static int g_nslots;

/* eboot is handle 0. Some launch environments randomize its image base (observed
 * at 0x43be8000 on 11.60); use the loader's mapbase for code, GOT and entry alike. */
static uint64_t eboot_mapbase(pid_t pid) {
  intptr_t proc = kernel_get_proc(pid);
  uint64_t obj = 0;
  if (!proc || kernel_copyout(proc + 0x3e8, &obj, 8) != 0)
    return 0;
  for (int i = 0; obj && i < 300; ++i) {
    uint64_t o[8];
    if (kernel_copyout(obj, o, sizeof o) != 0)
      return 0;
    if (o[0x28 / 8] == 0) {
      uint64_t base = o[0x30 / 8];
      return base >= 0x10000 && (base >> 47) == 0 ? base : 0;
    }
    obj = o[0];
  }
  return 0;
}

static int read_eboot_imports(pid_t pid) {
  intptr_t proc = kernel_get_proc(pid);
  uint64_t obj = 0;
  if (!proc || kernel_copyout(proc + 0x3e8, &obj, 8) != 0)
    return -1;
  for (int i = 0; obj && i < 300; i++) {
    uint8_t o[0x180];
    if (kernel_copyout(obj, o, sizeof o) != 0)
      return -1;
    if (*(uint64_t *)&o[0x28] == 0) { /* handle 0: eboot */
      uint64_t dynsec = *(uint64_t *)&o[0x148];
      uint64_t s[0x60 / 8];
      if (kernel_copyout(dynsec, s, sizeof s) != 0)
        return -1;
      uint64_t symtab = s[5], symsz = s[6], strtab = s[7], strsz = s[8], jmprel = s[9],
               jmpsz = s[10];
      uint8_t *sym = malloc(symsz), *str = malloc(strsz + 1), *jr = malloc(jmpsz);
      if (!sym || !str || !jr || kernel_copyout(symtab, sym, symsz) ||
          kernel_copyout(strtab, str, strsz) || kernel_copyout(jmprel, jr, jmpsz)) {
        free(sym);
        free(str);
        free(jr);
        return -1;
      }
      str[strsz] = 0;
      g_nslots = (int)(jmpsz / 24);
      free(g_slots);
      g_slots = calloc(g_nslots ? g_nslots : 1, sizeof *g_slots);
      if (!g_slots)
        g_nslots = 0;
      for (int k = 0; k < g_nslots; k++) {
        Elf64_Rela *r = (Elf64_Rela *)(jr + k * 24);
        uint64_t si = ELF64_R_SYM(r->r_info);
        if ((si + 1) * 24 > symsz) /* (left empty: no hook finds it) */
          continue;
        Elf64_Sym *sy = (Elf64_Sym *)(sym + si * 24);
        if (sy->st_name >= strsz)
          continue;
        const char *name = (const char *)str + sy->st_name;
        size_t n = strcspn(name, "#");
        if (n > 15)
          n = 15;
        memcpy(g_slots[k].nid, name, n);
        g_slots[k].slot = *(uint64_t *)&o[0x30] + r->r_offset;
      }
      free(sym);
      free(str);
      free(jr);
      return 0;
    }
    obj = *(uint64_t *)&o[0];
  }
  return -1;
}

static uint64_t slot_of(const char *nid) {
  for (int k = 0; k < g_nslots; k++)
    if (strcmp(g_slots[k].nid, nid) == 0)
      return g_slots[k].slot;
  return 0;
}

/* ---- an agent already in the game (this payload was sent again) ----
 * /data/summerlessonvr2/agent_state: "<pid> <base> <header offset> <size>". The agent keeps running
 * and this payload takes over serving it, from a copy of its image. */
static int adopt_agent(pid_t pid) {
  FILE *f = fopen(OUT_DIR "/agent_state", "r");
  if (!f)
    return -1;
  int ppid = -1;
  unsigned long base = 0, hdroff = 0, size = 0;
  int n = fscanf(f, "%d %lx %lx %lx", &ppid, &base, &hdroff, &size);
  fclose(f);
  if (n < 4 || ppid != pid || !size || size > 64 << 20 || hdroff >= size)
    return -1;
  uint64_t magic = 0;
  if (kernel_proc_copyout(pid, base + hdroff, &magic, 8) != 0 || magic != AGENT_MAGIC)
    return -1;
  g_mirror = malloc(size);
  if (!g_mirror || kernel_proc_copyout(pid, base, g_mirror, size) != 0) {
    free(g_mirror);
    g_mirror = NULL;
    return -1;
  }
  g_base = base, g_size = size, g_hdroff = hdroff;
  say("the agent is already in the game (at 0x%lx): serving it", base);
  return 0;
}

/* Import slots the trap fallback (inject_by_trap, below) points at unmapped pages. */
#define TRAP_BASE 0x2a0000ull
static const char *const k_trap_nids[] = {
    "Ud7j3+RDIBg", /* sceHmdSetupDialogUpdateStatus */
    "6lVRHMV5LY0", /* sceHmdSetupDialogGetResult */
    "q1cHNfGycLI", /* scePadRead */
    "4J2sUJmuHZQ", /* sceKernelGetProcessTime */
    "1jfXLRVzisc", /* sceKernelUsleep */
    "s-J66ar9g50", /* sceHmdInitialize315 */
    "NB1Y2kA2jCY", /* sceHmdSetupDialogInitialize */
    "NNgiV4T+akU", /* sceHmdSetupDialogOpen */
    "d2g5Ij7EUzo", /* sceHmdOpen */
    "K4KnH0QkT2c", /* sceHmdInitialize */
    "thDt9upZlp8", /* sceHmdGetDeviceInformation */
    "YndgXqQVV7c", /* scePadReadState */
};
#define NTRAPS (sizeof k_trap_nids / sizeof k_trap_nids[0])

/* see find_unresolved */
static uint64_t g_unresolved;

static int trap_index(uint64_t rip) {
  if (rip < TRAP_BASE || (rip - TRAP_BASE) % 0x1000 || (rip - TRAP_BASE) / 0x1000 >= NTRAPS)
    return -1;
  return (int)((rip - TRAP_BASE) / 0x1000);
}

/* Call fn(a0) on the stopped thread: a fresh stack 1 KiB below the red zone, returning to
 * address 0, where the fault stops it again; then the thread's registers are put back. */
static long call_in_game(pid_t pid, uint64_t fn, uint64_t a0) {
  struct reg bak, r;
  if (pt_getregs(pid, &bak) != 0)
    return -1;
  r = bak;
  uint64_t sp = ((bak.r_rsp - 1024) & ~(uint64_t)0xf) - 8, zero = 0;
  if (pt_copyin(pid, &zero, sp, 8) != 0)
    return -1;
  r.r_rsp = sp;
  r.r_rip = fn;
  r.r_rdi = a0;
  if (pt_setregs(pid, &r) != 0)
    return -1;
  long ret = -1;
  for (int i = 0; i < 50; i++) {
    if (pt_continue(pid, 0) != 0)
      break;
    int st = 0, waited = 0;
    for (; waited < 3000; waited++) {
      pid_t w = waitpid(pid, &st, WNOHANG);
      if (w == pid)
        break;
      usleep(1000);
    }
    if (waited >= 3000) {
      say("call_in_game: no return after 3 s");
      kill(pid, SIGSTOP);
      waitpid(pid, &st, 0);
      break;
    }
    if (!WIFSTOPPED(st))
      break;
    pt_getregs(pid, &r);
    if (r.r_rip == 0) {
      ret = r.r_rax;
      break;
    }
    say("call_in_game: stop with signal %d at 0x%lx, continuing", WSTOPSIG(st), (long)r.r_rip);
  }
  pt_setregs(pid, &bak);
  return ret;
}

/* ---- loading ---- */
static uint32_t resolution_pct(void); /* resolution.txt, below */

/* a symbol's address in the agent (0: none), and its size if asked */
static uint64_t agent_sym_size(const char *name, uint64_t *size) {
  const Elf64_Ehdr *eh = (const Elf64_Ehdr *)g_agent;
  const Elf64_Shdr *sh = (const Elf64_Shdr *)(g_agent + eh->e_shoff);
  for (int i = 0; i < eh->e_shnum; i++) {
    if (sh[i].sh_type != SHT_SYMTAB)
      continue;
    const Elf64_Sym *sy = (const Elf64_Sym *)(g_agent + sh[i].sh_offset);
    const char *str = (const char *)(g_agent + sh[sh[i].sh_link].sh_offset);
    for (size_t j = 0; j < sh[i].sh_size / sizeof *sy; j++)
      if (strcmp(str + sy[j].st_name, name) == 0) {
        if (size)
          *size = sy[j].st_size;
        return sy[j].st_value;
      }
  }
  return 0;
}
static uint64_t agent_sym(const char *name) { return agent_sym_size(name, 0); }

/* test.txt (tools/testrun.sh): a run without a player. Its words: `record` (the controller to
 * the log, as `pad:` lines), `replay` (the controller from pad.txt: the same lines without the
 * prefix), `worn` (the headset counted as on a head); head.txt the head's pose the same way
 * (`head:` lines). The agent's pads.c, vr.c and vr2.c. */
static void test_settings(uint64_t lo) {
#ifdef RELEASE
  (void)lo;
  return; /* a release build: no test mode (test.txt, pad.txt, head.txt are not read) */
#endif
  /* each table filled no further than the agent's own (an agent.elf of another build may hold
   * fewer steps than TEST_*_STEPS): its symbol's size */
  uint64_t ssz = 0, hsz = 0;
  uint64_t va = agent_sym("g_test"), sva = agent_sym_size("g_pad_script", &ssz),
           nva = agent_sym("g_pad_script_n");
  uint32_t pad_max = ssz / 16 < TEST_PAD_STEPS ? (uint32_t)(ssz / 16) : TEST_PAD_STEPS;
  char line[64] = "";
  FILE *f = fopen(OUT_DIR "/test.txt", "r");
  if (!va || !f) {
    if (f)
      fclose(f);
    return;
  }
  if (!fgets(line, sizeof line, f))
    line[0] = 0;
  fclose(f);
  uint32_t test = (strstr(line, "record") ? 1u : 0) | (strstr(line, "replay") ? 2u : 0) |
                  (strstr(line, "worn") ? 4u : 0);
  uint32_t n = 0;
  if ((test & 2u) && sva && nva && (f = fopen(OUT_DIR "/pad.txt", "r"))) {
    struct {
      uint32_t t, b;
      uint8_t v[8];
    } *s = (void *)(g_mirror + sva - lo); /* the agent's struct pad_step */
    unsigned t, b, v[6];
    while (n < pad_max &&
           fscanf(f, "%u %x %x %x %x %x %x %x", &t, &b, v, v + 1, v + 2, v + 3, v + 4, v + 5) == 8) {
      s[n].t = t, s[n].b = b;
      for (int i = 0; i < 6; i++)
        s[n].v[i] = (uint8_t)v[i];
      s[n].v[6] = s[n].v[7] = 0;
      n++;
    }
    fclose(f);
    memcpy(g_mirror + nva - lo, &n, 4);
  }
  uint32_t nh = 0;
  uint64_t hva = agent_sym_size("g_head_script", &hsz), hnva = agent_sym("g_head_script_n");
  uint32_t head_max = hsz / 32 < TEST_HEAD_STEPS ? (uint32_t)(hsz / 32) : TEST_HEAD_STEPS;
  if ((test & 2u) && hva && hnva && (f = fopen(OUT_DIR "/head.txt", "r"))) {
    struct {
      uint32_t t;
      float v[7];
    } *s = (void *)(g_mirror + hva - lo); /* the agent's struct head_step */
    unsigned t;
    float v[7];
    while (nh < head_max && fscanf(f, "%u %f %f %f %f %f %f %f", &t, v, v + 1, v + 2, v + 3, v + 4,
                               v + 5, v + 6) == 8) {
      s[nh].t = t;
      memcpy(s[nh].v, v, sizeof v);
      nh++;
    }
    fclose(f);
    memcpy(g_mirror + hnva - lo, &nh, 4);
  }
  memcpy(g_mirror + va - lo, &test, 4);
  say("test: %s%s%s%u steps of pad.txt, %u of head.txt",
      test & 1 ? "recording the controller and the head, " : "",
      test & 2 ? "replaying the controller and the head, " : "",
      test & 4 ? "the headset counted as worn, " : "", n, nh);
}

/* ---- the agent's own patches to the game's code (docs/MULTIVIEW.md, the build) ----
 * Only while the game is stopped at its entry point (inject_at_entry), never through the traps
 * (the game running then), and only when cvars.txt has a line agent.multiview=N (N > 0; inset
 * mode, in a release build too): the agent's boot gets N and returns a table of patches, each the game's stock
 * bytes and the new ones. Every entry is checked against the game's bytes first; any one that
 * differs (another build, another game) and none is written. */
#define AGENT_PATCH_MAGIC 0x48435441504c5653ull /* "SVLPATCH" */
#define AGENT_PATCH_MAX 20 /* (agent/multiview.c's too) */
struct a_patch {
  uint32_t at; /* from the eboot's base */
  uint8_t len, pad[3];
  uint8_t was[32], now[32];
};
struct a_patches {
  uint64_t magic;
  uint32_t n, pad;
  struct a_patch p[AGENT_PATCH_MAX];
};
_Static_assert(sizeof(struct a_patch) == 72, "agent patch ABI");
static int g_loading_at_entry;  /* load_agent called from inject_at_entry: the game stopped */
static uint64_t g_agent_patches; /* the table boot returned (in the game), 0 = none */

/* Inset mode by default (agent.multiview=600 without the line or the file); agent.multiview=0 for
 * the stock game at resolution.txt's size. A game whose code isn't Hikari 1.00's refuses the
 * patches and runs as the stock game (multiview_refused). */
#define MULTIVIEW_DEFAULT 600u
static uint32_t multiview_option(void) {
  char line[128];
  uint32_t n = MULTIVIEW_DEFAULT;
  FILE *f = fopen(OUT_DIR "/cvars.txt", "r");
  if (!f)
    return n;
  while (fgets(line, sizeof line, f))
    if (strncmp(line, "agent.multiview=", 16) == 0)
      n = (uint32_t)strtoul(line + 16, NULL, 10);
  fclose(f);
  return n > 4096 ? 0 : n;
}

/* No patches after all (the table refused: another game or build): the agent's g_multiview_rows
 * set back to 0, so it lays out nothing for the insets and runs as the stock game (its boot had
 * set it for agent.multiview=N). Through the agent's symbol, as test_settings finds g_test, but
 * written into the running agent (kernel copy-in): its image went in before its boot. */
static void multiview_refused(pid_t pid) {
  uint64_t hva = agent_sym("astro_hdr"), rva = agent_sym("g_multiview_rows");
  uint32_t zero = 0;
  if (!hva || !rva || !g_base || !g_hdroff ||
      kernel_proc_copyin(pid, &zero, g_base + g_hdroff + (rva - hva), 4) != 0) {
    say("multiview: the agent's g_multiview_rows could not be set back to 0");
    return;
  }
  say("multiview: inset mode off (the agent's g_multiview_rows 0): the stock game");
}

static void apply_agent_patches(pid_t pid) {
  uint64_t at = g_agent_patches;
  g_agent_patches = 0;
  if (!at)
    return;
  static struct a_patches t;
  uint64_t base = eboot_mapbase(pid);
  if (at < g_base || at + sizeof t > g_base + g_size || !base ||
      kernel_proc_copyout(pid, at, &t, sizeof t) != 0 || t.magic != AGENT_PATCH_MAGIC ||
      t.n == 0 || t.n > AGENT_PATCH_MAX) {
    say("multiview: the agent's patch table at 0x%lx is not one (n %u): no patches", (long)at,
        t.magic == AGENT_PATCH_MAGIC ? t.n : 0);
    multiview_refused(pid);
    return;
  }
  for (uint32_t i = 0; i < t.n; i++) {
    uint8_t cur[32];
    const struct a_patch *e = &t.p[i];
    if (e->len == 0 || e->len > 32 || e->at >= 0x6000000 ||
        kernel_proc_copyout(pid, base + e->at, cur, e->len) != 0 || memcmp(cur, e->was, e->len) != 0) {
      say("multiview: patch %u at 0x%x (%u bytes) is not on the game's stock bytes: no patches", i,
          e->at, e->len);
      multiview_refused(pid);
      return;
    }
  }
  for (uint32_t i = 0; i < t.n; i++) {
    uint8_t cur[32];
    const struct a_patch *e = &t.p[i];
    kernel_proc_copyin(pid, e->now, base + e->at, e->len);
    if (kernel_proc_copyout(pid, base + e->at, cur, e->len) != 0 || memcmp(cur, e->now, e->len) != 0)
      say("multiview: patch %u at 0x%x did not take", i, e->at);
  }
  say("multiview: %u patches to the game's code in", t.n);
}

static int load_agent(pid_t pid) {
  const Elf64_Ehdr *eh = (const Elf64_Ehdr *)g_agent;
  const Elf64_Phdr *ph = (const Elf64_Phdr *)(g_agent + eh->e_phoff);
  const Elf64_Shdr *sh = (const Elf64_Shdr *)(g_agent + eh->e_shoff);
  uint64_t lo = ~0ull, hi = 0;
  for (int i = 0; i < eh->e_phnum; i++) {
    if (ph[i].p_type != PT_LOAD)
      continue;
    if (ph[i].p_vaddr < lo)
      lo = ph[i].p_vaddr;
    if (ph[i].p_vaddr + ph[i].p_memsz > hi)
      hi = ph[i].p_vaddr + ph[i].p_memsz;
  }
  lo &= ~(uint64_t)(PAGE - 1);
  g_size = (hi - lo + PAGE - 1) & ~(uint64_t)(PAGE - 1);
  g_mirror = calloc(1, g_size);
  if (!g_mirror)
    return -1;
  intptr_t base = pt_mmap(pid, 0, g_size, PROT_READ | PROT_WRITE, MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
  if (base == -1 || base == 0) {
    pt_perror(pid, "pt_mmap");
    return -1;
  }
  g_base = base;
  say("agent: %zu bytes at 0x%lx in pid %d", g_size, (long)g_base, pid);
  for (int i = 0; i < eh->e_phnum; i++)
    if (ph[i].p_type == PT_LOAD && ph[i].p_filesz)
      memcpy(g_mirror + ph[i].p_vaddr - lo, g_agent + ph[i].p_offset, ph[i].p_filesz);
  int nrel = 0;
  for (int i = 0; i < eh->e_shnum; i++) {
    if (sh[i].sh_type != SHT_RELA)
      continue;
    const Elf64_Rela *r = (const Elf64_Rela *)(g_agent + sh[i].sh_offset);
    for (size_t j = 0; j < sh[i].sh_size / sizeof *r; j++) {
      if (ELF64_R_TYPE(r[j].r_info) != R_X86_64_RELATIVE) {
        say("agent: unsupported relocation type %lu", (unsigned long)ELF64_R_TYPE(r[j].r_info));
        return -1;
      }
      *(uint64_t *)(g_mirror + r[j].r_offset - lo) = g_base + r[j].r_addend;
      nrel++;
    }
  }
  /* astro_hdr from the symbol table */
  uint64_t hdr_va = 0;
  for (int i = 0; i < eh->e_shnum && !hdr_va; i++) {
    if (sh[i].sh_type != SHT_SYMTAB)
      continue;
    const Elf64_Sym *sy = (const Elf64_Sym *)(g_agent + sh[i].sh_offset);
    const char *str = (const char *)(g_agent + sh[sh[i].sh_link].sh_offset);
    for (size_t j = 0; j < sh[i].sh_size / sizeof *sy; j++)
      if (strcmp(str + sy[j].st_name, "astro_hdr") == 0)
        hdr_va = sy[j].st_value;
  }
  struct a_header *hdr = (struct a_header *)(g_mirror + hdr_va - lo);
  g_hdroff = hdr_va - lo;
  if (!hdr_va || hdr->magic != AGENT_MAGIC) {
    say("agent: no header");
    return -1;
  }
  hdr->firmware = g_fw->version;
  hdr->game_base = eboot_mapbase(pid);
  hdr->frame_pace = 2; /* 1 (120 fps) only once its patches are in, at the entry point */
  hdr->screen_pct = g_build >= 0 ? resolution_pct() : 0;
  say("agent: %d relocations, header at +0x%lx, %u imports", nrel, (long)hdr_va, hdr->nimports);
  FILE *ipd = fopen(OUT_DIR "/ipd", "r");
  if (ipd) {
    unsigned um = 0;
    if (fscanf(ipd, "%u", &um) == 1 && um >= 50000 && um <= 80000)
      hdr->ipd_um = um;
    fclose(ipd);
  }
  test_settings(lo);
  /* the game's statics (k_builds): the PS4 RHI viewport, whose frames' GPU labels and count the
   * agent times the game's GPU by, and the ones it reads and sets (0: an unknown build, left be) */
  if (g_build >= 0) {
    const struct game_build *gb = &k_builds[g_build];
    hdr->viewport = gb->viewport, hdr->console_mgr = gb->console_mgr, hdr->engine = gb->engine;
    hdr->max_fps = gb->max_fps, hdr->neo = gb->neo, hdr->sp_cache = gb->sp_cache;
    hdr->delta_time = gb->delta_time;
  }
  if (hdr->gcn && g_gcn.magic == GCN_MAGIC) {
    uint32_t *dst = (uint32_t *)G2M(hdr->gcn);
    dst[0] = g_gcn.magic, dst[1] = g_gcn.dwords, dst[2] = g_gcn.rsrc1_vgprs,
    dst[3] = g_gcn.rsrc1_sgprs;
    memcpy(dst + 4, g_gcn_code, g_gcn.dwords * 4);
  }

  /* imports */
  struct a_import *imp = (struct a_import *)G2M(hdr->imports);
  int bad = 0, late = 0;
  for (uint32_t i = 0; i < hdr->nimports; i++) {
    const char *mod = (const char *)G2M(imp[i].module);
    const char *nid = (const char *)G2M(imp[i].nid);
    uint32_t h;
    uint64_t addr = 0;
    if (kernel_dynlib_handle(pid, mod, &h) != 0) {
      late++; /* a PS5 library the agent loads later (CMD_RESOLVE) */
    } else if (!(addr = kernel_dynlib_resolve(pid, h, nid))) {
      say("agent: import %s %s not found", mod, nid);
      bad++;
    }
    *(uint64_t *)G2M(imp[i].slot) = addr;
  }

  /* hooks */
  if (read_eboot_imports(pid) != 0) {
    say("agent: can't read the eboot's imports");
    return -1;
  }
  struct a_hook *hk = (struct a_hook *)G2M(hdr->hooks);
  int nhooks = 0, nfound = 0;
  for (; hk[nhooks].nid; nhooks++) {
    const char *nid = (const char *)G2M(hk[nhooks].nid);
    uint64_t slot = slot_of(nid);
    hk[nhooks].slot = slot;
    if (!slot) {
      say("agent: hook %s: not imported by the game", nid);
      continue;
    }
    uint64_t cur = 0;
    pt_copyout(pid, slot, &cur, 8);
    *(uint64_t *)G2M(hk[nhooks].orig) = cur;
    nfound++;
  }
  say("agent: %d of %d hooks found in %d import slots; %d imports missing, %d resolved later",
      nfound, nhooks, g_nslots, bad, late);

  /* copy in, protect, patch */
  if (pt_copyin(pid, g_mirror, g_base, g_size) != 0) {
    say("agent: copyin failed");
    return -1;
  }
  /* Same order as hbldr's elfldr: kernel_mprotect sets a whole VM map entry, so every
   * segment is (re)protected in turn, and pt_mprotect on the others splits the entry. */
  for (int i = 0; i < eh->e_phnum; i++) {
    if (ph[i].p_type != PT_LOAD || ph[i].p_memsz == 0)
      continue;
    uint64_t a = (g_base + ph[i].p_vaddr - lo) & ~(uint64_t)(PAGE - 1);
    uint64_t e = (g_base + ph[i].p_vaddr - lo + ph[i].p_memsz + PAGE - 1) & ~(uint64_t)(PAGE - 1);
    int prot = ((ph[i].p_flags & PF_R) ? PROT_READ : 0) | ((ph[i].p_flags & PF_W) ? PROT_WRITE : 0) |
               ((ph[i].p_flags & PF_X) ? PROT_EXEC : 0);
    int r = (ph[i].p_flags & PF_X) ? kernel_mprotect(pid, a, e - a, prot)
                                   : pt_mprotect(pid, a, e - a, prot);
    if (r != 0) {
      say("agent: protect 0x%lx-0x%lx 0x%x failed", (long)a, (long)e, prot);
      return -1;
    }
  }
  for (int i = 0; i < nhooks; i++) {
    if (!hk[i].slot)
      continue;
    if (pt_copyin(pid, &hk[i].hook, hk[i].slot, 8) != 0)
      say("agent: patching slot 0x%lx failed", (long)hk[i].slot);
  }
  if (hdr->boot) {
    uint32_t mv = g_loading_at_entry ? multiview_option() : 0;
    long r = call_in_game(pid, hdr->boot, mv);
    say("agent: boot(%u) -> %ld", mv, mv ? 0 : r);
    g_agent_patches = mv && r > 0 ? (uint64_t)r : 0;
  }
  return 0;
}

/* Resolve every import slot that is still zero whose module is loaded now (modules the agent
 * loaded itself, and the game's own loaded later). */
static int resolve_missing(pid_t pid) {
  struct a_header *hdr = NULL;
  for (uint64_t off = 0; off + sizeof *hdr <= g_size; off += 8)
    if (*(uint64_t *)(g_mirror + off) == AGENT_MAGIC) {
      hdr = (struct a_header *)(g_mirror + off);
      break;
    }
  if (!hdr)
    return -1;
  struct a_import *imp = (struct a_import *)G2M(hdr->imports);
  int n = 0;
  for (uint32_t i = 0; i < hdr->nimports; i++) {
    uint64_t cur = 0;
    kernel_proc_copyout(pid, imp[i].slot, &cur, 8);
    if (cur)
      continue;
    const char *mod = (const char *)G2M(imp[i].module);
    const char *nid = (const char *)G2M(imp[i].nid);
    uint32_t h;
    uint64_t addr = 0;
    if (kernel_dynlib_handle(pid, mod, &h) != 0)
      continue; /* its module isn't loaded yet: a later request gets it */
    if (!(addr = kernel_dynlib_resolve(pid, h, nid))) {
      say("late import %s %s not found", mod, nid);
      continue;
    }
    kernel_proc_copyin(pid, &addr, imp[i].slot, 8);
    n++;
  }
  return n;
}

/* ---- imports of the modules the agent loaded ---- */
static int stub_unbound(pid_t pid, const uint64_t handles[4], uint64_t stub) {
  intptr_t proc = kernel_get_proc(pid);
  uint64_t obj = 0;
  int patched = 0;
  kernel_copyout(proc + 0x3e8, &obj, 8);
  for (int n = 0; obj && n < 400; n++) {
    uint8_t o[0x180];
    if (kernel_copyout(obj, o, sizeof o) != 0)
      break;
    uint64_t next = *(uint64_t *)&o[0], handle = *(uint64_t *)&o[0x28],
             mapbase = *(uint64_t *)&o[0x30], dynsec = *(uint64_t *)&o[0x148];
    int want = 0;
    for (int i = 0; i < 4; i++)
      want |= handles[i] && handles[i] == handle;
    uint64_t s[0x60 / 8];
    if (want && (dynsec >> 48) == 0xffff && kernel_copyout(dynsec, s, sizeof s) == 0) {
      uint64_t symsz = s[6], strsz = s[8], jmpsz = s[10];
      uint8_t *sym = malloc(symsz), *str = malloc(strsz + 1), *jr = malloc(jmpsz);
      if (sym && str && jr && !kernel_copyout(s[5], sym, symsz) &&
          !kernel_copyout(s[7], str, strsz) && !kernel_copyout(s[9], jr, jmpsz)) {
        str[strsz] = 0;
        for (uint64_t k = 0; k + 24 <= jmpsz; k += 24) {
          uint64_t off = *(uint64_t *)(jr + k), info = *(uint64_t *)(jr + k + 8), cur = 0;
          uint64_t slot = mapbase + off;
          if (kernel_proc_copyout(pid, slot, &cur, 8) != 0)
            continue;
          if (cur >= 0x10000 && (cur >> 47) == 0)
            continue;
          uint64_t si = info >> 32;
          uint32_t sn = (si + 1) * 24 <= symsz ? *(uint32_t *)(sym + si * 24) : (uint32_t)strsz;
          const char *name = sn < strsz ? (const char *)str + sn : "?";
          kernel_proc_copyin(pid, &stub, slot, 8);
          if (patched++ < 40)
            say("stub: module %lu import %s (was 0x%lx)", (unsigned long)handle, name,
                (unsigned long)cur);
        }
      }
      free(sym);
      free(str);
      free(jr);
    }
    obj = next;
  }
  return patched;
}

/* Point module `handle`'s import slot(s) for `nid` at `value`. */
static int override_import(pid_t pid, uint64_t handle, const char *nid, uint64_t value) {
  intptr_t proc = kernel_get_proc(pid);
  uint64_t obj = 0;
  int patched = 0;
  kernel_copyout(proc + 0x3e8, &obj, 8);
  for (int n = 0; obj && n < 400; n++) {
    uint8_t o[0x180];
    if (kernel_copyout(obj, o, sizeof o) != 0)
      break;
    uint64_t next = *(uint64_t *)&o[0], dynsec = *(uint64_t *)&o[0x148];
    uint64_t s[0x60 / 8];
    if (*(uint64_t *)&o[0x28] == handle && (dynsec >> 48) == 0xffff &&
        kernel_copyout(dynsec, s, sizeof s) == 0) {
      uint64_t mapbase = *(uint64_t *)&o[0x30], symsz = s[6], strsz = s[8], jmpsz = s[10];
      uint8_t *sym = malloc(symsz), *str = malloc(strsz + 1), *jr = malloc(jmpsz);
      if (sym && str && jr && !kernel_copyout(s[5], sym, symsz) &&
          !kernel_copyout(s[7], str, strsz) && !kernel_copyout(s[9], jr, jmpsz)) {
        str[strsz] = 0;
        size_t nl = strlen(nid);
        for (uint64_t k = 0; k + 24 <= jmpsz; k += 24) {
          uint64_t off = *(uint64_t *)(jr + k), info = *(uint64_t *)(jr + k + 8), si = info >> 32;
          uint32_t sn = (si + 1) * 24 <= symsz ? *(uint32_t *)(sym + si * 24) : (uint32_t)strsz;
          if (sn >= strsz)
            continue;
          const char *name = (const char *)str + sn;
          if (strncmp(name, nid, nl) != 0 || (name[nl] && name[nl] != '#'))
            continue;
          kernel_proc_copyin(pid, &value, mapbase + off, 8);
          patched++;
        }
      }
      free(sym);
      free(str);
      free(jr);
    }
    obj = next;
  }
  say("override: module %lu import %s -> 0x%lx (%d slots)", (unsigned long)handle, nid,
      (unsigned long)value, patched);
  return patched;
}

static struct a_header *mirror_header(void) {
  for (uint64_t off = 0; off + sizeof(struct a_header) <= g_size; off += 8)
    if (*(uint64_t *)(g_mirror + off) == AGENT_MAGIC)
      return (struct a_header *)(g_mirror + off);
  return NULL;
}

/* Re-point slots that something re-bound (a module load). */
static void repatch(pid_t pid) {
  struct a_header *hdr = NULL;
  /* the header address in the mirror never moves */
  for (uint64_t off = 0; off + sizeof *hdr <= g_size; off += 8) {
    if (*(uint64_t *)(g_mirror + off) == AGENT_MAGIC) {
      hdr = (struct a_header *)(g_mirror + off);
      break;
    }
  }
  if (!hdr)
    return;
  struct a_hook *hk = (struct a_hook *)G2M(hdr->hooks);
  int n = 0;
  for (int i = 0; hk[i].nid; i++) {
    if (!hk[i].slot)
      continue;
    uint64_t cur = 0;
    if (kernel_proc_copyout(pid, hk[i].slot, &cur, 8) != 0 || cur == hk[i].hook)
      continue;
    /* keep the newly bound function as the original (orig is a game address) */
    kernel_proc_copyin(pid, &cur, hk[i].orig, 8);
    *(uint64_t *)G2M(hk[i].orig) = cur;
    kernel_proc_copyin(pid, &hk[i].hook, hk[i].slot, 8);
    n++;
  }
  if (n) /* one line a pass (one a slot was ~100 lines at every module the game loads) */
    say("repatched %d import slots a module load re-bound", n);
}

/* ---- one instance at a time: a newer injector asks the running one to leave ----
 * The control port listens on every interface: a connection saying "quit" ends the instance, one
 * saying nothing (a probe: is one resident?) is answered "yes". So a sender can ask the resident
 * instance to leave and wait for it to be gone BEFORE loading the next: loading a payload while
 * one is resident brought the console down (three times in fifteen, before the new one's first
 * line, which comes before it touches this port; two processes using the jailbreak's kernel
 * access at once, the resident one scanning for the game every 5 ms, is the likeliest cause).
 * A newer instance also asks the older one itself, and leaves if the port stays busy. */
#define CTL_PORT 9088
static int g_ctl = -1;

/* Test runs (tools/testrun.sh): the game started and closed from the control port, by the
 * system's launcher (libSceSystemService; the parameters as PS5 payloads that start titles
 * pass them). */
struct lnc_app_param {
  uint32_t size;
  int32_t user_id;
  uint32_t app_opt;
  uint64_t crash_report;
  uint64_t check_flag;
};
int sceLncUtilLaunchApp(const char *tid, const char *argv[], struct lnc_app_param *p);
int sceSystemServiceLaunchApp(const char *tid, const char *argv[], struct lnc_app_param *p);
uint32_t sceLncUtilGetAppIdOfRunningBigApp(void);
int sceLncUtilGetAppTitleId(uint32_t app_id, char *title_id);
int sceLncUtilKillApp(uint32_t app_id);
int sceUserServiceInitialize(void *param);
int sceUserServiceGetForegroundUser(int *user_id);

/* On a thread of its own, the loop waiting for the game standing by until the launcher has
 * returned: a process it is still making can't be attached to (and is found by the loop
 * before that; the entry point is still ahead when the launcher returns). */
static volatile int g_launching;
static volatile time_t g_launched_at; /* the last launch from here (inject: its first image ends) */
static char g_launch_tid[10] = "CUSA07434"; /* `launch [title id]`: one of k_builds' */

static void *launch_game(void *arg) {
  (void)arg;
  int user = -1;
  sceUserServiceInitialize(NULL);
  int ur = sceUserServiceGetForegroundUser(&user);
  struct lnc_app_param p = {sizeof p, user, 0, 0, 0};
  const char *argv[] = {NULL};
  int rc = sceLncUtilLaunchApp(g_launch_tid, argv, &p);
  if (rc < 0)
    rc = sceSystemServiceLaunchApp(g_launch_tid, argv, &p);
  say("launch: %s, user %d (0x%x), rc 0x%x", g_launch_tid, user, (unsigned)ur, (unsigned)rc);
  g_launching = 0;
  return NULL;
}

static int start_launch(void) {
  pthread_t t;
  g_launching = 1;
  g_launched_at = time(NULL);
  int rc = pthread_create(&t, NULL, launch_game, NULL);
  if (rc == 0)
    pthread_detach(t);
  else
    g_launching = 0;
  return rc;
}

/* The processes (pid, parent, name) to the control port's `ps`: a stray injector shows here. */
static void list_procs(int c) {
  int mib[4] = {1, 14, 8, 0};
  size_t size = 0;
  if (sysctl(mib, 4, NULL, &size, NULL, 0) != 0)
    return;
  uint8_t *buf = malloc(size);
  if (!buf || sysctl(mib, 4, buf, &size, NULL, 0) != 0) {
    free(buf);
    return;
  }
  char line[64];
  for (uint8_t *p = buf; p + 512 <= buf + size;) {
    int structsize = *(int *)p;
    if (structsize <= 0)
      break;
    snprintf(line, sizeof line, "%d %d %.19s\n", *(pid_t *)&p[72], *(pid_t *)&p[76],
             (const char *)&p[447]);
    write(c, line, strlen(line));
    p += structsize;
  }
  free(buf);
}

static volatile pid_t g_live_pid; /* the game being served, for kill_game's soft stop */
struct a_shared_head;
static void relay_log(pid_t pid, uint64_t shared, const struct a_shared_head *sh);
static volatile uint64_t g_live_shared;
static int kill_game(void) {
  uint32_t id = sceLncUtilGetAppIdOfRunningBigApp();
  char tid[16] = "";
  int rc = -1;
  /* The PS5 VR stack inside the game shut down first (Hmd2's VR mode off, the hand tracker
   * stopped): killed outright, a game leaves the kernel whatever those keep, and four of the five
   * console crashes came around the tenth run after a jailbreak. The agent is told CMD_QUIT and
   * answers STOP_DONE in cmd_arg[3] (up to 3 s). */
  pid_t lp = g_live_pid;
  uint64_t ls = g_live_shared;
  if (lp > 0 && ls) {
    uint32_t quit = 1;
    uint64_t done = 0;
    kernel_proc_copyin(lp, &quit, ls + offsetof(struct a_shared_head, cmd), 4);
    for (int i = 0; i < 300; i++) {
      usleep(10000);
      if (kernel_proc_copyout(lp, ls + offsetof(struct a_shared_head, cmd_arg) + 24, &done, 8) != 0 ||
          done == 0x51554954ull)
        break;
    }
    say("kill: the agent's stop %s", done == 0x51554954ull ? "done" : "not answered in 3 s");
    struct a_shared_head sh;
    if (kernel_proc_copyout(lp, ls, &sh, sizeof sh) == 0)
      relay_log(lp, ls, &sh); /* its last lines (the VR mode's rc, the hand tracker's) */
  }
  if (id && sceLncUtilGetAppTitleId(id, tid) == 0 && title_known(tid))
    rc = sceLncUtilKillApp(id);
  say("kill: app 0x%x (%s), rc 0x%x", id, tid, (unsigned)rc);
  return rc;
}

static int ctl_open(void) {
  struct sockaddr_in a, lo;
  memset(&a, 0, sizeof a);
  a.sin_family = AF_INET;
  a.sin_port = htons(CTL_PORT);
  a.sin_addr.s_addr = htonl(INADDR_ANY);
  lo = a;
  lo.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  for (int i = 0; i < 100; i++) {
    int s = socket(AF_INET, SOCK_STREAM, 0), one = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    if (bind(s, (struct sockaddr *)&a, sizeof a) == 0 && listen(s, 4) == 0) {
      fcntl(s, F_SETFL, O_NONBLOCK);
      return s;
    }
    close(s);
    if (i == 0) { /* an older injector holds the port: ask it to quit */
      int c = socket(AF_INET, SOCK_STREAM, 0);
      if (connect(c, (struct sockaddr *)&lo, sizeof lo) == 0)
        write(c, "quit\n", 5);
      close(c);
    }
    usleep(100000);
  }
  return -1;
}

/* Someone wants this instance to leave: a connection that says "quit". A connection that says
 * nothing is a probe, answered "yes". Only asked where leaving is safe (not ptrace-attached). */
static int ctl_quit_requested(int remote_ok) {
  if (g_ctl < 0)
    return 0;
  struct sockaddr_in peer;
  socklen_t plen = sizeof peer;
  memset(&peer, 0, sizeof peer);
  int c = accept(g_ctl, (struct sockaddr *)&peer, &plen);
  if (c < 0)
    return 0;
  /* the accepted socket may be non-blocking like the listener: wait for what the client says */
  struct pollfd pfd = {c, POLLIN, 0};
  char buf[32] = {0};
  ssize_t n = poll(&pfd, 1, 300) > 0 ? recv(c, buf, sizeof buf - 1, 0) : -1;
  int quit = n >= 4 && memcmp(buf, "quit", 4) == 0;
  int local = peer.sin_addr.s_addr == htonl(INADDR_LOOPBACK);
  char reply[48];
#ifdef RELEASE
  /* a release build: the test runner's commands (launch, kill, ps) are not served; anyone on the
   * network reaches this port */
  if ((n >= 6 && memcmp(buf, "launch", 6) == 0) || (n >= 4 && memcmp(buf, "kill", 4) == 0) ||
      (n >= 2 && memcmp(buf, "ps", 2) == 0)) {
    write(c, "not in a release build\n", 23);
  } else
#endif
  if (n >= 6 && memcmp(buf, "launch", 6) == 0) {
    /* a test run: the game started here, while none is served; `launch CUSA11682` another of
     * the known titles (Hikari without one) */
    char want[16] = "CUSA07434";
    if (sscanf(buf + 6, " %15s", want) < 1)
      strcpy(want, "CUSA07434");
    if (strlen(want) != 9 || !title_known(want))
      snprintf(reply, sizeof reply, "launch: %.9s is not a known title\n", want);
    else if (remote_ok) {
      memcpy(g_launch_tid, want, 10);
      snprintf(reply, sizeof reply, "launch %s\n", start_launch() == 0 ? "started" : "failed");
    } else
      snprintf(reply, sizeof reply, "the game is running\n");
    write(c, reply, strlen(reply));
  } else if (n >= 4 && memcmp(buf, "kill", 4) == 0) {
    int p = 0;
    if (sscanf(buf + 4, "%d", &p) == 1 && p > 1) /* `kill <pid>`: a process, outright */
      snprintf(reply, sizeof reply, "kill %d: %d\n", p, kill(p, SIGKILL));
    else
      snprintf(reply, sizeof reply, "kill 0x%x\n", (unsigned)kill_game());
    write(c, reply, strlen(reply));
  } else if (n >= 2 && memcmp(buf, "ps", 2) == 0) {
    list_procs(c);
  } else if (quit && !local && !remote_ok) {
    /* a game is being served: a remote quit now would pull the libraries from under it */
    write(c, "the game is running\n", 20);
    quit = 0;
  } else if (!quit && n >= 0) {
    write(c, "yes\n", 4); /* a probe: one is resident (nothing on a connection that failed) */
  }
  close(c);
  return quit;
}

int sceKernelGetAppInfo(int pid, void *info);

/* A value from a param.sfo (string entries only). */
static int sfo_get(const uint8_t *sfo, size_t size, const char *key, char *out, size_t outsz) {
  if (size < 20 || memcmp(sfo, "\0PSF", 4) != 0)
    return -1;
  uint32_t keys = *(const uint32_t *)(sfo + 8), data = *(const uint32_t *)(sfo + 12);
  uint32_t n = *(const uint32_t *)(sfo + 16);
  for (uint32_t i = 0; i < n && 20 + i * 16 + 16 <= size; i++) {
    const uint8_t *e = sfo + 20 + i * 16;
    uint32_t koff = keys + *(const uint16_t *)e, len = *(const uint32_t *)(e + 4);
    uint32_t doff = data + *(const uint32_t *)(e + 12);
    if (koff >= size || doff + len > size || strncmp((const char *)sfo + koff, key, size - koff))
      continue;
    if (len >= outsz)
      len = outsz - 1;
    memcpy(out, sfo + doff, len);
    out[len] = 0;
    return 0;
  }
  return -1;
}

/* The title and version of a running app, from its param.sfo (/system_data/priv/appmeta). */
static int app_title(pid_t pid, char tid[10], char *title, size_t tsz, char *ver, size_t vsz) {
  uint8_t info[0x80];
  char path[96];
  memset(info, 0, sizeof info);
  if (sceKernelGetAppInfo(pid, info) != 0)
    return -1;
  memcpy(tid, info + 0x10, 9);
  tid[9] = 0;
  snprintf(path, sizeof path, "/system_data/priv/appmeta/%s/param.sfo", tid);
  static uint8_t sfo[16384];
  FILE *f = fopen(path, "rb");
  size_t n = f ? fread(sfo, 1, sizeof sfo, f) : 0;
  if (f)
    fclose(f);
  title[0] = 0, ver[0] = 0;
  sfo_get(sfo, n, "TITLE", title, tsz);
  sfo_get(sfo, n, "APP_VER", ver, vsz);
  return 0;
}

/* Another Summer Lesson (another region or version) is told about on screen once, as the payload
 * otherwise just waits. Checked a few seconds after it starts, when its image is certainly in
 * place. */
static void check_other_title(pid_t pid) {
  static pid_t seen[8];
  static time_t first[8];
  static unsigned next;
  time_t now = time(NULL);
  unsigned k;
  for (k = 0; k < 8 && seen[k] != pid; k++)
    ;
  if (k == 8) {
    k = next++ % 8;
    seen[k] = pid, first[k] = now;
    return;
  }
  if (first[k] == 0 || now - first[k] < 3)
    return;
  first[k] = 0; /* checked */
  char tid[10], title[128], ver[16];
  if (app_title(pid, tid, title, sizeof title, ver, sizeof ver) != 0 ||
      !strcasestr(title, TITLE_WORD))
    return;
  say("%s (%s, version %s) is not the build this payload knows", title, tid, ver);
  const char *v = ver[0] == '0' && ver[1] ? ver + 1 : ver; /* "01.00" -> "1.00" */
  notify("SummerLessonVR2: this Summer Lesson is %s version %s. Only " BUILDS_TEXT
         " work for now.", tid, v);
}

/* The game: the eboot.bin of a title and version in k_builds (its param.sfo), once its image is mapped
 * (its first page readable, holding the interpreter's name every PS4 eboot starts with), so that
 * the match comes from the first instructions on, before the game can open its setup dialog. The
 * version is read once per process. */
static pid_t find_game(pid_t skip) {
  static pid_t ver_pid = -1;
  static int ver_ok, ver_build = -1;
  int mib[4] = {1, 14, 8, 0};
  size_t size = 0;
  pid_t found = -1;
  if (sysctl(mib, 4, NULL, &size, NULL, 0) != 0)
    return -1;
  uint8_t *buf = malloc(size);
  if (!buf || sysctl(mib, 4, buf, &size, NULL, 0) != 0) {
    free(buf);
    return -1;
  }
  for (uint8_t *p = buf; p < buf + size;) {
    int structsize = *(int *)p;
    pid_t pid = *(pid_t *)&p[72];
    const char *name = (const char *)&p[447];
    if (structsize <= 0)
      break;
    p += structsize;
    uint8_t code[16], info[0x80];
    if (pid == skip || strcmp(name, "eboot.bin") != 0)
      continue;
    /* Every app is an eboot.bin. Its title, from the system, before any walk through its record
     * in the kernel: a process that is starting or ending has records being made or freed, and
     * reading those can bring the whole console down. */
    memset(info, 0, sizeof info);
    if (sceKernelGetAppInfo(pid, info) != 0 || !title_known((const char *)info + 0x10)) {
      check_other_title(pid);
      continue;
    }
    if (pid != ver_pid) {
      char tid[10], title[128], ver[16];
      ver_pid = pid;
      ver_build = app_title(pid, tid, title, sizeof title, ver, sizeof ver) == 0
                      ? build_index(tid, ver)
                      : -1;
      ver_ok = ver_build >= 0;
      say("%s (%s version %s): %s", title, tid, ver, ver_ok ? "a build this payload knows"
                                                            : "not a version this payload knows");
    }
    if (!ver_ok) {
      check_other_title(pid);
      continue;
    }
    /* its image is in place: every PS4 eboot begins with its interpreter's name */
    uint64_t base = eboot_mapbase(pid);
    if (base && kernel_proc_copyout(pid, base, code, sizeof code) == 0 &&
        memcmp(code, "/libexec/ld-elf", 15) == 0)
      found = pid, g_build = ver_build;
  }
  free(buf);
  return found;
}

/* The system's AV control service lets an app switch the output to VR only when its app info
 * carries attribute3 bit 0x400 (PS VR2), which a PS4 title never has. The kernel keeps the app
 * info in the process record (proc+0x460 on FW 13.60): it is found by its contents and the bit
 * is set there, so the service sees a PS VR2 app. */
static void mark_vr2_app(pid_t pid) {
  uint8_t info[0x80];
  static uint8_t pr[0x1000];
  memset(info, 0, sizeof info);
  if (sceKernelGetAppInfo(pid, info) != 0) {
    say("appinfo: GetAppInfo failed");
    return;
  }
  intptr_t proc = kernel_get_proc(pid);
  if (!proc || kernel_copyout(proc, pr, sizeof pr) != 0)
    return;
  int at = -1, hits = 0;
  for (int off = 0x20; off + 0x30 <= (int)sizeof pr; off += 4)
    if (!memcmp(pr + off, info + 0x20, 0x30))
      at = off - 0x20, hits++;
  if (hits != 1) {
    say("appinfo: %d matches in the process record, not marked", hits);
    return;
  }
  uint32_t attr3 = *(uint32_t *)(pr + at + 0x4c), want = attr3 | 0x400;
  if (want != attr3)
    kernel_copyin(&want, proc + at + 0x4c, 4);
  memset(info, 0, sizeof info);
  sceKernelGetAppInfo(pid, info);
  say("appinfo: at proc+0x%x, attribute3 0x%x -> 0x%x", at, attr3, *(uint32_t *)(info + 0x4c));
}

static int game_alive(pid_t pid, intptr_t proc) {
  return kill(pid, 0) == 0 && kernel_get_proc(pid) == proc;
}

/* The AV control service (SceSysAvControl.elf, image at VA 0x400000) refuses the PS VR2 output
 * for a PS4 (BC) title in two places. Both are patched while the game runs and restored when the
 * game ends or this injector exits. Addresses below are FW 13.60's; each firmware profile
 * (common/firmware.h) carries its own sites and original bytes, checked before patching.
 * 1. Its layout handler rejects the VR layout ("param.json does not have flags") by one branch,
 *    `je 0x41749e` at 0x41739b: replaced by a 6-byte NOP so it falls through to the accept path.
 * 2. checkVideoModeForChangeOutput (0x431e70) then refuses the VR video mode (res 0xe1ffffff,
 *    2040p 119.88 Hz) against the app's capability flags (AvManager:5387, 0x802a000e). After its
 *    "is this mode valid at all" check, `xor eax,eax; jmp 0x431f64` at 0x431ed8 returns 0. */
#define k_av_sites (g_fw->av)
#define AV_NSITES 2u
static pid_t g_av_pid = -1;
static int g_av_patched;

static pid_t find_proc(const char *name) {
  int mib[4] = {1, 14, 8, 0};
  size_t size = 0;
  pid_t found = -1;
  if (sysctl(mib, 4, NULL, &size, NULL, 0) != 0)
    return -1;
  uint8_t *buf = malloc(size);
  if (!buf || sysctl(mib, 4, buf, &size, NULL, 0) != 0) {
    free(buf);
    return -1;
  }
  for (uint8_t *p = buf; p < buf + size;) {
    int structsize = *(int *)p;
    pid_t pid = *(pid_t *)&p[72];
    const char *n = (const char *)&p[447];
    if (structsize <= 0)
      break;
    p += structsize;
    if (strcmp(n, name) == 0)
      found = pid;
  }
  free(buf);
  return found;
}

static void patch_av_service(void) {
  if (g_av_patched)
    return;
  g_av_pid = find_proc("SceSysAvControl.elf");
  if (g_av_pid < 0) {
    say("avservice: not found");
    return;
  }
  /* all sites must hold the expected bytes (or our patch, after a crash) before any is written */
  for (unsigned i = 0; i < AV_NSITES; i++) {
    const struct av_site *t = &k_av_sites[i];
    uint8_t cur[8];
    if (kernel_proc_copyout(g_av_pid, t->va, cur, t->len) != 0) {
      say("avservice: read 0x%lx failed", (unsigned long)t->va);
      return;
    }
    if (memcmp(cur, t->orig, t->len) != 0 && memcmp(cur, t->patch, t->len) != 0) {
      say("avservice: 0x%lx holds unexpected bytes (%02x %02x %02x ...) - not patching",
          (unsigned long)t->va, cur[0], cur[1], cur[2]);
      return;
    }
  }
  for (unsigned i = 0; i < AV_NSITES; i++)
    kernel_proc_copyin(g_av_pid, k_av_sites[i].patch, k_av_sites[i].va, k_av_sites[i].len);
  g_av_patched = 1;
  say("avservice: pid %d VR layout and output-mode gates opened", g_av_pid);
}

/* FW 11.40/11.60 (av_record): the AV service builds its per-app record (pid, appid, flags; 12 bytes) once, at
 * launch, before attribute3 is marked above, so it sees a PS VR1 title (flag 0x200000, no 0x10)
 * and keeps the headset in the cinematic USB layout. Once the game runs, the record for its pid
 * is found in the service's writable memory and changed to a PS VR2 title: 0x200000 cleared,
 * 0x10 set (the two bits that differ from a native PS VR2 title's record, besides an unrelated
 * SDK-version bit). Only that one record is touched and it disappears with the app. The service
 * is not patched for this and nothing is persisted. Returns 1 once a record was changed. */
#define AVREC_VR1 0x200000u
#define AVREC_VR2 0x10u
static int g_avrec_done;

#define AVREC_MAX 2 /* the service keeps two copies of the record (its data and its heap) */

static int av_mark_vr2_record(pid_t game) {
  /* On FW 13.60 marking attribute3 is enough, so its record is left alone. */
  if (g_avrec_done || g_av_pid < 0 || !g_fw->av_record)
    return g_avrec_done;
  struct {
    uint64_t va;
    uint32_t appid, flags;
  } cand[AVREC_MAX + 1];
  int mib[4] = {1, 14, 32, g_av_pid}; /* KERN_PROC_VMMAP */
  size_t size = 0;
  if (sysctl(mib, 4, NULL, &size, NULL, 0) != 0 || !size) {
    say("avrecord: vmmap query failed (errno %d)", errno);
    return 0;
  }
  size += size / 4;
  uint8_t *map = malloc(size);
  static uint8_t chunk[0x100000 + 16];
  if (!map || sysctl(mib, 4, map, &size, NULL, 0) != 0) {
    free(map);
    say("avrecord: vmmap read failed (errno %d)", errno);
    return 0;
  }
  int hits = 0;
  unsigned long scanned = 0;
  for (uint8_t *e = map; e + 64 <= map + size;) {
    int es = *(int *)e;
    if (es <= 0)
      break;
    uint64_t start = *(uint64_t *)(e + 8), end = *(uint64_t *)(e + 16);
    int prot = *(int *)(e + 56);
    e += es;
    if ((prot & 3) != 3 || end <= start || end - start > 0x10000000 || scanned > 0x20000000)
      continue;
    for (uint64_t a = start; a < end; a += 0x100000) {
      size_t n = end - a > 0x100000 ? 0x100000 : (size_t)(end - a);
      size_t rd = n + 16 < end - a ? n + 16 : (size_t)(end - a);
      if (kernel_proc_copyout(g_av_pid, a, chunk, rd) != 0)
        continue;
      scanned += n;
      for (size_t o = 0; o + 12 <= rd && o < n; o += 4) {
        uint32_t pid = *(uint32_t *)(chunk + o), fl = *(uint32_t *)(chunk + o + 8);
        if (pid != (uint32_t)game || (fl & 0x2e0000) != 0x2e0000 || !(fl & AVREC_VR1) ||
            (fl & AVREC_VR2))
          continue;
        if (hits <= AVREC_MAX)
          cand[hits].va = a + o + 8, cand[hits].appid = *(uint32_t *)(chunk + o + 4),
          cand[hits].flags = fl;
        hits++;
      }
    }
  }
  free(map);
  if (!hits) {
    say("avrecord: no record for pid %d in %lu bytes", game, scanned);
    return 0;
  }
  /* Nothing is written unless every match is the same app's record: a stray match elsewhere in
   * the service would otherwise be corrupted. */
  int same = hits <= AVREC_MAX;
  for (int i = 1; same && i < hits; i++)
    same = cand[i].appid == cand[0].appid && cand[i].flags == cand[0].flags;
  if (!same) {
    say("avrecord: %d candidate records for pid %d disagree or are too many, not changed", hits,
        game);
    g_avrec_done = 1; /* don't rescan: the result would be the same */
    return 0;
  }
  for (int i = 0; i < hits; i++) {
    uint32_t want = (cand[i].flags & ~AVREC_VR1) | AVREC_VR2, back = 0;
    kernel_proc_copyin(g_av_pid, &want, cand[i].va, 4);
    kernel_proc_copyout(g_av_pid, cand[i].va, &back, 4);
    say("avrecord: pid %d record at 0x%lx (appid 0x%x): flags 0x%x -> 0x%x", game,
        (unsigned long)(cand[i].va - 8), cand[i].appid, cand[i].flags, back);
  }
  g_avrec_done = 1;
  return 1;
}

static void restore_av_service(void) {
  g_avrec_done = 0;
  if (!g_av_patched || g_av_pid < 0)
    return;
  if (kill(g_av_pid, 0) != 0) {
    g_av_patched = 0;
    return;
  }
  for (unsigned i = 0; i < AV_NSITES; i++)
    kernel_proc_copyin(g_av_pid, k_av_sites[i].orig, k_av_sites[i].va, k_av_sites[i].len);
  say("avservice: gates restored");
  g_av_patched = 0;
}

static void reset_game_state(void) {
  free(g_mirror);
  g_mirror = NULL;
  free(g_slots);
  g_slots = NULL;
  g_nslots = 0;
  g_base = g_size = g_hdroff = 0;
  pt_tid = 0;
}

/* ---- when no thread is in game code: traps ----
 * Once the game has opened the PS VR setup dialog (if the agent wasn't in yet), its threads wait
 * in system calls and an attach doesn't catch one in game code. The eboot's import slots of
 * functions the game calls every frame, and of its first PS VR calls, are then pointed at
 * unmapped addresses and the game runs on: the first such call faults on its trap, which stops
 * that thread in user mode at a call boundary in game code, as the agent's mapping and boot
 * call need. Then every slot is put back, and each thread at a trap goes on to what its slot
 * now holds (the agent's hook, or the original function). */
static void move_off_traps(pid_t pid, const uint64_t *slot, const uint64_t *orig) {
  lwpid_t lwps[512];
  int n = pt_lwps(pid, lwps, 512);
  pid_t keep = pt_tid;
  for (int i = 0; i < n; i++) {
    struct reg r;
    int k;
    pt_tid = lwps[i];
    if (pt_getregs(pid, &r) == 0 && (k = trap_index(r.r_rip)) >= 0 && slot[k]) {
      /* on to what the slot holds now: the agent's hook once the agent is in, so the call that
       * fired (often the game's first PS VR call) is answered by the agent */
      uint64_t now = 0;
      if (pt_copyout(pid, slot[k], &now, 8) != 0 || trap_index(now) >= 0 || !now)
        now = orig[k];
      r.r_rip = now;
      pt_setregs(pid, &r);
    }
  }
  pt_tid = keep;
}

/* A thread that took a trap at the same time as the one that fired still has its SIGSEGV
 * queued; delivered after the detach, it would kill the game (rel4: the fault address a trap,
 * the thread long since moved on). Let the game run traced for a moment and drop every fault on
 * a trap page; other signals go through. Returns with the game stopped, ready to detach. */
static void drop_trap_faults(pid_t pid, const uint64_t *slot, const uint64_t *orig) {
  int pass = 0, stopping = 0;
  for (int round = 0; round < 64; round++) {
    pt_tid = 0;
    if (pt_continue(pid, pass) != 0)
      return;
    pass = 0;
    int st = 0;
    pid_t w = 0;
    for (int t = 0; !stopping && t < 200 && (w = waitpid(pid, &st, WNOHANG)) == 0; t++)
      usleep(1000);
    if (w == 0) { /* quiet for 200 ms: stop it to detach */
      kill(pid, SIGSTOP);
      stopping = 1;
      w = waitpid(pid, &st, 0);
    }
    if (w != pid || !WIFSTOPPED(st))
      return;
    int sig = WSTOPSIG(st);
    uint64_t addr = 0;
    if (sig == SIGSEGV && pt_event_siginfo(pid, &addr) == SIGSEGV && trap_index(addr) >= 0) {
      say("trap: dropped a queued fault on the %s trap", k_trap_nids[trap_index(addr)]);
      move_off_traps(pid, slot, orig);
      continue;
    }
    if (sig == SIGSTOP) {
      if (stopping)
        return;
      continue; /* the system's own stops */
    }
    pass = sig;
  }
}

/* Puts back the slots that still hold their trap. One the dynamic linker filled in meanwhile (its
 * module loaded while the traps were armed) is left as it is: the value saved when arming is the
 * "not resolved" stub then, and writing it back crashes the game (PRX_NOT_RESOLVED_FUNCTION). */
static void put_slots_back(pid_t pid, const uint64_t *slot, const uint64_t *orig, int stopped) {
  for (unsigned i = 0; i < NTRAPS; i++) {
    uint64_t now = 0, trap = TRAP_BASE + i * 0x1000;
    if (!slot[i])
      continue;
    if (stopped ? pt_copyout(pid, slot[i], &now, 8) != 0
                : kernel_proc_copyout(pid, slot[i], &now, 8) != 0)
      continue;
    if (now != trap)
      continue;
    if (stopped)
      pt_copyin(pid, &orig[i], slot[i], 8);
    else
      kernel_proc_copyin(pid, &orig[i], slot[i], 8);
  }
}

/* The game is attached and stopped. 0 = agent loaded, -1 = no trap fired in 20 s or the load
 * failed, -3 = the game ended. Leaves the game stopped (unless it ended). */
static int inject_by_trap(pid_t pid) {
  uint64_t slot[NTRAPS], orig[NTRAPS];
  int armed = 0;
  for (unsigned i = 0; i < NTRAPS; i++) {
    uint64_t trap = TRAP_BASE + i * 0x1000;
    slot[i] = slot_of(k_trap_nids[i]);
    orig[i] = 0;
    if (!slot[i] || pt_copyout(pid, slot[i], &orig[i], 8) != 0 ||
        (g_unresolved && orig[i] == g_unresolved) || /* its module isn't loaded yet */
        pt_copyin(pid, &trap, slot[i], 8) != 0) {
      slot[i] = 0;
      continue;
    }
    armed++;
  }
  say("trap: %d import slots armed (unresolved stub 0x%lx)", armed, (long)g_unresolved);
  int sig = 0, hit = -1, st = 0;
  for (int waited = 0; hit < 0;) {
    pt_tid = 0;
    if (pt_continue(pid, sig) != 0)
      break;
    pid_t w = 0;
    for (; waited < 20000; waited++) {
      if ((w = waitpid(pid, &st, WNOHANG)) == -1 && errno == EINTR)
        w = 0; /* a signal to this payload, not the game's news */
      if (w != 0)
        break;
      usleep(1000);
    }
    if (w == 0) { /* nothing fired: stop the game again to put the slots back */
      int still = 0;
      for (unsigned i = 0; i < NTRAPS; i++) {
        uint64_t now = 0;
        still += slot[i] && kernel_proc_copyout(pid, slot[i], &now, 8) == 0 &&
                 now == TRAP_BASE + i * 0x1000;
      }
      say("trap: nothing fired in 20 s (%d of %d traps still in place)", still, armed);
      kill(pid, SIGSTOP);
      for (int t = 0; t < 3000 && (w = waitpid(pid, &st, WNOHANG)) == 0; t++)
        usleep(1000);
      if (w == 0) { /* it can't be stopped: put the slots back without stopping it */
        put_slots_back(pid, slot, orig, 0);
        say("trap: the game didn't stop, slots put back");
        return -2;
      }
      break;
    }
    if (w != pid || !WIFSTOPPED(st)) {
      if (w == pid)
        say("trap: game ended (status 0x%x)", st);
      else
        say("trap: game lost (wait errno %d)", errno);
      return -3;
    }
    sig = WSTOPSIG(st);
    pt_tid = pt_event_lwp(pid);
    struct reg r;
    if (pt_tid > 0 && pt_getregs(pid, &r) == 0 && (hit = trap_index(r.r_rip)) >= 0) {
      say("trap: thread %d called %s", pt_tid, k_trap_nids[hit]);
      break;
    }
    if (sig == SIGSTOP) /* the system's own stops while the game starts */
      sig = 0;
  }
  put_slots_back(pid, slot, orig, 1);
  /* Every thread at a trap goes on to the real function before anything runs: left there, one
   * the agent's boot call lets run would fault again and again. */
  move_off_traps(pid, slot, orig);
  int rc = hit >= 0 ? load_agent(pid) : -1;
  move_off_traps(pid, slot, orig);
  drop_trap_faults(pid, slot, orig);
  pt_tid = 0;
  return rc;
}

/* inject: the agent goes in at the eboot's entry point (inject_at_entry). When that has passed
 * (this payload was sent with the game running), it goes in through the traps (inject_by_trap):
 * the import slots of the game's first PS VR calls and of calls it makes all the time point at
 * unmapped pages, and the first thread that makes one stops in game code at a call boundary,
 * where the agent is mapped and booted, and that call goes on into the agent's hook. If the
 * game's dialog is up by then, the agent's dialog hooks close it. 0 = in, -2 = the game can't be
 * stopped (too late), -1 = failed. */
/* The value an import slot holds while its module isn't loaded yet (g_unresolved): the "not
 * resolved" stub, the one value many slots share (a bound slot holds its own function). */

static int cmp_u64(const void *a, const void *b) {
  uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
  return x < y ? -1 : x > y;
}

static void find_unresolved(pid_t pid) {
  g_unresolved = 0;
  uint64_t *v = calloc(g_nslots ? g_nslots : 1, 8);
  int n = 0;
  if (!v)
    return;
  for (int k = 0; k < g_nslots; k++)
    if (kernel_proc_copyout(pid, g_slots[k].slot, &v[n], 8) == 0)
      n++;
  qsort(v, n, 8, cmp_u64);
  int best = 0;
  for (int i = 0, j; i < n; i = j) {
    for (j = i; j < n && v[j] == v[i]; j++)
      ;
    if (j - i > best && v[i])
      best = j - i, g_unresolved = v[i];
  }
  if (best < 4)
    g_unresolved = 0;
  free(v);
}

/* the game's own calls are bound: sceKernelUsleep's slot no longer holds the stub */
static int slots_bound(pid_t pid) {
  uint64_t v = 0, s = slot_of("1jfXLRVzisc"); /* sceKernelUsleep */
  if (!s)
    s = slot_of("4J2sUJmuHZQ"); /* sceKernelGetProcessTime */
  if (!s || pt_copyout(pid, s, &v, 8) != 0 || v < 0x100000000ull)
    return 0;
  find_unresolved(pid);
  return v != g_unresolved;
}

/* ---- at the entry point ----
 * The first attach comes while the game is still in libkernel's start code, with its modules
 * already linked. Traps armed there never fired in 1.0.2 (on two consoles): the game reached its
 * "connect PS VR" dialog first, and the agent only got in 20 s later on the re-armed traps, with
 * ShellUI's dialog on screen. A breakpoint on the eboot's entry point is code, not an import
 * slot: the main thread stops there before any of the game's own code has run, the agent goes in
 * on it, and its dialog hooks answer the game's first dialog call, so no real dialog opens. */
static uint64_t eboot_entry(pid_t pid) {
  uint8_t info[0x80], b[0x1000];
  char tid[10], path[96];
  memset(info, 0, sizeof info);
  if (sceKernelGetAppInfo(pid, info) != 0)
    return 0;
  memcpy(tid, info + 0x10, 9);
  tid[9] = 0;
  /* A patched game runs the patch's eboot, mounted beside the base game's as -patch0 (its version
   * is then not 1.00, and the game is refused) */
  snprintf(path, sizeof path, "/mnt/sandbox/pfsmnt/%s-patch0/eboot.bin", tid);
  int fd = open(path, O_RDONLY);
  if (fd < 0) {
    snprintf(path, sizeof path, "/mnt/sandbox/pfsmnt/%s-app0/eboot.bin", tid);
    fd = open(path, O_RDONLY);
  }
  if (fd < 0)
    return 0;
  say("entry: from %s", path);
  ssize_t n = read(fd, b, sizeof b);
  close(fd);
  size_t eo = 0;
  if (n >= 0x20 && *(uint32_t *)b == 0x1D3D154F) /* SELF: the ELF header follows its segment table */
    eo = 0x20 + 32 * (size_t) * (uint16_t *)&b[0x18];
  if (n < 0 || eo + 0x40 > (size_t)n || memcmp(b + eo, "\x7f" "ELF", 4) != 0)
    return 0;
  const Elf64_Ehdr *eh = (const Elf64_Ehdr *)(b + eo);
  uint64_t lo = ~0ull;
  for (int i = 0; i < eh->e_phnum; i++) {
    size_t po = eo + eh->e_phoff + (size_t)i * sizeof(Elf64_Phdr);
    if (po + sizeof(Elf64_Phdr) > (size_t)n)
      return 0;
    const Elf64_Phdr *ph = (const Elf64_Phdr *)(b + po);
    if (ph->p_type == PT_LOAD && ph->p_vaddr < lo)
      lo = ph->p_vaddr;
  }
  uint64_t base = eboot_mapbase(pid);
  return !base || lo == ~0ull ? 0 : base + eh->e_entry - lo;
}

/* ---- the resolution (resolution.txt) ----
 * The game draws both eyes side by side into one texture sized by its VR plugin's
 * CalculateRenderTargetSize (0x2f8fb20): its 1920x1080 screen times r.ScreenPercentage (130 from
 * its settings), so 2496x1404, 1248x1404 an eye; the panel the HMD reports plays no part (it
 * only reaches GetMonitorInfo). resolution.txt giving an eye width (960 to 2048) sets the
 * percentage that makes eyes that wide (the agent writes the cvar once frames run: hooks.c
 * gpu_label_thread). Without the file, 2000 (2026-11-04: at 2048 the 4-minute replay ran clean but
 * a 23-minute session outgrew even the reserve's spare memory, its GPU pool taking 13 blocks, and
 * the game aborted; at 1920 17 minutes needed none, at 2000 the replay two); "stock" in it, the
 * game's own 1248x1404. Read at load: the agent's header. */
#define RESOLUTION_DEFAULT 2000
static uint32_t resolution_pct(void) {
  char path[96], line[64] = {0};
  snprintf(path, sizeof path, OUT_DIR "/resolution.txt");
  FILE *f = fopen(path, "r");
  int had_file = f != NULL;
  if (f) {
    if (!fgets(line, sizeof line, f))
      line[0] = 0;
    fclose(f);
  }
  unsigned w = 0;
  if (strncmp(line, "stock", 5) == 0) {
    say("resolution: the game's own eye size (r.ScreenPercentage from its settings, Hikari's 130: "
        "1248x1404; resolution.txt stock)");
    return 0;
  }
  if (!had_file || sscanf(line, "%u", &w) < 1) {
    w = RESOLUTION_DEFAULT; /* no file, or nothing in it */
  } else if (w < 960 || w > 2048) {
    say("resolution: resolution.txt (%s) is not an eye width from 960 to 2048, nor stock: %u",
        line, RESOLUTION_DEFAULT);
    w = RESOLUTION_DEFAULT;
  }
  float pct = (float)w / 9.6f;
  uint32_t bits;
  memcpy(&bits, &pct, sizeof bits);
  say("resolution: r.ScreenPercentage %.2f: eyes %ux%u (%s %u)", pct,
      (unsigned)(960.0f * pct / 100.0f + 0.999f), (unsigned)(1080.0f * pct / 100.0f + 0.999f),
      had_file ? "resolution.txt" : "the default,", w);
  return bits;
}

/* ---- the frame rate (framerate.txt) ----
 * The game's VR plugin (Unreal's Morpheus) has three frame sequences, chosen by its setting
 * PSVRFrameSequence and stored at +0x2b8 of the plugin: 1, 60 fps reprojected to 120 Hz (two
 * vblank events a frame; what it ships with), 2, native 90 Hz, 3, native 120 Hz (one vblank event
 * a frame, refresh code 0x0d). framerate.txt saying 120 changes the two stores of 1 (the
 * plugin's constructor, and its default when the setting is unset) to 3, checked against the
 * stock bytes, and the agent paces a frame a refresh with Hmd2 at full rate. Unreal's own clock
 * follows real time, so nothing of the game's timing is patched. 60 (or no file): as it ships. */
struct eboot_patch {
  uint8_t len, was[11], now[11];
};
/* The two stores, the same bytes in every build (at k_builds' fps120_at; Hikari's 0x2f8d24b and
 * 0x2f8e564). (Measurements 3-8: the engine's own limiter is not the cap. Its flags are 0x1b at
 * run time: frame-rate smoothing already force-disabled, no fixed frame rate; the patches to
 * GetMaxTickRate and UpdateTimeAndHandleMaxTickRate changed nothing and are gone.) */
static const struct eboot_patch k_fps120[2] = {
    /* the plugin's constructor: mov dword [rbx+0x2b8], 1 */
    {10, {0xc7, 0x83, 0xb8, 0x02, 0, 0, 0x01, 0, 0, 0}, {0xc7, 0x83, 0xb8, 0x02, 0, 0, 0x03, 0, 0, 0}},
    /* the setting unset or 0: mov dword [r14+0x2b8], 1 */
    {11, {0x41, 0xc7, 0x86, 0xb8, 0x02, 0, 0, 0x01, 0, 0, 0},
     {0x41, 0xc7, 0x86, 0xb8, 0x02, 0, 0, 0x03, 0, 0, 0}},
};

/* 90 and 120 are switched off for now (2026-11-03): in the menus and in play the view was not
 * world-locked at either (a fine jitter as the head moves, black lines), and the cause is not
 * found. framerate.txt saying 90 or 120 is ignored, and said so; 1 brings them back. */
#ifndef NATIVE_MODES /* a test payload: make -C payload NATIVE=1 (never the one released) */
#define NATIVE_MODES 0
#endif

static void patch_frame_rate(pid_t pid) {
  char line[32] = {0};
  int fps = 60;
  FILE *f = fopen(OUT_DIR "/framerate.txt", "r");
  if (f) {
    if (fgets(line, sizeof line, f))
      fps = atoi(line);
    fclose(f);
  }
  if ((fps == 120 || fps == 90) && !NATIVE_MODES) {
    say("frame rate: framerate.txt says %d: 90 and 120 are off in this build (the view shook at "
        "both); the game's own 60 fps, reprojected to 120 Hz", fps);
    return;
  }
  if (fps != 120 && fps != 90) {
    if (fps != 60)
      say("frame rate: framerate.txt says %d; 60, 90 or 120", fps);
    say("frame rate: the game's own 60 fps, a frame every two refreshes, reprojected to 120 Hz");
    return;
  }
  uint64_t base = pid > 0 ? eboot_mapbase(pid) : 0;
  if (!base || !g_base) {
    say("frame rate: the game had started: its own 60 fps");
    return;
  }
  if (g_build < 0) {
    say("frame rate: not a build this payload knows: its own 60 fps");
    return;
  }
  const struct eboot_patch *pt = k_fps120;
  const uint32_t *at = k_builds[g_build].fps120_at;
  for (unsigned i = 0; i < 2; i++) {
    uint8_t cur[11];
    if (kernel_proc_copyout(pid, base + at[i], cur, pt[i].len) != 0 ||
        memcmp(cur, pt[i].was, pt[i].len) != 0) {
      say("frame rate: 0x%x is not %s %s's stock bytes: 60 fps", at[i], GAME_NAME,
          k_builds[g_build].version);
      return;
    }
  }
  for (unsigned i = 0; i < 2; i++) {
    uint8_t now[11];
    memcpy(now, pt[i].now, pt[i].len);
    if (fps == 90) /* the sequence store's value: 3 = native 120 Hz, 2 = native 90 Hz (mode 2) */
      now[pt[i].len - 4] = 2;
    kernel_proc_copyin(pid, now, base + at[i], pt[i].len);
  }
  uint32_t pace = 1, hz = fps == 90 ? 90 : 0;
  kernel_proc_copyin(pid, &pace, g_base + g_hdroff + offsetof(struct a_header, frame_pace), 4);
  kernel_proc_copyin(pid, &hz, g_base + g_hdroff + offsetof(struct a_header, refresh_hz), 4);
  say("frame rate: the game's native %d Hz sequence, a frame every refresh (framerate.txt)", fps);
}

static void log_vmmap(pid_t game, uint64_t floor, const char *when);

/* The game is attached and stopped. 0 = the agent is in (the game stays stopped), 1 = the entry
 * point didn't come in wait_ms (the game had started: use the traps), -1 = the load failed,
 * -3 = the game ended. */
static int inject_at_entry(pid_t pid, int wait_ms) {
  uint64_t entry = eboot_entry(pid);
  uint8_t orig = 0, bp = 0xcc;
  if (!entry || pt_copyout(pid, entry, &orig, 1) != 0 || pt_copyin(pid, &bp, entry, 1) != 0) {
    say("entry: no breakpoint possible (entry 0x%lx)", (long)entry);
    return 1;
  }
  say("entry: breakpoint at 0x%lx", (long)entry);
  int sig = 0, st = 0, hit = 0;
  for (int waited = 0; !hit;) {
    pt_tid = 0;
    if (pt_continue(pid, sig) != 0)
      break;
    pid_t w = 0;
    for (; waited < wait_ms; waited++) {
      if ((w = waitpid(pid, &st, WNOHANG)) == -1 && errno == EINTR)
        w = 0;
      if (w != 0)
        break;
      usleep(1000);
    }
    if (w == 0) {
      say("entry: not reached in %d ms (the game had started)", wait_ms);
      kill(pid, SIGSTOP);
      while ((w = waitpid(pid, &st, 0)) == -1 && errno == EINTR)
        ;
      break;
    }
    if (w != pid || !WIFSTOPPED(st)) {
      say("entry: game ended (status 0x%x)", st);
      return -3;
    }
    sig = WSTOPSIG(st);
    pt_tid = pt_event_lwp(pid);
    struct reg r;
    if (sig == SIGTRAP && pt_tid > 0 && pt_getregs(pid, &r) == 0 && r.r_rip == entry + 1) {
      r.r_rip = entry;
      hit = pt_setregs(pid, &r) == 0;
      if (hit)
        say("entry: thread %d at the entry point", pt_tid);
    }
    if (hit || sig == SIGSTOP || sig == SIGTRAP) /* the system's own stops while it starts */
      sig = 0;
  }
  pt_copyin(pid, &orig, entry, 1);
  if (!hit)
    return 1;
  if (read_eboot_imports(pid) != 0 || !slots_bound(pid)) {
    say("entry: imports not bound yet");
    return 1;
  }
  g_loading_at_entry = 1;
  int rc = load_agent(pid);
  g_loading_at_entry = 0;
  pt_tid = 0;
  return rc == 0 ? 0 : -1;
}

static int inject(pid_t pid) {
  choose_agent();
  int unstoppable = 0, entry_tried = 0;
  struct stat fst;
  if (debug_file("trap_only", &fst)) { /* testing the late path */
    say("entry: skipped (" OUT_DIR "/trap_only)");
    entry_tried = 1;
  }
  for (int tries = 0; tries < 400; tries++) {
    int st = 0;
    int a = pt_attach_timed(pid, 300, &st);
    if (a == -1) { /* not attachable yet (exec in progress) */
      usleep(5000);
      continue;
    }
    if (a == -2) { /* attached but it never stopped */
      pt_detach(pid, 0);
      if (++unstoppable >= 20) {
        say("attach: game %d can't be stopped (its PS VR dialog is up?)", pid);
        return -2;
      }
      usleep(20000);
      continue;
    }
    if (a == -3) {
      if (time(NULL) - g_launched_at < 20) {
        /* launched from here: what ended is its first image, the game's is next */
        usleep(20000);
        continue;
      }
      say("attach: game %d ended (status 0x%x)", pid, st);
      return -1;
    }
    int bound = read_eboot_imports(pid) == 0 && slots_bound(pid);
    if (!entry_tried) {
      entry_tried = 1;
      /* bound: the kernel has linked it and the entry point is moments away (or long past,
       * when this payload was sent with the game running) */
      int er = inject_at_entry(pid, bound ? 5000 : 30000);
      if (er == -3)
        return -1;
      if (er == 0) {
        patch_frame_rate(pid);
        apply_agent_patches(pid); /* (the game still stopped at its entry point) */
        /* what is at libSceAgcDriver's fixed addresses (0xfe0000000 on) as the game starts:
         * read only (run 4's MAP_FIXED probe replaced whatever was there and killed the game) */
        log_vmmap(pid, 0xf00000000ull, "at the entry point");
        pt_detach(pid, 0);
        pt_tid = 0;
        say("agent in at the entry point");
        return 0;
      }
      if (er == -1)
        return -1;
      bound = read_eboot_imports(pid) == 0 && slots_bound(pid);
    }
    if (!bound) { /* still loading its modules */
      pt_detach(pid, 0);
      usleep(10000);
      continue;
    }
    int rc = inject_by_trap(pid);
    if (rc == -3)
      return -1;
    if (rc == -2) /* it stopped once but not again: the dialog came up */
      return -2;
    pt_detach(pid, 0);
    pt_tid = 0;
    if (rc == 0) {
      say("agent in through the traps (try %d)", tries);
      patch_frame_rate(0);
      return 0;
    }
    usleep(20000);
  }
  say("no trap fired");
  return -1;
}

/* ---- hand tracking (libSceVrHand in the game) ---- */
/* CMD_MODEL: libSceVrHand's model id cmd_arg[0] (1 = handCenter_00, 2/6 = handPose_00/_02) read
 * from the system's own copy into the game's buffer (cmd_arg[1], cmd_arg[2] bytes); the game's
 * sandbox has no /system/priv. Answer in cmd_arg[3]: bytes, -1 = not read, -2 = buffer too small. */
static void serve_model(pid_t pid, uint64_t shared, const volatile uint64_t *arg) {
  uint64_t id = arg[0], buf = arg[1], size = arg[2];
  int64_t got = -1;
  char path[96] = "";
  if (id >= 1 && id <= 6 && ((0x46 >> id) & 1) && buf && size) {
    snprintf(path, sizeof path, "/system/priv/vr_hand_ro/%s_%02d.bin",
             (id & 1) ? "handCenter" : "handPose", (int)((id & 1) ? (id - 1) / 2 : (id - 2) / 2));
    int fd = open(path, O_RDONLY);
    static uint8_t chunk[1 << 20];
    if (fd >= 0) {
      got = 0;
      for (;;) {
        uint64_t left = size - (uint64_t)got, want = left < sizeof chunk ? left : sizeof chunk;
        ssize_t n = want ? read(fd, chunk, want) : 0;
        if (n <= 0)
          break;
        if (kernel_proc_copyin(pid, chunk, buf + (uint64_t)got, (size_t)n) != 0) {
          got = -1;
          break;
        }
        got += n;
      }
      uint8_t more;
      if (got == (int64_t)size && read(fd, &more, 1) == 1)
        got = -2;
      close(fd);
    }
  }
  say("hand model %lu <- %s: %ld (buffer %lu bytes)", (unsigned long)id, path, (long)got,
      (unsigned long)size);
  kernel_proc_copyin(pid, &got, shared + offsetof(struct a_shared_head, cmd_arg) + 24, 8);
}

/* CMD_CAMERA: libSceHmd2 opens the headset cameras (/dev/hmd2_image, /dev/hmd2_leddet) for
 * libSceVrHand, and an app's credentials are refused them (0x8a721003). The game gets this
 * payload's capabilities (its authid and attributes stay its own), as PS5VR's helper does for
 * PS5VR ("elevate <pid> 1"). Only asked for while hand tracking starts: cmd_arg[0] = 1 raises
 * them, 0 gives the game its own back (once the cameras are open). Answer in cmd_arg[0]. */
static uint8_t g_game_caps[16];
static pid_t g_game_caps_pid = -1;

static void open_cameras(pid_t pid, uint64_t shared, uint64_t raise) {
  uint8_t caps[16] = {0}, mine[16] = {0};
  int64_t rc = -1;
  if (!raise) {
    if (g_game_caps_pid == pid)
      rc = kernel_set_ucred_caps(pid, g_game_caps);
    if (rc == 0)
      g_game_caps_pid = -1;
    say("cameras: the game's own capabilities back (%ld)", (long)rc);
  } else if (kernel_get_ucred_caps(pid, caps) == 0 && kernel_get_ucred_caps(getpid(), mine) == 0) {
    if (g_game_caps_pid != pid) {
      memcpy(g_game_caps, caps, sizeof caps);
      g_game_caps_pid = pid;
    }
    rc = kernel_set_ucred_caps(pid, mine);
    say("cameras: game capabilities %016lx%016lx -> the payload's (%ld)",
        *(unsigned long *)(caps + 8), *(unsigned long *)caps, (long)rc);
  }
  kernel_proc_copyin(pid, &rc, shared + offsetof(struct a_shared_head, cmd_arg), 8);
}

/* The game's memory map from `floor` up (KERN_PROC_VMMAP entries: start, end, protection), to the
 * log: libSceAgcDriver maps its command buffers at fixed addresses near the top of the 64 GB
 * space (0xfe0000000 on), where run 3's fault was. */
static void log_vmmap(pid_t game, uint64_t floor, const char *when) {
  int mib[4] = {1, 14, 32, game};
  size_t size = 0;
  if (sysctl(mib, 4, NULL, &size, NULL, 0) != 0 || !size) {
    say("vmmap %s: query failed (errno %d)", when, errno);
    return;
  }
  size += size / 4;
  uint8_t *map = malloc(size);
  if (!map || sysctl(mib, 4, map, &size, NULL, 0) != 0) {
    free(map);
    say("vmmap %s: read failed (errno %d)", when, errno);
    return;
  }
  int n = 0, shown = 0;
  uint64_t top = 0;
  for (uint8_t *e = map; e + 64 <= map + size;) {
    int es = *(int *)e;
    if (es <= 0)
      break;
    uint64_t start = *(uint64_t *)(e + 8), end = *(uint64_t *)(e + 16);
    int prot = *(int *)(e + 56);
    const char *name = es > 0x80 ? (const char *)(e + 0x80) : "";
    e += es;
    n++;
    if (end > top)
      top = end;
    if (end > floor && shown++ < 40)
      say("vmmap %s: 0x%lx-0x%lx prot %d %.32s", when, (unsigned long)start, (unsigned long)end,
          prot, name);
  }
  say("vmmap %s: %d entries, the highest ends at 0x%lx", when, n, (unsigned long)top);
  free(map);
}

/* The game watched with ptrace while the agent loads the first PS5 library (run 1: the game died
 * inside that load with nothing in any log; /dev/klog has another reader on this console). Every
 * stop is logged with its thread, signal, fault address and registers and the signal passed on;
 * the watch ends when the agent's next request shows the load returned, when the game ends, or
 * after `ms`. The log relay pauses meanwhile. */
static void watch_game(pid_t pid, uint64_t shared, uint32_t seq_acked, int ms) {
  int st = 0, a = pt_attach_timed(pid, 500, &st);
  if (a != 0) {
    say("watch: attach -> %d (status 0x%x): not watched", a, st);
    if (a == -2)
      pt_detach(pid, 0);
    return;
  }
  say("watch: attached for the first library's load (up to %d ms)", ms);
  int sig = 0, waited = 0, stops = 0;
  for (;;) {
    pt_tid = 0;
    if (pt_continue(pid, sig) != 0) {
      say("watch: continue failed (errno %d)", errno);
      break;
    }
    sig = 0;
    pid_t w = 0;
    for (; waited < ms; waited++) {
      if ((w = waitpid(pid, &st, WNOHANG)) == -1 && errno == EINTR)
        w = 0;
      if (w != 0)
        break;
      struct a_shared_head sh;
      if (waited % 10 == 9 && kernel_proc_copyout(pid, shared, &sh, sizeof sh) == 0 &&
          sh.cmd_seq != seq_acked) {
        say("watch: the load returned (the agent's next request is up)");
        waited = ms;
        break;
      }
      usleep(1000);
    }
    if (w == 0) { /* time, or the load returned: stop it to detach */
      kill(pid, SIGSTOP);
      for (int t = 0; t < 2000 && (w = waitpid(pid, &st, WNOHANG)) == 0; t++)
        usleep(1000);
      if (w == pid && WIFSTOPPED(st))
        break;
      say("watch: the game did not stop for the detach (w %d, status 0x%x)", (int)w, st);
      if (w == pid && !WIFSTOPPED(st))
        return;
      break;
    }
    if (w != pid) {
      say("watch: wait failed (errno %d)", errno);
      return;
    }
    if (!WIFSTOPPED(st)) {
      say("watch: THE GAME ENDED: status 0x%x (%s %d)", st, WIFSIGNALED(st) ? "signal" : "exit code",
          WIFSIGNALED(st) ? WTERMSIG(st) : WEXITSTATUS(st));
      return;
    }
    sig = WSTOPSIG(st);
    pt_tid = pt_event_lwp(pid);
    if (sig == SIGSTOP) { /* the system's own stops */
      sig = 0;
      continue;
    }
    uint64_t addr = 0;
    int si = pt_event_siginfo(pid, &addr);
    struct reg r;
    memset(&r, 0, sizeof r);
    int gr = pt_tid > 0 ? pt_getregs(pid, &r) : -1;
    say("watch: thread %d stopped with signal %d (siginfo %d, address 0x%lx)%s: rip 0x%lx rsp 0x%lx "
        "rax 0x%lx rdi 0x%lx rsi 0x%lx",
        (int)pt_tid, sig, si, (unsigned long)addr, gr == 0 ? "" : " (no registers)",
        (unsigned long)r.r_rip, (unsigned long)r.r_rsp, (unsigned long)r.r_rax,
        (unsigned long)r.r_rdi, (unsigned long)r.r_rsi);
    if (++stops >= 40) {
      say("watch: %d stops: enough", stops);
      break;
    }
    /* the signal goes on to the game (a fatal one ends it, and the end is logged above) */
  }
  pt_tid = 0;
  pt_detach(pid, sig);
  say("watch: detached (%d stops)", stops);
}

/* Relay the agent's log and serve its requests until the game ends (0) or a newer injector
 * asks this one to leave (1). */
/* the agent's log ring (struct agent_shared: log[] follows the 0x40-byte head), what is new
 * since g_rpos, relayed to the console and the file; from the serve loop, and from kill_game
 * after the agent's stop (the loop is this thread, waiting in the kill then) */
static uint64_t g_rpos;
static void relay_log(pid_t pid, uint64_t shared, const struct a_shared_head *sh) {
  static char ring[LOG_SIZE];
  if (sh->log_wpos == g_rpos)
    return;
  if (sh->log_wpos - g_rpos > LOG_SIZE)
    g_rpos = sh->log_wpos - LOG_SIZE;
  uint64_t n = sh->log_wpos - g_rpos;
  uint64_t start = g_rpos % LOG_SIZE, first = n < LOG_SIZE - start ? n : LOG_SIZE - start;
  kernel_proc_copyout(pid, shared + 0x40 + start, ring, first);
  if (n > first)
    kernel_proc_copyout(pid, shared + 0x40, ring + first, n - first);
  fwrite(ring, 1, n, stdout);
  fflush(stdout);
  if (g_logf) {
    fwrite(ring, 1, n, g_logf);
    fflush(g_logf);
  }
  g_rpos = sh->log_wpos;
}

static int serve(pid_t pid, intptr_t proc, int adopted) {
  uint64_t shared = mirror_header()->shared;
  g_live_pid = pid, g_live_shared = shared;
  g_rpos = 0;
  if (adopted) /* the previous payload already relayed its log */
    kernel_proc_copyout(pid, shared, &g_rpos, 8);
  for (int ticks = 0;; ticks++) {
    struct a_shared_head sh;
    if (!game_alive(pid, proc) || kernel_proc_copyout(pid, shared, &sh, sizeof sh) != 0) {
      say("game gone");
      g_live_pid = 0, g_live_shared = 0;
      return 0;
    }
    relay_log(pid, shared, &sh);
    if (sh.cmd >= 2 && sh.cmd <= 12 && sh.cmd_seq != sh.cmd_ack) {
      /* the ptrace watch of the first library's load (run 3) only with OUT_DIR/watch: it found
       * its crash, and the console went down once during that bring-up with it attached
       * (measurement 4, whose code patch turned out to be a no-op) */
      struct stat wst;
      int watch = sh.cmd == 7 && sh.cmd_arg[0] == 0 && sh.cmd_arg[1] == 0 && !adopted &&
                  debug_file("watch", &wst);
      if (sh.cmd == 2)
        say("resolved %d late imports", resolve_missing(pid));
      else if (sh.cmd == 5) {
        say("pointed %d unbound imports at the agent's stub",
            stub_unbound(pid, sh.cmd_arg, mirror_header()->unbound_stub));
        unmount_libs(0); /* the libraries are loaded */
      }
      else if (sh.cmd == 7)
        stage_lib(sh.cmd_arg[0], sh.cmd_arg[1]);
      else if (sh.cmd == 8) {
        FILE *f = fopen(OUT_DIR "/ipd", "w");
        if (f) {
          fprintf(f, "%u\n", (unsigned)sh.cmd_arg[0]);
          fclose(f);
        }
      }
      else if (sh.cmd == 9) {
        char msg[200] = {0};
        kernel_proc_copyout(pid, sh.cmd_arg[0], msg, sizeof msg - 1);
        notify("%s", msg);
      }
      else if (sh.cmd == 6) {
        char nid[16] = {0};
        kernel_proc_copyout(pid, sh.cmd_arg[1], nid, 15);
        override_import(pid, sh.cmd_arg[0], nid, sh.cmd_arg[2]);
      }
      else if (sh.cmd == 10)
        serve_model(pid, shared, sh.cmd_arg);
      else if (sh.cmd == 11)
        open_cameras(pid, shared, sh.cmd_arg[0]);
      else if (sh.cmd == 12) { /* cvars.txt (console variables by name) into the agent's buffer */
        static char text[4096];
        size_t room = sh.cmd_arg[1] < sizeof text ? sh.cmd_arg[1] : sizeof text;
        memset(text, 0, sizeof text);
        FILE *f = fopen(OUT_DIR "/cvars.txt", "r");
        size_t n = f && room > 1 ? fread(text, 1, room - 1, f) : 0;
        if (f)
          fclose(f);
        text[n] = 0;
        kernel_proc_copyin(pid, text, sh.cmd_arg[0], n + 1);
        say("cvars.txt: %zu bytes for the agent", n);
      }
      kernel_proc_copyin(pid, &sh.cmd_seq, shared + offsetof(struct a_shared_head, cmd_ack), 4);
      if (watch) {
        log_vmmap(pid, 0xf00000000ull, "before the load");
        watch_game(pid, shared, sh.cmd_seq, 8000);
        if (game_alive(pid, proc))
          log_vmmap(pid, 0xf00000000ull, "after the load");
      }
    }
    if (sh.repatch) {
      uint32_t zero = 0;
      repatch(pid);
      kernel_proc_copyin(pid, &zero, shared + offsetof(struct a_shared_head, repatch), 4);
    }
    if (ticks % 25 == 24) {
      if (!g_avrec_done && g_av_patched && ticks < 25 * 20)
        av_mark_vr2_record(pid);
      repatch(pid); /* the loader may bind the game's imports after the hooks went in */
      if (ctl_quit_requested(0)) {
        say("a newer injector took over");
        return 1;
      }
    }
    usleep(sh.repatch ? 1000 : 20000);
  }
}

/* Resident: waits for the game (launched from the home screen or anywhere else), puts the
 * agent in as the game starts, serves it until the game ends, then waits for the next start. */
int main(void) {
  signal(SIGPIPE, SIG_IGN); /* a control-port client gone away must not end the injector */
  mkdir(OUT_DIR, 0777);
  g_logf = fopen(OUT_DIR "/agent.log", "a");
  unsigned fw = kernel_get_fw_version() >> 16;
  g_fw = astro_firmware(fw);
  if (!g_fw || !g_fw->av[0].va) {
    /* Not supported yet, or the AV control service of this firmware is still missing: save
     * what a profile is made from, decrypted, for adding one. */
    char dir[64];
    snprintf(dir, sizeof dir, OUT_DIR "/firmware-%x.%02x", fw >> 8, fw & 0xff);
    if (dump_firmware(dir) == 6)
      notify("SummerLessonVR2 doesn't support firmware %x.%02x yet. The files a profile for it is made from are in %s.",
             fw >> 8, fw & 0xff, dir);
    else
      notify("SummerLessonVR2 doesn't support firmware %x.%02x yet, and its files couldn't all be saved. Details in " OUT_DIR "/agent.log.",
             fw >> 8, fw & 0xff);
    return 1;
  }
  say("firmware %s", g_fw->name);
  if (prepare(OUT_DIR, g_fw) != 0 || load_gcn(OUT_DIR, &g_gcn, g_gcn_code, GCN_MAX_DWORDS) != 0) {
    notify("SummerLessonVR2: the PS VR2 files couldn't be built. Details in " OUT_DIR "/agent.log.");
    return 1;
  }
  g_ctl = ctl_open();
  if (g_ctl < 0) {
    /* never two instances at once (both would attach to the game, patch the AV service and read
     * the kernel): this one leaves instead */
    say("the control port (%d) could not be opened for 10 s: another SummerLessonVR2 still running, or "
        "the port not free. Not starting; send it again in a few seconds.", CTL_PORT);
    notify("SummerLessonVR2: another copy is still running (or port %d is not free). Send it again in a "
           "few seconds.", CTL_PORT);
    return 1;
  }
  choose_agent();
  say("resident (pid %d), agent %ld bytes", (int)getpid(), (long)g_agent_size);
#ifndef RELEASE
  klog_start(); /* (a release build keeps no copy of the kernel log: written to storage all along) */
#endif
  if (find_game(-1) < 0) /* a re-send with the game open says nothing */
    notify("SummerLessonVR2 is ready. Start " TITLE_NAME ".");
  pid_t done = -1;
  for (;;) {
    pid_t pid = g_launching ? -1 : find_game(done); /* not while the launcher makes it */
    if (pid < 0) {
      if (ctl_quit_requested(1)) {
        say("a newer injector took over");
        return 0;
      }
      usleep(5000);
      continue;
    }
    intptr_t proc = kernel_get_proc(pid);
    if (g_logf)
      fclose(g_logf);
    g_logf = fopen(OUT_DIR "/agent.log", "a");
    say("game pid %d (" TITLE_NAME ": %s %s)", pid, GAME_NAME,
        g_build < 0 ? "?" : k_builds[g_build].version);
    int adopted = adopt_agent(pid) == 0;
    find_sandbox(); /* this game's (its title's): where its PS VR2 libraries are mounted */
    /* ps-patch-system's patch-server stops the game on every module it loads and walks and
     * patches its modules; with it running, the PS VR2 libraries' load took Ghost VR's console
     * down 4 times (its main.c). A new game is left alone; one the agent is already in is served
     * as before (its libraries may be up already, and an agent left without us would wait). */
    if (find_proc("patch-server.elf") >= 0) {
      if (!adopted) {
        notify("SummerLessonVR2: ps-patch-system (patch-server) is running. It can crash the "
               "console when the PS VR2 libraries load, so SummerLessonVR2 stays out of this game. "
               "Stop it in your payload manager and start " TITLE_NAME " again.");
        say("patch-server.elf is running: not injecting into pid %d", pid);
        done = pid;
        continue;
      }
      say("patch-server.elf is running (the agent is already in pid %d: served as before)", pid);
    }
    if (!adopted) {
      mark_vr2_app(pid);
      /* The game opens its "Connect PS VR" setup dialog early and polls it from its main loop,
       * so a thread passes through game code now and then; the agent's dialog hooks close it. */
      int ir = inject(pid);
      if (ir != 0) {
        if (game_alive(pid, proc))
          notify(ir == -2 ? "SummerLessonVR2 came too late: the game's PS VR screen was already up. "
                            "Close " TITLE_NAME " and start it again (SummerLessonVR2 stays ready)."
                          : "SummerLessonVR2 couldn't get into the game. Close " TITLE_NAME
                            " and start it again.");
        say("load failed");
        done = pid;
        reset_game_state();
        continue;
      }
      say("hooks live");
      unlink(OUT_DIR "/test.txt"); /* a test run is one start: the next is an ordinary one */
      FILE *st = fopen(OUT_DIR "/agent_state", "w");
      if (st) {
        fprintf(st, "%d 0x%lx 0x%lx 0x%lx\n", pid, (long)g_base, (long)g_hdroff, (long)g_size);
        fclose(st);
      }
    }
    patch_av_service();
    int quit = serve(pid, proc, adopted);
    restore_av_service();
    unmount_libs(1);
    done = pid;
    reset_game_state();
    if (quit)
      return 0;
  }
}
