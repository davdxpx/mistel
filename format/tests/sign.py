"""Writes the signed files of the conformance suite from their unsigned sources.

Run it after changing `cases/signed.mncf`, `packs/files/pack.toml`, the files of `updates/files/` it signs, or the list
below:

    python format/tests/sign.py

The keys are the test keys of RFC 8032, section 7.1, tests 1 and 2. They are published test data that anyone can sign
with, so a file signed with them proves nothing. Never trust them anywhere but in this suite.
"""
import base64
import pathlib
import sys

HERE = pathlib.Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent / 'lib' / 'python'))

from mncf import ed25519, signature  # noqa: E402

KEY_ONE = bytes.fromhex('9d61b19deffd5a60ba844af492ec2cc44449c5697b326919703bac031cae7f60')
KEY_TWO = bytes.fromhex('4ccd089b28ff96da9db6c346ec114e0f5b8a319f35aba624da8cf6ed4fb8a6fb')
GROUP_ORDER = 2 ** 252 + 27742317777372353535851937790883648493


def signed(data, *seeds):
    return data + b''.join(signature.sign_line(seed, data) for seed in seeds)


def with_signers(data, *seeds):
    """The file with a `signers` line after its `file_version` line (or after `name` where it has none), naming the
    public keys of these seeds, as `mistel release` writes it before signing (spec, section 12)."""
    keys = ', '.join('"' + base64.b64encode(ed25519.public_key(seed)).decode() + '"' for seed in seeds)
    lines = data.split(b'\n')
    at = next((i for i, x in enumerate(lines) if x.startswith(b'file_version = ')),
              next(i for i, x in enumerate(lines) if x.startswith(b'name = ')))
    lines.insert(at + 1, f'signers = [{keys}]'.encode())
    return b'\n'.join(lines)


def high_s(data, seed):
    """A signature line whose S has the group order added: the same point equation, but not a canonical signature."""
    line = signature.sign_line(seed, data)
    sig = ed25519.sign(seed, data)
    s = int.from_bytes(sig[32:], 'little') + GROUP_ORDER
    bad = sig[:32] + s.to_bytes(32, 'little')
    return data + line.replace(base64.b64encode(sig), base64.b64encode(bad))


def wrong_key_id(data, seed, other_seed):
    line = signature.sign_line(seed, data)
    own = signature.key_id(ed25519.public_key(seed)).encode()
    other = signature.key_id(ed25519.public_key(other_seed)).encode()
    return data + line.replace(own, other)


def main():
    cases = HERE / 'cases'
    base = (cases / 'signed.mncf').read_bytes()
    one = signed(base, KEY_ONE)
    files = {
        cases / 'signed-one.mncf': one,
        cases / 'signed-changed.mncf': one.replace(b'name = "Conformance"', b'name = "Conformanze"', 1),
        cases / 'signed-two.mncf': signed(base, KEY_ONE, KEY_TWO),
        cases / 'signed-trailing.mncf': one + b'# a comment after the signature\n',
        cases / 'signed-bad-line.mncf': base + b'# mncf-signature: ed25519 21fe31dfa154a261 not-base64\n',
        cases / 'signed-other-algorithm.mncf': base + b'# mncf-signature: x-future 0123456789abcdef QUJD\n',
        cases / 'signed-high-s.mncf': high_s(base, KEY_ONE),
        cases / 'signed-wrong-key-id.mncf': wrong_key_id(base, KEY_ONE, KEY_TWO),
        cases / 'signed-algorithm-name.mncf': base + signature.sign_line(KEY_ONE, base).replace(b' ed25519 ',
                                                                                          b' x-other ', 1),
    }
    manifest = (HERE / 'packs' / 'files' / 'pack.toml').read_bytes()
    pack_signed = signed(manifest, KEY_ONE)
    files[HERE / 'packs' / 'files' / 'pack-signed.toml'] = pack_signed
    files[HERE / 'packs' / 'files' / 'pack-signed-changed.toml'] = pack_signed.replace(
        b'file_version = "2"', b'file_version = "3"', 1)
    files[cases / 'signed-signers.mncf'] = signed(with_signers(base, KEY_ONE), KEY_ONE)
    files[cases / 'signed-signers-changed.mncf'] = files[cases / 'signed-signers.mncf'].replace(
        b'name = "Conformance"', b'name = "Conformanze"', 1)

    up = HERE / 'updates' / 'files'
    v3, v5, v6 = ((up / f'v{n}.mncf').read_bytes() for n in (3, 5, 6))
    files[up / 'v3-one.mncf'] = signed(with_signers(v3, KEY_ONE), KEY_ONE)
    files[up / 'v3-one-bare.mncf'] = signed(v3, KEY_ONE)
    files[up / 'v3-both.mncf'] = signed(with_signers(v3, KEY_ONE, KEY_TWO), KEY_ONE, KEY_TWO)
    files[up / 'v3-other.mncf'] = v3 + b'# mncf-signature: x-future 0123456789abcdef QUJD\n'
    files[up / 'v5-one.mncf'] = signed(with_signers(v5, KEY_ONE), KEY_ONE)
    files[up / 'v5-one-bare.mncf'] = signed(v5, KEY_ONE)
    files[up / 'v5-one-changed.mncf'] = files[up / 'v5-one.mncf'].replace(b'Money works again.', b'Money works.', 1)
    files[up / 'v5-two.mncf'] = signed(with_signers(v5, KEY_TWO), KEY_TWO)
    files[up / 'v5-both.mncf'] = signed(with_signers(v5, KEY_ONE, KEY_TWO), KEY_ONE, KEY_TWO)
    files[up / 'v6-two.mncf'] = signed(with_signers(v6, KEY_TWO), KEY_TWO)
    p1, p2 = (up / 'pack-v1.toml').read_bytes(), (up / 'pack-v2.toml').read_bytes()
    files[up / 'pack-v1-one.toml'] = signed(with_signers(p1, KEY_ONE), KEY_ONE)
    files[up / 'pack-v2-one.toml'] = signed(with_signers(p2, KEY_ONE), KEY_ONE)
    files[up / 'pack-v2-two.toml'] = signed(with_signers(p2, KEY_TWO), KEY_TWO)
    for path, data in files.items():
        path.write_bytes(data)
        print(f'wrote {path.relative_to(HERE).as_posix()}')


if __name__ == '__main__':
    main()
