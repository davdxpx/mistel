"""The platforms of Appendix A: what a reader needs to know about each one."""
import re
from dataclasses import dataclass

_SWITCH_BUTTONS = (
    'A B X Y L R ZL ZR PLUS MINUS LSTICK RSTICK DUP DDOWN DLEFT DRIGHT '
    'LSTICK_UP LSTICK_DOWN LSTICK_LEFT LSTICK_RIGHT RSTICK_UP RSTICK_DOWN RSTICK_LEFT RSTICK_RIGHT SL SR'
).split()
_N64_BUTTONS = 'A B Z START L R CUP CDOWN CLEFT CRIGHT DUP DDOWN DLEFT DRIGHT'.split()
_HEX_ID = re.compile(r'^[0-9A-Fa-f]{1,64}\Z')
_SHA256_ID = re.compile(r'^sha256:[0-9A-Fa-f]{64}\Z')
_PC_BUTTON = re.compile(r'^[A-Z0-9_]+\Z')


@dataclass(frozen=True)
class Platform:
    """One row of Appendix A."""
    name: str
    game_id: re.Pattern
    archs: tuple          # the architectures a runtime of this platform can have; the first is the default
    byte_order: str       # 'little' or 'big'
    pointer_size: int
    modules: tuple        # module names, or () when any name is allowed
    module_id: str        # 'hex' (a build ID, prefix match) or 'sha256'
    regions: tuple
    addresses: bool       # whether `address` locations exist on this platform
    buttons: tuple        # button names, or () when any upper-case name is allowed

    def valid_module_id(self, value):
        pattern = _HEX_ID if self.module_id == 'hex' else _SHA256_ID
        return isinstance(value, str) and bool(pattern.match(value))

    def module_id_matches(self, listed, running):
        """Whether a module identity from a file matches the identity of a module that runs (section 5)."""
        a, b = listed.lower(), running.lower()
        if self.module_id == 'hex':
            short, long_ = (a, b) if len(a) <= len(b) else (b, a)
            return long_.startswith(short)
        return a == b

    def valid_button(self, name):
        if self.buttons:
            return name in self.buttons
        return bool(_PC_BUTTON.match(name))


_SWITCH_MODULES = ('main', 'sdk') + tuple(f'subsdk{i}' for i in range(10))

PLATFORMS = {
    'switch': Platform('switch', re.compile(r'^[0-9A-Fa-f]{16}\Z'), ('arm64',), 'little', 8, _SWITCH_MODULES, 'hex',
                       ('heap', 'alias', 'aslr'), False, tuple(_SWITCH_BUTTONS)),
    'switch2': Platform('switch2', re.compile(r'^[0-9A-Fa-f]{16}\Z'), ('arm64',), 'little', 8, _SWITCH_MODULES, 'hex',
                        ('heap', 'alias', 'aslr'), False, tuple(_SWITCH_BUTTONS) + ('C',)),
    'n64': Platform('n64', re.compile(r'^[\x21-\x7E]{4}\Z'), ('mips',), 'big', 4, ('rom',), 'sha256',
                    (), True, tuple(_N64_BUTTONS)),
    'pc': Platform('pc', re.compile(r'^[a-z0-9-]+:\S+\Z'), ('x86-64', 'arm64'), 'little', 8, (), 'sha256',
                   (), False, ()),
}


def platform(name):
    """The platform of that name, or None."""
    return PLATFORMS.get(name)
