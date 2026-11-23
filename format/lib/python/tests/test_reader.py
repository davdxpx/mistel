"""Unit tests of the Python reader: the parts the conformance suite does not reach on its own."""
import base64
import hashlib
import io
import pathlib
import sys
import unittest
import zipfile

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1]))

import mncf  # noqa: E402
from mncf import ed25519, encode, signature, values  # noqa: E402
from mncf.pack import read_pack, valid_path  # noqa: E402

# RFC 8032, section 7.1: secret key, public key, message, signature.
RFC8032 = [
    ('9d61b19deffd5a60ba844af492ec2cc44449c5697b326919703bac031cae7f60',
     'd75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a', '',
     'e5564300c360ac729086e2cc806e828a84877f1eb8e5d974d873e065224901555fb8821590a33bacc61e39701cf9b46bd25bf5f0595bbe24655141438e7a100b'),
    ('4ccd089b28ff96da9db6c346ec114e0f5b8a319f35aba624da8cf6ed4fb8a6fb',
     '3d4017c3e843895a92b70aa74d1b7ebc9c982ccf2ec4968cc0cd55f12af4660c', '72',
     '92a009a9f0d4cab8720e820b5f642540a2b27b5416503f8fb3762223ebdb69da085ac1e43e15996e458f3613d0f11d8c387b2eaeb4302aeeb00d291612bb0c00'),
    ('c5aa8df43f9f837bedb7442f31dcb7b166d38535076f094b85ce3a2e0b4458f7',
     'fc51cd8e6218a1a38da47ed00230f0580816ed13ba3303ac5deb911548908025', 'af82',
     '6291d657deec24024827e69c3abe01a30ce548a284743a445e3680d7db5ac3ac18ff9b538d16f290ae67f760984dc6594a7c15e9716ed28dc027beceea1ec40a'),
    ('833fe62409237b9d62ec77587520911e9a759cec1d19755b7da901b96dca3d42',
     'ec172b93ad5e563bf4932c70e1245034c35467ef2efd4d64ebf819683467e2bf', hashlib.sha512(b'abc').hexdigest(),
     'dc2a4459e7369633a52b1bf277839a00201009a3efbf3ecb69bea2186c26b58909351fc9ac90b3ecfdfbc7c66431e0303dca179c138ac17ad9bef1177331a704'),
]

FILE = b'''format = "mncf/1"
name = "Example"

[game]
platform = "switch"
id = "0100000000ABC000"

[[cheat]]
id = "money"
name = "Money"

  [[cheat.write]]
  at = { region = "heap", offset = 0x10 }
  type = "u32"
  value = 5
'''


class Ed25519Test(unittest.TestCase):
    def test_rfc8032_vectors(self):
        for seed, pub, msg, sig in RFC8032:
            seed, pub, msg, sig = (bytes.fromhex(x) for x in (seed, pub, msg, sig))
            self.assertEqual(ed25519.public_key(seed), pub)
            self.assertEqual(ed25519.sign(seed, msg), sig)
            self.assertTrue(ed25519.verify(pub, msg, sig))

    def test_rejects_changes(self):
        seed, pub, msg, sig = (bytes.fromhex(x) for x in RFC8032[1])
        self.assertFalse(ed25519.verify(pub, msg + b'\0', sig))
        self.assertFalse(ed25519.verify(pub, msg, sig[:63] + bytes([sig[63] ^ 1])))
        self.assertFalse(ed25519.verify(pub, msg, sig[:5] + bytes([sig[5] ^ 0x10]) + sig[6:]))
        self.assertFalse(ed25519.verify(pub[:31], msg, sig))

    def test_rejects_non_canonical_s(self):
        seed, pub, msg, sig = (bytes.fromhex(x) for x in RFC8032[0])
        order = 2 ** 252 + 27742317777372353535851937790883648493
        s = int.from_bytes(sig[32:], 'little') + order
        self.assertFalse(ed25519.verify(pub, msg, sig[:32] + s.to_bytes(32, 'little')))

    def test_rejects_y_not_below_p(self):
        p = 2 ** 255 - 19
        self.assertIsNone(ed25519._decompress((p + 1).to_bytes(32, 'little')))


class SignatureTest(unittest.TestCase):
    seed = bytes.fromhex(RFC8032[0][0])

    def test_line_and_check(self):
        signed = FILE + signature.sign_line(self.seed, FILE)
        trust = mncf.TrustList()
        trust.trust('one', ed25519.public_key(self.seed))
        result = signature.check(signed, trust)
        self.assertEqual((result.result, result.signers), ('verified', ['one']))
        self.assertEqual(signature.check(signed).result, 'unknown-key')
        self.assertEqual(signature.check(FILE).result, 'unsigned')

    def test_split_needs_line_start(self):
        text = FILE + b'about = "no # mncf-signature: here"\n'
        self.assertEqual(signature.split(text), (text, [], True))

    def test_key_id(self):
        pub = bytes.fromhex(RFC8032[0][1])
        self.assertEqual(signature.key_id(pub), hashlib.sha256(pub).hexdigest()[:16])

    def test_signed_file_loads(self):
        signed = FILE + signature.sign_line(self.seed, FILE)
        f = mncf.load(signed)
        self.assertEqual(f.signed, FILE)
        self.assertEqual(f.signature.key_ids, [signature.key_id(ed25519.public_key(self.seed))])

    def test_non_canonical_base64(self):
        line = signature.sign_line(self.seed, FILE)
        sig64 = line.split(b' ')[4].rstrip(b'\n')
        last = sig64[-3:-2]
        alphabet = b'ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/'
        bumped = alphabet[(alphabet.index(last) ^ 1)].to_bytes(1, 'big')
        bad = line.replace(sig64, sig64[:-3] + bumped + b'==')
        trust = mncf.TrustList()
        trust.trust('one', ed25519.public_key(self.seed))
        self.assertEqual(signature.check(FILE + bad, trust).result, 'invalid')


class ValuesTest(unittest.TestCase):
    def test_patterns(self):
        self.assertEqual(values.parse_pattern('68 ?? 00 B9'), [0x68, None, 0, 0xB9])
        for bad in ('68 ?? 00', '?? 12 00 B9', '68 12 00 ??', '68 12 00 B9 ', '6812 00 B9', ''):
            self.assertIsNone(values.parse_pattern(bad), bad)

    def test_big_integers(self):
        self.assertEqual(values.integer('0xFFFFFFFF80000000'), 0xFFFFFFFF80000000)
        self.assertIsNone(values.integer('0x10000000000000000'))
        self.assertIsNone(values.integer('12'))
        self.assertIsNone(values.integer(True))

    def test_fits(self):
        self.assertTrue(values.fits('s8', -128))
        self.assertFalse(values.fits('s8', 128))
        self.assertFalse(values.fits('u8', True))
        self.assertFalse(values.fits('f32', 1e39))
        self.assertTrue(values.fits('f64', 1e300))

    def test_encode(self):
        self.assertEqual(values.encode('u32', 1, 'big'), b'\0\0\0\1')
        self.assertEqual(values.encode('s16', -1, 'little'), b'\xff\xff')
        self.assertEqual(values.encode('f32', 1.5, 'little'), bytes.fromhex('0000c03f'))


class EncodeTest(unittest.TestCase):
    def test_arm64_stubs(self):
        self.assertEqual(encode.arm64_stub('skip'), bytes.fromhex('c0035fd6'))
        self.assertEqual(encode.arm64_stub('return', 0), bytes.fromhex('000080d2c0035fd6'))
        self.assertEqual(len(encode.arm64_stub('return', 0x123456789ABCDEF0)), 20)
        self.assertEqual(encode.arm64_stub('return', -1), bytes.fromhex('00008092c0035fd6'))

    def test_x86_64_stubs(self):
        self.assertEqual(encode.x86_64_stub('return', 0xFFFFFFFF), bytes.fromhex('b8ffffffffc3'))
        self.assertEqual(encode.x86_64_stub('return', -(1 << 31)), bytes.fromhex('48c7c000000080c3'))
        self.assertEqual(encode.x86_64_stub('return', -(1 << 31) - 1)[:2], b'\x48\xb8')
        self.assertIsNone(encode.stub('mips', 'skip'))

    def test_fmov(self):
        self.assertEqual(encode.fmov_imm8(1.0), 0x70)
        self.assertEqual(encode.fmov_imm8(2.0), 0x00)
        self.assertEqual(encode.fmov_imm8(31.0), 0x3F)
        self.assertEqual(encode.fmov_imm8(0.125), 0x40)
        self.assertEqual(encode.fmov_imm8(-1.0), 0xF0)
        for v in (0.0, 0.1, 32.0, 1 / 16, float('nan'), True):
            self.assertIsNone(encode.fmov_imm8(v))

    def test_movz_range(self):
        self.assertIsNone(encode.arm64_imm('movz', 32, 0, 0x10000))
        self.assertIsNone(encode.arm64_imm('movz', 32, 0, -1))
        self.assertIsNone(encode.arm64_imm('movz-movk', 32, 0, 0x100000000))


class LoadTest(unittest.TestCase):
    def test_load_and_evaluate(self):
        f = mncf.load(FILE)
        self.assertEqual((f.name, f.game_id, f.platform.name), ('Example', '0100000000ABC000', 'switch'))
        rt = mncf.Runtime('switch', regions={'heap': (0x1000, 0x100)})
        ev = mncf.evaluate(f, rt, mncf.Choices(on=['money']))
        r = ev.cheat('money')
        self.assertEqual((r.state, r.uses, r.ops[0].address, r.ops[0].bytes), ('on', 'core', 0x1010, b'\5\0\0\0'))

    def test_errors(self):
        for data, code in ((b'\xef\xbb\xbf' + FILE, 'encoding'), (FILE.replace(b'\n', b'\r\n'), 'encoding'),
                           (b'format = ', 'toml'), (FILE.replace(b'mncf/1', b'mncf/9'), 'format')):
            with self.assertRaises(mncf.MncfError) as e:
                mncf.load(data)
            self.assertEqual(e.exception.code, code)

    def test_find_all_overlapping_and_sections(self):
        data = bytes(4) + b'\xAA' * 5 + bytes(7) + b'\xBB' * 4 + bytes(12)
        m = mncf.Module('00', 0, data, sections={'code': (0, 18), 'rodata': (18, 32)})
        self.assertEqual(mncf.find_all(m, [0xAA] * 4), [4, 5])
        self.assertEqual(mncf.find_all(m, [0xAA] * 4, 'rodata'), [])
        self.assertEqual(mncf.find_all(m, [0xAA, None, 0xAA, 0xAA], limit=1), [4])
        self.assertEqual(mncf.find_all(m, [0xBB] * 4), [], 'a match must lie inside one section')

    def test_identify_prefers_runtime_build(self):
        f = mncf.load(FILE.replace(b'[[cheat]]', b'[builds."1.0"]\nmodules = { main = "AB" }\n\n[[cheat]]'))
        self.assertEqual(mncf.identify(f, mncf.Runtime('switch', modules={'main': mncf.Module('abcd')})), '1.0')
        self.assertIsNone(mncf.identify(f, mncf.Runtime('switch', build='2.0')))


class PackTest(unittest.TestCase):
    def zipped(self, entries):
        buf = io.BytesIO()
        with zipfile.ZipFile(buf, 'w') as z:
            for name, data in entries.items():
                z.writestr(name, data)
        return buf.getvalue()

    def test_paths(self):
        for good in ('a.mncf', 'games/a.mncf'):
            self.assertTrue(valid_path(good))
        for bad in ('', '/a.mncf', 'a/../b.mncf', './a.mncf', 'a\\b.mncf', 'a//b.mncf'):
            self.assertFalse(valid_path(bad), bad)

    def test_read(self):
        digest = hashlib.sha256(FILE).hexdigest()
        manifest = f'format = "mncf-pack/1"\nname = "P"\n\n[[file]]\npath = "a.mncf"\nsha256 = "{digest}"\n'.encode()
        pack = read_pack(self.zipped({'pack.toml': manifest, 'a.mncf': FILE}))
        self.assertTrue(pack.usable)
        self.assertEqual([(f.path, f.status, f.data) for f in pack.files], [('a.mncf', 'ok', FILE)])
        seed = bytes.fromhex(RFC8032[0][0])
        signed = manifest + signature.sign_line(seed, manifest)
        trust = mncf.TrustList()
        trust.trust('one', ed25519.public_key(seed))
        self.assertEqual(read_pack(self.zipped({'pack.toml': signed, 'a.mncf': FILE}), trust).signature.result,
                         'verified')
        changed = signed.replace(b'name = "P"', b'name = "Q"')
        self.assertFalse(read_pack(self.zipped({'pack.toml': changed, 'a.mncf': FILE}), trust).usable)

    def test_duplicate_entries(self):
        buf = io.BytesIO()
        with zipfile.ZipFile(buf, 'w') as z:
            z.writestr('pack.toml', b'x')
            with self.assertWarns(UserWarning):
                z.writestr('pack.toml', b'y')
        with self.assertRaises(mncf.MncfError) as e:
            read_pack(buf.getvalue())
        self.assertEqual(e.exception.code, 'zip')

    def test_trust_list_needs_32_bytes(self):
        with self.assertRaises(ValueError):
            mncf.TrustList().trust('x', base64.b64decode('AAAA'))



class LimitsTest(unittest.TestCase):
    def zipped(self, entries, method=zipfile.ZIP_DEFLATED):
        buf = io.BytesIO()
        with zipfile.ZipFile(buf, 'w', method) as z:
            for name, data in entries:
                z.writestr(name, data)
        return buf.getvalue()

    def code(self, call):
        with self.assertRaises(mncf.MncfError) as e:
            call()
        return e.exception.code

    def test_file_size(self):
        big = FILE + b'#' * (mncf.MAX_FILE - len(FILE)) + b'\n'
        self.assertEqual(self.code(lambda: mncf.load(big)), 'size')

    def test_pack_entry_that_inflates_past_the_limit(self):
        bomb = bytes(mncf.MAX_FILE + 1)              # zeros deflate to a few kilobytes
        pack = self.zipped([('pack.toml', bomb)])
        self.assertLess(len(pack), 100_000)
        self.assertEqual(self.code(lambda: read_pack(pack)), 'size')

    def test_pack_entries_and_total(self):
        many = self.zipped([(f'f{i}.mncf', b'') for i in range(4097)] + [('pack.toml', b'')], zipfile.ZIP_STORED)
        self.assertEqual(self.code(lambda: read_pack(many)), 'size')
        import mncf.pack as pack_module
        digest = hashlib.sha256(FILE).hexdigest()
        manifest = ('format = "mncf-pack/1"\nname = "P"\n' + ''.join(
            f'\n[[file]]\npath = "f{i}.mncf"\nsha256 = "{digest}"\n' for i in range(3))).encode()
        pack = self.zipped([('pack.toml', manifest)] + [(f'f{i}.mncf', FILE) for i in range(3)])
        saved = pack_module.MAX_TOTAL
        pack_module.MAX_TOTAL = len(manifest) + 2 * len(FILE)
        try:
            self.assertEqual(self.code(lambda: read_pack(pack)), 'size')
        finally:
            pack_module.MAX_TOTAL = saved
        self.assertEqual([f.status for f in read_pack(pack).files], ['ok', 'ok', 'ok'])


class TrustListTest(unittest.TestCase):
    pub = bytes.fromhex(RFC8032[0][1])

    def text(self, body):
        return 'format = "mncf-keys/1"\n' + body

    def test_read(self):
        b64 = base64.b64encode(self.pub).decode()
        trust = mncf.read_trust_list(self.text(f'\n[[key]]\nname = "One"\npublic = "{b64}"\nid = "{mncf.key_id(self.pub)}"\n'
                                               '\n[[revoked]]\nid = "39f713d0a644253f"\n'))
        self.assertEqual(trust.keys(mncf.key_id(self.pub)), [('One', self.pub)])
        self.assertEqual(trust.revoked, {'39f713d0a644253f'})
        self.assertEqual(mncf.read_trust_list(self.text('')).revoked, set())

    def test_errors(self):
        b64 = base64.b64encode(self.pub).decode()
        for text, code in ((f'[[key]]\nname = "One"\npublic = "{b64}"\n', 'format'),
                           (self.text(f'\n[[key]]\nname = "One"\npublic = "{b64[:-2]}"\n'), 'schema:key'),
                           (self.text(f'\n[[key]]\npublic = "{b64}"\n'), 'schema:key'),
                           (self.text(f'\n[[key]]\nname = "One"\npublic = "{b64}"\nid = "0000000000000000"\n'),
                            'schema:key'),
                           (self.text('\n[[revoked]]\nid = "ABC"\n'), 'schema:revoked')):
            with self.assertRaises(mncf.MncfError) as e:
                mncf.read_trust_list(text)
            self.assertEqual(e.exception.code, code, text)


if __name__ == '__main__':
    unittest.main()
