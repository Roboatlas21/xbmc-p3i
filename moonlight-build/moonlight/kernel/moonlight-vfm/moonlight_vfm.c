// SPDX-License-Identifier: GPL-2.0
/* Experimental CE21 NG linear NV12 DMA-BUF -> existing amvideo provider.
 * No DRM modesetting, UVM dependency, framebuffer replacement or DT changes.
 * The userspace client retains each AVFrame until REAP or successful STOP.
 */
#include <linux/module.h>
#include <linux/math64.h>
#include <linux/miscdevice.h>
#include <linux/fs.h>
#include <linux/poll.h>
#include <linux/compat.h>
#include <linux/slab.h>
#include <linux/mutex.h>
#include <linux/spinlock.h>
#include <linux/dma-buf.h>
#include <linux/dma-mapping.h>
#include <linux/scatterlist.h>
#include <linux/uaccess.h>
#include <linux/amlogic/media/vfm/vframe_provider.h>
#include <linux/amlogic/media/vfm/vframe_receiver.h>
#include "moonlight_vfm.h"
#include "mlvfm_queue.h"

/* Exported in the pinned CE21 NG kernel; intentionally no symbol lookup or
 * register poking fallback if the installed kernel does not provide these. */
extern int vfm_map_add(char *id, char *chain);
extern int vfm_map_remove(char *id);
extern u32 set_blackout_policy(int policy);

#define PROVIDER "moonlight_vfm"
struct imported_plane {
  struct dma_buf *buf;
  struct dma_buf_attachment *attachment;
  struct sg_table *sg;
  dma_addr_t address;
};
struct frame_slot {
  struct vframe_s vf;
  struct imported_plane plane[2];
  bool dropped;
};
struct session {
  struct mutex ioctl_lock;
  spinlock_t lock;
  wait_queue_head_t wait;
  struct vframe_provider_s provider;
  struct mlvfm_queue queue;
  struct frame_slot slots[MLVFM_SLOTS];
  bool active, stopping;
  u32 blackout_policy;
};
static atomic_t opened = ATOMIC_INIT(0);
static struct miscdevice bridge;
static u64 bridge_dma_mask = DMA_BIT_MASK(32);

static void release_plane(struct imported_plane *p)
{
  if (p->sg)
    dma_buf_unmap_attachment(p->attachment, p->sg, DMA_TO_DEVICE);
  if (p->attachment)
    dma_buf_detach(p->buf, p->attachment);
  if (p->buf)
    dma_buf_put(p->buf);
  memset(p, 0, sizeof(*p));
}
static void release_slot(struct frame_slot *slot)
{
  release_plane(&slot->plane[0]);
  release_plane(&slot->plane[1]);
}
static int import_plane(struct imported_plane *p,
                        const struct mlvfm_plane *src, u32 rows)
{
  u64 end = (u64)src->offset + (u64)src->pitch * rows;
  int ret;
  p->buf = dma_buf_get(src->fd);
  if (IS_ERR(p->buf)) {
    ret = PTR_ERR(p->buf);
    p->buf = NULL;
    return ret;
  }
  if (end > p->buf->size)
    return -EINVAL;
  p->attachment = dma_buf_attach(p->buf, bridge.this_device);
  if (IS_ERR(p->attachment)) {
    ret = PTR_ERR(p->attachment);
    p->attachment = NULL;
    return ret;
  }
  p->sg = dma_buf_map_attachment(p->attachment, DMA_TO_DEVICE);
  if (IS_ERR(p->sg)) {
    ret = PTR_ERR(p->sg);
    p->sg = NULL;
    return ret;
  }
  /* The vendor canvas uses physical addresses. Refuse scatter/gather or an
   * IOMMU-translated mapping rather than programming the wrong address. */
  if (p->sg->nents != 1 || p->sg->orig_nents != 1 ||
      sg_dma_address(p->sg->sgl) != sg_phys(p->sg->sgl) ||
      end > sg_dma_len(p->sg->sgl))
    return -EOPNOTSUPP;
  p->address = sg_dma_address(p->sg->sgl) + src->offset;
  if ((p->address & 7) ||
      (u64)sg_dma_address(p->sg->sgl) + end > (1ULL << 32))
    return -EINVAL;
  return 0;
}
static struct vframe_s *provider_peek(void *arg)
{
  struct session *s = arg;
  unsigned long flags;
  int i;
  spin_lock_irqsave(&s->lock, flags);
  i = s->stopping ? -1 : mlvfm_peek(&s->queue);
  spin_unlock_irqrestore(&s->lock, flags);
  return i < 0 ? NULL : &s->slots[i].vf;
}
static struct vframe_s *provider_get(void *arg)
{
  struct session *s = arg;
  unsigned long flags;
  int i;
  spin_lock_irqsave(&s->lock, flags);
  i = s->stopping ? -1 : mlvfm_get(&s->queue);
  spin_unlock_irqrestore(&s->lock, flags);
  return i < 0 ? NULL : &s->slots[i].vf;
}
static void provider_put(struct vframe_s *vf, void *arg)
{
  struct session *s = arg;
  unsigned long flags;
  unsigned int i;
  spin_lock_irqsave(&s->lock, flags);
  for (i = 0; i < MLVFM_SLOTS; ++i)
    if (vf == &s->slots[i].vf) {
      mlvfm_put(&s->queue, i);
      break;
    }
  spin_unlock_irqrestore(&s->lock, flags);
  wake_up_interruptible(&s->wait);
}
static int provider_event(int type, void *data, void *arg)
{
  /* Never invent decoder metadata for HDR/DV or participate in deinterlacing. */
  if (type == VFRAME_EVENT_RECEIVER_GET_AUX_DATA && data) {
    struct provider_aux_req_s *req = data;
    req->aux_buf = NULL;
    req->aux_size = 0;
    req->dv_enhance_exist = 0;
  }
  return 0;
}
static int provider_states(struct vframe_states *states, void *arg)
{
  struct session *s = arg;
  unsigned long flags;
  unsigned int i;
  memset(states, 0, sizeof(*states));
  states->vf_pool_size = MLVFM_SLOTS;
  spin_lock_irqsave(&s->lock, flags);
  states->buf_avail_num = s->queue.count;
  for (i = 0; i < MLVFM_SLOTS; ++i) {
    states->buf_free_num += s->queue.state[i] == MLVFM_FREE;
    states->buf_recycle_num += s->queue.state[i] == MLVFM_DONE;
  }
  spin_unlock_irqrestore(&s->lock, flags);
  return 0;
}
static const struct vframe_operations_s provider_ops = {
  .peek = provider_peek, .get = provider_get, .put = provider_put,
  .event_cb = provider_event, .vf_states = provider_states,
};

static void stop_session(struct session *s)
{
  unsigned int i;
  unsigned long flags;
  if (!s->active)
    return;
  spin_lock_irqsave(&s->lock, flags);
  s->stopping = true;
  spin_unlock_irqrestore(&s->lock, flags);
  /* In CE21 this makes provider UNREG drain the IRQ, synchronously disable
   * the layer, and skip vf_keep_current(). Do it BEFORE unmapping any buffer.
   * Unlike FORCE_BLACKOUT, this policy cannot be cleared by an already running
   * display IRQ before UNREG drains it. START saved the actual sysfs policy,
   * not get_blackout_policy() (which also includes the transient force flag).
   * This cleanup also runs on close/process death. */
  set_blackout_policy(1);
  vf_unreg_provider(&s->provider);
  /* After the old IRQ has drained, keep the detached last-frame metadata
   * blacked out until the next provider supplies a real frame. */
  vf_notify_receiver_by_name("amvideo", VFRAME_EVENT_PROVIDER_FORCE_BLACKOUT, NULL);
  vfm_map_remove(PROVIDER);
  set_blackout_policy(s->blackout_policy);
  s->active = false;
  for (i = 0; i < MLVFM_SLOTS; ++i)
    release_slot(&s->slots[i]);
  spin_lock_irqsave(&s->lock, flags);
  memset(&s->queue, 0, sizeof(s->queue));
  spin_unlock_irqrestore(&s->lock, flags);
  wake_up_interruptible(&s->wait);
}
static int start_session(struct session *s, const struct mlvfm_start *start)
{
  char chain[] = PROVIDER " amvideo";
  int ret;
  if (start->abi != MLVFM_ABI || start->pending_frames < 1 ||
      start->pending_frames > 2 || start->blackout_policy > 1 || start->reserved)
    return -EINVAL;
  if (s->active || vf_get_provider("amvideo"))
    return -EBUSY; /* Never steal a live Kodi/decoder route. */
  memset(&s->queue, 0, sizeof(s->queue));
  s->queue.limit = start->pending_frames;
  s->blackout_policy = start->blackout_policy;
  s->stopping = false;
  ret = vfm_map_add(PROVIDER, chain);
  if (ret)
    return -EBUSY;
  vf_provider_init(&s->provider, PROVIDER, &provider_ops, s);
  ret = vf_reg_provider(&s->provider);
  if (ret) {
    vfm_map_remove(PROVIDER);
    return -EBUSY;
  }
  s->active = true;
  if (vf_get_provider("amvideo") != &s->provider) {
    stop_session(s);
    return -EBUSY;
  }
  vf_notify_receiver(PROVIDER, VFRAME_EVENT_PROVIDER_START, NULL);
  return 0;
}
static int submit_frame(struct session *s, const struct mlvfm_frame *f)
{
  unsigned int i = f->cookie, p;
  unsigned long flags;
  struct frame_slot *slot;
  struct vframe_s *vf;
  int ret, dropped;
  if (!s->active || s->stopping)
    return -EPIPE;
  if (i >= MLVFM_SLOTS || f->fourcc != MLVFM_NV12 ||
      !f->width || !f->height || f->width > 4096 || f->height > 2160 ||
      (f->width & 1) || (f->height & 1) || !f->duration ||
      f->duration > 96000 || (f->flags & ~(MLVFM_FULL_RANGE | MLVFM_BT709)))
    return -EINVAL;
  for (p = 0; p < 2; ++p)
    if (f->planes[p].fd < 0 || f->planes[p].reserved ||
        f->planes[p].pitch < f->width || f->planes[p].pitch > 16384 ||
        (f->planes[p].pitch & 31))
      return -EINVAL;
  spin_lock_irqsave(&s->lock, flags);
  ret = mlvfm_reserve(&s->queue, i);
  spin_unlock_irqrestore(&s->lock, flags);
  if (ret)
    return -EBUSY;
  slot = &s->slots[i];
  memset(slot, 0, sizeof(*slot));
  for (p = 0; p < 2; ++p) {
    ret = import_plane(&slot->plane[p], &f->planes[p],
                       p ? f->height / 2 : f->height);
    if (ret)
      goto fail;
  }
  vf = &slot->vf;
  vf->index = i;
  vf->type = VIDTYPE_PROGRESSIVE | VIDTYPE_VIU_FIELD | VIDTYPE_VIU_NV12;
  vf->type_original = vf->type;
  vf->width = f->width;
  vf->height = f->height;
  vf->bufWidth = f->planes[0].pitch;
  vf->duration = f->duration;
  vf->pts_us64 = f->pts_us;
  vf->pts = div_u64(f->pts_us, 100) * 9;
  vf->canvas0Addr = vf->canvas1Addr = (u32)-1;
  vf->plane_num = 2;
  vf->signal_type = (1U << 29) | (5U << 26) | (1U << 24) |
    ((f->flags & MLVFM_FULL_RANGE) ? (1U << 25) : 0) |
    ((f->flags & MLVFM_BT709) ? 0x010101 : 0x060606);
  for (p = 0; p < 2; ++p) {
    vf->canvas0_config[p].phy_addr = slot->plane[p].address;
    vf->canvas0_config[p].width = f->planes[p].pitch;
    vf->canvas0_config[p].height = p ? f->height / 2 : f->height;
    vf->canvas0_config[p].block_mode = CANVAS_BLKMODE_LINEAR;
    vf->canvas1_config[p] = vf->canvas0_config[p];
  }
  spin_lock_irqsave(&s->lock, flags);
  dropped = mlvfm_publish(&s->queue, i);
  if (dropped >= 0)
    s->slots[dropped].dropped = true;
  spin_unlock_irqrestore(&s->lock, flags);
  vf_notify_receiver(PROVIDER, VFRAME_EVENT_PROVIDER_VFRAME_READY, NULL);
  wake_up_interruptible(&s->wait);
  return 0;
fail:
  release_slot(slot);
  spin_lock_irqsave(&s->lock, flags);
  s->queue.state[i] = MLVFM_FREE;
  spin_unlock_irqrestore(&s->lock, flags);
  return ret;
}
static long bridge_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
  struct session *s = file->private_data;
  void __user *user = (void __user *)arg;
  struct mlvfm_info info = { MLVFM_ABI, MLVFM_SLOTS };
  struct mlvfm_start start;
  struct mlvfm_frame frame;
  struct mlvfm_done done;
  unsigned long flags;
  unsigned int i;
  long ret = 0;
  mutex_lock(&s->ioctl_lock);
  switch (cmd) {
  case MLVFM_QUERY:
    ret = copy_to_user(user, &info, sizeof(info)) ? -EFAULT : 0;
    break;
  case MLVFM_START:
    ret = copy_from_user(&start, user, sizeof(start)) ? -EFAULT :
      start_session(s, &start);
    break;
  case MLVFM_SUBMIT:
    ret = copy_from_user(&frame, user, sizeof(frame)) ? -EFAULT :
      submit_frame(s, &frame);
    break;
  case MLVFM_REAP:
    spin_lock_irqsave(&s->lock, flags);
    for (i = 0; i < MLVFM_SLOTS; ++i)
      if (s->queue.state[i] == MLVFM_DONE)
        break;
    spin_unlock_irqrestore(&s->lock, flags);
    if (i == MLVFM_SLOTS) {
      ret = -EAGAIN;
      break;
    }
    done.cookie = i;
    done.dropped = s->slots[i].dropped;
    /* Preserve the completion if userspace supplied an invalid pointer. */
    if (copy_to_user(user, &done, sizeof(done))) {
      ret = -EFAULT;
      break;
    }
    release_slot(&s->slots[i]);
    spin_lock_irqsave(&s->lock, flags);
    s->queue.state[i] = MLVFM_FREE;
    spin_unlock_irqrestore(&s->lock, flags);
    break;
  case MLVFM_STOP:
    stop_session(s);
    break;
  default:
    ret = -ENOTTY;
  }
  mutex_unlock(&s->ioctl_lock);
  return ret;
}
#ifdef CONFIG_COMPAT
static long bridge_compat_ioctl(struct file *f, unsigned int cmd, unsigned long arg)
{
  return bridge_ioctl(f, cmd, (unsigned long)compat_ptr(arg));
}
#endif
static unsigned int bridge_poll(struct file *file, poll_table *wait)
{
  struct session *s = file->private_data;
  unsigned long flags;
  unsigned int i, mask = 0;
  poll_wait(file, &s->wait, wait);
  spin_lock_irqsave(&s->lock, flags);
  for (i = 0; i < MLVFM_SLOTS; ++i)
    if (s->queue.state[i] == MLVFM_DONE)
      mask |= POLLIN | POLLRDNORM;
  spin_unlock_irqrestore(&s->lock, flags);
  return mask;
}
static int bridge_open(struct inode *inode, struct file *file)
{
  struct session *s;
  if (atomic_cmpxchg(&opened, 0, 1))
    return -EBUSY;
  s = kzalloc(sizeof(*s), GFP_KERNEL);
  if (!s) {
    atomic_set(&opened, 0);
    return -ENOMEM;
  }
  mutex_init(&s->ioctl_lock);
  spin_lock_init(&s->lock);
  init_waitqueue_head(&s->wait);
  file->private_data = s;
  return nonseekable_open(inode, file);
}
static int bridge_release(struct inode *inode, struct file *file)
{
  struct session *s = file->private_data;
  stop_session(s);
  kfree(s);
  atomic_set(&opened, 0);
  return 0;
}
static const struct file_operations bridge_ops = {
  .owner = THIS_MODULE, .open = bridge_open, .release = bridge_release,
  .unlocked_ioctl = bridge_ioctl, .poll = bridge_poll, .llseek = no_llseek,
#ifdef CONFIG_COMPAT
  .compat_ioctl = bridge_compat_ioctl,
#endif
};
static struct miscdevice bridge = {
  .minor = MISC_DYNAMIC_MINOR, .name = "moonlight-vfm", .fops = &bridge_ops,
  .mode = 0600,
};
static int __init bridge_init(void)
{
  int ret = misc_register(&bridge);
  if (ret)
    return ret;
  bridge.this_device->dma_mask = &bridge_dma_mask;
  bridge.this_device->coherent_dma_mask = bridge_dma_mask;
  pr_info("moonlight-vfm: experimental CE21 NV12 bridge ABI %u\n", MLVFM_ABI);
  return 0;
}
static void __exit bridge_exit(void)
{
  misc_deregister(&bridge);
}
module_init(bridge_init);
module_exit(bridge_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Experimental Moonlight CE21 NG DMA-BUF frame provider");
