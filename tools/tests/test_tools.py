"""Tests of the mistel command: check, convert, pattern, keygen, sign, verify and pack, on made-up files only."""
import contextlib
import io
import pathlib
import sys
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tools'))
sys.path.insert(0, str(ROOT / 'format' / 'lib' / 'python' / 'tests'))

import synthetic as syn  # noqa: E402
import mncf  # noqa: E402
from mistel import cli, keys  # noqa: E402
from mncf import convert, disasm  # noqa: E402
from mncf.module import parse_nso  # noqa: E402

SEED = bytes.fromhex('9d61b19deffd5a60ba844af492ec2cc44449c5697b326919703bac031cae7f60')


def module():
    return parse_nso(syn.sample_nso())


@contextlib.contextmanager
def outside_repositories():
    """A temporary folder can lie in a git work tree, such as a home folder kept in git; the tests that write keys
    there act as if it did not."""
    saved = keys.repository_of
    keys.repository_of = lambda path: None
    try:
        yield
    finally:
        keys.repository_of = saved



class KeysTest(unittest.TestCase):
    def test_keygen_outside_repositories(self):
        with self.assertRaises(keys.KeyFileError):
            keys.keygen('Test', keys.TOOLS / 'never-here')
        with tempfile.TemporaryDirectory() as d:
            (pathlib.Path(d) / 'repo' / '.git').mkdir(parents=True)
            with self.assertRaises(keys.KeyFileError):
                keys.keygen('Test', pathlib.Path(d) / 'repo' / 'keys')
            with outside_repositories():
                secret, public = keys.keygen('Test key', pathlib.Path(d) / 'keys', seed=SEED)
            self.assertEqual(keys.read_secret(secret), ('Test key', SEED))
            self.assertEqual(keys.read_public(public)[1], mncf.ed25519.public_key(SEED))
            with self.assertRaises(keys.KeyFileError):
                keys.keygen('Test key', pathlib.Path(d) / 'keys')

    def test_sign_verify_pack(self):
        text = convert.from_atmosphere(convert.Source(syn.BUILD_ID, syn.CHEATS, module=module()),
                                       game_id=syn.GAME_ID).text
        with tempfile.TemporaryDirectory() as d:
            d = pathlib.Path(d)
            with outside_repositories():
                _, public = keys.keygen('Test key', d / 'keys', seed=SEED)
            path = d / 'example.mncf'
            path.write_text(text, encoding='utf-8', newline='\n')
            keys.sign_file(path, SEED)
            keys.sign_file(path, bytes(32))
            trust = keys.trust_list([public])
            ok, lines = keys.verify(path, trust)
            self.assertTrue(ok)
            self.assertIn('verified by Test key', lines[0])
            self.assertEqual(mncf.load(path.read_bytes()).signed, text.encode())
            pack = keys.make_pack([('example.mncf', path.read_bytes())], 'Example pack', seed=SEED)
            (d / 'example.mncp').write_bytes(pack)
            ok, lines = keys.verify(d / 'example.mncp', trust)
            self.assertTrue(ok, lines)
            self.assertTrue(lines[0].startswith('pack.toml: verified'))
            keys.sign_file(d / 'example.mncp', bytes(32))
            ok, lines = keys.verify(d / 'example.mncp', trust)
            self.assertTrue(ok, lines)
            tampered = path.read_bytes().replace(b'name = "Converted cheats"', b'name = "Changed"')
            self.assertNotEqual(tampered, path.read_bytes())
            (d / 'changed.mncf').write_bytes(tampered)
            self.assertEqual(keys.verify(d / 'changed.mncf', trust)[0], False)

    def test_refuses(self):
        with self.assertRaises(keys.KeyFileError):
            keys.sign_bytes(b'format = "mncf/1"', SEED)
        with self.assertRaises(keys.KeyFileError):
            keys.make_pack([('../x.mncf', b'')], 'x')


class CliTest(unittest.TestCase):
    def run_cli(self, *argv):
        out = io.StringIO()
        with contextlib.redirect_stdout(out), contextlib.redirect_stderr(out):
            code = cli.main(list(argv))
        return code, out.getvalue()

    def test_commands(self):
        with tempfile.TemporaryDirectory() as d:
            d = pathlib.Path(d)
            nso = d / 'main.nso'
            nso.write_bytes(syn.sample_nso())
            cheats = d / syn.GAME_ID / 'cheats' / f'{syn.BUILD_ID}.txt'
            cheats.parent.mkdir(parents=True)
            cheats.write_text(syn.CHEATS, encoding='utf-8', newline='\n')
            code, out = self.run_cli('convert', str(cheats), '--module', str(nso), '--version',
                                     f'{syn.BUILD_ID}=1.0.0', '-v')
            self.assertEqual(code, 0, out)
            self.assertIn('restore entries dropped', out)
            mncf_path = cheats.with_suffix('.mncf')
            code, out = self.run_cli('check', str(mncf_path), '--module', str(nso))
            self.assertEqual(code, 0, out)
            self.assertIn('build: 1.0.0', out)
            changed = d / 'other.nso'
            text, ro, data = syn.sample_code()
            patched = text[:syn.STR_W8] + syn.words(0xB9001269) + text[syn.STR_W8 + 4:]
            changed.write_bytes(syn.make_nso(patched, ro, data, 0x400))
            code, out = self.run_cli('check', str(mncf_path), '--module', str(changed))
            self.assertEqual(code, 1, out)                       # patterns are made from the original bytes
            self.assertIn('fail money: pattern-not-found', out)
            cave = text + bytes(0x10) + syn.words(0x11111111)
            changed.write_bytes(syn.make_nso(cave, ro, data, 0x400))
            code, out = self.run_cli('check', str(mncf_path), '--module', str(changed))
            self.assertEqual(code, 1, out)
            self.assertIn('fail master-setup: expect-mismatch', out)
            # Only the cheat that stays raw code depends on the master; money keeps its pattern and works.
            self.assertIn('fail loop: requires:master-setup', out)
            self.assertNotIn('fail money', out)
            code, out = self.run_cli('convert', str(mncf_path), '--build', '1.0.0', '-o', str(d / 'back.txt'))
            self.assertEqual(code, 0, out)
            same, differ, missing = convert.compare(syn.CHEATS, (d / 'back.txt').read_text(encoding='utf-8'))
            self.assertEqual((differ, missing), ([], ['Money off']))
            code, out = self.run_cli('pattern', str(nso), hex(syn.BL))
            self.assertEqual(code, 0, out)
            self.assertIn('find = "68 12 00 B9 ?? ?? ?? 94"', out)
            self.assertIn('offset = 4', out)
            with outside_repositories():
                code, out = self.run_cli('keygen', 'Test key', '--out', str(d / 'keys'))
            self.assertEqual(code, 0, out)
            code, out = self.run_cli('sign', str(mncf_path), '--key', str(d / 'keys' / 'Test-key.key'))
            self.assertEqual(code, 0, out)
            code, out = self.run_cli('verify', str(mncf_path), '--trust', str(d / 'keys' / 'Test-key.pub'))
            self.assertEqual(code, 0, out)
            code, out = self.run_cli('pack', str(d / 'p.mncp'), str(mncf_path), '--name', 'P',
                                     '--sign', str(d / 'keys' / 'Test-key.key'))
            self.assertEqual(code, 0, out)
            code, out = self.run_cli('verify', str(d / 'p.mncp'), '--trust', str(d / 'keys' / 'Test-key.pub'))
            self.assertEqual(code, 0, out)
            code, out = self.run_cli('check', str(d / 'missing.mncf'))
            self.assertEqual(code, 1)


    def test_check_asm_and_exclude(self):
        with tempfile.TemporaryDirectory() as d:
            d = pathlib.Path(d)
            nso = d / 'main.nso'
            nso.write_bytes(syn.sample_nso())
            cheats = d / f'{syn.BUILD_ID}.txt'
            cheats.write_text(syn.CHEATS, encoding='utf-8', newline='\n')
            runs = d / 'main.changed.tsv'
            runs.write_text(f'# BUILD_ID\t{syn.BUILD_ID.lower()}\n{syn.STR_W8:x}\t4\n', encoding='utf-8', newline='\n')
            code, out = self.run_cli('convert', str(cheats), '--game', syn.GAME_ID, '--module', str(nso), '--exclude',
                                     str(runs), '-o', str(d / 'x.mncf'), '-v')
            self.assertEqual(code, 0, out)
            self.assertIn("main+0x1008: the module's bytes there are not its own", out)
            if not disasm.available():
                return
            text = (d / 'x.mncf').read_text(encoding='utf-8')
            self.assertIn('bytes = "nop"', text)
            (d / 'x.mncf').write_text(text.replace('bytes = "nop"', 'bytes = "ret"', 1), encoding='utf-8',
                                      newline='\n')
            code, out = self.run_cli('check', str(d / 'x.mncf'), '--module', str(nso))
            self.assertIn('asm.bytes says "ret", the bytes are "nop"', out)


if __name__ == '__main__':
    unittest.main()
