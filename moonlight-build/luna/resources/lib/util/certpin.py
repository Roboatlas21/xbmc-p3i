"""Public-key pinning for GameStream HTTPS without third-party Python packages.

The native client hashes the DER SubjectPublicKeyInfo (OpenSSL i2d_PUBKEY).
Parse only the bounded X.509 fields necessary to extract the same SPKI bytes;
certificate renewal with the same key must preserve the identity.
"""
import hashlib


class CertificatePinError(ValueError):
    pass


def _der_tlv(der, position, limit):
    if position + 2 > limit:
        raise CertificatePinError('Truncated X.509 element')
    tag = der[position]
    length0 = der[position + 1]
    position += 2
    if length0 & 0x80:
        count = length0 & 0x7f
        if count == 0 or count > 4 or position + count > limit:
            raise CertificatePinError('Invalid X.509 element length')
        if der[position] == 0:
            raise CertificatePinError('Noncanonical X.509 length')
        length = int.from_bytes(der[position:position + count], 'big')
        if length < 128:
            raise CertificatePinError('Noncanonical X.509 short length')
        position += count
    else:
        length = length0
    end = position + length
    if end > limit:
        raise CertificatePinError('X.509 element exceeds certificate')
    return tag, position, end, end


def spki_sha256(der):
    if not isinstance(der, bytes) or not 64 <= len(der) <= 65536:
        raise CertificatePinError('Invalid X.509 certificate size')
    tag, start, end, after = _der_tlv(der, 0, len(der))
    if tag != 0x30 or after != len(der):
        raise CertificatePinError('Expected X.509 certificate sequence')
    tag, tbs_start, tbs_end, _ = _der_tlv(der, start, end)
    if tag != 0x30:
        raise CertificatePinError('Expected X.509 TBSCertificate')
    cursor = tbs_start
    tag, _, _, next_cursor = _der_tlv(der, cursor, tbs_end)
    if tag == 0xa0:  # Optional EXPLICIT version
        cursor = next_cursor
    for expected in (0x02, 0x30, 0x30, 0x30, 0x30):
        tag, _, _, cursor = _der_tlv(der, cursor, tbs_end)
        if tag != expected:
            raise CertificatePinError('Malformed X.509 subject or issuer')
    start_spki = cursor
    tag, _, _, cursor = _der_tlv(der, cursor, tbs_end)
    if tag != 0x30:
        raise CertificatePinError('Missing X.509 SubjectPublicKeyInfo')
    return hashlib.sha256(der[start_spki:cursor]).digest()
