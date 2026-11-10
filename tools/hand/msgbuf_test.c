/* The kernel message buffer's delta by marker lines (payload/msgbuf.h) on the host, against a
 * simulated 16 KB ring: the first snapshot, ordinary growth, a wrap between polls, a flood of
 * identical lines larger than the ring, the anchor marker overwritten, the marker present but the
 * text after it truncated (the buffer replaced), two markers in one poll.
 *   cc -O2 -o /tmp/msgbuf_test tools/hand/msgbuf_test.c && /tmp/msgbuf_test
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include <stdio.h>
#include <stdlib.h>
#include "../../payload/msgbuf.h"

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)

/* the ring: appended text, the last RING bytes kept, a snapshot oldest first (as kern.msgbuf) */
#define RING 16384
static char ring[RING];
static size_t ring_len, ring_head; /* bytes held, where the oldest is */
static void ring_append(const char *s) {
  size_t n = strlen(s);
  for (size_t i = 0; i < n; i++) {
    ring[(ring_head + ring_len) % RING] = s[i];
    if (ring_len < RING)
      ring_len++;
    else
      ring_head = (ring_head + 1) % RING;
  }
}
static size_t ring_snapshot(char *out) {
  for (size_t i = 0; i < ring_len; i++)
    out[i] = ring[(ring_head + i) % RING];
  return ring_len;
}
static void ring_clear(void) { ring_len = ring_head = 0; }

static unsigned g_seq;
static void mark(void) {
  char l[64];
  sprintf(l, "<118>[payload.elf] " MSGBUF_MARK "%u 5eedc0de\n", ++g_seq);
  ring_append(l);
}
static void foreign_mark(unsigned seq) { /* another payload instance's marker: a plain line */
  char l[64];
  sprintf(l, "<118>[payload.elf] " MSGBUF_MARK "%u 0ddba11e\n", seq);
  ring_append(l);
}

/* one poll as the payload does it: the lines of [from, to) "parsed" (copied out); -1 when lines
 * were possibly missed, else the bytes parsed */
static struct msgbuf_track tr = {.nonce = "5eedc0de"};
static unsigned newest;
static long poll(char *out, size_t outn) {
  static char buf[RING + 16];
  size_t n = ring_snapshot(buf), from, to;
  int lost = msgbuf_delta(&tr, buf, n, &from, &to);
  size_t got = to - from;
  if (got >= outn)
    got = outn - 1;
  memcpy(out, buf + from, got);
  out[got] = 0;
  msgbuf_advance(&tr, buf, from, to, &newest);
  return lost ? -1 : (long)(to - from);
}
static int count(const char *s, const char *needle) {
  int k = 0;
  for (const char *p = s; (p = strstr(p, needle)); p += strlen(needle))
    k++;
  return k;
}

int main(void) {
  static char out[RING + 16], line[128];
  long r;
  /* 1. the first snapshot: a history of unknown age, none of it parsed (no marker of ours yet),
   * flagged */
  for (int i = 0; i < 50; i++) {
    sprintf(line, "<118>[SceShellCore] history line %d of the message buffer\n", i);
    ring_append(line);
  }
  r = poll(out, sizeof out);
  CHECK(r == -1 && out[0] == 0 && tr.mlen == 0);
  /* 2. nothing new, no marker: nothing parsed (the history's age unknown), still a loss */
  r = poll(out, sizeof out);
  CHECK(r == -1 && out[0] == 0);
  /* 3. a marker appears: the text after it is parsed (the history before it never: its age
   * unknown), flagged as a re-anchoring; the anchor is the marker, consumed = what follows it */
  mark();
  ring_append("<118>[XFD] VrCamera StateChanged: Stop\n");
  r = poll(out, sizeof out);
  CHECK(r == -1 && strcmp(out, "<118>[XFD] VrCamera StateChanged: Stop\n") == 0 && tr.seq == 1);
  CHECK(tr.consumed == strlen("<118>[XFD] VrCamera StateChanged: Stop\n"));
  /* 4. ordinary growth: exactly the new lines, a partial last line left for later */
  ring_append("<118>[A] one\n<118>[B] two\npartial");
  r = poll(out, sizeof out);
  CHECK(r > 0 && strcmp(out, "<118>[A] one\n<118>[B] two\n") == 0);
  ring_append(" line\n");
  r = poll(out, sizeof out);
  CHECK(r > 0 && strcmp(out, "partial line\n") == 0);
  r = poll(out, sizeof out);
  CHECK(r == 0 && out[0] == 0);
  /* 5. a second marker and lines: the anchor moves to it, consumed counts from it */
  mark();
  ring_append("<118>[C] three\n");
  r = poll(out, sizeof out);
  CHECK(r > 0 && count(out, MSGBUF_MARK "2 ") == 1 && strstr(out, "<118>[C] three\n") && tr.seq == 2);
  CHECK(tr.consumed == strlen("<118>[C] three\n"));
  /* 6. a wrap between polls with the anchor surviving (the ring full, its oldest lines gone, the
   * anchor 4 KB in): only the new lines */
  for (int i = 0; i < 185; i++) {
    sprintf(line, "<118>[SceShellCore] FMEM  13.9/  27.7   0.0/   0.0 payload.elf line %3d\n", i);
    ring_append(line);
  }
  CHECK(ring_len == RING && ring_head > 0);
  r = poll(out, sizeof out);
  CHECK(r > 0 && count(out, "FMEM") == 185 && !strstr(out, "[C] three"));
  /* 7. a flood of IDENTICAL lines, 2 KB of it, the anchor surviving: parsed once, exactly */
  mark();
  r = poll(out, sizeof out);
  CHECK(r > 0 && tr.seq == 3);
  for (int i = 0; i < 40; i++)
    ring_append("<118>[SceShellCore] FMEM  13.9/  27.7   0.0/   0.0 payload.elf\n");
  r = poll(out, sizeof out);
  CHECK(r > 0 && count(out, "FMEM") == 40);
  r = poll(out, sizeof out);
  CHECK(r == 0);
  /* 8. a flood of identical lines LARGER than the ring between two polls, no marker in it: the
   * anchor wrapped out, nothing parsed (a BUSY line inside the flood is beyond the ring: lost,
   * and SAID so; the levels unknown until lines after a marker say), the same each poll until a
   * marker appears, then only what follows it */
  for (int i = 0; i < 400; i++)
    ring_append("<118>[SceShellCore] FMEM  13.9/  27.7   0.0/   0.0 payload.elf\n");
  ring_append("<118>[XFD] VrCamera StateChanged: StartJobRunning\n");
  for (int i = 0; i < 400; i++)
    ring_append("<118>[SceShellCore] FMEM  13.9/  27.7   0.0/   0.0 payload.elf\n");
  r = poll(out, sizeof out);
  CHECK(r == -1 && tr.mlen == 0 && out[0] == 0);
  r = poll(out, sizeof out);
  CHECK(r == -1 && out[0] == 0);
  mark();
  ring_append("<118>[XFD] VrCamera StateChanged: Running\n");
  r = poll(out, sizeof out);
  CHECK(r == -1 && strcmp(out, "<118>[XFD] VrCamera StateChanged: Running\n") == 0 && tr.seq == 4);
  r = poll(out, sizeof out);
  CHECK(r == 0);
  /* 9. the anchor marker overwritten while a NEWER marker survives: lost (flagged), the text
   * after the newest marker parsed, the anchor moved to it */
  for (int i = 0; i < 110; i++)
    ring_append("<118>[SceShellCore] FMEM  13.9/  27.7   0.0/   0.0 payload.elf\n");
  mark(); /* 5 */
  ring_append("<118>[XFD] VrCamera StateChanged: WaitNoDraw\n");
  for (int i = 0; i < 150; i++)
    ring_append("<118>[SceShellCore] FMEM  13.9/  27.7   0.0/   0.0 payload.elf\n");
  mark(); /* 6 */
  ring_append("<118>[XFD] VrCamera StateChanged: state Stop\n");
  r = poll(out, sizeof out); /* (after marker 6 only: WaitNoDraw, before it, is not read) */
  CHECK(r == -1 && strcmp(out, "<118>[XFD] VrCamera StateChanged: state Stop\n") == 0 && tr.seq == 6);
  CHECK(tr.consumed == strlen("<118>[XFD] VrCamera StateChanged: state Stop\n"));
  /* 10. the marker present but the text after it truncated (the buffer replaced: cleared, the
   * same marker line logged again by someone, less after it): a loss, the anchor kept, the count
   * cut to what is there, nothing read again (a stale line must not set a level); then what
   * follows is new */
  ring_append("<118>[D] four\n");
  r = poll(out, sizeof out);
  CHECK(r == 14);
  ring_clear();
  sprintf(line, "<118>[payload.elf] " MSGBUF_MARK "%u 5eedc0de\n<118>[D] again\n", g_seq);
  ring_append(line);
  r = poll(out, sizeof out);
  CHECK(r == -1 && out[0] == 0 && tr.consumed == 15 && tr.seq == g_seq);
  ring_append("<118>[E] five\n");
  r = poll(out, sizeof out);
  CHECK(r == 14 && strcmp(out, "<118>[E] five\n") == 0);
  /* 11. two markers in one poll: everything after the anchor parsed (both marker lines in it),
   * the anchor the newest, consumed from it */
  ring_append("<118>[F] six\n");
  mark(); /* 7 */
  ring_append("<118>[G] seven\n");
  mark(); /* 8 */
  ring_append("<118>[H] eight\n");
  r = poll(out, sizeof out);
  CHECK(r > 0 && count(out, MSGBUF_MARK) == 2 && strstr(out, "[F] six") && strstr(out, "[H] eight") && tr.seq == 8);
  CHECK(tr.consumed == strlen("<118>[H] eight\n") && newest == 8);
  r = poll(out, sizeof out);
  CHECK(r == 0);
  /* 12. a marker line among the parsed text does not move the anchor backwards: an OLDER seq
   * (a stale copy) is ignored */
  ring_append("<118>[payload.elf] " MSGBUF_MARK "3 5eedc0de\n<118>[I] nine\n");
  r = poll(out, sizeof out);
  CHECK(r > 0 && tr.seq == 8 && strstr(out, "[I] nine"));
  /* 13. another instance's markers (another nonce) are plain lines: never an anchor, never
   * "seen", and not matched when ours is lost */
  foreign_mark(100);
  ring_append("<118>[J] ten\n");
  r = poll(out, sizeof out);
  CHECK(r > 0 && tr.seq == 8 && newest == 8 && count(out, MSGBUF_MARK "100 ") == 1 && strstr(out, "[J] ten"));
  for (int i = 0; i < 400; i++)
    ring_append("<118>[SceShellCore] FMEM  13.9/  27.7   0.0/   0.0 payload.elf\n");
  foreign_mark(101);
  ring_append("<118>[K] eleven\n");
  r = poll(out, sizeof out);
  CHECK(r == -1 && tr.mlen == 0 && out[0] == 0); /* ours gone, theirs not an anchor */
  mark(); /* 9 */
  ring_append("<118>[L] twelve\n");
  r = poll(out, sizeof out);
  CHECK(r == -1 && strcmp(out, "<118>[L] twelve\n") == 0 && tr.seq == 9 && newest == 9);
  CHECK(tr.consumed == strlen("<118>[L] twelve\n"));
  /* 14. the tracking reset for a new game (the payload's serve) with a NEW nonce: the next poll
   * is a first snapshot again, all of it parsed and flagged; the old nonce's markers are
   * nobody's (no anchor) until the first marker of the new nonce */
  memset(&tr, 0, sizeof tr);
  strcpy(tr.nonce, "0b5e55ed");
  r = poll(out, sizeof out);
  CHECK(r == -1 && out[0] == 0 && tr.mlen == 0 && newest == 0);
  ring_append("<118>[payload.elf] " MSGBUF_MARK "1 0b5e55ed\n<118>[M] thirteen\n");
  r = poll(out, sizeof out);
  CHECK(r == -1 && tr.seq == 1 && newest == 1 && tr.consumed == strlen("<118>[M] thirteen\n"));
  ring_append("<118>[N] fourteen\n");
  r = poll(out, sizeof out);
  CHECK(r == 18 && strcmp(out, "<118>[N] fourteen\n") == 0);
  printf("%s: %d failures\n", fails ? "FAILED" : "ok", fails);
  return fails != 0;
}
