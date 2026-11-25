"""Runs the conformance suite in format/tests against this reader.

    python format/lib/python/tests/conformance.py            # every case, one line per failure
    python format/lib/python/tests/conformance.py --dump     # each case's results as JSON, to compare readers
"""
import base64
import dataclasses
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


def zip_bytes(z, folder):
    """The bytes of a case's ZIP table: text, exact hex, or entries made from files of the folder."""
    if 'text' in z:
        return z['text'].encode('utf-8')
    if 'hex' in z:
        return bytes.fromhex(z['hex'])
    buf = io.BytesIO()
    with zipfile.ZipFile(buf, 'w') as archive:
        for entry, source in z.items():
            archive.writestr(entry, (folder / source).read_bytes())
    return buf.getvalue()


def pack_results(case, folder):
    data = zip_bytes(case.get('zip', {}), folder)
    try:
        pack = read_pack(data, trust_list(case))
    except mncf.MncfError as e:
        return {'error': e.code}
    return {'signature': pack.signature.result, 'signers': pack.signature.signers,
            'files': {f.path: f.status for f in pack.files}}


def side_bytes(case, side, folder):
    """The bytes of one side of an update case, `installed` or `new`: a file of the folder, or `<side>_text`, or
    `<side>_hex` for exact bytes."""
    if f'{side}_text' in case:
        return case[f'{side}_text'].encode('utf-8')
    if f'{side}_hex' in case:
        return bytes.fromhex(case[f'{side}_hex'])
    return (folder / case[side]).read_bytes()


def syntax_results(case):
    """What the reader says of the forms of section 17: addresses, versions, ids and dates."""
    from mncf import update
    return {'url': [update.url_host(x['text']) or '' for x in case.get('url', [])],
            'version': [update.is_version(x['text']) for x in case.get('version', [])],
            'compare': [update.compare(x['a'], x['b']) for x in case.get('compare', [])],
            'id': [update.is_file_id(x['text']) for x in case.get('id', [])],
            'date': [update.is_date(x['text']) for x in case.get('date', [])]}


def update_results(case, folder):
    """Whether the new side of an update case is a new version of the installed side (spec, section 17), the change
    entries to show and the cheats to mark, and with [settings], what the player set, moved to the new version."""
    from mncf import update
    if any(k in case for k in ('url', 'version', 'compare', 'id', 'date')):
        return syntax_results(case)
    trust = trust_list(case)
    if 'installed_zip' in case:
        old = read_pack(zip_bytes(case['installed_zip'], folder), trust)
        check = update.check_pack(old, zip_bytes(case['new_zip'], folder), trust)
    else:
        old = mncf.load(side_bytes(case, 'installed', folder), trust)
        check = update.check(old, side_bytes(case, 'new', folder), trust)
    out = {'reason': check.reason, 'from': old.file_version or '', 'to': check.to_version or '',
           'since': [], 'texts': [], 'new': [], 'fixed': []}
    new = check.file
    if new is None:
        return out
    shown = update.since(new, old.file_version)
    out['since'] = [r.version for r in shown]
    out['texts'] = [update.text(new, r, case.get('language')) or '' for r in shown]
    if 'installed_zip' in case:
        out['pairs'] = {k: v or '' for k, v in update.pairs(old, new, trust).items()}
        return out
    out['new'], out['fixed'] = update.marks(new, old.file_version)
    if 'settings' in case:
        s = case['settings']
        c = update.carry(new, old.file_version, mncf.Choices(s.get('on', []), s.get('off', []), s.get('values', {})),
                         s.get('preset') or None, s.get('kept', []))
        out['settings'] = {'on': c.choices.on, 'off': c.choices.off, 'values': c.choices.values,
                           'preset': c.preset or '', 'ids': {k: v or '' for k, v in c.ids.items()},
                           'dropped': c.dropped, 'gone': c.gone, 'reset': c.reset}
    return out


SETTINGS = (('on', []), ('off', []), ('values', {}), ('preset', ''), ('ids', {}), ('dropped', []), ('gone', []),
            ('reset', {}))


def compare_updates(case, got):
    """The differences between an update case's [expect] table and what the reader said, as lines. Only the keys
    the case gives are compared, and `reason`; under `settings`, every key, with its default where it is missing."""
    problems = []
    if 'url' in got:
        for kind, key in (('url', 'host'), ('version', 'valid'), ('compare', 'result'), ('id', 'valid'),
                          ('date', 'valid')):
            for item, value in zip(case.get(kind, []), got[kind]):
                if not _same(item[key], value):
                    problems.append(f'{kind} {item.get("text", item.get("a"))!r}: expected {item[key]!r}, '
                                    f'got {value!r}')
        return problems
    want = case.get('expect', {})
    keys = [k for k in want if k != 'settings'] + ([] if 'reason' in want else ['reason'])
    for key in keys:
        w = want.get(key, '')
        if key not in got or not _same(w, got[key]):
            problems.append(f'{key}: expected {w!r}, got {got.get(key)!r}')
    if 'settings' in want:
        g = got.get('settings', {})
        for key, default in SETTINGS:
            w = want['settings'].get(key, default)
            if not _same(w, g.get(key)):
                problems.append(f'settings.{key}: expected {w!r}, got {g.get(key)!r}')
    return problems


def case_module(m, build_id):
    """The code module a conversion case describes: zero bytes with the given pieces, its sections and runs."""
    from mncf.module import CodeModule
    image = bytearray(m['size'])
    for piece in m.get('bytes', []):
        raw = bytes.fromhex(piece['hex'])
        image[piece['at']:piece['at'] + len(raw)] = raw
    sections = {k: tuple(m[k]) for k in ('code', 'rodata', 'data') if k in m}
    return CodeModule(bytes(image), sections, build_id, m.get('text_size'), [tuple(r) for r in m.get('exclude', [])])


def case_limits(limits):
    """The limits of a case back to Atmosphère: Atmosphère's, "none", or a table that changes some of them, where
    "none" is no limit."""
    from mncf import convert
    if limits is None:
        return convert.ATMOSPHERE_LIMITS
    if limits == 'none':
        return convert.NO_LIMITS
    return dataclasses.replace(convert.ATMOSPHERE_LIMITS, **{k: None if v == 'none' else v for k, v in limits.items()})


def back_results(case, folder):
    """What the converter writes back to Atmosphère for one build: the file's name and text, how each cheat was
    written, and the reason each other cheat was left out. With [[source]], several files go into one file for each
    build: `back` lists them, and the one of `build_id` is compared, with `written` and `reasons` for each source."""
    from mncf import convert

    def load(t):
        text = t['text'] if 'text' in t else (folder / t['mncf']).read_text(encoding='utf-8')
        return mncf.load(text.encode('utf-8'))

    def choices(t):
        return mncf.Choices(values=dict(t['values'])) if 'values' in t else None

    module = case_module(case['module'], case.get('module_build_id')) if 'module' in case else None
    try:
        limits = case_limits(case.get('limits'))
        if 'source' in case:
            sources = [convert.BackSource(load(s), choices(s), s.get('prefer', 'raw'), s.get('prefer_cheats'))
                       for s in case['source']]
            files = convert.to_atmosphere_merged(sources, {case['module_build_id']: module} if module else None,
                                                 limits)
            back = [{k: v for k, v in (('name', b.name), ('reason', b.reason)) if v} |
                    {'builds': [k or '' for k in b.builds]} for b in files]
            b = next(b for b in files if b.build_id == case['build_id'])
            return {'back': back, 'name': b.name, 'text': b.text, 'written': [dict(r.written) for r in b.reports],
                    'reasons': [dict(r.reasons) for r in b.reports]}
        name, out, report = convert.to_atmosphere(load(case), case['build'], module, case.get('prefer', 'raw'),
                                                  choices(case), case.get('prefer_cheats'), limits)
    except ValueError as e:
        return {'error': getattr(e, 'code', 'error')}
    return {'name': name, 'text': out, 'written': dict(report.written), 'reasons': dict(report.reasons)}


def convert_results(case, folder):
    """The text the converter writes for a conversion case, or the error it gives."""
    from mncf import convert
    if case.get('to') == 'atmosphere':
        return back_results(case, folder)
    patches = case.get('from', 'atmosphere') == 'patches'
    sources = []
    for src in case['source']:
        module = case_module(src['module'], src.get('build_id')) if 'module' in src else None
        if patches:
            data = bytes.fromhex(src['hex']) if 'hex' in src else src['text'].encode('utf-8')
            sources.append(convert.PatchSource(data, module, file_name=src.get('file_name'), set_name=src.get('set'),
                                               module_name=src.get('module_name', 'main'),
                                               build_id=src.get('build_id'), version=src.get('version'),
                                               build_key=src.get('build_key')))
        else:
            sources.append(convert.Source(src['build_id'], src['text'], version=src.get('version'), module=module,
                                          build_key=src.get('build_key')))
    try:
        conv = (convert.from_patches if patches else convert.from_atmosphere)(
            sources, game_id=case['game'], platform=case.get('platform', 'switch'), game_name=case.get('game_name'),
            name=case.get('name'), origin=case.get('origin'), authors=case.get('authors'), asm=False,
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
                         ('written', {}), ('reasons', {}), ('back', [])):
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


KINDS = (('cases', file_results), ('packs', pack_results), ('convert', convert_results), ('passes', pass_results),
         ('updates', update_results))


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
    if name.startswith('updates/'):
        return compare_updates(case, got)
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
