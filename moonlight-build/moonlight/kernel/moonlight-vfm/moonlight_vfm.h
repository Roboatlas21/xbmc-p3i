/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
#ifndef MOONLIGHT_VFM_UAPI_H
#define MOONLIGHT_VFM_UAPI_H

#include <linux/types.h>
#include <linux/ioctl.h>

/* Fixed-width, pointer-free ABI shared by ARM32 userspace and ARM64 kernel. */
#define MLVFM_ABI 1
#define MLVFM_SLOTS 32
#define MLVFM_NV12 0x3231564eU
#define MLVFM_FULL_RANGE 1U
#define MLVFM_BT709 2U

struct mlvfm_info {
  __u32 abi;
  __u32 slots;
};
struct mlvfm_start {
  __u32 abi;
  __u32 pending_frames; /* 1: lowest latency; 2: balanced */
  __u32 blackout_policy; /* snapshot of /sys/class/video/blackout_policy */
  __u32 reserved;
};
struct mlvfm_plane {
  __s32 fd;
  __u32 offset;
  __u32 pitch;
  __u32 reserved;
};
struct mlvfm_frame {
  __u32 cookie;
  __u32 fourcc;
  __u32 width;
  __u32 height;
  __u32 duration; /* 96 kHz units */
  __u32 flags;
  __aligned_u64 pts_us;
  struct mlvfm_plane planes[2];
};
struct mlvfm_done {
  __u32 cookie;
  __u32 dropped; /* queued frame superseded before the sink acquired it */
};

#define MLVFM_QUERY  _IOR('M', 0x70, struct mlvfm_info)
#define MLVFM_START  _IOW('M', 0x71, struct mlvfm_start)
#define MLVFM_SUBMIT _IOW('M', 0x72, struct mlvfm_frame)
#define MLVFM_REAP   _IOR('M', 0x73, struct mlvfm_done)
#define MLVFM_STOP   _IO('M', 0x74)
#endif
