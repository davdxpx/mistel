"""`mistel release`: a new version of an `.mncf` file or of a pack's manifest (spec, section 17).

The file is changed as text, so that the author's comments and layout stay: the `id`, `file_version` and `signers`
lines among the file's own keys, the `url` of `[update]`, and a new `[[changes]]` entry before the others. Whatever
is changed is read again and checked before anything is written.
"""
import base64
import datetime
import re
from dataclasses import dataclass, field

import mncf
from mncf import ed25519, signature, update
from mncf import write as writer


class ReleaseError(ValueError):
    pass


# ---- the text of a TOML file ----------------------------------------------------------------------------------------

def _lines(text):
    """(index, line, whether it starts outside a multi-line string) for each line of a TOML text."""
    out, inside = [], None
    for i, line in enumerate(text.split('\n')):
        out.append((i, line, inside is None))
        j = 0
        while j < len(line):
            if inside is None:
                if line[j] == '#':
                    break
                if line.startswith('"""', j) or line.startswith("'''", j):
                    inside, j = line[j:j + 3], j + 3
                    continue
                if line[j] in '"\'':
                    q, j = line[j], j + 1
                    while j < len(line) and line[j] != q:
                        j += 2 if q == '"' and line[j] == '\\' else 1
            elif line.startswith(inside, j) and not (inside == '"""' and j > 0 and line[j - 1] == '\\'):
                inside, j = None, j + 3
                continue
            j += 1
    return out


def _header(line):
    s = line.strip()
    return s if s.startswith('[') else None


class Text:
    """The lines of a TOML file, with small changes that keep everything else as it is."""

    def __init__(self, text):
        self.lines = text.split('\n')

    @property
    def text(self):
        return '\n'.join(self.lines)

    def _scan(self):
        return _lines(self.text)

    def first_header(self, after=0):
        for i, line, outside in self._scan():
            if i >= after and outside and _header(line):
                return i
        return len(self.lines)

    def find_header(self, name):
        for i, line, outside in self._scan():
            if outside and _header(line) == name:
                return i
        return None

    def _key_line(self, key, start, end):
        rx = re.compile(r'^\s*' + re.escape(key) + r'\s*=')
        for i, line, outside in self._scan():
            if start <= i < end and outside and rx.match(line):
                return i
        return None

    def set_top(self, key, value_text, after=('file_version', 'id', 'name')):
        """Sets one of the file's own keys, written on one line: replaces its line, or adds it after the first of
        the keys `after` that the file has, or at the end of its own keys."""
        end = self.first_header()
        i = self._key_line(key, 0, end)
        line = f'{key} = {value_text}'
        if i is not None:
            self.lines[i] = line
            return
        for k in after:
            j = self._key_line(k, 0, end)
            if j is not None:
                self.lines.insert(j + 1, line)
                return
        while end > 0 and not self.lines[end - 1].strip():
            end -= 1
        self.lines.insert(end, line)

    def set_in_table(self, header, key, value_text):
        """Sets a key of a table; adds the table before the first table of the file where it has none."""
        h = self.find_header(header)
        line = f'{key} = {value_text}'
        if h is None:
            at = self.first_header()
            self.lines[at:at] = [header, line, '']
            return
        end = self.first_header(h + 1)
        i = self._key_line(key, h + 1, end)
        if i is not None:
            self.lines[i] = line
        else:
            self.lines.insert(h + 1, line)

    def add_entry(self, block):
        """Adds a `[[changes]]` entry before the first one, or after `[update]`, or before the first table."""
        at = self.find_header('[[changes]]')
        if at is None:
            u = self.find_header('[update]')
            at = self.first_header(u + 1) if u is not None else self.first_header()
        self.lines[at:at] = block + ['']


def entry_block(release):
    out = ['[[changes]]', f'version = {writer.string(release.version)}']
    if release.date:
        out.append(f'date = {writer.string(release.date)}')
    if release.text:
        out.append(f'text = {writer.string(release.text)}')
    for key in ('added', 'fixed', 'removed'):
        names = getattr(release, key)
        if names:
            out.append(f'{key} = {writer.scalar(names)}')
    if release.renamed:
        out.append(f'renamed = {writer.scalar(release.renamed, "renamed")}')
    return out


# ---- versions -------------------------------------------------------------------------------------------------------

def next_version(version):
    """The version after `version`: its last number plus one; "1" where it is not a version."""
    if not update.is_version(version):
        return '1'
    parts = version.split('.')
    parts[-1] = str(int(parts[-1]) + 1)
    return '.'.join(parts)


# ---- a release ------------------------------------------------------------------------------------------------------

@dataclass
class Release:
    """What `make` did: the new text, the change entry, and what the author should know about it."""
    text: str
    version: str
    entry: object                                   # the update.Release written or found in the file
    written: bool                                   # False where the file had an entry for the version already
    notes: list = field(default_factory=list)
    check: object = None                            # update.check of the new version against the previous one


def _pairs(texts):
    out = {}
    for t in texts or []:
        old, sep, new = t.partition('=')
        if not sep or not old or not new:
            raise ReleaseError(f'--renamed takes old=new, not {t!r}')
        out[old] = new
    return out


def _names(items):
    """The ids of a file's cheats, or the paths of a pack's files, in order."""
    if hasattr(items, 'cheats'):
        return [c.id for c in items.cheats]
    return [f.path for f in items.files]


def diff(previous, current, renamed=None, fixed=None):
    """A change entry's lists from the ids of two versions: what was added and removed, with the renames given."""
    old, new = _names(previous), _names(current)
    renamed = dict(renamed or {})
    for a, b in renamed.items():
        if a not in old:
            raise ReleaseError(f'--renamed {a}={b}: the previous version has no {a!r}')
        if b not in new:
            raise ReleaseError(f'--renamed {a}={b}: this version has no {b!r}')
    targets = set(renamed.values())
    added = [x for x in new if x not in old and x not in targets]
    removed = [x for x in old if x not in new and x not in renamed]
    fixed = list(fixed or [])
    for x in fixed:
        if x not in new:
            raise ReleaseError(f'--fixed {x}: this version has no {x!r}')
        if x in added:
            raise ReleaseError(f'--fixed {x}: {x!r} is new in this version')
    return added, fixed, removed, renamed


def rename_hints(previous, current, added, removed):
    """Removed and added cheats with the same name: probably one cheat that got a new id."""
    if not hasattr(previous, 'cheats'):
        return []
    old = {c.id: c.name for c in previous.cheats if c.id in removed}
    new = {c.name: c.id for c in current.cheats if c.id in added}
    return [f'{a} was removed and {new[n]} added with the same name; if it is the same cheat, give '
            f'--renamed {a}={new[n]} so that what players set for it follows it' for a, n in old.items() if n in new]


def make(text, current, previous=None, version=None, entry_text=None, date=None, renamed=None, fixed=None,
         file_id=None, url=None, signers=(), load=mncf.load):
    """The text of a new version. `current` is the loaded text, `previous` the loaded previous version or None,
    `signers` the public keys that will sign it. Raises ReleaseError."""
    t = Text(signature.split(text.encode('utf-8'))[0].decode('utf-8'))
    notes = []
    base = previous.file_version if previous is not None else current.file_version
    if version is None and previous is not None and update.above(current.file_version, previous.file_version):
        version = current.file_version
    if version is None:
        version = next_version(base) if previous is not None or update.is_version(base) else '1'
    if not update.is_version(version):
        raise ReleaseError(f'{version!r} is not a version: numbers separated by dots, such as "5" or "2.1"')
    if previous is not None and not update.above(version, previous.file_version):
        raise ReleaseError(f'version {version} is not above the previous version {previous.file_version}')
    if file_id is not None:
        if not update.is_file_id(file_id):
            raise ReleaseError(f'{file_id!r} is not an id: lower-case letters, digits, ".", "-" and "_" in parts '
                               f'separated by "/"')
        t.set_top('id', writer.string(file_id), after=('name',))
    elif current.id is None:
        notes.append('the file has no id: give --id once, so that runtimes can tell its versions apart')
    if url is not None:
        if update.url_host(url) is None:
            raise ReleaseError(f'{url!r} is not an address a runtime may use: https, a host name, no user name '
                               f'and no "#" (spec, section 17.3)')
        t.set_in_table('[update]', 'url', writer.string(url))
    t.set_top('file_version', writer.string(version), after=('id', 'name'))
    if signers:
        keys = list(current.signers)
        for k in signers:
            if k not in keys:
                keys.append(k)
        t.set_top('signers', writer.scalar([base64.b64encode(k).decode() for k in keys]),
                  after=('file_version', 'id', 'name'))

    have = next((r for r in current.changes if update.compare(r.version, version) == 0), None)
    written = have is None
    if have is None:
        if previous is not None:
            added, fixed_ids, removed, renamed_ids = diff(previous, current, _pairs(renamed), fixed)
            notes += rename_hints(previous, current, added, removed)
        else:
            added, fixed_ids, removed, renamed_ids = [], list(fixed or []), [], _pairs(renamed)
            notes.append('without --previous, the entry names no added or removed cheats')
        day = date or datetime.date.today().isoformat()
        if not update.is_date(day):
            raise ReleaseError(f'{day!r} is not a date of the form YYYY-MM-DD')
        have = update.Release(version, day, entry_text, added, fixed_ids, removed, renamed_ids)
        t.add_entry(entry_block(have))
        if not entry_text:
            notes.append('the entry has no text: players see only the version number; give --text')
    else:
        notes.append(f'the file has an entry for version {version} already; it is kept as it is')
        if previous is not None:
            added, _, removed, _ = diff(previous, current, have.renamed)
            if set(added) != set(have.added) or set(removed) != set(have.removed):
                notes.append(f'its lists differ from the cheats: added {added}, removed {removed} by their ids')
    out = t.text
    try:
        new = load(out.encode('utf-8'))
    except mncf.MncfError as e:
        raise ReleaseError(f'the new version does not load ({e.code}: {e.message})') from None
    if new.file_version != version or (written and not any(update.compare(r.version, version) == 0
                                                            for r in new.changes)):
        raise ReleaseError('the change entry could not be written; see the warnings of mistel check')
    for w in new.warnings:
        notes.append(f'warning {w}')
    return Release(out, version, have, written, notes)


def sign(text, seeds):
    """The text signed with each seed, in order."""
    data = text.encode('utf-8')
    signed = data
    for seed in seeds:
        data += signature.sign_line(seed, signed)
    return data


def public_keys(seeds):
    return [ed25519.public_key(s) for s in seeds]
