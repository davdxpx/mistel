"""`mistel check`: a file against the spec, and with code modules its patterns, `expect` and `asm` against them."""
import pathlib

import mncf
from mncf import disasm
from mncf.model import Patch

WHOLE = (0, 1 << 64)


def check(path, modules=None, trust=None):
    """(whether the file passed, lines of text)."""
    lines, ok = [], True
    try:
        f = mncf.load(pathlib.Path(path).read_bytes(), trust)
    except mncf.MncfError as e:
        return False, [f'error {e.code}: {e.message}']
    for w in f.warnings:
        lines.append(f'warning {w}')
    lines.append(f'signature: {f.signature.result}' + (' by ' + ', '.join(f.signature.signers)
                                                       if f.signature.signers else ''))
    keys = list(f.builds)
    for i, a in enumerate(keys):
        for b in keys[i + 1:]:
            if _overlap(f, f.builds[a], f.builds[b]):
                lines.append(f'warning builds "{a}" and "{b}" can match the same modules; "{a}" wins')
    for c in f.cheats:
        if c.problem:
            ok = False
            lines.append(f'fail {c.id}: {c.problem}')
    if not modules:
        good = sum(1 for c in f.cheats if not c.problem)
        lines.append(f'{good} of {len(f.cheats)} cheats follow the spec')
        return ok, lines

    rt = mncf.Runtime(f.platform.name, modules={n: m.reader_module() for n, m in modules.items()},
                      regions={r: WHOLE for r in f.platform.regions})
    ev = mncf.evaluate(f, rt)
    lines.append(f'build: {ev.build or "none of the file"}')
    fit = 0
    for c, r in zip(f.cheats, ev.cheats):
        if c.problem:
            continue
        if r.state == 'refused':
            ok = False
            lines.append(f'fail {c.id}: {r.reason}')
            continue
        fit += 1
        for note in _asm(c, r):
            lines.append(f'warning {c.id}: {note}')
    lines.append(f'{fit} of {len(f.cheats)} cheats fit the module')
    return ok, lines


def _overlap(f, a, b):
    common = set(a.modules) & set(b.modules)
    if not common or (set(a.modules) != set(b.modules) and not (set(a.modules) <= set(b.modules)
                                                                 or set(b.modules) <= set(a.modules))):
        return False
    return all(f.platform.module_id_matches(a.modules[m], b.modules[m]) for m in common)


def _asm(cheat, result):
    """Where a patch's `asm` does not say what its `expect` and `bytes` are."""
    notes = []
    if not disasm.available() or result.uses != 'core':
        return notes
    resolved = iter(result.ops)
    for op in cheat.ops:
        r = next(resolved, None)
        if not isinstance(op, Patch) or op.asm is None or r is None or r.kind != 'patch':
            continue
        for k, data in (('expect', r.expect), ('bytes', r.bytes)):
            if k in op.asm:
                actual = disasm.arm64(data, r.offset or 0)
                if actual is not None and not disasm.same(actual, op.asm[k]):
                    notes.append(f'asm.{k} says "{op.asm[k]}", the bytes are "{actual}"')
    return notes
