"""Reject fake signature-only artwork and accept real PNG structure."""
import struct
import unittest
import zlib

from resources.lib.util.artwork import valid_artwork


def sample_png():
    def chunk(name, body):
        return (struct.pack('>I', len(body)) + name + body +
                struct.pack('>I', zlib.crc32(name + body) & 0xffffffff))
    return (b'\x89PNG\r\n\x1a\n' +
            chunk(b'IHDR', struct.pack('>IIBBBBB', 1, 1, 8, 2, 0, 0, 0)) +
            chunk(b'IDAT', zlib.compress(b'\x00\xaa\xbb\xcc')) +
            chunk(b'IEND', b''))


class ArtworkValidationTests(unittest.TestCase):
    def test_valid_real_png_is_accepted(self):
        self.assertTrue(valid_artwork(sample_png()))

    def test_fake_png_signature_and_iend_is_rejected(self):
        self.assertFalse(valid_artwork(b'\x89PNG\r\n\x1a\n' + b'x' * 64 +
                                       b'\x00\x00\x00\x00IEND\xaeB\x60\x82'))

    def test_truncated_png_corrupt_crc_and_trailing_bytes_rejected(self):
        good = sample_png()
        for bad in (good[:-3], good[:-12], good[:43] + b'!' + good[44:],
                    good + b'x'):
            with self.subTest(size=len(bad)):
                self.assertFalse(valid_artwork(bad))

    def test_valid_chunk_crc_with_illegal_png_filter_rejected(self):
        def chunk(kind, payload):
            return (struct.pack('>I', len(payload)) + kind + payload +
                    struct.pack('>I', zlib.crc32(kind + payload) & 0xffffffff))
        bad = (b'\x89PNG\r\n\x1a\n' +
               chunk(b'IHDR', struct.pack('>IIBBBBB', 1, 1, 8, 2, 0, 0, 0)) +
               chunk(b'IDAT', zlib.compress(b'\x09\xaa\xbb\xcc')) +
               chunk(b'IEND', b''))
        self.assertFalse(valid_artwork(bad))

    def test_invalid_png_color_depth_combinations_rejected(self):
        def chunk(kind, payload):
            return (struct.pack('>I', len(payload)) + kind + payload +
                    struct.pack('>I', zlib.crc32(kind + payload) & 0xffffffff))
        for color, depth in ((2, 1), (6, 2), (3, 16), (4, 4), (2, 4)):
            with self.subTest(color=color, depth=depth):
                header = chunk(b'IHDR', struct.pack(
                    '>IIBBBBB', 1, 1, depth, color, 0, 0, 0))
                data = b'\x89PNG\r\n\x1a\n' + header + chunk(
                    b'IDAT', zlib.compress(b'\x00\x00\x00\x00')) + chunk(b'IEND', b'')
                self.assertFalse(valid_artwork(data))

    def test_fake_jpeg_signature_and_eoi_is_rejected(self):
        self.assertFalse(valid_artwork(b'\xff\xd8' + b'not a JPEG' * 8 + b'\xff\xd9'))


    def test_syntactically_plausible_but_undecodable_jpeg_not_cached(self):
        from unittest import mock
        from resources.lib.util import artwork
        # Correct marker framing but not a decodable JPEG entropy stream.
        sof = (b'\xff\xc0\x00\x08\x08\x00\x01\x00\x01\x01')
        sos = (b'\xff\xda\x00\x06\x01\x01\x00\x00')
        # Include a legal COM marker so we reach the minimum body length.
        fake = b'\xff\xd8\xff\xfe\x00\x09comment' + sof + sos + b'\x11\x22\x33\x44' + b'\xff\xd9'
        self.assertGreaterEqual(len(fake), 32)
        self.assertTrue(artwork._valid_jpeg(fake))
        with mock.patch.object(artwork, '_decode_jpeg_if_available',
                               return_value=None):
            self.assertTrue(valid_artwork(fake))  # fresh download remains usable
            self.assertFalse(valid_artwork(fake, for_cache=True))
        with mock.patch.object(artwork, '_decode_jpeg_if_available',
                               return_value=False):
            self.assertFalse(valid_artwork(fake))

    def test_pillow_validates_decoded_jpeg_when_available(self):
        import io
        try:
            from PIL import Image
        except ImportError:
            self.skipTest('Pillow not installed on this host')
        stream = io.BytesIO()
        Image.new('RGB', (4, 4), (10, 20, 30)).save(stream, format='JPEG')
        self.assertTrue(valid_artwork(stream.getvalue(), for_cache=True))

    def test_oversized_or_empty_rejected(self):
        self.assertFalse(valid_artwork(b''))
        self.assertFalse(valid_artwork('not bytes'))
        self.assertFalse(valid_artwork(b'x' * (16*1024*1024+1)))


if __name__ == '__main__':
    unittest.main()
