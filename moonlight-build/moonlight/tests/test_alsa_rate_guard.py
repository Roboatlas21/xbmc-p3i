"""Guard the actual ALSA setup path against sample-rate negotiation drift."""
from pathlib import Path
import unittest

CODE = (Path(__file__).resolve().parents[1] /
        'src/audio/alsa.c').read_text()


class AlsaSampleRateGuardTests(unittest.TestCase):
    def test_mismatch_fails_before_pcm_hw_params_are_applied(self):
        setup = CODE.split('static int alsa_renderer_init(', 1)[1]
        negotiate = setup.index('snd_pcm_hw_params_set_rate_near(')
        verify = setup.index('moonlight_alsa_rate_matches(')
        apply = setup.index('snd_pcm_hw_params(handle, hw_params)')
        self.assertLess(negotiate, verify)
        self.assertLess(verify, apply)
        guard = setup[verify:apply]
        self.assertIn('sampleRate', guard)
        self.assertIn('opusConfig->sampleRate', guard)
        self.assertIn('goto error;', guard)
        self.assertIn('rc = -EINVAL;', guard)

    def test_error_exit_releases_partial_decoder_and_pcm(self):
        setup = CODE.split('static int alsa_renderer_init(', 1)[1]
        cleanup = setup.split('error:', 1)[1].split('static void alsa_renderer_cleanup', 1)[0]
        for expected in ('snd_pcm_hw_params_free(hw_params)',
                         'snd_pcm_close(handle)',
                         'opus_multistream_decoder_destroy(decoder)',
                         'free(pcmBuffer)'):
            self.assertIn(expected, cleanup)


if __name__ == '__main__':
    unittest.main()
