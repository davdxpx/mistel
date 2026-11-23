"""Runs the cheats of a file pass by pass, as a runtime does (sections 7.2, 7.3, 7.9, 11.3 and 11.5).

    eng = mncf.Engine(f, rt, mncf.Choices(on=['money']))
    eng.switch('speed', True)                  # what the player does between passes
    eng.set_value('money.count', 5000)
    eng.hold(['ZL'])
    p = eng.tick(read_guest_memory)            # one pass: the changes to make, each cheat's state, monitor values
    for ch in p.changes:
        ...                                    # patch, restore, hook, unhook, write, raw
    eng.stop()                                 # the changes that put back every place the cheats changed

The engine reads guest memory only through the reader it is given, and changes nothing itself: the caller makes the
changes of each pass, all code changes in one stop.
"""
from dataclasses import dataclass, field

from .evaluate import ADDRESS_MASK, Choices, CheatResult, Target, follow_all, load_state, own_result, relations, \
    switch_off, switch_on, waits_for_buttons
from .values import decode, hex_bytes, type_size


@dataclass
class Change:
    """One change of a pass (section 11.5).

    `patch` and `restore` write `bytes` at `address` in code; `hook` makes the function at `address` do `action`
    (returning `value`) and `unhook` lets it run as it was; `write` writes `bytes` into guest memory; `raw` runs a
    cheat's raw entry (`syntax`, `build`, `code`) once.
    """
    kind: str
    address: int = None
    bytes: bytes = None
    cheat: str = None
    action: str = None
    value: object = None
    syntax: str = None
    build: str = None
    code: str = None

    def as_dict(self):
        out = {}
        for k, v in self.__dict__.items():
            if v is not None:
                out[k] = hex_bytes(v) if isinstance(v, (bytes, bytearray)) else v
        return out


@dataclass
class CheatState:
    id: str
    state: str           # off, on, waiting, refused
    reason: str = ''


@dataclass
class Shown:
    """A value a monitor read in a pass."""
    label: str
    type: str
    value: object

    def as_dict(self):
        out = {'type': self.type, 'value': self.value}
        if self.label is not None:
            out['label'] = self.label
        return out


@dataclass
class Pass:
    """What one pass did: its number (from 1), its changes in order, every cheat's state after it, in file order,
    and the values the monitors read, by cheat."""
    number: int
    changes: list
    cheats: list
    shown: dict

    def cheat(self, cheat_id):
        for c in self.cheats:
            if c.id == cheat_id:
                return c
        return None


@dataclass
class _Run:
    """What the engine keeps for one cheat between passes."""
    refused: str = None          # the reason it is refused, or None
    state: str = 'off'
    reason: str = ''
    done: bool = False           # a once or timed cheat that switched itself off
    passes: int = 0              # passes since it was switched on
    started: float = None        # the time of its first pass, for timed
    pressed: bool = False        # an action that was pressed since it was switched on
    held_before: bool = False    # whether its conditions held at its last test, for trigger = "enter"
    last: dict = field(default_factory=dict)     # condition index -> bytes read at its last test, for "changed"
    active: bool = False         # whether its patches and overrides are applied
    covers: dict = field(default_factory=dict)   # operation index -> (address, bytes) of an applied patch or stub
    hooks: dict = field(default_factory=dict)    # operation index -> (address, action, value) of a hook

    def restart(self):
        self.done, self.passes, self.started, self.pressed = False, 0, None, False
        self.held_before, self.last = False, {}


@dataclass
class _Work:
    """What one cheat does in a pass."""
    active: bool = False
    keep: bool = False           # a pass between two tests of an interval: nothing changes
    places: dict = None          # operation index -> resolved places
    out: list = field(default_factory=list)
    shown: list = None
    acted: bool = False


class _Reads:
    """Guest memory as one pass reads it: each place once."""

    def __init__(self, read):
        self._read = read
        self._seen = {}

    def __call__(self, address, size):
        key = (address, size)
        if key not in self._seen:
            raw = self._read(address, size)
            self._seen[key] = bytes(raw) if raw is not None else None
        return self._seen[key]


def _buttons(text):
    return frozenset(text.split('+'))


class Engine:
    """The cheats of a loaded file in one runtime, from pass to pass.

    `rate` is the number of passes per second. A `timed` cheat measures its time with the `now` the caller gives to
    each pass, in seconds, or, where the caller gives none, with the number of passes before it divided by `rate`.
    """

    def __init__(self, f, rt, choices=None, rate=60):
        if not rate > 0:
            raise ValueError('rate is the number of passes per second, above 0')
        choices = choices or Choices()
        self.file = f
        self.runtime = rt
        self.rate = rate
        self._values = dict(choices.values)
        self._cache = {}
        ev, on, self._own = load_state(f, rt, Choices(choices.on, choices.off, self._values), self._cache)
        self.build = ev.build
        self.evaluation = ev
        self._on = on
        self._runs = {}
        for r in ev.cheats:
            self._runs[r.id] = _Run(refused=r.reason if r.state == 'refused' else None, state=r.state,
                                    reason=r.reason)
        self._held = set(rt.held)
        self._held_before = set(rt.held)
        self._presses = []
        self._number = 0
        self._kept = {}              # address -> original byte, for every code byte a cheat covers
        self._now = {}               # address -> the byte the engine knows is there now
        self._hooked = {}            # address -> (action, value)

    # What the player does between passes

    def switch(self, cheat_id, on=True):
        """Switches a cheat on or off as the player does (section 7.8). A master cheat is not a switch, and a refused
        cheat cannot be switched on: both stay as they are. Returns whether the cheat is on afterwards."""
        c = self._cheat(cheat_id)
        run = self._runs[cheat_id]
        if c.master or run.refused:
            return cheat_id in self._on
        if on and cheat_id not in self._on:
            self._set_on(switch_on(self.file, self._on, cheat_id))
        elif not on and cheat_id in self._on:
            self._set_on(switch_off(self.file, self._on, cheat_id))
        return cheat_id in self._on

    def set_value(self, key, value):
        """Sets a value the player chose: the key is the cheat's id and the value's id, joined by a dot. A value the
        player cannot choose refuses the cheat with value-out-of-range, until a value that fits is set."""
        cheat_id, _, value_id = key.partition('.')
        c = self._cheat(cheat_id)
        if value_id not in c.values:
            raise KeyError(f'cheat {cheat_id} has no value {value_id}')
        self._values[key] = value
        self._own[cheat_id] = own_result(self.file, c, self.runtime, self.build, Choices(values=self._values),
                                         self._cache)
        self._refresh()

    def hold(self, buttons):
        """Sets the buttons the player holds now."""
        self._held = set(buttons)

    def press(self, buttons):
        """Says that the player pressed these buttons together since the last pass, as "ZL+R" or a list of names,
        for a press shorter than a pass."""
        self._presses.append(_buttons(buttons) if isinstance(buttons, str) else frozenset(buttons))

    # What the engine says

    def states(self):
        """Every cheat's state now, in file order."""
        return [CheatState(c.id, self._runs[c.id].state, self._runs[c.id].reason) for c in self.file.cheats]

    def state(self, cheat_id):
        run = self._runs[self._cheat(cheat_id).id]
        return CheatState(cheat_id, run.state, run.reason)

    # Passes

    def tick(self, memory=None, now=None):
        """Runs one pass and returns what to change. `memory(address, size)` reads guest memory as it is at this
        pass (default: the runtime's reader); `now` is the time in seconds."""
        f = self.file
        self._number += 1
        if now is None:
            now = (self._number - 1) / self.rate
        read = _Reads(memory if memory is not None else self.runtime.read)
        pressed = self._pressed()
        self._held_before = set(self._held)
        self._presses = []

        for c in f.cheats:                                      # toggles, in file order
            if c.toggle and not c.master and not self._runs[c.id].refused and _buttons(c.toggle) in pressed:
                self.switch(c.id, c.id not in self._on)
        for c in f.cheats:                                      # timed cheats whose time is over
            run = self._runs[c.id]
            if c.run == 'timed' and c.id in self._on:
                if run.started is None:
                    run.started = now
                if now - run.started >= c.duration:
                    self._set_on(switch_off(f, self._on, c.id), done=c.id)

        work = {}
        for c in f.cheats:
            if c.id in self._on and not self._runs[c.id].refused:
                work[c.id] = self._work(c, read, pressed)

        checked = self._check(work, read)
        for c in f.cheats:                                      # what a refusal in this pass switched off
            if c.id in work and (c.id not in self._on or self._runs[c.id].refused):
                del work[c.id]
        changes = self._code(work, checked)
        shown = {}
        for c in f.cheats:
            w = work.get(c.id)
            if w is None:
                continue
            changes.extend(w.out)
            if w.shown is not None:
                shown[c.id] = w.shown
        for c in f.cheats:                                      # once cheats that did their work
            w = work.get(c.id)
            if c.run == 'once' and w is not None and w.acted and c.id in self._on:
                self._set_on(switch_off(f, self._on, c.id), done=c.id)
        return Pass(self._number, changes, self.states(), shown)

    def stop(self):
        """Switches every cheat off and returns the changes that put back every place the cheats changed, as before a
        file is loaded again or the game stops (section 11.3)."""
        self._set_on([])
        return self._code({}, {})

    # The parts of a pass

    def _pressed(self):
        """The button combinations of the file that were pressed since the last pass."""
        combos = set()
        for c in self.file.cheats:
            for text in (c.toggle, c.press):
                if text:
                    combos.add(_buttons(text))
        out = set()
        for combo in combos:
            if (combo <= self._held and not combo <= self._held_before) or any(combo <= p for p in self._presses):
                out.add(combo)
        return out

    def _work(self, c, read, pressed):
        """What a cheat that is on does in this pass, and its state after it."""
        run = self._runs[c.id]
        own = self._own[c.id]
        run.passes += 1
        if own.uses == 'raw':
            r = own.raw_entry
            run.state, run.reason = 'on', ''
            return _Work(out=[Change('raw', cheat=c.id, syntax=r.syntax, build=r.build, code=r.code)], acted=True)
        if (run.passes - 1) % c.interval:
            return _Work(active=run.active, keep=True)
        pressing = c.run == 'action' and _buttons(c.press) in pressed
        if waits_for_buttons(c, self._held, run.pressed or pressing):
            return self._wait(run, '')
        plat = self.file.platform
        places = {}
        for p in own.plans:
            ps = follow_all(plat, p, read)
            if ps is None:
                return self._wait(run, 'path-broken')
            places[p.index] = ps
        result = self._test(c, own.plans, places, read)
        if result is None:
            return self._wait(run, 'path-broken')
        holds, last = result
        acts = holds and (c.trigger == 'while' or not run.held_before) and (c.run != 'action' or pressing)
        out, shown = [], None
        if acts:
            order = plat.byte_order
            for p in own.plans:
                address = places[p.index][0].address
                if p.kind == 'write':
                    if p.expect is not None:
                        there = read(address, len(p.expect))
                        if there is None:
                            return self._wait(run, 'path-broken')
                        if there != p.expect:
                            continue
                    out.append(Change('write', address, p.data, cheat=c.id))
                elif p.kind == 'read':
                    raw = read(address, type_size(p.type))
                    if raw is None:
                        return self._wait(run, 'path-broken')
                    shown = (shown or []) + [Shown(p.label, p.type, decode(p.type, raw, order))]
        run.held_before, run.last = holds, last
        if acts and c.run == 'action':
            run.pressed = True
        run.state, run.reason = ('waiting' if c.run == 'action' and not run.pressed else 'on'), ''
        return _Work(active=holds and (c.run != 'action' or run.pressed), places=places, out=out, shown=shown,
                     acted=acts)

    @staticmethod
    def _wait(run, reason):
        run.state, run.reason = 'waiting', reason
        return _Work()

    def _test(self, c, plans, places, read):
        """Whether the cheat's conditions hold now, and the bytes its `changed` conditions read; None if a place
        cannot be read. Every condition is tested, so that the result does not depend on the order of the tests."""
        order = self.file.platform.byte_order
        results, last = [], {}
        run = self._runs[c.id]
        for p in plans:
            if p.kind != 'when':
                continue
            raw = read(places[p.index][0].address, type_size(p.type))
            if raw is None:
                return None
            if p.op.op == 'changed':
                last[p.index] = raw
                results.append(p.index in run.last and run.last[p.index] != raw)
                continue
            v = p.value
            if isinstance(v, Target):
                other = read(places[p.index][1].address, type_size(p.op.value.type))
                if other is None:
                    return None
                v = decode(p.op.value.type, other, order)
            results.append(_holds(p.op.op, decode(p.type, raw, order), v))
        if not results:
            return True, last
        return (all(results) if c.match == 'all' else any(results)), last

    def _check(self, work, read):
        """Checks `expect` for every patch and override stub that covers a place it did not cover in the pass before
        (section 7.4), and refuses the cheats where it differs. Returns the original bytes found, by place."""
        checked = {}
        refused = []
        for c in self.file.cheats:
            w = work.get(c.id)
            if w is None or not w.active or w.keep:
                continue
            run = self._runs[c.id]
            for p in self._own[c.id].plans:
                if p.kind not in ('patch', 'override') or p.data is None:
                    continue
                address = w.places[p.index][0].address
                cover = run.covers.get(p.index)
                if cover is not None and cover[0] == address and len(cover[1]) == len(p.data):
                    continue
                n = len(p.expect)
                there = None
                if any(((address + i) & ADDRESS_MASK) not in self._kept for i in range(n)):
                    there = read(address, n)
                    if there is None:
                        refused.append((c.id, 'outside'))
                        break
                found = bytes(self._kept.get((address + i) & ADDRESS_MASK, there[i] if there else 0)
                              for i in range(n))
                if found != p.expect and (p.kind == 'override' or found != p.data):
                    refused.append((c.id, 'expect-mismatch'))
                    break
                checked[(c.id, p.index)] = found
        for cheat_id, reason in refused:
            self._own[cheat_id] = CheatResult(cheat_id, 'refused', reason)
        if refused:
            self._refresh()
        return checked

    def _code(self, work, checked):
        """The code changes of a pass (section 11.5). Every code byte that a cheat that is active covers gets that
        cheat's byte, the last such cheat in file order winning; every other byte the engine changed gets its original
        back. A patch or stub is written whole when one of its bytes is not what it should be; the bytes no cheat
        covers any more are put back whole, in runs, except a run whose bytes all hold their originals already."""
        want, hooks, new_kept, new_now = {}, {}, {}, {}
        ranges = []                                 # (address, length) of every active patch and stub
        for c in self.file.cheats:
            w = work.get(c.id)
            run = self._runs[c.id]
            if w is None or not w.active:
                run.active, run.covers, run.hooks = False, {}, {}
                continue
            covers, hooked = {}, {}
            for p in self._own[c.id].plans:
                if p.kind not in ('patch', 'override'):
                    continue
                if w.keep:                          # between two tests: as the last test left it
                    if p.data is None:
                        if p.index in run.hooks:
                            address, action, value = run.hooks[p.index]
                            hooks[address] = (action, value)
                            hooked[p.index] = run.hooks[p.index]
                        continue
                    if p.index not in run.covers:
                        continue
                    address, data = run.covers[p.index]
                else:
                    address = w.places[p.index][0].address
                    data = p.data
                if data is None:
                    hooks[address] = (p.action, p.value)
                    hooked[p.index] = (address, p.action, p.value)
                    continue
                found = checked.get((c.id, p.index))
                for i, b in enumerate(data):
                    a = (address + i) & ADDRESS_MASK
                    want[a] = b
                    if a not in self._kept and a not in new_kept:
                        new_kept[a] = p.expect[i]
                        new_now[a] = found[i] if found is not None else p.expect[i]
                covers[p.index] = (address, data)
                ranges.append((address, len(data)))
            run.active, run.covers, run.hooks = True, covers, hooked
        self._kept.update(new_kept)
        self._now.update(new_now)

        marked = {}                                 # address -> 'patch' or 'restore'
        for address, n in ranges:
            places = [(address + i) & ADDRESS_MASK for i in range(n)]
            if any(want[a] != self._now[a] for a in places):
                for a in places:
                    marked[a] = 'patch'
        for a in self._kept:
            if a not in want:
                marked[a] = 'restore'
        runs = []                                   # [start, kind, bytes, whether a byte differs]
        for a in sorted(marked):
            kind = marked[a]
            target = want[a] if kind == 'patch' else self._kept[a]
            if runs and runs[-1][1] == kind and runs[-1][0] + len(runs[-1][2]) == a:
                runs[-1][2].append(target)
            else:
                runs.append([a, kind, bytearray([target]), False])
            runs[-1][3] = runs[-1][3] or target != self._now[a]
        for a, kind in marked.items():
            if kind == 'patch':
                self._now[a] = want[a]
            else:
                del self._kept[a]
                del self._now[a]
        code = [(start, 0, Change(kind, start, bytes(data))) for start, kind, data, differs in runs if differs]
        for a in sorted(set(self._hooked) | set(hooks)):
            if a in hooks:
                if self._hooked.get(a) != hooks[a]:
                    code.append((a, 1, Change('hook', a, action=hooks[a][0], value=hooks[a][1])))
            else:
                code.append((a, 1, Change('unhook', a)))
        self._hooked = hooks
        code.sort(key=lambda x: (x[0], x[1]))
        return [ch for _, _, ch in code]

    # Switching

    def _set_on(self, on, done=None):
        """Makes these the cheats that are on, and gives the ones that changed their state."""
        before = set(self._on)
        self._on = on
        for c in self.file.cheats:
            run = self._runs[c.id]
            if run.refused:
                continue
            if c.id in before and c.id not in on:
                run.state, run.reason = 'off', ('done' if c.id == done else '')
                run.done = c.id == done
            elif c.id not in before and c.id in on:
                run.restart()
                waits = self._own[c.id].uses == 'core' and waits_for_buttons(c, self._held, False)
                run.state, run.reason = ('waiting' if waits else 'on'), ''

    def _refresh(self):
        """Gives every cheat the result of its relations after a cheat's own result changed, and switches off the
        cheats that are refused now."""
        results = relations(self.file, self._own)
        for c in self.file.cheats:
            r = results[c.id]
            run = self._runs[c.id]
            if r.state == 'refused':
                run.refused, run.state, run.reason = r.reason, 'refused', r.reason
            elif run.refused:
                run.refused, run.state, run.reason = None, 'off', ''
        on = self._on
        for c in self.file.cheats:
            if c.id in on and self._runs[c.id].refused:
                on = switch_off(self.file, on, c.id)
        self._set_on(on)

    def _cheat(self, cheat_id):
        c = self.file.cheat(cheat_id)
        if c is None:
            raise KeyError(f'the file has no cheat {cheat_id}')
        return c


def _holds(op, a, v):
    """Whether a condition's operator holds between the value read and the value it compares with (section 7.9)."""
    if isinstance(v, bool):
        v = int(v)
    if op == 'between':
        return v[0] <= a <= v[1]
    if op == '==':
        return a == v
    if op == '!=':
        return a != v
    if op == '<':
        return a < v
    if op == '<=':
        return a <= v
    if op == '>':
        return a > v
    if op == '>=':
        return a >= v
    if op == '&':
        return (a & v) != 0
    return (a & v) == 0
