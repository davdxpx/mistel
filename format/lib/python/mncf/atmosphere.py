"""Atmosphère cheat files (the dmnt cheat code of the Switch): reading, decoding, understanding and writing.

A file is a list of entries: `[Name]` or `{Name}` (the master entry) followed by opcodes, words of 8 hexadecimal
digits. `derive` reads an entry's opcodes the way the cheat machine runs them and gives the plain writes they make,
with the button mask and the memory condition around them, wherever the code says that plainly.
"""
import re
from dataclasses import dataclass, field

REGIONS = {0: 'main', 1: 'heap', 2: 'alias', 3: 'aslr'}
REGION_CODES = {v: k for k, v in REGIONS.items()}
KEYS = [('A', 0x1), ('B', 0x2), ('X', 0x4), ('Y', 0x8), ('LSTICK', 0x10), ('RSTICK', 0x20), ('L', 0x40),
        ('R', 0x80), ('ZL', 0x100), ('ZR', 0x200), ('PLUS', 0x400), ('MINUS', 0x800), ('DLEFT', 0x1000),
        ('DUP', 0x2000), ('DRIGHT', 0x4000), ('DDOWN', 0x8000), ('LSTICK_LEFT', 0x10000), ('LSTICK_UP', 0x20000),
        ('LSTICK_RIGHT', 0x40000), ('LSTICK_DOWN', 0x80000), ('RSTICK_LEFT', 0x100000), ('RSTICK_UP', 0x200000),
        ('RSTICK_RIGHT', 0x400000), ('RSTICK_DOWN', 0x800000), ('SL', 0x1000000), ('SR', 0x2000000)]
CONDITIONS = {1: '>', 2: '>=', 3: '<', 4: '<=', 5: '==', 6: '!='}
CONDITION_CODES = {v: k for k, v in CONDITIONS.items()}
WIDTHS = (1, 2, 4, 8)
MASK64 = (1 << 64) - 1
_WORD = re.compile(r'^[0-9A-Fa-f]{8}\Z')


class AtmosphereError(ValueError):
    """A file that is not an Atmosphère cheat file."""
    code = 'atmosphere'


class Unclear(Exception):
    """The opcodes do something the core operations of .mncf cannot say plainly. `why` says what."""

    def __init__(self, why):
        super().__init__(why)
        self.why = why


@dataclass
class Entry:
    name: str
    master: bool
    line: int
    lines: list = field(default_factory=list)     # the code lines as written, stripped
    words: list = field(default_factory=list)

    @property
    def code(self):
        return '\n'.join(self.lines) + '\n' if self.lines else ''


def parse(text):
    """The entries of an Atmosphère cheat file."""
    entries = []
    current = None
    for n, raw in enumerate(text.lstrip('﻿').splitlines(), 1):
        line = raw.strip()
        while line:
            if line[0] in '[{':
                close = ']' if line[0] == '[' else '}'
                end = line.find(close)
                if end < 0:
                    raise AtmosphereError(f'line {n}: the name has no closing {close}')
                current = Entry(line[1:end].strip(), line[0] == '{', n)
                entries.append(current)
                line = line[end + 1:].strip()
                continue
            tokens = line.split()
            for tok in tokens:
                if not _WORD.match(tok):
                    raise AtmosphereError(f'line {n}: {tok!r} is not a word of 8 hexadecimal digits')
            if current is None:
                raise AtmosphereError(f'line {n}: code before the first name')
            current.lines.append(line)
            current.words.extend(int(t, 16) for t in tokens)
            line = ''
    return entries


# ---- opcodes --------------------------------------------------------------------------------------------------------

@dataclass
class Op:
    kind: str          # store, begin-cond, end-cond, loop, load-const, load-mem, store-reg-imm, arith, keys,
    words: tuple       # arith-reg, store-reg, reg-cond, save, save-mask, static-reg, pause, resume, log
    f: dict


def _nib(w, i):
    """The i-th hexadecimal digit of a word, from the left."""
    return (w >> (28 - 4 * i)) & 0xF


def decode(words):
    """The opcodes in a list of words."""
    ops, i = [], 0

    def take(n):
        nonlocal i
        if i + n > len(words):
            raise Unclear('the code ends inside an opcode')
        got = tuple(words[i:i + n])
        i += n
        return got

    while i < len(words):
        w = words[i]
        t = _nib(w, 0)
        if t == 0x0:
            width = _nib(w, 1)
            if width not in WIDTHS:
                raise Unclear(f'a store of width {width}')
            ws = take(4 if width == 8 else 3)
            value = (ws[2] << 32 | ws[3]) if width == 8 else ws[2]
            ops.append(Op('store', ws, {'width': width, 'mem': _nib(w, 2), 'reg': _nib(w, 3),
                                        'offset': (w & 0xFF) << 32 | ws[1], 'value': value}))
        elif t == 0x1:
            width = _nib(w, 1)
            if width not in WIDTHS:
                raise Unclear(f'a condition of width {width}')
            ws = take(4 if width == 8 else 3)
            value = (ws[2] << 32 | ws[3]) if width == 8 else ws[2]
            ops.append(Op('begin-cond', ws, {'width': width, 'mem': _nib(w, 2), 'cond': _nib(w, 3),
                                             'use_reg': _nib(w, 4), 'reg': _nib(w, 5),
                                             'offset': (w & 0xFF) << 32 | ws[1], 'value': value}))
        elif t == 0x2:
            ops.append(Op('end-cond', take(1), {'else': _nib(w, 1) == 1}))
        elif t == 0x3:
            if _nib(w, 1) == 0:
                ops.append(Op('loop', take(2), {'start': True, 'reg': _nib(w, 3)}))
            else:
                ops.append(Op('loop', take(1), {'start': False, 'reg': _nib(w, 3)}))
        elif t == 0x4:
            ws = take(3)
            ops.append(Op('load-const', ws, {'reg': _nib(w, 3), 'value': ws[1] << 32 | ws[2]}))
        elif t == 0x5:
            ws = take(2)
            ops.append(Op('load-mem', ws, {'width': _nib(w, 1), 'mem': _nib(w, 2), 'reg': _nib(w, 3),
                                           'from': _nib(w, 4), 'oreg': _nib(w, 5),
                                           'offset': (w & 0xFF) << 32 | ws[1]}))
        elif t == 0x6:
            ws = take(3)
            ops.append(Op('store-reg-imm', ws, {'width': _nib(w, 1), 'reg': _nib(w, 3), 'inc': _nib(w, 4),
                                                'use_oreg': _nib(w, 5), 'oreg': _nib(w, 6),
                                                'value': ws[1] << 32 | ws[2]}))
        elif t == 0x7:
            ws = take(2)
            ops.append(Op('arith', ws, {'width': _nib(w, 1), 'reg': _nib(w, 3), 'op': _nib(w, 4), 'value': ws[1]}))
        elif t == 0x8:
            ops.append(Op('keys', take(1), {'mask': w & 0x0FFFFFFF}))
        elif t == 0x9:
            width = _nib(w, 1)
            if _nib(w, 5) == 1:
                ws = take(3 if width == 8 else 2)
                value = (ws[1] << 32 | ws[2]) if width == 8 else ws[1]
                ops.append(Op('arith-reg', ws, {'width': width, 'op': _nib(w, 2), 'dst': _nib(w, 3),
                                                'src': _nib(w, 4), 'imm': value}))
            else:
                ops.append(Op('arith-reg', take(1), {'width': width, 'op': _nib(w, 2), 'dst': _nib(w, 3),
                                                     'src': _nib(w, 4), 'src2': _nib(w, 6)}))
        elif t == 0xA:
            kind = _nib(w, 5)
            ws = take(2 if kind in (2, 4, 5) else 1)
            imm = ((w & 0xF) << 32 | ws[1]) if len(ws) == 2 else None
            ops.append(Op('store-reg', ws, {'width': _nib(w, 1), 'src': _nib(w, 2), 'reg': _nib(w, 3),
                                            'inc': _nib(w, 4), 'kind': kind, 'mem': _nib(w, 6),
                                            'oreg': _nib(w, 7), 'imm': imm}))
        elif t == 0xC:
            sub = _nib(w, 1)
            if sub == 0:
                kind = _nib(w, 5)
                n = {0: 2, 1: 1, 2: 2, 3: 1, 4: 3 if _nib(w, 2) == 8 else 2, 5: 1}.get(kind)
                if n is None:
                    raise Unclear('a register condition of an unknown kind')
                ops.append(Op('reg-cond', take(n), {}))
            elif sub == 4:
                ws = take(3)
                ops.append(Op('keys', ws, {'mask': ws[1] << 32 | ws[2], 'repeat': _nib(w, 2)}))
            elif sub in (1, 2, 3):
                ops.append(Op({1: 'save', 2: 'save-mask', 3: 'static-reg'}[sub], take(1), {}))
            else:
                raise Unclear(f'code type C{sub:X}')
        elif t == 0xF:
            sub = _nib(w, 2)
            if _nib(w, 1) != 0xF or sub not in (0, 1):
                raise Unclear(f'code type {w >> 20:03X}')
            ops.append(Op('pause' if sub == 0 else 'resume', take(1), {}))
        else:
            raise Unclear(f'code type {t:X}')
    return ops


# ---- what the opcodes do --------------------------------------------------------------------------------------------

@dataclass(frozen=True)
class Place:
    """A place the code writes: a region of Appendix A and an offset, maybe followed by a pointer path."""
    region: str
    offset: int
    path: tuple = None

    def plus(self, n):
        if self.path:
            return Place(self.region, self.offset, self.path[:-1] + ((self.path[-1] + n) & MASK64,))
        return Place(self.region, (self.offset + n) & MASK64)

    def read(self):
        return Place(self.region, self.offset, (self.path or ()) + (0,))


@dataclass
class Store:
    place: Place
    width: int
    value: int

    @property
    def data(self):
        return (self.value & ((1 << (8 * self.width)) - 1)).to_bytes(self.width, 'little')


@dataclass
class Condition:
    place: Place
    width: int
    op: str
    value: int


@dataclass
class Derived:
    stores: list
    keys: int = 0
    condition: Condition = None
    released: list = None          # the stores of an else branch of the button block: what is put back on release
    reads: set = field(default_factory=set)      # registers read before the entry wrote them
    writes: set = field(default_factory=set)     # registers the entry writes


def derive(entry):
    """What the entry does, as plain stores, or raises Unclear."""
    ops = decode(entry.words)
    out = Derived([])
    body = list(ops)
    if body and body[0].kind == 'keys':
        if body[0].f.get('repeat'):
            raise Unclear('a button block that repeats')
        out.keys = body[0].f['mask']
        body = body[1:]
        if not body or body[-1].kind != 'end-cond':
            raise Unclear('a button block that does not end')
        body = body[:-1]
        elses = [i for i, op in enumerate(body) if op.kind == 'end-cond' and op.f['else'] and _depth(body[:i]) == 0]
        if elses:
            i = elses[0]
            out.released = _plain(body[i + 1:], out)
            body = body[:i]
    if body and body[0].kind == 'begin-cond':
        c = body[0].f
        if c['use_reg'] or c['mem'] not in REGIONS or c['cond'] not in CONDITIONS:
            raise Unclear('a memory condition through a register')
        if not body or body[-1].kind != 'end-cond' or body[-1].f['else']:
            raise Unclear('a memory condition that does not end plainly')
        out.condition = Condition(Place(REGIONS[c['mem']], c['offset']), c['width'], CONDITIONS[c['cond']],
                                  c['value'])
        body = body[1:-1]
    out.stores = _plain(body, out)
    if out.released is not None:
        if out.condition is not None:
            raise Unclear('a button block with an else branch and a condition')
        held = {s.place.plus(i) for s in out.stores for i in range(s.width)}
        if any(s.place.plus(i) not in held for s in out.released for i in range(s.width)):
            raise Unclear('an else branch that writes places the button block does not')

    return out


def _depth(ops):
    d = 0
    for op in ops:
        if op.kind in ('begin-cond', 'keys', 'reg-cond'):
            d += 1
        elif op.kind == 'end-cond' and not op.f['else']:
            d -= 1
    return d


def _plain(ops, out):
    """The stores of a sequence of opcodes without blocks, following the registers."""
    regs = {}

    def get(r):
        if r not in regs:
            if r not in out.writes:
                out.reads.add(r)
            return ('const', 0)
        return regs[r]

    def put(r, v):
        regs[r] = v
        out.writes.add(r)

    stores = []
    for op in ops:
        f = op.f
        if op.kind == 'store':
            base = get(f['reg'])
            if base[0] != 'const' or f['mem'] not in REGIONS:
                raise Unclear('a store whose offset register holds an address')
            stores.append(Store(Place(REGIONS[f['mem']], (f['offset'] + base[1]) & MASK64), f['width'], f['value']))
        elif op.kind == 'load-const':
            put(f['reg'], ('const', f['value']))
        elif op.kind == 'load-mem':
            if f['width'] != 8:
                raise Unclear('a load of a value that is not a pointer')
            if f['from'] == 0:
                if f['mem'] not in REGIONS:
                    raise Unclear('a load from an unknown region')
                put(f['reg'], ('place', Place(REGIONS[f['mem']], f['offset']).read()))
            elif f['from'] == 1:
                src = get(f['reg'])
                if src[0] != 'place':
                    raise Unclear('a load from an address the code computed')
                put(f['reg'], ('place', src[1].plus(f['offset']).read()))
            else:
                raise Unclear('a load through an offset register')
        elif op.kind == 'arith':
            if f['op'] not in (0, 1):
                raise Unclear('arithmetic other than adding or subtracting')
            put(f['reg'], _add(get(f['reg']), f['value'] if f['op'] == 0 else -f['value']))
        elif op.kind == 'arith-reg':
            src = get(f['src'])
            other = ('const', f['imm']) if 'imm' in f else get(f['src2'])
            if f['op'] == 9:
                put(f['dst'], src)
            elif f['op'] in (0, 1) and other[0] == 'const':
                put(f['dst'], _add(src, other[1] if f['op'] == 0 else -other[1]))
            elif f['op'] == 0 and src[0] == 'const':
                put(f['dst'], _add(other, src[1]))
            else:
                raise Unclear('arithmetic on registers that the core operations cannot say')
        elif op.kind == 'store-reg-imm':
            target = get(f['reg'])
            if f['use_oreg']:
                target = _add(target, _const(get(f['oreg'])))
            if target[0] != 'place':
                raise Unclear('a store to an address the code computed')
            if f['width'] not in WIDTHS:
                raise Unclear(f'a store of width {f["width"]}')
            stores.append(Store(target[1], f['width'], f['value']))
            if f['inc']:
                put(f['reg'], _add(get(f['reg']), f['width']))
        elif op.kind == 'store-reg':
            value = get(f['src'])
            if value[0] != 'const':
                raise Unclear('a store of a value the code computed')
            k = f['kind']
            if k == 0:
                target = get(f['reg'])
            elif k == 1:
                target = _add(get(f['reg']), _const(get(f['oreg'])))
            elif k == 2:
                target = _add(get(f['reg']), f['imm'])
            elif k in (3, 4, 5) and f['mem'] in REGIONS:
                off = (_const(get(f['oreg'])) if k in (3, 5) else 0) + (f['imm'] or 0)
                target = ('place', Place(REGIONS[f['mem']], off & MASK64))
            else:
                raise Unclear('a register store of an unknown kind')
            if target[0] != 'place':
                raise Unclear('a store to an address the code computed')
            if f['width'] not in WIDTHS:
                raise Unclear(f'a store of width {f["width"]}')
            stores.append(Store(target[1], f['width'], value[1]))
            if f['inc']:
                put(f['reg'], _add(get(f['reg']), f['width']))
        else:
            raise Unclear({'begin-cond': 'a memory condition inside the code', 'end-cond': 'a block inside the code',
                           'keys': 'a button block inside the code', 'loop': 'a loop',
                           'reg-cond': 'a register condition', 'save': 'saved registers',
                           'save-mask': 'saved registers', 'static-reg': 'static registers',
                           'pause': 'pausing the game', 'resume': 'pausing the game'}.get(op.kind, op.kind))
    return stores


def _const(v):
    if v[0] != 'const':
        raise Unclear('an offset register that holds an address')
    return v[1]


def _add(v, n):
    if v[0] == 'const':
        return ('const', (v[1] + n) & MASK64)
    return ('place', v[1].plus(n))


def keys_to_buttons(mask):
    names = [name for name, bit in KEYS if mask & bit]
    if not names or sum(bit for name, bit in KEYS if mask & bit) != mask:
        return None
    return '+'.join(names)


def buttons_to_keys(text):
    bits = dict(KEYS)
    if any(name not in bits for name in text.split('+')):
        raise Unclear(f'the buttons {text} have no Atmosphère key mask')
    return sum(bits[name] for name in text.split('+'))


def is_noop(entry):
    """Whether an entry has no code, or only ends of blocks: a heading such as `[--- Items ---]`."""
    try:
        return all(op.kind == 'end-cond' for op in decode(entry.words))
    except Unclear:
        return False


# ---- writing --------------------------------------------------------------------------------------------------------

def store_words(place, width, value):
    """The opcodes that write value at place: a type 0 store, or for a pointer path the loads and a type 6 store."""
    if place.offset >> 40:
        raise Unclear('an offset beyond the 40 bits of a store')
    region = REGION_CODES[place.region]
    if not place.path:
        first = 0x00000000 | width << 24 | region << 20 | (place.offset >> 32) & 0xFF
        words = [first, place.offset & 0xFFFFFFFF]
        return words + ([value >> 32 & 0xFFFFFFFF, value & 0xFFFFFFFF] if width == 8 else [value & 0xFFFFFFFF])
    reg = 0xF
    words = [0x58000000 | region << 20 | reg << 16 | (place.offset >> 32) & 0xFF, place.offset & 0xFFFFFFFF]
    path = list(place.path)
    for step in path[:-1]:
        words += _add_words(reg, step, then_load=True)
    last = path[-1]
    if last:
        words += _add_words(reg, last, then_load=False)
    words += [0x60000000 | width << 24 | reg << 16, value >> 32 & 0xFFFFFFFF, value & 0xFFFFFFFF]
    return words


def _add_words(reg, step, then_load):
    """The opcodes that add a path step to the register, and with then_load read the pointer there."""
    signed = step - (1 << 64) if step >> 63 else step
    if then_load and 0 <= signed < (1 << 40):
        return [0x58001000 | reg << 16 | (signed >> 32) & 0xFF, signed & 0xFFFFFFFF]
    words = []
    if 0 < abs(signed) < (1 << 32):
        op = 0 if signed > 0 else 1
        words += [0x78000000 | reg << 16 | op << 12, abs(signed)]
    elif signed:                                   # a 64-bit add: type 9, register plus a value
        words += [0x98000100 | reg << 16 | reg << 12, step >> 32 & 0xFFFFFFFF, step & 0xFFFFFFFF]
    if then_load:
        words += [0x58001000 | reg << 16, 0]
    return words


def condition_words(place, width, op, value):
    if place.offset >> 40:
        raise Unclear('an offset beyond the 40 bits of a condition')
    region = REGION_CODES[place.region]
    first = 0x10000000 | width << 24 | region << 20 | CONDITION_CODES[op] << 16 | (place.offset >> 32) & 0xFF
    words = [first, place.offset & 0xFFFFFFFF]
    return words + ([value >> 32 & 0xFFFFFFFF, value & 0xFFFFFFFF] if width == 8 else [value & 0xFFFFFFFF])


def format_words(words):
    """Code lines: one opcode per line, as cheat files write them."""
    return [' '.join(f'{w:08X}' for w in op.words) for op in decode(list(words))]


def write_file(entries):
    """The text of an Atmosphère cheat file: (name, master, code lines) for each entry."""
    out = []
    for name, master, lines in entries:
        out.append(('{%s}' if master else '[%s]') % name)
        out.extend(lines)
        out.append('')
    return '\n'.join(out)


def registers_written(entry):
    """The registers an entry may write, for an entry that derive cannot read: all of them when its opcodes are not
    even known."""
    try:
        ops = decode(entry.words)
    except Unclear:
        return set(range(16))
    regs = set()
    for op in ops:
        f = op.f
        if op.kind in ('load-const', 'load-mem', 'arith') or (op.kind == 'loop' and f.get('start')):
            regs.add(f['reg'])
        elif op.kind == 'arith-reg':
            regs.add(f['dst'])
        elif op.kind in ('store-reg-imm', 'store-reg') and f['inc']:
            regs.add(f['reg'])
        elif op.kind in ('save', 'save-mask', 'static-reg'):
            return set(range(16))
    return regs


def main_offsets(entry):
    """The offsets of every plain store into the main module in an entry, whatever blocks are around them."""
    try:
        ops = decode(entry.words)
    except Unclear:
        return set()
    return {op.f['offset'] for op in ops if op.kind == 'store' and op.f['mem'] == 0}
