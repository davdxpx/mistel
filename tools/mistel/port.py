"""`mistel port`: a file's cheats for a new build of the game, from the old build's code.

Step one, the places in the main module's code and read-only data. For each such place that a cheat names by an offset
for the old build, the original bytes around it in the old build's main, with the bytes that move between builds left
open (branch and page offsets), are searched in the new build's main. Where they are found exactly once, that is the
place in the new build, and the cheat gets its offset there, checked against `expect`. A place found by a pattern needs
nothing; the tool checks that the pattern still finds one place. Places in data, in regions or at addresses, places in
other modules, and raw code stay as they are and are named in the report.

Only the code modules of the player's own dump are read: the old and the new build's main, as NSO files or plain
images. Nothing else of the game, and no keys.
"""
import copy
import re
from dataclasses import dataclass, field

import mncf
from mncf import arm64
from mncf import write as writer
from mncf.evaluate import Module, find_all
from mncf.pattern import make_pattern
from mncf.values import integer, parse_bytes, parse_pattern

WHOLE = (0, 1 << 64)
OPS = (('patch', 'at'), ('write', 'at'), ('override', 'function'), ('read', 'at'), ('when', 'at'))


class PortError(ValueError):
    pass


@dataclass
class Place:
    """What became of one place of a cheat."""
    op: str                 # patch, write, override, read, when
    outcome: str            # pattern, same, moved, has, not-found, ambiguous, changed, no-pattern, data, region,
                            # address, module, no-offset, pattern-not-found, pattern-ambiguous
    old: int = None
    new: int = None


@dataclass
class CheatPort:
    id: str
    name: str
    places: list = field(default_factory=list)
    raw: bool = False       # it has raw code for the old build and none for the new one (kept for the old build)

    @property
    def outcome(self):
        """fits (nothing to change), ported (new offsets), or the first problem of its places"""
        bad = [p.outcome for p in self.places if p.outcome not in ('pattern', 'same', 'moved', 'has', 'region-fixed')]
        if bad:
            return bad[0]
        if self.raw and not self.places:
            return 'raw'
        return 'ported' if any(p.outcome == 'moved' for p in self.places) else 'fits'


@dataclass
class Port:
    text: str                   # the new file
    old_key: str
    new_key: str
    cheats: list                # CheatPort for each cheat, in file order
    fit: int = 0                # cheats the reader finds fit for the new build with the new main
    total: int = 0

    def count(self, outcome):
        return sum(1 for c in self.cheats if c.outcome == outcome)


def build_key(f, build_id):
    """The file's build whose main module is this build ID, or None."""
    for b in f.builds.values():
        main = b.modules.get('main')
        if main and f.platform.module_id_matches(main, build_id):
            return b.key
    return None


def _length(kind, op):
    for key in ('expect', 'bytes'):
        if isinstance(op.get(key), str) and parse_bytes(op[key]) is not None:
            return len(parse_bytes(op[key]))
    return 4


def _in_code(module, offset):
    return module.section_of(offset) in ('code', 'rodata')


def _find(module, items, section=None, limit=2):
    """Where the pattern is, up to limit places, as find_all gives them: found by the pattern's longest run of fixed
    bytes and then checked whole, which is much faster than a regular expression over a main of 50 MB."""
    m = Module(None, 0, module.image, len(module.image), module.sections)
    a0 = a1 = 0
    i = 0
    while i < len(items):
        if items[i] is None:
            i += 1
            continue
        j = i
        while j < len(items) and items[j] is not None:
            j += 1
        if j - i > a1 - a0:
            a0, a1 = i, j
        i = j
    if a1 - a0 < 4:
        return find_all(m, items, section, limit)
    # from the longest run on, as a regular expression that starts with it, so that the engine looks for that run
    # and checks the rest; the bytes before it are checked here, for the few places found
    image = module.image
    rx = re.compile(b''.join(b'.' if b is None else re.escape(bytes([b])) for b in items[a0:]), re.DOTALL)
    found, seen = [], set()
    for start, end in m.search_ranges(section):
        end = min(end, len(image))
        pos = start + a0
        while True:
            hit = rx.search(image, pos, end)
            if hit is None:
                break
            h = hit.start() - a0
            if h not in seen and all(b is None or image[h + n] == b for n, b in enumerate(items[:a0])):
                seen.add(h)
                found.append(h)
                if len(found) >= limit:
                    return found
            pos = hit.start() + 1
    return found


def _arm32(module):
    """Whether the module's code is 32-bit ARM rather than AArch64: more of ARM's function returns (bx lr, pop with pc)
    than of AArch64's ret in it."""
    end = min(len(module.image), module.text_size or module.sections.get('code', (0, len(module.image)))[1])
    code = bytes(module.image[:end])
    a64 = code.count(RET)
    a32 = code.count(bytes.fromhex('1eff2fe1')) + len(re.findall(rb'[\x80-\xff]\xbd\xe8', code))
    return a32 > a64


def _branch_signs(image, lo, hi):
    """The top bytes of B and BL in [lo, hi): their low bits are the top of the branch offset, which flips when the
    target moves to the other side of the branch."""
    out = set()
    for off in range(lo & ~3, hi, 4):
        if off + 4 <= len(image) and lo <= off + 3 < hi and (image[off + 3] & 0x7C) == 0x14:
            out.add(off + 3)
    return out


def _search(old, new, offset, length):
    """The offset in the new main of the place at `offset` in the old one: (outcome, new offset)."""
    if not _in_code(old, offset):
        return 'data', None
    p = make_pattern(old, offset, cover=min(length, 4))
    if p is None:
        n = _by_neighbour(old, new, offset, length)
        return ('found', n) if n is not None else ('no-pattern', None)
    hits = _find(new, p.items)
    if not hits:
        return _around(old, new, offset, length)
    if len(hits) > 1 or not _confirmed(old, new, offset, hits[0] + p.place, length):
        n = _by_neighbour(old, new, offset, length)
        if n is not None:
            return 'found', n
        return ('ambiguous' if len(hits) > 1 else 'not-found'), None
    return 'found', hits[0] + p.place


SURE = 7            # instructions the same next to a place (before and after it, the place left out) for a find to count
RET = bytes.fromhex('c0035fd6')


def _alike(old_image, new_image, o, n):
    """Whether the old word at o and the new one at n are the same instruction where only what moves between builds
    differs: an ADRP or ADR the same kind into the same register, a B or BL the same kind, and otherwise the bytes that
    do not move the same."""
    a, b = old_image[o:o + 4], new_image[n:n + 4]
    if len(a) < 4 or len(b) < 4:
        return False
    if a == b:
        return True
    wa, wb = int.from_bytes(a, 'little'), int.from_bytes(b, 'little')
    if (wa & 0x1F000000) == 0x10000000:                     # ADR, ADRP: the whole word but its kind and register
        return (wa & 0x9F00001F) == (wb & 0x9F00001F)
    if (wa & 0x7C000000) == 0x14000000:                     # B, BL
        return (wa & 0xFC000000) == (wb & 0xFC000000)
    x, y = _masked(old_image, o, o + 4), _masked(new_image, n, n + 4)
    return all(i is None or j is None or i == j for i, j in zip(x, y))


def _same_word(old, new, o, n):
    if o < 0 or n < 0:
        return False
    return _alike(old.image, new.image, o, n)


def _confirmed(old, new, offset, n, length):
    """Whether the code next to a place found at n is the same as next to it in the old main: SURE instructions or
    more before and after it together, or the whole function, from the return before it to the one after it. A short
    pattern can be unique in the old main and found once in the new one at code that only looks alike."""
    counts = []
    for step, o0, n0 in ((-4, offset - 4, n - 4), (4, offset + length, n + length)):
        k, end = 0, False
        while k <= SURE and _same_word(old, new, o0 + step * k, n0 + step * k):
            k += 1
            if old.image[o0 + step * (k - 1):o0 + step * (k - 1) + 4] == RET:
                end = True
                break
        counts.append((k, end))
    (before, start), (after, stop) = counts
    return before + after >= SURE or (start and stop)


NEAR = 1024         # how far from a place the code that finds it may be, in bytes
ANCHOR = 32         # the length of that code


def _masked(image, lo, hi):
    """The bytes [lo, hi) with the ones that move between builds left open."""
    moving = arm64.moving_bytes(image, lo, hi) | _branch_signs(image, lo, hi)
    return [None if i in moving else image[i] for i in range(lo, hi)]


def _same(old, new, o_lo, o_hi, n_lo):
    """Whether the old main's bytes [o_lo, o_hi) are the new main's from n_lo on, bytes that move left open."""
    n_hi = n_lo + o_hi - o_lo
    if o_lo >= o_hi:
        return True
    if n_lo < 0 or n_hi > len(new.image):
        return False
    if o_lo % 4 == 0 and n_lo % 4 == 0 and (o_hi - o_lo) % 4 == 0:
        return all(_alike(old.image, new.image, o_lo + i, n_lo + i) for i in range(0, o_hi - o_lo, 4))
    a, b = _masked(old.image, o_lo, o_hi), _masked(new.image, n_lo, n_hi)
    return all(x is None or y is None or x == y for x, y in zip(a, b))


def _by_neighbour(old, new, offset, length):
    """The place found through code near it, for a place whose own code is in the old main more than once (copies of
    a function) or is found more than once in the new one: the nearest ANCHOR bytes within NEAR bytes, in the same
    section, that are found once in the old main and once in the new one, where every byte between them and the place
    is the same in both. The place is then as far from them as before."""
    section = old.section_of(offset)
    start, end = old.sections[section]
    if section == 'code' and old.text_size is not None:
        end = min(end, old.text_size)
    end = min(end, len(old.image))
    for d in range(ANCHOR, NEAR + ANCHOR, ANCHOR):
        for lo in (offset - d, offset + length + d - ANCHOR):
            hi = lo + ANCHOR
            if lo < start or hi > end:
                continue
            items = _masked(old.image, lo, hi)
            if sum(1 for b in items if b is not None) < ANCHOR // 2 or _find(old, items) != [lo]:
                continue
            hits = _find(new, items)
            if len(hits) != 1:
                continue
            n = hits[0] + offset - lo
            if lo < offset:
                between = _same(old, new, hi, offset, hits[0] + ANCHOR)
            else:
                between = _same(old, new, offset + length, lo, n + length)
            if between:
                return n
    return None


def _around(old, new, offset, length):
    """The place found by the code around it alone, its own bytes left open: where an update changed the instruction a
    cheat expects but not the code around it, so that the report can say the code there changed."""
    start, end = old.sections[old.section_of(offset)]
    for w in (8, 16, 24, 32, 48, 64):
        lo, hi = max(start, offset - w), min(end, len(old.image), offset + length + w)
        moving = arm64.moving_bytes(old.image, lo, hi) | _branch_signs(old.image, lo, hi)
        items = [None if i in moving or offset <= i < offset + length else old.image[i] for i in range(lo, hi)]
        while items and items[0] is None:
            items.pop(0)
            lo += 1
        while items and items[-1] is None:
            items.pop()
        if sum(1 for b in items if b is not None) < 8 or _find(old, items) != [lo]:
            continue
        hits = _find(new, items)
        if len(hits) == 1 and _confirmed(old, new, offset, hits[0] + offset - lo, length):
            return 'found', hits[0] + offset - lo
        return 'not-found', None
    return 'not-found', None


def _expect_at(new, offset, op):
    """Whether the new main holds a patch's or an override's `expect` at offset (writes and reads have none to check)."""
    want = parse_bytes(op.get('expect')) if isinstance(op.get('expect'), str) else None
    if want is None:
        return True
    have = new.image[offset:offset + len(want)]
    done = parse_bytes(op.get('bytes')) if isinstance(op.get('bytes'), str) else None
    return have == want or (done is not None and have == done)


def _place(kind, op, key, old, new, old_key, new_key, keys):
    """Ports one location table op[key] in place; returns its Place."""
    loc = op.get(key)
    if not isinstance(loc, dict):
        return Place(kind, 'no-offset')
    if 'address' in loc:
        return Place(kind, 'address')
    if 'region' in loc:
        return Place(kind, 'region' if 'offsets' in loc else 'region-fixed')
    if loc.get('module') != 'main':
        return Place(kind, 'module')
    if 'find' in loc:
        items = parse_pattern(loc['find'])
        hits = _find(new, items, loc.get('section')) if items else []
        return Place(kind, 'pattern' if len(hits) == 1 else 'pattern-not-found' if not hits else 'pattern-ambiguous')
    length = _length(kind, op)
    if 'offsets' in loc:
        offsets = loc['offsets']
        if new_key in offsets:
            return Place(kind, 'has', offsets.get(old_key), offsets[new_key])
        if old_key not in offsets:
            return Place(kind, 'no-offset')
        o = integer(offsets[old_key])
        outcome, n = _search(old, new, o, length)
        if n is None:
            return Place(kind, outcome, o)
        if kind in ('patch', 'override') and not _expect_at(new, n, op):
            return Place(kind, 'changed', o, n)
        offsets[new_key] = n
        return Place(kind, 'moved' if n != o else 'same', o, n)
    o = integer(loc.get('offset', 0))
    if kind in ('patch', 'override') and _in_code(new, o) and _expect_at(new, o, op) and \
            old.image[o:o + length] == new.image[o:o + length]:
        return Place(kind, 'same', o, o)
    outcome, n = _search(old, new, o, length)
    if n is None:
        return Place(kind, outcome, o)
    if kind in ('patch', 'override') and not _expect_at(new, n, op):
        return Place(kind, 'changed', o, n)
    if n == o:
        return Place(kind, 'same', o, n)
    # a fixed offset that moved: per build from now on, the same for every build the file had
    loc.pop('offset', None)
    rest = {k: v for k, v in loc.items() if k != 'module'}
    loc.clear()
    loc['module'] = 'main'
    loc['offsets'] = {**{k: o for k in keys}, new_key: n}
    loc.update(rest)
    return Place(kind, 'moved', o, n)


def port(data, old, new, old_key=None, new_key=None, version=None):
    """The file of bytes `data` for the build of the code module `new`, from the build of `old` (CodeModule each)."""
    f = mncf.load(data)
    if not new.short_id:
        raise PortError('the new main names no build ID: give an NSO file')
    for which, m in (('old', old), ('new', new)):
        if _arm32(m):
            raise PortError(f'the {which} main is 32-bit ARM code: mistel port finds places in AArch64 code only (a game '
                            f'that moved from 32 to 64 bits in an update was compiled anew, and no place can be found by '
                            f'its code)')
    old_key = old_key or (build_key(f, old.short_id) if old.short_id else None)
    if old_key is None or old_key not in f.builds:
        raise PortError('the file has no build of the old main' + (f' ({old.short_id})' if old.short_id else '') +
                        ': give --from with the build key')
    doc = copy.deepcopy(f.doc)
    have = build_key(f, new.short_id)
    if have is not None:
        new_key = have
    else:
        new_key = new_key or version or new.short_id
        if new_key in f.builds:
            raise PortError(f'the file has a build {new_key!r} for another main already: give --build')
        entry = {'modules': {'main': new.short_id}}
        if version and version != new_key:
            entry = {'version': version, **entry}
        doc.setdefault('builds', {})[new_key] = entry
    keys = list(f.builds)
    cheats = []
    for c, table in zip(f.cheats, doc['cheat']):
        cp = CheatPort(c.id, c.name or c.id)
        if c.problem:
            cp.places.append(Place('cheat', 'invalid'))
            cheats.append(cp)
            continue
        for kind, key in OPS:
            for op in table.get(kind, []):
                cp.places.append(_place(kind, op, key, old, new, old_key, new_key, keys))
                if kind == 'when' and isinstance(op.get('value'), dict) and 'at' in op['value']:
                    cp.places.append(_place(kind, op['value'], 'at', old, new, old_key, new_key, keys))
        raws = {r.build for r in c.raw}
        cp.raw = old_key in raws and new_key not in raws
        cheats.append(cp)
    text = writer.dumps(doc)
    ported = mncf.load(text.encode())
    rt = mncf.Runtime(ported.platform.name, modules={'main': new.reader_module()},
                      regions={r: WHOLE for r in ported.platform.regions})
    ev = mncf.evaluate(ported, rt)
    fit = sum(1 for r in ev.cheats if r.state != 'refused')
    return Port(text, old_key, new_key, cheats, fit, len(ev.cheats))


WORDS = {
    'fits': 'fits as it is',
    'ported': 'ported',
    'not-found': 'the code around its place is not in the new build',
    'ambiguous': 'the code around its place is in the new build more than once',
    'changed': 'its place is found, but the code there is not what it expects',
    'no-pattern': 'the code around its place is not unique even in the old build',
    'data': 'a place in data: not ported yet (step 2)',
    'region': 'a place in a region with an offset for each build: not ported',
    'address': 'a fixed address: not ported',
    'module': 'a place in a module other than main: not ported',
    'no-offset': 'no offset for the old build',
    'pattern-not-found': 'its pattern is not in the new build',
    'pattern-ambiguous': 'its pattern is in the new build more than once',
    'invalid': 'the cheat breaks a rule of the spec',
    'raw': 'only raw code, for the old build: not ported yet (step 3)',
}
