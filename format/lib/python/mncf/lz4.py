"""LZ4 block decompression, as Switch code modules (NSO) use it for their segments.

The `lz4` package is used when it is installed; the plain Python decoder below gives the same result, more slowly.
"""


class LZ4Error(ValueError):
    pass


def decompress(src, size):
    """The `size` bytes that the LZ4 block `src` holds."""
    try:
        import lz4.block
    except ImportError:
        return _decompress(src, size)
    try:
        return lz4.block.decompress(src, uncompressed_size=size)
    except lz4.block.LZ4BlockError as e:
        raise LZ4Error(str(e)) from None


def _length(src, i, n):
    while True:
        if i >= len(src):
            raise LZ4Error('the block ends inside a length')
        b = src[i]
        i += 1
        n += b
        if b != 255:
            return i, n


def _decompress(src, size):
    dst = bytearray()
    i, end = 0, len(src)
    while i < end:
        token = src[i]
        i += 1
        lit = token >> 4
        if lit == 15:
            i, lit = _length(src, i, lit)
        if i + lit > end:
            raise LZ4Error('the block ends inside literals')
        dst += src[i:i + lit]
        i += lit
        if i >= end:
            break
        if i + 2 > end:
            raise LZ4Error('the block ends inside an offset')
        off = src[i] | src[i + 1] << 8
        i += 2
        match = token & 15
        if match == 15:
            i, match = _length(src, i, match)
        match += 4
        start = len(dst) - off
        if off == 0 or start < 0:
            raise LZ4Error('a match points before the start')
        if off >= match:
            dst += dst[start:start + match]
        else:
            chunk = bytes(dst[start:])
            reps, rest = divmod(match, off)
            dst += chunk * reps + chunk[:rest]
        if len(dst) > size:
            raise LZ4Error('the block is longer than its segment')
    if len(dst) != size:
        raise LZ4Error(f'the block holds {len(dst)} bytes, the segment {size}')
    return bytes(dst)
