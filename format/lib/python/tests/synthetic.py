"""Made-up code modules and cheat files for the tests: an LZ4 block compressor, NSO files, a small module with known
places, and an Atmosphère cheat file for it."""
import pathlib
import struct
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1]))

BUILD_ID = '0123456789ABCDEF'
GAME_ID = '0100000000ABC000'


def lz4_compress(data):
    """A valid LZ4 block with matches, greedy over a table of 4-byte prefixes. The last 12 bytes stay literals, as
    the block format asks."""
    out = bytearray()
    table = {}
    i = anchor = 0
    end = len(data)
    limit = end - 12

    def lengths(n):
        while n >= 255:
            out.append(255)
            n -= 255
        out.append(n)

    while i < limit:
        key = data[i:i + 4]
        cand = table.get(key)
        table[key] = i
        if cand is None or i - cand > 0xFFFF:
            i += 1
            continue
        m = 4
        while i + m < limit and data[cand + m] == data[i + m]:
            m += 1
        lit = i - anchor
        token_lit = min(lit, 15)
        token_match = min(m - 4, 15)
        out.append(token_lit << 4 | token_match)
        if lit >= 15:
            lengths(lit - 15)
        out += data[anchor:i]
        out += struct.pack('<H', i - cand)
        if m - 4 >= 15:
            lengths(m - 4 - 15)
        i += m
        anchor = i
    lit = end - anchor
    out.append(min(lit, 15) << 4)
    if lit >= 15:
        lengths(lit - 15)
    out += data[anchor:]
    return bytes(out)


def make_nso(text, ro, data, bss=0, build_id=BUILD_ID, compress=True):
    """An NSO file whose segments lie at page-aligned memory offsets one after another."""
    def page(n):
        return (n + 0xFFF) & ~0xFFF
    mems = [0, page(len(text)), page(len(text)) + page(len(ro))]
    segs = [text, ro, data]
    stored = [lz4_compress(s) if compress else s for s in segs]
    header = bytearray(0x100)
    header[0:4] = b'NSO0'
    struct.pack_into('<I', header, 0x0C, 0b111 if compress else 0)
    pos = 0x100
    for i, (mem, seg, st) in enumerate(zip(mems, segs, stored)):
        struct.pack_into('<III', header, 0x10 + 0x10 * i, pos, mem, len(seg))
        struct.pack_into('<I', header, 0x60 + 4 * i, len(st))
        pos += len(st)
    struct.pack_into('<I', header, 0x3C, bss)
    header[0x40:0x60] = bytes.fromhex(build_id).ljust(0x20, b'\0')
    return bytes(header) + b''.join(stored)


def words(*ws):
    return b''.join(w.to_bytes(4, 'little') for w in ws)


FILLER = [0xD503201F, 0xA9BF7BFD, 0x910003FD, 0xD65F03C0, 0xAA1303E0, 0xF9400260, 0x52800000, 0x2A1F03E0]

# Places in the made-up module.
FUNCTION = 0x1000          # stp; mov; str w8, [x19, #0x10]; bl; adrp; ldr; ldp; ret
STR_W8 = 0x1008
BL = 0x100C
TEXT_SIZE = 0x1FE0         # the code ends here; up to 0x2000 the page is zero: a cave
DATA = 0x3000


def sample_code():
    import random
    rng = random.Random(7)
    text = bytearray(words(*(rng.choice(FILLER) for _ in range(TEXT_SIZE // 4))))
    text[FUNCTION:FUNCTION + 32] = words(0xA9BF7BFD, 0x910003FD, 0xB9001268, 0x94000040, 0x90000008, 0xB9402100,
                                         0xA8C17BFD, 0xD65F03C0)
    text[0x1800:0x1808] = words(0x1E2E1000, 0x1E202008)       # fmov s0, #1.0; fcmp s0, s0
    ro = bytearray(0x800)
    ro[0x100:0x108] = b'Example\0'
    data = bytearray(0x400)
    data[0x20:0x28] = (0x10000100).to_bytes(8, 'little')
    return bytes(text), bytes(ro), bytes(data)


def sample_nso(compress=True):
    text, ro, data = sample_code()
    return make_nso(text, ro, data, bss=0x400, compress=compress)


CHEATS = '''[--- Section A ---]
20000000

[Cheats by someone, www.example.org]

{Master setup}
04000000 00001FF0 D503201F

[Money]
04000000 00001008 B9001269

[Money off]
04000000 00001008 B9001268

[Branch away]
04000000 0000100C 14000100

[--- Data ---]

[Data flag]
04000000 00003010 00000001

[Heap value]
02100000 00000040 0000FFFF

[Pointer]
580F0000 00003020
580F1000 00000010
780F0000 00000008
640F0000 00000000 00000063

[Hold A]
80000001
04000000 0000100C D503201F
21000000
04000000 0000100C 94000040
20000000

[Condition]
14150000 00000100 00000005
02100000 00000104 00000001
20000000

[Items]
20000000

[Cave jump]
80000002
080A0000 00001FE8 D503201F 52800020
040A0000 00001010 140003F6
21000000
040A0000 00001010 90000008
20000000

[Items]
20000001

[Speed]
C4000000 00000000 00000100
040A0000 00001800 1E201000
20000000

[Loop]
300F0000 00000004
04000000 00003000 00000000
310F0000
'''


SHIFT = 0x40


def shifted_code():
    """The same code as sample_code, SHIFT bytes later: a later build of the made-up game."""
    text, ro, data = sample_code()
    return words(*(FILLER * 2)[:SHIFT // 4]) + text[:TEXT_SIZE - SHIFT], ro, data


def shifted_cheats():
    """CHEATS for the shifted build: every place in the code before the cave moves by SHIFT."""
    out = []
    for line in CHEATS.splitlines():
        parts = line.split()
        if len(parts) >= 3 and parts[0] in ('04000000', '040A0000') and FUNCTION <= int(parts[1], 16) < TEXT_SIZE:
            parts[1] = f'{int(parts[1], 16) + SHIFT:08X}'
            line = ' '.join(parts)
        out.append(line)
    return '\n'.join(out) + '\n'
