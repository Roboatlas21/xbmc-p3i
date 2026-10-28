"""Inject allocation/ALSA errors into the actual initializer without audio hardware.

Only the external APIs are simulated. The initializer and its error path are
extracted unchanged from alsa.c; each acquired resource must be released even
when common-c returns immediately on init failure without calling cleanup.
"""
import os
from pathlib import Path
import re
import shlex
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class AlsaInitializationTests(unittest.TestCase):
    def test_every_initialization_failure_releases_partial_state(self):
        source = (ROOT / 'src/audio/alsa.c').read_text()
        start = source.index('static int alsa_renderer_init(')
        end = source.index('\nstatic void alsa_renderer_cleanup', start)
        initializer = source[start:end]
        stats = re.search(r'typedef struct \{.*?\} AlsaRuntimeStats;', source, re.S)[0]
        check = next(line for line in source.splitlines() if line.startswith('#define CHECK_RETURN'))
        harness = r'''
#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "alsa_resync.h"
typedef int OpusMSDecoder, snd_pcm_t, snd_pcm_hw_params_t, snd_pcm_sw_params_t;
typedef unsigned long snd_pcm_uframes_t;
typedef struct { int sampleRate, channelCount, streams, coupledStreams,
    samplesPerFrame; unsigned char mapping[8]; } Config;
typedef Config *POPUS_MULTISTREAM_CONFIGURATION;
#define AUDIO_CONFIGURATION_MAX_CHANNEL_COUNT 8
#define OPUS_OK 0
static snd_pcm_t *handle;
static OpusMSDecoder *decoder;
static short *pcmBuffer;
static int samplesPerFrame;
static bool alsaResyncEnabled;
static AlsaResyncConfig alsaResyncConfig;
static AlsaResyncStats alsaResyncStats;
static int fail_at, calls, live, optional_failed;
static int step(void) { return ++calls == fail_at ? -12 : 0; }
static int optional_step(void) {
    int rc = step(); if (rc) optional_failed = 1; return rc;
}
static void *acquire(size_t size) {
    if (step()) return NULL;
    void *p = calloc(1, size); assert(p); ++live; return p;
}
static void release(void *p) { if (p) { assert(live > 0); --live; free(p); } }
static int allocate(void **p) { *p = acquire(sizeof(int)); return *p ? 0 : -12; }
static OpusMSDecoder *create(int *rc) {
    OpusMSDecoder *p = acquire(sizeof(int)); *rc = p ? 0 : -7; return p;
}
#define malloc acquire
#define free release
#define opus_multistream_decoder_create(a,b,c,d,e,rc) create(rc)
#define opus_multistream_decoder_destroy release
#define snd_pcm_open(p,...) allocate((void **)(p))
#define snd_pcm_close(p) (release(p), 0)
#define snd_pcm_hw_params_malloc(p) allocate((void **)(p))
#define snd_pcm_sw_params_malloc(p) allocate((void **)(p))
#define snd_pcm_hw_params_free release
#define snd_pcm_sw_params_free release
#define snd_pcm_hw_params_any(...) step()
#define snd_pcm_hw_params_set_access(...) step()
#define snd_pcm_hw_params_set_format(...) step()
#define snd_pcm_hw_params_set_rate_near(...) step()
#define snd_pcm_hw_params_set_channels(...) step()
#define snd_pcm_hw_params_set_period_size_near(...) step()
#define snd_pcm_hw_params_set_buffer_size_near(...) step()
#define snd_pcm_hw_params(...) step()
#define snd_pcm_hw_params_get_buffer_size(...) optional_step()
#define snd_pcm_hw_params_get_period_size(...) optional_step()
#define snd_pcm_sw_params_current(...) step()
#define snd_pcm_sw_params_set_avail_min(...) step()
#define snd_pcm_sw_params_set_start_threshold(...) step()
#define snd_pcm_sw_params(...) step()
#define snd_pcm_sw_params_get_start_threshold(...) optional_step()
#define snd_pcm_sw_params_get_avail_min(...) optional_step()
#define snd_pcm_prepare(...) step()
'''
        harness += '\n' + stats + '\nstatic AlsaRuntimeStats alsaStats;\n' + check + '\n' + initializer
        harness += r'''
static void successful_cleanup(void) {
    release(handle); handle = NULL;
    release(decoder); decoder = NULL;
    release(pcmBuffer); pcmBuffer = NULL;
}
int main(void) {
    Config config = {48000, 2, 1, 1, 240, {0, 1}};
    unsetenv("MOONLIGHT_ALSA_RESYNC");
    assert(alsa_renderer_init(0, &config, NULL, 0) == 0);
    int total = calls;
    assert(live == 3);
    successful_cleanup();
    for (int n = 1; n <= total; ++n) {
        calls = optional_failed = 0; fail_at = n;
        int result = alsa_renderer_init(0, &config, NULL, 0);
        assert(result < 0 || optional_failed);
        if (result == 0) successful_cleanup(); /* Optional geometry reads. */
        assert(!handle && !decoder && !pcmBuffer && live == 0);
        calls = fail_at = 0;
        assert(alsa_renderer_init(0, &config, NULL, 0) == 0);
        successful_cleanup(); /* Retrying after a failure must also work. */
        assert(live == 0);
    }
    printf("ALSA init: %d injected failure points and retries passed\n", total);
}
'''
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp)
            (path / 'test.c').write_text(harness)
            command = shlex.split(os.environ.get('CC', 'cc'))
            command += ['-std=c99', '-Wall', '-Wextra', '-Werror',
                        '-Wno-unused-parameter', '-Wno-unused-but-set-variable',
                        '-Wno-unused-value', '-I' + str(ROOT / 'src/audio'),
                        str(path / 'test.c'), '-o', str(path / 'test')]
            subprocess.run(command, check=True)
            subprocess.run([str(path / 'test')], check=True, capture_output=True)


if __name__ == '__main__':
    unittest.main()
