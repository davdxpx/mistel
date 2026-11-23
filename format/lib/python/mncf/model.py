"""The parts of an `.mncf` file after it was read and checked."""
from dataclasses import dataclass, field


class MncfError(Exception):
    """The file cannot be used at all. `code` is one of Appendix D's codes."""

    def __init__(self, code, message):
        super().__init__(f'{code}: {message}')
        self.code = code
        self.message = message


@dataclass(frozen=True)
class ValueRef:
    """A reference to a value the player sets: `"$count"`."""
    name: str


@dataclass
class Location:
    """A place (section 6.2). `kind` is module, module-builds, pattern, region, region-builds or address."""
    kind: str
    module: str = None
    region: str = None
    address: int = None
    offset: int = 0
    offsets: dict = None
    find: list = None
    section: str = None
    path: list = None


@dataclass
class Patch:
    at: Location
    expect: bytes
    bytes: bytes = None
    encode: dict = None          # insn, width, rd, value (arm64-imm/1)
    asm: dict = None


@dataclass
class Write:
    at: Location
    type: str
    value: object                # a number, bytes, or a ValueRef
    expect: object = None


@dataclass
class Override:
    function: Location
    action: str
    value: object = None         # an integer or a ValueRef
    expect: bytes = None


@dataclass
class Read:
    at: Location
    type: str
    label: str = None


@dataclass
class Condition:
    at: Location
    type: str
    op: str
    value: object = None         # a number, a ValueRef, a (low, high) tuple, a Place, or None for `changed`


@dataclass
class Place:
    """Another place a condition compares with."""
    at: Location
    type: str


@dataclass
class Raw:
    syntax: str
    build: str
    code: str


@dataclass
class ValueSpec:
    id: str
    type: str
    default: object
    min: object = None
    max: object = None
    step: object = None
    unit: str = None
    display: str = 'dec'
    choices: dict = None
    flags: dict = None


@dataclass
class Cheat:
    """One cheat. `problem` is the reason it is refused whatever the runtime, or None."""
    index: int
    id: str
    table: dict
    problem: str = None
    name: str = None
    about: str = None
    group: str = None
    tags: list = field(default_factory=list)
    default: bool = False
    master: bool = False
    run: str = 'on'
    duration: float = None
    hold: str = None
    toggle: str = None
    press: str = None
    requires: list = field(default_factory=list)
    excludes: list = field(default_factory=list)
    radio: str = None
    match: str = 'all'
    trigger: str = 'while'
    interval: int = 1
    values: dict = field(default_factory=dict)
    ops: list = field(default_factory=list)          # Patch, Write, Override, Read and Condition, in file order
    raw: list = field(default_factory=list)

    @property
    def core_ops(self):
        return [op for op in self.ops if not isinstance(op, Condition)]


@dataclass
class Build:
    key: str
    version: str
    modules: dict


@dataclass
class Preset:
    id: str
    name: str
    cheats: list
    values: dict


@dataclass
class SignatureResult:
    """The result of section 12 for a file or a pack manifest."""
    result: str                   # verified, unsigned, unknown-key, invalid, revoked
    signers: list = field(default_factory=list)     # names of the trusted keys that signed it
    key_ids: list = field(default_factory=list)     # key ids of every signature line, in order


@dataclass
class MncfFile:
    data: bytes
    signed: bytes                  # the bytes a signature covers
    signature: SignatureResult
    doc: dict
    name: str
    platform: object               # a platforms.Platform
    game_id: str
    game_name: str = None
    authors: list = field(default_factory=list)
    about: str = None
    file_version: str = None
    language: str = 'en'
    origin: dict = None            # format and name of the source a converter read
    required: list = field(default_factory=list)
    optional: list = field(default_factory=list)
    builds: dict = field(default_factory=dict)       # build key -> Build, in file order
    choices: dict = field(default_factory=dict)      # list name -> {int: name}
    cheats: list = field(default_factory=list)
    presets: list = field(default_factory=list)
    i18n: dict = field(default_factory=dict)
    warnings: list = field(default_factory=list)

    def cheat(self, cheat_id):
        for c in self.cheats:
            if c.id == cheat_id:
                return c
        return None
