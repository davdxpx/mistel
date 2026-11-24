"""Runs the conformance suite in format/tests against this reader.

    python format/lib/python/tests/conformance.py            # every case, one line per failure
    python format/lib/python/tests/conformance.py --dump     # each case's results as JSON, to compare readers
"""
import base64
import io
import json
import pathlib
import sys
import tomllib
import zipfile

HERE = pathlib.Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent))
SUITE = HERE.parents[2] / 'tests'

import mncf  # noqa: E402
from mncf.pack import read_pack  # noqa: E402


def case_bytes(case, folder):
    if 'file' in case:
        return (folder / case['file']).read_bytes()
    if 'hex' in case:
        return bytes.fromhex(case['hex'])
    return case['text'].encode('utf-8')


def trust_list(case):
    trust = mncf.TrustList()
    t = case.get('trust', {})
    for key in t.get('key', []):
        trust.trust(key['name'], base64.b64decode(key['public']))
    for kid in t.get('revoked', []):
        trust.revoke(kid)
    return trust


class Memory:
    """The guest memory of a case: the modules' images, the regions, and pieces of memory of their own."""

    def __init__(self):
        self.chunks = []

    def add(self, start, data):
        self.chunks.append((start, bytearray(data)))

    def put(self, at, raw):
        """Writes bytes into the piece they lie in, or makes a piece of their own."""
        for start, buf in self.chunks:
            if start <= at and at + len(raw) <= start + len(buf):
                buf[at - start:at - start + len(raw)] = raw
                return
        self.chunks.append((at, bytearray(raw)))

    def read(self, address, size):
        for start, buf in self.chunks:
            if start <= address and address + size <= start + len(buf):
                return bytes(buf[address - start:address - start + size])
        return None


def put_pieces(memory, pieces):
    for piece in pieces:
        memory.put(piece['at'], bytes.fromhex(piece['hex']) if 'hex' in piece else bytes(piece['size']))


def runtime(env, memory=None):
    """The runtime of a case's [env] table, with its guest memory in `memory` (a Memory) when one is given."""
    memory = memory if memory is not None else Memory()
    modules = {}
    for name, m in env.get('modules', {}).items():
        data = bytearray(m['size'])
        for piece in m.get('bytes', []):
            raw = bytes.fromhex(piece['hex'])
            data[piece['at']:piece['at'] + len(raw)] = raw
        sections = {k: tuple(m[k]) for k in ('code', 'rodata', 'data') if k in m}
        modules[name] = mncf.Module(m['id'], m.get('base', 0), bytes(data), m['size'], sections or None)
        memory.add(m.get('base', 0), data)
    regions = {}
    for name, r in env.get('regions', {}).items():
        regions[name] = (r['base'], r['size'])
        memory.add(r['base'], bytes(r['size']))
    put_pieces(memory, env.get('memory', []))
    return mncf.Runtime(
        env.get('platform', 'switch'), game_id=env.get('game'), arch=env.get('arch'), modules=modules,
        regions=regions, memory=memory.read, capabilities=env.get('capabilities', mncf.CAPABILITIES),
        extensions=env.get('extensions', mncf.KNOWN_EXTENSIONS), raw_syntaxes=env.get('raw', ['atmosphere']),
        build=env.get('build'), held=env.get('held', []), unsupported=env.get('unsupported', []),
        rewritten={m: [tuple(r) for r in runs] for m, runs in env.get('rewritten', {}).items()})


def file_results(case, folder):
    """What the reader says about a file case, in the shape of the case's [expect] table."""
    data = case_bytes(case, folder)
    try:
        f = mncf.load(data, trust_list(case))
    except mncf.MncfError as e:
        return {'error': e.code}
    env = case.get('env', {})
    choices = mncf.Choices(env.get('on', []), env.get('off', []), env.get('values', {}))
    ev = mncf.evaluate(f, runtime(env), choices)
    out = {'signature': f.signature.result, 'signers': f.signature.signers, 'build': ev.build or '',
           'warnings': sorted(f.warnings), 'cheat': {}}
    for c in ev.cheats:
        out['cheat'][c.id] = {'state': c.state, 'reason': c.reason, 'uses': c.uses,
                              'ops': [op.as_dict() for op in c.ops]}
    return out


STEP_KEYS = {'about', 'memory', 'values', 'off', 'on', 'held', 'press', 'now', 'pass', 'stop', 'expect'}
STEP_EXPECT_KEYS = {'changes', 'shown', 'cheat'}


def _states(states):
    return {c.id: {'state': c.state, 'reason': c.reason} for c in states}


def pass_results(case, folder):
    """What the engine does in a pass case: the states after loading, and for each step the changes of its pass,
    every cheat's state after it, and what the monitors read. The changes of each pass are made in the memory."""
    try:
        f = mncf.load(case_bytes(case, folder), trust_list(case))
    except mncf.MncfError as e:
        return {'error': e.code}
    env = case.get('env', {})
    memory = Memory()
    rt = runtime(env, memory)
    eng = mncf.Engine(f, rt, mncf.Choices(env.get('on', []), env.get('off', []), env.get('values', {})),
                      rate=case.get('rate', 60))
    out = {'load': _states(eng.states()), 'steps': []}
    for step in case.get('step', []):
        put_pieces(memory, step.get('memory', []))
        for key, value in step.get('values', {}).items():
            eng.set_value(key, value)
        for cheat_id in step.get('off', []):
            eng.switch(cheat_id, False)
        for cheat_id in step.get('on', []):
            eng.switch(cheat_id, True)
        if 'held' in step:
            eng.hold(step['held'])
        for combo in step.get('press', []):
            eng.press(combo)
        got = {}
        if step.get('stop', False):
            got['changes'] = [ch.as_dict() for ch in eng.stop()]
            got['shown'] = {}
        elif step.get('pass', True):
            p = eng.tick(memory.read, step.get('now'))
            got['changes'] = [ch.as_dict() for ch in p.changes]
            got['shown'] = {k: [v.as_dict() for v in vs] for k, vs in p.shown.items()}
        for ch in got.get('changes', []):
            if ch['kind'] in ('patch', 'restore', 'write'):
                memory.put(ch['address'], bytes.fromhex(ch['bytes']))
        got['cheat'] = _states(eng.states())
        out['steps'].append(got)
    return out


def compare_passes(case, got):
    """The differences between a pass case's expected results and what the engine did, as lines."""
    if 'error' in got:
        return [f'error: the file does not load ({got["error"]})']
    problems = []

    def states(where, want, have):
        for cid, w in want.items():
            if cid not in have:
                problems.append(f'{where}{cid}: no such cheat')
                continue
            for key in ('state', 'reason'):
                if key in w and w[key] != have[cid][key]:
                    problems.append(f'{where}{cid}.{key}: expected {w[key]!r}, got {have[cid][key]!r}')

    def items(where, want, have):
        if len(want) != len(have):
            problems.append(f'{where}: expected {len(want)}, got {len(have)}: {have!r}')
            return
        for i, (w, g) in enumerate(zip(want, have)):
            for k, v in w.items():
                if k not in g or not _same(v, g[k], k):
                    problems.append(f'{where}[{i}].{k}: expected {v!r}, got {g.get(k)!r}')

    states('load: ', case.get('expect', {}).get('cheat', {}), got['load'])
    steps = case.get('step', [])
    for n, (step, g) in enumerate(zip(steps, got['steps']), 1):
        want = step.get('expect', {})
        for key in sorted(set(step) - STEP_KEYS) + sorted(set(want) - STEP_EXPECT_KEYS):
            problems.append(f'step {n}: unknown key {key}')
        if 'changes' in g:
            items(f'step {n}: changes', want.get('changes', []), g['changes'])
            shown_want = want.get('shown', {})
            if set(shown_want) != set(g['shown']):
                problems.append(f'step {n}: shown: expected {sorted(shown_want)}, got {sorted(g["shown"])}')
            for cid, values in shown_want.items():
                if cid in g['shown']:
                    items(f'step {n}: shown.{cid}', values, g['shown'][cid])
        elif 'changes' in want or 'shown' in want:
            problems.append(f'step {n}: expects changes, but has no pass')
        states(f'step {n}: ', want.get('cheat', {}), g['cheat'])
    return problems


def pack_results(case, folder):
    z = case.get('zip', {})
    if 'text' in z:
        data = z['text'].encode('utf-8')
    else:
        buf = io.BytesIO()
        with zipfile.ZipFile(buf, 'w') as archive:
            for entry, source in z.items():
                archive.writestr(entry, (folder / source).read_bytes())
        data = buf.getvalue()
    try:
        pack = read_pack(data, trust_list(case))
    except mncf.MncfError as e:
        return {'error': e.code}
    return {'signature': pack.signature.result, 'signers': pack.signature.signers,
            'files': {f.path: f.status for f in pack.files}}


def case_module(m, build_id):
    """The code module a conversion case describes: zero bytes with the given pieces, its sections and runs."""
    from mncf.module import CodeModule
    image = bytearray(m['size'])
    for piece in m.get('bytes', []):
        raw = bytes.fromhex(piece['hex'])
        image[piece['at']:piece['at'] + len(raw)] = raw
    sections = {k: tuple(m[k]) for k in ('code', 'rodata', 'data') if k in m}
    return CodeModule(bytes(image), sections, build_id, m.get('text_size'), [tuple(r) for r in m.get('exclude', [])])


def back_results(case, folder):
    """What the converter writes back to Atmosphère for one build: the file's name and text, how each cheat was
    written, and the reason each other cheat was left out."""
    from mncf import convert
    text = case['text'] if 'text' in case else (folder / case['mncf']).read_text(encoding='utf-8')
    f = mncf.load(text.encode('utf-8'))
    module = case_module(case['module'], case.get('module_build_id')) if 'module' in case else None
    choices = mncf.Choices(values=dict(case['values'])) if 'values' in case else None
    try:
        name, out, report = convert.to_atmosphere(f, case['build'], module, case.get('prefer', 'raw'), choices,
                                                  case.get('prefer_cheats'))
    except ValueError as e:
        return {'error': getattr(e, 'code', 'error')}
    return {'name': name, 'text': out, 'written': dict(report.written), 'reasons': dict(report.reasons)}


def convert_results(case, folder):
    """The text the converter writes for a conversion case, or the error it gives."""
    from mncf import convert
    if case.get('to') == 'atmosphere':
        return back_results(case, folder)
    sources = []
    for src in case['source']:
        module = case_module(src['module'], src['build_id']) if 'module' in src else None
        sources.append(convert.Source(src['build_id'], src['text'], version=src.get('version'), module=module,
                                      build_key=src.get('build_key')))
    try:
        conv = convert.from_atmosphere(sources, game_id=case['game'], platform=case.get('platform', 'switch'),
                                       game_name=case.get('game_name'), name=case.get('name'),
                                       origin=case.get('origin'), authors=case.get('authors'), asm=False,
                                       values=case.get('values', True))
    except ValueError as e:
        return {'error': getattr(e, 'code', 'error')}
    return {'text': conv.text}


def _same(expected, actual, key=None):
    if key in ('bytes', 'expect') and isinstance(expected, str) and isinstance(actual, str):
        return expected.upper() == actual.upper()
    if isinstance(expected, dict):
        return isinstance(actual, dict) and expected.keys() == actual.keys() and \
            all(_same(v, actual[k], k) for k, v in expected.items())
    if isinstance(expected, list):
        return isinstance(actual, list) and len(expected) == len(actual) and \
            all(_same(a, b) for a, b in zip(expected, actual))
    if isinstance(expected, bool) or isinstance(actual, bool):
        return type(expected) is type(actual) and expected == actual
    return expected == actual


def compare(expect, got, folder=None):
    """The differences between a case's [expect] table and what the reader said, as lines."""
    problems = []
    if 'error' in expect or 'error' in got:
        if expect.get('error') != got.get('error'):
            problems.append(f'error: expected {expect.get("error")!r}, got {got.get("error")!r}')
        return problems
    for key, default in (('signature', 'unsigned'), ('signers', []), ('build', ''), ('warnings', []), ('name', None),
                         ('written', {}), ('reasons', {})):
        if key not in got:
            continue
        want = sorted(expect.get(key, default)) if key == 'warnings' else expect.get(key, default)
        if want != got[key]:
            problems.append(f'{key}: expected {want!r}, got {got[key]!r}')
    if 'text' in got:
        want = (folder / expect['file']).read_text(encoding='utf-8') if folder and 'file' in expect else None
        if want != got['text']:
            a, b = (want or '').splitlines(), got['text'].splitlines()
            n = next((i for i, (x, y) in enumerate(zip(a, b)) if x != y), min(len(a), len(b)))
            problems.append(f'text differs at line {n + 1}: expected {a[n] if n < len(a) else None!r}, '
                            f'got {b[n] if n < len(b) else None!r}')
        return problems
    if 'files' in got:
        if expect.get('files', {}) != got['files']:
            problems.append(f'files: expected {expect.get("files")!r}, got {got["files"]!r}')
    if 'cheat' in got:
        want = expect.get('cheat', {})
        if set(want) != set(got['cheat']):
            problems.append(f'cheats: expected {sorted(want)}, got {sorted(got["cheat"])}')
        for cid, w in want.items():
            g = got['cheat'].get(cid)
            if g is None:
                continue
            for key in ('state', 'reason', 'uses'):
                if key in w and w[key] != g[key]:
                    problems.append(f'{cid}.{key}: expected {w[key]!r}, got {g[key]!r}')
            if 'ops' in w:
                if len(w['ops']) != len(g['ops']):
                    problems.append(f'{cid}.ops: expected {len(w["ops"])} operations, got {len(g["ops"])}')
                    continue
                for i, (wo, go) in enumerate(zip(w['ops'], g['ops'])):
                    for k, v in wo.items():
                        if k not in go or not _same(v, go[k], k):
                            problems.append(f'{cid}.ops[{i}].{k}: expected {v!r}, got {go.get(k)!r}')
    return problems


KINDS = (('cases', file_results), ('packs', pack_results), ('convert', convert_results), ('passes', pass_results))


def cases():
    """(name, case table, results function, folder) for every case of the suite, in name order."""
    for kind, run in KINDS:
        folder = SUITE / kind
        for path in sorted(folder.glob('*.toml')):
            yield f'{kind}/{path.stem}', tomllib.loads(path.read_text(encoding='utf-8')), run, folder


def check(name, case, got, folder):
    """The differences between a case's expected results and what this library gives, as lines."""
    if name.startswith('passes/'):
        return compare_passes(case, got)
    return compare(case.get('expect', {}), got, folder)


def main(argv):
    dump = '--dump' in argv
    failed, total, results = 0, 0, {}
    for name, case, run, folder in cases():
        total += 1
        got = run(case, folder)
        results[name] = got
        problems = check(name, case, got, folder)
        if problems:
            failed += 1
            for p in problems:
                print(f'{name}: {p}')
    if dump:
        print(json.dumps(results, indent=1, sort_keys=True, default=str))
    print(f'conformance: {total - failed} of {total} cases passed')
    return 1 if failed else 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
