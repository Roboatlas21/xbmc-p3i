/* Lightweight test-only Moonlight audio API. */
#pragma once
#define AUDIO_CONFIGURATION_MAX_CHANNEL_COUNT 8
#define CAPABILITY_DIRECT_SUBMIT 1
#define CAPABILITY_SUPPORTS_ARBITRARY_AUDIO_DURATION 16
typedef struct {
  int sampleRate, channelCount, streams, coupledStreams, samplesPerFrame;
  unsigned char mapping[8];
} OPUS_MULTISTREAM_CONFIGURATION, *POPUS_MULTISTREAM_CONFIGURATION;
typedef struct {
  int (*init)(int, POPUS_MULTISTREAM_CONFIGURATION, void *, int);
  void (*cleanup)(void);
  void (*decodeAndPlaySample)(char *, int);
  int capabilities;
} AUDIO_RENDERER_CALLBACKS;
