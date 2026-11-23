"""Signature lines and trust (section 12)."""
import base64
import hashlib
import re

from . import ed25519
from .model import SignatureResult

PREFIX = b'# mncf-signature: '
_LINE = re.compile(rb'^# mncf-signature: (ed25519) ([0-9a-f]{16}) ([A-Za-z0-9+/]{86}==)\Z')
_ANY_LINE = re.compile(rb'^# mncf-signature: ([a-z0-9-]+) ')


def key_id(public):
    """The key id of a 32-byte public key: the first 16 hexadecimal digits of its SHA-256 hash, in lower case."""
    return hashlib.sha256(public).hexdigest()[:16]


class TrustList:
    """The keys a reader trusts, by name, and the key ids it no longer trusts."""

    def __init__(self):
        self._keys = {}
        self.revoked = set()

    def trust(self, name, public):
        if len(public) != 32:
            raise ValueError('an Ed25519 public key is 32 bytes')
        self._keys.setdefault(key_id(public), []).append((name, bytes(public)))

    def revoke(self, kid):
        self.revoked.add(kid.lower())

    def keys(self, kid):
        return self._keys.get(kid, [])


_KEY_ID = re.compile(r'^[0-9a-f]{16}\Z')


def read_trust_list(text):
    """A TrustList from the text of a trust list file (`format = "mncf-keys/1"`, spec section 12)."""
    from . import MncfError, parse_toml
    doc = parse_toml(text)
    if not doc or next(iter(doc)) != 'format' or doc['format'] != 'mncf-keys/1':
        raise MncfError('format', '`format = "mncf-keys/1"` must be the first key of a trust list')
    trust = TrustList()
    keys, revoked = doc.get('key', []), doc.get('revoked', [])
    if not isinstance(keys, list) or not all(isinstance(k, dict) for k in keys):
        raise MncfError('schema:key', '`key` must be an array of tables')
    for k in keys:
        name, public = k.get('name'), k.get('public')
        try:
            raw = base64.b64decode(public, validate=True) if isinstance(public, str) else b''
        except ValueError:
            raw = b''
        if not isinstance(name, str) or not name or len(raw) != 32 or base64.b64encode(raw).decode() != public:
            raise MncfError('schema:key', 'a key needs a `name` and a `public` key of 32 bytes in base64')
        if 'id' in k and k['id'] != key_id(raw):
            raise MncfError('schema:key', f'the key id of {name!r} is {key_id(raw)}, not {k["id"]!r}')
        trust.trust(name, raw)
    if not isinstance(revoked, list) or not all(isinstance(r, dict) for r in revoked):
        raise MncfError('schema:revoked', '`revoked` must be an array of tables')
    for r in revoked:
        kid = r.get('id')
        if not isinstance(kid, str) or not _KEY_ID.match(kid):
            raise MncfError('schema:revoked', 'a revoked key needs an `id` of 16 lower-case hexadecimal digits')
        trust.revoke(kid)
    return trust


def split(data):
    """(signed bytes, signature lines, whether only signature lines follow the first one)."""
    at = 0 if data.startswith(PREFIX) else data.find(b'\n' + PREFIX)
    if at < 0:
        return data, [], True
    start = at if at == 0 else at + 1
    lines = data[start:].split(b'\n')
    if lines and lines[-1] == b'':
        lines.pop()
    clean = all(line.startswith(PREFIX) for line in lines)
    return data[:start], lines, clean


def check(data, trust=None):
    """The signature result of a file's bytes."""
    trust = trust or TrustList()
    signed, lines, clean = split(data)
    if not lines:
        return SignatureResult('unsigned')
    if not clean:
        return SignatureResult('invalid')
    results, signers, kids = [], [], []
    for line in lines:
        m = _LINE.match(line)
        if m is None:
            other = _ANY_LINE.match(line)
            if other is not None and other.group(1) != b'ed25519':
                results.append('unknown-key')       # an algorithm this reader does not know
                continue
            results.append('invalid')
            continue
        kid, sig64 = m.group(2).decode(), m.group(3)
        sig = base64.b64decode(sig64)
        kids.append(kid)
        if base64.b64encode(sig) != sig64:
            results.append('invalid')
        elif kid in trust.revoked:
            results.append('revoked')
        elif trust.keys(kid):
            good = [name for name, pub in trust.keys(kid) if ed25519.verify(pub, signed, sig)]
            results.append('verified' if good else 'invalid')
            signers.extend(good)
        else:
            results.append('unknown-key')
    for result in ('invalid', 'revoked', 'verified', 'unknown-key'):
        if result in results:
            return SignatureResult(result, signers if result == 'verified' else [], kids)
    return SignatureResult('unsigned')


def sign_line(seed, signed):
    """The signature line, with its line end, for these signed bytes."""
    pub = ed25519.public_key(seed)
    sig = base64.b64encode(ed25519.sign(seed, signed)).decode()
    return f'# mncf-signature: ed25519 {key_id(pub)} {sig}\n'.encode()
