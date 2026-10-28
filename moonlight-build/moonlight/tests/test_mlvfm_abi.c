/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Compile for both -m32 and -m64; no libc or target hardware required. */
#include "../kernel/moonlight-vfm/moonlight_vfm.h"
typedef char frame_size[sizeof(struct mlvfm_frame) == 64 ? 1 : -1];
typedef char pts_offset[__builtin_offsetof(struct mlvfm_frame, pts_us) == 24 ? 1 : -1];
typedef char planes_offset[__builtin_offsetof(struct mlvfm_frame, planes) == 32 ? 1 : -1];
typedef char query_size[sizeof(struct mlvfm_info) == 8 ? 1 : -1];
typedef char start_size[sizeof(struct mlvfm_start) == 16 ? 1 : -1];
typedef char done_size[sizeof(struct mlvfm_done) == 8 ? 1 : -1];
typedef char submit_ioctl[_IOC_SIZE(MLVFM_SUBMIT) == 64 ? 1 : -1];
