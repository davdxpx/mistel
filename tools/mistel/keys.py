"""Signing keys, signatures and packs: `mistel keygen`, `sign`, `verify` and `pack` (spec, sections 12 and 13).

A secret key file is TOML with the 32-byte Ed25519 secret key in base64; it stays on the author's own machine and
never goes into a repository. A public key file holds the public key, its key id and the name a trust list shows.
"""
import base64
import hashlib
import io
import os
import pathlib
import subprocess
import tomllib

import zipfile

import mncf
from mncf import ed25519, signature
from mncf import write as writer
from mncf.pack import read_pack, valid_path


class KeyFileError(ValueError):
    pass


TOOLS = pathlib.Path(__file__).resolve().parent


def repository_of(path):
    """The git work tree a path lies in, or None."""
    p = pathlib.Path(path).resolve()
    for parent in [p] + list(p.parents):
        if (parent / '.git').exists():
            return parent
    return None


def unsafe_place(path):
    """Why a secret key must not be written at path, or None: never in the repository of these tools, and in another
    git work tree only where git ignores the file."""
    root = repository_of(path)
    if root is None:
        return None
    if root in [TOOLS] + list(TOOLS.parents):
        return f'{path} is inside the Mistel repository'
    try:
        ignored = subprocess.run(['git', '-C', str(root), 'check-ignore', '-q', str(path)],
                                 capture_output=True).returncode == 0
    except OSError:
        ignored = False
    return None if ignored else f'{path} is inside the git work tree {root} and not ignored there'


def keygen(name, folder, seed=None):
    """Writes <folder>/<name>.key (secret) and <folder>/<name>.pub, and returns their paths."""
    folder = pathlib.Path(folder)
    stem = ''.join(ch if ch.isalnum() or ch in '-_' else '-' for ch in name).strip('-') or 'key'
    secret_path, public_path = folder / f'{stem}.key', folder / f'{stem}.pub'
    why = unsafe_place(secret_path)
    if why:
        raise KeyFileError(why + ': keep secret keys outside repositories')
    for p in (secret_path, public_path):
        if p.exists():
            raise KeyFileError(f'{p} exists already')
    seed = seed if seed is not None else os.urandom(32)
    pub = ed25519.public_key(seed)
    kid = signature.key_id(pub)
    folder.mkdir(parents=True, exist_ok=True)
    secret = (f'# An mncf signing key. Keep it offline and never commit it.\nalgorithm = "ed25519"\n'
              f'name = {writer.string(name)}\nkey_id = "{kid}"\nsecret = "{base64.b64encode(seed).decode()}"\n')
    fd = os.open(secret_path, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
    with os.fdopen(fd, 'w', encoding='utf-8', newline='\n') as out:
        out.write(secret)
    public_path.write_text(public_text(name, pub), encoding='utf-8', newline='\n')
    return secret_path, public_path


def public_text(name, pub):
    return (f'algorithm = "ed25519"\nname = {writer.string(name)}\nkey_id = "{signature.key_id(pub)}"\n'
            f'public = "{base64.b64encode(pub).decode()}"\n')


def _key_file(path, field):
    try:
        doc = tomllib.loads(pathlib.Path(path).read_text(encoding='utf-8'))
        raw = base64.b64decode(doc[field], validate=True)
    except (OSError, tomllib.TOMLDecodeError, KeyError, ValueError) as e:
        raise KeyFileError(f'{path}: not a key file with `{field}` ({e})') from None
    if doc.get('algorithm') != 'ed25519' or len(raw) != 32:
        raise KeyFileError(f'{path}: not an ed25519 key')
    return doc.get('name', pathlib.Path(path).stem), raw


def read_secret(path):
    return _key_file(path, 'secret')


def read_public(path):
    return _key_file(path, 'public')


def trust_list(public_paths, revoked=()):
    trust = mncf.TrustList()
    for p in public_paths:
        name, pub = read_public(p)
        trust.trust(name, pub)
    for kid in revoked:
        trust.revoke(kid)
    return trust


def sign_bytes(data, seed):
    """The bytes of a file with one more signature line. The file must end with a line end, and whatever follows its
    first signature line must be signature lines."""
    signed, lines, clean = signature.split(data)
    if not clean:
        raise KeyFileError('the file has other lines after its first signature line')
    if not signed.endswith(b'\n'):
        raise KeyFileError('the file must end with a line end before it is signed')
    return data + signature.sign_line(seed, signed)


def sign_file(path, seed, out=None):
    data = pathlib.Path(path).read_bytes()
    if str(path).endswith('.mncp'):
        result = sign_pack(data, seed)
    else:
        mncf.load(data)
        result = sign_bytes(data, seed)
    pathlib.Path(out or path).write_bytes(result)


def _zip(entries):
    buf = io.BytesIO()
    with zipfile.ZipFile(buf, 'w', zipfile.ZIP_DEFLATED) as z:
        for name, data in entries:
            info = zipfile.ZipInfo(name, date_time=(1980, 1, 1, 0, 0, 0))
            info.compress_type = zipfile.ZIP_DEFLATED
            info.external_attr = 0o644 << 16
            z.writestr(info, data)
    return buf.getvalue()


def sign_pack(data, seed):
    """A pack with its manifest signed once more."""
    with zipfile.ZipFile(io.BytesIO(data)) as z:
        entries = [(i.filename, z.read(i.filename)) for i in z.infolist() if not i.filename.endswith('/')]
    out = [(n, sign_bytes(d, seed) if n == 'pack.toml' else d) for n, d in entries]
    if not any(n == 'pack.toml' for n, _ in out):
        raise KeyFileError('the pack has no pack.toml')
    return _zip(out)


def make_pack(files, name, authors=None, about=None, file_version=None, seed=None):
    """The bytes of a pack of these (path in the pack, bytes) files, each checked as an .mncf file first."""
    lines = ['format = "mncf-pack/1"', f'name = {writer.string(name)}']
    if authors:
        lines.append(f'authors = {writer.scalar(authors)}')
    if about:
        lines.append(f'about = {writer.string(about)}')
    if file_version:
        lines.append(f'file_version = {writer.string(file_version)}')
    seen = set()
    for path, data in files:
        if not valid_path(path) or path == 'pack.toml' or path in seen:
            raise KeyFileError(f'{path!r} cannot be a path in a pack')
        seen.add(path)
        mncf.load(data)
        lines += ['', '[[file]]', f'path = {writer.string(path)}', f'sha256 = "{hashlib.sha256(data).hexdigest()}"']
    manifest = ('\n'.join(lines) + '\n').encode()
    if seed is not None:
        manifest = sign_bytes(manifest, seed)
    pack = _zip([('pack.toml', manifest)] + list(files))
    read_pack(pack)
    return pack


def verify(path, trust):
    """Lines that say what a file's or a pack's signatures are, and whether it may be applied."""
    data = pathlib.Path(path).read_bytes()
    out = []
    if str(path).endswith('.mncp'):
        pack = read_pack(data, trust)
        out.append(_line('pack.toml', pack.signature))
        for f in pack.files:
            if f.status != 'ok':
                out.append(f'{f.path}: {f.status}')
                continue
            out.append(_line(f.path, signature.check(f.data, trust)))
        return pack.usable and all(f.status == 'ok' for f in pack.files), out
    result = signature.check(data, trust)
    out.append(_line(pathlib.Path(path).name, result))
    return result.result not in ('invalid', 'revoked'), out


def _line(label, result):
    text = f'{label}: {result.result}'
    if result.signers:
        text += ' by ' + ', '.join(result.signers)
    if result.key_ids:
        text += ' (key ids ' + ', '.join(result.key_ids) + ')'
    return text
