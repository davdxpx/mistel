"""Disassembly for the `asm` fields, through Capstone (BSD licensed) when it is installed: `pip install capstone`."""
import re

try:
    import capstone
except ImportError:          # the tools work without it; they only leave `asm` out
    capstone = None

_engine = None


def available():
    return capstone is not None


def _arm64():
    global _engine
    if _engine is None:
        arch = getattr(capstone, 'CS_ARCH_ARM64', None)
        if arch is None:
            arch = capstone.CS_ARCH_AARCH64
        _engine = capstone.Cs(arch, capstone.CS_MODE_ARM)
    return _engine


def arm64(data, address=0):
    """The instructions in data as text joined by "; ", or None when Capstone is missing or the bytes are not whole
    instructions."""
    if capstone is None or len(data) % 4:
        return None
    insns = list(_arm64().disasm(bytes(data), address))
    if sum(i.size for i in insns) != len(data):
        return None
    return '; '.join(f'{i.mnemonic} {i.op_str}'.strip() for i in insns)


def same(a, b):
    """Whether two instruction texts say the same, ignoring spaces and case."""
    norm = lambda s: re.sub(r'\s+', '', s.lower())  # noqa: E731
    return norm(a) == norm(b)
