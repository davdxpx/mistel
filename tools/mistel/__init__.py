"""The `mistel` tools for people who make cheats: check, convert, pattern, sign, verify, pack, keygen, release and
update-check.

The tools read, write and convert `.mncf` files with the MIT library in format/lib/python. Installed, that library is
the `mncf` package; run from a checkout of the repository, it is found next to the tools.
"""
import pathlib
import sys

try:
    import mncf  # noqa: F401
except ImportError:
    _reader = pathlib.Path(__file__).resolve().parents[2] / 'format' / 'lib' / 'python'
    if (_reader / 'mncf').is_dir():
        sys.path.insert(0, str(_reader))

__version__ = '1.1.0'
