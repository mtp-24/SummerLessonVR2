/* Command-buffer pieces for our compute work on Hmd2's GPU queue (gen1_submit_acb): the inset
 * pass and the debug menu (inset.c). PM4 type-3 packets in their gfx7 compute forms; between
 * steps, a label written once the step's stores are in memory, a wait for it on the queue and the
 * caches invalidated, as Linux's gfx7 compute rings sync.
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include "vr2.h"

u32 acb_pm4(u32 op, u32 body) { return 0xc0000000u | ((body - 1) << 16) | (op << 8); }

/* RELEASE_MEM, gfx7 compute form: once the work before it is done and its stores are in memory
 * (the L2 written back and invalidated, the L1s: TC_WB, TCL1, TC), 32 bits of `v` (data_sel 1)
 * or the GPU's clock (data_sel 3) to `addr` */
u32 acb_release(u32 *cb, u32 n, u64 addr, u32 data_sel, u32 v) {
  cb[n++] = acb_pm4(0x49, 6);
  cb[n++] = (1u << 16) | (1u << 17) | (1u << 15) | (5u << 8) | 0x14;
  cb[n++] = data_sel << 29;
  cb[n++] = (u32)addr, cb[n++] = (u32)(addr >> 32), cb[n++] = v, cb[n++] = 0;
  return n;
}

/* The GPU's clock to `addr` once the work before it is done: acb_release's packet without its
 * cache actions (TC_WB, TCL1, TC clear). A timestamp only: no write-back and invalidate of the
 * whole L2 in the middle of the game's frame for it. Nothing of ours is made visible by it; the
 * pass's closing acb_barrier does that (and so the timestamps' own writes too, before the CPU
 * reads them after its label). */
u32 acb_timestamp(u32 *cb, u32 n, u64 addr) {
  cb[n++] = acb_pm4(0x49, 6);
  cb[n++] = (5u << 8) | 0x14;
  cb[n++] = 3u << 29;
  cb[n++] = (u32)addr, cb[n++] = (u32)(addr >> 32), cb[n++] = 0, cb[n++] = 0;
  return n;
}

/* ACQUIRE_MEM over everything: the L1 texture caches and the constant cache, with `l2` the L2
 * and the instruction cache too */
u32 acb_acquire(u32 *cb, u32 n, int l2) {
  cb[n++] = acb_pm4(0x58, 6);
  cb[n++] = (1u << 22) | (1u << 27) | (l2 ? (1u << 23) | (1u << 29) : 0);
  cb[n++] = 0xffffffffu, cb[n++] = 0xff, cb[n++] = 0, cb[n++] = 0, cb[n++] = 0x0a;
  return n;
}

/* the step before done: its label (its stores written back to memory, as RELEASE_MEM does), the
 * queue waiting for it (WAIT_REG_MEM, memory, equal), the L1 caches */
u32 acb_barrier(u32 *cb, u32 n, u64 label, u32 v) {
  n = acb_release(cb, n, label, 1, v);
  cb[n++] = acb_pm4(0x3c, 6);
  cb[n++] = 3 | (1u << 4);
  cb[n++] = (u32)label, cb[n++] = (u32)(label >> 32), cb[n++] = v, cb[n++] = 0xffffffffu;
  cb[n++] = 4;
  return acb_acquire(cb, n, 0);
}
