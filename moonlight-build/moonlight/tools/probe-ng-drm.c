/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Read-only DRM plane inventory. NO drmSetMaster, framebuffer creation,
 * atomic property writes, KMS commits, or ownership changes. */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <xf86drm.h>
#include <xf86drmMode.h>
#include <drm_fourcc.h>

static bool argb_format(const drmModePlanePtr plane) {
  for (uint32_t i=0; i<plane->count_formats; i++)
    if (plane->formats[i] == DRM_FORMAT_ARGB8888) return true;
  return false;
}

static const char *plane_type(int fd, uint32_t id) {
  drmModeObjectPropertiesPtr list =
    drmModeObjectGetProperties(fd, id, DRM_MODE_OBJECT_PLANE);
  if (!list) return "unknown";
  const char *result="unknown";
  for(uint32_t i=0;i<list->count_props;i++) {
    drmModePropertyPtr prop=drmModeGetProperty(fd,list->props[i]);
    if (!prop) continue;
    if (!strcmp(prop->name,"type")) {
      if (list->prop_values[i]==DRM_PLANE_TYPE_OVERLAY) result="overlay";
      else if (list->prop_values[i]==DRM_PLANE_TYPE_PRIMARY) result="primary";
      else if (list->prop_values[i]==DRM_PLANE_TYPE_CURSOR) result="cursor";
    }
    drmModeFreeProperty(prop);
  }
  drmModeFreeObjectProperties(list);
  return result;
}

int main(int argc, char **argv) {
  const char *path=argc>1?argv[1]:"/dev/dri/card0";
  if (argc>2) { fprintf(stderr,"Usage: %s [/dev/dri/card0]\n",argv[0]);return 2; }
  int fd=open(path,O_RDWR|O_CLOEXEC);
  if(fd<0) { perror("DRM probe open");return 1; }
  puts("DRM plane inventory: read-only. This program never takes KMS master.");
  if (drmSetClientCap(fd,DRM_CLIENT_CAP_UNIVERSAL_PLANES,1)<0)
    fprintf(stderr,"Universal plane enumeration unavailable: %s\n",strerror(errno));
  if (drmSetClientCap(fd,DRM_CLIENT_CAP_ATOMIC,1)<0)
    fprintf(stderr,"Atomic client capability unavailable: %s\n",strerror(errno));
  drmModeResPtr res=drmModeGetResources(fd);
  if (!res) {
    fprintf(stderr,"DRM resources unavailable: %s\n",strerror(errno));
    close(fd);
    return 1;
  }
  printf("CRTCs=%d\n",res->count_crtcs);
  drmModePlaneResPtr planes=drmModeGetPlaneResources(fd);
  if (!planes) {
    fprintf(stderr,"No readable plane inventory: %s\n",strerror(errno));
    drmModeFreeResources(res);
    close(fd);
    return 1;
  }
  unsigned candidates=0;
  for(uint32_t i=0;i<planes->count_planes;i++) {
    drmModePlanePtr p=drmModeGetPlane(fd,planes->planes[i]);
    if (!p) continue;
    const char *type=plane_type(fd,p->plane_id);
    bool idle=p->fb_id==0 && p->crtc_id==0;
    bool argb=argb_format(p);
    printf("plane=%u type=%s idle=%s ARGB8888=%s possible_crtcs=0x%x\n",
           p->plane_id,type,idle?"yes":"no",argb?"yes":"no",p->possible_crtcs);
    if (idle && argb && !strcmp(type,"overlay")) candidates++;
    drmModeFreePlane(p);
  }
  printf("Candidate unused ARGB overlay planes: %u (NOT permission or safety proof)\n",
         candidates);
  puts("Cannot infer DRM master ownership from a candidate. Do not write to Kodi planes.");
  drmModeFreePlaneResources(planes);
  drmModeFreeResources(res);
  close(fd);
  return 0;
}
