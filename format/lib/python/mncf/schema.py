"""Checks a parsed file against the spec and builds the model.

Problems outside a cheat make the whole file unusable (MncfError) or, for keys the spec does not define, a warning.
Problems inside a cheat turn only that cheat off: its `problem` becomes `unknown-key:<key>`, `extension-missing:<name>`
or `invalid:<key>`, where <key> is the key's path from the cheat with its parts joined by dots.
"""
import math
import re

from . import encode as enc
from .model import (Build, Cheat, Condition, Location, MncfError, Override, Patch, Place, Preset, Raw, Read, ValueRef,
                    ValueSpec, Write)
from .values import (BYTES, EXTENSION, FLOAT_TYPES, ID, INT_TYPES, NUMBER_TYPES, PLAYER_VALUE, VALUE_TYPES,
                     WRITE_TYPES, fits, integer, is_int, is_number, parse_bytes, parse_pattern, type_size)

KNOWN_EXTENSIONS = ('raw/1', 'arm64-imm/1')

FILE_KEYS = ('format', 'name', 'authors', 'about', 'file_version', 'language', 'origin', 'extensions', 'game', 'builds',
             'cheat', 'choices', 'preset', 'i18n')
RUNS = ('on', 'once', 'action', 'timed', 'monitor')
OPS = ('==', '!=', '<', '<=', '>', '>=', '&', '!&', 'between', 'changed')
SECTIONS = ('code', 'rodata', 'data')
ACTIONS = ('return', 'skip')
RAW_SYNTAXES = ('atmosphere',)

# The key tree of a cheat, for finding keys the spec does not define. A dict is a table with those keys, ('array', t)
# an array of such tables, ('map', t) a table of user-named tables, None a value. A key in EXTENSION_KEYS needs its
# extension in the file's lists.
_LOC = {k: None for k in ('module', 'offset', 'offsets', 'find', 'section', 'region', 'address', 'path')}
_VALUE = {k: None for k in ('type', 'min', 'max', 'step', 'default', 'unit', 'display', 'choices', 'flags')}
CHEAT_TREE = {
    **{k: None for k in ('id', 'name', 'about', 'group', 'tags', 'default', 'master', 'run', 'duration', 'hold',
                         'toggle', 'press', 'requires', 'excludes', 'radio', 'match', 'trigger', 'interval')},
    'values': ('map', _VALUE),
    'patch': ('array', {'at': _LOC, 'expect': None, 'bytes': None, 'asm': {'expect': None, 'bytes': None},
                        'encode': {'insn': None, 'width': None, 'rd': None, 'value': None}}),
    'write': ('array', {'at': _LOC, 'type': None, 'value': None, 'expect': None}),
    'override': ('array', {'function': _LOC, 'action': None, 'value': None, 'expect': None}),
    'read': ('array', {'at': _LOC, 'type': None, 'label': None}),
    'when': ('array', {'at': _LOC, 'type': None, 'op': None, 'value': {'at': _LOC, 'type': None}}),
    'raw': ('array', {'syntax': None, 'build': None, 'code': None}),
}
EXTENSION_KEYS = {('raw',): 'raw/1', ('patch', 'encode'): 'arm64-imm/1'}


class Problem(Exception):
    """A rule broken inside a cheat."""

    def __init__(self, reason):
        super().__init__(reason)
        self.reason = reason


def _invalid(path):
    return Problem('invalid:' + path if path else 'invalid')


# ---- the file -------------------------------------------------------------------------------------------------------

def build_file(doc, platforms):
    """The checked parts of a parsed file: a dict of MncfFile fields. Raises MncfError."""
    if not doc or next(iter(doc)) != 'format':
        raise MncfError('format', '`format` must be the first key of the file')
    if doc['format'] != 'mncf/1':
        raise MncfError('format', f'the format is {doc["format"]!r}, this reader reads "mncf/1"')
    warnings = [f'unknown-key:{k}' for k in doc if k not in FILE_KEYS]
    out = {'warnings': warnings, 'doc': doc}

    out['name'] = _req_str(doc, 'name')
    for key in ('about', 'file_version', 'language'):
        if key in doc:
            out[key] = _req_str(doc, key)
    if 'authors' in doc:
        out['authors'] = _str_list(doc['authors'], 'authors')

    if 'origin' in doc:
        origin = doc['origin']
        if not isinstance(origin, dict) or not isinstance(origin.get('format'), str) or \
                not all(isinstance(origin.get(k, ''), str) for k in ('name', 'site')):
            raise MncfError('schema:origin', '`origin` must be a table with a `format` string and maybe `name` and '
                                             '`site` strings')
        for k in origin:
            if k not in ('format', 'name', 'site'):
                warnings.append(f'unknown-key:origin.{k}')
        out['origin'] = dict(origin)
    required, optional = [], []
    if 'extensions' in doc:
        ext = doc['extensions']
        if not isinstance(ext, dict):
            raise MncfError('schema:extensions', '`extensions` must be a table')
        for k in ext:
            if k not in ('required', 'optional'):
                warnings.append(f'unknown-key:extensions.{k}')
        required = _str_list(ext.get('required', []), 'extensions.required')
        optional = _str_list(ext.get('optional', []), 'extensions.optional')
        for name in required + optional:
            if not EXTENSION.match(name):
                raise MncfError('schema:extensions', f'{name!r} is not an extension name with its version')
    out['required'], out['optional'] = required, optional

    game = doc.get('game')
    if not isinstance(game, dict):
        raise MncfError('schema:game', 'the `game` table is missing')
    for k in game:
        if k not in ('platform', 'id', 'name'):
            warnings.append(f'unknown-key:game.{k}')
    plat_name = game.get('platform')
    if not isinstance(plat_name, str):
        raise MncfError('schema:game.platform', '`game.platform` must be a string')
    plat = platforms.get(plat_name)
    if plat is None:
        raise MncfError('platform', f'the platform {plat_name!r} is not in Appendix A')
    out['platform'] = plat
    gid = game.get('id')
    if not isinstance(gid, str) or not plat.game_id.match(gid):
        raise MncfError('schema:game.id', f'`game.id` is not a game ID of the {plat_name} platform')
    out['game_id'] = gid
    if 'name' in game:
        if not isinstance(game['name'], str):
            raise MncfError('schema:game.name', '`game.name` must be a string')
        out['game_name'] = game['name']

    builds = {}
    raw_builds = doc.get('builds', {})
    if not isinstance(raw_builds, dict):
        raise MncfError('schema:builds', '`builds` must be a table')
    for key, b in raw_builds.items():
        if not key or not isinstance(b, dict):
            raise MncfError('schema:builds', f'build {key!r} must be a table with a non-empty key')
        for k in b:
            if k not in ('version', 'modules'):
                warnings.append(f'unknown-key:builds.{key}.{k}')
        mods = b.get('modules')
        if not isinstance(mods, dict) or not mods:
            raise MncfError('schema:builds', f'build {key!r} needs a `modules` table')
        for m, ident in mods.items():
            if (plat.modules and m not in plat.modules) or not m:
                raise MncfError('schema:builds', f'build {key!r}: {m!r} is not a module of the {plat_name} platform')
            if not plat.valid_module_id(ident):
                raise MncfError('schema:builds', f'build {key!r}: {ident!r} is not a module identity of {plat_name}')
        version = b.get('version', key)
        if not isinstance(version, str):
            raise MncfError('schema:builds', f'build {key!r}: `version` must be a string')
        builds[key] = Build(key, version, dict(mods))
    out['builds'] = builds

    choices = {}
    raw_choices = doc.get('choices', {})
    if not isinstance(raw_choices, dict):
        raise MncfError('schema:choices', '`choices` must be a table')
    for name, table in raw_choices.items():
        parsed = _choice_list(table)
        if parsed is None:
            raise MncfError('schema:choices', f'choice list {name!r} must map whole numbers to names')
        choices[name] = parsed
    out['choices'] = choices

    raw_cheats = doc.get('cheat')
    if not isinstance(raw_cheats, list) or not raw_cheats or not all(isinstance(c, dict) for c in raw_cheats):
        raise MncfError('schema:cheat', 'the file needs at least one [[cheat]]')
    ids = []
    for i, c in enumerate(raw_cheats):
        cid = c.get('id')
        if not isinstance(cid, str) or not ID.match(cid):
            raise MncfError('schema:cheat.id', f'cheat {i + 1} has no valid `id`')
        if cid in ids:
            raise MncfError(f'duplicate-id:{cid}', f'two cheats have the id {cid!r}')
        ids.append(cid)
    ctx = _Context(plat, builds, choices, set(ids), required + optional)
    out['cheats'] = [check_cheat(i, c, ctx) for i, c in enumerate(raw_cheats)]

    presets = []
    raw_presets = doc.get('preset', [])
    if not isinstance(raw_presets, list):
        raise MncfError('schema:preset', '`preset` must be an array of tables')
    for i, p in enumerate(raw_presets):
        preset = _preset(p, out['cheats'], [x.id for x in presets])
        if preset is None:
            label = p.get('id') if isinstance(p, dict) and isinstance(p.get('id'), str) else str(i + 1)
            warnings.append(f'preset:{label}')
        else:
            presets.append(preset)
    out['presets'] = presets

    i18n = doc.get('i18n', {})
    if not isinstance(i18n, dict) or not all(isinstance(v, dict) for v in i18n.values()):
        warnings.append('i18n')
        i18n = {}
    out['i18n'] = i18n
    return out


def _req_str(table, key, prefix=''):
    v = table.get(key)
    if not isinstance(v, str):
        raise MncfError(f'schema:{prefix}{key}', f'`{prefix}{key}` must be a string')
    return v


def _str_list(v, key):
    if not isinstance(v, list) or not all(isinstance(x, str) for x in v):
        raise MncfError(f'schema:{key}', f'`{key}` must be an array of strings')
    return list(v)


_CHOICE_KEY = re.compile(r'^(-?)(?:0x([0-9A-Fa-f]{1,16})|([0-9]{1,20}))\Z')


def _choice_key(k):
    m = _CHOICE_KEY.match(k)
    if m is None:
        return None
    n = int(m.group(2), 16) if m.group(2) else int(m.group(3))
    return -n if m.group(1) else n


def _choice_list(table):
    if not isinstance(table, dict) or not table:
        return None
    out = {}
    for k, v in table.items():
        n = _choice_key(k)
        if n is None or not isinstance(v, str):
            return None
        out[n] = v
    return out


def _preset(p, cheats, seen):
    if not isinstance(p, dict):
        return None
    pid, name, ids = p.get('id'), p.get('name'), p.get('cheats', [])
    if not isinstance(pid, str) or not ID.match(pid) or pid in seen or not isinstance(name, str):
        return None
    by_id = {c.id: c for c in cheats}
    if not isinstance(ids, list) or not all(isinstance(x, str) and x in by_id for x in ids):
        return None
    values = p.get('values', {})
    if not isinstance(values, dict):
        return None
    numbers = {}
    for k, v in values.items():
        cid, _, vid = k.partition('.')
        cheat = by_id.get(cid)
        spec = cheat.values.get(vid) if cheat is not None else None
        n = _value_number(spec.type, v) if spec is not None else None
        if n is None or not _in_spec(spec, n):
            return None
        numbers[k] = n
    for k in p:
        if k not in ('id', 'name', 'cheats', 'values'):
            return None
    return Preset(pid, name, list(ids), numbers)


# ---- one cheat ------------------------------------------------------------------------------------------------------

class _Context:
    def __init__(self, plat, builds, choices, ids, listed):
        self.platform = plat
        self.builds = builds
        self.choices = choices
        self.ids = ids
        self.listed = listed
        self.unknown_listed = [e for e in listed if e not in KNOWN_EXTENSIONS]


def check_cheat(index, table, ctx):
    cheat = Cheat(index, table['id'], table)
    try:
        _check_keys(table, CHEAT_TREE, (), ctx)
        _fill_cheat(cheat, table, ctx)
    except Problem as p:
        cheat.problem = p.reason
    return cheat


def _check_keys(table, tree, path, ctx):
    for key, value in table.items():
        sub = path + (key,)
        ext = EXTENSION_KEYS.get(sub)
        if key not in tree or (ext is not None and ext not in ctx.listed):
            if ctx.unknown_listed:
                raise Problem('extension-missing:' + ctx.unknown_listed[0])
            raise Problem('unknown-key:' + '.'.join(sub))
        node = tree[key]
        if node is None:
            continue
        if isinstance(node, dict):
            if isinstance(value, dict):
                _check_keys(value, node, sub, ctx)
        elif node[0] == 'array':
            if isinstance(value, list):
                for item in value:
                    if isinstance(item, dict):
                        _check_keys(item, node[1], sub, ctx)
        elif node[0] == 'map':
            if isinstance(value, dict):
                for name, item in value.items():
                    if isinstance(item, dict):
                        _check_keys(item, node[1], sub + (name,), ctx)


def _fill_cheat(c, t, ctx):
    plat = ctx.platform
    c.name = _string(t, 'name', 'name', required=True)
    c.about = _string(t, 'about', 'about')
    if 'group' in t:
        g = t['group']
        if not isinstance(g, str) or not g or any(not part for part in g.split('/')):
            raise _invalid('group')
        c.group = g
    if 'tags' in t:
        if not isinstance(t['tags'], list) or not all(isinstance(x, str) for x in t['tags']):
            raise _invalid('tags')
        c.tags = list(t['tags'])
    c.default = _boolean(t, 'default', False)
    c.master = _boolean(t, 'master', False)
    c.run = t.get('run', 'on')
    if not isinstance(c.run, str) or c.run not in RUNS:
        raise _invalid('run')
    if 'duration' in t or c.run == 'timed':
        d = t.get('duration')
        if c.run != 'timed' or not is_number(d) or not math.isfinite(d) or d <= 0:
            raise _invalid('duration')
        c.duration = d
    for key in ('hold', 'toggle', 'press'):
        if key in t:
            setattr(c, key, _buttons(t[key], key, plat))
    if (c.run == 'action') != (c.press is not None):
        raise _invalid('press')
    for key in ('requires', 'excludes'):
        if key in t:
            v = t[key]
            if not isinstance(v, list) or not all(isinstance(x, str) and x in ctx.ids and x != c.id for x in v):
                raise _invalid(key)
            setattr(c, key, list(v))
    if 'radio' in t:
        if not isinstance(t['radio'], str) or not t['radio']:
            raise _invalid('radio')
        c.radio = t['radio']
    c.match = t.get('match', 'all')
    if not isinstance(c.match, str) or c.match not in ('all', 'any'):
        raise _invalid('match')
    c.trigger = t.get('trigger', 'while')
    if not isinstance(c.trigger, str) or c.trigger not in ('while', 'enter'):
        raise _invalid('trigger')
    c.interval = t.get('interval', 1)
    if not is_int(c.interval) or c.interval < 1:
        raise _invalid('interval')

    if 'values' in t:
        if not isinstance(t['values'], dict):
            raise _invalid('values')
        for vid, spec in t['values'].items():
            c.values[vid] = _value_spec(vid, spec, ctx)

    ops = []
    for key, value in t.items():
        if key in ('patch', 'write', 'override', 'read', 'when', 'raw'):
            if not isinstance(value, list) or not all(isinstance(x, dict) for x in value) or not value:
                raise _invalid(key)
            for item in value:
                if key == 'patch':
                    ops.append(_patch(item, c, ctx))
                elif key == 'write':
                    ops.append(_write(item, c, ctx))
                elif key == 'override':
                    ops.append(_override(item, c, ctx))
                elif key == 'read':
                    ops.append(_read(item, ctx))
                elif key == 'when':
                    ops.append(_condition(item, c, ctx))
                else:
                    c.raw.append(_raw(item, ctx))
    c.ops = ops
    kinds = {type(op) for op in ops}
    if c.run == 'monitor':
        if kinds & {Patch, Write, Override} or Read not in kinds:
            raise _invalid('run')
    elif Read in kinds:
        raise _invalid('read')
    if not (kinds - {Condition}) and not c.raw:
        raise _invalid('')


def _string(t, key, path, required=False):
    if key not in t:
        if required:
            raise _invalid(path)
        return None
    if not isinstance(t[key], str):
        raise _invalid(path)
    return t[key]


def _boolean(t, key, default):
    if key not in t:
        return default
    if not isinstance(t[key], bool):
        raise _invalid(key)
    return t[key]


def _buttons(v, path, plat):
    if not isinstance(v, str) or not v:
        raise _invalid(path)
    names = v.split('+')
    if any(not plat.valid_button(n) for n in names) or len(set(names)) != len(names):
        raise _invalid(path)
    return v


def _value_number(t, v):
    """A number of the file as a value of type t, or None if it is not one that fits."""
    if t == 'bool':
        return v if isinstance(v, bool) else None
    if t in INT_TYPES:
        n = integer(v)
        return n if n is not None and fits(t, n) else None
    if t in FLOAT_TYPES:
        return v if is_number(v) and not math.isnan(v) and fits(t, v) else None
    return None


def _in_spec(s, n):
    """Whether a number is one a player can choose for this value: inside its range and among its choices."""
    return not ((s.min is not None and n < s.min) or (s.max is not None and n > s.max) or
                (s.choices is not None and n not in s.choices))


def _value_spec(vid, spec, ctx):
    path = f'values.{vid}'
    if not ID.match(vid) or not isinstance(spec, dict):
        raise _invalid(path)
    t = spec.get('type')
    if not isinstance(t, str) or t not in VALUE_TYPES:
        raise _invalid(path + '.type')
    s = ValueSpec(vid, t, None)
    for key in ('min', 'max', 'step'):
        if key in spec:
            if t == 'bool':
                raise _invalid(f'{path}.{key}')
            n = _value_number(t, spec[key])
            if n is None or (key == 'step' and n <= 0):
                raise _invalid(f'{path}.{key}')
            setattr(s, key, n)
    if s.min is not None and s.max is not None and s.min > s.max:
        raise _invalid(path + '.max')
    if 'unit' in spec:
        if not isinstance(spec['unit'], str):
            raise _invalid(path + '.unit')
        s.unit = spec['unit']
    if 'display' in spec:
        if not isinstance(spec['display'], str) or spec['display'] not in ('dec', 'hex'):
            raise _invalid(path + '.display')
        s.display = spec['display']
    if 'choices' in spec:
        ch = spec['choices']
        parsed = ctx.choices.get(ch) if isinstance(ch, str) else _choice_list(ch)
        if t not in INT_TYPES or parsed is None or not all(fits(t, k) for k in parsed):
            raise _invalid(path + '.choices')
        s.choices = parsed
    if 'flags' in spec:
        fl = spec['flags']
        parsed = _choice_list(fl) if isinstance(fl, dict) else None
        if t not in INT_TYPES or parsed is None or not all(0 <= b < 8 * type_size(t) for b in parsed):
            raise _invalid(path + '.flags')
        s.flags = parsed
    if 'default' not in spec:
        raise _invalid(path + '.default')
    d = _value_number(t, spec['default'])
    if d is None or (s.min is not None and d < s.min) or (s.max is not None and d > s.max) or \
            (s.choices is not None and d not in s.choices):
        raise _invalid(path + '.default')
    s.default = d
    return s


def _ref(v, c, path, kinds=None):
    m = PLAYER_VALUE.match(v) if isinstance(v, str) else None
    if m is None:
        return None
    spec = c.values.get(m.group(1))
    if spec is None or (kinds is not None and spec.type not in kinds):
        raise _invalid(path)
    return ValueRef(m.group(1))


def location(v, path, ctx):
    """A checked location (section 6.2)."""
    if not isinstance(v, dict):
        raise _invalid(path)
    bases = [k for k in ('module', 'region', 'address') if k in v]
    if len(bases) != 1:
        raise _invalid(path)
    base = bases[0]
    loc = Location(kind=base)
    if 'path' in v:
        p = v['path']
        if not isinstance(p, list) or not p or any(integer(x) is None for x in p):
            raise _invalid(path + '.path')
        loc.path = [integer(x) for x in p]
    if base == 'address':
        if not ctx.platform.addresses:
            raise _invalid(path + '.address')
        for k in ('offset', 'offsets', 'find', 'section'):
            if k in v:
                raise _invalid(f'{path}.{k}')
        a = integer(v['address'])
        if a is None or a < 0:
            raise _invalid(path + '.address')
        loc.address = a
        return loc
    name = v[base]
    if not isinstance(name, str) or not name:
        raise _invalid(f'{path}.{base}')
    setattr(loc, base, name)
    if 'find' in v:
        if base != 'module':
            raise _invalid(path + '.find')
        if 'offsets' in v:
            raise _invalid(path + '.offsets')
        pattern = parse_pattern(v['find'])
        if pattern is None:
            raise _invalid(path + '.find')
        loc.kind, loc.find = 'pattern', pattern
        if 'offset' in v:
            if not is_int(v['offset']):
                raise _invalid(path + '.offset')
            loc.offset = v['offset']
        if 'section' in v:
            if not isinstance(v['section'], str) or v['section'] not in SECTIONS:
                raise _invalid(path + '.section')
            loc.section = v['section']
        return loc
    if 'section' in v:
        raise _invalid(path + '.section')
    if ('offset' in v) == ('offsets' in v):
        raise _invalid(path)
    if 'offset' in v:
        n = integer(v['offset'])
        if n is None or n < 0:
            raise _invalid(path + '.offset')
        loc.offset = n
        return loc
    offs = v['offsets']
    if not isinstance(offs, dict) or not offs:
        raise _invalid(path + '.offsets')
    loc.offsets = {}
    for key, n in offs.items():
        n = integer(n)
        if key not in ctx.builds or n is None or n < 0:
            raise _invalid(path + '.offsets')
        loc.offsets[key] = n
    loc.kind = base + '-builds'
    return loc


def _patch(t, c, ctx):
    at = location(t.get('at'), 'patch.at', ctx)
    expect = parse_bytes(t.get('expect'))
    if expect is None:
        raise _invalid('patch.expect')
    if ('bytes' in t) == ('encode' in t):
        raise _invalid('patch.bytes')
    p = Patch(at, expect)
    if 'bytes' in t:
        p.bytes = parse_bytes(t['bytes'])
        if p.bytes is None:
            raise _invalid('patch.bytes')
        if len(p.bytes) != len(expect):
            raise _invalid('patch.expect')
    else:
        e = t['encode']
        if not isinstance(e, dict):
            raise _invalid('patch.encode')
        insn, width, rd, value = e.get('insn'), e.get('width'), e.get('rd'), e.get('value')
        if not isinstance(insn, str) or insn not in enc.ARM64_IMM_INSNS:
            raise _invalid('patch.encode.insn')
        if not is_int(width) or width not in (32, 64):
            raise _invalid('patch.encode.width')
        if not is_int(rd) or not 0 <= rd <= 31:
            raise _invalid('patch.encode.rd')
        ref = _ref(value, c, 'patch.encode.value', NUMBER_TYPES)
        if ref is None:
            if not is_number(value) or enc.arm64_imm(insn, width, rd, value) is None:
                raise _invalid('patch.encode.value')
        p.encode = {'insn': insn, 'width': width, 'rd': rd, 'value': ref if ref is not None else value}
        if len(expect) != enc.ARM64_IMM_INSNS[insn]:
            raise _invalid('patch.expect')
    if 'asm' in t:
        a = t['asm']
        if not isinstance(a, dict) or not all(isinstance(a.get(k, ''), str) for k in ('expect', 'bytes')):
            raise _invalid('patch.asm')
        p.asm = dict(a)
    return p


def _write(t, c, ctx):
    at = location(t.get('at'), 'write.at', ctx)
    typ = t.get('type')
    if not isinstance(typ, str) or typ not in WRITE_TYPES:
        raise _invalid('write.type')
    if 'value' not in t:
        raise _invalid('write.value')
    v = t['value']
    if typ == 'bytes':
        value = parse_bytes(v)
        if value is None:
            raise _invalid('write.value')
        expect = None
        if 'expect' in t:
            expect = parse_bytes(t['expect'])
            if expect is None or len(expect) != len(value):
                raise _invalid('write.expect')
        return Write(at, typ, value, expect)
    ref = _ref(v, c, 'write.value', VALUE_TYPES)
    if ref is not None and typ in INT_TYPES and c.values[ref.name].type in FLOAT_TYPES:
        raise _invalid('write.value')
    value = ref if ref is not None else _value_number(typ, v)
    if value is None:
        raise _invalid('write.value')
    expect = None
    if 'expect' in t:
        expect = _value_number(typ, t['expect'])
        if expect is None:
            raise _invalid('write.expect')
    return Write(at, typ, value, expect)


def _override(t, c, ctx):
    fn = location(t.get('function'), 'override.function', ctx)
    action = t.get('action')
    if not isinstance(action, str) or action not in ACTIONS:
        raise _invalid('override.action')
    o = Override(fn, action)
    if action == 'return':
        if 'value' not in t:
            raise _invalid('override.value')
        ref = _ref(t['value'], c, 'override.value', tuple(INT_TYPES) + ('bool',))
        n = integer(t['value']) if ref is None else None
        if ref is None and (n is None or not -(1 << 63) <= n < (1 << 64)):
            raise _invalid('override.value')
        o.value = ref if ref is not None else n
    elif 'value' in t:
        raise _invalid('override.value')
    if 'expect' in t:
        o.expect = parse_bytes(t['expect'])
        if o.expect is None:
            raise _invalid('override.expect')
    return o


def _read(t, ctx):
    at = location(t.get('at'), 'read.at', ctx)
    typ = t.get('type')
    if not isinstance(typ, str) or typ not in NUMBER_TYPES:
        raise _invalid('read.type')
    r = Read(at, typ)
    if 'label' in t:
        if not isinstance(t['label'], str):
            raise _invalid('read.label')
        r.label = t['label']
    return r


def _condition(t, c, ctx):
    at = location(t.get('at'), 'when.at', ctx)
    typ = t.get('type')
    if not isinstance(typ, str) or typ not in NUMBER_TYPES:
        raise _invalid('when.type')
    op = t.get('op')
    if not isinstance(op, str) or op not in OPS:
        raise _invalid('when.op')
    if op in ('&', '!&') and typ not in INT_TYPES:
        raise _invalid('when.type')
    cond = Condition(at, typ, op)
    if op == 'changed':
        if 'value' in t:
            raise _invalid('when.value')
        return cond
    if 'value' not in t:
        raise _invalid('when.value')
    v = t['value']
    if op == 'between':
        if not isinstance(v, list) or len(v) != 2:
            raise _invalid('when.value')
        low, high = (_value_number(typ, x) for x in v)
        if low is None or high is None or low > high:
            raise _invalid('when.value')
        cond.value = (low, high)
        return cond
    if isinstance(v, dict):
        other_type = v.get('type')
        if 'at' not in v:
            raise _invalid('when.value.at')
        place_at = location(v['at'], 'when.value.at', ctx)
        if not isinstance(other_type, str) or other_type not in NUMBER_TYPES:
            raise _invalid('when.value.type')
        cond.value = Place(place_at, other_type)
        return cond
    ref = _ref(v, c, 'when.value', VALUE_TYPES)
    cond.value = ref if ref is not None else _value_number(typ, v)
    if cond.value is None:
        raise _invalid('when.value')
    return cond


def _raw(t, ctx):
    syntax, build, code = t.get('syntax'), t.get('build'), t.get('code')
    if not isinstance(syntax, str) or not syntax:
        raise _invalid('raw.syntax')
    if not isinstance(build, str) or build not in ctx.builds:
        raise _invalid('raw.build')
    if not isinstance(code, str) or not code.strip():
        raise _invalid('raw.code')
    if syntax == 'atmosphere':
        tokens = code.split()
        if any(len(tok) != 8 or not all(ch in '0123456789abcdefABCDEF' for ch in tok) for tok in tokens):
            raise _invalid('raw.code')
    return Raw(syntax, build, code)


__all__ = ['build_file', 'check_cheat', 'location', 'Problem', 'KNOWN_EXTENSIONS', 'BYTES', 'RAW_SYNTAXES']
