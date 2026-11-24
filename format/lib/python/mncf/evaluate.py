"""Identifies the build and gives every cheat its state and reason (sections 5, 6, 11 and Appendix B)."""
import math
import re
from dataclasses import dataclass, field

from . import encode as enc
from .model import Condition, Override, Patch, Place, Read, ValueRef, Write
from .values import FLOAT_TYPES, encode as encode_value, fits, hex_bytes, type_size

CAPABILITIES = ('guest-read', 'guest-write', 'code-bytes', 'code-hook', 'buttons', 'display')
# Parts of the core a runtime may not have yet (spec, section 11.1), in the order they are checked.
FEATURES = ('run.once', 'run.action', 'run.timed', 'run.monitor', 'hold', 'toggle', 'press', 'when', 'when.many',
            'match.any', 'trigger.enter', 'interval', 'override')
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
                 build=None, held=(), unsupported=(), rewritten=None):
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
        self.unsupported = set(unsupported)   # parts of the core this runtime does not have yet: FEATURES
        self.rewritten = rewritten or {}      # module name -> (offset, length) runs of code the runtime changed

    def was_rewritten(self, module, offset, length):
        return any(o < offset + length and offset < o + n for o, n in self.rewritten.get(module, ()))

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
    plans: list = field(default=None, repr=False)      # the planned operations, for the pass engine
    raw_entry: object = field(default=None, repr=False)


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
    return load_state(f, rt, choices or Choices(), {})[0]


def load_state(f, rt, choices, cache):
    """The evaluation, the cheats that are on, and each cheat's own result before relations were looked at."""
    build = identify(f, rt)
    arch = rt.arch or f.platform.archs[0]
    own = {c.id: _own(f, c, rt, build, arch, choices, cache) for c in f.cheats}
    results = dict(own)
    _relations(f, results)
    on = _switch(f, results, choices, rt)
    return Evaluation(build, [results[c.id] for c in f.cheats]), on, own


def own_result(f, c, rt, build, choices, cache):
    """A cheat's own result for the choices: refused with a reason, or its planned operations or raw entry."""
    return _own(f, c, rt, build, rt.arch or f.platform.archs[0], choices, cache)


def relations(f, own):
    """The results after the relations between cheats (section 7.8), from each cheat's own result."""
    results = dict(own)
    _relations(f, results)
    return results


def _own(f, c, rt, build, arch, choices, cache):
    """A cheat's result before its relations to other cheats are looked at."""
    file_reason = _file_reason(f, rt)
    if file_reason:
        return CheatResult(c.id, 'refused', file_reason)
    if c.problem:
        return CheatResult(c.id, 'refused', c.problem)
    return _cheat(f, c, rt, build, arch, choices, cache)


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


def uses(c):
    """The parts of the core in FEATURES that a cheat uses, in that order."""
    conditions = [op for op in c.ops if isinstance(op, Condition)]
    used = {f'run.{c.run}' if c.run != 'on' else None, 'hold' if c.hold else None, 'toggle' if c.toggle else None,
            'press' if c.press else None, 'when' if conditions else None, 'when.many' if len(conditions) > 1 else None,
            'match.any' if c.match == 'any' and conditions else None,
            'trigger.enter' if c.trigger == 'enter' and conditions else None,
            'interval' if c.interval != 1 and conditions else None,
            'override' if any(isinstance(op, Override) for op in c.ops) else None}
    return [k for k in FEATURES if k in used]


def _cheat(f, c, rt, build, arch, choices, cache):
    missing_part = next((k for k in uses(c) if k in rt.unsupported), None)
    if missing_part:
        return CheatResult(c.id, 'refused', 'runtime:unsupported:' + missing_part)
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
            return _core(f, c, rt, build, arch, choices, cache)
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
    return CheatResult(c.id, 'off', '', 'raw', [ResolvedOp('raw', syntax=r.syntax, build=r.build, code=r.code)],
                       raw_entry=r)


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


def _core(f, c, rt, build, arch, choices, cache):
    """Resolves a cheat's operations for the runtime as it is now: plans them, then follows their pointer paths."""
    ops, plans, waiting = [], [], False
    plat = f.platform
    try:
        for i, op in enumerate(c.ops):
            p = _plan(f, c, i, op, rt, build, arch, choices, cache)
            plans.append(p)
            places = follow_all(plat, p, rt.read)
            if places is None:
                waiting = True
                ops.append(ResolvedOp(p.kind))
                continue
            if p.path_expect:
                _check_expect(rt, places[0], p.expect, p.data if p.kind == 'patch' else None)
            ops.append(p.resolved(places))
    except _Refused as r:
        return CheatResult(c.id, 'refused', r.reason)
    return CheatResult(c.id, 'off', '', 'core', ops, waiting, plans)


@dataclass
class Target:
    """A place an operation acts on before its pointer path is followed: the place of its base, the path, and how
    many bytes the operation reads or writes there."""
    base: object                   # a _Place
    path: list = None
    size: int = 0


@dataclass
class Plan:
    """One operation of a cheat, resolved as far as it can be without reading guest memory that changes.

    `targets` holds the operation's place and, for a condition that compares with another place, that place too.
    `data` is what a patch, a write or an override stub writes; an override that the runtime hooks has none.
    """
    kind: str
    index: int
    op: object
    targets: list
    data: bytes = None
    expect: bytes = None
    type: str = None
    action: str = None
    value: object = None
    label: str = None
    path_expect: bool = False      # a patch or override stub behind a pointer path: expect is checked there

    def resolved(self, places):
        p = places[0]
        if self.kind == 'patch':
            return ResolvedOp('patch', p.address, p.module, p.offset, expect=self.expect, bytes=self.data)
        if self.kind == 'write':
            return ResolvedOp('write', p.address, p.module, p.offset, self.type, expect=self.expect, bytes=self.data)
        if self.kind == 'override':
            return ResolvedOp('override', p.address, p.module, p.offset, action=self.action, value=self.value,
                              expect=self.expect, bytes=self.data)
        if self.kind == 'read':
            return ResolvedOp('read', p.address, p.module, p.offset, self.type, label=self.label)
        v = self.value
        if isinstance(v, Target):
            v = {'address': places[1].address, 'type': self.op.value.type}
        elif isinstance(v, tuple):
            v = list(v)
        return ResolvedOp('when', p.address, p.module, p.offset, self.type, op=self.op.op, value=v)


def _plan(f, c, index, op, rt, build, arch, choices, cache):
    order = f.platform.byte_order
    if isinstance(op, Patch):
        target = _target(f, op.at, rt, build, len(op.expect), cache)
        if op.encode is not None:
            e = op.encode
            v = _value(c, e['value'], choices) if isinstance(e['value'], ValueRef) else e['value']
            data = enc.arm64_imm(e['insn'], e['width'], e['rd'], v)
            if data is None:
                raise _Refused('value-out-of-range')
        else:
            data = op.bytes
        if not target.path:
            _check_rewritten(rt, target.base, len(op.expect))
            _check_expect(rt, target.base, op.expect, data)
        return Plan('patch', index, op, [target], data=data, expect=op.expect, path_expect=bool(target.path))
    if isinstance(op, Write):
        size = len(op.value) if op.type == 'bytes' else type_size(op.type)
        target = _target(f, op.at, rt, build, size, cache)
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
        return Plan('write', index, op, [target], data=data, expect=expect, type=op.type)
    if isinstance(op, Override):
        v = op.value
        if isinstance(v, ValueRef):
            v = int(_value(c, v, choices))
        patches = _by_bytes(op, rt, arch)
        stub = enc.stub(arch, op.action, v) if patches else None
        size = len(op.expect) if op.expect is not None else 4
        target = _target(f, op.function, rt, build, size, cache)
        if patches:
            if len(stub) > len(op.expect):
                raise _Refused('value-out-of-range' if isinstance(op.value, ValueRef) else 'invalid:override.expect')
            if not target.path:
                _check_rewritten(rt, target.base, len(stub))
                _check_expect(rt, target.base, op.expect, None)
        return Plan('override', index, op, [target], data=stub, expect=op.expect, action=op.action, value=v,
                    path_expect=patches and bool(target.path))
    if isinstance(op, Read):
        target = _target(f, op.at, rt, build, type_size(op.type), cache)
        return Plan('read', index, op, [target], type=op.type, label=op.label)
    targets = [_target(f, op.at, rt, build, type_size(op.type), cache)]
    v = op.value
    if isinstance(v, ValueRef):
        v = _value(c, v, choices)
    elif isinstance(v, Place):
        other = _target(f, v.at, rt, build, type_size(v.type), cache)
        targets.append(other)
        v = other
    return Plan('when', index, op, targets, type=op.type, value=v)


@dataclass
class _Place:
    address: int
    module: str = None
    offset: int = None
    in_module: object = None       # the Module whose own bytes the place is in, for expect


def _target(f, loc, rt, build, size, cache):
    """The place of a location's base (section 6.2), checked, and the pointer path that follows it."""
    plat = f.platform
    need = plat.pointer_size if loc.path else size
    if loc.kind == 'address':
        place = _Place(loc.address)
        if rt.read(loc.address, need) is None:
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
        if off + need > r[1]:
            raise _Refused('outside')
        place = _Place((r[0] + off) & ADDRESS_MASK)
    return Target(place, loc.path or None, size)


def follow(plat, target, read):
    """The place a target names now: its base, or where its pointer path leads. None where the path does not resolve
    at the moment: a null pointer, or a pointer or the place outside guest memory."""
    if not target.path:
        return target.base
    address = target.base.address
    mask = (1 << (8 * plat.pointer_size)) - 1
    for step in target.path:
        raw = read(address, plat.pointer_size)
        if raw is None:
            return None
        pointer = int.from_bytes(raw, plat.byte_order)
        if pointer == 0:
            return None
        address = (pointer + step) & mask
    if read(address, target.size) is None:
        return None
    return _Place(address)


def follow_all(plat, plan, read):
    """The places of every target of a planned operation, or None if one of them does not resolve now."""
    places = []
    for t in plan.targets:
        p = follow(plat, t, read)
        if p is None:
            return None
        places.append(p)
    return places


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


def _check_rewritten(rt, place, length):
    """Refuses a change over code the runtime rewrote for its own reasons (section 11.3)."""
    if place.module is not None and rt.was_rewritten(place.module, place.offset, length):
        raise _Refused('runtime:rewritten')


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
    """Switches on the cheats that are on when the file is loaded (section 11.2) and gives every cheat its state.
    Returns the cheats that are on."""
    wanted = [c.id for c in f.cheats if not c.master and
              ((c.default and c.id not in choices.off) or c.id in choices.on)]
    on = []
    for x in wanted:
        if results[x].state != 'refused':
            on = switch_on(f, on, x)
    for c in f.cheats:
        r = results[c.id]
        if r.state == 'refused':
            continue
        if c.id not in on:
            r.state = 'off'
        elif r.path_broken:
            r.state, r.reason = 'waiting', 'path-broken'
        elif r.uses == 'core' and waits_for_buttons(c, rt.held, False):
            r.state = 'waiting'
        else:
            r.state = 'on'
    return on


def waits_for_buttons(c, held, pressed_once):
    """Whether a cheat that is on waits for buttons: held buttons it needs, or the first press of an action."""
    if c.hold and not set(c.hold.split('+')) <= set(held):
        return True
    return c.run == 'action' and not pressed_once


def switch_on(f, on, x):
    """The cheats that are on after x is switched on (section 7.8): x and the cheats it requires are on, and every
    cheat that conflicts with one of them is off, with the cheats that require it."""
    group = _closure(f, x)
    on = [y for y in on if not any(_conflicts(f, y, g) for g in group)]
    on = _drop_unmet(f, on)
    for g in group:
        if g not in on:
            on.append(g)
    return _drop_masters(f, on)


def switch_off(f, on, x):
    """The cheats that are on after x is switched off: without x, the cheats that require it, and the master cheats
    that nothing on needs any more."""
    return _drop_masters(f, _drop_unmet(f, [y for y in on if y != x]))


def _drop_masters(f, on):
    needed = set()
    for y in on:
        if not f.cheat(y).master:
            needed.update(_closure(f, y))
    return [y for y in on if y in needed]


def _drop_unmet(f, on):
    while True:
        kept = [y for y in on if all(req in on for req in f.cheat(y).requires)]
        if kept == on:
            return on
        on = kept
