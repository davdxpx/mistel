"""Compares the C conformance runner's --dump with the Python runner's, byte for byte.

    python format/lib/c/tests/compare_dump.py <path of mncf_conformance>

The Python side is format/lib/python/tests/conformance.py: its cases and results functions, dumped as its
--dump does (json.dumps with indent=1, sort_keys=True). The conversion cases are left out, because the C
library has no converter yet. Exit 0 when both are the same.
"""
import json
import pathlib
import subprocess
import sys

HERE = pathlib.Path(__file__).resolve().parent
SUITE = HERE.parents[2] / 'tests'
sys.path.insert(0, str(HERE.parents[1] / 'python' / 'tests'))

import conformance  # noqa: E402


def python_dump():
    results = {}
    for name, case, run, folder in conformance.cases():
        if name.startswith('convert/'):
            continue
        results[name] = run(case, folder)
    return json.dumps(results, indent=1, sort_keys=True, default=str)


def c_dump(runner):
    proc = subprocess.run([runner, str(SUITE), '--dump'], stdout=subprocess.PIPE, check=False)
    text = proc.stdout.decode('utf-8')
    start = text.find('{\n')
    end = text.rfind('\n}\n')
    if start < 0 or end < 0:
        sys.exit(f'compare_dump: no JSON in the output of {runner}')
    return text[start:end + 2], proc.returncode


def main(argv):
    if len(argv) != 1:
        sys.exit(__doc__)
    want = python_dump()
    got, code = c_dump(argv[0])
    if got != want:
        a, b = want.splitlines(), got.splitlines()
        n = next((i for i, (x, y) in enumerate(zip(a, b)) if x != y), min(len(a), len(b)))
        print(f'compare_dump: the dumps differ at line {n + 1}:')
        print(f'  python: {a[n] if n < len(a) else None!r}')
        print(f'  c:      {b[n] if n < len(b) else None!r}')
        return 1
    print(f'compare_dump: the dumps are the same ({len(want.encode())} bytes, {want.count(chr(10)) + 1} lines)')
    return 1 if code != 0 else 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
