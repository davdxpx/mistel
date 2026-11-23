"""Writes `.mncf` text: any document as `load` reads it (a dict of TOML values), in one fixed layout.

The layout follows the spec's examples: the file's own keys, then `[game]`, the builds, the choice lists, every cheat
with its values and operations as indented sub-tables, the presets and the translations. Keys come in the order the
spec lists them and keys it does not define after them, in their own order. Locations, offsets, addresses and path
steps are written in hexadecimal, other whole numbers in decimal up to 0xFFFF and in hexadecimal above. Writing a
file that was read gives the same file again, without its signature lines.
"""
import math
import re

TOP = ('format', 'name', 'authors', 'about', 'file_version', 'language', 'origin', 'extensions')
TABLES = ('game', 'builds', 'choices', 'cheat', 'preset', 'i18n')
GAME = ('platform', 'id', 'name')
BUILD = ('version', 'modules')
CHEAT = ('id', 'name', 'about', 'group', 'tags', 'default', 'master', 'run', 'duration', 'hold', 'toggle', 'press',
         'requires', 'excludes', 'radio', 'match', 'trigger', 'interval')
VALUE = ('type', 'min', 'max', 'step', 'default', 'unit', 'display', 'choices', 'flags')
OPS = {'patch': ('at', 'expect', 'bytes', 'encode', 'asm'), 'write': ('at', 'type', 'value', 'expect'),
       'override': ('function', 'action', 'value', 'expect'), 'read': ('at', 'type', 'label'),
       'when': ('at', 'type', 'op', 'value'), 'raw': ('syntax', 'build', 'code')}
INLINE = {'at': ('module', 'region', 'address', 'find', 'offset', 'offsets', 'section', 'path'),
          'function': ('module', 'region', 'address', 'find', 'offset', 'offsets', 'section', 'path'),
          'value': ('at', 'type'), 'encode': ('insn', 'width', 'rd', 'value'), 'asm': ('expect', 'bytes'),
          'origin': ('format', 'name', 'site'), 'extensions': ('required', 'optional'), 'modules': ()}
PRESET = ('id', 'name', 'cheats', 'values')
HEX_KEYS = {'offset', 'address', 'path', 'offsets'}
MULTILINE = {'code', 'about'}
_BARE = re.compile(r'^[A-Za-z0-9_-]+\Z')
_INDENT = '  '


def key(k):
    return k if _BARE.match(k) else string(k)


def string(s):
    out = ['"']
    for ch in s:
        if ch in '"\\':
            out.append('\\' + ch)
        elif ch == '\n':
            out.append('\\n')
        elif ch == '\t':
            out.append('\\t')
        elif ord(ch) < 0x20 or ord(ch) == 0x7F:
            out.append(f'\\u{ord(ch):04X}')
        else:
            out.append(ch)
    out.append('"')
    return ''.join(out)


def multiline(s):
    body = s.replace('\\', '\\\\').replace('"""', '""\\"')
    if any(ord(ch) < 0x20 and ch not in '\n\t' for ch in body):
        return string(s)
    return '"""\n' + body + '"""' if s.endswith('\n') else '"""\n' + body + '\\\n"""'


def integer(n, hexa):
    if n > 0x7FFFFFFFFFFFFFFF:
        return f'"0x{n:X}"'
    if n < 0 or (not hexa and n <= 0xFFFF):
        return str(n)
    return f'0x{n:X}'


def number(v):
    if math.isnan(v):
        return 'nan'
    if math.isinf(v):
        return 'inf' if v > 0 else '-inf'
    text = repr(v)
    return text if any(ch in text for ch in '.en') else text + '.0'


def scalar(v, k=None, hexa=False):
    """A value: inline tables and arrays included."""
    if isinstance(v, bool):
        return 'true' if v else 'false'
    if isinstance(v, int):
        return integer(v, hexa or k in HEX_KEYS)
    if isinstance(v, float):
        return number(v)
    if isinstance(v, str):
        return string(v)
    if isinstance(v, list):
        return '[' + ', '.join(scalar(x, k, hexa or k in HEX_KEYS) for x in v) + ']'
    if isinstance(v, dict):
        order = INLINE.get(k, ())
        keys = [x for x in order if x in v] + [x for x in v if x not in order]
        inner_hex = k in HEX_KEYS
        parts = [f'{key(x)} = {scalar(v[x], x, inner_hex)}' for x in keys]
        return '{ ' + ', '.join(parts) + ' }' if parts else '{}'
    raise TypeError(f'no TOML form for {type(v).__name__}')


def _value_line(k, v, indent=''):
    if k in MULTILINE and isinstance(v, str) and '\n' in v:
        return f'{indent}{key(k)} = {multiline(v)}'
    return f'{indent}{key(k)} = {scalar(v, k)}'


def _ordered(table, order):
    return [k for k in order if k in table] + [k for k in table if k not in order]


def _is_table_array(v):
    return isinstance(v, list) and v and all(isinstance(x, dict) for x in v)


def _section(out, path, table, array=False):
    """A table the layout does not know, under its header: its values, then its tables and arrays of tables."""
    values = [k for k, v in table.items() if not isinstance(v, dict) and not _is_table_array(v)]
    subs = [k for k in table if k not in values]
    if values or not subs or array:
        out += ['', f'[[{path}]]' if array else f'[{path}]']
        out += [_value_line(k, table[k]) for k in values]
    for k in subs:
        v = table[k]
        if isinstance(v, dict):
            _section(out, f'{path}.{key(k)}', v)
        else:
            for item in v:
                _section(out, f'{path}.{key(k)}', item, array=True)


def dumps(doc):
    """The text of a document."""
    out = []
    for k in _ordered(doc, TOP):
        v = doc[k]
        if k in TABLES and not _shaped(k, v) or \
                k not in TABLES and not (isinstance(v, dict) and k not in INLINE) and not _is_table_array(v):
            out.append(_value_line(k, v))
    shaped = {k: doc[k] for k in TABLES if k in doc and _shaped(k, doc[k])}
    game = shaped.get('game')
    if game is not None:
        out += ['', '[game]'] + [_value_line(k, game[k]) for k in _ordered(game, GAME)]
    for bkey, b in shaped.get('builds', {}).items():
        out += ['', f'[builds.{key(bkey)}]'] + [_value_line(k, b[k]) for k in _ordered(b, BUILD)]
    for name, table in shaped.get('choices', {}).items():
        out += ['', f'[choices.{key(name)}]'] + [_value_line(k, v) for k, v in table.items()]
    for c in shaped.get('cheat', []):
        _cheat(out, c)
    for p in shaped.get('preset', []):
        out += ['', '[[preset]]'] + [_value_line(k, p[k]) for k in _ordered(p, PRESET)]
    if 'i18n' in shaped:
        _section(out, 'i18n', shaped['i18n'])
    for k, v in doc.items():
        if k in TOP or k in TABLES:
            continue
        if isinstance(v, dict) and k not in INLINE:
            _section(out, key(k), v)
        elif _is_table_array(v):
            for item in v:
                _section(out, key(k), item, array=True)
    while out and out[0] == '':
        out.pop(0)
    return '\n'.join(out) + '\n'


def _shaped(k, v):
    """Whether a top-level table of the layout has the shape the layout writes; else it is written as a value."""
    if k in ('cheat', 'preset'):
        return _is_table_array(v)
    if k in ('builds', 'choices'):
        return isinstance(v, dict) and all(isinstance(x, dict) for x in v.values())
    return isinstance(v, dict)


def _cheat(out, c):
    out += ['', '[[cheat]]']
    subs = []
    for k in _ordered(c, CHEAT):
        v = c[k]
        if (k == 'values' and isinstance(v, dict) and all(isinstance(x, dict) for x in v.values()) and v) or \
                _is_table_array(v):
            subs.append(k)
        else:
            out.append(_value_line(k, v))
    for k in subs:
        v = c[k]
        if k == 'values' and not _is_table_array(v):
            for vid, spec in v.items():
                out += ['', f'{_INDENT}[cheat.values.{key(vid)}]']
                out += [_value_line(x, spec[x], _INDENT) for x in _ordered(spec, VALUE)]
            continue
        for item in v:
            out += ['', f'{_INDENT}[[cheat.{key(k)}]]']
            out += [_value_line(x, item[x], _INDENT) for x in _ordered(item, OPS.get(k, ()))]


def write_file(f):
    """The text of a loaded file, in this layout, without its signature lines."""
    return dumps(f.doc)
