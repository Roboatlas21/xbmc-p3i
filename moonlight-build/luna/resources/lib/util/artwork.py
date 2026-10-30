"""Bounded image validation for Sunshine box art without a Pillow dependency."""
import io
import struct
import zlib

_MAX_FILE = 16 * 1024 * 1024
_MAX_PIXELS = 32 * 1024 * 1024
_MAX_DECOMPRESSED = 96 * 1024 * 1024
_PNG_MAGIC = b'\x89PNG\r\n\x1a\n'
_SOF = frozenset((0xC0, 0xC1, 0xC2, 0xC3, 0xC5, 0xC6, 0xC7,
                  0xC9, 0xCA, 0xCB, 0xCD, 0xCE, 0xCF))


def _valid_png(data):
    if not data.startswith(_PNG_MAGIC):
        return False
    pos = 8
    have_ihdr = have_idat = False
    width = height = depth = color = interlaced = 0
    compressed = []
    size_compressed = 0
    while pos + 12 <= len(data):
        length = struct.unpack_from('>I', data, pos)[0]
        if length > len(data) - pos - 12:
            return False
        kind = data[pos + 4:pos + 8]
        end = pos + 8 + length
        payload = data[pos + 8:end]
        expected_crc = struct.unpack_from('>I', data, end)[0]
        if zlib.crc32(kind + payload) & 0xffffffff != expected_crc:
            return False
        pos = end + 4
        if not have_ihdr:
            if kind != b'IHDR' or length != 13:
                return False
            width, height, depth, color, compression, filtering, interlaced = (
                struct.unpack('>IIBBBBB', payload))
            valid_depths = {
                0: (1, 2, 4, 8, 16),  # grayscale
                2: (8, 16),           # RGB
                3: (1, 2, 4, 8),     # indexed
                4: (8, 16),           # grayscale with alpha
                6: (8, 16),           # RGBA
            }
            if (not width or not height or width * height > _MAX_PIXELS or
                    compression or filtering or interlaced not in (0, 1) or
                    depth not in valid_depths.get(color, ())):
                return False
            have_ihdr = True
        elif kind == b'IDAT':
            size_compressed += length
            if size_compressed > _MAX_FILE:
                return False
            compressed.append(payload)
            have_idat = True
        elif kind == b'IEND':
            if length or pos != len(data) or not have_idat:
                return False
            try:
                decoder = zlib.decompressobj()
                total = 0
                uncompressed = []
                for fragment in compressed:
                    part = decoder.decompress(fragment, _MAX_DECOMPRESSED - total + 1)
                    total += len(part)
                    if total > _MAX_DECOMPRESSED or decoder.unconsumed_tail:
                        return False
                    uncompressed.append(part)
                tail = decoder.flush(_MAX_DECOMPRESSED - total + 1)
                total += len(tail)
                if total > _MAX_DECOMPRESSED or not decoder.eof or decoder.unused_data:
                    return False
                uncompressed.append(tail)
                channels = {0: 1, 2: 3, 3: 1, 4: 2, 6: 4}[color]
                raw = b''.join(uncompressed)
                if not interlaced:
                    row_bytes = (width * channels * depth + 7) // 8
                    stride = row_bytes + 1
                    if len(raw) != stride * height:
                        return False
                    # Every PNG scanline must use one of filters 0..4.
                    return all(raw[row] <= 4 for row in range(0, len(raw), stride))
                # Adam7 interlacing: verify all seven pass scanline lengths and
                # their filter bytes without relying on an optional image lib.
                position = 0
                for x, y, dx, dy in ((0, 0, 8, 8), (4, 0, 8, 8),
                                     (0, 4, 4, 8), (2, 0, 4, 4),
                                     (0, 2, 2, 4), (1, 0, 2, 2), (0, 1, 1, 2)):
                    pw = max(0, (width - x + dx - 1) // dx)
                    ph = max(0, (height - y + dy - 1) // dy)
                    if not pw or not ph:
                        continue
                    stride = (pw * channels * depth + 7) // 8 + 1
                    if position + stride * ph > len(raw):
                        return False
                    for row in range(position, position + stride * ph, stride):
                        if raw[row] > 4:
                            return False
                    position += stride * ph
                return position == len(raw)
            except (ValueError, zlib.error):
                return False
        elif kind == b'IHDR':
            return False
    return False


def _valid_jpeg(data):
    if not data.startswith(b'\xff\xd8') or not data.endswith(b'\xff\xd9'):
        return False
    pos = 2
    have_sof = have_sos = False
    scan_bytes = 0
    in_scan = False
    while pos + 1 < len(data):
        if in_scan:
            # Handle stuffed FF00 and restart markers. Progressive JPEG can
            # contain multiple SOS segments separated by DHT/DRI markers.
            while pos + 1 < len(data):
                if data[pos] != 0xff:
                    pos += 1
                    scan_bytes += 1
                elif data[pos + 1] == 0x00:
                    pos += 2
                    scan_bytes += 1
                elif 0xD0 <= data[pos + 1] <= 0xD7:
                    pos += 2
                else:
                    in_scan = False
                    break
        if pos + 1 >= len(data) or data[pos] != 0xff:
            return False
        while pos < len(data) and data[pos] == 0xff:
            pos += 1
        if pos == len(data):
            return False
        marker = data[pos]
        pos += 1
        if marker == 0xD9:
            return have_sof and have_sos and scan_bytes > 0 and pos == len(data)
        if marker in (0xD8, 0x00):
            return False
        if 0xD0 <= marker <= 0xD7 or marker == 0x01:
            continue
        if pos + 2 > len(data):
            return False
        length = struct.unpack_from('>H', data, pos)[0]
        if length < 2 or pos + length > len(data):
            return False
        if marker in _SOF:
            if length < 8:
                return False
            height, width = struct.unpack_from('>HH', data, pos + 3)
            if not width or not height or width * height > _MAX_PIXELS:
                return False
            have_sof = True
        if marker == 0xDA:
            if not have_sof or length < 6:
                return False
            have_sos = True
            in_scan = True
        pos += length
    return False


def _decode_jpeg_if_available(data):
    """True=decoded, False=invalid, None=no Pillow decoder installed."""
    try:
        from PIL import Image
    except ImportError:
        return None
    try:
        # verify checks the JPEG envelope, while load decodes every scan.
        with Image.open(io.BytesIO(data)) as image:
            image.verify()
        with Image.open(io.BytesIO(data)) as image:
            if (image.width <= 0 or image.height <= 0 or
                    image.width * image.height > _MAX_PIXELS):
                return False
            image.load()
        return True
    except (OSError, ValueError, SyntaxError, EOFError):
        return False


def valid_artwork(data, *, for_cache=False):
    if not isinstance(data, bytes) or not 32 <= len(data) <= _MAX_FILE:
        return False
    if data.startswith(_PNG_MAGIC):
        return _valid_png(data)
    if data.startswith(b'\xff\xd8'):
        if not _valid_jpeg(data):
            return False
        decoded = _decode_jpeg_if_available(data)
        if decoded is False:
            return False
        # Without a decoder we may show newly downloaded JPEGs through Kodi,
        # but must not keep recycling one that Kodi might be unable to load.
        return decoded is True or not for_cache
    return False
