"""Converts Atmosphère cheat files to `.mncf`, and `.mncf` back to Atmosphère for one build (spec, section 15).

    conv = from_atmosphere([Source(build_id, text, version='2.0.2', module=module)], game_id=..., origin='cheats.zip')
    conv.text, conv.doc, conv.report

Every cheat keeps its code as a `raw/1` entry. Where the code is plain writes, it also gets core operations: a write
into the main module's code becomes a patch, one into data or a region a write, a pointer chain a `path`, a button
block `hold`, a memory condition `when`, a `movz` or `fmov` a value the player sets (`arm64-imm/1`). With the code
module, patches get `expect` from it and a pattern instead of a fixed offset wherever the bytes mean the same at
another place. Several files for builds of the same game become one file with all their builds.
"""
import re

from dataclasses import dataclass, field

from . import arm64, atmosphere as atm, disasm, patches as patches_mod
from . import encode as enc
from .model import Condition, Override, Patch, Read, ValueRef, Write
from .pattern import make_pattern
from .values import encode as encode_value, fits, hex_bytes, type_size
from .evaluate import Choices, _Refused, _value
from .write import dumps

RESTORE_NAME = re.compile(r'\b(restore|restores|original|vanilla)\b|\boff\W*\Z', re.I | re.A)
CREDIT_NAME = re.compile(r'credit|made by|created by|written by|cheats? by|codes? by|patch(?:es)? by|thanks|www\.|'
                         r'https?:|\.com\b|\.org\b|\.net\b|discord|patreon|\bTID\s*=|\bBID\s*=', re.I | re.A)
LINK = re.compile(r'www\.|https?:|\.com\b|\.org\b|\.net\b', re.I | re.A)
AUTHOR = re.compile(r'\bby\s+(.+)|\bcredits?\s*[:=]\s*(.+)', re.I | re.A)
PLATFORM = re.compile(r'^(.+?)\s*(?:\s(?:on|at|from)\s|@|\()\s*(.+?)\)?\Z', re.I | re.A)
SITE = re.compile(r'(?:https?://)?(?:www\.)?([A-Za-z0-9-]+(?:\.[A-Za-z0-9-]+)*\.[A-Za-z]{2,})', re.A)
DECORATION = re.compile(r'^[\s\-=*~#_.:+<>|]+|[\s\-=*~#_.:+<>|]+\Z', re.A)
GROUP_END = [0x20000001]
EDIZON = re.compile(r'^--\s*section\s*(start|end)\s*:\s*(.*?)\s*--\Z', re.I | re.A)
NOT_A_NAME = re.compile(r'^(all credited|all credits?|everyone|everybody|unknown|anonymous|various|'
                        r'(the )?community|n/?a)\Z', re.I | re.A)


class ConvertError(ValueError):
    """Sources that cannot become one file. `code` is `source`; an unreadable file is an AtmosphereError, `atmosphere`."""
    code = 'source'


@dataclass
class Source:
    """One Atmosphère cheat file: the build ID of the main module it is for, its text, and what else is known."""
    build_id: str
    text: str
    version: str = None        # the version the player sees, which also names the build in the file
    module: object = None      # the main module of that build (a module.CodeModule), for expect and patterns
    build_key: str = None      # the build's name in the file; default: the version, else the build ID


@dataclass
class CheatReport:
    id: str
    name: str
    build: str
    outcome: str          # pattern, build, raw or restore
    why: str = ''
    patterns: int = 0
    fixed: int = 0        # operations that keep offsets for this build
    values: int = 0
    notes: list = field(default_factory=list)


@dataclass
class Report:
    cheats: list = field(default_factory=list)
    groups: list = field(default_factory=list)
    credits: list = field(default_factory=list)
    warnings: list = field(default_factory=list)
    notes: list = field(default_factory=list)          # lines of the sources that are neither names nor code
    unreadable: list = field(default_factory=list)     # names of entries whose code could not be read

    def count(self, outcome):
        return sum(1 for c in self.cheats if c.outcome == outcome)


@dataclass
class Conversion:
    doc: dict
    text: str
    report: Report


def slug(name, taken):
    base = re.sub(r'[^a-z0-9]+', '-', name.lower()).strip('-') or 'cheat'
    if not 'a' <= base[0] <= 'z':
        base = 'cheat-' + base
    base = base[:48].rstrip('-')
    out, n = base, 2
    while out in taken:
        out = f'{base}-{n}'
        n += 1
    taken.add(out)
    return out


def authors_of(credits):
    """The people that credit lines name: what follows "by" or "credits:", up to a comma or the end. A person named
    with the platform they posted on ("Ann on GBAtemp", "Ann @ GBAtemp") is written "Ann (GBAtemp)"."""
    names = []
    for line in credits:
        m = AUTHOR.search(line)
        if not m:
            continue
        text = re.split(r'[,!;|]', m.group(1) or m.group(2))[0].strip()
        for name in re.split(r'\s+&\s+|\s+and\s+', text, flags=re.A):
            name = name.strip(' .')
            on = PLATFORM.match(name)
            if on:
                name = f'{on.group(1).strip()} ({on.group(2).strip(" .()")})'
            if name and not LINK.match(name) and not NOT_A_NAME.match(name) and name not in names:
                names.append(name)
    return names


def site_of(credits):
    """The website the credit lines name, as a lower-case domain without www, or None."""
    for line in credits:
        m = SITE.search(line)
        if m:
            return m.group(1).lower()
    return None


# ---- one file -------------------------------------------------------------------------------------------------------

@dataclass
class _Item:
    name: str
    master: bool
    group: str
    entry: object
    derived: object = None


def _read(src, report, key):
    """The entries of a file with code, their groups, and the credit lines."""
    items, group = [], None
    notes = []
    for e in atm.parse(src.text, notes):
        if e.broken:
            report.unreadable.append(e.name)
            report.cheats.append(CheatReport('', e.name, key, 'unreadable',
                                             'lines that mix code with other text: ' + '; '.join(e.broken)))
            continue
        section = EDIZON.match(e.name) if not e.words or atm.is_noop(e) else None
        if section:
            if section.group(1).lower() == 'start':
                group = DECORATION.sub('', section.group(2)).replace('/', '-').strip() or None
                if group and group not in report.groups:
                    report.groups.append(group)
            else:
                group = None
            continue
        if not e.words or atm.is_noop(e):
            if CREDIT_NAME.search(e.name):
                if e.name not in report.credits:
                    report.credits.append(e.name)
            elif e.words == GROUP_END:
                group = None
            elif not e.master:
                cleaned = DECORATION.sub('', e.name).replace('/', '-').strip()
                group = cleaned or None
                if group and group not in report.groups:
                    report.groups.append(group)
            continue
        items.append(_Item(e.name, e.master, group, e))
    for n, line in notes:
        if line not in report.notes:
            report.notes.append(line)
    written = set()
    for it in sorted(items, key=lambda i: not i.master):
        try:
            d = atm.derive(it.entry)
        except atm.Unclear as u:
            it.derived = u
            written |= atm.registers_written(it.entry)
            continue
        it.derived = atm.Unclear('it reads a register that an earlier cheat sets') if d.reads & written else d
        written |= d.writes
    return items


def _original(module, originals, offset, width):
    """The original bytes at a place of the main module: from the module, else from the file's own restore code."""
    if module is not None and not module.excluded(offset, width):
        return module.image[offset:offset + width]
    out = bytearray()
    for i in range(width):
        for (off, w), data in originals.items():
            if off <= offset + i < off + w:
                out.append(data[offset + i - off])
                break
        else:
            return None
    return bytes(out)


def _in_code(module, offset, width):
    return module.section_of(offset) in ('code', 'rodata') and \
        module.section_of(offset + width - 1) in ('code', 'rodata')


def _restores(items, module):
    """The entries that only put back original code, and the original bytes the file's code gives."""
    originals, restore = {}, set()
    for it in items:
        d = it.derived
        if isinstance(d, atm.Unclear) or not d.stores:
            continue
        if d.released is not None:
            for s in d.released:
                if s.place.region == 'main' and not s.place.path:
                    originals.setdefault((s.place.offset, s.width), s.data)
        if d.keys or d.condition or any(s.place.region != 'main' or s.place.path for s in d.stores):
            continue
        if module is not None and not any(module.excluded(s.place.offset, s.width) for s in d.stores):
            if all(_in_code(module, s.place.offset, s.width) and
                   module.image[s.place.offset:s.place.offset + s.width] == s.data for s in d.stores):
                restore.add(id(it))
            continue
        if RESTORE_NAME.search(it.name):
            others = set()
            for other in items:
                if other is not it:
                    others |= atm.main_offsets(other.entry)
            if all(s.place.offset in others for s in d.stores):
                restore.add(id(it))
                for s in d.stores:
                    originals[(s.place.offset, s.width)] = s.data
    return restore, originals


def _location(place, key):
    if place.region == 'main':
        loc = {'module': 'main', 'offsets': {key: place.offset}}
    else:
        loc = {'region': place.region, 'offsets': {key: place.offset}}
    if place.path:
        loc['path'] = [p - (1 << 64) if p >> 63 else p for p in place.path]
    return loc


def _merge_runs(stores):
    runs = []
    for s in stores:
        if runs and runs[-1][0] + len(runs[-1][1]) == s.place.offset:
            runs[-1][1].extend(s.data)
        else:
            runs.append([s.place.offset, bytearray(s.data)])
    return [(off, bytes(data)) for off, data in runs]


def _fmov_value(imm8):
    sign = -1.0 if imm8 & 0x80 else 1.0
    cd = (imm8 >> 4) & 3
    e = cd + 1 if not imm8 & 0x40 else cd - 3
    return sign * (16 + (imm8 & 15)) / 16 * 2.0 ** e


def _encode_of(data):
    """The `encode` table and the value's type for new bytes that are one movz, a movz and a movk, or one fmov with an
    immediate, else None."""
    words = [int.from_bytes(data[i:i + 4], 'little') for i in range(0, len(data), 4)]
    if len(data) == 4 and (words[0] & 0x7FE00000) == 0x52800000:
        w = words[0]
        return {'insn': 'movz', 'width': 64 if w >> 31 else 32, 'rd': w & 31, 'value': (w >> 5) & 0xFFFF}, 'u16'
    if len(data) == 8 and (words[0] & 0x7FE00000) == 0x52800000 and (words[1] & 0x7FE00000) == 0x72A00000 and \
            words[0] >> 31 == words[1] >> 31 and words[0] & 31 == words[1] & 31:
        lo, hi = (words[0] >> 5) & 0xFFFF, (words[1] >> 5) & 0xFFFF
        return {'insn': 'movz-movk', 'width': 64 if words[0] >> 31 else 32, 'rd': words[0] & 31,
                'value': hi << 16 | lo}, 'u32'
    if len(data) == 4 and (words[0] & 0xFF201FE0) == 0x1E201000 and (words[0] >> 22) & 3 in (0, 1):
        w = words[0]
        double = (w >> 22) & 3 == 1
        return {'insn': 'fmov', 'width': 64 if double else 32, 'rd': w & 31,
                'value': _fmov_value((w >> 13) & 0xFF)}, 'f64' if double else 'f32'
    return None


class _Cheat:
    """One converted cheat of one file, before cheats of several builds are merged."""

    def __init__(self, item, key, rep):
        self.item, self.key, self.rep = item, key, rep
        self.hold = None
        self.press = None          # buttons of a block that patches without an else branch: run = "action"
        self.needs_master = False
        self.ops = []          # (kind, table)
        self.values = {}
        self.raw = {'syntax': 'atmosphere', 'build': key, 'code': item.entry.code}


def _master_ranges(master):
    """The main module byte ranges a master entry writes, or None when it is not known what it writes."""
    if master is None:
        return []
    d = master.derived
    if isinstance(d, atm.Unclear):
        return None
    return [(s.place.offset, s.place.offset + s.width) for s in d.stores + (d.released or [])
            if s.place.region == 'main' and not s.place.path]


def _depends(it, c, master, ranges):
    """Whether a cheat depends on the master entry of its file: it reads registers the master sets, or it touches,
    reads through or branches into bytes the master writes. A cheat that stays raw code, or a master whose effect is
    not known, counts as dependent, as in Atmosphère, where the master always runs first."""
    if master is None:
        return False
    d = it.derived
    if ranges is None or isinstance(d, atm.Unclear) or not c.ops:
        return True
    if d.reads & master.derived.writes:
        return True

    def inside(offset, width=1):
        return any(lo < offset + width and offset < hi for lo, hi in ranges)

    places = [(s.place, s.width) for s in d.stores + (d.released or [])]
    if d.condition is not None:
        places.append((d.condition.place, d.condition.width))
    for place, width in places:
        if place.region != 'main':
            continue
        if inside(place.offset, 8 if place.path else width):
            return True
    for s in d.stores:
        if s.place.region == 'main' and not s.place.path and s.place.offset % 4 == 0:
            data = s.data
            for i in range(0, len(data) - len(data) % 4, 4):
                target = arm64.pc_relative_target(int.from_bytes(data[i:i + 4], 'little'), s.place.offset + i)
                if target is not None and inside(target, 4):
                    return True
    return False


def _convert_one(src, key, report, asm, values):
    module = src.module
    items = _read(src, report, key)
    restore, originals = _restores(items, module)
    master = next((it for it in items if it.master and id(it) not in restore), None)
    ranges = _master_ranges(master)
    out = []
    for it in items:
        rep = CheatReport('', it.name, key, 'raw')
        if id(it) in restore:
            rep.outcome, rep.why = 'restore', 'it only puts back the original code'
            report.cheats.append(rep)
            continue
        c = _Cheat(it, key, rep)
        if isinstance(it.derived, atm.Unclear):
            rep.why = it.derived.why
        else:
            try:
                _core(c, it.derived, module, originals, asm, values)
                rep.outcome = 'pattern' if rep.patterns and not rep.fixed else 'build'
            except atm.Unclear as u:
                c.ops, c.values, c.hold, c.press = [], {}, None, None
                rep.why, rep.outcome, rep.patterns, rep.fixed, rep.values, rep.notes = u.why, 'raw', 0, 0, 0, []
        if not it.master:
            c.needs_master = _depends(it, c, master, ranges)
        out.append(c)
        report.cheats.append(rep)
    return out


def _core(c, d, module, originals, asm, values):
    rep, key = c.rep, c.key
    buttons = None
    if d.keys:
        buttons = atm.keys_to_buttons(d.keys)
        if buttons is None:
            raise atm.Unclear('a button mask with bits that have no name')
    code, ops = [], []
    for s in d.stores:
        if s.place.region == 'main' and not s.place.path:
            if module is None:
                if _original(None, originals, s.place.offset, s.width) is None:
                    raise atm.Unclear('a write into the main module: without the module, code and data look alike')
                code.append(s)
            elif _in_code(module, s.place.offset, s.width):
                code.append(s)
            elif module.section_of(s.place.offset) is None:
                raise atm.Unclear('a write outside the main module')
            else:
                ops.append(('write', {'at': _location(s.place, key), 'type': f'u{8 * s.width}', 'value': s.value}))
                rep.fixed += 1
        else:
            ops.append(('write', {'at': _location(s.place, key), 'type': f'u{8 * s.width}', 'value': s.value}))
            rep.fixed += 1
    for s in d.released or []:
        if s.place.region != 'main' or s.place.path:
            raise atm.Unclear('it writes data when the buttons are released')
        if _original(module, originals, s.place.offset, s.width) != s.data:
            raise atm.Unclear('the release branch writes other bytes than the original code')
    patches = []
    for off, data in _merge_runs(code):
        expect = _original(module, originals, off, len(data))
        if expect is None:
            raise atm.Unclear('no original bytes for every byte it writes')
        if expect == data:
            rep.notes.append(f'main+0x{off:X} already holds those bytes; the patch stays, as the source writes them')
        patches.append(_patch(c, off, data, expect, module, asm, values))
    if not patches and not ops:
        raise atm.Unclear('it writes only what is already there')
    if buttons is not None:
        if d.released is None and patches:
            # Atmosphère patches at the press and leaves the patch when the buttons are let go: run = "action".
            if ops:
                raise atm.Unclear('a button block without an else branch that patches code and writes data')
            c.press = buttons
        else:
            c.hold = buttons
    c.ops = [('patch', p) for p in patches] + ops
    if d.condition is not None:
        cond = d.condition
        if cond.place.region == 'main' and module is not None and _in_code(module, cond.place.offset, cond.width):
            raise atm.Unclear('a condition on the code itself')
        c.ops.insert(0, ('when', {'at': _location(cond.place, key), 'type': f'u{8 * cond.width}', 'op': cond.op,
                                  'value': cond.value}))


def _patch(c, off, data, expect, module, asm, values, name='main'):
    rep, key = c.rep, c.key
    loc = None
    if module is not None:
        if module.excluded(off, len(data)):
            rep.notes.append(f'{name}+0x{off:X}: the module\'s bytes there are not its own, so no pattern')
        elif arm64.relative_outside(data, off, len(data)):
            rep.notes.append(f'{name}+0x{off:X}: the new code branches or refers outside itself, so it fits this '
                             'build only')
        elif module.text_size is not None and off >= module.text_size and module.section_of(off) == 'code':
            rep.notes.append(f'{name}+0x{off:X}: code written after the end of the code, kept for this build')
        else:
            p = make_pattern(module, off, cover=min(4, len(data)))
            if p is None:
                rep.notes.append(f'{name}+0x{off:X}: no unique pattern of up to 64 bytes')
            else:
                loc = p.location(name)
                if arm64.moving_bytes(module.image, off, off + len(expect)):
                    rep.notes.append(f'{name}+0x{off:X}: the original code refers to another place, so `expect` fits '
                                     'this build only')
    if loc is None:
        loc = {'module': name, 'offsets': {key: off}}
        rep.fixed += 1
    else:
        rep.patterns += 1
    table = {'at': loc, 'expect': hex_bytes(expect)}
    encoded = _encode_of(data) if values else None
    if encoded is not None:
        enc_table, vtype = encoded
        vid = 'value' if 'value' not in c.values else f'value-{len(c.values) + 1}'
        c.values[vid] = {'type': vtype, 'default': enc_table['value']}
        table['encode'] = dict(enc_table, value=f'${vid}')
        rep.values += 1
    else:
        table['bytes'] = hex_bytes(data)
    if asm and module is not None and module.section_of(off) == 'code':
        a, b = disasm.arm64(expect, off), disasm.arm64(data, off)
        if a is not None and b is not None:
            table['asm'] = {'expect': a, 'bytes': b}
    return table


# ---- several files --------------------------------------------------------------------------------------------------

def _shape(ops):
    """The operations without the offsets of their build: what must agree for two builds' cheats to be one."""
    out = []
    for kind, table in ops:
        t = dict(table)
        loc = dict(t.get('at', {}))
        if 'offsets' in loc:
            loc['offsets'] = None
        t['at'] = loc
        t.pop('asm', None)
        out.append((kind, repr(sorted(t.items(), key=lambda kv: kv[0]))))
    return out


def _mergeable(a, b):
    return a.hold == b.hold and a.press == b.press and a.needs_master == b.needs_master and a.values == b.values and _shape(a.ops) == _shape(b.ops)


def _merge_into(a, b):
    for (_, ta), (_, tb) in zip(a.ops, b.ops):
        if 'offsets' in ta.get('at', {}):
            ta['at']['offsets'].update(tb['at']['offsets'])


def from_atmosphere(sources, *, game_id, platform='switch', game_name=None, name=None, origin=None, authors=None,
                    asm=True, values=True):
    """A Conversion of one or more Atmosphère cheat files, each for one build of the same game.

    `origin` names where the files came from, such as a ZIP or folder name. `authors` are the authors to name when
    the files credit nobody. `asm` fills `asm` fields where Capstone is installed; `values` turns movz and fmov
    patches into values the player sets.
    """
    if isinstance(sources, Source):
        sources = [sources]
    report = Report()
    builds, per_source = {}, []
    ids = [src.build_id.upper() for src in sources]
    if len(set(ids)) != len(ids):
        raise ConvertError('two files are for the same build ID')
    versions = [src.version for src in sources if src.version]
    for src in sources:
        bid = src.build_id.upper()
        shared = src.version is not None and versions.count(src.version) > 1
        key = src.build_key or (bid if shared else src.version) or bid
        if key in builds:
            raise ConvertError(f'two files name the build {key!r}')
        if src.module is not None and src.module.short_id and not (src.module.short_id.startswith(bid[:16]) or
                                                                   bid.startswith(src.module.short_id)):
            report.warnings.append(f'the module of build {key} is {src.module.short_id}, its cheat file {bid}')
        build = {'version': src.version} if src.version and src.version != key else {}
        if shared:
            report.warnings.append(f'builds {bid} and another one are both version {src.version}: named by their '
                                   'build IDs')
        build['modules'] = {'main': bid}
        builds[key] = build
        per_source.append(_convert_one(src, key, report, asm, values))

    # Cheats of several builds with the same name become one where they do the same; masters first, so that a cheat
    # is only one cheat across builds whose masters are one cheat too.
    merged, master_of = [], {}
    for want_master in (True, False):
        by_name = {}
        for cheats in per_source:
            for c in cheats:
                if c.item.master != want_master:
                    continue
                target = next((g for g in by_name.get(c.item.name, []) if c.key not in {x.key for x in g}
                               and _mergeable(g[0], c)
                               and (want_master or master_of.get(g[0].key) is master_of.get(c.key))), None)
                if target is None:
                    target = [c]
                    merged.append(target)
                    by_name.setdefault(c.item.name, []).append(target)
                else:
                    target.append(c)
                if want_master:
                    master_of[c.key] = target
    order = {id(c): i for i, c in enumerate(c for cheats in per_source for c in cheats)}
    merged.sort(key=lambda g: min(order[id(c)] for c in g))

    taken, cheats, ids = set(), [], {}
    for g in merged:
        ids[id(g)] = slug(g[0].item.name, taken)
        for c in g:
            c.rep.id = ids[id(g)]
    for g in merged:
        first = g[0]
        for c in g[1:]:
            _merge_into(first, c)
        t = {'id': ids[id(g)], 'name': first.item.name}
        if first.item.group:
            t['group'] = first.item.group
        if first.item.master:
            t['master'] = True
        elif master_of.get(first.key) is not None and any(c.needs_master for c in g):
            t['requires'] = [ids[id(master_of[first.key])]]
        if first.press:
            t['run'], t['press'] = 'action', first.press
        if first.hold:
            t['hold'] = first.hold
        if first.values:
            t['values'] = dict(first.values)
        for kind, table in first.ops:
            t.setdefault(kind, []).append(table)
        t['raw'] = [c.raw for c in g]
        cheats.append(t)
    if not cheats:
        raise atm.AtmosphereError('the file has no cheat with code')

    doc = {'format': 'mncf/1', 'name': name or (f'{game_name} cheats' if game_name else 'Converted cheats')}
    found = authors_of(report.credits)
    if found or authors:
        doc['authors'] = found or list(authors)
    about = []
    if report.credits:
        about.append('Credits in the source: ' + '; '.join(report.credits))
    if report.notes:
        about.append('Lines in the source that are not cheat code: ' + '; '.join(report.notes))
    if report.unreadable:
        about.append('Entries whose code could not be read, left out: ' + '; '.join(report.unreadable))
    if about:
        doc['about'] = '\n'.join(about)
    doc['origin'] = {'format': 'atmosphere'}
    if origin:
        doc['origin']['name'] = origin
    site = site_of(report.credits)
    if site:
        doc['origin']['site'] = site
    exts = ['raw/1'] + (['arm64-imm/1'] if any(c.get('values') for c in cheats) else [])
    doc['extensions'] = {'optional': exts}
    doc['game'] = {'platform': platform, 'id': game_id.upper()}
    if game_name:
        doc['game']['name'] = game_name
    doc['builds'] = builds
    doc['cheat'] = cheats
    return Conversion(doc, dumps(doc), report)


# ---- patch files: IPS, IPS32, IPSwitch ------------------------------------------------------------------------------

@dataclass
class PatchSource:
    """One patch file of a code module: its bytes, the module it patches, and what else is known.

    A patch needs `expect`, which a patch file does not hold, so the module is required: the original bytes come from
    it. The build ID comes from the file itself (`@nsobid-`), else from its name (`<build ID>.ips`), else from
    `build_id`, else from the module.
    """
    data: bytes
    module: object = None      # the module.CodeModule it patches
    file_name: str = None      # the file's name, such as `<build ID>.ips`
    set_name: str = None       # the patch set: its folder in exefs_patches/, or the mod's name
    module_name: str = 'main'  # main, sdk or subsdk0 to subsdk9
    build_id: str = None
    version: str = None
    build_key: str = None


class _PatchItem:
    """What the several-builds merge needs of a cheat's source entry."""
    master = False

    def __init__(self, name, group):
        self.name, self.group = name, group


def _set_name(file_name):
    """The patch set a file belongs to, from its path: the folder it is in (`exefs_patches/<set>/<build ID>.ips`, or a
    mod's folder above `exefs/`), else its own name where that is no build ID, else "Patch"."""
    parts = [p for p in (file_name or '').replace('\\', '/').split('/') if p]
    if not parts:
        return 'Patch'
    for folder in reversed(parts[:-1]):
        if folder.lower() not in ('exefs', 'exefs_patches', 'atmosphere', 'contents') and \
                not re.match(r'^[0-9A-Fa-f]{16}\Z', folder):
            return folder
    stem = parts[-1].rsplit('.', 1)[0]
    return 'Patch' if not stem or patches_mod.build_id_of(parts[-1]) else stem


def _patch_cheat(item, key, rep, runs, module, mname, asm, values):
    c = _Cheat.__new__(_Cheat)
    c.item, c.key, c.rep = item, key, rep
    c.hold = c.press = None
    c.needs_master = False
    c.ops, c.values, c.raw = [], {}, None
    for off, data in runs:
        pieces, at = [], off
        while at < off + len(data):                   # one run for each section it crosses
            sec = module.section_of(at)
            stop = min(off + len(data), module.sections[sec][1] if sec else at + 1)
            pieces.append((sec, at, data[at - off:stop - off]))
            at = stop
        for sec, at, piece in pieces:
            if sec is None:
                raise atm.Unclear(f'{mname}+0x{at:X} is in no section of the module')
            if module.excluded(at, len(piece)):
                raise atm.Unclear(f'{mname}+0x{at:X}: the module\'s bytes there are not its own, so there is no '
                                  '`expect`')
            expect = module.image[at:at + len(piece)]
            if expect == piece:
                rep.notes.append(f'{mname}+0x{at:X} already holds those bytes; the patch stays, as the source '
                                 'writes them')
            if sec == 'data':
                c.ops.append(('write', {'at': {'module': mname, 'offsets': {key: at}}, 'type': 'bytes',
                                        'value': hex_bytes(piece), 'expect': hex_bytes(expect)}))
                rep.fixed += 1
            else:
                c.ops.append(('patch', _patch(c, at, piece, expect, module, asm, values, mname)))
    return c


def from_patches(sources, *, game_id, platform='switch', game_name=None, name=None, origin=None, authors=None,
                 asm=True, values=True):
    """A Conversion of patch files (IPS, IPS32 or IPSwitch text) of the same game, for one or more builds.

    Each IPS file becomes one cheat, named by its patch set, that is on by default, as a loader applies it whenever
    it is there. Each patch of an IPSwitch file becomes one cheat, named by its comment, on by default where it is
    `@enabled`. A write into code or read-only data becomes a patch, one into data a write that is made only while
    the place holds its original bytes, as a loader sets them only once.
    """
    if isinstance(sources, PatchSource):
        sources = [sources]
    report = Report()
    builds, per_source, formats = {}, [], []
    read = []
    for src in sources:
        f = patches_mod.read(src.data)
        formats.append(f.format)
        bid = f.build_id or (patches_mod.build_id_of(src.file_name) if src.file_name else None) or \
            (patches_mod.trim_build_id(src.build_id) if src.build_id else None) or \
            (patches_mod.trim_build_id(src.module.build_id) if src.module is not None and src.module.build_id
             else None)
        if src.module is None:
            raise ConvertError(f'{src.file_name or "a patch file"}: a patch needs the module it patches, for `expect`')
        if bid is None:
            raise ConvertError(f'{src.file_name or "a patch file"}: no build ID in the file, its name or the module')
        read.append((src, f, bid))
    versions = {}
    for src, _, bid in read:
        if src.version:
            versions.setdefault((src.version, src.module_name), set()).add(bid)
    for src, f, bid in read:
        shared = bool(src.version) and len(versions[(src.version, src.module_name)]) > 1
        key = src.build_key or (bid if shared else src.version) or bid
        mid = src.module.build_id
        if mid and not (mid.upper().startswith(bid) or bid.startswith(mid.upper())):
            report.warnings.append(f'the module of build {key} is {mid.upper()}, its patch file {bid}')
        build = builds.setdefault(key, {'version': src.version} if src.version and src.version != key else {})
        mods = build.setdefault('modules', {})
        if mods.get(src.module_name, bid) != bid:
            raise ConvertError(f'build {key!r} has two build IDs for {src.module_name}')
        mods[src.module_name] = bid
        set_name = src.set_name or _set_name(src.file_name)
        for n, line in f.notes:
            if line not in report.notes:
                report.notes.append(line)
        for n, line in f.comments:
            if CREDIT_NAME.search(line) and line not in report.credits:
                report.credits.append(line)
        group = set_name if len(f.patches) > 1 else None
        out = []
        for i, p in enumerate(f.patches, 1):
            named = p.name if p.name and not CREDIT_NAME.search(p.name) else ''
            title = named or (set_name if len(f.patches) == 1 else f'{set_name} {i}')
            rep = CheatReport('', title, key, 'pattern')
            runs, dropped = patches_mod.image_runs(p, len(src.module.image))
            rep.notes += [f'left out {d}' for d in dropped]
            try:
                if not runs:
                    raise atm.Unclear('it writes nothing into the module')
                c = _patch_cheat(_PatchItem(title, group), key, rep, runs, src.module, src.module_name, asm, values)
            except atm.Unclear as u:
                rep.outcome, rep.why = 'unreadable', u.why
                rep.patterns, rep.fixed, rep.values = 0, 0, 0
                rep.notes = [f'left out {d}' for d in dropped]
                report.unreadable.append(f'{title} ({u.why})')
                report.cheats.append(rep)
                continue
            c.default = p.enabled
            rep.outcome = 'pattern' if rep.patterns and not rep.fixed else 'build'
            out.append(c)
            report.cheats.append(rep)
        per_source.append(out)

    merged, by_name = [], {}
    for cheats in per_source:
        for c in cheats:
            target = next((g for g in by_name.get(c.item.name, []) if c.key not in {x.key for x in g}
                           and _mergeable(g[0], c) and g[0].default == c.default
                           and g[0].item.group == c.item.group), None)
            if target is None:
                target = [c]
                merged.append(target)
                by_name.setdefault(c.item.name, []).append(target)
            else:
                target.append(c)
    taken, cheats = set(), []
    for g in merged:
        first = g[0]
        cid = slug(first.item.name, taken)
        for c in g:
            c.rep.id = cid
            if c is not first:
                _merge_into(first, c)
        t = {'id': cid, 'name': first.item.name}
        if first.item.group:
            t['group'] = first.item.group
        if first.default:
            t['default'] = True
        if first.values:
            t['values'] = dict(first.values)
        for kind, table in first.ops:
            t.setdefault(kind, []).append(table)
        cheats.append(t)
    if not cheats:
        raise ConvertError('no patch of the files could be converted: ' + '; '.join(report.unreadable or ['none']))

    doc = {'format': 'mncf/1', 'name': name or (f'{game_name} patches' if game_name else 'Converted patches')}
    found = authors_of(report.credits)
    if found or authors:
        doc['authors'] = found or list(authors)
    about = []
    if report.credits:
        about.append('Credits in the source: ' + '; '.join(report.credits))
    if report.notes:
        about.append('Lines in the source that are not patches: ' + '; '.join(report.notes))
    if report.unreadable:
        about.append('Patches that could not be converted, left out: ' + '; '.join(report.unreadable))
    if about:
        doc['about'] = '\n'.join(about)
    doc['origin'] = {'format': 'ips' if formats[0] == 'ips32' else formats[0]}
    if origin:
        doc['origin']['name'] = origin
    site = site_of(report.credits)
    if site:
        doc['origin']['site'] = site
    if any(c.get('values') for c in cheats):
        doc['extensions'] = {'optional': ['arm64-imm/1']}
    doc['game'] = {'platform': platform, 'id': game_id.upper()}
    if game_name:
        doc['game']['name'] = game_name
    doc['builds'] = builds
    doc['cheat'] = cheats
    return Conversion(doc, dumps(doc), report)


# ---- back to Atmosphère ---------------------------------------------------------------------------------------------

@dataclass
class BackReport:
    written: list = field(default_factory=list)     # (cheat id, 'raw' or 'core')
    skipped: list = field(default_factory=list)     # (cheat id, why)
    reasons: dict = field(default_factory=dict)     # cheat id -> the stable reason it was skipped (section 15)


@dataclass
class BackFile:
    """One build of a file as an Atmosphère cheat file, or the reason it cannot be one."""
    build: str
    name: str = None          # <build ID>.txt
    text: str = None
    report: BackReport = None
    reason: str = None        # atmosphere:module for a build that names no main module


def to_atmosphere_all(f, modules=None, prefer='raw', choices=None, prefer_cheats=None):
    """Every build of a loaded file as an Atmosphère cheat file, in the file's order: a list of BackFile.
    `modules` maps build keys to the main module of that build, for the cheats that find their place by pattern."""
    out = []
    for key, build in f.builds.items():
        if 'main' not in build.modules:
            out.append(BackFile(key, reason='atmosphere:module'))
            continue
        name, text, report = to_atmosphere(f, key, (modules or {}).get(key), prefer, choices, prefer_cheats)
        out.append(BackFile(key, name, text, report))
    return out


ATM_ENTRIES = 127       # entries of an Atmosphère file besides the master: dmnt holds 0x80, the first the master
ATM_WORDS = 0x100       # words of one entry


def _word_count(lines):
    return sum(len(line.split()) for line in lines)


def to_atmosphere(f, build_key, module=None, prefer='raw', choices=None, prefer_cheats=None):
    """(file name, text, BackReport): the cheats of a loaded file as an Atmosphère cheat file for one build.

    A build the file does not name is written from its patterns, with the module of that build; its file name is the
    module's build ID. `choices` gives the values the player chose (mncf.Choices); else the defaults are written.
    `prefer` is "raw" or "core" for every cheat, and `prefer_cheats` maps cheat ids to "raw" or "core" where one
    cheat should differ, such as one whose value the player set. The file keeps Atmosphère's limits: one master entry
    with the code of every master cheat, and up to 127 other entries of up to 256 words each.
    """
    modes = dict(prefer_cheats or {})
    for mode in [prefer, *modes.values()]:
        if mode not in ('raw', 'core'):
            raise ValueError(f'prefer is "raw" or "core", not {mode!r}')
    build = f.builds.get(build_key)
    if build is not None:
        main_id = build.modules.get('main')
        if main_id is None:
            raise ValueError(f'build {build_key!r} names no main module')
    elif module is not None and module.short_id:
        main_id = module.short_id
    else:
        raise ValueError(f'the file has no build {build_key!r}, and no module of it is given')
    chosen = choices.values if choices is not None else {}
    report = BackReport()
    entries, group, master, master_lines = [], None, None, []

    def leave_out(c, why, code):
        report.skipped.append((c.id, why))
        report.reasons[c.id] = code

    for c in sorted(f.cheats, key=lambda c: not c.master):
        if c.problem:
            report.skipped.append((c.id, c.problem))
            report.reasons[c.id] = c.problem
            continue
        raw = next((r for r in c.raw if r.build == build_key and r.syntax == 'atmosphere'), None)
        raw_lines = [line.strip() for line in raw.code.strip().splitlines() if line.strip()] if raw else None
        lines, how = None, None
        if raw is None and not c.ops:
            report.skipped.append((c.id, 'only raw code, for other builds'))
            report.reasons[c.id] = 'build-unknown'
            continue
        if raw is not None and modes.get(c.id, prefer) == 'raw':
            lines, how = raw_lines, 'raw'
        else:
            try:
                words = _core_words(c, build_key, module, chosen)
                if len(words) > ATM_WORDS:
                    raise atm.Unclear(f'{len(words)} words, more than an Atmosphère entry holds',
                                      'atmosphere:too-long')
                lines, how = atm.format_words(words), 'core'
            except atm.Unclear as u:
                if raw is None:
                    leave_out(c, u.why, u.code or 'atmosphere:unclear')
                    continue
                lines, how = raw_lines, 'raw'
        if _word_count(lines) > ATM_WORDS:
            leave_out(c, f'{_word_count(lines)} words, more than an Atmosphère entry holds', 'atmosphere:too-long')
            continue
        if c.master:
            # Atmosphère runs one master entry: the code of every master cheat goes into it, in file order.
            if _word_count(master_lines) + _word_count(lines) > ATM_WORDS:
                leave_out(c, 'the master entry would hold more than 256 words', 'atmosphere:too-long')
                continue
            master = master or c.name
            master_lines += lines
            report.written.append((c.id, how))
            continue
        # A new group starts with its name as an empty entry; a group is ended by an entry with only 20000001.
        cheat_group = c.group or None
        marks = []
        if cheat_group != group:
            marks = [(cheat_group, False, [])] if cheat_group else [(group, False, ['20000001'])]
        if len(entries) + len(marks) + 1 > ATM_ENTRIES:
            leave_out(c, 'more entries than an Atmosphère file holds', 'atmosphere:too-many')
            continue
        entries += marks
        group = cheat_group
        entries.append((c.name, False, lines))
        report.written.append((c.id, how))
    if master is not None:
        entries.insert(0, (master, True, master_lines))
    return f'{main_id[:16].upper()}.txt', atm.write_file(entries), report


def _base(loc, build_key, module):
    if loc.kind in ('module', 'region'):
        return loc.offset
    if loc.kind in ('module-builds', 'region-builds'):
        if build_key not in loc.offsets:
            raise atm.Unclear('a location without an offset for this build', 'build-unknown')
        return loc.offsets[build_key]
    if loc.kind == 'pattern':
        if module is None:
            raise atm.Unclear('a pattern, and no module to search', 'atmosphere:no-module')
        from .evaluate import find_all
        found = find_all(module.reader_module(), loc.find, loc.section, 2)
        if len(found) != 1:
            raise atm.Unclear('a pattern that does not match exactly once in the module',
                              'pattern-ambiguous' if found else 'pattern-not-found')
        return found[0] + loc.offset
    raise atm.Unclear('an absolute address', 'atmosphere:address')


def _place(loc, build_key, module):
    if loc.module and loc.module != 'main':
        raise atm.Unclear('a module other than main', 'atmosphere:module')
    region = 'main' if loc.module else loc.region
    if region not in atm.REGION_CODES:
        raise atm.Unclear(f'the region {region}', 'atmosphere:region')
    path = tuple(p & atm.MASK64 for p in loc.path) if loc.path else None
    return atm.Place(region, _base(loc, build_key, module), path)


def _stores(place, data):
    out, i = [], 0
    while i < len(data):
        width = next(w for w in (4, 2, 1) if w <= len(data) - i)
        out.append(atm.Store(place.plus(i), width, int.from_bytes(data[i:i + width], 'little')))
        i += width
    return out


def _default(c, v, chosen=None):
    """A value of the cheat: a number, the value the player chose, or the default. A chosen value that the player
    cannot choose makes the cheat unclear, as the reader refuses it (value-out-of-range)."""
    if not isinstance(v, ValueRef):
        return v
    key = f'{c.id}.{v.name}'
    if key not in (chosen or {}):
        return c.values[v.name].default
    try:
        return _value(c, v, Choices(values=chosen))
    except _Refused:
        raise atm.Unclear('a chosen value the cheat cannot take', 'value-out-of-range') from None


def _check_inside(module, place, length):
    """With the module, a place in main lies inside it with every byte read or written there: for a pointer path,
    the pointer at its base."""
    if module is not None and place.region == 'main' and place.offset + (8 if place.path else length) > \
            len(module.image):
        raise atm.Unclear('a place past the end of the module', 'outside')


def _check_module(module, place, expect, data):
    """With the module, the bytes a patch or stub writes over must be `expect`, or `data` already: Atmosphère code
    writes without looking. Not where the module's bytes are not its own."""
    if module is None or place.path or place.region != 'main':
        return
    if place.offset + len(expect) > len(module.image):
        raise atm.Unclear('a place past the end of the module', 'outside')
    here = module.image[place.offset:place.offset + len(expect)]
    if here not in (expect, data) and not module.excluded(place.offset, len(expect)):
        raise atm.Unclear('the module holds other bytes than `expect`', 'expect-mismatch')


def _core_words(c, build_key, module, chosen=None):
    if c.run not in ('on', 'action'):
        raise atm.Unclear(f'run = "{c.run}" has no Atmosphère form',
                          'atmosphere:monitor' if c.run == 'monitor' else 'atmosphere:run')
    if c.toggle:
        raise atm.Unclear('toggle buttons have no Atmosphère form', 'atmosphere:toggle')
    stores, restores, conds = [], [], []
    for op in c.ops:
        if isinstance(op, Patch):
            place = _place(op.at, build_key, module)
            _check_inside(module, place, len(op.expect))
            if op.encode is not None:
                e = op.encode
                data = enc.arm64_imm(e['insn'], e['width'], e['rd'], _default(c, e['value'], chosen))
                if data is None:
                    raise atm.Unclear('a value its instruction cannot hold', 'value-out-of-range')
            else:
                data = op.bytes
            _check_module(module, place, op.expect, data)
            stores += _stores(place, data)
            restores += _stores(place, op.expect)
        elif isinstance(op, Write):
            place = _place(op.at, build_key, module)
            _check_inside(module, place, len(op.value) if op.type == 'bytes' else type_size(op.type))
            v = _default(c, op.value, chosen)
            if isinstance(v, bool):
                v = int(v)
            if op.type != 'bytes' and not fits(op.type, v):
                raise atm.Unclear('a value its write cannot hold', 'value-out-of-range')
            data = op.value if op.type == 'bytes' else encode_value(op.type, v, 'little')
            stores += _stores(place, data)
        elif isinstance(op, Override):
            if op.expect is None:
                raise atm.Unclear('an override without expect', 'atmosphere:override')
            stub = enc.arm64_stub(op.action, int(_default(c, op.value, chosen) or 0))
            if len(stub) > len(op.expect):
                raise atm.Unclear('an override whose stub is longer than expect', 'atmosphere:override')
            place = _place(op.function, build_key, module)
            _check_inside(module, place, len(stub))
            _check_module(module, place, op.expect[:len(stub)], stub)
            stores += _stores(place, stub)
            restores += _stores(place, op.expect[:len(stub)])
        elif isinstance(op, Condition):
            conds.append(op)
        elif isinstance(op, Read):
            raise atm.Unclear('a monitor has no Atmosphère form', 'atmosphere:monitor')
    if len(conds) > 1 or (conds and (c.match != 'all' or c.trigger != 'while' or c.interval != 1)):
        raise atm.Unclear('conditions that Atmosphère cannot say', 'atmosphere:condition')
    body = [w for s in stores for w in atm.store_words(s.place, s.width, s.value)]
    if conds:
        cond = conds[0]
        place = _place(cond.at, build_key, module)
        _check_inside(module, place, type_size(cond.type) if cond.type in ('u8', 'u16', 'u32', 'u64') else 1)
        value = _default(c, cond.value, chosen)
        if place.path or cond.type not in ('u8', 'u16', 'u32', 'u64') or cond.op not in atm.CONDITION_CODES or \
                not isinstance(value, int) or isinstance(value, bool):
            raise atm.Unclear('a condition that Atmosphère cannot say', 'atmosphere:condition')
        body = atm.condition_words(place, int(cond.type[1:]) // 8, cond.op, value) + body + [0x20000000]
    if c.run == 'action':
        if c.hold or any(isinstance(op, Write) for op in c.ops):
            raise atm.Unclear('an action with writes or hold has no Atmosphère form', 'atmosphere:action')
        words = [0x80000000 | atm.buttons_to_keys(c.press)] + body + [0x20000000]
    elif c.hold:
        words = [0x80000000 | atm.buttons_to_keys(c.hold)] + body
        if restores:
            words += [0x21000000] + [w for s in restores for w in atm.store_words(s.place, s.width, s.value)]
        words += [0x20000000]
    else:
        words = body
    if not words:
        raise atm.Unclear('nothing to write', 'atmosphere:empty')
    return words


def compare(original, regenerated, effects=False):
    """(same, differ, missing): the names of the original's entries whose code the other file has word for word,
    has otherwise, or lacks. With effects, code that writes the same bytes at the same places under the same buttons
    and condition, and puts the same back on release, counts as the same."""
    def table(text):
        return {(e.name, e.master): e for e in atm.parse(text, []) if e.words and not atm.is_noop(e) and not e.broken}
    a, b = table(original), table(regenerated)
    same, differ, missing = [], [], []
    for k, e in a.items():
        if k not in b:
            missing.append(k[0])
        elif b[k].words == e.words or (effects and _effect(b[k]) is not None and _effect(b[k]) == _effect(e)):
            same.append(k[0])
        else:
            differ.append(k[0])
    return same, differ, missing


def _effect(entry):
    try:
        d = atm.derive(entry)
    except atm.Unclear:
        return None
    written = {}
    for s in d.stores:
        for i, b in enumerate(s.data):
            p = s.place.plus(i)
            written[(p.region, p.offset, p.path)] = b
    cond = (d.condition.place, d.condition.width, d.condition.op, d.condition.value) if d.condition else None
    return d.keys, cond, d.released is not None, written


__all__ = ['Source', 'PatchSource', 'ConvertError', 'Conversion', 'Report', 'CheatReport', 'BackReport',
           'BackFile', 'from_atmosphere', 'from_patches', 'to_atmosphere', 'to_atmosphere_all', 'compare',
           'authors_of', 'slug']
