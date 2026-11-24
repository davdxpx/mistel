"""Patch files of the Switch's code modules (section 15): IPS, IPS32 and IPSwitch text (`.pchtxt`), as Atmosphère's
`exefs_patches` and the emulators load them."""
import re
from dataclasses import dataclass, field

NSO_HEADER = 0x100          # what a patch file's offsets count before the module's image: the NSO file's header
_HEX = re.compile(r'^[0-9A-Fa-f]+\Z', re.A)
_ESCAPES = {'n': '\n', 't': '\t', 'r': '\r', '0': '\0', '\\': '\\', '"': '"', "'": "'"}


class PatchFileError(ValueError):
    """A file that is not a patch file of one of these formats. `code` is `patch`."""
    code = 'patch'


@dataclass
class Patch:
    """One patch of a file: the records it writes, in file order. Each record is (offset, bytes), the offset counted
    from the start of the module's NSO file, its 0x100-byte header first and then the image."""
    name: str               # the comment that names it (pchtxt); empty for an IPS file
    enabled: bool           # @enabled or @disabled (pchtxt); an IPS file is always applied
    records: list = field(default_factory=list)


@dataclass
class PatchFile:
    format: str             # ips, ips32 or pchtxt
    patches: list
    build_id: str = None    # the build ID the file names itself (@nsobid-), in upper-case hexadecimal
    notes: list = field(default_factory=list)       # (line, text): forced comments (#) and lines that are no patch
    comments: list = field(default_factory=list)    # (line, text): the other comments, for credits


def read(data):
    """The patch file in these bytes: IPS or IPS32 by their magic, else IPSwitch text."""
    data = bytes(data)
    if data[:5] in (b'PATCH', b'IPS32'):
        return read_ips(data)
    if data[:3] == b'\xef\xbb\xbf':
        data = data[3:]
    try:
        text = data.decode('utf-8')
    except UnicodeDecodeError:
        raise PatchFileError('neither an IPS file nor IPSwitch text in UTF-8') from None
    return read_pchtxt(text)


def read_ips(data):
    """An IPS or IPS32 file: `PATCH` or `IPS32`, then records up to `EOF` or `EEOF`. A record is an offset (3 or 4
    bytes, big-endian), a size (2 bytes) and that many bytes; a size of 0 is a run: a count (2 bytes) and the byte it
    repeats. Whatever follows the end marker is ignored."""
    data = bytes(data)
    if data[:5] == b'IPS32':
        fmt, width, end = 'ips32', 4, b'EEOF'
    elif data[:5] == b'PATCH':
        fmt, width, end = 'ips', 3, b'EOF'
    else:
        raise PatchFileError('not an IPS file: it starts with neither PATCH nor IPS32')
    records, i = [], 5
    while True:
        if data[i:i + len(end)] == end:
            return PatchFile(fmt, [Patch('', True, records)])
        if i + width + 2 > len(data):
            raise PatchFileError(f'the file ends at 0x{i:X} without {end.decode()}')
        offset = int.from_bytes(data[i:i + width], 'big')
        size = int.from_bytes(data[i + width:i + width + 2], 'big')
        i += width + 2
        if size == 0:
            if i + 3 > len(data):
                raise PatchFileError(f'a run at 0x{i:X} is cut off')
            count, value = int.from_bytes(data[i:i + 2], 'big'), data[i + 2]
            i += 3
            if count:
                records.append((offset, bytes([value]) * count))
        else:
            if i + size > len(data):
                raise PatchFileError(f'a record at 0x{i:X} claims {size} bytes past the end of the file')
            records.append((offset, data[i:i + size]))
            i += size


def _string_end(text, start):
    """The index of the double quote that closes the string opening at `start`, or -1."""
    k = start + 1
    while k < len(text):
        if text[k] == '\\':
            k += 2
        elif text[k] == '"':
            return k
        else:
            k += 1
    return -1


def _uncomment(line):
    """The line without a `//` comment that is not inside a string, and the comment (None without one)."""
    k = 0
    while k < len(line):
        if line[k] == '"':
            end = _string_end(line, k)
            if end < 0:
                return line, None
            k = end + 1
        elif line.startswith('//', k):
            return line[:k], line[k + 2:].strip()
        else:
            k += 1
    return line, None


def _string(text):
    out, k = [], 0
    while k < len(text):
        ch = text[k]
        if ch == '\\' and k + 1 < len(text) and text[k + 1] in _ESCAPES:
            out.append(_ESCAPES[text[k + 1]])
            k += 2
        else:
            out.append(ch)
            k += 1
    return ''.join(out).encode('utf-8')


def _value(text, little):
    """The bytes of a patch line's value, or None: a string in double quotes, or hexadecimal digits in pairs, which
    `@little-endian` reverses."""
    text = text.strip()
    if text.startswith('"'):
        end = _string_end(text, 0)
        return _string(text[1:end]) if end > 0 and not text[end + 1:].strip() else None
    digits = ''.join(text.split())
    if not digits or len(digits) % 2 or not _HEX.match(digits):
        return None
    raw = bytes.fromhex(digits)
    return raw[::-1] if little else raw


def read_pchtxt(text):
    """IPSwitch text: directives, comments, and patches that `@enabled` or `@disabled` start and the next of them or
    `@stop` ends. The last comment before a patch names it. A patch line is an offset of 8 hexadecimal digits, a
    space and a value; `@flag offset_shift` adds to every offset after it."""
    patches, notes, comments = [], [], []
    build_id, shift, little, last, current = None, 0, False, '', None
    for n, raw_line in enumerate(text.replace('\r\n', '\n').split('\n'), 1):
        line, comment = _uncomment(raw_line)
        stripped = line.strip()
        if comment:
            comments.append((n, comment))
            if not stripped:
                last = comment        # a comment on a line of its own names the next patch; one after code does not
        if not stripped:
            continue
        lower = stripped.lower()
        if lower.startswith('@stop'):
            break
        if lower.startswith('@nsobid-'):
            bid = stripped[8:].strip()
            if not _HEX.match(bid) or len(bid) > 64:
                raise PatchFileError(f'line {n}: @nsobid- needs a build ID in hexadecimal')
            build_id = trim_build_id(bid)
        elif lower.startswith('@flag'):
            words = stripped.split()
            if len(words) >= 3 and words[1].lower() == 'offset_shift':
                try:
                    shift = int(words[2], 0)
                except ValueError:
                    raise PatchFileError(f'line {n}: offset_shift needs a number') from None
        elif lower.startswith('@little-endian'):
            little = True
        elif lower.startswith('@big-endian'):
            little = False
        elif lower.startswith('@enabled') or lower.startswith('@disabled'):
            current = Patch(last, lower.startswith('@enabled'))
            patches.append(current)
        elif stripped.startswith('#'):
            notes.append((n, stripped[1:].strip()))
        else:
            parts = stripped.split(None, 1)
            value = _value(parts[1], little) if len(parts) == 2 else None
            if current is None or len(parts[0]) != 8 or not _HEX.match(parts[0]) or value is None:
                notes.append((n, raw_line.strip()))
                continue
            current.records.append((int(parts[0], 16) + shift, value))
    return PatchFile('pchtxt', patches, build_id, notes, comments)


def build_id_of(file_name):
    """The build ID that a patch file's name gives, `<build ID>.ips`: an even number of hexadecimal digits, 16 to 64.
    The zero bytes at its end are left out, down to 16 digits. None when the name is no build ID."""
    stem = re.sub(r'\.(ips|ips32|pchtxt)\Z', '', file_name.replace('\\', '/').rsplit('/', 1)[-1], flags=re.I)
    if not 16 <= len(stem) <= 64 or len(stem) % 2 or not _HEX.match(stem):
        return None
    return trim_build_id(stem)


def trim_build_id(text):
    """A build ID in upper case, without the zero bytes at its end, down to 16 digits."""
    text = text.upper()
    while len(text) > 16 and text.endswith('00'):
        text = text[:-2]
    return text


def _hex(n):
    return f'-0x{-n:X}' if n < 0 else f'0x{n:X}'


def _bytes(n):
    return f'{n} byte' if n == 1 else f'{n} bytes'


def image_runs(patch, size):
    """The bytes a patch writes into a module's image of `size` bytes, as (offset, bytes) runs of consecutive bytes in
    order of offset, and what it left out. A later record wins over an earlier one at the same byte; the part of a
    record inside the NSO header or past the image is left out, as loaders do."""
    written, dropped = {}, []
    for offset, data in patch.records:
        start = offset - NSO_HEADER
        for k, b in enumerate(data):
            at = start + k
            if 0 <= at < size:
                written[at] = b
        if start < 0:
            dropped.append(f'{_hex(offset)}: {_bytes(min(len(data), -start))} in the NSO header')
        if start + len(data) > size:
            dropped.append(f'{_hex(offset)}: {_bytes(min(len(data), start + len(data) - max(start, size)))} past the '
                           'module')
    runs = []
    for at in sorted(written):
        if runs and runs[-1][0] + len(runs[-1][1]) == at:
            runs[-1][1].append(written[at])
        else:
            runs.append((at, bytearray([written[at]])))
    return [(at, bytes(b)) for at, b in runs], dropped
