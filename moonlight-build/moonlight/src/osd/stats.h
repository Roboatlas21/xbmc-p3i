/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef MOONLIGHT_OSD_STATS_H
#define MOONLIGHT_OSD_STATS_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Counts submitted/display-dequeued events, not guaranteed panel presents. */
typedef struct {
  uint32_t submitted;
  uint32_t dequeued;
  uint32_t uses_v4l2;
  uint32_t submit_failures;
  uint32_t codec_eagain;
  uint32_t backpressure_waits;
  uint32_t backpressure_wait_us;
  uint32_t backpressure_drops;
  uint32_t idr_frames;
  uint32_t codec_submit_us;
  uint32_t codec_submit_samples;
  uint32_t dequeue_gaps_25ms;
  uint32_t dequeue_gaps_33ms;
  uint32_t host_samples;
  uint32_t host_tenths_ms;
  uint32_t assembly_samples;
  uint32_t assembly_us;
  uint32_t stream_width;
  uint32_t stream_height;
  /* 1=H.264, 2=HEVC; other decoders do not claim these statistics. */
  uint32_t codec_family;
  /* Diagnostic counters; single writers publish only lock-free u32 values. */
  uint32_t input_frames;
  uint32_t last_input_ms;
  uint32_t input_gaps_50ms;
  uint32_t input_gaps_100ms;
  uint32_t submit_slow_20ms;
  uint32_t submit_slow_50ms;
  uint32_t queue_wait_us;
  uint32_t queue_wait_samples;
  uint32_t queue_wait_20ms;
} AmlFrameStats;

typedef struct {
  uint64_t frame_submits;
  uint64_t v4l2_dequeues;
  bool v4l2_enabled;
  float submit_fps;
  bool has_submit_fps;
  float dequeue_fps;
  bool has_dequeue_fps;
  /* Kernel amvideo sink's selected-frame changes, NOT physical TV presents. */
  float sink_fps;
  bool has_sink_fps;
  uint32_t pending_frames;
  bool has_pending_frames;
  uint32_t decoder_failures; /* Lifetime failed codec_write, not decoder drops */
  uint32_t idr_interval;
  float submit_mean_ms;
  bool has_submit_mean;
  uint32_t busy_retries; /* codec_write EAGAIN events this window */
  uint32_t backpressure_waits; /* Units throttled before codec_write this window */
  float backpressure_wait_mean_ms;
  bool has_backpressure_wait;
  uint32_t backpressure_drops; /* Units dropped after bounded pre-write wait */
  uint32_t dequeue_gaps_25ms;
  uint32_t dequeue_gaps_33ms;
  uint32_t kernel_queue;
  uint32_t kernel_pool;
  bool has_kernel_queue;
  bool has_kernel_pool;
  /* vdec_status frame count is the decoder's own counter, NOT an
   * arrival, decode-submission, V4L2 dequeue, or panel-present count. */
  float decoder_fps;
  bool has_decoder_fps;
  uint32_t decoder_drops; /* Delta over one sample period */
  uint32_t decoder_frame_errors;
  uint32_t decoder_hw_errors;
  bool has_decoder_deltas;
  uint32_t rtt_ms;
  bool has_rtt;
  float host_ms;
  bool has_host_ms;
  float assembly_ms;
  bool has_assembly_ms;
  uint32_t packet_video;
  uint32_t packet_fec;
  uint32_t packet_lost; /* Legacy field: FEC recoveries FAILED, not dropped packets */
  float video_pps;
  float fec_pps;
  bool has_network_rate;
  uint32_t fec_recovered_delta;
  uint32_t fec_failed_delta;
  uint32_t oos_delta;
  uint32_t invalid_delta;
  float mbps;
  bool has_mbps;
  float sys_cpu_percent;
  bool has_sys_cpu;
  float thermal0_c;
  bool has_thermal0;
} OsdSnapshot;

typedef struct {
  AmlFrameStats prior;
  uint64_t last_ms;
  bool initialized;
  uint32_t prior_sink_count;
  uint64_t sink_last_ms;
  bool sink_initialized;
  uint64_t prior_cpu_busy, prior_cpu_total;
  bool cpu_initialized;
  uint32_t prior_dec_frames, prior_dec_drops, prior_dec_frame_err, prior_dec_hw_err;
  uint64_t decoder_last_ms;
  unsigned decoder_channel;
  uint32_t decoder_width, decoder_height, decoder_codec_family;
  bool decoder_initialized;
  uint64_t net_last_ms;
  bool net_initialized;
  uint32_t net_prior_video, net_prior_fec, net_prior_recovered;
  uint32_t net_prior_failed, net_prior_oos, net_prior_invalid;
} OsdTracker;

typedef struct {
  unsigned channel;
  uint32_t width, height;
  uint32_t codec_family;
  uint32_t frame_count, drop_count, frame_errors, hw_errors;
} OsdVdecCounters;

bool osd_vdec_parse(const char *text, uint32_t expected_width,
                    uint32_t expected_height, uint32_t expected_codec_family,
                    OsdVdecCounters *out);
typedef struct {
  uint32_t video, fec, recovered, failed, oos, invalid;
} OsdNetworkCounters;
void osd_tracker_network_update(OsdTracker *tracker, uint64_t now_ms,
                                const OsdNetworkCounters *source,
                                OsdSnapshot *out);
void osd_tracker_decoder_reset(OsdTracker *tracker);
void osd_tracker_decoder_update(OsdTracker *tracker, uint64_t now_ms,
                                const OsdVdecCounters *sample,
                                OsdSnapshot *out);
void osd_tracker_reset(OsdTracker *tracker);
void osd_tracker_derive(OsdTracker *tracker, uint64_t now_ms,
                        const AmlFrameStats *native, OsdSnapshot *out);
bool osd_parse_cpu_ticks(const char *line, uint64_t *busy, uint64_t *total);
void osd_tracker_cpu_reset(OsdTracker *tracker);
void osd_tracker_cpu_update(OsdTracker *tracker, uint64_t busy,
                            uint64_t total, OsdSnapshot *out);
bool osd_parse_thermal_millidegrees(const char *text, float *celsius);
bool osd_parse_sysfs_counter(const char *text, uint32_t *out);
void osd_tracker_sink_reset(OsdTracker *tracker);
void osd_tracker_sink_update(OsdTracker *tracker, uint64_t now_ms,
                             uint32_t count, OsdSnapshot *out);
bool osd_parse_stat_value(const char *source, const char *key, uint32_t *out);
void osd_format(const OsdSnapshot *snap, char *first, size_t first_size,
                char *second, size_t second_size);

#endif
