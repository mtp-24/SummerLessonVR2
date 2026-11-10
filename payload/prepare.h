/* First-run preparation of the PS VR2 libraries and the GCN reprojection shader.
 * SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include "../common/firmware.h"

#define GCN_MAGIC 0x314e4347u /* "GCN1" */
#define GCN_MAX_DWORDS 0x1400

/* <dir>/hmd2_gcn.bin: this header, then the code */
struct gcn_file {
  unsigned magic, dwords, rsrc1_vgprs, rsrc1_sgprs;
};

/* Builds the four libraries in <dir>/libs and <dir>/hmd2_gcn.bin if they are missing, and hand
 * tracking's (optional) where the firmware has it. 0 = ready. */
int prepare(const char *dir, const struct astro_firmware *fw);
int load_gcn(const char *dir, struct gcn_file *h, unsigned *code, unsigned cap);

/* Firmware without a complete profile: decrypts the files a profile is made from into <dir>
 * (the five libraries and the AV control service). Returns how many of the six were saved. */
int dump_firmware(const char *dir);
