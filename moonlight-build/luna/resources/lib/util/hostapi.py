import os
import re
import ssl
import uuid

try:
    import http.client as httplib
except ImportError:
    import httplib

DEFAULT_KEY_DIR = '/storage/.cache/moonlight'
HTTPS_PORT = 47984
TIMEOUT = 4
UNIQUEID_CHARS = 16
DEFAULT_UNIQUE_ID = '0123456789ABCDEF'


class HostError(Exception):
    pass


class Host(object):
    def __init__(self, address, key_dir=DEFAULT_KEY_DIR, port=HTTPS_PORT,
                 log=None):
        self.address = address
        self.key_dir = key_dir
        self.port = port
        self.log = log or (lambda msg: None)
        self._ctx = None

    def _context(self):
        if self._ctx is not None:
            return self._ctx

        cert = os.path.join(self.key_dir, 'client.pem')
        key = os.path.join(self.key_dir, 'key.pem')
        if not os.path.exists(cert) or not os.path.exists(key):
            raise HostError('no pairing keys in %s' % self.key_dir)

        ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT)
        ctx.check_hostname = False
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
            # Native Moonlight reads one 16-byte item and ignores trailing data.
            # Match that identity exactly so Luna's direct host API calls and
            # moonlight-embedded present the same persisted device ID.
            with open(path, 'rb') as f:
                value = f.read(UNIQUEID_CHARS)
            if len(value) == UNIQUEID_CHARS:
                return value.decode('ascii')
        except (OSError, UnicodeError):
            pass
        return DEFAULT_UNIQUE_ID

    def _get(self, path, params):
        query = '&'.join('%s=%s' % (k, v) for k, v in params)
        url = '%s?uniqueid=%s&uuid=%s&%s' % (
            path, self._unique_id(), uuid.uuid4(), query) if query else \
            '%s?uniqueid=%s&uuid=%s' % (path, self._unique_id(), uuid.uuid4())

        conn = httplib.HTTPSConnection(self.address, self.port,
                                       context=self._context(), timeout=TIMEOUT)
        try:
            conn.request('GET', url)
            response = conn.getresponse()
            body = response.read()
            if response.status != 200:
                raise HostError('%s answered with status %s' % (path, response.status))
            return body
        finally:
            conn.close()

    def applist(self):
        body = self._get('/applist', [])
        text = body.decode('utf-8', 'replace')

        apps = []
        for chunk in re.findall(r'<App>.*?</App>', text, re.S):
            name = re.search(r'<AppTitle>(.*?)</AppTitle>', chunk, re.S)
            number = re.search(r'<ID>(.*?)</ID>', chunk, re.S)
            if name and number:
                apps.append((number.group(1).strip(), name.group(1).strip()))

        if not apps and 'PairStatus>0' in text:
            raise HostError('this device is not paired with the host')
        return apps

    def has_pairing_keys(self):
        return (os.path.exists(os.path.join(self.key_dir, 'client.pem'))
                and os.path.exists(os.path.join(self.key_dir, 'key.pem')))

    def pair_status(self):
        body = self._get('/serverinfo', [])
        return b'<PairStatus>1</PairStatus>' in body

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
