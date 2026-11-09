/* agent/rtime.c on the host, on made-up submits (make -C agent first, for agent/nids_gen.h):
 *   cc -O2 -I../../agent -o /tmp/rtimetest rtime_test.c && /tmp/rtimetest
 * Three threads: A submits 4 times a frame after 1000, 200, 300 and 500 us of CPU (2000 a frame),
 * B once after 100 us and hands the frame over, C ("Worker") only waits, after 3000 us a frame.
 * The 10 s report says so, the busiest first, with the process's time.
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#undef memcpy
#undef memset
#define memcpy agent_memcpy_unused
#define memset agent_memset_unused
#define memcmp agent_memcmp_unused
#define strlen agent_strlen_unused
#include "vr2.h"
#undef memcpy
#undef memset
#undef memcmp
#undef strlen

static u64 g_now = 1000000, g_frame = 1, g_cpu[3], g_proc;
static char g_all[8192];
static int g_thread;
static char g_last[1024];
static u64 now(void) { return g_now; }
u64 (*sceKernelGetProcessTime)(void) = now;
static void *self(void) { return (void *)(long)(0x1000 + g_thread); }
void *(*scePthreadSelf)(void) = self;
static int cg(int clock, void *ts) {
  if (clock != 14 && clock != 15)
    return -1;
  u64 v = clock == 15 ? g_proc : g_cpu[g_thread];
  ((s64 *)ts)[0] = (s64)(v / 1000000000ull), ((s64 *)ts)[1] = (s64)(v % 1000000000ull);
  return 0;
}
static int getname(void *t, char *name) {
  if ((long)t == 0x1002)
    strcpy(name, "Worker");
  return (long)t == 0x1002 ? 0 : -1;
}
int (*scePthreadGetname)(void *, char *) = getname;
int (*clock_gettime)(int, void *) = cg;
u64 hooks_frame_count(void) { return g_frame; }
u32 g_opt_rtime = 1;
void agent_log(const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(g_last, sizeof g_last, fmt, ap);
  va_end(ap);
  printf("  log: %s\n", g_last);
  if (strlen(g_all) + strlen(g_last) + 2 < sizeof g_all)
    strcat(g_all, g_last), strcat(g_all, "\n");
}
int agent_snprintf(char *out, size_t n, const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  int r = vsnprintf(out, n, fmt, ap);
  va_end(ap);
  return r < 0 ? 0 : (size_t)r >= n ? (int)n - 1 : r;
}

#include "../../agent/rtime.c"

int main(void) {
  static const u64 a_us[4] = {1000, 200, 300, 500};
  u32 sizes[2] = {4096, 4096};
  for (int f = 0; f < 1300; f++) {
    for (int s = 0; s < 4; s++) {
      g_thread = 0, g_cpu[0] += a_us[s] * 1000;
      rtime_note(RT_SUBMIT, 2, sizes);
    }
    g_thread = 1, g_cpu[1] += 100 * 1000;
    rtime_note(RT_SUBMIT, 1, sizes);
    rtime_note(RT_HANDOVER, 0, 0);
    g_thread = 2, g_cpu[2] += 3000 * 1000;
    rtime_note(RT_WAIT, 0, 0);
    g_proc += 6000 * 1000;
    g_now += 16667, g_frame++;
  }
  char *r = g_all, *x;
  while ((x = strstr(r + 1, "rtime: 10 s")))
    r = x; /* the last report */
  char *w = strstr(r, "Worker: 3000 us a frame"), *a = strstr(r, "0x1000: 2000 us a frame");
  int ok = w && a && w < a && strstr(r, "the process 6000 us") && strstr(r, "0x1001: 100 us a frame, 0 percent; sh-- 1.0") &&
           strstr(r, "---w") && strstr(g_last, " 0:1000 us/8 KB") && strstr(g_last, " 1:200 us") && strstr(g_last, " 2:300 us") &&
           strstr(g_last, " 3:500 us");
  printf(ok ? "ok\n" : "FAIL: the last report's places\n");
  return !ok;
}
