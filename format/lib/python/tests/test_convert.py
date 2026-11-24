"""Tests of the converter and the writer, on made-up modules and cheat files only."""
import math
import pathlib
import sys
import tomllib
import unittest

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))

import conformance  # noqa: E402
import synthetic as syn  # noqa: E402
import mncf  # noqa: E402
from mncf import arm64, atmosphere as atm, convert, disasm, lz4, write  # noqa: E402
from mncf.module import CodeModule, module_from_bytes, parse_nso, read_runs  # noqa: E402
from mncf.pattern import make_pattern  # noqa: E402


def module(compress=True):
    return parse_nso(syn.sample_nso(compress))


def source(mod=None, text=syn.CHEATS, build_id=syn.BUILD_ID, version='1.0.0'):
    return convert.Source(build_id, text, version=version, module=mod)


class Lz4Test(unittest.TestCase):
    def test_round_trip(self):
        for data in (b'', b'abc', bytes(5000), b'abcd' * 1000 + b'xyz', bytes(range(256)) * 40, syn.sample_code()[0]):
            packed = syn.lz4_compress(data)
            self.assertEqual(lz4._decompress(packed, len(data)), data)
            self.assertEqual(lz4.decompress(packed, len(data)), data)

    def test_overlapping_match(self):
        block = bytes([0x1F, ord('a'), 1, 0, 0, 0x50]) + b'bcdef'     # 'a', then 19 more 'a' at offset 1
        self.assertEqual(lz4._decompress(block, 25), b'a' * 20 + b'bcdef')

    def test_errors(self):
        with self.assertRaises(lz4.LZ4Error):
            lz4._decompress(bytes([0x10, ord('a'), 5, 0]), 30)
        with self.assertRaises(lz4.LZ4Error):
            lz4._decompress(syn.lz4_compress(bytes(100)), 99)


class ModuleTest(unittest.TestCase):
    def test_nso(self):
        for compress in (True, False):
            m = module(compress)
            text, ro, data = syn.sample_code()
            self.assertEqual(m.image[:len(text)], text)
            self.assertEqual(m.image[0x2000:0x2000 + len(ro)], ro)
            self.assertEqual(m.image[0x3000:0x3000 + len(data)], data)
            self.assertEqual(len(m.image), 0x3800)
            self.assertEqual(m.sections, {'code': (0, 0x2000), 'rodata': (0x2000, 0x3000), 'data': (0x3000, 0x3800)})
            self.assertEqual((m.short_id, m.text_size), (syn.BUILD_ID, syn.TEXT_SIZE))

    def test_plain_image_runs_and_errors(self):
        m = module_from_bytes(bytes(64))
        self.assertEqual((m.sections, m.build_id), ({'code': (0, 64)}, None))
        self.assertEqual(read_runs('# BUILD_ID\tabc\n1000\t8\n\n2000\t10\n'), [(0x1000, 8), (0x2000, 0x10)])
        with self.assertRaises(ValueError):
            read_runs('1000\n')
        with self.assertRaises(ValueError):
            parse_nso(b'NSO0' + bytes(10))


class Arm64Test(unittest.TestCase):
    def test_targets(self):
        self.assertEqual(arm64.pc_relative_target(0x94000040, 0x1000), 0x1100)      # bl +0x100
        self.assertEqual(arm64.pc_relative_target(0x97FFFFFF, 0x1000), 0xFFC)       # bl -4
        self.assertEqual(arm64.pc_relative_target(0x54000080, 0x1000), 0x1010)      # b.eq +0x10
        self.assertEqual(arm64.pc_relative_target(0xB4000040, 0x1000), 0x1008)      # cbz x0, +8
        self.assertEqual(arm64.pc_relative_target(0x36000040, 0x1000), 0x1008)      # tbz w0, #0, +8
        self.assertEqual(arm64.pc_relative_target(0x90000008, 0x1234), 0x1000)      # adrp x8, this page
        self.assertEqual(arm64.pc_relative_target(0xB0000008, 0x1234), 0x2000)      # adrp x8, next page
        self.assertEqual(arm64.pc_relative_target(0x58000040, 0x1000), 0x1008)      # ldr x0, literal
        self.assertIsNone(arm64.pc_relative_target(0xD503201F, 0x1000))             # nop

    def test_moving_bytes(self):
        code = syn.words(0x94000040, 0x90000008, 0xB9402100, 0xB9402101, 0xD503201F)
        self.assertEqual(sorted(arm64.moving_bytes(code, 0, len(code))), [0, 1, 2, 4, 5, 6, 7, 9, 10, 13, 14])

    def test_relative_outside(self):
        self.assertTrue(arm64.relative_outside(syn.words(0x14000100), 0x1000, 4))
        self.assertFalse(arm64.relative_outside(syn.words(0x14000001, 0xD503201F), 0x1000, 8))
        self.assertFalse(arm64.relative_outside(syn.words(0xD503201F), 0x1000, 4))


class PatternTest(unittest.TestCase):
    def test_unique_and_masked(self):
        m = module()
        p = make_pattern(m, syn.STR_W8)
        self.assertEqual((p.text, p.place, p.start), ('68 12 00 B9', 0, syn.STR_W8))
        p = make_pattern(m, syn.BL)
        self.assertEqual((p.text, p.place), ('68 12 00 B9 ?? ?? ?? 94', 4))
        self.assertEqual(mncf.find_all(m.reader_module(), p.items), [syn.STR_W8])

    def test_no_pattern(self):
        m = module()
        self.assertIsNone(make_pattern(m, 0x1FF0))                     # zero bytes of the cave
        self.assertIsNone(make_pattern(m, syn.DATA + 0x10))            # data is not searched

    def test_excluded_bytes(self):
        m = module()
        m.exclude = [(syn.STR_W8, 4)]
        p = make_pattern(m, syn.BL)
        self.assertNotIn('68 12 00 B9', p.text)


class AtmosphereTest(unittest.TestCase):
    def test_parse(self):
        entries = atm.parse('[A] 04000000 00000010 00000001\n{M}\n04000000 00000020\n 00000002\n[Empty]\n')
        self.assertEqual([(e.name, e.master, e.words) for e in entries],
                         [('A', False, [0x04000000, 0x10, 1]), ('M', True, [0x04000000, 0x20, 2]), ('Empty', False, [])])
        self.assertEqual(entries[1].code, '04000000 00000020\n00000002\n')
        for bad in ('04000000', '[A\n', '[A]\n0400 0000'):
            with self.assertRaises(atm.AtmosphereError):
                atm.parse(bad)

    def test_decode_lengths(self):
        ws = [0x08000000, 0x10, 1, 2, 0x40F00000, 1, 2, 0x580F0000, 0x10, 0x780F0000, 8, 0x80000001, 0x20000000,
              0x9409F100, 5, 0x300F0000, 2, 0x310F0000, 0xA4F00000, 0xFF000000, 0xC4000000, 0, 2]
        self.assertEqual([op.kind for op in atm.decode(ws)],
                         ['store', 'load-const', 'load-mem', 'arith', 'keys', 'end-cond', 'arith-reg', 'loop', 'loop',
                          'store-reg', 'pause', 'keys'])
        with self.assertRaises(atm.Unclear):
            atm.decode([0xB0000000])

    def test_derive(self):
        d = atm.derive(atm.Entry('x', False, 1, words=[0x580F0000, 0x3020, 0x580F1000, 0x10, 0x780F0000, 8,
                                                        0x640F0000, 0, 0x63]))
        self.assertEqual([(s.place, s.width, s.value) for s in d.stores],
                         [(atm.Place('main', 0x3020, (0x10, 8)), 4, 0x63)])
        d = atm.derive(atm.Entry('x', False, 1, words=[0x80000001, 0x04000000, 0x100C, 0xD503201F, 0x21000000,
                                                        0x04000000, 0x100C, 0x94000040, 0x20000000]))
        self.assertEqual((d.keys, len(d.stores), len(d.released)), (1, 1, 1))
        d = atm.derive(atm.Entry('x', False, 1, words=[0x14150000, 0x100, 5, 0x02100000, 0x104, 1, 0x20000000]))
        self.assertEqual((d.condition.place, d.condition.op, d.condition.value), (atm.Place('heap', 0x100), '==', 5))
        with self.assertRaises(atm.Unclear):
            atm.derive(atm.Entry('x', False, 1, words=[0x300F0000, 4, 0x310F0000]))
        d = atm.derive(atm.Entry('x', False, 1, words=[0x040A0000, 0x10, 1]))
        self.assertEqual(d.reads, {0xA})

    def test_store_words_round_trip(self):
        for place, width, value in ((atm.Place('main', 0x1234), 4, 0xD503201F),
                                    (atm.Place('heap', 0x10_0000_0000), 8, 0x1122334455667788),
                                    (atm.Place('main', 0x1234560, (0x18, 0x20)), 4, 0x94),
                                    (atm.Place('alias', 0x10, (-8 & atm.MASK64, 0)), 2, 7)):
            d = atm.derive(atm.Entry('x', False, 1, words=atm.store_words(place, width, value)))
            self.assertEqual([(s.place, s.width, s.value) for s in d.stores], [(place, width, value)])

    def test_wide_path_steps(self):
        for place in (atm.Place('main', 0x10, (0x18, 1 << 32)), atm.Place('heap', 0x20, (-0x20 & atm.MASK64, 5 << 40, 8))):
            d = atm.derive(atm.Entry('x', False, 1, words=atm.store_words(place, 4, 7)))
            self.assertEqual(d.stores[0].place, place)
        with self.assertRaises(atm.Unclear):
            atm.store_words(atm.Place('main', 1 << 40), 4, 0)
        with self.assertRaises(atm.Unclear):
            atm.condition_words(atm.Place('heap', 1 << 40), 4, '==', 0)

    def test_odd_store_widths(self):
        for words in ([0x580F0000, 0x10, 0x630F0000, 0, 1], [0x40000000, 0, 5, 0x580F0000, 0x10, 0xA30F0000]):
            with self.assertRaises(atm.Unclear):
                atm.derive(atm.Entry('x', False, 1, words=words))

    def test_keys_and_registers(self):
        self.assertEqual(atm.keys_to_buttons(0x102), 'B+ZL')
        self.assertEqual(atm.buttons_to_keys('B+ZL'), 0x102)
        self.assertIsNone(atm.keys_to_buttons(0x10000000))
        loop = atm.Entry('x', False, 1, words=[0x300F0000, 4, 0x400E0000, 0, 1, 0x310F0000])
        self.assertEqual(atm.registers_written(loop), {0xE, 0xF})
        self.assertEqual(atm.registers_written(atm.Entry('x', False, 1, words=[0xB0000000])), set(range(16)))


class ConvertTest(unittest.TestCase):
    def outcomes(self, conv):
        return {c.id: (c.outcome, c.why) for c in conv.report.cheats if c.id}

    def evaluate(self, conv, mod):
        f = mncf.load(conv.text.encode())
        rt = mncf.Runtime('switch', modules={'main': mod.reader_module(0x8000000)},
                          regions={'heap': (0x10000000, 0x10000)})
        return f, mncf.evaluate(f, rt)

    def test_with_module(self):
        mod = module()
        conv = convert.from_atmosphere(source(mod), game_id=syn.GAME_ID, name='Example', origin='example.zip')
        outcome = self.outcomes(conv)
        self.assertEqual(outcome['money'][0], 'pattern')
        self.assertEqual(outcome['branch-away'][0], 'build')
        self.assertEqual(outcome['master-setup'][0], 'build')
        self.assertEqual(outcome['hold-a'][0], 'pattern')
        self.assertEqual(outcome['cave-jump'][0], 'build')
        self.assertEqual(outcome['speed'][0], 'pattern')
        for cid in ('data-flag', 'heap-value', 'pointer', 'condition'):
            self.assertEqual(outcome[cid][0], 'build', cid)
        self.assertEqual(outcome['loop'], ('raw', 'a loop'))
        self.assertNotIn('money-off', outcome)
        self.assertEqual(conv.report.count('restore'), 1)
        self.assertEqual(conv.report.groups, ['Section A', 'Data', 'Items'])
        f, ev = self.evaluate(conv, mod)
        self.assertEqual(ev.build, '1.0.0')
        for c in ev.cheats:
            self.assertNotEqual(c.state, 'refused', f'{c.id}: {c.reason}')
        money = ev.cheat('money').ops[0]
        self.assertEqual((money.offset, money.expect, money.bytes),
                         (syn.STR_W8, bytes.fromhex('681200b9'), bytes.fromhex('691200b9')))
        speed = ev.cheat('speed').ops[0]
        self.assertEqual(speed.bytes, bytes.fromhex('0010201e'))
        self.assertEqual(f.cheat('speed').values['value'].type, 'f32')
        self.assertEqual(f.cheat('speed').values['value'].default, 2.0)
        self.assertEqual((f.cheat('money').group, f.cheat('heap-value').group), ('Section A', 'Data'))
        self.assertEqual((f.cheat('hold-a').hold, f.cheat('cave-jump').hold), ('A', 'B'))
        speed = f.cheat('speed')       # patches without an else branch: the patch stays after the press
        self.assertEqual((speed.hold, speed.run, speed.press), (None, 'action', 'ZL'))
        self.assertEqual((f.cheat('cave-jump').group, f.cheat('speed').group), ('Items', None))
        # Only a cheat that depends on the master requires it; Loop stays raw code, which may.
        self.assertEqual((f.cheat('money').requires, f.cheat('loop').requires), ([], ['master-setup']))
        self.assertTrue(f.cheat('master-setup').master)
        self.assertEqual(f.authors, ['someone'])
        self.assertEqual(f.doc['origin'], {'format': 'atmosphere', 'name': 'example.zip', 'site': 'example.org'})
        self.assertEqual(f.optional, ['raw/1', 'arm64-imm/1'])
        self.assertIn('find = "68 12 00 B9"', conv.text)
        if disasm.available():
            self.assertIn('str w9, [x19, #0x10]', conv.text)

    def test_without_module(self):
        conv = convert.from_atmosphere(source(), game_id=syn.GAME_ID)
        outcome = self.outcomes(conv)
        self.assertEqual(outcome['money'][0], 'build')                # expect from the restore entry
        self.assertEqual(outcome['branch-away'][0], 'build')          # expect from the release branch of Hold A
        self.assertEqual(outcome['hold-a'][0], 'build')
        self.assertEqual(outcome['cave-jump'][0], 'raw')              # no original bytes after the end of the code
        self.assertEqual(outcome['speed'][0], 'raw')
        self.assertEqual(outcome['heap-value'][0], 'build')
        f = mncf.load(conv.text.encode())
        self.assertEqual(f.cheat('money').ops[0].expect, bytes.fromhex('681200b9'))
        self.assertNotIn('asm', conv.text)

    def test_authors(self):
        self.assertEqual(convert.authors_of(['credits: Some Forum', 'www.example.org',
                                             'This set is created by Ann & Bo, enjoy!']), ['Some Forum', 'Ann', 'Bo'])
        self.assertEqual(convert.authors_of(['Cheats by Ann on Some Forum & Bo @ Other', 'by Cy (a forum)']),
                         ['Ann (Some Forum)', 'Bo (Other)', 'Cy (a forum)'])
        self.assertEqual(convert.site_of(['credits: Some Forum', 'https://www.Example.org/cheats']), 'example.org')
        plain = '[Money]\n04100000 00000010 00000001\n'
        f = mncf.load(convert.from_atmosphere(source(text=plain), game_id=syn.GAME_ID, authors=['Player']).text.encode())
        self.assertEqual(f.authors, ['Player'])
        f = mncf.load(convert.from_atmosphere(source(text=plain), game_id=syn.GAME_ID).text.encode())
        self.assertEqual(f.authors, [])
        f = mncf.load(convert.from_atmosphere(source(text=syn.CHEATS), game_id=syn.GAME_ID,
                                              authors=['Player']).text.encode())
        self.assertEqual(f.authors, ['someone'])

    def test_excluded_runs(self):
        mod = module()
        mod.exclude = [(syn.STR_W8, 4)]
        conv = convert.from_atmosphere(source(mod), game_id=syn.GAME_ID, asm=False)
        f = mncf.load(conv.text.encode())
        money = f.cheat('money').ops[0]
        self.assertEqual((money.at.kind, money.expect), ('module-builds', bytes.fromhex('681200b9')))

    def test_two_builds(self):
        a = source(module(), version='1.0.0')
        text, ro, data = syn.shifted_code()
        b_mod = parse_nso(syn.make_nso(text, ro, data, 0x400, build_id='1122334455667788'))
        b = source(b_mod, syn.shifted_cheats(), '1122334455667788', '1.0.1')
        conv = convert.from_atmosphere([a, b], game_id=syn.GAME_ID, asm=False)
        f = mncf.load(conv.text.encode())
        self.assertEqual(list(f.builds), ['1.0.0', '1.0.1'])
        self.assertEqual(len(f.cheats), len(convert.from_atmosphere(a, game_id=syn.GAME_ID).doc['cheat']))
        money = f.cheat('money')
        self.assertEqual([r.build for r in money.raw], ['1.0.0', '1.0.1'])
        self.assertEqual(f.cheat('branch-away').ops[0].at.offsets, {'1.0.0': 0x100C, '1.0.1': 0x104C})
        for build_mod, key in ((module(), '1.0.0'), (b_mod, '1.0.1')):
            rt = mncf.Runtime('switch', modules={'main': build_mod.reader_module()}, regions={'heap': (0, 0x10000)})
            ev = mncf.evaluate(f, rt)
            self.assertEqual(ev.build, key)
            for c in ev.cheats:
                self.assertNotEqual(c.state, 'refused', f'{key} {c.id}: {c.reason}')

    def test_back_to_atmosphere(self):
        mod = module()
        f = mncf.load(convert.from_atmosphere(source(mod), game_id=syn.GAME_ID).text.encode())
        name, back, report = convert.to_atmosphere(f, '1.0.0', mod)
        self.assertEqual(name, f'{syn.BUILD_ID}.txt')
        same, differ, missing = convert.compare(syn.CHEATS, back)
        self.assertEqual((differ, missing), ([], ['Money off']))
        self.assertTrue(all(how == 'raw' for _, how in report.written))
        _, core, report = convert.to_atmosphere(f, '1.0.0', mod, prefer='core')
        same, differ, missing = convert.compare(syn.CHEATS, core, effects=True)
        self.assertEqual(differ, [])
        self.assertEqual((dict(report.written)['loop'], dict(report.written)['money']), ('raw', 'core'))
        name, other, report = convert.to_atmosphere(f, '9.9.9', mod, prefer='core')
        self.assertEqual(name, f'{syn.BUILD_ID}.txt')          # a build the file does not name: from patterns
        self.assertIn('money', dict(report.written))
        self.assertNotIn('branch-away', dict(report.written))   # offsets for 1.0.0 only
        with self.assertRaises(ValueError):
            convert.to_atmosphere(f, '9.9.9')

    def test_back_refuses_what_atmosphere_cannot_say(self):
        text = """format = "mncf/1"
name = "x"
extensions = { optional = ["arm64-imm/1"] }

[game]
platform = "switch2"
id = "0100000000ABC000"

[builds.one]
modules = { main = "0123456789ABCDEF" }

[[cheat]]
id = "c-button"
name = "C button"
hold = "C"

  [[cheat.write]]
  at = { region = "heap", offset = 0x10 }
  type = "u8"
  value = 1

[[cheat]]
id = "too-wide"
name = "Too wide"

  [cheat.values.n]
  type = "u32"
  default = 70000

  [[cheat.patch]]
  at = { module = "main", offset = 0x1000 }
  expect = "08 00 80 52"
  encode = { insn = "movz", width = 32, rd = 8, value = "$n" }
"""
        f = mncf.load(text.encode())
        _, back, report = convert.to_atmosphere(f, 'one', prefer='core')
        self.assertEqual([cid for cid, _ in report.skipped], ['c-button', 'too-wide'])


def same_doc(a, b):
    """Whether two documents hold the same values, with NaN equal to a NaN of the same sign."""
    if isinstance(a, float) and isinstance(b, float) and math.isnan(a) and math.isnan(b):
        return math.copysign(1, a) == math.copysign(1, b)
    if isinstance(a, dict) and isinstance(b, dict):
        return set(a) == set(b) and all(same_doc(a[k], b[k]) for k in a)
    if isinstance(a, list) and isinstance(b, list):
        return len(a) == len(b) and all(same_doc(x, y) for x, y in zip(a, b))
    return type(a) is type(b) and a == b


class PeanutsGapsTest(unittest.TestCase):
    """What the first runtime that used the library found."""

    def test_master_only_where_a_cheat_depends_on_it(self):
        text = """{Setup}
04000000 00001FE8 D503201F
400A0000 00000000 00000000

[Uses the cave]
04000000 00001008 140003F8

[Uses its register]
040A0000 00002010 00000001

[On its own]
04000000 00001008 B9001269
"""
        conv = convert.from_atmosphere(source(module(), text=text), game_id=syn.GAME_ID, asm=False)
        f = mncf.load(conv.text.encode())
        self.assertEqual([c.requires for c in f.cheats if not c.master], [['setup'], ['setup'], []])

    def test_lines_that_are_not_code(self):
        text = """// cheats for Example Quest
[Money]
04000000 00001008 B9001269
made for 1.0.0

[Broken]
04000000 00001008 GGGGGGGG
"""
        with self.assertRaises(atm.AtmosphereError):
            atm.parse(text)
        notes = []
        entries = atm.parse(text, notes)
        self.assertEqual(notes, [(1, '// cheats for Example Quest'), (4, 'made for 1.0.0')])
        self.assertEqual([e.broken for e in entries], [[], ['04000000 00001008 GGGGGGGG']])
        conv = convert.from_atmosphere(source(module(), text=text), game_id=syn.GAME_ID, asm=False)
        f = mncf.load(conv.text.encode())
        self.assertEqual([c.id for c in f.cheats], ['money'])
        self.assertIn('Lines in the source that are not cheat code: // cheats for Example Quest; made for 1.0.0',
                      f.about)
        self.assertIn('Entries whose code could not be read, left out: Broken', f.about)
        self.assertEqual(conv.report.count('unreadable'), 1)

    def test_edizon_sections_and_names_that_are_no_person(self):
        text = """[--SectionStart:Items--]
[Money]
04000000 00001008 B9001269
[--SectionEnd:Items--]
[Heap]
04100000 00000010 00000001
[Cheats created by All Credited, enjoy]
"""
        f = mncf.load(convert.from_atmosphere(source(text=text), game_id=syn.GAME_ID).text.encode())
        self.assertEqual([(c.id, c.group) for c in f.cheats], [('money', 'Items'), ('heap', None)])
        self.assertEqual(f.authors, [])
        self.assertEqual(convert.authors_of(['by All Credited', 'by everyone', 'by Ann']), ['Ann'])

    def test_builds_with_the_same_version(self):
        a = source(text='[Heap]\n04100000 00000010 00000001\n', version='1.0.0')
        b = source(text='[Heap]\n04100000 00000010 00000002\n', build_id='1122334455667788', version='1.0.0')
        f = mncf.load(convert.from_atmosphere([a, b], game_id=syn.GAME_ID).text.encode())
        self.assertEqual({k: b.version for k, b in f.builds.items()},
                         {syn.BUILD_ID: '1.0.0', '1122334455667788': '1.0.0'})
        with self.assertRaises(convert.ConvertError):
            convert.from_atmosphere([a, a], game_id=syn.GAME_ID)

    def test_back_with_the_players_values(self):
        text = '[Tickets]\n040A0000 00001008 52986A08\n'
        f = mncf.load(convert.from_atmosphere(source(module(), text=text), game_id=syn.GAME_ID).text.encode())
        _, back, _ = convert.to_atmosphere(f, '1.0.0', module(), 'core', mncf.Choices(values={'tickets.value': 7}))
        self.assertIn('04000000 00001008 528000E8', back)          # movz w8, #7
        # A value the player cannot choose is refused as the reader refuses it; the raw entry is written instead.
        for bad in ('7', True, -1, 1 << 40):
            _, back, report = convert.to_atmosphere(f, '1.0.0', module(), 'core',
                                                    mncf.Choices(values={'tickets.value': bad}))
            self.assertEqual(report.written, [('tickets', 'raw')], bad)
            self.assertIn('040A0000 00001008 52986A08', back)
        f.cheats[0].raw = []
        _, _, report = convert.to_atmosphere(f, '1.0.0', module(), 'core', mncf.Choices(values={'tickets.value': -1}))
        self.assertEqual(report.skipped, [('tickets', 'a chosen value the cheat cannot take')])

    def test_compare_reads_the_original_leniently(self):
        original = '// notes\n[Heap]\n04100000 00000010 00000001\n\n[Broken]\n04100000 00000010 GGGGGGGG\n'
        same, differ, missing = convert.compare(original, '[Heap]\n04100000 00000010 00000001\n')
        self.assertEqual((same, differ, missing), (['Heap'], [], []))

    def test_runtime_lacks_parts_of_the_core_and_rewrote_code(self):
        text = syn.CHEATS
        f = mncf.load(convert.from_atmosphere(source(module(), text=text), game_id=syn.GAME_ID).text.encode())
        rt = mncf.Runtime('switch', modules={'main': module().reader_module()}, regions={'heap': (0, 0x10000)},
                          unsupported=['hold', 'run.action'], rewritten={'main': [(syn.STR_W8, 4)]})
        ev = mncf.evaluate(f, rt)
        reasons = {c.id: c.reason for c in ev.cheats}
        self.assertEqual(reasons['hold-a'], 'runtime:unsupported:hold')
        self.assertEqual(reasons['speed'], 'runtime:unsupported:run.action')
        self.assertEqual(reasons['money'], 'runtime:rewritten')
        self.assertEqual(reasons['heap-value'], '')


class WriteTest(unittest.TestCase):
    def test_round_trip_of_the_suite(self):
        count = 0
        for name, case, run, folder in conformance.cases():
            if run is not conformance.file_results:
                continue
            try:
                f = mncf.load(conformance.case_bytes(case, folder))
            except mncf.MncfError:
                continue
            text = write.write_file(f)
            again = mncf.load(text.encode())
            self.assertTrue(same_doc(again.doc, f.doc), name)
            self.assertEqual(write.dumps(again.doc), text, name)
            count += 1
        self.assertGreater(count, 40)

    def test_documents_that_load_but_break_rules(self):
        doc = {'format': 'mncf/1', 'name': 'x', 'game': {'platform': 'switch', 'id': '0100000000ABC000'},
               'cheat': [{'id': 'a', 'name': 'a', 'values': 5}, {'id': 'b', 'name': 'b', 'values': {'n': 3}}],
               'i18n': {'de': {'name': 'y', 'cheat': {'a': {'name': 'z'}}}, 'note': 'q'}, 'extra\n': 1}
        self.assertEqual(tomllib.loads(write.dumps(doc)), doc)
        for odd in ({'preset': [5]}, {'i18n': 5}, {'preset': []}, {'choices': {'items': 3}}):
            doc2 = dict(doc, **odd)
            self.assertEqual(tomllib.loads(write.dumps(doc2)), doc2)

    def test_layout(self):
        doc = {'cheat': [{'name': 'Money', 'id': 'money', 'write': [{'value': 999999, 'type': 'u32',
                                                                     'at': {'offset': 0x1200, 'region': 'heap'}}]}],
               'game': {'id': '0100000000ABC000', 'platform': 'switch'}, 'name': 'Example', 'format': 'mncf/1'}
        self.assertEqual(write.dumps(doc), '''format = "mncf/1"
name = "Example"

[game]
platform = "switch"
id = "0100000000ABC000"

[[cheat]]
id = "money"
name = "Money"

  [[cheat.write]]
  at = { region = "heap", offset = 0x1200 }
  type = "u32"
  value = 0xF423F
''')
        self.assertEqual(write.scalar(-5, 'path'), '-5')
        self.assertEqual(write.scalar(0xFFFFFFFF80000000), '"0xFFFFFFFF80000000"')
        self.assertEqual(write.scalar(float('inf')), 'inf')
        self.assertEqual(write.multiline('a\nb'), '"""\na\nb\\\n"""')


if __name__ == '__main__':
    unittest.main()
