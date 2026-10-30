import os
import stat
import hashlib
import re

from resources.lib.util.certpin import spki_sha256, CertificatePinError
from xml.etree import ElementTree
import ssl
import uuid

try:
    import http.client as httplib
except ImportError:
    import httplib

DEFAULT_KEY_DIR = '/storage/.cache/moonlight'
HTTPS_PORT = 47984
HTTP_PORT = 47989
TIMEOUT = 4
MAX_XML_BYTES = 2 * 1024 * 1024
MAX_ART_BYTES = 16 * 1024 * 1024
UNIQUEID_CHARS = 16
DEFAULT_UNIQUE_ID = '0123456789ABCDEF'


class HostError(Exception):
    pass


class HostIdentityError(HostError):
    """Authenticated host public key does not match the saved pairing key."""
    pass


class Host(object):
    def __init__(self, address, key_dir=DEFAULT_KEY_DIR, port=HTTPS_PORT,
                 log=None, http_port=HTTP_PORT):
        self.address = address
        self.key_dir = key_dir
        self.port = port
        self.http_port = http_port
        self.log = log or (lambda msg: None)
        self._ctx = None
        self._spki_pin = None
        self._last_pair_status_public = False

    def has_trusted_pin(self):
        # Existence alone is not authentication. _get() validates the peer
        # public key against the saved certificate before sending a request.
        identity = hashlib.sha256(self.address.encode('utf-8')).hexdigest()
        return os.path.lexists(os.path.join(
            self.key_dir, 'server-%s.pem' % identity))

    def _context(self):
        if self._ctx is not None:
            return self._ctx

        cert = os.path.join(self.key_dir, 'client.pem')
        key = os.path.join(self.key_dir, 'key.pem')
        if not os.path.exists(cert) or not os.path.exists(key):
            raise HostError('no pairing keys in %s' % self.key_dir)

        ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT)
        ctx.check_hostname = False
        # Native Moonlight pins SHA-256 of DER SubjectPublicKeyInfo, NOT the
        # complete certificate. Verify the same public key after TLS connect
        # and BEFORE sending an authenticated request. This permits a
        # renewed self-signed cert retaining its original paired key.
        identity = hashlib.sha256(self.address.encode('utf-8')).hexdigest()
        pinned = os.path.join(self.key_dir, 'server-%s.pem' % identity)
        self._spki_pin = None
        if os.path.lexists(pinned):
            try:
                if os.path.getsize(pinned) > 8192:
                    raise HostIdentityError('Saved Sunshine certificate is oversized')
                with open(pinned, encoding='ascii') as source:
                    pem = source.read()
                self._spki_pin = spki_sha256(ssl.PEM_cert_to_DER_cert(pem))
            except (OSError, UnicodeError, ValueError, CertificatePinError) as error:
                raise HostIdentityError(
                    'Saved Sunshine host certificate is invalid') from error
        # The handshake permits Sunshine's self-signed certificate; our
        # explicit SPKI check below supplies server identity authentication.
        # Existing hosts without a trusted pin retain legacy behavior.
        ctx.verify_mode = ssl.CERT_NONE
        ctx.load_cert_chain(cert, key)
        try:
            ctx.set_ciphers('DEFAULT:@SECLEVEL=0')
        except ssl.SSLError:
            pass

        self._ctx = ctx
        return ctx

    def _unique_id(self):
        path = os.path.join(self.key_dir, 'uniqueid.dat')
        try:
            # Native Moonlight uses exactly the first 16 hexadecimal bytes.
            # Do not masquerade as the default identity when an existing
            # pairing file is corrupt, unreadable or has an invalid encoding.
            fd = os.open(path, os.O_RDONLY | os.O_NOFOLLOW |
                         os.O_NONBLOCK | os.O_CLOEXEC)
            with os.fdopen(fd, 'rb') as f:
                info = os.fstat(f.fileno())
                if not stat.S_ISREG(info.st_mode) or info.st_uid != os.geteuid():
                    raise HostError('Existing Moonlight device ID is not an owned regular file')
                value = f.read(UNIQUEID_CHARS)
        except FileNotFoundError:
            if os.path.lexists(path):
                raise HostError('Existing Moonlight device ID is inaccessible')
            return DEFAULT_UNIQUE_ID
        except OSError as error:
            raise HostError('Cannot read Moonlight device ID') from error
        if (len(value) != UNIQUEID_CHARS or
                any(byte not in b'0123456789abcdefABCDEF' for byte in value)):
            raise HostError('Existing Moonlight device ID is corrupt or truncated')
        return value.decode('ascii')

    def _advertised_https_port(self):
        """Use public discovery only to locate TLS, never to establish trust.

        A public /serverinfo response is untrusted. The subsequent HTTPS
        connection MUST still match the previously paired SPKI before any
        authenticated request is sent.
        """
        try:
            root = ElementTree.fromstring(self._get_public_serverinfo())
            if root.attrib.get('status_code', '200') != '200':
                return None
            raw = (root.findtext('.//HttpsPort') or '').strip()
            if not re.fullmatch(r'[0-9]{1,5}', raw):
                return None
            port = int(raw)
            return port if 1 <= port <= 65535 else None
        except (OSError, ssl.SSLError, HostError, ElementTree.ParseError,
                TypeError, ValueError):
            return None

    def _get(self, path, params):
        context = self._context()
        # The native PIN handshake is the only permitted unpinned bootstrap.
        if self._spki_pin is None:
            raise HostIdentityError(
                'Sunshine host has no verified certificate; PIN re-pair required')
        query = '&'.join('%s=%s' % (k, v) for k, v in params)
        url = '%s?uniqueid=%s&uuid=%s&%s' % (
            path, self._unique_id(), uuid.uuid4(), query) if query else \
            '%s?uniqueid=%s&uuid=%s' % (path, self._unique_id(), uuid.uuid4())

        maximum = MAX_ART_BYTES if path == '/appasset' else MAX_XML_BYTES
        port = self.port
        for attempt in range(2):
            conn = httplib.HTTPSConnection(self.address, port,
                                           context=context, timeout=TIMEOUT)
            try:
                try:
                    conn.connect()
                except (OSError, ssl.SSLError):
                    # Nonstandard Sunshine HTTPS ports are advertised over
                    # public HTTP. Retry ONLY a failed TLS connection, and
                    # only for the default port. A TLS identity mismatch
                    # never triggers discovery or an HTTP downgrade.
                    if attempt == 0 and port == HTTPS_PORT:
                        advertised = self._advertised_https_port()
                        if advertised is not None and advertised != port:
                            port = advertised
                            continue
                    raise
                peer = conn.sock.getpeercert(binary_form=True)
                try:
                    matched = spki_sha256(peer) == self._spki_pin
                except CertificatePinError as error:
                    raise HostIdentityError(
                        'Sunshine presented an invalid certificate') from error
                if not matched:
                    raise HostIdentityError(
                        'Sunshine server public key differs from the paired host')
                conn.request('GET', url)
                response = conn.getresponse()
                body = response.read(maximum + 1)
                if len(body) > maximum:
                    raise HostError('%s response exceeds %s bytes' % (path, maximum))
                if response.status != 200:
                    raise HostError('%s answered with status %s' % (path, response.status))
                # Cache the discovered port only after the pinned HTTPS
                # request actually succeeded.
                self.port = port
                return body
            finally:
                conn.close()

    def _get_public_serverinfo(self):
        """Unauthenticated fallback for the identity whose TLS keys were revoked."""
        path = '/serverinfo'
        url = '%s?uniqueid=%s&uuid=%s' % (
            path, self._unique_id(), uuid.uuid4())
        conn = httplib.HTTPConnection(self.address, self.http_port, timeout=TIMEOUT)
        try:
            conn.request('GET', url)
            response = conn.getresponse()
            body = response.read(MAX_XML_BYTES + 1)
            if len(body) > MAX_XML_BYTES:
                raise HostError('Public serverinfo response exceeds size limit')
            if response.status != 200:
                raise HostError('HTTP pairing verification returned %s' % response.status)
            return body
        finally:
            conn.close()

    @staticmethod
    def parse_applist(body):
        """Parse Sunshine XML without leaking escaped titles into launch names."""
        try:
            root = ElementTree.fromstring(body)
        except (ElementTree.ParseError, TypeError, ValueError) as error:
            raise HostError('Sunshine returned malformed application XML') from error
        status = root.attrib.get('status_code')
        if status not in (None, '200'):
            raise HostError('Sunshine application request failed (status %s)' % status)
        paired = root.findtext('.//PairStatus')
        if paired is not None and paired.strip() == '0':
            raise HostError('this device is not paired with the host')
        apps = []
        for app in root.findall('.//App'):
            name = (app.findtext('AppTitle') or '').strip()
            number = (app.findtext('ID') or '').strip()
            if not name or not re.fullmatch(r'[0-9]+', number):
                raise HostError('Sunshine returned an incomplete application entry')
            apps.append((number, name))
        return apps

    def applist(self):
        return self.parse_applist(self._get('/applist', []))

    def has_pairing_keys(self):
        return (os.path.exists(os.path.join(self.key_dir, 'client.pem'))
                and os.path.exists(os.path.join(self.key_dir, 'key.pem')))

    @staticmethod
    def parse_pair_status(body):
        try:
            root = ElementTree.fromstring(body)
        except (ElementTree.ParseError, TypeError, ValueError) as error:
            raise HostError('Sunshine returned malformed pairing status XML') from error
        if root.attrib.get('status_code', '200') != '200':
            raise HostError('Sunshine rejected the pairing status request')
        paired = (root.findtext('.//PairStatus') or '').strip()
        if paired not in ('0', '1'):
            raise HostError('Sunshine did not provide a valid pairing status')
        return paired == '1'

    def pair_status(self, allow_http_fallback=False):
        self._last_pair_status_public = False
        try:
            body = self._get('/serverinfo', [])
        except HostIdentityError:
            # A mismatched trusted key is NOT proof of successful unpair.
            # Never downgrade this security failure to unauthenticated HTTP.
            raise
        except (HostError, OSError, ssl.SSLError):
            if not allow_http_fallback:
                raise
            # Once unpaired, Sunshine may reject our old client certificate.
            # A public serverinfo response still must contain explicit, valid
            # PairStatus=0; unreachable/malformed responses remain errors.
            body = self._get_public_serverinfo()
            self._last_pair_status_public = True
        try:
            return self.parse_pair_status(body)
        except HostError as error:
            # Some servers return an XML authentication rejection with HTTP
            # 200 after revoking the client certificate. Fall back only for
            # that explicit condition, never for malformed or missing data.
            if (not allow_http_fallback or
                    str(error) != 'Sunshine rejected the pairing status request'):
                raise
            public_status = self.parse_pair_status(self._get_public_serverinfo())
            self._last_pair_status_public = True
            return public_status

    def paired(self):
        try:
            return self.pair_status()
        except (HostError, OSError, ssl.SSLError):
            return False

    def boxart(self, app_id):
        try:
            body = self._get('/appasset', [('appid', app_id),
                                           ('AssetType', '2'),
                                           ('AssetIdx', '0')])
        except (HostError, OSError, ssl.SSLError):
            return None

        if body[:4] == b'\x89PNG' or body[:2] == b'\xff\xd8':
            return body
        return None
