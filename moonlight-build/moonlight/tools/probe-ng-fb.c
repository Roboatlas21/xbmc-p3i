/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Read-only Amlogic fbdev capability inventory. Does not mmap framebuffers,
 * change screeninfo, unblank output, acquire DRM master, or draw pixels. */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <linux/fb.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

static int inspect(const char *path) {
  int fd=open(path,O_RDONLY|O_CLOEXEC|O_NONBLOCK);
  if(fd<0) {
    if(errno==ENOENT) {
      printf("%s: absent\n",path);
      return 0;
    }
    fprintf(stderr,"%s: open failed: %s\n",path,strerror(errno));
    return 1;
  }
  struct fb_fix_screeninfo fix;
  struct fb_var_screeninfo var;
  memset(&fix,0,sizeof(fix));
  memset(&var,0,sizeof(var));
  if(ioctl(fd,FBIOGET_FSCREENINFO,&fix)<0 ||
     ioctl(fd,FBIOGET_VSCREENINFO,&var)<0) {
    fprintf(stderr,"%s: framebuffer read-only ioctls unavailable: %s\n",
            path,strerror(errno));
    close(fd);
    return 1;
  }
  close(fd);
  printf("%s: id=%.*s size=%ux%u virtual=%ux%u bpp=%u "
         "visual=%u type=%u memory=%u bytes\n",
         path,(int)sizeof(fix.id),fix.id,var.xres,var.yres,
         var.xres_virtual,var.yres_virtual,var.bits_per_pixel,
         fix.visual,fix.type,fix.smem_len);
  printf("  RGBA channels: red(%u,%u), green(%u,%u), blue(%u,%u), transp(%u,%u)\n",
         var.red.offset,var.red.length,var.green.offset,var.green.length,
         var.blue.offset,var.blue.length,var.transp.offset,var.transp.length);
  printf("  offset=%ux%u activate=0x%x\n",var.xoffset,var.yoffset,var.activate);
  puts("  WARNING: framebuffer availability and transparency do NOT establish exclusive plane ownership.");
  return 0;
}

int main(int argc,char **argv) {
  if(argc>2) {
    fprintf(stderr,"Usage: %s [/dev/fbN]\n",argv[0]);
    return 2;
  }
  puts("CoreELEC NG fbdev inspection: NO graphics writes, mapping, or blanking.");
  if(argc==2) {
    if(strncmp(argv[1],"/dev/fb",7) ||
       argv[1][7]<'0' || argv[1][7]>'9' || argv[1][8]!='\0') {
      fputs("Specify one /dev/fbN path only\n",stderr);
      return 2;
    }
    return inspect(argv[1]);
  }
  const int zero=inspect("/dev/fb0");
  const int one=inspect("/dev/fb1");
  const int two=inspect("/dev/fb2");
  return zero || one || two ? 1 : 0;
}
