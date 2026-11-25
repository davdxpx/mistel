"""Updates (section 17): a file's identity, its versions, the address of its newest version and its change entries.

    check = update.check(installed, data, trust)    # whether `data` is a new version of the installed file
    check.reason                                    # '' when it is, else a reason of Appendix E
    update.since(check.file, installed.file_version)   # the change entries to show, newest first
    carried = update.carry(check.file, installed.file_version, choices, preset)   # what the player set, moved over

The same for packs: check_pack, and pairs to match the files of an old pack with those of a new one.
"""
import base64
import re
from dataclasses import dataclass, field

from .model import MncfError
from .values import ID

VERSION = re.compile(r'^[0-9]{1,9}(\.[0-9]{1,9}){0,7}\Z')
FILE_ID = re.compile(r'^[a-z0-9._-]+(/[a-z0-9._-]+)*\Z')
MAX_ID = 128
MAX_URL = 2048
DATE = re.compile(r'^([0-9]{4})-([0-9]{2})-([0-9]{2})\Z')
_LABEL = r'[a-z0-9](?:[a-z0-9-]{0,61}[a-z0-9])?'
URL = re.compile(r'^https://(' + _LABEL + r'(?:\.' + _LABEL + r')+)(?::([0-9]{1,5}))?'
                 r"(/[A-Za-z0-9\-._~!$&'()*+,;=:@/%]*)?(\?[A-Za-z0-9\-._~!$&'()*+,;=:@/?%]*)?\Z")
_BAD_ESCAPE = re.compile(r'%(?![0-9A-Fa-f]{2})')
UPDATE_KEYS = ('url',)
CHANGE_KEYS = ('version', 'date', 'text', 'added', 'fixed', 'removed', 'renamed')
USER_AGENT = 'mncf-update/1'
MAX_REDIRECTS = 5

REASONS = ('update:signature', 'update:game', 'update:id', 'update:version', 'update:signer')


# ---- versions -------------------------------------------------------------------------------------------------------

def is_version(v):
    """Whether a value is a version of section 17.1: 1 to 8 numbers of 1 to 9 digits, separated by dots."""
    return isinstance(v, str) and VERSION.match(v) is not None


def _numbers(v):
    parts = [int(x) for x in v.split('.')]
    while len(parts) > 1 and parts[-1] == 0:
        parts.pop()
    return parts


def compare(a, b):
    """-1, 0 or 1 as version a is below, the same as, or above version b. Raises ValueError for a non-version."""
    if not is_version(a) or not is_version(b):
        raise ValueError('not a version')
    x, y = _numbers(a), _numbers(b)
    n = max(len(x), len(y))
    x, y = x + [0] * (n - len(x)), y + [0] * (n - len(y))
    return (x > y) - (x < y)


def above(a, b):
    """Whether version a is above b, where b None or not a version counts as below every version."""
    if not is_version(a):
        return False
    return not is_version(b) or compare(a, b) > 0


# ---- the keys -------------------------------------------------------------------------------------------------------

def is_file_id(v):
    return isinstance(v, str) and 0 < len(v) <= MAX_ID and FILE_ID.match(v) is not None and \
        all(p not in ('.', '..') for p in v.split('/'))


def url_host(v):
    """The host of an address section 17.3 allows, or None when it does not allow it."""
    if not isinstance(v, str) or len(v) > MAX_URL:
        return None
    m = URL.match(v)
    if m is None or _BAD_ESCAPE.search(v):
        return None
    host, port = m.group(1), m.group(2)
    if host.rsplit('.', 1)[1].isdigit():
        return None
    if port is not None and not 1 <= int(port) <= 65535:
        return None
    return host


def is_date(v):
    m = DATE.match(v) if isinstance(v, str) else None
    if m is None:
        return False
    y, mo, d = (int(x) for x in m.groups())
    if not 1 <= mo <= 12:
        return False
    leap = y % 4 == 0 and (y % 100 != 0 or y % 400 == 0)
    days = (31, 29 if leap else 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31)[mo - 1]
    return 1 <= d <= days


@dataclass
class Update:
    """`[update]`: the address of the newest version, and its host."""
    url: str
    host: str


@dataclass
class Release:
    """One change entry (`[[changes]]`)."""
    version: str
    date: str = None
    text: str = None
    added: list = field(default_factory=list)
    fixed: list = field(default_factory=list)
    removed: list = field(default_factory=list)
    renamed: dict = field(default_factory=dict)


def read_signers(doc, warnings):
    """The public keys of `signers` (section 12), or [] with the warning `signers` when the key breaks a rule."""
    if 'signers' not in doc:
        return []
    v = doc['signers']
    keys = []
    if isinstance(v, list):
        for s in v:
            try:
                raw = base64.b64decode(s, validate=True) if isinstance(s, str) else b''
            except ValueError:
                raw = b''
            if len(raw) != 32 or base64.b64encode(raw).decode() != s or raw in keys:
                keys = None
                break
            keys.append(raw)
    else:
        keys = None
    if keys is None:
        warnings.append('signers')
        return []
    return keys


def read_keys(doc, warnings, item=None):
    """The keys of section 17.1 of a file or a pack manifest: a dict with `id`, `update` and `changes`. Problems become
    warnings. `item` checks one name in the lists of a change entry: an id of a cheat, or a pack's path."""
    item = item or (lambda x: isinstance(x, str) and ID.match(x) is not None)
    out = {'id': None, 'update': None, 'changes': []}
    if 'id' in doc:
        if is_file_id(doc['id']):
            out['id'] = doc['id']
        else:
            warnings.append('id')
    version = doc.get('file_version')
    versioned = is_version(version)
    if 'update' in doc:
        u = doc['update']
        if isinstance(u, dict):
            for k in u:
                if k not in UPDATE_KEYS:
                    warnings.append(f'unknown-key:update.{k}')
        host = url_host(u.get('url')) if isinstance(u, dict) else None
        if host is None or out['id'] is None or not versioned:
            warnings.append('update')
        else:
            out['update'] = Update(u['url'], host)
    if 'changes' in doc:
        entries = doc['changes']
        if not isinstance(entries, list) or not all(isinstance(e, dict) for e in entries) or \
                (entries and not versioned):
            warnings.append('changes')
        else:
            releases = []
            for i, e in enumerate(entries):
                for k in e:
                    if k not in CHANGE_KEYS:
                        warnings.append(f'unknown-key:changes.{k}')
                r = _release(e, version, releases, item)
                if r is None:
                    v = e.get('version')
                    warnings.append(f'changes:{v if isinstance(v, str) else i + 1}')
                else:
                    releases.append(r)
            out['changes'] = sorted(releases, key=lambda r: _numbers(r.version), reverse=True)
    return out


def _names(v, item):
    if not isinstance(v, list) or not all(item(x) for x in v) or len(set(v)) != len(v):
        return None
    return list(v)


def _release(e, file_version, seen, item):
    v = e.get('version')
    if not is_version(v) or compare(v, file_version) > 0 or any(compare(v, r.version) == 0 for r in seen):
        return None
    r = Release(v)
    if 'date' in e:
        if not is_date(e['date']):
            return None
        r.date = e['date']
    if 'text' in e:
        if not isinstance(e['text'], str):
            return None
        r.text = e['text']
    for key in ('added', 'fixed', 'removed'):
        if key in e:
            names = _names(e[key], item)
            if names is None:
                return None
            setattr(r, key, names)
    if len(set(r.added) | set(r.fixed) | set(r.removed)) != len(r.added) + len(r.fixed) + len(r.removed):
        return None
    if 'renamed' in e:
        ren = e['renamed']
        if not isinstance(ren, dict) or not all(item(k) and item(x) for k, x in ren.items()) or \
                len(set(ren.values())) != len(ren) or any(k in r.removed for k in ren):
            return None
        r.renamed = dict(ren)
    return r


# ---- a new version (section 17.2) -----------------------------------------------------------------------------------

@dataclass
class UpdateCheck:
    """Whether a file is a new version of the one a runtime holds. `reason` is '' when it is, else a reason of
    Appendix E. `file` is the new file, or pack, where it loads, also when it is not taken."""
    reason: str
    file: object = None
    from_version: str = None
    to_version: str = None

    @property
    def ok(self):
        return self.reason == ''


def _signed_by(old_sig, new_sig):
    """Step 6: whether a signature line of the new file verifies with a key that signed the old one."""
    if old_sig.result == 'unsigned':
        return True
    new_keys = new_sig.verified
    for kid in old_sig.key_ids:
        key = old_sig.verified.get(kid)
        if key is not None:
            if any(k == key for k in new_keys.values()):
                return True
        elif kid in new_keys:
            return True
    return False


def _steps(old, new, game):
    if new.signature.result in ('invalid', 'revoked'):
        return 'update:signature'
    if game and (old.platform.name != new.platform.name or old.game_id.lower() != new.game_id.lower()):
        return 'update:game'
    if old.id is None or new.id != old.id:
        return 'update:id'
    if not above(new.file_version, old.file_version):
        return 'update:version'
    if not _signed_by(old.signature, new.signature):
        return 'update:signer'
    return ''


def check(installed, data, trust=None):
    """Whether the bytes `data` are a new version of the loaded file `installed` (section 17.2). Load `installed`
    with the same trust list, so that both signatures are checked against the keys the runtime trusts now."""
    from . import load
    try:
        new = load(data, trust)
    except MncfError as e:
        return UpdateCheck(f'update:error:{e.code}', None, installed.file_version)
    return UpdateCheck(_steps(installed, new, True), new, installed.file_version, new.file_version)


def check_pack(installed, data, trust=None):
    """Whether the bytes `data` are a new version of the pack `installed` (section 17.6)."""
    from .pack import read_pack
    try:
        new = read_pack(data, trust)
    except MncfError as e:
        return UpdateCheck(f'update:error:{e.code}', None, installed.file_version)
    return UpdateCheck(_steps(installed, new, False), new, installed.file_version, new.file_version)


# ---- what to show and what the player set (sections 17.4 and 17.5) -------------------------------------------------

def since(f, version):
    """The change entries of a file or pack above `version` (None: all of them), newest first."""
    return [r for r in f.changes if above(r.version, version)]


def text(f, release, language=None):
    """The text of a change entry in a language, from the file's translations, or its own text."""
    if language:
        tr = f.i18n.get(language) if isinstance(getattr(f, 'i18n', None), dict) else None
        ch = tr.get('changes') if isinstance(tr, dict) else None
        entry = ch.get(release.version) if isinstance(ch, dict) else None
        if isinstance(entry, dict) and isinstance(entry.get('text'), str):
            return entry['text']
    return release.text


def ids(f, version, names, have=None):
    """Where each id (or a pack's path) of the version the player has is in file f: {old: new, or None where it is
    gone}. `have` says whether f has a name; by default whether it is one of f's cheats."""
    if have is None:
        have = lambda x: f.cheat(x) is not None  # noqa: E731
    out = {n: n for n in names}
    for r in reversed(since(f, version)):
        for n, cur in out.items():
            if cur is None:
                continue
            if cur in r.removed:
                out[n] = None
            elif cur in r.renamed:
                out[n] = r.renamed[cur]
    return {n: (cur if cur is not None and have(cur) else None) for n, cur in out.items()}


def marks(f, version):
    """The cheats to mark in file f for a player who had `version`: (new, fixed), ids of f in file order."""
    new, fixed = set(), set()
    for r in reversed(since(f, version)):
        new = {r.renamed.get(x, x) for x in new if x not in r.removed} | set(r.added)
        fixed = {r.renamed.get(x, x) for x in fixed if x not in r.removed} | set(r.fixed)
    order = [c.id for c in f.cheats]
    return [x for x in order if x in new], [x for x in order if x in fixed and x not in new]


def fits(spec, v):
    """'' where a value the player chose fits a value of the file, else why not: type, range or choice."""
    import math
    from .values import FLOAT_TYPES
    from .values import fits as fits_type
    if spec.type == 'bool':
        ok = isinstance(v, bool)
    elif spec.type in FLOAT_TYPES:
        ok = type(v) in (int, float) and not math.isnan(v) and fits_type(spec.type, v)
    else:
        ok = type(v) is int and fits_type(spec.type, v)
    if not ok:
        return 'type'
    if (spec.min is not None and v < spec.min) or (spec.max is not None and v > spec.max):
        return 'range'
    if spec.choices is not None and v not in spec.choices:
        return 'choice'
    return ''


@dataclass
class Carried:
    """What the player set, moved to a new version (section 17.5)."""
    choices: object                                   # a Choices with the new ids
    preset: str = None                                # the preset the player chose, where the new version has it
    ids: dict = field(default_factory=dict)           # every old cheat id seen: its new id, or None where it is gone
    dropped: list = field(default_factory=list)       # old cheat ids whose settings were dropped, in the order seen
    gone: list = field(default_factory=list)          # old "cheat.value" keys whose value the cheat no longer has
    reset: dict = field(default_factory=dict)         # new "cheat.value" keys back at their default: type, range, choice


def carry(f, version, choices, preset=None, kept=()):
    """What the player set for the version `version` of a file, moved to the new version f: the cheats switched on and
    off, the values and the preset. `kept` names cheats for which the runtime keeps something else, so that their new
    ids are in the result's `ids` too."""
    from .evaluate import Choices
    seen = []
    for cid in list(choices.on) + list(choices.off) + [k.partition('.')[0] for k in choices.values] + list(kept):
        if cid not in seen:
            seen.append(cid)
    m = ids(f, version, seen)
    out = Carried(Choices([], [], {}), ids=m, dropped=[c for c in seen if m[c] is None])
    for src, dst in ((choices.on, out.choices.on), (choices.off, out.choices.off)):
        for cid in src:
            if m[cid] is not None and m[cid] not in dst:
                dst.append(m[cid])
    for key, value in choices.values.items():
        cid, _, vid = key.partition('.')
        if m[cid] is None:
            continue
        spec = f.cheat(m[cid]).values.get(vid)
        if spec is None:
            out.gone.append(key)
            continue
        new_key = f'{m[cid]}.{vid}'
        if new_key in out.choices.values or new_key in out.reset:
            continue
        why = fits(spec, value)
        if why:
            out.reset[new_key] = why
        else:
            out.choices.values[new_key] = value
    if preset is not None and any(p.id == preset for p in f.presets):
        out.preset = preset
    return out


def pairs(old, new, trust=None):
    """Matches the files of an old pack with those of a new one (section 17.6): {old path: new path, or None}."""
    from . import load

    def identity(pf):
        if pf.status != 'ok':
            return None
        try:
            f = load(pf.data, trust)
        except MncfError:
            return None
        return (f.id, f.platform.name, f.game_id.lower()) if f.id is not None else None

    olds = {pf.path: identity(pf) for pf in old.files}
    news = {pf.path: identity(pf) for pf in new.files}
    out, taken = {}, set()
    for path, ident in olds.items():
        if ident is None:
            continue
        match = next((p for p, i in news.items() if i == ident and p not in taken), None)
        if match is not None:
            out[path], _ = match, taken.add(match)
    paths = ids(new, old.file_version, [p for p in olds if p not in out], have=lambda p: p in news)
    for path, target in paths.items():
        if target is not None and target not in taken and (olds[path] is None or news[target] is None):
            out[path], _ = target, taken.add(target)
        else:
            out[path] = None
    return {p: out[p] for p in olds}
