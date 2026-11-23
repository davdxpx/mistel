"""Scalars of the format: integers that may be written as strings, bytes strings, patterns and typed values."""
import math
import re
import struct

ID = re.compile(r'^[a-z][a-z0-9-]*\Z')
BYTES = re.compile(r'^[0-9A-Fa-f]{2}( [0-9A-Fa-f]{2})*\Z')
PATTERN = re.compile(r'^([0-9A-Fa-f]{2}|\?\?)( ([0-9A-Fa-f]{2}|\?\?))*\Z')
BIG_INT = re.compile(r'^0x[0-9A-Fa-f]{1,16}\Z')
EXTENSION = re.compile(r'^(x-)?[a-z0-9][a-z0-9-]*/[0-9]+\Z')
PLAYER_VALUE = re.compile(r'^\$([a-z][a-z0-9-]*)\Z')

U64 = 1 << 64

INT_TYPES = {
    'u8': (1, False), 'u16': (2, False), 'u32': (4, False), 'u64': (8, False),
    's8': (1, True), 's16': (2, True), 's32': (4, True), 's64': (8, True),
}
FLOAT_TYPES = {'f32': 4, 'f64': 8}
NUMBER_TYPES = tuple(INT_TYPES) + tuple(FLOAT_TYPES)
WRITE_TYPES = NUMBER_TYPES + ('bytes',)
VALUE_TYPES = NUMBER_TYPES + ('bool',)


def is_int(v):
    return type(v) is int


def is_number(v):
    return type(v) in (int, float)


def integer(v):
    """An integer of the file: a TOML integer, or a string such as "0xFFFFFFFF80000000". None if it is neither."""
    if is_int(v):
        return v
    if isinstance(v, str) and BIG_INT.match(v):
        return int(v, 16)
    return None


def parse_bytes(text):
    """A bytes string as bytes, or None if it is not one."""
    if not isinstance(text, str) or not BYTES.match(text):
        return None
    return bytes.fromhex(text)


def parse_pattern(text):
    """A pattern as a list of byte values and None for `??`, or None if it is not a valid pattern (section 6.3)."""
    if not isinstance(text, str) or not PATTERN.match(text):
        return None
    items = [None if t == '??' else int(t, 16) for t in text.split(' ')]
    if len(items) < 4 or items[0] is None or items[-1] is None:
        return None
    return items


def hex_bytes(data):
    """Bytes in the file's notation: upper-case pairs separated by single spaces."""
    return ' '.join(f'{b:02X}' for b in data)


def type_size(t):
    if t in INT_TYPES:
        return INT_TYPES[t][0]
    if t in FLOAT_TYPES:
        return FLOAT_TYPES[t]
    if t == 'bool':
        return 1
    return None


def int_range(t):
    size, signed = INT_TYPES[t]
    if signed:
        return -(1 << (8 * size - 1)), (1 << (8 * size - 1)) - 1
    return 0, (1 << (8 * size)) - 1


def fits(t, v):
    """Whether the number v can be stored as type t."""
    if t == 'bool':
        return v in (0, 1) and type(v) in (int, bool)
    if t in INT_TYPES:
        if not is_int(v):
            return False
        lo, hi = int_range(t)
        return lo <= v <= hi
    if t in FLOAT_TYPES:
        if type(v) not in (int, float):
            return False
        if t == 'f32' and type(v) is float and math.isfinite(v):
            try:
                struct.pack('<f', v)
            except OverflowError:
                return False
        return True
    return False


def encode(t, v, byte_order):
    """The bytes of the number v as type t, in the platform's byte order."""
    if t == 'bool':
        return bytes([1 if v else 0])
    if t in INT_TYPES:
        size, signed = INT_TYPES[t]
        return int(v).to_bytes(size, byte_order, signed=signed)
    fmt = ('<' if byte_order == 'little' else '>') + ('f' if t == 'f32' else 'd')
    return struct.pack(fmt, float(v))
