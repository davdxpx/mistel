"""Machine code the reader makes itself: override stubs (section 7.5, Appendix A) and `arm64-imm/1` instructions."""
import math

U64_MASK = (1 << 64) - 1


def _le32(*words):
    return b''.join(w.to_bytes(4, 'little') for w in words)


def arm64_stub(action, value=None):
    """The ARM64 stub at a function's entry: `ret` for "skip", the value moved into x0 and then `ret` for "return"."""
    ret = 0xD65F03C0
    if action == 'skip':
        return _le32(ret)
    v = value & U64_MASK
    if v <= 0xFFFF:
        return _le32(0xD2800000 | v << 5, ret)                       # movz x0, #v
    inverted = ~v & U64_MASK
    if inverted <= 0xFFFF:
        return _le32(0x92800000 | inverted << 5, ret)                # movn x0, #~v
    words = [0xD2800000 | (v & 0xFFFF) << 5]                         # movz x0, #(bits 0-15)
    for hw in (1, 2, 3):
        chunk = (v >> (16 * hw)) & 0xFFFF
        if chunk:
            words.append(0xF2800000 | hw << 21 | chunk << 5)         # movk x0, #chunk, lsl #16*hw
    return _le32(*words, ret)


def x86_64_stub(action, value=None):
    """The x86-64 stub: `ret` for "skip", `mov eax/rax, value` and then `ret` for "return"."""
    if action == 'skip':
        return b'\xC3'
    v = value & U64_MASK
    if v <= 0xFFFFFFFF:
        return b'\xB8' + v.to_bytes(4, 'little') + b'\xC3'
    signed = v - (1 << 64)
    if -(1 << 31) <= signed < 0:
        return b'\x48\xC7\xC0' + (signed & 0xFFFFFFFF).to_bytes(4, 'little') + b'\xC3'
    return b'\x48\xB8' + v.to_bytes(8, 'little') + b'\xC3'


STUBS = {'arm64': arm64_stub, 'x86-64': x86_64_stub}


def stub(arch, action, value=None):
    """The stub bytes for this architecture, or None where Appendix A defines none."""
    make = STUBS.get(arch)
    return make(action, value) if make else None


def fmov_imm8(value):
    """The 8-bit immediate of `fmov` for this value, or None if `fmov` cannot encode it exactly."""
    if not isinstance(value, (int, float)) or isinstance(value, bool):
        return None
    v = float(value)
    if v == 0 or not math.isfinite(v):
        return None
    sign = 1 if v < 0 else 0
    mantissa, exponent = math.frexp(abs(v))          # abs(v) = mantissa * 2**exponent, 0.5 <= mantissa < 1
    e = exponent - 1                                  # abs(v) = (2 * mantissa) * 2**e, 1 <= 2 * mantissa < 2
    m16 = mantissa * 32 - 16                          # (2 * mantissa - 1) * 16
    if not (-3 <= e <= 4) or m16 != int(m16) or not 0 <= m16 <= 15:
        return None
    bcd = (e - 1) if e >= 1 else (0b100 | (e + 3))   # b = 0 for e in 1..4, b = 1 for e in -3..0
    return sign << 7 | bcd << 4 | int(m16)


def arm64_imm(insn, width, rd, value):
    """The instruction bytes of an `encode` table (`arm64-imm/1`), or None if the value does not fit."""
    sf = 1 if width == 64 else 0
    if insn == 'movz':
        if not isinstance(value, int) or isinstance(value, bool) or not 0 <= value <= 0xFFFF:
            return None
        return _le32(sf << 31 | 0x52800000 | value << 5 | rd)
    if insn == 'movz-movk':
        if not isinstance(value, int) or isinstance(value, bool) or not 0 <= value <= 0xFFFFFFFF:
            return None
        lo, hi = value & 0xFFFF, value >> 16
        return _le32(sf << 31 | 0x52800000 | lo << 5 | rd, sf << 31 | 0x72A00000 | hi << 5 | rd)
    if insn == 'fmov':
        imm8 = fmov_imm8(value)
        if imm8 is None:
            return None
        ftype = 1 if width == 64 else 0
        return _le32(0x1E201000 | ftype << 22 | imm8 << 13 | rd)
    return None


ARM64_IMM_INSNS = {'movz': 4, 'movz-movk': 8, 'fmov': 4}
