/*
 * Optional atomic DRM plane for statistics. GPL-3.0-or-later.
 * Reconstructs the standalone ARGB overlay path visible in Vortex's NO ELF.
 * Never borrows Kodi's descriptor or touches a plane that is already in use.
 */
#define _GNU_SOURCE
#include "drm.h"
#include "layout.h"
#include "plane_ownership.h"
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include <xf86drm.h>
#include <xf86drmMode.h>
#include <drm_mode.h>
#include <drm_fourcc.h>

#define OSD_PROP_COUNT 10
static const char *property_names[OSD_PROP_COUNT] = {
  "FB_ID", "CRTC_ID", "SRC_X", "SRC_Y", "SRC_W", "SRC_H",
  "CRTC_X", "CRTC_Y", "CRTC_W", "CRTC_H"
};

typedef struct {
  uint32_t handle, fb, pitch;
  uint64_t bytes;
  uint32_t *pixels;
} OsdBuffer;

struct OsdDrm {
  int fd, width, height, x, y, scale, crtc_index;
  uint32_t crtc_id, plane_id, prop[OSD_PROP_COUNT];
  OsdBuffer buffer[2];
  int visible;
  unsigned max_rows;
  bool owns_plane;
  bool reused_attached_plane;
  bool plane_committed;
};

/* Tiny, dependency-free 5x7 diagnostic font: space, digits, letters, symbols. */
static const uint8_t bitmap[][7] = {
  {0,0,0,0,0,0,0},
  {14,17,19,21,25,17,14},{4,12,4,4,4,4,14},
  {14,17,1,2,4,8,31},{30,1,1,14,1,1,30},
  {2,6,10,18,31,2,2},{31,16,30,1,1,17,14},
  {6,8,16,30,17,17,14},{31,1,2,4,8,8,8},
  {14,17,17,14,17,17,14},{14,17,17,15,1,2,12},
  {14,17,17,31,17,17,17},{30,17,17,30,17,17,30},
  {14,17,16,16,16,17,14},{30,17,17,17,17,17,30},
  {31,16,16,30,16,16,31},{31,16,16,30,16,16,16},
  {14,17,16,23,17,17,14},{17,17,17,31,17,17,17},
  {14,4,4,4,4,4,14},{7,2,2,2,18,18,12},
  {17,18,20,24,20,18,17},{16,16,16,16,16,16,31},
  {17,27,21,21,17,17,17},{17,25,21,19,17,17,17},
  {14,17,17,17,17,17,14},{30,17,17,30,16,16,16},
  {14,17,17,17,21,18,13},{30,17,17,30,20,18,17},
  {15,16,16,14,1,1,30},{31,4,4,4,4,4,4},
  {17,17,17,17,17,17,14},{17,17,17,17,17,10,4},
  {17,17,17,21,21,21,10},{17,17,10,4,10,17,17},
  {17,17,10,4,4,4,4},{31,1,2,4,8,16,31},
  {0,0,0,31,0,0,0},{0,0,0,0,0,12,12},
  {0,4,0,0,4,0,0},{1,2,2,4,8,8,16},
  {6,9,10,4,10,18,12},{4,4,4,4,4,4,4},
  {0,0,0,31,0,0,0},{6,9,2,4,4,0,4},
  {0,0,0,0,4,8,16}
};

static const uint8_t *glyph(char c) {
  if (c >= '0' && c <= '9') return bitmap[1+c-'0'];
  if (c >= 'a' && c <= 'z') c -= ('a'-'A');
  if (c >= 'A' && c <= 'Z') return bitmap[11+c-'A'];
  const char *special = "-.:/%|=?,";
  const char *at = strchr(special, c);
  return at ? bitmap[37+(size_t)(at-special)] : bitmap[0];
}

static void draw_text(OsdDrm *s, OsdBuffer *buf, const char *text, int x, int y) {
  const int jump = 6*s->scale;
  for (const char *c=text; *c && x+5*s->scale<s->width; c++,x+=jump) {
    const uint8_t *g=glyph(*c);
    for(int row=0; row<7; row++) for(int col=0; col<5; col++) {
      if (!(g[row] & (1 << (4-col)))) continue;
      for(int yy=0;yy<s->scale;yy++) for(int xx=0;xx<s->scale;xx++) {
        const int px=x+col*s->scale+xx, py=y+row*s->scale+yy;
        if (px>=0 && px<s->width && py>=0 && py<s->height)
          buf->pixels[(size_t)py*(buf->pitch/4)+(size_t)px]=0xffffffffu;
      }
    }
  }
}

static void destroy_buffer(OsdDrm *s, OsdBuffer *b) {
  if(b->fb) drmModeRmFB(s->fd,b->fb);
  if(b->pixels) munmap(b->pixels,b->bytes);
  if(b->handle) {
    struct drm_mode_destroy_dumb arg={.handle=b->handle};
    drmIoctl(s->fd,DRM_IOCTL_MODE_DESTROY_DUMB,&arg);
  }
  memset(b,0,sizeof(*b));
}

static bool create_buffer(OsdDrm *s,OsdBuffer *b) {
  struct drm_mode_create_dumb arg={.width=(uint32_t)s->width,
                                   .height=(uint32_t)s->height,.bpp=32};
  if(drmIoctl(s->fd,DRM_IOCTL_MODE_CREATE_DUMB,&arg)<0) return false;
  b->handle=arg.handle; b->bytes=arg.size; b->pitch=arg.pitch;
  struct drm_mode_map_dumb mapping={.handle=arg.handle};
  if(drmIoctl(s->fd,DRM_IOCTL_MODE_MAP_DUMB,&mapping)<0) return false;
  b->pixels=mmap(NULL,b->bytes,PROT_READ|PROT_WRITE,MAP_SHARED,s->fd,mapping.offset);
  if(b->pixels==MAP_FAILED){b->pixels=NULL;return false;}
  uint32_t handles[4]={b->handle}, pitches[4]={b->pitch}, offsets[4]={0};
  return drmModeAddFB2(s->fd,s->width,s->height,DRM_FORMAT_ARGB8888,
                       handles,pitches,offsets,&b->fb,0)==0;
}

static bool get_plane_props(OsdDrm *s,uint32_t plane) {
  drmModeObjectPropertiesPtr props=drmModeObjectGetProperties(s->fd,plane,DRM_MODE_OBJECT_PLANE);
  if(!props) return false;
  uint32_t ids[OSD_PROP_COUNT]={0};
  bool overlay=false;
  for(uint32_t i=0;i<props->count_props;i++){
    drmModePropertyPtr prop=drmModeGetProperty(s->fd,props->props[i]);
    if(!prop)continue;
    for(int j=0;j<OSD_PROP_COUNT;j++)
      if(!strcmp(prop->name,property_names[j]))ids[j]=prop->prop_id;
    if(!strcmp(prop->name,"type") && props->prop_values[i]==DRM_PLANE_TYPE_OVERLAY)
      overlay=true;
    drmModeFreeProperty(prop);
  }
  drmModeFreeObjectProperties(props);
  if(!overlay) return false;
  for(int i=0;i<OSD_PROP_COUNT;i++)if(!ids[i])return false;
  memcpy(s->prop,ids,sizeof(ids));
  return true;
}

static bool plane_supports_argb(drmModePlanePtr p) {
  if(!p)return false;
  for(uint32_t j=0;j<p->count_formats;j++)
    if(p->formats[j]==DRM_FORMAT_ARGB8888)return true;
  return false;
}

static bool choose_plane(OsdDrm *s) {
  drmModePlaneResPtr list=drmModeGetPlaneResources(s->fd);
  if(!list){
    fprintf(stderr,"OSD: cannot enumerate DRM planes: %s\n",strerror(errno));
    return false;
  }

  /* First choice remains a genuinely unused overlay plane. */
  for(uint32_t i=0;i<list->count_planes;i++){
    drmModePlanePtr p=drmModeGetPlane(s->fd,list->planes[i]);
    if(!p)continue;
    const bool compatible=s->crtc_index<32 &&
        (p->possible_crtcs & (1u<<s->crtc_index));
    if(plane_supports_argb(p) && compatible && !p->fb_id && !p->crtc_id &&
       get_plane_props(s,p->plane_id)) {
      s->plane_id=p->plane_id;
      drmModeFreePlane(p);
      drmModeFreePlaneResources(list);
      fprintf(stderr,"OSD: selected unused ARGB overlay plane %u\n",s->plane_id);
      return true;
    }
    drmModeFreePlane(p);
  }

  /* Vortex borrows Kodi's KMS context. CE21 NG's 4.9 kernel lacks the
   * pidfd_getfd path used by the NO build, but Luna stops Kodi before
   * Moonlight starts. In that explicitly-authorized handoff we may safely
   * replace Kodi's still-attached ARGB overlay plane on the active CRTC.
   * Never do this for a standalone Moonlight invocation. */
  const char *takeover=getenv("MOONLIGHT_OSD_TAKEOVER");
  const bool allow_attached=takeover && !strcmp(takeover,"1");
  if(allow_attached){
    for(uint32_t i=0;i<list->count_planes;i++){
      drmModePlanePtr p=drmModeGetPlane(s->fd,list->planes[i]);
      if(!p)continue;
      const bool compatible=s->crtc_index<32 &&
          (p->possible_crtcs & (1u<<s->crtc_index));
      const bool on_target=p->crtc_id==s->crtc_id && p->crtc_id!=0;
      if(plane_supports_argb(p) && compatible && on_target &&
         get_plane_props(s,p->plane_id)) {
        s->plane_id=p->plane_id;
        s->reused_attached_plane=true;
        fprintf(stderr,
                "OSD: reusing attached ARGB overlay plane %u after Kodi handoff\n",
                s->plane_id);
        drmModeFreePlane(p);
        drmModeFreePlaneResources(list);
        return true;
      }
      drmModeFreePlane(p);
    }
  }

  fprintf(stderr,
          "OSD: no usable ARGB overlay plane%s\n",
          allow_attached ? " after Kodi handoff" : "");
  drmModeFreePlaneResources(list);
  return false;
}

static bool set_plane(OsdDrm *s,uint32_t fb) {
  drmModeAtomicReqPtr req=drmModeAtomicAlloc();
  if(!req)return false;
  const uint64_t vals[OSD_PROP_COUNT]={
    fb,fb?s->crtc_id:0,0,0,
    (uint64_t)s->width<<16,(uint64_t)s->height<<16,
    (uint64_t)s->x,(uint64_t)s->y,(uint64_t)s->width,(uint64_t)s->height
  };
  bool good=true;
  for(int i=0;i<OSD_PROP_COUNT;i++){
    if(drmModeAtomicAddProperty(req,s->plane_id,s->prop[i],vals[i])<0){good=false;break;}
  }
  if(good && drmModeAtomicCommit(s->fd,req,0,NULL)!=0) good=false;
  drmModeAtomicFree(req);
  return good;
}

void osd_drm_close(OsdDrm *s) {
  if(!s)return;
  if(s->fd>=0 && osd_plane_disable_needed(s->owns_plane, s->plane_committed) && !set_plane(s,0))
    fprintf(stderr,"OSD: cannot disable overlay plane %u on shutdown\n",s->plane_id);
  if(s->fd>=0){
    for(int i=0;i<2;i++) destroy_buffer(s,&s->buffer[i]);
    close(s->fd);
  }
  free(s);
}

OsdDrm *osd_drm_open(const OsdOptions *opt) {
  OsdDrm *s=calloc(1,sizeof(*s));
  if(!s)return NULL;
  s->fd=-1;
  s->fd=open("/dev/dri/card0",O_RDWR|O_CLOEXEC);
  if(s->fd<0){
    fprintf(stderr,"OSD: cannot open /dev/dri/card0: %s\n",strerror(errno));
    goto fail;
  }
  /* NG 4.9 cannot use Vortex's pidfd_getfd DRM borrowing path. Luna stops
   * Kodi first, so Moonlight should become DRM master for the OSD lifetime. */
  if(drmSetMaster(s->fd)!=0){
    fprintf(stderr,"OSD: cannot become DRM master: %s\n",strerror(errno));
    goto fail;
  }
  if(drmSetClientCap(s->fd,DRM_CLIENT_CAP_UNIVERSAL_PLANES,1)!=0){
    fprintf(stderr,"OSD: universal planes unsupported: %s\n",strerror(errno));
    goto fail;
  }
  if(drmSetClientCap(s->fd,DRM_CLIENT_CAP_ATOMIC,1)!=0){
    fprintf(stderr,"OSD: atomic KMS unsupported: %s\n",strerror(errno));
    goto fail;
  }
  drmModeResPtr resources=drmModeGetResources(s->fd);
  if(!resources){
    fprintf(stderr,"OSD: cannot read DRM resources: %s\n",strerror(errno));
    goto fail;
  }
  int screen_w=0,screen_h=0;
  for(int i=0;i<resources->count_crtcs;i++){
    drmModeCrtcPtr crtc=drmModeGetCrtc(s->fd,resources->crtcs[i]);
    if(crtc && crtc->mode_valid && crtc->width>0 && crtc->height>0){
      s->crtc_id=resources->crtcs[i];s->crtc_index=i;
      screen_w=crtc->width;screen_h=crtc->height;
      drmModeFreeCrtc(crtc);
      break;
    }
    if(crtc)drmModeFreeCrtc(crtc);
  }
  drmModeFreeResources(resources);
  if(screen_w<640 || screen_h<360){
    fprintf(stderr,"OSD: no active DRM CRTC/mode found\n");
    goto fail;
  }
  if(!choose_plane(s))goto fail;
  s->scale=opt->font_size/8;
  if(s->scale<1)s->scale=1;
  if(s->scale>5)s->scale=5;
  s->width=screen_w>1140?1100:screen_w-30;
  /* Space for up to eight measured rows, but limit the panel footprint
   * on lower-resolution output. Unused rows remain fully transparent. */
  s->max_rows = OSD_LAYOUT_MAX_ROWS;
  while (s->max_rows > 2 &&
         (int)(s->max_rows * (unsigned)s->scale * 9 + 16) > screen_h / 2)
    s->max_rows--;
  s->height = (int)s->max_rows * s->scale * 9 + 16;
  s->x=opt->x<0 ? screen_w-s->width-20 : opt->x;
  s->y=opt->y;
  if(s->x<0 || s->x+s->width>screen_w || s->y<0 || s->y+s->height>screen_h)goto fail;
  if(!create_buffer(s,&s->buffer[0]) || !create_buffer(s,&s->buffer[1])){
    fprintf(stderr,"OSD: failed to create ARGB dumb buffers: %s\n",strerror(errno));
    goto fail;
  }
  s->visible=-1;
  s->owns_plane=true;
  fprintf(stderr,"OSD: visual overlay ready on plane %u%s\n",
          s->plane_id,s->reused_attached_plane?" (Kodi handoff)":"");
  return s;
fail:
  osd_drm_close(s);
  return NULL;
}

bool osd_drm_draw(OsdDrm *s,const char *first,const char *second) {
  const int index=s->visible==0?1:0;
  OsdBuffer *b=&s->buffer[index];
  if(!b->pixels || b->pitch<(uint32_t)(s->width*4))return false;
  /* The bounded wrapper prevents silently drawing beyond the right edge.
   * Make unused space transparent rather than darkening the video. */
  OsdLayout layout;
  const unsigned cols=(unsigned)(s->width-24)/(unsigned)(6*s->scale);
  osd_layout_wrap(first,second,cols,s->max_rows,&layout);
  for(int row=0;row<s->height;row++){
    uint32_t *p=(uint32_t*)((char*)b->pixels+(size_t)row*b->pitch);
    const uint32_t background =
        row < (int)layout.rows*s->scale*9 + 16 ? 0xb0000000u : 0x00000000u;
    for(int col=0;col<s->width;col++)p[col]=background;
  }
  for(unsigned line=0;line<layout.rows;line++)
    draw_text(s,b,layout.text[line],12,8+(int)line*s->scale*9);
  if(!set_plane(s,b->fb))return false;
  s->plane_committed=true;
  s->visible=index;
  return true;
}
