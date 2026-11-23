"""Identifies the build and gives every cheat its state and reason (sections 5, 6, 11 and Appendix B)."""
import math
import re
from dataclasses import dataclass, field

from . import encode as enc
from .model import Condition, Override, Patch, Place, Read, ValueRef, Write
from .values import FLOAT_TYPES, encode as encode_value, fits, hex_bytes, type_size

CAPABILITIES = ('guest-read', 'guest-write', 'code-bytes', 'code-hook', 'buttons', 'display')
ADDRESS_MASK = (1 << 64) - 1


class Module:
    """A code module that runs: its identity, where it is loaded, and its bytes as the module file gave them.

    `sections` maps "code", "rodata" and "data" to (start, end) offsets. Without it, the whole module is code.
    """

    def __init__(self, ident, base=0, data=None, size=None, sections=None):
        self.id = ident
        self.base = base
        self.data = data
        self.size = size if size is not None else (len(data) if data is not None else None)
        self.sections = sections or ({'code': (0, self.size)} if self.size is not None else {})

    def search_ranges(self, section):
        names = (section,) if section else ('code', 'rodata')
        return [self.sections[n] for n in names if n in self.sections]


class Runtime:
    """What the runtime that applies the file has and knows. A reader's caller fills it in.

    `memory(address, size)` returns the bytes of guest memory there, or None where there is no guest memory. Without
    it, the reader reads module bytes (at their base) and nothing else.
    """

    def __init__(self, platform, game_id=None, arch=None, modules=None, regions=None, memory=None,
                 capabilities=CAPABILITIES, extensions=('raw/1', 'arm64-imm/1'), raw_syntaxes=('atmosphere',),
                 build=None, held=()):
        self.platform = platform
        self.game_id = game_id
        self.arch = arch
        self.modules = modules or {}
        self.regions = regions or {}          # name -> (base, size)
        self.memory = memory
        self.capabilities = set(capabilities)
        self.extensions = set(extensions)
        self.raw_syntaxes = set(raw_syntaxes)
        self.build = build                    # a build key the runtime names itself, as a recompilation does
        self.held = set(held)

    def read(self, address, size):
        if self.memory is not None:
            return self.memory(address, size)
        for m in self.modules.values():
            if m.data is not None and m.base <= address and address + size <= m.base + len(m.data):
                return bytes(m.data[address - m.base:address - m.base + size])
        return None


@dataclass
class Choices:
    """What the player chose: cheats switched on or off against their default, and values ("cheat.value": number)."""
    on: list = field(default_factory=list)
    off: list = field(default_factory=list)
    values: dict = field(default_factory=dict)


@dataclass
class ResolvedOp:
    kind: str
    address: int = None
    module: str = None
    offset: int = None
    type: str = None
    expect: bytes = None
    bytes: bytes = None
    action: str = None
    value: object = None
    op: str = None
    label: str = None
    syntax: str = None
    build: str = None
    code: str = None

    def as_dict(self):
        out = {}
        for k, v in self.__dict__.items():
            if v is None:
                continue
            out[k] = hex_bytes(v) if isinstance(v, (bytes, bytearray)) else v
        return out


@dataclass
class CheatResult:
    id: str
    state: str           # off, on, waiting, refused
    reason: str = ''
    uses: str = ''       # core, raw, or empty for a refused cheat
    ops: list = field(default_factory=list)
    path_broken: bool = field(default=False, repr=False)


@dataclass
class Evaluation:
    build: str
    cheats: list

    def cheat(self, cheat_id):
        for c in self.cheats:
            if c.id == cheat_id:
                return c
        return None


class _Refused(Exception):
    def __init__(self, reason):
        super().__init__(reason)
        self.reason = reason


class _Waiting(Exception):
    pass


def identify(f, rt):
    """The key of the build that runs, or None (section 5)."""
    if rt.build is not None:
        return rt.build if rt.build in f.builds else None
    for b in f.builds.values():
        if all(name in rt.modules and rt.modules[name].id is not None
               and f.platform.module_id_matches(ident, rt.modules[name].id) for name, ident in b.modules.items()):
            return b.key
    return None


def evaluate(f, rt, choices=None):
    """The build that runs and every cheat's state, reason and resolved operations."""
    choices = choices or Choices()
    build = identify(f, rt)
    arch = rt.arch or f.platform.archs[0]
    results = {}
    file_reason = _file_reason(f, rt)
    for c in f.cheats:
        if file_reason:
            results[c.id] = CheatResult(c.id, 'refused', file_reason)
        elif c.problem:
            results[c.id] = CheatResult(c.id, 'refused', c.problem)
        else:
            results[c.id] = _cheat(f, c, rt, build, arch, choices)
    _relations(f, results)
    _switch(f, results, choices, rt)
    return Evaluation(build, [results[c.id] for c in f.cheats])


def _file_reason(f, rt):
    if rt.platform != f.platform.name or (rt.game_id is not None and rt.game_id.lower() != f.game_id.lower()):
        return 'game-mismatch'
    if f.signature.result in ('invalid', 'revoked'):
        return 'signature'
    for name in f.required:
        if name not in rt.extensions:
            return 'extension-missing:' + name
    return None


def _by_bytes(op, rt, arch):
    """Whether the runtime applies this override by writing a stub over the function's first bytes."""
    return 'code-bytes' in rt.capabilities and op.expect is not None and arch in enc.STUBS


def _needs(c, rt, arch):
    caps, exts = [], []
    for op in c.ops:
        locs = [op.function if isinstance(op, Override) else op.at]
        if isinstance(op, Condition) and isinstance(op.value, Place):
            locs.append(op.value.at)
        if any(loc.path for loc in locs) or isinstance(op, (Condition, Read)):
            caps.append('guest-read')
        if isinstance(op, Write):
            caps.append('guest-write')
        if isinstance(op, Patch):
            caps.append('code-bytes')
            if op.encode is not None:
                exts.append('arm64-imm/1')
        if isinstance(op, Read):
            caps.append('display')
        if isinstance(op, Override) and not _by_bytes(op, rt, arch):
            caps.append('code-hook')
    return caps, exts


def _cheat(f, c, rt, build, arch, choices):
    button_caps = ['buttons'] if (c.hold or c.toggle or c.press) else []
    core = c.core_ops
    caps, exts = _needs(c, rt, arch)
    caps += button_caps
    missing = [cap for cap in CAPABILITIES if cap in caps and cap not in rt.capabilities]
    missing_ext = [e for e in exts if e not in rt.extensions]
    runs_raw = 'raw/1' in rt.extensions and any(r.syntax in rt.raw_syntaxes for r in c.raw)
    entry = next((r for r in c.raw if runs_raw and r.build == build and r.syntax in rt.raw_syntaxes), None)
    buttons_missing = [cap for cap in button_caps if cap not in rt.capabilities]
    if core:
        if not missing and not missing_ext:
            return _core(f, c, rt, build, arch, choices)
        if entry is not None and not buttons_missing:
            return _raw(c, entry)
        reason = 'capability-missing:' + missing[0] if missing else 'extension-missing:' + missing_ext[0]
        return CheatResult(c.id, 'refused', reason)
    if not runs_raw:
        return CheatResult(c.id, 'refused', 'extension-missing:raw/1')
    if buttons_missing:
        return CheatResult(c.id, 'refused', 'capability-missing:' + buttons_missing[0])
    if entry is None:
        return CheatResult(c.id, 'refused', 'build-unknown')
    return _raw(c, entry)


def _raw(c, r):
    return CheatResult(c.id, 'off', '', 'raw', [ResolvedOp('raw', syntax=r.syntax, build=r.build, code=r.code)])


def _value(c, ref, choices):
    """The value the player chose, or the default; refuses a chosen value that is not one the player can choose."""
    spec = c.values[ref.name]
    key = f'{c.id}.{ref.name}'
    if key not in choices.values:
        return spec.default
    v = choices.values[key]
    if spec.type == 'bool':
        ok = isinstance(v, bool)
    elif spec.type in FLOAT_TYPES:
        ok = type(v) in (int, float) and not math.isnan(v) and fits(spec.type, v)
    else:
        ok = type(v) is int and fits(spec.type, v)
    if not ok or (spec.min is not None and v < spec.min) or (spec.max is not None and v > spec.max) or \
            (spec.choices is not None and v not in spec.choices):
        raise _Refused('value-out-of-range')
    return v


def _core(f, c, rt, build, arch, choices):
    ops, waiting, cache = [], False, {}
    order = f.platform.byte_order
    try:
        for op in c.ops:
            try:
                ops.append(_op(f, c, op, rt, build, arch, order, choices, cache))
            except _Waiting:
                waiting = True
                ops.append(ResolvedOp(_kind(op)))
    except _Refused as r:
        return CheatResult(c.id, 'refused', r.reason)
    return CheatResult(c.id, 'off', '', 'core', ops, waiting)


def _kind(op):
    return {Patch: 'patch', Write: 'write', Override: 'override', Read: 'read', Condition: 'when'}[type(op)]


def _op(f, c, op, rt, build, arch, order, choices, cache):
    if isinstance(op, Patch):
        place = _resolve(f, op.at, rt, build, len(op.expect), cache)
        if op.encode is not None:
            e = op.encode
            v = _value(c, e['value'], choices) if isinstance(e['value'], ValueRef) else e['value']
            data = enc.arm64_imm(e['insn'], e['width'], e['rd'], v)
            if data is None:
                raise _Refused('value-out-of-range')
        else:
            data = op.bytes
        _check_expect(rt, place, op.expect, data)
        return ResolvedOp('patch', place.address, place.module, place.offset, expect=op.expect, bytes=data)
    if isinstance(op, Write):
        size = len(op.value) if op.type == 'bytes' else type_size(op.type)
        place = _resolve(f, op.at, rt, build, size, cache)
        if op.type == 'bytes':
            data = op.value
            expect = op.expect
        else:
            v = op.value
            if isinstance(v, ValueRef):
                v = _value(c, v, choices)
                if isinstance(v, bool):
                    v = int(v)
                if not fits(op.type, v):
                    raise _Refused('value-out-of-range')
            data = encode_value(op.type, v, order)
            expect = encode_value(op.type, op.expect, order) if op.expect is not None else None
        return ResolvedOp('write', place.address, place.module, place.offset, op.type, expect=expect, bytes=data)
    if isinstance(op, Override):
        v = op.value
        if isinstance(v, ValueRef):
            v = int(_value(c, v, choices))
        patches = _by_bytes(op, rt, arch)
        stub = enc.stub(arch, op.action, v) if patches else None
        size = len(op.expect) if op.expect is not None else 4
        place = _resolve(f, op.function, rt, build, size, cache)
        if patches:
            if len(stub) > len(op.expect):
                raise _Refused('value-out-of-range' if isinstance(op.value, ValueRef) else 'invalid:override.expect')
            _check_expect(rt, place, op.expect, None)
        return ResolvedOp('override', place.address, place.module, place.offset, action=op.action, value=v,
                          expect=op.expect, bytes=stub)
    if isinstance(op, Read):
        place = _resolve(f, op.at, rt, build, type_size(op.type), cache)
        return ResolvedOp('read', place.address, place.module, place.offset, op.type, label=op.label)
    place = _resolve(f, op.at, rt, build, type_size(op.type), cache)
    v = op.value
    if isinstance(v, ValueRef):
        v = _value(c, v, choices)
    elif isinstance(v, Place):
        other = _resolve(f, v.at, rt, build, type_size(v.type), cache)
        v = {'address': other.address, 'type': v.type}
    elif isinstance(v, tuple):
        v = list(v)
    return ResolvedOp('when', place.address, place.module, place.offset, op.type, op=op.op, value=v)


@dataclass
class _Place:
    address: int
    module: str = None
    offset: int = None
    in_module: object = None       # the Module whose own bytes the place is in, for expect


def _resolve(f, loc, rt, build, size, cache):
    plat = f.platform
    if loc.kind == 'address':
        place = _Place(loc.address)
        if rt.read(loc.address, plat.pointer_size if loc.path else size) is None:
            raise _Refused('outside')
    elif loc.kind in ('module', 'module-builds', 'pattern'):
        m = rt.modules.get(loc.module)
        if m is None:
            raise _Refused('module-unknown')
        if loc.kind == 'module':
            off = loc.offset
        elif loc.kind == 'module-builds':
            if build is None or build not in loc.offsets:
                raise _Refused('build-unknown')
            off = loc.offsets[build]
        else:
            off = _search(m, loc, cache) + loc.offset
        need = plat.pointer_size if loc.path else size
        if off < 0 or (m.size is not None and off + need > m.size):
            raise _Refused('outside')
        place = _Place((m.base + off) & ADDRESS_MASK, loc.module, off, m)
    else:
        r = rt.regions.get(loc.region)
        if r is None:
            raise _Refused('region-unknown')
        if loc.kind == 'region':
            off = loc.offset
        else:
            if build is None or build not in loc.offsets:
                raise _Refused('build-unknown')
            off = loc.offsets[build]
        need = plat.pointer_size if loc.path else size
        if off + need > r[1]:
            raise _Refused('outside')
        place = _Place((r[0] + off) & ADDRESS_MASK)
    if not loc.path:
        return place
    address = place.address
    mask = (1 << (8 * plat.pointer_size)) - 1
    for step in loc.path:
        raw = rt.read(address, plat.pointer_size)
        if raw is None:
            raise _Waiting()
        pointer = int.from_bytes(raw, plat.byte_order)
        if pointer == 0:
            raise _Waiting()
        address = (pointer + step) & mask
    if rt.read(address, size) is None:
        raise _Waiting()
    return _Place(address)


def _search(m, loc, cache):
    key = (id(m), tuple(-1 if b is None else b for b in loc.find), loc.section)
    if key in cache:
        found = cache[key]
    else:
        found = find_all(m, loc.find, loc.section, 2)
        cache[key] = found
    if not found:
        raise _Refused('pattern-not-found')
    if len(found) > 1:
        raise _Refused('pattern-ambiguous')
    return found[0]


def compile_pattern(items):
    """A regular expression that finds a pattern (a list of byte values and None) at every offset, overlapping."""
    body = b''.join(b'.' if b is None else re.escape(bytes([b])) for b in items)
    return re.compile(b'(?=' + body + b')', re.DOTALL)


def find_all(m, items, section=None, limit=None):
    """Every module offset where the pattern matches, within the searched sections, up to limit matches."""
    if m.data is None:
        return []
    rx = compile_pattern(items)
    found, seen = [], set()
    for start, end in m.search_ranges(section):
        end = min(end, len(m.data))
        for hit in rx.finditer(m.data, start, end):
            if hit.start() + len(items) > end:
                break
            if hit.start() not in seen:            # sections that overlap count a place once
                seen.add(hit.start())
                found.append(hit.start())
            if limit is not None and len(found) >= limit:
                return found
    return found


def _check_expect(rt, place, expect, data):
    if place.in_module is not None and place.in_module.data is not None:
        start = place.offset
        actual = bytes(place.in_module.data[start:start + len(expect)])
    else:
        actual = rt.read(place.address, len(expect))
        if actual is None:
            raise _Refused('outside')
    if actual != expect and (data is None or actual != data):
        raise _Refused('expect-mismatch')


def _closure(f, cheat_id):
    """The cheat and every cheat it requires, directly or not, in file order."""
    seen, todo = set(), [cheat_id]
    while todo:
        x = todo.pop()
        if x in seen:
            continue
        seen.add(x)
        todo.extend(f.cheat(x).requires)
    return [c.id for c in f.cheats if c.id in seen]


def _relations(f, results):
    changed = True
    while changed:
        changed = False
        for c in f.cheats:
            r = results[c.id]
            if r.state == 'refused':
                continue
            for req in c.requires:
                if results[req].state == 'refused':
                    results[c.id] = CheatResult(c.id, 'refused', 'requires:' + req)
                    changed = True
                    break
    for c in f.cheats:
        r = results[c.id]
        if r.state == 'refused':
            continue
        conflict = _conflict_in(f, _closure(f, c.id), c.id)
        if conflict:
            results[c.id] = CheatResult(c.id, 'refused', 'excludes:' + conflict)


def _conflict_in(f, group, own):
    """A cheat of the group that can never be on together with the rest: what `excludes:<id>` names, or None."""
    members = set(group)
    for x in group:
        for y in f.cheat(x).excludes:
            if y in members:
                return y if y != own else x
    for i, x in enumerate(group):
        rx = f.cheat(x).radio
        if rx is None:
            continue
        for y in group[i + 1:]:
            if f.cheat(y).radio == rx:
                return y if y != own else x
    return None


def _conflicts(f, a, b):
    ca, cb = f.cheat(a), f.cheat(b)
    return a != b and (b in ca.excludes or a in cb.excludes or (ca.radio is not None and ca.radio == cb.radio))


def _switch(f, results, choices, rt):
    wanted = [c.id for c in f.cheats if not c.master and
              ((c.default and c.id not in choices.off) or c.id in choices.on)]
    on = []
    for x in wanted:
        if results[x].state == 'refused':
            continue
        group = _closure(f, x)
        on = [y for y in on if not any(_conflicts(f, y, g) for g in group)]
        on = _drop_unmet(f, on)
        for g in group:
            if g not in on:
                on.append(g)
    users = [y for y in on if not f.cheat(y).master]
    needed = set()
    for y in users:
        needed.update(_closure(f, y))
    on = [y for y in on if y in needed]
    for c in f.cheats:
        r = results[c.id]
        if r.state == 'refused':
            continue
        if c.id not in on:
            r.state = 'off'
        elif r.path_broken:
            r.state, r.reason = 'waiting', 'path-broken'
        elif c.hold and not set(c.hold.split('+')) <= rt.held:
            r.state = 'waiting'
        else:
            r.state = 'on'


def _drop_unmet(f, on):
    while True:
        kept = [y for y in on if all(req in on for req in f.cheat(y).requires)]
        if kept == on:
            return on
        on = kept
