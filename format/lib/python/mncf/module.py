"""Code modules for the converter: a Switch NSO (with LZ4 segments or without), or a plain image of bytes."""
import hashlib
import re
import struct
from dataclasses import dataclass, field

from .evaluate import Module
from .lz4 import decompress

PAGE = 0x1000
_HEX = re.compile(r'^(0x)?[0-9A-Fa-f]{1,16}\Z')


def _align(n):
    return (n + PAGE - 1) & ~(PAGE - 1)


class ModuleError(ValueError):
    pass


@dataclass
class CodeModule:
    """A module's image as loaded, its sections as (start, end) offsets, and its identity.

    `exclude` lists (offset, length) runs whose bytes are not the module's own, such as code a runtime rewrote:
    the converter takes no original bytes and no pattern from them.
    """
    image: bytes
    sections: dict
    build_id: str = None       # the Switch build ID in hexadecimal
    text_size: int = None      # where the code itself ends, before the zero bytes up to the next page
    exclude: list = field(default_factory=list)

    @property
    def short_id(self):
        """The build ID as cheat files name it: its first 8 bytes, 16 upper-case hexadecimal digits."""
        return self.build_id[:16].upper() if self.build_id else None

    def section_of(self, offset):
        for name, (start, end) in self.sections.items():
            if start <= offset < end:
                return name
        return None

    def excluded(self, offset, length):
        return any(o < offset + length and offset < o + n for o, n in self.exclude)

    def reader_module(self, base=0):
        ident = self.short_id or 'sha256:' + hashlib.sha256(self.image).hexdigest()
        return Module(ident, base, self.image, len(self.image), self.sections)


def parse_nso(data):
    """The module an NSO file holds."""
    if len(data) < 0x100 or data[:4] != b'NSO0':
        raise ModuleError('not an NSO file')
    flags = struct.unpack_from('<I', data, 0x0C)[0]
    segments = []
    for i, header in enumerate((0x10, 0x20, 0x30)):
        file_off, mem_off, size = struct.unpack_from('<III', data, header)
        packed = struct.unpack_from('<I', data, 0x60 + 4 * i)[0]
        compressed = bool(flags & (1 << i))
        stored = packed if compressed else size
        if file_off + stored > len(data):
            raise ModuleError(f'segment {i} lies past the end of the file')
        raw = data[file_off:file_off + stored]
        segments.append((mem_off, decompress(raw, size) if compressed else bytes(raw)))
    bss = struct.unpack_from('<I', data, 0x3C)[0]
    (text_mem, text), (ro_mem, ro), (data_mem, dat) = segments
    if not text_mem <= ro_mem <= data_mem:
        raise ModuleError('the segments are not in the order code, read-only data, data')
    image = bytearray(data_mem + len(dat) + bss)
    for mem, seg in segments:
        image[mem:mem + len(seg)] = seg
    sections = {
        'code': (text_mem, min(_align(text_mem + len(text)), ro_mem)),
        'rodata': (ro_mem, min(_align(ro_mem + len(ro)), data_mem)),
        'data': (data_mem, len(image)),
    }
    return CodeModule(bytes(image), sections, data[0x40:0x60].hex().upper(), text_mem + len(text))


def module_from_bytes(data):
    """A module from the bytes of a file: an NSO, or else a plain image that is all code."""
    if data[:4] == b'NSO0':
        return parse_nso(data)
    return CodeModule(bytes(data), {'code': (0, len(data))})


def read_runs(text):
    """(offset, length) runs from lines of two hexadecimal numbers separated by a tab, such as a list of the places a
    runtime rewrote; lines starting with # are skipped."""
    runs = []
    for n, line in enumerate(text.splitlines(), 1):
        line = line.strip()
        if not line or line.startswith('#'):
            continue
        parts = line.split('\t')
        if len(parts) < 2 or not all(_HEX.match(p.strip()) for p in parts[:2]):
            raise ModuleError(f'line {n} is not an offset and a length')
        runs.append((int(parts[0].strip(), 16), int(parts[1].strip(), 16)))
    return runs
