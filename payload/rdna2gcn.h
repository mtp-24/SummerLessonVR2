/* rdna2gcn: RDNA (gfx1013, wave64) compute shader -> GCN (gfx7). See rdna2gcn.c.
 * SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

struct rdna2gcn_info {
  unsigned src_dwords;  /* source consumed, up to and including s_endpgm */
  unsigned out_dwords;  /* gfx7 code written */
  unsigned vgprs, sgprs;
  unsigned rsrc1_vgprs, rsrc1_sgprs; /* COMPUTE_PGM_RSRC1 fields */
  const char *error;
  unsigned error_at; /* source dword offset */
  unsigned wave32;   /* translated as a wave32 shader */
  unsigned hw_threads; /* split wave32 group: COMPUTE_NUM_THREAD_X to dispatch it with (Y, Z = 1,
                        * TIDIG_COMP_CNT = 0); 0 = the group as given */
};

/* What the shader's header says about it (rdna2gcn_compute) */
struct rdna2gcn_cs {
  unsigned wave32;     /* the header's wave size flag */
  unsigned threads[3]; /* COMPUTE_NUM_THREAD_X/Y/Z */
  unsigned tidig;      /* COMPUTE_PGM_RSRC2 TIDIG_COMP_CNT */
};

/* Returns 0 on success. out may be 0 to only size the result (out_dwords). */
int rdna2gcn(const unsigned *in, unsigned in_dwords, unsigned *out, unsigned out_cap,
             struct rdna2gcn_info *info);

/* The same for a compute shader whose header is known (libSceVrHand's): wave32 groups of more
 * than 32 threads are split one hardware wave per wave32 (info->hw_threads), and the gfx10
 * V#s that scalar buffer loads read get a gfx7 dword 3 as MUBUF's do. */
int rdna2gcn_compute(const unsigned *in, unsigned in_dwords, unsigned *out, unsigned out_cap,
                     const struct rdna2gcn_cs *cs, struct rdna2gcn_info *info);
