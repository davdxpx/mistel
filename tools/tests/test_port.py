"""Tests of mistel port on made-up code modules: a build and a later one whose code moved, and cheats of each kind of
place."""
import contextlib
import io
import pathlib
import sys
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tools'))
sys.path.insert(0, str(ROOT / 'format' / 'lib' / 'python' / 'tests'))

from mistel import cli, port  # noqa: E402  (finds the mncf library of this checkout)
import mncf  # noqa: E402
import synthetic as syn  # noqa: E402
from mncf.module import parse_nso  # noqa: E402

NEW_ID = 'FEDCBA9876543210'
OLD = syn.STR_W8                    # str w8, [x19, #0x10] in the old build
NEW = syn.STR_W8 + syn.SHIFT        # the same instruction in the new one

FILE = f'''format = "mncf/1"
name = "Port example"
extensions = {{ optional = ["raw/1"] }}

[game]
platform = "switch"
id = "{syn.GAME_ID}"

[builds."1.0.0"]
modules = {{ main = "{syn.BUILD_ID}" }}

[[cheat]]
id = "money"
name = "Money"
# a place in code with an offset for its build

  [[cheat.patch]]
  at = {{ module = "main", offsets = {{ "1.0.0" = 0x{OLD:X} }} }}
  expect = "68 12 00 B9"
  bytes = "69 12 00 B9"

[[cheat]]
id = "branch"
name = "Branch away"

  [[cheat.patch]]
  at = {{ module = "main", offset = 0x{syn.BL:X} }}
  expect = "40 00 00 94"
  bytes = "00 01 00 14"

[[cheat]]
id = "by-pattern"
name = "By pattern"

  [[cheat.patch]]
  at = {{ module = "main", find = "68 12 00 B9 40 00 00 94" }}
  expect = "68 12 00 B9"
  bytes = "69 12 00 B9"

[[cheat]]
id = "data-flag"
name = "Data flag"

  [[cheat.write]]
  at = {{ module = "main", offsets = {{ "1.0.0" = 0x{syn.DATA + 0x10:X} }} }}
  type = "u32"
  value = 1

[[cheat]]
id = "heap"
name = "Heap value"

  [[cheat.write]]
  at = {{ region = "heap", offset = 0x40 }}
  type = "u32"
  value = 65535

[[cheat]]
id = "raw-only"
name = "Raw only"

  [[cheat.raw]]
  syntax = "atmosphere"
  build = "1.0.0"
  code = "04000000 00001008 B9001269"
'''


def old_module():
    return parse_nso(syn.sample_nso())


def new_module(change=None):
    text, ro, data = syn.shifted_code()
    if change:
        text = bytearray(text)
        for at, word in change:
            text[at:at + 4] = syn.words(word)
        text = bytes(text)
    return parse_nso(syn.make_nso(text, ro, data, bss=0x400, build_id=NEW_ID))


class PortTest(unittest.TestCase):
    def test_places_move_with_the_code(self):
        p = port.port(FILE.encode(), old_module(), new_module(), version='1.0.1')
        by = {c.id: c for c in p.cheats}
        self.assertEqual((p.old_key, p.new_key), ('1.0.0', '1.0.1'))
        self.assertEqual(by['money'].outcome, 'ported')
        self.assertEqual((by['money'].places[0].old, by['money'].places[0].new), (OLD, NEW))
        self.assertEqual(by['branch'].outcome, 'ported')
        self.assertEqual(by['by-pattern'].outcome, 'fits')
        self.assertEqual(by['data-flag'].outcome, 'data')
        self.assertEqual(by['heap'].outcome, 'fits')
        self.assertTrue(by['raw-only'].raw)
        f = mncf.load(p.text.encode())
        self.assertEqual(f.builds['1.0.1'].modules, {'main': NEW_ID})
        money = f.cheat('money').ops[0].at
        self.assertEqual(money.offsets, {'1.0.0': OLD, '1.0.1': NEW})
        branch = f.cheat('branch').ops[0].at
        self.assertEqual(branch.offsets, {'1.0.0': syn.BL, '1.0.1': syn.BL + syn.SHIFT})   # a fixed offset that moved
        # the reader finds the ported places in the new build, with their original bytes
        ev = mncf.evaluate(f, mncf.Runtime('switch', modules={'main': new_module().reader_module()},
                                           regions={'heap': port.WHOLE}))
        states = {c.id: (c.state, c.reason) for c in ev.cheats}
        self.assertEqual(ev.build, '1.0.1')
        self.assertEqual(states['money'], ('off', ''))
        self.assertEqual(states['branch'], ('off', ''))
        self.assertEqual(states['data-flag'][1], 'build-unknown')
        self.assertEqual(states['raw-only'][1], 'build-unknown')

    def test_changed_code_is_not_taken(self):
        # the update changed the instruction itself (str w8 at another field now): the place is found by the code around it, but the
        # cheat would patch other code than it expects
        p = port.port(FILE.encode(), old_module(), new_module([(NEW, 0xB9001688)]), version='1.0.1')
        by = {c.id: c for c in p.cheats}
        self.assertEqual(by['money'].outcome, 'changed')
        self.assertEqual(by['by-pattern'].outcome, 'pattern-not-found')
        self.assertNotIn('1.0.1', mncf.load(p.text.encode()).cheat('money').ops[0].at.offsets)

    def test_code_around_the_place_is_gone(self):
        change = [(NEW - 8 + i, 0xD503201F) for i in range(0, 0x20, 4)]
        p = port.port(FILE.encode(), old_module(), new_module(change), version='1.0.1')
        self.assertEqual({c.id: c.outcome for c in p.cheats}['money'], 'not-found')

    def test_the_build_must_be_known(self):
        other = FILE.replace(syn.BUILD_ID, '1111111111111111')
        with self.assertRaises(port.PortError):
            port.port(other.encode(), old_module(), new_module())
        p = port.port(other.encode(), old_module(), new_module(), old_key='1.0.0')
        self.assertEqual(p.new_key, NEW_ID)

    def test_a_build_the_file_has_already(self):
        twice = FILE.replace(f'[builds."1.0.0"]\nmodules = {{ main = "{syn.BUILD_ID}" }}\n',
                             f'[builds."1.0.0"]\nmodules = {{ main = "{syn.BUILD_ID}" }}\n\n'
                             f'[builds."1.0.1"]\nmodules = {{ main = "{NEW_ID}" }}\n')
        p = port.port(twice.encode(), old_module(), new_module())
        self.assertEqual(p.new_key, '1.0.1')
        self.assertEqual({c.id: c.outcome for c in p.cheats}['money'], 'ported')

    def test_cli(self):
        with tempfile.TemporaryDirectory() as d:
            d = pathlib.Path(d)
            (d / 'cheats.mncf').write_text(FILE, encoding='utf-8', newline='\n')
            (d / 'old.nso').write_bytes(syn.sample_nso())
            text, ro, data = syn.shifted_code()
            (d / 'new.nso').write_bytes(syn.make_nso(text, ro, data, bss=0x400, build_id=NEW_ID))
            out = io.StringIO()
            with contextlib.redirect_stdout(out), contextlib.redirect_stderr(out):
                code = cli.main(['port', str(d / 'cheats.mncf'), '--old', str(d / 'old.nso'), '--new', str(d / 'new.nso'),
                                 '--version', '1.0.1', '-v'])
            self.assertEqual(code, 0, out.getvalue())
            self.assertIn('2 ported, 2 fit as they are, 2 not', out.getvalue())
            self.assertIn('data-flag: a place in data: not ported yet (step 2)', out.getvalue())
            self.assertIn('raw-only: only raw code, for the old build: not ported yet (step 3)', out.getvalue())
            self.assertTrue((d / 'cheats.ported.mncf').is_file())


if __name__ == '__main__':
    unittest.main()
