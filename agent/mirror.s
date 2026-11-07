// TV mirror: one pixel per thread, a crop of the game's left eye texture written to a linear
// A8R8G8B8 display buffer (bytes B, G, R, A). GCN (gfx7), assembled by tools/gcn_asm.py.
//
// s[0:1]  constant block (mirror.c struct consts)
// s2, s3  workgroup x, y (8x8 threads each)
// v0, v1  thread x, y
  s_load_dwordx8 s[4:11], s[0:1], 0x0      // T#: the eye texture
  s_load_dwordx4 s[12:15], s[0:1], 0x8     // S#: the game's sampler
  s_load_dwordx4 s[16:19], s[0:1], 0xc     // V#: the output buffer
  s_load_dwordx8 s[20:27], s[0:1], 0x10    // u0 du v0 dv pitch(bytes)
  s_lshl_b32 s28, s2, 3
  s_lshl_b32 s29, s3, 3
  v_add_i32 v2, vcc, s28, v0               // pixel x
  v_add_i32 v3, vcc, s29, v1               // pixel y
  v_cvt_f32_u32 v4, v2
  v_cvt_f32_u32 v5, v3
  s_waitcnt lgkmcnt(0)
  v_mov_b32 v6, s20
  v_mac_f32 v6, s21, v4                    // u
  v_mov_b32 v7, s22
  v_mac_f32 v7, s23, v5                    // v
  image_sample_lz v[8:10], v[6:7], s[4:11], s[12:15] dmask:0x7
  v_lshlrev_b32 v11, 2, v2
  v_mad_u32_u24 v12, v3, s24, v11          // byte offset
  s_waitcnt vmcnt(0)
  v_mul_f32 v8, 0x437f0000, v8             // * 255
  v_mul_f32 v9, 0x437f0000, v9
  v_mul_f32 v10, 0x437f0000, v10
  v_add_f32 v8, 0.5, v8
  v_add_f32 v9, 0.5, v9
  v_add_f32 v10, 0.5, v10
  v_cvt_u32_f32 v8, v8                     // R
  v_cvt_u32_f32 v9, v9                     // G
  v_cvt_u32_f32 v10, v10                   // B
  v_lshlrev_b32 v9, 8, v9
  v_lshlrev_b32 v8, 16, v8
  v_or_b32 v10, v10, v9
  v_or_b32 v10, v10, v8
  v_or_b32 v10, 0xff000000, v10            // opaque
  buffer_store_dword v10, v12, s[16:19], 0 offen
  s_waitcnt vmcnt(0) expcnt(0)
  s_endpgm
