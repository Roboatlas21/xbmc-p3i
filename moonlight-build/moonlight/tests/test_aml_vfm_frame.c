/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <assert.h>
#include <stdio.h>
#include <libavutil/hwcontext_drm.h>
#include <drm_fourcc.h>
#include "../src/video/aml_vfm_frame.h"

int main(void)
{
  AVDRMFrameDescriptor desc = {0};
  AVFrame frame = {0};
  struct mlvfm_frame out;
  desc.nb_objects = desc.nb_layers = 1;
  desc.objects[0].fd = 7;
  desc.objects[0].size = 1920 * 1080 * 3 / 2;
  desc.objects[0].format_modifier = DRM_FORMAT_MOD_LINEAR;
  desc.layers[0].format = DRM_FORMAT_NV12;
  desc.layers[0].nb_planes = 2;
  desc.layers[0].planes[0].pitch = desc.layers[0].planes[1].pitch = 1920;
  desc.layers[0].planes[1].offset = 1920 * 1080;
  frame.width = 1920;
  frame.height = 1080;
  frame.format = AV_PIX_FMT_DRM_PRIME;
  frame.data[0] = (unsigned char *)&desc;
  frame.colorspace = AVCOL_SPC_BT709;
  frame.pts = 12345;
  assert(aml_vfm_frame(&frame, 1920, 1080, 1600, &out) == 0);
  assert(out.planes[1].offset == 1920 * 1080 && out.pts_us == 12345);
  assert(out.flags == MLVFM_BT709);
  frame.color_range = AVCOL_RANGE_JPEG;
  assert(aml_vfm_frame(&frame, 1920, 1080, 1600, &out) == 0);
  assert(out.flags == (MLVFM_BT709 | MLVFM_FULL_RANGE));
  desc.objects[0].size--;
  assert(aml_vfm_frame(&frame, 1920, 1080, 1600, &out) < 0);
  desc.objects[0].size++;
  desc.layers[0].planes[1].object_index = 2;
  assert(aml_vfm_frame(&frame, 1920, 1080, 1600, &out) < 0);
  desc.layers[0].planes[1].object_index = 0;
  desc.objects[0].format_modifier = DRM_FORMAT_MOD_INVALID;
  assert(aml_vfm_frame(&frame, 1920, 1080, 1600, &out) < 0);
  desc.objects[0].format_modifier = DRM_FORMAT_MOD_LINEAR;
  frame.color_trc = AVCOL_TRC_SMPTE2084;
  assert(aml_vfm_frame(&frame, 1920, 1080, 1600, &out) < 0);
  frame.color_trc = AVCOL_TRC_BT709;
  frame.crop_bottom = 2;
  assert(aml_vfm_frame(&frame, 1920, 1080, 1600, &out) < 0);
  frame.crop_bottom = 0;
  frame.format = AV_PIX_FMT_NV12;
  assert(aml_vfm_frame(&frame, 1920, 1080, 1600, &out) < 0);
  frame.format = AV_PIX_FMT_DRM_PRIME;
  desc.layers[0].planes[0].pitch = 1919;
  assert(aml_vfm_frame(&frame, 1920, 1080, 1600, &out) < 0);
  puts("VFM descriptors: bounds, layouts, color and format rejection OK");
  return 0;
}
