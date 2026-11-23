"""check_text.py <file>...: the text rules of CONTRIBUTING.md for the given text files. Exit 0 = clean, 1 = a rule is broken.

No em dash, no emoji, and no middle dot as a separator between items. License files are left as they are, and so are files
that are not UTF-8 text.
"""
import re
import sys
from pathlib import Path

RULES = [
    (re.compile('—'), 'em dash: use a full stop, a comma, a colon or parentheses'),
    (re.compile('·|•(?=.)'), 'middle dot or bullet used as a separator: use a list, a comma or "and"'),
    (re.compile('[\U0001F000-\U0001FAFF☀-➿⭐⭕️]'), 'emoji or pictograph: use words'),
]
SKIP = re.compile(r'(^|/)(LICENSE|COPYING)(\.[A-Za-z]+)?$')


def check(path):
    if SKIP.search(path.replace('\\', '/')):
        return []
    try:
        text = Path(path).read_text(encoding='utf-8')
    except (UnicodeDecodeError, FileNotFoundError, IsADirectoryError):
        return []
    found = []
    for n, line in enumerate(text.splitlines(), 1):
        for rx, why in RULES:
            if rx.search(line):
                found.append(f'{path}:{n}: {why}: {line.strip()[:120]}')
    return found


def main(paths):
    sys.stdout.reconfigure(encoding='utf-8', errors='replace')
    found = [f for p in paths for f in check(p)]
    for f in found:
        print(f)
    return 1 if found else 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
