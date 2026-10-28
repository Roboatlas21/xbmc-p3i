/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "stats.h"
#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>

void osd_tracker_reset(OsdTracker *tracker) {
  memset(tracker, 0, sizeof(*tracker));
}

void osd_tracker_derive(OsdTracker *tracker, uint64_t now_ms,
                        const AmlFrameStats *native, OsdSnapshot *out) {
  out->frame_submits = native->submitted;
  out->v4l2_dequeues = native->dequeued;
  out->v4l2_enabled = native->uses_v4l2 != 0;
  out->decoder_failures = native->submit_failures;
  if (tracker->initialized) {
    const uint32_t duration_samples =
        native->codec_submit_samples - tracker->prior.codec_submit_samples;
    if (duration_samples) {
      const uint32_t sum_us = native->codec_submit_us - tracker->prior.codec_submit_us;
      out->submit_mean_ms = (float)((double)sum_us / duration_samples / 1000.0);
      out->has_submit_mean = true;
    }
    out->idr_interval = native->idr_frames - tracker->prior.idr_frames;
    out->busy_retries = native->codec_eagain - tracker->prior.codec_eagain;
    out->backpressure_waits =
        native->backpressure_waits - tracker->prior.backpressure_waits;
    out->backpressure_drops =
        native->backpressure_drops - tracker->prior.backpressure_drops;
    if (out->backpressure_waits) {
      const uint32_t wait_us =
          native->backpressure_wait_us - tracker->prior.backpressure_wait_us;
      out->backpressure_wait_mean_ms =
          (float)((double)wait_us / out->backpressure_waits / 1000.0);
      out->has_backpressure_wait = true;
    }
  }
  if (tracker->initialized && now_ms > tracker->last_ms &&
      now_ms - tracker->last_ms >= 100 &&
      now_ms - tracker->last_ms <= 60000) {
    const double sec = (now_ms - tracker->last_ms) / 1000.0;
    out->submit_fps = (float)((uint32_t)(native->submitted - tracker->prior.submitted) / sec);
    out->has_submit_fps = true;
    if (out->v4l2_enabled) {
      out->dequeue_fps = (float)((uint32_t)(native->dequeued - tracker->prior.dequeued) / sec);
      out->has_dequeue_fps = true;
    }
    out->dequeue_gaps_25ms = native->dequeue_gaps_25ms - tracker->prior.dequeue_gaps_25ms;
    out->dequeue_gaps_33ms = native->dequeue_gaps_33ms - tracker->prior.dequeue_gaps_33ms;
  }
  /* Use interval deltas to avoid a lifetime average masking recent stalls.
   * Unsigned differences also tolerate u32 counter wrap within each interval. */
  const uint32_t host_samples = native->host_samples - tracker->prior.host_samples;
  const uint32_t assembly_samples = native->assembly_samples - tracker->prior.assembly_samples;
  if (host_samples) {
    const uint32_t tenths = native->host_tenths_ms - tracker->prior.host_tenths_ms;
    out->host_ms = ((float)tenths / host_samples) / 10.0f;
    out->has_host_ms = true;
  }
  if (assembly_samples) {
    const uint32_t micros = native->assembly_us - tracker->prior.assembly_us;
    out->assembly_ms = ((float)micros / assembly_samples) / 1000.0f;
    out->has_assembly_ms = true;
  }
  tracker->prior = *native;
  tracker->last_ms = now_ms;
  tracker->initialized = true;
}

/* Parse one nonnegative decimal sysfs module parameter. Refuse invalid or
 * wrapped negative values rather than showing fabricated output rates. */
/* /proc/stat aggregate CPU usage, never a Moonlight process percentage. */
bool osd_parse_cpu_ticks(const char *line, uint64_t *busy, uint64_t *total) {
  if (!line || !busy || !total || strncmp(line, "cpu", 3) ||
      !isspace((unsigned char)line[3])) return false;
  unsigned long long u,n,s,i,io,irq,soft,steal;
  if (sscanf(line + 3, "%llu %llu %llu %llu %llu %llu %llu %llu",
             &u,&n,&s,&i,&io,&irq,&soft,&steal) != 8) return false;
  *busy=(uint64_t)u+n+s+irq+soft+steal;
  *total=*busy+i+io;
  return *total >= *busy;
}

void osd_tracker_cpu_reset(OsdTracker *tracker) {
  if (tracker) tracker->cpu_initialized=false;
}

void osd_tracker_cpu_update(OsdTracker *tracker, uint64_t busy,
                            uint64_t total, OsdSnapshot *out) {
  if (!tracker || !out || total < busy) return;
  if (tracker->cpu_initialized && total > tracker->prior_cpu_total &&
      busy >= tracker->prior_cpu_busy) {
    const uint64_t delta=total-tracker->prior_cpu_total;
    const uint64_t work=busy-tracker->prior_cpu_busy;
    if (work <= delta) {
      out->sys_cpu_percent=(float)(100.0*(double)work/(double)delta);
      out->has_sys_cpu=true;
    }
  }
  tracker->prior_cpu_busy=busy;
  tracker->prior_cpu_total=total;
  tracker->cpu_initialized=true;
}

/* Linux thermal_zone0/temp uses milli-Celsius; reject unknown units. */
bool osd_parse_thermal_millidegrees(const char *text, float *celsius) {
  uint32_t value=0;
  if (!celsius || !osd_parse_sysfs_counter(text,&value) ||
      value < 1000 || value > 150000) return false;
  *celsius=(float)value/1000.0f;
  return true;
}

bool osd_parse_sysfs_counter(const char *text, uint32_t *out) {
  if (!text || !out) return false;
  const char *p = text;
  while (isspace((unsigned char)*p)) p++;
  if (!isdigit((unsigned char)*p)) return false;
  errno = 0;
  char *end = NULL;
  const unsigned long long value = strtoull(p, &end, 10);
  if (errno || value > UINT32_MAX) return false;
  while (isspace((unsigned char)*end)) end++;
  if (*end != '\0') return false;
  *out = (uint32_t)value;
  return true;
}

void osd_tracker_sink_reset(OsdTracker *tracker) {
  tracker->sink_initialized = false;
}

void osd_tracker_sink_update(OsdTracker *tracker, uint64_t now_ms,
                             uint32_t count, OsdSnapshot *out) {
  if (!tracker || !out) return;
  if (tracker->sink_initialized && now_ms > tracker->sink_last_ms &&
      now_ms - tracker->sink_last_ms >= 100 &&
      now_ms - tracker->sink_last_ms <= 60000 &&
      count >= tracker->prior_sink_count) {
    const double sec = (now_ms - tracker->sink_last_ms) / 1000.0;
    const uint32_t delta = count - tracker->prior_sink_count;
    if ((double)delta / sec <= 1000.0) {
      out->sink_fps = (float)(delta / sec);
      out->has_sink_fps = true;
    }
  }
  tracker->prior_sink_count = count;
  tracker->sink_last_ms = now_ms;
  tracker->sink_initialized = true;
}

/* Read-only RTP counters from the upstream moonlight-common-c interface.
 * These are packet classifications, not a measured stream bitrate, measured
 * network loss percentage, or confirmed on-screen frame drops. */
void osd_tracker_network_update(OsdTracker *t, uint64_t now_ms,
                                const OsdNetworkCounters *n,
                                OsdSnapshot *out) {
  if (!t || !n || !out) return;
  if (t->net_initialized && now_ms > t->net_last_ms &&
      now_ms - t->net_last_ms >= 100 &&
      now_ms - t->net_last_ms <= 60000 &&
      n->video >= t->net_prior_video && n->fec >= t->net_prior_fec &&
      n->recovered >= t->net_prior_recovered &&
      n->failed >= t->net_prior_failed && n->oos >= t->net_prior_oos &&
      n->invalid >= t->net_prior_invalid) {
    const double sec = (now_ms - t->net_last_ms) / 1000.0;
    const uint32_t vid = n->video - t->net_prior_video;
    const uint32_t fec = n->fec - t->net_prior_fec;
    if (vid / sec <= 100000.0 && fec / sec <= 100000.0) {
      out->video_pps = (float)(vid / sec);
      out->fec_pps = (float)(fec / sec);
      out->fec_recovered_delta = n->recovered - t->net_prior_recovered;
      out->fec_failed_delta = n->failed - t->net_prior_failed;
      out->oos_delta = n->oos - t->net_prior_oos;
      out->invalid_delta = n->invalid - t->net_prior_invalid;
      out->has_network_rate = true;
      out->has_network_recovery = n->recovery_valid && t->net_recovery_valid;
    }
  }
  t->net_prior_video = n->video;
  t->net_prior_fec = n->fec;
  t->net_prior_recovered = n->recovered;
  t->net_prior_failed = n->failed;
  t->net_prior_oos = n->oos;
  t->net_prior_invalid = n->invalid;
  t->net_last_ms = now_ms;
  t->net_initialized = true;
  t->net_recovery_valid = n->recovery_valid;
}

bool osd_parse_stat_value(const char *source, const char *key, uint32_t *out) {
  if (!source || !key || !*key || !out) return false;
  const size_t size = strlen(key);
  const char *line = source;
  while (*line) {
    const char *end = strchr(line, '\n');
    if (!end) end = line + strlen(line);
    const char *start = line;
    while (start < end && isspace((unsigned char)*start)) start++;
    if ((size_t)(end - start) > size && strncmp(start, key, size) == 0 &&
        (start[size] == '=' || start[size] == ':' || isspace((unsigned char)start[size]))) {
      const char *value = start + size;
      while (value < end && (isspace((unsigned char)*value) || *value == ':' || *value == '=')) value++;
      if (value == end || !isdigit((unsigned char)*value)) return false;
      char *after = NULL;
      const unsigned long n = strtoul(value, &after, 10);
      if (after == value || n > UINT_MAX || (after < end && !isspace((unsigned char)*after))) return false;
      *out = (uint32_t)n;
      return true;
    }
    line = *end ? end + 1 : end;
  }
  return false;
}

/* Values are labeled for the actual source and time window. Formats stay
 * separated for text logging; graphical rendering wraps at word boundaries. */
void osd_format(const OsdSnapshot *s, char *first, size_t fn,
                char *second, size_t sn) {
  if (!s) return;
  char dec[24] = "n/a", sink[24] = "n/a", submit[24] = "n/a";
  char dq[24] = "n/a", decdrop[24] = "n/a", ferr[24] = "n/a";
  char herr[24] = "n/a", kqueue[24] = "n/a", kpool[24] = "n/a";
  char waitq[24] = "n/a";
  if (s->has_decoder_fps) snprintf(dec,sizeof(dec),"%.1f/s",s->decoder_fps);
  if (s->has_sink_fps) snprintf(sink,sizeof(sink),"%.1f/s",s->sink_fps);
  if (s->has_submit_fps) snprintf(submit,sizeof(submit),"%.1f/s",s->submit_fps);
  if (!s->v4l2_enabled) strcpy(dq, "n/a (direct)");
  else if (s->has_dequeue_fps) snprintf(dq,sizeof(dq),"%.1f/s",s->dequeue_fps);
  if (s->has_decoder_deltas) {
    snprintf(decdrop,sizeof(decdrop),"+%u",s->decoder_drops);
    snprintf(ferr,sizeof(ferr),"+%u",s->decoder_frame_errors);
    snprintf(herr,sizeof(herr),"+%u",s->decoder_hw_errors);
  }
  if (s->has_kernel_queue) snprintf(kqueue,sizeof(kqueue),"%u",s->kernel_queue);
  if (s->has_kernel_pool) snprintf(kpool,sizeof(kpool),"%u",s->kernel_pool);
  if (s->has_pending_frames) snprintf(waitq,sizeof(waitq),"%u",s->pending_frames);
  if (first && fn) {
    snprintf(first, fn,
             "Dec %s | Sink %s | Sub %s | DQ %s | DecDrop %s | FrameErr %s | HwErr %s | "
             "VQ %s | KernelQ %s | Pool %s",
             dec, sink, submit, dq, decdrop, ferr, herr, waitq, kqueue, kpool);
  }

  char host[24]="n/a", assembly[24]="n/a", subms[24]="n/a", bpwait[24]="n/a";
  char rtt[24]="n/a", video[24]="n/a", fec[24]="n/a";
  char recovered[24]="n/a", failed[24]="n/a", oos[24]="n/a";
  char invalid[24]="n/a", cpu[24]="n/a", thermal[24]="n/a";
  if (s->has_host_ms) snprintf(host,sizeof(host),"%.2fms",s->host_ms);
  if (s->has_assembly_ms) snprintf(assembly,sizeof(assembly),"%.2fms",s->assembly_ms);
  if (s->has_submit_mean) snprintf(subms,sizeof(subms),"%.2fms",s->submit_mean_ms);
  if (s->has_backpressure_wait)
    snprintf(bpwait,sizeof(bpwait),"%.2fms",s->backpressure_wait_mean_ms);
  if (s->has_rtt) snprintf(rtt,sizeof(rtt),"%ums",s->rtt_ms);
  if (s->has_network_rate) {
    snprintf(video,sizeof(video),"%.0f/s",s->video_pps);
    snprintf(fec,sizeof(fec),"%.0f/s",s->fec_pps);
  }
  if (s->has_network_recovery) {
    snprintf(recovered,sizeof(recovered),"+%u",s->fec_recovered_delta);
    snprintf(failed,sizeof(failed),"+%u",s->fec_failed_delta);
    snprintf(oos,sizeof(oos),"+%u",s->oos_delta);
    snprintf(invalid,sizeof(invalid),"+%u",s->invalid_delta);
  }
  if (s->has_sys_cpu) snprintf(cpu,sizeof(cpu),"%.0f%%",s->sys_cpu_percent);
  if (s->has_thermal0) snprintf(thermal,sizeof(thermal),"%.1fC",s->thermal0_c);
  if (second && sn) {
    snprintf(second, sn,
             "Host %s | RTT %s | Assembly %s | Submit %s | "
             "IDR +%u | CodecWriteFail %u | EAGAIN +%u | "
             "BPWait +%u/%s | BPDrop +%u | "
             "VideoPkt %s | FECpkt %s | FECrec %s | FECfail %s | "
             "OOS %s | Invalid %s | SysCPU %s | Th0 %s",
             host,rtt,assembly,subms,s->idr_interval,s->decoder_failures,
             s->busy_retries,s->backpressure_waits,bpwait,s->backpressure_drops,
             video,fec,recovered,failed,oos,invalid,cpu,thermal);
  }
}
