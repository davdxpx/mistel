"""The shortest byte pattern that finds one place in a module exactly once (spec, section 6.3).

The bytes that change when the code moves in another build (branch and page offsets, see arm64.moving_bytes) become
`??`, so that a pattern keeps finding its place after an update as long as the code around it stays the same.
"""
from dataclasses import dataclass

from . import arm64
from .evaluate import Module, find_all

STEP = 4


@dataclass
class Pattern:
    items: list          # byte values, None for `??`
    start: int           # the module offset where the pattern matches
    place: int           # the place's offset from the match: the location's `offset`

    @property
    def text(self):
        return ' '.join('??' if b is None else f'{b:02X}' for b in self.items)

    def location(self, module='main'):
        loc = {'module': module, 'find': self.text}
        if self.place:
            loc['offset'] = self.place
        return loc


def search_ranges(module):
    """The default search: code and read-only data, cut where the image ends."""
    size = len(module.image)
    return [(min(s, size), min(e, size)) for s, e in (module.sections[n] for n in ('code', 'rodata')
                                                       if n in module.sections)]


def _items(module, start, end, moving):
    """The window's bytes, None where they move between builds or are not the module's own."""
    return [None if i in moving or module.excluded(i, 1) else module.image[i] for i in range(start, end)]


def _anchors(items, base, site):
    """The words of fixed bytes in the window, aligned as the place is, nearest the place first:
    (absolute offset, bytes)."""
    first = (site - base) % STEP
    words = []
    for i in range(first, len(items) - STEP + 1, STEP):
        piece = items[i:i + STEP]
        if None not in piece:
            words.append((abs(base + i - site), base + i, bytes(piece)))
    return [(start, piece) for _, start, piece in sorted(words)]



def _occurrences(image, needle, ranges, limit):
    found = []
    for start, end in ranges:
        at = image.find(needle, start, end)
        while at >= 0:
            found.append(at)
            if len(found) > limit:
                return None
            at = image.find(needle, at + 1, end)
    return found


def make_pattern(module, offset, cover=4, longest=64, limit=200000):
    """The shortest pattern that finds `offset` exactly once and covers the first `cover` bytes of the place, or None
    when the place is not in code or read-only data, or no pattern up to `longest` bytes is unique."""
    section = module.section_of(offset)
    if section not in ('code', 'rodata'):
        return None
    sec_start, sec_end = module.sections[section]
    sec_end = min(sec_end, len(module.image))
    if section == 'code' and module.text_size is not None:
        if offset >= module.text_size:
            return None                    # the zero bytes after the code: nothing there to find
        sec_end = min(sec_end, module.text_size)
    lo, hi = max(sec_start, offset - longest), min(sec_end, offset + longest)

    moving = arm64.moving_bytes(module.image, lo, hi)
    window = _items(module, lo, hi, moving)
    cover = max(1, min(cover, hi - offset))
    ranges = search_ranges(module)
    best = None
    for a0, needle in _anchors(window, lo, offset)[:3]:
        hits = _occurrences(module.image, needle, ranges, limit)
        if hits is None:
            continue
        for order in ('forward', 'backward', 'both'):
            found = _grow(module, offset, cover, lo, hi, window, a0, hits, ranges, order, longest)
            if found and (best is None or (len(found.items), abs(found.place)) < (len(best.items), abs(best.place))):
                best = found

    if best is None:
        return None
    m = Module(None, 0, module.image, len(module.image), module.sections)
    if find_all(m, best.items, None, 2) != [best.start]:
        return None
    return best


def _grow(module, offset, cover, lo, hi, window, a0, hits, ranges, order, longest):
    start = min(offset, a0)
    if offset % STEP == 0:
        start -= start % STEP
    end = max(offset + cover, a0 + 1)
    end = min(hi, start + ((end - start + STEP - 1) // STEP) * STEP)
    image = module.image
    alive = None
    toggle = 0
    while True:
        items = window[start - lo:end - lo]
        if items and items[0] is not None and items[-1] is not None and \
                start <= a0 and a0 < end and end - start >= 4:
            shift = a0 - start
            if alive is None:
                alive = [h - shift for h in hits]
            alive = [q for q in alive if _matches(image, q, items, ranges)]
            if alive == [start]:
                return Pattern(items, start, offset - start)
        grow_back = start - STEP >= lo
        grow_ahead = end + STEP <= hi
        if end - start >= longest or not (grow_back or grow_ahead):
            return None
        if order == 'forward':
            ahead = grow_ahead
        elif order == 'backward':
            ahead = not grow_back
        else:
            ahead = grow_ahead and (toggle % 2 == 0 or not grow_back)
            toggle += 1
        if ahead:
            end += STEP
        else:
            start -= STEP
            if alive is not None:
                alive = [q - STEP for q in alive]


def _matches(image, q, items, ranges):
    n = len(items)
    if not any(s <= q and q + n <= e for s, e in ranges):
        return False
    for i, b in enumerate(items):
        if b is not None and image[q + i] != b:
            return False
    return True
