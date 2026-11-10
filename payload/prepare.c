/* First run: build, from this console's own system files, what the PS4 game needs to drive
 * PS VR2 - nothing of Sony's ships with SummerLessonVR2:
 *  - libSceAgcDriver, libSceAgc, libSceHmd2, libSceVrTracker2, decrypted with the self pager,
 *    marked ELF ABI version 3 (the loader refuses version 2, PS5-only, in a PS4 process), Hmd2's
 *    sceKernelGetProsperoCompiledSdkVersion stub answering for itself (it fails in a PS4
 *    process and Hmd2's start fails with it), and fake-signed;
 *  - Hmd2's reprojection shader translated from RDNA to GCN (the game is a gen1 GPU context).
 * Only the firmware profiles in common/firmware.h are accepted (their SHA-256 is checked). Cached output is
 * tied to the selected firmware so files prepared before an update cannot be reused.
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include "prepare.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include "fself.h"
#include "rdna2gcn.h"
#include "selfpager.h"

void say(const char *fmt, ...);

#define PAID 0x3100000000000002ull
#define AGC_BASE 0xfe0400000ull /* libSceAgcDriver's command buffers (2 MB, 2 MB aligned) */

static void hex(const unsigned char *d, char *out) {
  for (int i = 0; i < 32; i++)
    sprintf(out + 2 * i, "%02x", d[i]);
}

static int write_file(const char *path, const void *data, size_t len) {
  int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0777);
  if (fd < 0)
    return -1;
  size_t done = 0;
  while (done < len) {
    ssize_t w = write(fd, (const char *)data + done, len - done);
    if (w <= 0) {
      close(fd);
      return -1;
    }
    done += (size_t)w;
  }
  close(fd);
  return 0;
}

/* file offset of a virtual address inside a loaded segment, and how much follows it there */
static long va_to_off(const unsigned char *elf, unsigned long va, unsigned long *avail) {
  unsigned long phoff = *(const unsigned long *)(elf + 0x20);
  unsigned short phnum = *(const unsigned short *)(elf + 0x38);
  for (unsigned i = 0; i < phnum; i++) {
    const unsigned char *ph = elf + phoff + i * 56;
    unsigned type = *(const unsigned *)ph;
    unsigned long off = *(const unsigned long *)(ph + 8), vaddr = *(const unsigned long *)(ph + 16),
                  fsz = *(const unsigned long *)(ph + 32);
    if (type == 1 && va >= vaddr && va < vaddr + fsz) {
      if (avail)
        *avail = vaddr + fsz - va;
      return (long)(off + va - vaddr);
    }
  }
  return -1;
}

static int build_shader(const unsigned char *hmd2, const char *dir, const struct astro_firmware *fw) {
  unsigned long avail = 0;
  long off = va_to_off(hmd2, fw->hmd_shader_va, &avail);
  if (off < 0)
    return say("prepare: Hmd2 shader not found"), -1;
  static unsigned code[GCN_MAX_DWORDS];
  struct rdna2gcn_info info;
  if (rdna2gcn((const unsigned *)(hmd2 + off), (unsigned)(avail / 4), code, GCN_MAX_DWORDS, &info)) {
    say("prepare: shader translation failed: %s at dword %u", info.error, info.error_at);
    return -1;
  }
  if (info.src_dwords != fw->shader_src_dwords || info.out_dwords != fw->shader_out_dwords)
    return say("prepare: unexpected shader lengths for FW %s (%u -> %u)", fw->name,
               info.src_dwords, info.out_dwords), -1;
  struct gcn_file h = {GCN_MAGIC, info.out_dwords, info.rsrc1_vgprs, info.rsrc1_sgprs};
  char path[256];
  snprintf(path, sizeof path, "%s/hmd2_gcn.bin", dir);
  int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0777);
  if (fd < 0 || write(fd, &h, sizeof h) != sizeof h ||
      write(fd, code, info.out_dwords * 4) != (ssize_t)(info.out_dwords * 4)) {
    if (fd >= 0)
      close(fd);
    return say("prepare: can't write %s (errno %d)", path, errno), -1;
  }
  close(fd);
  say("prepare: reprojection shader: %u RDNA dwords -> %u GCN dwords, %u VGPRs %u SGPRs",
      info.src_dwords, info.out_dwords, info.vgprs, info.sgprs);
  return 0;
}

/* One library: decrypted, checked against the profile, ABI 3 (Hmd2: its SDK version stub and
 * the reprojection shader), fake-signed into libdir. VideoOut (fw->libs[4]) is only checked. */
static int build_lib(const struct astro_lib *l, const char *dir, const char *libdir,
                     const struct astro_firmware *fw, int check_only) {
  char path[256], tmp[264];
  snprintf(path, sizeof path, "/system/common/lib/%s.sprx", l->name);
  int fd = open(path, O_RDONLY);
  if (fd < 0)
    return say("prepare: can't open %s (errno %d)", path, errno), -1;
  char *elf = 0;
  uint64_t size = 0;
  int rc = decrypt_self(fd, &elf, &size);
  close(fd);
  if (rc != 0 || !elf)
    return say("prepare: can't decrypt %s (%d)", path, rc), -1;
  unsigned char digest[32];
  char hx[65];
  fself_sha256(elf, size, digest);
  hex(digest, hx);
  if (size != l->size || strcmp(hx, l->sha256) != 0) {
    say("prepare: %s is not the FW %s build (size %lu, sha256 %s): unsupported firmware",
        l->name, fw->name, (unsigned long)size, hx);
    munmap(elf, size);
    return -1;
  }
  if (check_only) {
    munmap(elf, size);
    return 0;
  }
  unsigned char *e = (unsigned char *)elf;
  e[8] = 3; /* EI_ABIVERSION */
  /* libSceAgcDriver maps its 2 MB command-buffer area at the hint 0xfe0000000 and, if that map
   * fails, only prints to stderr and goes on writing there. In a PS4 process of an early SDK
   * (Summer Lesson, 2.508) a 64 KB reservation the game needs already sits at 0xfe0000000 (run 5's
   * memory map), so the map failed and the driver's first write crashed the game (runs 1, 3).
   * Its base is read from its own variable everywhere else: the one constant moves it to
   * AGC_BASE, which nothing in the process uses (the kernel's regions end at 0xfe0400000).
   * The constant is found by its instruction (movabs rax, 0xfe0000000; once, at 0x501 on 12.40),
   * not at a firmware's offset, so every firmware's driver gets the move; none or two refuse. */
  if (!strcmp(l->name, "libSceAgcDriver")) {
    static const unsigned char was[10] = {0x48, 0xb8, 0x00, 0x00, 0x00, 0xe0, 0x0f, 0x00, 0x00, 0x00};
    const unsigned long long now = AGC_BASE;
    long at = -1;
    unsigned hits = 0;
    for (uint64_t i = 0; i + sizeof was <= size; i++)
      if (e[i] == 0x48 && !memcmp(e + i, was, sizeof was))
        at = (long)i, hits++;
    if (hits != 1) {
      munmap(elf, size);
      return say("prepare: AgcDriver's base 0xfe0000000 found %u times, not once", hits), -1;
    }
    memcpy(e + at + 2, &now, 8);
    say("prepare: AgcDriver's command buffers moved from 0xfe0000000 to 0x%llx (at 0x%lx)", now,
        at + 2);
  }
  if (!strcmp(l->name, "libSceHmd2")) {
    long at = va_to_off(e, fw->hmd_sdk_plt, 0);
    /* mov dword [rdi], 0x08050001 ; xor eax, eax ; ret */
    static const unsigned char stub[16] = {0xc7, 0x07, 0x01, 0x00, 0x05, 0x08, 0x31, 0xc0,
                                           0xc3, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc};
    if (at < 0 || memcmp(e + at, fw->hmd_sdk_orig, 6) != 0) {
      munmap(elf, size);
      return say("prepare: Hmd2's SDK version stub is not where expected"), -1;
    }
    memcpy(e + at, stub, 16);
    if (build_shader(e, dir, fw)) {
      munmap(elf, size);
      return -1;
    }
  }
  size_t need = fself_make(e, size, PAID, FSELF_PTYPE_FAKE, 0, 0);
  unsigned char *out = malloc(need);
  size_t got = out ? fself_make(e, size, PAID, FSELF_PTYPE_FAKE, out, need) : 0;
  munmap(elf, size);
  snprintf(path, sizeof path, "%s/%s.sprx", libdir, l->name);
  snprintf(tmp, sizeof tmp, "%s.new", path);
  if (!got || write_file(tmp, out, got) != 0 || rename(tmp, path) != 0) {
    free(out);
    unlink(tmp);
    return say("prepare: can't write %s (errno %d)", path, errno), -1;
  }
  free(out);
  say("prepare: %s ready (%lu bytes)", path, (unsigned long)got);
  return 0;
}

int prepare(const char *dir, const struct astro_firmware *fw) {
  if (!fw)
    return -1;
  char libdir[256], path[256], gcn[256], marker[256], expected[80], actual[80] = {0};
  snprintf(libdir, sizeof libdir, "%s/libs", dir);
  snprintf(gcn, sizeof gcn, "%s/hmd2_gcn.bin", dir);
  snprintf(marker, sizeof marker, "%s/prepared", dir);
  snprintf(expected, sizeof expected, "SummerLessonVR2 cache v4 FW %s\n", fw->name);
  mkdir(dir, 0777);
  mkdir(libdir, 0777);
  int mf = open(marker, O_RDONLY);
  ssize_t mn = mf >= 0 ? read(mf, actual, sizeof actual - 1) : -1;
  if (mf >= 0)
    close(mf);
  int have = mn == (ssize_t)strlen(expected) && !strcmp(actual, expected) && access(gcn, R_OK) == 0;
  for (unsigned i = 0; i < 4 && have; i++) {
    snprintf(path, sizeof path, "%s/%s.sprx", libdir, fw->libs[i].name);
    have = access(path, R_OK) == 0;
  }
  if (!have) {
    unlink(marker); /* an interrupted build must never look complete */
    say("prepare: building the PS VR2 libraries and shader from this console's firmware");
    for (unsigned i = 0; i < 5; i++)
      if (fw->libs[i].name && build_lib(&fw->libs[i], dir, libdir, fw, i == 4))
        return -1;
    if (write_file(marker, expected, strlen(expected)) != 0)
      return -1;
  }
  /* hand tracking's library, where the profile has one: optional, built on its own (also on a
   * console whose cache an older version made) */
  if (fw->hand.name) {
    snprintf(path, sizeof path, "%s/%s.sprx", libdir, fw->hand.name);
    if (access(path, R_OK) != 0 && build_lib(&fw->hand, dir, libdir, fw, 0) != 0)
      say("prepare: no hand tracking (%s not made)", fw->hand.name);
  }
  return 0;
}

int dump_firmware(const char *dir) {
  static const char *const src[] = {
      "/system/common/lib/libSceAgcDriver.sprx", "/system/common/lib/libSceAgc.sprx",
      "/system/common/lib/libSceHmd2.sprx",      "/system/common/lib/libSceVrTracker2.sprx",
      "/system/common/lib/libSceVideoOut.sprx",  "/SceSysAvControl.elf",
  };
  char path[256];
  int saved = 0;
  mkdir(dir, 0777);
  for (unsigned i = 0; i < sizeof src / sizeof src[0]; i++) {
    int fd = open(src[i], O_RDONLY);
    char *elf = 0;
    uint64_t size = 0;
    int rc = fd < 0 ? -1 : decrypt_self(fd, &elf, &size);
    if (fd >= 0)
      close(fd);
    snprintf(path, sizeof path, "%s/%s", dir, strrchr(src[i], '/') + 1);
    if (rc != 0 || !elf || write_file(path, elf, size) != 0)
      say("dump: %s not saved (%d, errno %d)", src[i], rc, errno);
    else
      saved++;
    if (elf)
      munmap(elf, size);
  }
  say("dump: %d of 6 files saved in %s", saved, dir);
  return saved;
}

int load_gcn(const char *dir, struct gcn_file *h, unsigned *code, unsigned cap) {
  char path[256];
  snprintf(path, sizeof path, "%s/hmd2_gcn.bin", dir);
  int fd = open(path, O_RDONLY);
  if (fd < 0)
    return -1;
  int ok = read(fd, h, sizeof *h) == sizeof *h && h->magic == GCN_MAGIC && h->dwords <= cap &&
           read(fd, code, h->dwords * 4) == (ssize_t)(h->dwords * 4);
  close(fd);
  return ok ? 0 : -1;
}
