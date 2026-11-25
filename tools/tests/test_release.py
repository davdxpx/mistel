"""Tests of mistel release, mistel update-check and the update keys of mistel pack and sign, on made-up files only."""
import contextlib
import io
import pathlib
import sys
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tools'))

from mistel import cli, fetch, keys, release  # noqa: E402  (finds the mncf library of this checkout)
import mncf  # noqa: E402
from mncf import ed25519, update  # noqa: E402
from mncf.pack import read_pack  # noqa: E402

KEY_ONE = bytes.fromhex('9d61b19deffd5a60ba844af492ec2cc44449c5697b326919703bac031cae7f60')
KEY_TWO = bytes.fromhex('4ccd089b28ff96da9db6c346ec114e0f5b8a319f35aba624da8cf6ed4fb8a6fb')

V1 = '''format = "mncf/1"
name = "Example cheats"
# The ids stay the same from version to version.
id = "example.org/someone/example"
file_version = "1"

[update]
url = "https://example.org/someone/example.mncf"

[game]
platform = "switch"
id = "0100000000ABC000"

[[cheat]]
id = "money"
name = "Money"

  [[cheat.write]]
  at = { region = "heap", offset = 0x10 }
  type = "u32"
  value = 999

[[cheat]]
id = "max-iv"
name = "Best values"
about = """
[Not a table]
Kept as it is.
"""

  [[cheat.write]]
  at = { region = "heap", offset = 0x20 }
  type = "u8"
  value = 31

[[cheat]]
id = "speed"
name = "Speed"

  [[cheat.write]]
  at = { region = "heap", offset = 0x30 }
  type = "u8"
  value = 2
'''

V2_CHEATS = V1.replace('id = "max-iv"', 'id = "max-ivs"').replace('''[[cheat]]
id = "speed"
name = "Speed"

  [[cheat.write]]
  at = { region = "heap", offset = 0x30 }
  type = "u8"
  value = 2
''', '''[[cheat]]
id = "shiny-odds"
name = "Better odds"

  [[cheat.write]]
  at = { region = "heap", offset = 0x50 }
  type = "u8"
  value = 1
''')


def run_cli(*argv):
    out = io.StringIO()
    with contextlib.redirect_stdout(out), contextlib.redirect_stderr(out):
        code = cli.main([str(a) for a in argv])
    return code, out.getvalue()


@contextlib.contextmanager
def keys_in(folder):
    saved = keys.repository_of
    keys.repository_of = lambda path: None
    try:
        one, _ = keys.keygen('Key one', folder / 'one', seed=KEY_ONE)
        two, _ = keys.keygen('Key two', folder / 'two', seed=KEY_TWO)
        yield one, two
    finally:
        keys.repository_of = saved


class TextTest(unittest.TestCase):
    def test_header_inside_a_string_is_no_header(self):
        t = release.Text('a = """\n[x]\n"""\n\n[game]\nb = 1\n')
        self.assertEqual(t.first_header(), 4)

    def test_set_top_keeps_the_rest(self):
        t = release.Text('format = "mncf/1"\nname = "N"  # a comment\n\n[game]\nid = "x"\n')
        t.set_top('file_version', '"2"', after=('id', 'name'))
        t.set_top('id', '"example.org/n"', after=('name',))
        self.assertEqual(t.text, 'format = "mncf/1"\nname = "N"  # a comment\nid = "example.org/n"\n'
                                 'file_version = "2"\n\n[game]\nid = "x"\n')

    def test_next_version(self):
        self.assertEqual(release.next_version('4'), '5')
        self.assertEqual(release.next_version('2.9'), '2.10')
        self.assertEqual(release.next_version(None), '1')
        self.assertEqual(release.next_version('v3'), '1')


class ReleaseTest(unittest.TestCase):
    def test_entry_from_the_ids(self):
        with tempfile.TemporaryDirectory() as d:
            d = pathlib.Path(d)
            (d / 'v1.mncf').write_bytes(V1.encode())
            (d / 'example.mncf').write_bytes(V2_CHEATS.encode())
            code, out = run_cli('release', d / 'example.mncf', '--previous', d / 'v1.mncf', '--text',
                                'Fits game version 1.0.3.', '--date', '2026-12-02', '--renamed', 'max-iv=max-ivs')
            self.assertEqual(code, 0, out)
            self.assertIn('version 2', out)
            text = (d / 'example.mncf').read_text(encoding='utf-8')
            self.assertIn('# The ids stay the same from version to version.\n', text)
            self.assertIn('[Not a table]\n', text)
            self.assertIn('[[changes]]\nversion = "2"\ndate = "2026-12-02"\ntext = "Fits game version 1.0.3."\n'
                          'added = ["shiny-odds"]\nremoved = ["speed"]\nrenamed = { max-iv = "max-ivs" }\n\n[game]',
                          text)
            f = mncf.load(text.encode())
            self.assertEqual(f.warnings, [])
            self.assertEqual(f.file_version, '2')
            check = update.check(mncf.load(V1.encode()), text.encode())
            self.assertTrue(check.ok, check.reason)
            carried = update.carry(f, '1', mncf.Choices(on=['max-iv', 'speed']))
            self.assertEqual(carried.choices.on, ['max-ivs'])

    def test_rename_hint(self):
        with tempfile.TemporaryDirectory() as d:
            d = pathlib.Path(d)
            (d / 'v1.mncf').write_bytes(V1.encode())
            (d / 'example.mncf').write_bytes(V2_CHEATS.encode())
            code, out = run_cli('release', d / 'example.mncf', '--previous', d / 'v1.mncf', '--date', '2026-12-02')
            self.assertEqual(code, 0, out)
            self.assertIn('--renamed max-iv=max-ivs', out)
            self.assertIn('the entry has no text', out)

    def test_bad_renames_and_versions(self):
        with tempfile.TemporaryDirectory() as d:
            d = pathlib.Path(d)
            (d / 'v1.mncf').write_bytes(V1.encode())
            (d / 'example.mncf').write_bytes(V2_CHEATS.encode())
            code, out = run_cli('release', d / 'example.mncf', '--previous', d / 'v1.mncf', '--renamed', 'x=y')
            self.assertEqual(code, 1, out)
            self.assertIn('the previous version has no', out)
            code, out = run_cli('release', d / 'example.mncf', '--previous', d / 'v1.mncf', '--version', '1.0')
            self.assertEqual(code, 1, out)
            self.assertIn('not above the previous version 1', out)
            self.assertEqual((d / 'example.mncf').read_bytes(), V2_CHEATS.encode())

    def test_first_release_sets_id_and_address(self):
        plain = V1.replace('id = "example.org/someone/example"\nfile_version = "1"\n', '').replace(
            '[update]\nurl = "https://example.org/someone/example.mncf"\n\n', '')
        with tempfile.TemporaryDirectory() as d:
            d = pathlib.Path(d)
            (d / 'example.mncf').write_bytes(plain.encode())
            code, out = run_cli('release', d / 'example.mncf', '--id', 'example.org/someone/example', '--url',
                                'https://example.org/someone/example.mncf', '--text', 'First.', '--date', '2026-12-02')
            self.assertEqual(code, 0, out)
            f = mncf.load((d / 'example.mncf').read_bytes())
            self.assertEqual((f.id, f.file_version, f.update.host), ('example.org/someone/example', '1', 'example.org'))
            self.assertEqual([r.text for r in f.changes], ['First.'])
            code, out = run_cli('release', d / 'example.mncf', '--url', 'http://example.org/x.mncf')
            self.assertEqual(code, 1, out)

    def test_an_entry_the_author_wrote_is_kept(self):
        own = V2_CHEATS.replace('file_version = "1"', 'file_version = "2"').replace(
            '[game]', '[[changes]]\nversion = "2"\ntext = "My own words."\nadded = ["shiny-odds"]\nremoved = ["speed"]\n'
                      'renamed = { max-iv = "max-ivs" }\n\n[game]')
        with tempfile.TemporaryDirectory() as d:
            d = pathlib.Path(d)
            (d / 'v1.mncf').write_bytes(V1.encode())
            (d / 'example.mncf').write_bytes(own.encode())
            code, out = run_cli('release', d / 'example.mncf', '--previous', d / 'v1.mncf')
            self.assertEqual(code, 0, out)
            self.assertIn('kept as it is', out)
            self.assertNotIn('lists differ', out)
            self.assertEqual((d / 'example.mncf').read_bytes(), own.encode())

    def test_signers_and_key_change(self):
        with tempfile.TemporaryDirectory() as d:
            d = pathlib.Path(d)
            with keys_in(d / 'keys') as (one, two):
                (d / 'v1.mncf').write_bytes(V1.encode())
                code, out = run_cli('release', d / 'v1.mncf', '--version', '1', '--sign', one, '--force')
                self.assertEqual(code, 0, out)
                v1 = mncf.load((d / 'v1.mncf').read_bytes())
                self.assertEqual(v1.signers, [ed25519.public_key(KEY_ONE)])
                self.assertEqual(v1.signature.result, 'unknown-key')
                self.assertEqual(list(v1.signature.verified.values()), [ed25519.public_key(KEY_ONE)])

                (d / 'v2.mncf').write_bytes(V2_CHEATS.encode())
                code, out = run_cli('release', d / 'v2.mncf', '--previous', d / 'v1.mncf', '--renamed',
                                    'max-iv=max-ivs', '--sign', two)
                self.assertEqual(code, 1, out)
                self.assertIn('update:signer', out)
                self.assertIn('sign one version with the old key and the new one', out)
                code, out = run_cli('release', d / 'v2.mncf', '--previous', d / 'v1.mncf', '--renamed',
                                    'max-iv=max-ivs', '--sign', one, '--sign', two)
                self.assertEqual(code, 0, out)
                v2 = mncf.load((d / 'v2.mncf').read_bytes())
                self.assertEqual(len(v2.signature.verified), 2)

                (d / 'v3.mncf').write_bytes((d / 'v2.mncf').read_bytes())
                code, out = run_cli('release', d / 'v3.mncf', '--previous', d / 'v2.mncf', '--sign', two, '--text',
                                    'Signed with the new key only.')
                self.assertEqual(code, 0, out)
                v3 = mncf.load((d / 'v3.mncf').read_bytes())
                self.assertEqual(v3.file_version, '3')
                self.assertTrue(update.check(v2, (d / 'v3.mncf').read_bytes()).ok)
                self.assertEqual(update.check(v1, (d / 'v3.mncf').read_bytes()).reason, 'update:signer')

    def test_sign_lists_the_key(self):
        with tempfile.TemporaryDirectory() as d:
            d = pathlib.Path(d)
            with keys_in(d / 'keys') as (one, two):
                (d / 'v1.mncf').write_bytes(V1.encode())
                code, out = run_cli('sign', d / 'v1.mncf', '--key', one)
                self.assertEqual(code, 0, out)
                code, out = run_cli('sign', d / 'v1.mncf', '--key', two)
                self.assertEqual(code, 0, out)
                self.assertIn('is not in the signers', out)
                f = mncf.load((d / 'v1.mncf').read_bytes())
                self.assertEqual(f.signers, [ed25519.public_key(KEY_ONE)])
                code, out = run_cli('verify', d / 'v1.mncf')
                self.assertEqual(code, 0, out)
                self.assertIn('checked with the keys in its signers', out)


class PackTest(unittest.TestCase):
    def test_pack_versions(self):
        with tempfile.TemporaryDirectory() as d:
            d = pathlib.Path(d)
            with keys_in(d / 'keys') as (one, _):
                (d / 'a.mncf').write_bytes(V1.encode())
                (d / 'b.mncf').write_bytes(V1.replace('example.org/someone/example', 'example.org/someone/b').encode())
                code, out = run_cli('pack', d / 'v1.mncp', d / 'a.mncf', d / 'b.mncf', '--name', 'Example pack',
                                    '--id', 'example.org/someone/pack', '--url', 'https://example.org/pack.mncp',
                                    '--file-version', '1', '--text', 'First.', '--date', '2026-12-01', '--sign', one)
                self.assertEqual(code, 0, out)
                (d / 'c.mncf').write_bytes(V1.replace('example.org/someone/example', 'example.org/someone/c').encode())
                code, out = run_cli('pack', d / 'v2.mncp', d / 'a.mncf', d / 'c.mncf', '--name', 'Example pack',
                                    '--previous', d / 'v1.mncp', '--text', 'C instead of B.', '--date', '2026-12-02',
                                    '--sign', one)
                self.assertEqual(code, 0, out)
                self.assertIn('version 2', out)
                v1, v2 = (read_pack((d / f'v{n}.mncp').read_bytes()) for n in (1, 2))
                self.assertEqual((v2.id, v2.update.host), ('example.org/someone/pack', 'example.org'))
                self.assertEqual([(r.version, r.added, r.removed) for r in v2.changes],
                                 [('2', ['c.mncf'], ['b.mncf']), ('1', [], [])])
                self.assertTrue(update.check_pack(v1, (d / 'v2.mncp').read_bytes()).ok)
                self.assertEqual(update.pairs(v1, v2), {'a.mncf': 'a.mncf', 'b.mncf': None})
                code, out = run_cli('pack', d / 'v3.mncp', d / 'a.mncf', '--name', 'Example pack', '--previous',
                                    d / 'v2.mncp')
                self.assertEqual(code, 1, out)
                self.assertIn('update:signer', out)


def answers(table):
    """A stand-in for one HTTPS request: the answers by address."""
    asked = []

    def get(url, limit, timeout, connect):
        asked.append(url)
        status, location, data = table[url]
        return status, location, data
    return get, asked


class FetchTest(unittest.TestCase):
    def test_redirects(self):
        get, asked = answers({'https://example.org/x.mncf': (302, 'https://cdn.example.net/x.mncf?sig=1', b''),
                              'https://cdn.example.net/x.mncf?sig=1': (200, None, b'data')})
        got = fetch.fetch('https://example.org/x.mncf', get=get, connect=lambda h, p: None)
        self.assertEqual((got.data, got.url), (b'data', 'https://cdn.example.net/x.mncf?sig=1'))
        self.assertEqual(asked, ['https://example.org/x.mncf', 'https://cdn.example.net/x.mncf?sig=1'])

    def test_refusals(self):
        get, _ = answers({'https://example.org/x.mncf': (302, 'http://example.org/x.mncf', b'')})
        with self.assertRaises(fetch.FetchError):
            fetch.fetch('https://example.org/x.mncf', get=get, connect=lambda h, p: None)
        loop = {f'https://example.org/{n}': (302, f'/{n + 1}', b'') for n in range(10)}
        get, asked = answers(loop)
        with self.assertRaises(fetch.FetchError):
            fetch.fetch('https://example.org/0', get=get, connect=lambda h, p: None)
        self.assertEqual(len(asked), 6)
        get, _ = answers({'https://example.org/x.mncf': (404, None, b'')})
        with self.assertRaises(fetch.FetchError):
            fetch.fetch('https://example.org/x.mncf', get=get, connect=lambda h, p: None)
        with self.assertRaises(fetch.FetchError):
            fetch.fetch('http://example.org/x.mncf')
        with self.assertRaises(fetch.FetchError):
            fetch._public('localhost', 443)

    def test_update_check(self):
        with tempfile.TemporaryDirectory() as d:
            d = pathlib.Path(d)
            (d / 'v1.mncf').write_bytes(V1.encode())
            (d / 'v2.mncf').write_bytes(V2_CHEATS.encode())
            code, out = run_cli('release', d / 'v2.mncf', '--previous', d / 'v1.mncf', '--renamed', 'max-iv=max-ivs',
                                '--text', 'Fits game version 1.0.3.', '--date', '2026-12-02')
            self.assertEqual(code, 0, out)
            saved = fetch._get, fetch._public
            fetch._get = answers({'https://example.org/someone/example.mncf':
                                  (200, None, (d / 'v2.mncf').read_bytes())})[0]
            fetch._public = lambda h, p: None
            try:
                code, out = run_cli('update-check', d / 'v1.mncf')
                self.assertEqual(code, 0, out)
                self.assertIn('version 2 at the address, 1 here: newer', out)
                self.assertIn('2 (2026-12-02): Fits game version 1.0.3.', out)
                self.assertIn('new: shiny-odds', out)
                code, out = run_cli('update-check', d / 'v2.mncf')
                self.assertEqual(code, 0, out)
                self.assertIn('the same version', out)
            finally:
                fetch._get, fetch._public = saved
            other = V1.replace('example.org/someone/example', 'example.org/someone/other').replace(
                'file_version = "1"', 'file_version = "9"')
            (d / 'other.mncf').write_bytes(other.encode())
            code, out = run_cli('update-check', d / 'v1.mncf', '--new', d / 'other.mncf')
            self.assertEqual(code, 1, out)
            self.assertIn('update:id', out)


if __name__ == '__main__':
    unittest.main()
