"""Unit tests of updates (spec, section 17): the parts the conformance suite does not reach on its own."""
import base64
import pathlib
import sys
import unittest

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1]))

import mncf  # noqa: E402
from mncf import ed25519, signature, update, write  # noqa: E402

SUITE = pathlib.Path(__file__).resolve().parents[3] / 'tests' / 'updates' / 'files'
KEY_ONE = bytes.fromhex('9d61b19deffd5a60ba844af492ec2cc44449c5697b326919703bac031cae7f60')


class VersionTest(unittest.TestCase):
    def test_compare_needs_versions(self):
        with self.assertRaises(ValueError):
            update.compare('v3', '3')

    def test_above(self):
        self.assertTrue(update.above('1', None))
        self.assertTrue(update.above('1', 'v3'))
        self.assertFalse(update.above('v4', '3'))
        self.assertFalse(update.above('3.0', '3'))
        self.assertTrue(update.above('3.0.1', '3'))


class KeysTest(unittest.TestCase):
    def test_round_trip(self):
        data = (SUITE / 'v5.mncf').read_bytes()
        f = mncf.load(data)
        self.assertEqual(f.warnings, [])
        self.assertEqual(f.id, 'example.org/someone/update-example')
        self.assertEqual(f.update.host, 'example.org')
        self.assertEqual([r.version for r in f.changes], ['5', '4', '3'])
        again = mncf.load(write.write_file(f).encode())
        self.assertEqual(write.write_file(again), write.write_file(f))
        self.assertEqual([r.version for r in again.changes], ['5', '4', '3'])

    def test_layout(self):
        text = write.dumps({'format': 'mncf/1', 'name': 'N', 'game': {'platform': 'switch', 'id': '0100000000ABC000'},
                            'changes': [{'renamed': {'a': 'b'}, 'version': '2'}], 'file_version': '2',
                            'update': {'url': 'https://example.org/x.mncf'}, 'id': 'example.org/x',
                            'cheat': [{'id': 'b', 'name': 'B', 'write': [
                                {'at': {'region': 'heap', 'offset': 0}, 'type': 'u8', 'value': 1}]}]})
        self.assertEqual(text.split('\n')[:12], [
            'format = "mncf/1"', 'name = "N"', 'id = "example.org/x"', 'file_version = "2"', '',
            '[update]', 'url = "https://example.org/x.mncf"', '', '[[changes]]', 'version = "2"',
            'renamed = { a = "b" }', ''])

    def test_verified_keys(self):
        f = mncf.load((SUITE / 'v5-one.mncf').read_bytes())
        public = ed25519.public_key(KEY_ONE)
        self.assertEqual(f.signature.result, 'unknown-key')
        self.assertEqual(f.signers, [public])
        self.assertEqual(f.signature.verified, {signature.key_id(public): public})
        bare = mncf.load((SUITE / 'v5-one-bare.mncf').read_bytes())
        self.assertEqual(bare.signature.verified, {})
        self.assertEqual(bare.signature.key_ids, [signature.key_id(public)])

    def test_pack_keys(self):
        from mncf.pack import read_pack
        import io
        import zipfile
        buf = io.BytesIO()
        with zipfile.ZipFile(buf, 'w') as z:
            z.writestr('pack.toml', (SUITE / 'pack-v2-one.toml').read_bytes())
            for name, src in (('a-new.mncf', 'pa.mncf'), ('b.mncf', 'pb.mncf'), ('d.mncf', 'pd.mncf')):
                z.writestr(name, (SUITE / src).read_bytes())
        pack = read_pack(buf.getvalue())
        self.assertEqual(pack.warnings, [])
        self.assertEqual(pack.id, 'example.org/someone/update-pack')
        self.assertEqual(pack.changes[0].renamed, {'old/b.mncf': 'b.mncf', 'a.mncf': 'a-new.mncf'})
        self.assertEqual(pack.signers, [ed25519.public_key(KEY_ONE)])
        self.assertEqual(pack.signature.result, 'unknown-key')


class CarryTest(unittest.TestCase):
    def test_kept_and_host_data(self):
        new = mncf.load((SUITE / 'v5.mncf').read_bytes())
        c = update.carry(new, '3', mncf.Choices(), kept=['max-iv', 'speed', 'money'])
        self.assertEqual(c.ids, {'max-iv': 'max-ivs', 'speed': None, 'money': 'money'})
        self.assertEqual(c.dropped, ['speed'])

    def test_preset_kept(self):
        new = mncf.load((SUITE / 'v5.mncf').read_bytes())
        self.assertEqual(update.carry(new, '3', mncf.Choices(), 'relaxed').preset, 'relaxed')
        self.assertIsNone(update.carry(new, '3', mncf.Choices(), 'race').preset)

    def test_float_values(self):
        text = (b'format = "mncf/1"\nname = "F"\nid = "example.org/f"\nfile_version = "2"\n\n[game]\n'
                b'platform = "switch"\nid = "0100000000ABC000"\n\n[[cheat]]\nid = "speed"\nname = "Speed"\n\n'
                b'  [cheat.values.x]\n  type = "f32"\n  min = 0.5\n  max = 4.0\n  default = 1.0\n\n'
                b'  [[cheat.write]]\n  at = { region = "heap", offset = 0x10 }\n  type = "f32"\n  value = "$x"\n')
        new = mncf.load(text)
        c = update.carry(new, '1', mncf.Choices(values={'speed.x': 2, 'speed.y': 1}))
        self.assertEqual(c.choices.values, {'speed.x': 2})
        self.assertEqual(c.gone, ['speed.y'])
        self.assertEqual(update.carry(new, '1', mncf.Choices(values={'speed.x': True})).reset, {'speed.x': 'type'})
        self.assertEqual(update.carry(new, '1', mncf.Choices(values={'speed.x': float('nan')})).reset,
                         {'speed.x': 'type'})


class CheckTest(unittest.TestCase):
    def test_check_keeps_the_new_file(self):
        old = mncf.load((SUITE / 'v3.mncf').read_bytes())
        check = update.check(old, (SUITE / 'v5.mncf').read_bytes())
        self.assertTrue(check.ok)
        self.assertEqual(check.file.file_version, '5')
        self.assertEqual((check.from_version, check.to_version), ('3', '5'))
        refused = update.check(old, (SUITE / 'v3.mncf').read_bytes())
        self.assertFalse(refused.ok)
        self.assertIsNotNone(refused.file)

    def test_revoked_installed_key(self):
        trust = mncf.TrustList()
        trust.revoke(signature.key_id(ed25519.public_key(KEY_ONE)))
        old = mncf.load((SUITE / 'v3-one.mncf').read_bytes(), trust)
        self.assertEqual(old.signature.result, 'revoked')
        check = update.check(old, (SUITE / 'v5-one.mncf').read_bytes(), trust)
        self.assertEqual(check.reason, 'update:signature')

    def test_signers_order_does_not_matter(self):
        text = (SUITE / 'v5.mncf').read_bytes()
        public = base64.b64encode(ed25519.public_key(KEY_ONE)).decode()
        doc = text.replace(b'file_version = "5"\n', f'file_version = "5"\nsigners = ["{public}"]\n'.encode())
        signed = doc + signature.sign_line(KEY_ONE, doc)
        old = mncf.load((SUITE / 'v3-one.mncf').read_bytes())
        self.assertTrue(update.check(old, signed).ok)


if __name__ == '__main__':
    unittest.main()
