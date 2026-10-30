#pragma once
#include <stddef.h>
#define PA_SAMPLE_S16LE 3
#define PA_STREAM_PLAYBACK 1
#define PA_CHANNEL_MAP_ALSA 1
typedef struct { int placeholder; } pa_simple;
typedef struct { int format,rate,channels; } pa_sample_spec;
typedef struct { int channels; } pa_channel_map;
pa_simple *pa_simple_new(const char *,const char *,int,const char *,const char *,const pa_sample_spec *,const pa_channel_map *,const void *,int *);
void pa_simple_free(pa_simple *);
pa_channel_map *pa_channel_map_init_auto(pa_channel_map *,int,int);
int pa_simple_write(pa_simple *,const void *,size_t,int *);
