/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "aml_vfm_frame.h"
#include <libavutil/hwcontext_drm.h>
#include <libavutil/avutil.h>
#include <drm_fourcc.h>
#include <errno.h>
#include <stdint.h>
#include <string.h>

int aml_vfm_frame(const AVFrame *frame, int width, int height,
                  unsigned int duration, struct mlvfm_frame *output)
{
  const AVDRMFrameDescriptor *desc;
  const AVDRMLayerDescriptor *layer;
  memset(output, 0, sizeof(*output));
  unsigned int p;
  if (frame->format != AV_PIX_FMT_DRM_PRIME || !frame->data[0] ||
      frame->crop_top || frame->crop_bottom || frame->crop_left || frame->crop_right ||
      frame->width != width || frame->height != height ||
      width < 2 || height < 2 || width > 4096 || height > 2160 ||
      (width & 1) || (height & 1) || duration == 0 || duration > 96000 ||
      frame->color_trc == AVCOL_TRC_SMPTE2084 || frame->color_trc == AVCOL_TRC_ARIB_STD_B67)
    return AVERROR(ENOTSUP);
  desc = (const AVDRMFrameDescriptor *)frame->data[0];
  if (desc->nb_layers != 1 || desc->nb_objects < 1 || desc->nb_objects > 2)
    return AVERROR(ENOTSUP);
  layer = &desc->layers[0];
  if (layer->format != DRM_FORMAT_NV12 || layer->nb_planes != 2)
    return AVERROR(ENOTSUP);
  output->fourcc = MLVFM_NV12;
  output->width = frame->width;
  output->height = frame->height;
  output->duration = duration;
  output->pts_us = frame->pts == AV_NOPTS_VALUE || frame->pts < 0 ? 0 : frame->pts;
  if (frame->color_range == AVCOL_RANGE_JPEG)
    output->flags |= MLVFM_FULL_RANGE;
  if (frame->colorspace == AVCOL_SPC_BT709 ||
      (frame->colorspace == AVCOL_SPC_UNSPECIFIED && frame->height >= 720))
    output->flags |= MLVFM_BT709;
  else if (frame->colorspace != AVCOL_SPC_SMPTE170M &&
           frame->colorspace != AVCOL_SPC_BT470BG &&
           frame->colorspace != AVCOL_SPC_UNSPECIFIED)
    return AVERROR(ENOTSUP);
  for (p = 0; p < 2; ++p) {
    const AVDRMPlaneDescriptor *plane = &layer->planes[p];
    const AVDRMObjectDescriptor *obj;
    uint64_t end;
    if (plane->object_index < 0 || plane->object_index >= desc->nb_objects ||
        plane->offset < 0 || plane->offset > UINT32_MAX ||
        plane->pitch < frame->width || plane->pitch > 16384 || (plane->pitch & 31))
      return AVERROR(EINVAL);
    obj = &desc->objects[plane->object_index];
    end = (uint64_t)plane->offset + (uint64_t)plane->pitch * (p ? frame->height / 2 : frame->height);
    if (obj->fd < 0 || obj->format_modifier != DRM_FORMAT_MOD_LINEAR || end > obj->size)
      return AVERROR(ENOTSUP);
    output->planes[p].fd = obj->fd;
    output->planes[p].offset = plane->offset;
    output->planes[p].pitch = plane->pitch;
  }
  return 0;
}
