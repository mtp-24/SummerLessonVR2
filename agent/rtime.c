/* Measurement only (agent.rtime=1, a debug option): how much CPU time the game's threads spend a
 * frame, for the multi-view question (docs/MULTIVIEW.md: whether two more views fit at 60 fps on
 * the threads that prepare the views). Nothing here changes what the game does.
 *
 * Each game thread is sampled (its own CPU clock: clock_gettime, the thread's CPU-time clock) when
 * it calls one of the hooked functions: the command-buffer submits, the hand-over of a frame, the
 * pad read, and the waits (condition variables, event flags, event queues, sleeps), so every
 * thread that ever waits or submits is seen, with its name. Every 10 s:
 *   - the process's CPU time a frame (its CPU-time clock), and that of the threads seen;
 *   - each thread with its CPU time a frame and share of the wall time, and what it was seen
 *     doing (s submits, h hands frames over, p reads the pad, w waits), the busiest first;
 *   - for the thread that submits the most, the CPU time before each submit by its place in the
 *     frame (the first after the frame count changed is 0) and the bytes each carries.
 * A blocking wait costs no CPU time; a spinning one does, so a thread's time is an upper bound of
 * its work. (2026-11-06, the first runs: the submitting threads alone used ~1 ms a frame, the
 * same at 960 and 2048 eyes: the views' work is elsewhere, hence the waits.)
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include "vr2.h"

#define RT_THREADS 40
#define RT_PLACES 16
#define RT_SHOWN 14
#define CLOCK_THREAD_CPUTIME_ID 14 /* FreeBSD's */
#define CLOCK_PROCESS_CPUTIME_ID 15

struct rt_thread {
  void *self;
  char name[24];
  u32 kinds; /* RT_* seen */
  u64 cpu_last, cpu_mark; /* ns: at its last sample, at the last report */
  u64 frame_last;         /* the frame count at its last submit */
  u32 place, subs;
  u64 place_ns[RT_PLACES], place_bytes[RT_PLACES];
  u32 place_n[RT_PLACES];
};
static struct rt_thread g_rt[RT_THREADS];
static u32 g_rt_n, g_rt_failed, g_rt_full;
static u64 g_rt_next, g_rt_frame_mark, g_rt_wall_mark, g_rt_proc_mark;

static int cpu_ns(int clock, u64 *ns) {
  struct {
    s64 sec, nsec;
  } ts = {0, 0};
  if (!clock_gettime || clock_gettime(clock, &ts) != 0)
    return -1;
  *ns = (u64)ts.sec * 1000000000ull + (u64)ts.nsec;
  return 0;
}

static void rt_report(u64 wall_us, u64 frame) {
  u64 frames = frame - g_rt_frame_mark, wall = wall_us - g_rt_wall_mark, proc = 0;
  g_rt_frame_mark = frame, g_rt_wall_mark = wall_us;
  int have_proc = cpu_ns(CLOCK_PROCESS_CPUTIME_ID, &proc) == 0;
  u64 proc_ns = have_proc && g_rt_proc_mark ? proc - g_rt_proc_mark : 0;
  if (have_proc)
    g_rt_proc_mark = proc;
  u64 ns[RT_THREADS], seen = 0;
  for (u32 i = 0; i < g_rt_n; i++) {
    ns[i] = g_rt[i].cpu_last - g_rt[i].cpu_mark;
    g_rt[i].cpu_mark = g_rt[i].cpu_last;
    seen += ns[i];
  }
  if (frames && wall && frames < 100000) {
    LOG("rtime: 10 s, %lu frames: the process %lu us of CPU a frame (%lu percent of one core), "
        "the %u threads seen %lu us%s", frames, proc_ns / 1000 / frames, proc_ns / 10 / wall,
        g_rt_n, seen / 1000 / frames, g_rt_full ? " (more threads than the table holds)" : "");
    u32 done[RT_THREADS] = {0};
    for (u32 k = 0; k < RT_SHOWN && k < g_rt_n; k++) {
      u32 top = RT_THREADS;
      for (u32 i = 0; i < g_rt_n; i++)
        if (!done[i] && (top == RT_THREADS || ns[i] > ns[top]))
          top = i;
      if (top == RT_THREADS || ns[top] / 1000 / frames < 20)
        break;
      done[top] = 1;
      struct rt_thread *t = &g_rt[top];
      LOG("rtime:   %s: %lu us a frame, %lu percent; %s%s%s%s %lu.%lu submits a frame", t->name,
          ns[top] / 1000 / frames, ns[top] / 10 / wall, t->kinds & RT_SUBMIT ? "s" : "-",
          t->kinds & RT_HANDOVER ? "h" : "-", t->kinds & RT_PAD ? "p" : "-",
          t->kinds & RT_WAIT ? "w" : "-", (u64)t->subs / frames, (u64)t->subs * 10 / frames % 10);
    }
    u32 sub = RT_THREADS;
    for (u32 i = 0; i < g_rt_n; i++)
      if (g_rt[i].subs && (sub == RT_THREADS || g_rt[i].subs > g_rt[sub].subs))
        sub = i;
    if (sub != RT_THREADS) {
      struct rt_thread *t = &g_rt[sub];
      char line[512];
      u32 len = 0;
      for (u32 p = 0; p < RT_PLACES && len < sizeof line - 40; p++)
        if (t->place_n[p])
          len += (u32)agent_snprintf(line + len, sizeof line - len, " %u:%lu us/%lu KB/%u", p,
                                     t->place_ns[p] / 1000 / t->place_n[p],
                                     t->place_bytes[p] / 1024 / t->place_n[p], t->place_n[p]);
      line[len < sizeof line ? len : sizeof line - 1] = 0;
      LOG("rtime:   %s by its submit's place in the frame (CPU before it/its bytes/times seen):%s",
          t->name, line);
    }
  }
  for (u32 i = 0; i < g_rt_n; i++) {
    g_rt[i].subs = 0;
    memset(g_rt[i].place_ns, 0, sizeof g_rt[i].place_ns);
    memset(g_rt[i].place_bytes, 0, sizeof g_rt[i].place_bytes);
    memset(g_rt[i].place_n, 0, sizeof g_rt[i].place_n);
  }
}

void rtime_note(u32 kind, u32 n, const u32 *bytes) {
#ifdef RELEASE
  (void)kind, (void)n, (void)bytes;
  return;
#endif
  if (!g_opt_rtime || g_rt_failed)
    return;
  static volatile u32 busy; /* one at a time (the statics): a sample skipped meanwhile is only
                             * counted in the thread's next one */
  if (__atomic_exchange_n(&busy, 1, __ATOMIC_ACQUIRE))
    return;
  u64 ns;
  if (cpu_ns(CLOCK_THREAD_CPUTIME_ID, &ns) != 0) {
    g_rt_failed = 1;
    LOG("rtime: the thread's CPU clock can't be read (clock_gettime %s): off",
        clock_gettime ? "refused it" : "not found");
    __atomic_store_n(&busy, 0, __ATOMIC_RELEASE);
    return;
  }
  void *self = scePthreadSelf();
  u64 frame = hooks_frame_count(), wall = sceKernelGetProcessTime();
  struct rt_thread *t = 0;
  for (u32 i = 0; i < g_rt_n; i++)
    if (g_rt[i].self == self)
      t = &g_rt[i];
  if (!t && g_rt_n < RT_THREADS) {
    t = &g_rt[g_rt_n++];
    memset(t, 0, sizeof *t);
    t->self = self, t->cpu_last = t->cpu_mark = ns, t->frame_last = frame;
    char name[64];
    memset(name, 0, sizeof name);
    if (!scePthreadGetname || scePthreadGetname(self, name) != 0 || !name[0])
      agent_snprintf(name, sizeof name, "%p", self);
    memcpy(t->name, name, sizeof t->name - 1);
  } else if (!t) {
    g_rt_full = 1;
  } else {
    if (kind == RT_SUBMIT) {
      if (frame != t->frame_last)
        t->place = 0, t->frame_last = frame;
      else if (t->place < RT_PLACES - 1)
        t->place++;
      u64 b = 0;
      for (u32 i = 0; bytes && i < n; i++)
        b += bytes[i];
      t->place_ns[t->place] += ns - t->cpu_last;
      t->place_bytes[t->place] += b;
      t->place_n[t->place]++;
      t->subs++;
    }
    t->cpu_last = ns;
  }
  if (t)
    t->kinds |= kind;
  if (!g_rt_next) {
    g_rt_next = wall + 10000000, g_rt_frame_mark = frame, g_rt_wall_mark = wall;
    cpu_ns(CLOCK_PROCESS_CPUTIME_ID, &g_rt_proc_mark);
  } else if (wall >= g_rt_next) {
    g_rt_next = wall + 10000000;
    rt_report(wall, frame);
  }
  __atomic_store_n(&busy, 0, __ATOMIC_RELEASE);
}
