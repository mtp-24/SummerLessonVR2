/* Agent runtime: import table, header, log ring, small libc pieces.
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include "agent.h"
#include "vr.h"

#include <stdarg.h>

/* Import slots (filled by the injector). */
#define IMP(ret, name, args) ret(*name) args;
#include "imports.inc"
#undef IMP

#define IMP(ret, name, args) {MOD_##name, NID_##name, (void **)&name},
static struct agent_import g_imports[] = {
#include "imports.inc"
};
#undef IMP

extern struct agent_hook g_hooks[];
extern const u32 g_nhooks;
u64 agent_boot(u64 multiview);

struct agent_shared g_shared __attribute__((aligned(4096)));
struct agent_gcn g_gcn __attribute__((aligned(256)));

/* Imports of the modules the agent loads that nothing provides (ShaderTranscode_minimal,
 * DbgThreadSanitizer): calling an unbound import would park the game for good. */
static u64 unbound_import(void) {
  static volatile u32 n;
  if (__atomic_fetch_add(&n, 1, __ATOMIC_RELAXED) < 8)
    LOG("agent: an unbound import was called (from %p)", __builtin_return_address(0));
  return 0;
}
_Static_assert(offsetof(struct agent_shared, log) == 0x40, "injector reads the log at +0x40");
_Static_assert(sizeof(struct agent_header) == 144, "injector header ABI");
_Static_assert(offsetof(struct agent_header, firmware) == 92, "injector firmware field ABI");
_Static_assert(offsetof(struct agent_header, build) == 68, "injector build field ABI");
_Static_assert(offsetof(struct agent_header, frame_pace) == 104, "injector frame pace field ABI");
_Static_assert(offsetof(struct agent_header, screen_pct) == 112, "injector screen_pct field ABI");
_Static_assert(offsetof(struct agent_header, viewport) == 116, "injector viewport field ABI");
_Static_assert(offsetof(struct agent_header, delta_time) == 140, "injector statics ABI");

__attribute__((used, section(".astro_hdr"))) struct agent_header astro_hdr = {
    .magic = AGENT_MAGIC,
    .imports = g_imports,
    .nimports = sizeof g_imports / sizeof g_imports[0],
    .hooks = g_hooks,
    .nhooks = 0, /* set by agent_boot's caller from g_nhooks (see hooks.c) */
    .shared = &g_shared,
    .boot = agent_boot,
    .vr_state = &g_vr,
    .vr_persist = offsetof(struct vr_state, frame),
    .unbound_stub = unbound_import,
    .gcn = &g_gcn,
};

void *memcpy(void *d, const void *s, size_t n) {
  void *ret = d;
  __asm__ volatile("rep movsb" : "+D"(d), "+S"(s), "+c"(n) : : "memory");
  return ret;
}

void *memset(void *d, int c, size_t n) {
  void *ret = d;
  __asm__ volatile("rep stosb" : "+D"(d), "+c"(n) : "a"(c) : "memory");
  return ret;
}

int memcmp(const void *a, const void *b, size_t n) {
  const u8 *x = a, *y = b;
  for (; n; n--, x++, y++)
    if (*x != *y)
      return *x - *y;
  return 0;
}

size_t strlen(const char *s) {
  size_t n = 0;
  while (s[n])
    n++;
  return n;
}

/* --- formatting: %d %i %u %x %X %p %s %c %f with l/ll/z, width, 0 flag, .prec --- */
static void put(char *out, size_t n, size_t *pos, char c) {
  if (*pos + 1 < n)
    out[*pos] = c;
  (*pos)++;
}

static void put_num(char *out, size_t n, size_t *pos, u64 v, int base, int upper, int neg,
                    int width, int zero) {
  char tmp[24];
  int len = 0;
  const char *dig = upper ? "0123456789ABCDEF" : "0123456789abcdef";
  do {
    tmp[len++] = dig[v % base];
    v /= base;
  } while (v);
  int total = len + (neg ? 1 : 0);
  if (neg && zero)
    put(out, n, pos, '-');
  for (; total < width; total++)
    put(out, n, pos, zero ? '0' : ' ');
  if (neg && !zero)
    put(out, n, pos, '-');
  while (len)
    put(out, n, pos, tmp[--len]);
}

int agent_vsnprintf(char *out, size_t n, const char *fmt, va_list ap) {
  size_t pos = 0;
  for (; *fmt; fmt++) {
    if (*fmt != '%') {
      put(out, n, &pos, *fmt);
      continue;
    }
    fmt++;
    int zero = 0, width = 0, prec = -1, lng = 0;
    if (*fmt == '0') {
      zero = 1;
      fmt++;
    }
    while (*fmt >= '0' && *fmt <= '9')
      width = width * 10 + (*fmt++ - '0');
    if (*fmt == '.') {
      prec = 0;
      fmt++;
      while (*fmt >= '0' && *fmt <= '9')
        prec = prec * 10 + (*fmt++ - '0');
    }
    while (*fmt == 'l' || *fmt == 'z') {
      lng = 1;
      fmt++;
    }
    switch (*fmt) {
    case 'd':
    case 'i': {
      s64 v = lng ? va_arg(ap, s64) : va_arg(ap, int);
      put_num(out, n, &pos, v < 0 ? (u64)-v : (u64)v, 10, 0, v < 0, width, zero);
      break;
    }
    case 'u':
      put_num(out, n, &pos, lng ? va_arg(ap, u64) : va_arg(ap, unsigned), 10, 0, 0, width, zero);
      break;
    case 'x':
    case 'X':
      put_num(out, n, &pos, lng ? va_arg(ap, u64) : va_arg(ap, unsigned), 16, *fmt == 'X', 0,
              width, zero);
      break;
    case 'p':
      put(out, n, &pos, '0');
      put(out, n, &pos, 'x');
      put_num(out, n, &pos, (u64)va_arg(ap, void *), 16, 0, 0, 0, 0);
      break;
    case 's': {
      const char *s = va_arg(ap, const char *);
      if (!s)
        s = "(null)";
      for (; *s; s++)
        put(out, n, &pos, *s);
      break;
    }
    case 'c':
      put(out, n, &pos, (char)va_arg(ap, int));
      break;
    case 'f': {
      double v = va_arg(ap, double);
      if (prec < 0)
        prec = 4;
      if (v != v) {
        put(out, n, &pos, 'n');
        put(out, n, &pos, 'a');
        put(out, n, &pos, 'n');
        break;
      }
      int neg = v < 0;
      if (neg)
        v = -v;
      u64 scale = 1;
      for (int i = 0; i < prec; i++)
        scale *= 10;
      u64 whole = (u64)v;
      u64 frac = (u64)((v - (double)whole) * (double)scale + 0.5);
      if (frac >= scale) {
        whole++;
        frac -= scale;
      }
      put_num(out, n, &pos, whole, 10, 0, neg, 0, 0);
      if (prec) {
        put(out, n, &pos, '.');
        put_num(out, n, &pos, frac, 10, 0, 0, prec, 1);
      }
      break;
    }
    case '%':
      put(out, n, &pos, '%');
      break;
    default:
      put(out, n, &pos, '?');
      break;
    }
  }
  if (n)
    out[pos < n ? pos : n - 1] = 0;
  return (int)pos;
}

int agent_snprintf(char *out, size_t n, const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  int r = agent_vsnprintf(out, n, fmt, ap);
  va_end(ap);
  /* what it wrote, not what it would have: lines are built with o += agent_snprintf(b + o,
   * sizeof b - o, ...), which a longer line would have taken past the buffer's end */
  return n ? (r < (int)n - 1 ? r : (int)n - 1) : 0;
}

void agent_log(const char *fmt, ...) {
  char line[512];
  va_list ap;
  va_start(ap, fmt);
  int len = agent_vsnprintf(line, sizeof line - 1, fmt, ap);
  va_end(ap);
  if (len > (int)sizeof line - 2)
    len = sizeof line - 2;
  line[len++] = '\n';
  /* spun on a while, then the core given up between tries: a holder of lower priority on the
   * same core gets to finish */
  for (u32 spins = 0; __atomic_exchange_n(&g_shared.log_lock, 1, __ATOMIC_ACQUIRE); spins++) {
    __builtin_ia32_pause();
    if (spins >= 2000 && sceKernelUsleep)
      sceKernelUsleep(1); /* 0 returns at once, to no thread of lower priority */
  }
  u64 w = g_shared.log_wpos;
  for (int i = 0; i < len; i++)
    g_shared.log[(w + i) % LOG_SIZE] = line[i];
  __atomic_store_n(&g_shared.log_wpos, w + len, __ATOMIC_RELEASE);
  __atomic_store_n(&g_shared.log_lock, 0, __ATOMIC_RELEASE);
}

/* Ask the injector (which polls g_shared every 20 ms) to do something, from any thread: one request
 * at a time (the VR, hand, GPU-label and game threads all ask). wait_ms: how long the injector may
 * take. *out (if given) = cmd_arg[k] afterwards.
 * g_shared.cmd is also where the injector says CMD_QUIT (kill_game): a request is only put in an
 * empty mailbox and only taken back out if it is still there (compare-and-swap both ways), so a
 * request ending at the moment of the stop cannot write the stop over with 0. */
static volatile u32 g_call_lock;

int agent_call(u32 cmd, u64 a0, u64 a1, u64 a2, u64 a3, u32 wait_ms, u32 k, u64 *out) {
  while (__atomic_exchange_n(&g_call_lock, 1, __ATOMIC_ACQUIRE))
    sceKernelUsleep(1000);
  u32 seq = g_shared.cmd_seq + 1, empty = 0;
  int rc = -1;
  /* the mailbox taken first, then the arguments, then the sequence the injector acts on (its
   * cmd_seq != cmd_ack): after a stop nothing is written, not even over cmd_arg[3], where the
   * VR thread's STOP_DONE goes */
  if (__atomic_compare_exchange_n(&g_shared.cmd, &empty, cmd, 0, __ATOMIC_ACQ_REL,
                                  __ATOMIC_ACQUIRE)) {
    g_shared.cmd_arg[0] = a0, g_shared.cmd_arg[1] = a1;
    g_shared.cmd_arg[2] = a2, g_shared.cmd_arg[3] = a3;
    __atomic_store_n(&g_shared.cmd_seq, seq, __ATOMIC_RELEASE);
    for (u32 i = 0; i < wait_ms / 10 + 1; i++) {
      if (__atomic_load_n(&g_shared.cmd_ack, __ATOMIC_ACQUIRE) == seq) {
        rc = 0;
        break;
      }
      sceKernelUsleep(10000);
    }
    u32 mine = cmd;
    __atomic_compare_exchange_n(&g_shared.cmd, &mine, 0, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
    if (out && k < 4)
      *out = g_shared.cmd_arg[k];
  } /* else the mailbox holds CMD_QUIT: nothing is asked, *out left as it was */
  __atomic_store_n(&g_call_lock, 0, __ATOMIC_RELEASE);
  return rc;
}
