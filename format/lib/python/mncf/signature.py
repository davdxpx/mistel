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
