"""Compile the real native argument parser against minimal host-only stubs."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]

STUBS = {
    'Limelight.h': '''#pragma once
#include <string.h>
typedef struct { int width,height,fps,bitrate,packetSize,streamingRemotely,audioConfiguration,supportedVideoFormats,encryptionFlags; } STREAM_CONFIGURATION;
static inline void LiInitializeStreamConfiguration(STREAM_CONFIGURATION *c) { memset(c,0,sizeof(*c)); }
#define STREAM_CFG_AUTO 0
#define STREAM_CFG_REMOTE 1
#define STREAM_CFG_LOCAL 2
#define AUDIO_CONFIGURATION_STEREO 2
#define AUDIO_CONFIGURATION_51_SURROUND 6
#define AUDIO_CONFIGURATION_71_SURROUND 8
#define SCM_H264 1
#define ENCFLG_ALL 7
#define ENCFLG_NONE 0
#define ENCFLG_AUDIO 1
''',
    'platform.h': '''#pragma once
enum codecs { CODEC_UNSPECIFIED, CODEC_H264, CODEC_HEVC, CODEC_AV1 };
''',
    'cpu.h': '''#pragma once
static inline int has_fast_aes(void) { return 0; }
static inline int has_slow_aes(void) { return 0; }
''',
    'util.h': '#pragma once\n',
    'main.c': '''#include "platform.h"
#include "config.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
int main(int argc, char **argv) {
 CONFIGURATION config;
 config_parse(argc, argv, &config);
 printf("action=%s host=%s\\n", config.action ? config.action : "", config.address ? config.address : "");
 if (getenv("MOONLIGHT_TEST_MAPPING"))
   printf("mapping=%zu:%s\\n", strlen(config.mapping), config.mapping);
 return 0;
}
''',
}


class NativeQuitParserTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        if not shutil.which('cc'):
            raise unittest.SkipTest('C compiler not available')
        cls.work = tempfile.TemporaryDirectory()
        root = Path(cls.work.name)
        for name, content in STUBS.items():
            (root / name).write_text(content)
        (root / 'input').mkdir()
        (root / 'audio').mkdir()
        (root / 'input/evdev.h').write_text('')
        (root / 'audio/audio.h').write_text('')
        for name in ('config.c', 'config.h'):
            shutil.copyfile(ROOT / 'src' / name, root / name)
        cls.binary = root / 'test-config'
        base = ['cc', '-std=gnu99', '-D_GNU_SOURCE', '-Wno-sign-compare',
                '-Wno-unused-parameter', '-I', str(root), str(root / 'config.c'),
                str(root / 'main.c')]
        subprocess.run(base + ['-o', str(cls.binary)], check=True,
                       capture_output=True)
        cls.sanitized = root / 'test-config-asan'
        sanitizer = subprocess.run(base + ['-g', '-fsanitize=address,undefined',
                                           '-o', str(cls.sanitized)],
                                  capture_output=True, text=True)
        if sanitizer.returncode:
            cls.sanitized = None

    @classmethod
    def tearDownClass(cls):
        if hasattr(cls, 'work'):
            cls.work.cleanup()

    def invoke(self, saved, args):
        with tempfile.TemporaryDirectory() as folder:
            if saved is not None:
                (Path(folder) / 'moonlight.conf').write_text('address = ' + saved + '\n')
            return subprocess.run([str(self.binary), *args], cwd=folder,
                                  env=os.environ.copy(), capture_output=True, text=True)

    def test_explicit_quit_host_overrides_saved_default(self):
        for saved in (None, 'host-A', 'host-B'):
            with self.subTest(saved=saved):
                result = self.invoke(saved, ['quit', 'host-A'])
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertEqual(result.stdout.strip(), 'action=quit host=host-A')

    def test_genuine_extra_host_is_rejected(self):
        result = self.invoke('host-B', ['quit', 'host-A', 'host-C'])
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('Too many positional arguments', result.stderr)

    def test_implicit_host_remains_the_configured_default(self):
        result = self.invoke('host-B', ['quit'])
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stdout.strip(), 'action=quit host=host-B')

    def test_accessible_mapping_path_is_not_freed(self):
        if self.sanitized is None:
            self.skipTest('Host sanitizers unavailable')
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder) / 'controller-mapping.txt'
            path.write_text('sample controller mapping')
            (Path(folder) / 'moonlight.conf').write_text(
                'address = host-A\\nmapping = controller-mapping.txt\\n'.replace('\\n', '\n'))
            env = dict(os.environ, MOONLIGHT_TEST_MAPPING='1',
                       ASAN_OPTIONS='detect_leaks=0')
            result = subprocess.run([str(self.sanitized), 'quit'],
                                    cwd=folder, env=env, capture_output=True,
                                    text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertIn('mapping=22:controller-mapping.txt', result.stdout)

    def test_explicit_trust_reset_retains_exact_host(self):
        result = self.invoke('host-B', ['forget-server-cert', 'host-A'])
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stdout.strip(),
                         'action=forget-server-cert host=host-A')

    def test_quit_target_survives_host_specific_overlay(self):
        source = (ROOT / 'src/main.c').read_text()
        self.assertIn('config.address = explicit_quit_host;', source)


if __name__ == '__main__':
    unittest.main()
