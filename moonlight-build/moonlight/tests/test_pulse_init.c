/* Fault-injection harness: compile and execute the real PulseAudio renderer. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../src/audio/pulse.c"

static int opus_fail, map_fail, pa_fail, decode_calls, opus_frees, pa_frees;
static OpusMSDecoder opus_stub;
static pa_simple pa_stub;

OpusMSDecoder *opus_multistream_decoder_create(int rate, int ch, int streams,
        int coupled, const unsigned char *map, int *error) {
  *error = opus_fail ? -7 : OPUS_OK;
  return opus_fail ? NULL : &opus_stub;
}
void opus_multistream_decoder_destroy(OpusMSDecoder *d) {
  assert(d == &opus_stub);
  ++opus_frees;
}
int opus_multistream_decode(OpusMSDecoder *d, const void *data, int length,
        short *buffer, int samples, int fec) {
  assert(d == &opus_stub);
  ++decode_calls;
  return 0;
}
pa_simple *pa_simple_new(const char *server, const char *name, int direction,
        const char *device, const char *stream, const pa_sample_spec *spec,
        const pa_channel_map *map, const void *properties, int *error) {
  *error = pa_fail ? -99 : 0;
  return pa_fail ? NULL : &pa_stub;
}
void pa_simple_free(pa_simple *p) {
  assert(p == &pa_stub);
  ++pa_frees;
}
pa_channel_map *pa_channel_map_init_auto(pa_channel_map *map, int channels,
                                         int definition) {
  if (map_fail) return NULL;
  map->channels = channels;
  return map;
}
int pa_simple_write(pa_simple *p, const void *buffer, size_t length, int *error) {
  return 0;
}
const char *pa_strerror(int code) { return "mock error"; }

int main(void) {
  OPUS_MULTISTREAM_CONFIGURATION config = {
    .sampleRate=48000, .channelCount=2, .streams=1, .coupledStreams=1,
    .samplesPerFrame=240, .mapping={0,1}
  };
  assert(audio_pulse_init(NULL));
  assert(dev == NULL); /* A successful probe must not leave a freed handle. */
  opus_fail=1;
  assert(audio_callbacks_pulse.init(0,&config,NULL,0) == -1);
  assert(decoder == NULL && pcmBuffer == NULL && dev == NULL && decode_calls == 0);
  opus_fail=0;
  map_fail=1;
  assert(audio_callbacks_pulse.init(0,&config,NULL,0) == -1);
  assert(decoder == NULL && pcmBuffer == NULL && dev == NULL && opus_frees == 1);
  map_fail=0;
  pa_fail=1;
  assert(audio_callbacks_pulse.init(0,&config,NULL,0) == -1);
  assert(decoder == NULL && pcmBuffer == NULL && dev == NULL && opus_frees == 2);
  pa_fail=0;
  assert(audio_callbacks_pulse.init(0,&config,NULL,0) == 0);
  audio_callbacks_pulse.decodeAndPlaySample("",0);
  audio_callbacks_pulse.cleanup();
  assert(decoder == NULL && pcmBuffer == NULL && dev == NULL && opus_frees == 3);
  assert(pa_frees >= 2);
  puts("PASS: Pulse probe, decoder, channel map, device and teardown failure paths");
  return 0;
}
