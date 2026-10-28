/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef AML_VFM_FRAME_H
#define AML_VFM_FRAME_H
#include <libavutil/frame.h>
#include "../../kernel/moonlight-vfm/moonlight_vfm.h"
int aml_vfm_frame(const AVFrame *frame, int width, int height,
                  unsigned int duration, struct mlvfm_frame *output);
#endif
