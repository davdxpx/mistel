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


def _find(module, items, section=None):
    m = Module(None, 0, module.image, len(module.image), module.sections)
    return find_all(m, items, section, 2)


def _search(old, new, offset, length):
    """The offset in the new main of the place at `offset` in the old one: (outcome, new offset)."""
    if not _in_code(old, offset):
        return 'data', None
    p = make_pattern(old, offset, cover=min(length, 4))
    if p is None:
        return 'no-pattern', None
    hits = _find(new, p.items)
    if not hits:
        return _around(old, new, offset, length)
    if len(hits) > 1:
        return 'ambiguous', None
    return 'found', hits[0] + p.place


def _around(old, new, offset, length):
    """The place found by the code around it alone, its own bytes left open: where an update changed the instruction a
    cheat expects but not the code around it, so that the report can say the code there changed."""
    start, end = old.sections[old.section_of(offset)]
    for w in (8, 16, 24, 32, 48, 64):
        lo, hi = max(start, offset - w), min(end, len(old.image), offset + length + w)
        moving = arm64.moving_bytes(old.image, lo, hi)
        items = [None if i in moving or offset <= i < offset + length else old.image[i] for i in range(lo, hi)]
        while items and items[0] is None:
            items.pop(0)
            lo += 1
        while items and items[-1] is None:
            items.pop()
        if sum(1 for b in items if b is not None) < 8 or _find(old, items) != [lo]:
            continue
        hits = _find(new, items)
        return ('found', hits[0] + offset - lo) if len(hits) == 1 else ('not-found', None)
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
