/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef MOONLIGHT_OSD_DRM_H
#define MOONLIGHT_OSD_DRM_H
#include <stdbool.h>
#include "osd.h"
typedef struct OsdDrm OsdDrm;
/* Uses only an unused ARGB8888 OVERLAY plane. Fails closed otherwise. */
OsdDrm *osd_drm_open(const OsdOptions *options);
bool osd_drm_draw(OsdDrm *drm, const char *line1, const char *line2);
void osd_drm_close(OsdDrm *drm);
#endif
