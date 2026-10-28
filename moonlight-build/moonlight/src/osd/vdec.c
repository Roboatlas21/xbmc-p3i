/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Read-only, codec-specific decoder statistics from CoreELEC NG's
 * /sys/class/vdec/vdec_status; never change decoder kernel settings. */
#include "stats.h"
#include <ctype.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

/* Pannal's media_modules-aml vdec.c prints a stanza per connected decoder.
 * Fail closed on incomplete/ambiguous data rather than attributing unrelated
 * Kodi or another channel's counters to Moonlight. */
static bool decoder_match(const char *block, uint32_t codec_family) {
  const char *key = strstr(block, "device name");
  if (!key) return false;
  const char *sep = strchr(key, ':');
  if (!sep) return false;
  ++sep;
  while (*sep && isspace((unsigned char)*sep)) ++sep;
  const char *end = strchr(sep, '\n');
  if (!end) end = sep + strlen(sep);
  char name[80] = {0};
  const size_t length = (size_t)(end - sep);
  if (!length || length >= sizeof(name)) return false;
  memcpy(name, sep, length);
  /* Accept both multi-instance and legacy decoder names. */
  if (strncmp(name, "amvdec_", 7) && strncmp(name, "ammvdec_", 8))
    return false;
  if (codec_family == 1) return strstr(name, "h264") != NULL;
  if (codec_family == 2) return strstr(name, "h265") != NULL ||
                                strstr(name, "hevc") != NULL;
  return false;
}

bool osd_vdec_parse(const char *text, uint32_t expected_width,
                    uint32_t expected_height, uint32_t expected_codec_family,
                    OsdVdecCounters *out) {
  if (!text || !out || !expected_width || !expected_height ||
      (expected_codec_family != 1 && expected_codec_family != 2)) return false;
  unsigned matches = 0;
  OsdVdecCounters selected = {0};
  const char *cursor = text;
  while ((cursor = strstr(cursor, "vdec channel ")) != NULL) {
    if (cursor != text && cursor[-1] != '\n') {cursor++; continue;}
    unsigned channel = 0;
    if (sscanf(cursor, "vdec channel %u statistics:", &channel) != 1) {
      cursor++;
      continue;
    }
    const char *next = strstr(cursor + 1, "\nvdec channel ");
    const char *end = next ? next + 1 : text + strlen(text);
    const size_t length = (size_t)(end - cursor);
    if (length < 40 || length >= 4096) { cursor = end; continue; }
    char block[4096];
    memcpy(block, cursor, length);
    block[length] = '\0';
    OsdVdecCounters item = {.channel=channel,.codec_family=expected_codec_family};
    if (decoder_match(block, expected_codec_family) &&
        osd_parse_stat_value(block, "frame width", &item.width) &&
        osd_parse_stat_value(block, "frame height", &item.height) &&
        item.width == expected_width && item.height == expected_height &&
        osd_parse_stat_value(block, "frame count", &item.frame_count) &&
        osd_parse_stat_value(block, "drop count", &item.drop_count) &&
        osd_parse_stat_value(block, "fra err count", &item.frame_errors) &&
        osd_parse_stat_value(block, "hw err count", &item.hw_errors)) {
      selected = item;
      if (++matches > 1) return false; /* Multiple matching decoder channels */
    }
    cursor = end;
  }
  if (matches != 1) return false;
  *out = selected;
  return true;
}

void osd_tracker_decoder_reset(OsdTracker *tracker) {
  if (tracker) tracker->decoder_initialized = false;
}

void osd_tracker_decoder_update(OsdTracker *tracker, uint64_t now_ms,
                                const OsdVdecCounters *s,
                                OsdSnapshot *out) {
  if (!tracker || !s || !out) return;
  const bool same = tracker->decoder_initialized &&
                    tracker->decoder_channel == s->channel &&
                    tracker->decoder_width == s->width &&
                    tracker->decoder_height == s->height &&
                    tracker->decoder_codec_family == s->codec_family;
  if (same && now_ms > tracker->decoder_last_ms &&
      now_ms - tracker->decoder_last_ms >= 100 &&
      now_ms - tracker->decoder_last_ms <= 60000 &&
      s->frame_count >= tracker->prior_dec_frames &&
      s->drop_count >= tracker->prior_dec_drops &&
      s->frame_errors >= tracker->prior_dec_frame_err &&
      s->hw_errors >= tracker->prior_dec_hw_err) {
    const double seconds = (now_ms - tracker->decoder_last_ms) / 1000.0;
    const uint32_t frames = s->frame_count - tracker->prior_dec_frames;
    const double fps = frames / seconds;
    if (fps <= 240.0) {
      out->decoder_fps = (float)fps;
      out->has_decoder_fps = true;
      out->decoder_drops = s->drop_count - tracker->prior_dec_drops;
      out->decoder_frame_errors = s->frame_errors - tracker->prior_dec_frame_err;
      out->decoder_hw_errors = s->hw_errors - tracker->prior_dec_hw_err;
      out->has_decoder_deltas = true;
    }
  }
  tracker->decoder_channel = s->channel;
  tracker->decoder_width = s->width;
  tracker->decoder_height = s->height;
  tracker->decoder_codec_family = s->codec_family;
  tracker->prior_dec_frames = s->frame_count;
  tracker->prior_dec_drops = s->drop_count;
  tracker->prior_dec_frame_err = s->frame_errors;
  tracker->prior_dec_hw_err = s->hw_errors;
  tracker->decoder_last_ms = now_ms;
  tracker->decoder_initialized = true;
}
