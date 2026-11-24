"""Packs (section 13): a ZIP file `.mncp` with a manifest `pack.toml` at its root."""
import hashlib
import io
import re
import zipfile
import zlib
from dataclasses import dataclass, field

from . import MAX_FILE, decode, parse_toml, signature
from .model import MncfError

PACK_KEYS = ('format', 'name', 'authors', 'about', 'file_version', 'file')
MAX_ENTRIES = 4096                 # entries of a pack
MAX_TOTAL = 256 * 1024 * 1024      # bytes of all the entries a reader reads from one pack
_SHA256 = re.compile(r'^[0-9a-f]{64}\Z')


@dataclass
class PackFile:
    path: str
    sha256: str
    data: bytes = None
    status: str = 'ok'        # ok, hash (the bytes differ from the manifest) or missing


@dataclass
class Pack:
    name: str
    signature: object
    files: list
    authors: list = field(default_factory=list)
    about: str = None
    file_version: str = None
    warnings: list = field(default_factory=list)

    @property
    def usable(self):
        """Whether a runtime may apply the pack's files at all: not when the manifest's signature is bad."""
        return self.signature.result not in ('invalid', 'revoked')


def valid_path(path):
    parts = path.split('/')
    return bool(path) and '\\' not in path and all(p and p not in ('.', '..') for p in parts)


def _entry(archive, info, budget):
    """The bytes of one entry. Only stored and deflated entries are read, so that every reader reads the same packs,
    and none larger than MAX_FILE, whatever its header claims."""
    if info.compress_type not in (zipfile.ZIP_STORED, zipfile.ZIP_DEFLATED) or info.flag_bits & 1:
        raise MncfError('zip', f'{info.orig_filename}: neither stored nor deflated, or encrypted')
    if info.file_size > MAX_FILE:
        raise MncfError('size', f'{info.orig_filename}: more than {MAX_FILE} bytes')
    try:
        with archive.open(info) as fh:
            data = fh.read(MAX_FILE + 1)
    except (zipfile.BadZipFile, zlib.error, OSError, ValueError, EOFError, RuntimeError, NotImplementedError) as e:
        raise MncfError('zip', f'{info.orig_filename}: {e}') from None
    if len(data) > MAX_FILE:
        raise MncfError('size', f'{info.orig_filename}: more than {MAX_FILE} bytes')
    budget[0] += len(data)
    if budget[0] > MAX_TOTAL:
        raise MncfError('size', f'the pack holds more than {MAX_TOTAL} bytes')
    return data


def read_pack(data, trust=None):
    """Reads a pack's bytes. Raises MncfError when the pack cannot be used at all."""
    try:
        archive = zipfile.ZipFile(io.BytesIO(data))
        infos = [i for i in archive.infolist() if not i.orig_filename.endswith('/')]
    except (zipfile.BadZipFile, OSError, ValueError, EOFError, NotImplementedError) as e:
        raise MncfError('zip', f'not a ZIP file ({e})') from None
    if len(infos) > MAX_ENTRIES:
        raise MncfError('size', f'the pack has more than {MAX_ENTRIES} entries')
    budget = [0]
    entries_by_name = {}
    for info in infos:
        if info.orig_filename in entries_by_name:
            raise MncfError('zip', 'two entries have the same name')
        entries_by_name[info.orig_filename] = info
    if 'pack.toml' not in entries_by_name:
        raise MncfError('manifest', 'there is no pack.toml at the root')
    raw = _entry(archive, entries_by_name['pack.toml'], budget)
    try:
        text = decode(raw)
    except MncfError as e:
        raise MncfError('manifest', e.message) from None
    try:
        doc = parse_toml(text)
    except MncfError as e:
        raise MncfError('manifest', e.message) from None
    if not doc or next(iter(doc)) != 'format' or doc['format'] != 'mncf-pack/1':
        raise MncfError('format', '`format = "mncf-pack/1"` must be the first key of pack.toml')
    warnings = [f'unknown-key:{k}' for k in doc if k not in PACK_KEYS]
    name = doc.get('name')
    if not isinstance(name, str):
        raise MncfError('schema:name', '`name` must be a string')
    pack = Pack(name, signature.check(raw, trust), [], warnings=warnings)
    for key in ('about', 'file_version'):
        if key in doc:
            if not isinstance(doc[key], str):
                raise MncfError(f'schema:{key}', f'`{key}` must be a string')
            setattr(pack, key, doc[key])
    if 'authors' in doc:
        if not isinstance(doc['authors'], list) or not all(isinstance(a, str) for a in doc['authors']):
            raise MncfError('schema:authors', '`authors` must be an array of strings')
        pack.authors = list(doc['authors'])
    entries = doc.get('file')
    if not isinstance(entries, list) or not entries or not all(isinstance(e, dict) for e in entries):
        raise MncfError('schema:file', 'the manifest needs at least one [[file]]')
    seen = set()
    for e in entries:
        path, digest = e.get('path'), e.get('sha256')
        if not isinstance(path, str):
            raise MncfError('schema:file.path', '`path` must be a string')
        if not valid_path(path) or path == 'pack.toml' or path in seen:
            raise MncfError(f'path:{path}', f'{path!r} is not a usable path in a pack')
        if not isinstance(digest, str) or not _SHA256.match(digest):
            raise MncfError('schema:file.sha256', f'{path}: `sha256` must be 64 lower-case hexadecimal digits')
        for k in e:
            if k not in ('path', 'sha256'):
                warnings.append(f'unknown-key:file.{k}')
        seen.add(path)
        item = PackFile(path, digest)
        if path not in entries_by_name:
            item.status = 'missing'
        else:
            item.data = _entry(archive, entries_by_name[path], budget)
            if hashlib.sha256(item.data).hexdigest() != digest:
                item.status, item.data = 'hash', None
        pack.files.append(item)
    return pack
