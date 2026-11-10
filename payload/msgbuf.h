/* The kernel message buffer (sysctl kern.msgbuf) read without consuming it: a snapshot every poll,
 * and the new text since the last poll found by the payload's OWN MARKER LINES in the log
 * ("SummerLessonVR2 mark <seq> <nonce>", klog_printf, once a second the whole time a game is
 * served): unique, so the place of the last one seen is never in doubt, and the text after it
 * is append-only, so what was parsed after it is a byte count. Content anchors (the previous
 * snapshot's tail) were tried first and fail once the ring is full: identical lines repeated
 * (the FMEM census) let a false match skip lines with nothing to show for it. /dev/klog hands
 * each line to ONE of its readers (klogsrv and the payload split the lines when both read it), so
 * it can't feed the palm tracker's gate either.
 * Pure functions, so tools/hand/msgbuf_test.c runs them on the host against a simulated ring.
 * SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include <stddef.h>
#include <string.h>

#define MSGBUF_MARK "SummerLessonVR2 mark " /* the line: MSGBUF_MARK "<seq> <nonce>" */

struct msgbuf_track {
  char nonce[12]; /* this instance's, "%08x": other instances' markers are plain lines */
  char mark[64];  /* the anchor: the newest marker line's text seen (without its newline) */
  size_t mlen;    /* 0: none yet (the first poll), or lost and none seen since */
  size_t consumed; /* bytes after the anchor's line end already parsed */
  unsigned seq;    /* the anchor's seq (0 none) */
  int started;     /* the first snapshot has been taken */
};

/* The end of the last complete line in (buf, n): the offset after its newline, 0 when none. */
static inline size_t msgbuf_complete(const char *buf, size_t n) {
  while (n && buf[n - 1] != '\n')
    n--;
  return n;
}

/* the LAST occurrence of needle in (buf, n) that ends a line (followed by '\n'): its offset, or -1 */
static inline long msgbuf_find_line(const char *buf, size_t n, const char *needle, size_t nl) {
  if (!nl || nl + 1 > n)
    return -1;
  for (size_t at = n - nl - 1 + 1; at-- > 0;)
    if (buf[at] == needle[0] && buf[at + nl] == '\n' && memcmp(buf + at, needle, nl) == 0)
      return (long)at;
  return -1;
}

/* the newest complete marker line of THIS instance (MSGBUF_MARK "<seq> <nonce>") in (buf, n):
 * the offset of MSGBUF_MARK, its text's length (to the newline) in *len, its seq in *seq; -1
 * when none */
static inline long msgbuf_newest_mark(const char *buf, size_t n, const char *nonce, size_t *len,
                                      unsigned *seq) {
  const size_t pl = sizeof MSGBUF_MARK - 1, nl = strlen(nonce);
  if (n < pl + 2)
    return -1;
  for (size_t at = n - pl; at-- > 0;) {
    if (buf[at] != MSGBUF_MARK[0] || memcmp(buf + at, MSGBUF_MARK, pl) != 0)
      continue;
    size_t e = at + pl;
    unsigned s = 0;
    while (e < n && buf[e] >= '0' && buf[e] <= '9')
      s = s * 10 + (unsigned)(buf[e++] - '0');
    if (e >= n || buf[e] != ' ' || e + 1 + nl > n || memcmp(buf + e + 1, nonce, nl) != 0 ||
        e + 1 + nl >= n || buf[e + 1 + nl] != '\n')
      continue; /* another instance's, or not complete */
    *len = e + 1 + nl - at;
    *seq = s;
    return (long)at;
  }
  return -1;
}

static inline void msgbuf_anchor_at(struct msgbuf_track *tr, const char *buf, long at, size_t len,
                                    unsigned seq, size_t to) {
  memcpy(tr->mark, buf + at, len);
  tr->mark[len] = 0;
  tr->mlen = len;
  tr->seq = seq;
  tr->consumed = to - ((size_t)at + len + 1);
}

/* The range [*from, *to) of a snapshot (buf, n) to parse: the complete lines after what the
 * anchor already accounts for. Returns 1 when lines were possibly missed, else 0: the first
 * snapshot (a history of unknown age); the anchor gone from the buffer (wrapped out); the text
 * after the anchor shorter than what was parsed (the buffer replaced: the anchor kept, the count
 * cut to what is there, nothing read again). After the anchor is gone the reading
 * is anchored again on the newest marker of ours still in the buffer and goes on after it: the
 * lines after it are newer than the loss and were never parsed (a newer marker in parsed text
 * would have been the anchor); what is before it is not read (its age unknown: the caller sets
 * the level states unknown, and only the lines after re-establish them). Without any marker of
 * ours nothing is parsed until one appears. Nothing is ever parsed twice. */
static inline int msgbuf_delta(struct msgbuf_track *tr, const char *buf, size_t n, size_t *from,
                               size_t *to) {
  *to = msgbuf_complete(buf, n);
  int first = !tr->started;
  tr->started = 1;
  long at = tr->mlen ? msgbuf_find_line(buf, *to, tr->mark, tr->mlen) : -1;
  if (at >= 0) {
    size_t e = (size_t)at + tr->mlen + 1; /* after the anchor's newline */
    if (tr->consumed <= *to - e) {
      *from = e + tr->consumed;
      return 0;
    }
    /* less text after the anchor than was parsed: the buffer replaced. A loss (the levels unknown
     * by the caller); the anchor stays, the count is cut to what is there and nothing is read
     * again (a stale line superseded in the old buffer must not set a level) */
    tr->consumed = *to - e;
    *from = *to;
    return 1;
  }
  tr->mlen = 0;
  tr->consumed = 0;
  size_t len;
  unsigned seq;
  at = msgbuf_newest_mark(buf, *to, tr->nonce, &len, &seq);
  if (at >= 0) {
    msgbuf_anchor_at(tr, buf, at, len, seq, *to);
    *from = *to - tr->consumed; /* after it; msgbuf_advance adds what is parsed now */
    tr->consumed = 0;
  } else {
    *from = *to;
  }
  return first || 1;
}

/* After parsing [from, to): the anchor moves to the newest marker line among the NEW text when it
 * is newer than the anchor (its seq higher, or none yet), consumed = the bytes after it; else
 * consumed grows by what was parsed. *newest = the newest marker's seq seen so far, the anchor's
 * (0 none; a re-anchoring in msgbuf_delta counts). */
static inline void msgbuf_advance(struct msgbuf_track *tr, const char *buf, size_t from, size_t to,
                                  unsigned *newest) {
  size_t len;
  unsigned seq;
  long at = from < to ? msgbuf_newest_mark(buf + from, to - from, tr->nonce, &len, &seq) : -1;
  if (at >= 0 && (!tr->mlen || seq > tr->seq))
    msgbuf_anchor_at(tr, buf, (long)from + at, len, seq, to);
  else if (tr->mlen)
    tr->consumed += to - from;
  *newest = tr->mlen ? tr->seq : 0;
}
