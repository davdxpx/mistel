"""What the tools need to know about ARM64 instructions: which ones are relative to their own address, and which of
their bytes change when the code moves in another build of the game.
"""


def word(data, offset):
    return int.from_bytes(data[offset:offset + 4], 'little')


def _signed(value, bits):
    return value - (1 << bits) if value & (1 << (bits - 1)) else value


def pc_relative_target(w, pc):
    """The address a PC-relative instruction at pc refers to, or None for an instruction that is not PC-relative."""
    if (w & 0x7C000000) == 0x14000000:                      # B, BL
        return pc + 4 * _signed(w & 0x3FFFFFF, 26)
    if (w & 0xFF000010) == 0x54000000 or (w & 0x7E000000) == 0x34000000:   # B.cond, CBZ, CBNZ
        return pc + 4 * _signed((w >> 5) & 0x7FFFF, 19)
    if (w & 0x7E000000) == 0x36000000:                      # TBZ, TBNZ
        return pc + 4 * _signed((w >> 5) & 0x3FFF, 14)
    if (w & 0x3B000000) == 0x18000000 or (w & 0xFF000000) == 0xD8000000:   # LDR (literal), PRFM (literal)
        return pc + 4 * _signed((w >> 5) & 0x7FFFF, 19)
    if (w & 0x1F000000) == 0x10000000:                      # ADR, ADRP
        imm = _signed(((w >> 5) & 0x7FFFF) << 2 | (w >> 29) & 3, 21)
        if w & 0x80000000:
            return (pc & ~0xFFF) + (imm << 12)
        return pc + imm
    return None


def is_adrp(w):
    return (w & 0x9F000000) == 0x90000000


def _low12_base(w):
    """The base register of an ADD (immediate) or a load or store with an unsigned 12-bit offset, or None."""
    if (w & 0x7F800000) == 0x11000000 or (w & 0x3B000000) == 0x39000000:
        return (w >> 5) & 31
    return None


def moving_bytes(data, start, end, lookback=32):
    """The offsets in [start, end) whose bytes change when the code moves: the address fields of PC-relative
    instructions and the low 12 bits of the page offset that follow an ADRP. Code is read in words aligned to 4."""
    moving = set()
    first = max(0, (start - lookback) & ~3)
    pages = {}                                              # register -> set by an ADRP
    for off in range(first, (end + 3) & ~3, 4):
        if off + 4 > len(data):
            break
        w = word(data, off)
        fields = ()
        if is_adrp(w) or (w & 0x9F000000) == 0x10000000:
            fields = (0, 1, 2, 3)
        elif pc_relative_target(w, off) is not None:
            fields = (0, 1, 2)
        else:
            base = _low12_base(w)
            if base is not None and base in pages:
                fields = (1, 2)
                if w & 31 == base:                          # add x8, x8, #lo or ldr x8, [x8, #lo]: x8 changed
                    pages.pop(base)
        if is_adrp(w):
            pages[w & 31] = off
        for i in fields:
            if start <= off + i < end:
                moving.add(off + i)
    return moving


def relative_outside(data, offset, length):
    """Whether bytes written at offset hold a PC-relative instruction whose target lies outside them, so that the
    same bytes mean something else at another place."""
    if offset % 4:
        return False
    for i in range(0, length - length % 4, 4):
        target = pc_relative_target(word(data, i), offset + i)
        if target is not None and not offset <= target < offset + length:
            return True
    return False
