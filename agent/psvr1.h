/* PS4 PS VR API structures the game passes (layouts as in shadPS4/AstroQuest).
 * SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include "agent.h"

/* --- libSceHmd --- */
#define HMD_ERR_PARAMETER_NULL 0x81110008
#define HMD_STATUS_READY 0

struct hmd_device_information {
  u32 status;
  s32 user_id;
  u8 reserve0[4];
  u32 panel_width, panel_height;
  u16 latency_90hz, latency_120hz;
  u8 hmu_mount;
  u8 reserve1[7];
};

struct hmd_fov {
  float tan_out, tan_in, tan_top, tan_bottom;
};

struct hmd_eye_offset {
  float x, y, z;
  u8 reserve[20];
};

struct hmd_reprojection_resource {
  void *onion_buff;
  void *garlic_buff;
  s32 thread_priority;
  u32 padding;
  u64 cpu_affinity_mask;
  u32 pipe_id;
  u32 queue_id;
  u32 reserved[3];
};

struct hmd_eye_uv {
  float scale_x, scale_y, offset_x, offset_y;
};

struct hmd_reprojection_param {
  const void *texture[2]; /* sce::Gnm::Texture (GCN T#, 32 bytes) per eye */
  const void *sampler;
  struct hmd_eye_uv uv[2];
  u64 *frame_label;
  u32 unknown_40;
  u32 padding0;
  u64 unknown_48;
  u32 unknown_50;
  u32 padding1;
  u8 flags;
};

struct hmd_tracker_state {
  float position[3];
  float orientation[4];
  u32 padding0;
  u64 timestamp;
  u64 sensor_read_system_timestamp;
  u32 user_frame_number;
  u32 padding1;
};

/* --- libSceHmdSetupDialog / CommonDialog --- */
#define CDLG_STATUS_RUNNING 2
#define CDLG_STATUS_FINISHED 3
struct hmd_setup_dialog_result {
  s32 result; /* 0 = OK */
  u8 reserved[32];
};

/* --- libSceVrTracker --- */
#define VRT_ERR_ARGUMENT_INVALID 0x81810002
#define VRT_DEVICE_HMD 0
#define VRT_DEVICE_DUALSHOCK4 1

struct vrt_query_memory_result {
  u32 size;
  u32 onion_size, onion_align;
  u32 garlic_size, garlic_align;
  u32 work_size, work_align;
  u32 reserved[9];
};

struct vrt_pose {
  float px, py, pz;
  u32 r0;
  float qx, qy, qz, qw;
  u32 r1[8];
}; /* 0x40 */

struct vrt_hmd_info {
  struct vrt_pose device_pose, left_eye_pose, right_eye_pose, head_pose;
  u32 rear_tracking_status;
  u32 reserved0[3];
  u64 sensor_read_system_timestamp;
  u32 reserved1[10];
};

struct vrt_get_result_param {
  u32 size;
  s32 handle;
  u32 result_type;
  u32 reserved0;
  u64 prediction_time;
  u32 orientation_type;
  u32 reserved1;
  u32 usage_type;
  u32 user_frame_number;
  u32 debug_marker_type;
  u32 reserved2[2];
};

struct vrt_led_result {
  float x, y, rx, ry;
  u32 reserved[4];
};

struct vrt_result {
  s32 handle;
  u32 connected;
  u32 reserved0[2];
  u64 timestamp;
  u64 device_timestamp;
  u32 recalibrate_necessity;
  u32 playarea_brightness_risk;
  u32 reserved1[2];
  u32 led_color;
  u32 status;
  u32 position_quality;
  u32 orientation_quality;
  float vel[3], acc[3], ang_vel[3], ang_acc[3];
  float camera_orientation[4];
  union {
    struct vrt_hmd_info hmd;
    struct {
      struct vrt_pose device_pose;
      u32 reserved[64];
    } pad;
  };
  u32 user_frame_number;
  u32 led_adjustment_status;
  u64 timestamp_of_led_result;
  u32 reserved2[2];
  s32 number_of_led_result[2];
  u32 reserved3[4];
  struct vrt_led_result led[2][16];
};

/* --- libSceCamera --- */
#define CAM_ERR_PARAM 0x802E0000
#define CAM_ERR_NOT_START 0x802E0006

struct cam_frame_position {
  u32 x, y, xsize, ysize;
};
struct cam_exposure_gain {
  u32 control, exposure, gain, mode;
};
struct cam_white_balance {
  u32 control, red, blue, green;
};
struct cam_gamma {
  u32 control, value;
  u8 reserved[16];
};
struct cam_meta {
  u32 meta_mode;
  u32 format[2][4];
  u64 frame[2];
  u64 timestamp[2];
  u32 device_timestamp[2];
  struct cam_exposure_gain exposure_gain[2];
  struct cam_white_balance white_balance[2];
  struct cam_gamma gamma[2];
  u32 luminance[2];
  float acc_x, acc_y, acc_z;
  u64 vcounter;
  u32 reserved[14];
};
struct cam_frame_data {
  u32 size_this;
  u32 read_mode;
  struct cam_frame_position position[2][4];
  void *frame[2][4];
  u32 frame_size[2][4];
  u32 status[2];
  struct cam_meta meta;
  void *frame_garlic[2][4];
};
struct cam_format {
  u32 level[4];
};
struct cam_config_ext {
  struct cam_format format;
  u32 resolution;
  u32 framerate;
  u32 width, height;
  u32 reserved1;
  void *base_option;
};
struct cam_config {
  u32 size_this;
  u32 type;
  struct cam_config_ext ext[2];
};
struct cam_calibration_element {
  u32 h_vertices, v_vertices, left, right, top, bottom;
  float x_table[1276];
  float y_table[1276];
};
struct cam_calibration_data {
  u32 format_type;
  u32 function_type;
  struct cam_calibration_element data[2];
};

_Static_assert(sizeof(struct hmd_device_information) == 0x20, "hmd info");
_Static_assert(sizeof(struct vrt_pose) == 0x40, "pose");
