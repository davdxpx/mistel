"""Reads, writes and converts files of the Mistel Native Cheat Format, version 1 (`mncf/1`).

    f = mncf.load(data, trust)                 # bytes of an .mncf file; raises mncf.MncfError
    rt = mncf.Runtime('switch', modules={'main': mncf.Module('A1B2C3D4E5F60718', base, image, sections=...)})
    ev = mncf.evaluate(f, rt)                  # the build that runs, and each cheat's state, reason and operations

Writing is in mncf.write, converting Atmosphère cheat files in mncf.convert, packs in mncf.pack.
"""
import datetime
import tomllib

from . import ed25519, encode, platforms, signature
from .evaluate import CAPABILITIES, Choices, CheatResult, Evaluation, Module, ResolvedOp, Runtime, evaluate, find_all, \
    identify
from .model import MncfError, MncfFile, SignatureResult
from .schema import KNOWN_EXTENSIONS, build_file
from .signature import TrustList, key_id

__all__ = ['load', 'evaluate', 'identify', 'find_all', 'Runtime', 'Module', 'Choices', 'Evaluation', 'CheatResult',
           'ResolvedOp', 'MncfError', 'MncfFile', 'SignatureResult', 'TrustList', 'key_id', 'KNOWN_EXTENSIONS',
           'CAPABILITIES', 'ed25519', 'encode', 'platforms', 'signature']


def decode(data):
    """The text of a file's bytes, checked against section 2: UTF-8 without a byte order mark, LF line ends."""
    if data.startswith(b'\xef\xbb\xbf'):
        raise MncfError('encoding', 'the file starts with a byte order mark')
    try:
        text = data.decode('utf-8')
    except UnicodeDecodeError as e:
        raise MncfError('encoding', f'the file is not UTF-8 ({e.reason} at byte {e.start})') from None
    if '\r' in text:
        raise MncfError('encoding', 'the file has CR characters; line ends are LF only')
    return text


MAX_DEPTH = 128


def _depth_and_dates(v, depth=0):
    if depth > MAX_DEPTH:
        raise MncfError('toml', f'arrays and tables nest deeper than {MAX_DEPTH} levels')
    if isinstance(v, (datetime.date, datetime.time)):
        raise MncfError('toml', 'the format has no date or time values')
    if isinstance(v, dict):
        for x in v.values():
            _depth_and_dates(x, depth + 1)
    elif isinstance(v, list):
        for x in v:
            _depth_and_dates(x, depth + 1)


def parse_toml(text):
    """The TOML document of a text, without date or time values and nested at most MAX_DEPTH levels."""
    try:
        doc = tomllib.loads(text)
    except (tomllib.TOMLDecodeError, RecursionError) as e:
        raise MncfError('toml', str(e) or 'nested too deep') from None
    _depth_and_dates(doc)
    return doc


def load(data, trust=None):
    """Reads and checks an .mncf file. Raises MncfError when the file cannot be used at all."""
    data = bytes(data)
    text = decode(data)
    doc = parse_toml(text)
    sig = signature.check(data, trust)
    signed, _, _ = signature.split(data)
    parts = build_file(doc, platforms.PLATFORMS)
    return MncfFile(data=data, signed=signed, signature=sig, **parts)
