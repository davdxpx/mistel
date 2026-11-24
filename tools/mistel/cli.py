"""The `mistel` command."""
import argparse
import pathlib
import re
import sys

import mncf

from mncf import atmosphere, convert, disasm, patches
from mncf.module import ModuleError, module_from_bytes, read_runs
from mncf.pattern import make_pattern

from . import __version__, check as checking, keys

_HEX16 = re.compile(r'^[0-9A-Fa-f]{16}$')


def _int(text):
    return int(text, 0)


def load_module(path):
    return module_from_bytes(pathlib.Path(path).read_bytes())


def module_arg(text):
    """A --module argument of check: PATH, or NAME=PATH for a module other than main."""
    name, sep, path = text.partition('=')
    if sep and name and not pathlib.Path(text).exists():
        return name, path
    return 'main', text


def _modules(args):
    out = {}
    for text in args.module or []:
        name, path = module_arg(text)
        out[name] = load_module(path)
    return out


def cmd_check(args):
    trust = keys.trust_list(args.trust or [], args.revoked or [])
    ok, lines = checking.check(args.file, _modules(args), trust)
    print('\n'.join(lines))
    return 0 if ok else 1


def _ids(path):
    """The build ID from `<build ID>.txt` and the title ID from a `<title ID>/cheats/` folder, where they are."""
    p = pathlib.Path(path).resolve()
    build = p.stem.upper() if _HEX16.match(p.stem) else None
    game = next((q.name.upper() for q in p.parents if _HEX16.match(q.name)), None)
    return build, game


def _pairs(values, what):
    out = {}
    for text in values or []:
        k, sep, v = text.partition('=')
        if not sep or not _HEX16.match(k):
            raise ValueError(f'{what} must be <build ID>=<value>, not {text!r}')
        out[k.upper()] = v
    return out


PATCH_FILES = ('.ips', '.ips32', '.pchtxt')


def cmd_convert(args):
    inputs = [pathlib.Path(p) for p in args.input]
    if any(p.suffix == '.mncf' for p in inputs) or args.to == 'atmosphere':
        return _back(args, inputs)
    if all(p.suffix.lower() in PATCH_FILES for p in inputs):
        return _patch_files(args, inputs)
    versions = _pairs(args.version, '--version')
    modules = {}
    for path in args.module or []:
        m = load_module(path)
        if not m.short_id:
            raise ValueError(f'{path} is not an NSO, so its build is unknown')
        if args.exclude:
            m.exclude = read_runs(pathlib.Path(args.exclude).read_text(encoding='utf-8'))
        modules[m.short_id] = m
    sources, game_id = [], args.game
    for p in inputs:
        build_id, game = _ids(p)
        if args.build_id and len(inputs) == 1:
            build_id = args.build_id.upper()
        if not build_id:
            raise ValueError(f'{p.name}: give --build-id where the file name is not the build ID')
        game_id = game_id or game
        sources.append(convert.Source(build_id, p.read_text(encoding='utf-8'), version=versions.get(build_id),
                                      module=modules.get(build_id)))
    if not game_id:
        raise ValueError('give --game where the path does not say the title ID')
    origin = args.origin or (inputs[0].name if len(inputs) == 1 else None)
    conv = convert.from_atmosphere(sources, game_id=game_id, game_name=args.game_name, name=args.name,
                                   origin=origin, authors=args.author, asm=not args.no_asm,
                                   values=not args.no_values)
    mncf.load(conv.text.encode())
    out = pathlib.Path(args.output) if args.output else inputs[0].with_suffix('.mncf')
    out.write_text(conv.text, encoding='utf-8', newline='\n')
    r = conv.report
    print(f'wrote {out}')
    print(f'{len(r.cheats)} cheats: {r.count("pattern")} with patterns, {r.count("build")} with offsets for their '
          f'build, {r.count("raw")} only as raw code, {r.count("restore")} restore entries dropped, '
          f'{r.count("unreadable")} unreadable entries left out; {len(r.groups)} groups, '
          f'{sum(c.values for c in r.cheats)} values')
    for line in r.notes:
        print(f'not cheat code, kept in about: {line}')
    if args.verbose:
        for c in r.cheats:
            print(f'  {c.outcome:8} {c.id or "-"} ({c.build})' + (f': {c.why}' if c.why else ''))
            for note in c.notes:
                print(f'           {note}')
    for w in r.warnings:
        print(f'warning: {w}')
    if not modules:
        print("without --module, patches take `expect` only from the file's own restore code, and nothing gets a "
              "pattern")
    elif not disasm.available() and not args.no_asm:
        print('Capstone is not installed, so the patches have no asm (pip install capstone)')
    return 0


def _patch_files(args, inputs):
    """IPS, IPS32 and IPSwitch files to .mncf, each with the module of its build ID."""
    versions = _pairs(args.version, '--version')
    modules = []
    for text in args.module or []:
        name, path = module_arg(text)
        m = load_module(path)
        if args.exclude:
            m.exclude = read_runs(pathlib.Path(args.exclude).read_text(encoding='utf-8'))
        modules.append((name, m))
    if not modules:
        raise ValueError('patch files need --module: the module they patch gives `expect`')
    sources, game_id = [], args.game
    for p in inputs:
        data = p.read_bytes()
        bid = patches.read(data).build_id or patches.build_id_of(p.name) or \
            (args.build_id.upper() if args.build_id and len(inputs) == 1 else None)
        fits = [(n, m) for n, m in modules if bid and m.build_id and
                (m.build_id.upper().startswith(bid) or bid.startswith(m.build_id.upper()))]
        if not fits and len(modules) == 1:
            fits = modules
        if not fits:
            raise ValueError(f'{p.name}: no --module has the build ID {bid}')
        name, module = fits[0]
        game_id = game_id or _ids(p)[1]
        in_set = p.parent.parent.name.lower() == 'exefs_patches'
        sources.append(convert.PatchSource(
            data, module, file_name=p.name, set_name=args.set or (p.parent.name if in_set else None),
            module_name=name, build_id=bid,
            version=next((v for k, v in versions.items() if bid and bid.startswith(k)), None)))
    if not game_id:
        raise ValueError('give --game where the path does not say the title ID')
    origin = args.origin or (inputs[0].name if len(inputs) == 1 else None)
    conv = convert.from_patches(sources, game_id=game_id, game_name=args.game_name, name=args.name, origin=origin,
                                authors=args.author, asm=not args.no_asm, values=not args.no_values)
    mncf.load(conv.text.encode())
    out = pathlib.Path(args.output) if args.output else inputs[0].with_suffix('.mncf')
    out.write_text(conv.text, encoding='utf-8', newline='\n')
    r = conv.report
    print(f'wrote {out}')
    print(f'{len(r.cheats)} patches: {r.count("pattern")} with patterns, {r.count("build")} with offsets for their '
          f'build, {r.count("unreadable")} left out; {sum(c.values for c in r.cheats)} values')
    for line in r.notes:
        print(f'not a patch, kept in about: {line}')
    for c in r.cheats:
        if c.outcome == 'unreadable':
            print(f'left out {c.name}: {c.why}')
        elif args.verbose:
            print(f'  {c.outcome:8} {c.id} ({c.build})')
        if args.verbose:
            for note in c.notes:
                print(f'           {note}')
    for w in r.warnings:
        print(f'warning: {w}')
    return 0


def _back(args, inputs):
    if len(inputs) != 1 or not args.build:
        raise ValueError('back to Atmosphère takes one .mncf file and --build')
    f = mncf.load(inputs[0].read_bytes())
    module = load_module(args.module[0]) if args.module else None
    name, text, report = convert.to_atmosphere(f, args.build, module, 'core' if args.from_core else 'raw')
    out = pathlib.Path(args.output) if args.output else inputs[0].with_name(name)
    out.write_text(text, encoding='utf-8', newline='\n')
    print(f'wrote {out}: {len(report.written)} cheats '
          f'({sum(1 for _, how in report.written if how == "core")} from core operations)')
    for cid, why in report.skipped:
        print(f'skipped {cid}: {why}')
    return 0


def cmd_pattern(args):
    module = load_module(args.module)
    p = make_pattern(module, args.offset, cover=args.cover, longest=args.longest)
    if p is None:
        print(f'no unique pattern of up to {args.longest} bytes covers 0x{args.offset:X}')
        return 1
    print(f'find = "{p.text}"')
    if p.place:
        print(f'offset = {p.place}')
    asm = disasm.arm64(module.image[args.offset:args.offset + 4], args.offset)
    if asm:
        print(f'# 0x{args.offset:X}: {asm}')
    return 0


def cmd_keygen(args):
    secret, public = keys.keygen(args.name, args.out)
    print(f'wrote {secret} (secret: keep it offline, never commit it)')
    print(f'wrote {public}')
    return 0


def cmd_sign(args):
    name, seed = keys.read_secret(args.key)
    for path in args.files:
        keys.sign_file(path, seed, args.output if len(args.files) == 1 else None)
        print(f'signed {args.output or path} with {name}')
    return 0


def cmd_verify(args):
    trust = keys.trust_list(args.trust or [], args.revoked or [])
    ok = True
    for path in args.files:
        good, lines = keys.verify(path, trust)
        ok = ok and good
        print('\n'.join(lines))
    return 0 if ok else 1


def cmd_pack(args):
    files = [(pathlib.Path(p).name, pathlib.Path(p).read_bytes()) for p in args.files]
    seed = keys.read_secret(args.sign)[1] if args.sign else None
    data = keys.make_pack(files, args.name, args.author, args.about, args.file_version, seed)
    pathlib.Path(args.output).write_bytes(data)
    print(f'wrote {args.output}: {len(files)} files' + (', manifest signed' if seed else ''))
    return 0


def parser():
    p = argparse.ArgumentParser(prog='mistel', description='Tools for .mncf cheat files.')
    p.add_argument('--version', action='version', version=f'mistel {__version__}')
    sub = p.add_subparsers(dest='command', required=True)

    c = sub.add_parser('check', help='check a file, and its patterns and expect against code modules')
    c.add_argument('file')
    c.add_argument('--module', action='append', help='a code module (NSO or plain image): PATH or NAME=PATH')
    c.add_argument('--trust', action='append', help='a public key file to trust')
    c.add_argument('--revoked', action='append', help='a key id that is no longer trusted')
    c.set_defaults(run=cmd_check)

    v = sub.add_parser('convert', help='Atmosphère cheats to .mncf, or .mncf to Atmosphère for one build')
    v.add_argument('input', nargs='+', help='Atmosphère files <build ID>.txt, one per build; IPS, IPS32 or '
                                            'IPSwitch patch files (.ips, .ips32, .pchtxt); or one .mncf file')
    v.add_argument('-o', '--output')
    v.add_argument('--module', action='append', help='a main NSO; it serves the cheat file of its own build. For '
                                                     'patch files: PATH or NAME=PATH, such as subsdk0=PATH')
    v.add_argument('--set', help='patch files: the name of their cheat (default: the folder in exefs_patches/)')
    v.add_argument('--exclude', help='runs of the module whose bytes are not its own: lines of a hexadecimal offset '
                                     'and length separated by a tab')
    v.add_argument('--to', choices=('mncf', 'atmosphere'))
    v.add_argument('--build', help='back to Atmosphère: the build to write')
    v.add_argument('--build-id', help='the build ID, where the file name does not say it')
    v.add_argument('--version', action='append', help='<build ID>=<version>: the version the player sees')
    v.add_argument('--game', help='the title ID, where the path does not say it')
    v.add_argument('--game-name')
    v.add_argument('--name', help='the title of the .mncf file')
    v.add_argument('--origin', help='where the cheats came from, such as a ZIP or folder name')
    v.add_argument('--author', action='append', help='an author to name when the files credit nobody')
    v.add_argument('--no-asm', action='store_true', help='leave out the asm fields')
    v.add_argument('--no-values', action='store_true', help='keep movz and fmov patches as plain bytes')
    v.add_argument('--from-core', action='store_true', help='write core operations even where a raw entry exists')
    v.add_argument('-v', '--verbose', action='store_true', help='say for each cheat what became of it')
    v.set_defaults(run=cmd_convert)

    t = sub.add_parser('pattern', help='the shortest unique pattern for a place in a module')
    t.add_argument('module')
    t.add_argument('offset', type=_int)
    t.add_argument('--cover', type=int, default=4, help='bytes of the place the pattern covers (default 4)')
    t.add_argument('--longest', type=int, default=64, help='the longest pattern to try (default 64)')
    t.set_defaults(run=cmd_pattern)

    k = sub.add_parser('keygen', help='make an Ed25519 key pair, outside any repository')
    k.add_argument('name', help='the name a trust list shows for the key')
    k.add_argument('--out', required=True, help='the folder for <name>.key and <name>.pub')
    k.set_defaults(run=cmd_keygen)

    s = sub.add_parser('sign', help='add a signature line to .mncf files, or sign a pack\'s manifest')
    s.add_argument('files', nargs='+')
    s.add_argument('--key', required=True, help='the secret key file')
    s.add_argument('-o', '--output', help='write the signed file here instead of in place (one file only)')
    s.set_defaults(run=cmd_sign)

    r = sub.add_parser('verify', help='check the signatures of .mncf files and packs')
    r.add_argument('files', nargs='+')
    r.add_argument('--trust', action='append', help='a public key file to trust')
    r.add_argument('--revoked', action='append', help='a key id that is no longer trusted')
    r.set_defaults(run=cmd_verify)

    a = sub.add_parser('pack', help='put .mncf files into an .mncp pack')
    a.add_argument('output')
    a.add_argument('files', nargs='+')
    a.add_argument('--name', required=True)
    a.add_argument('--author', action='append')
    a.add_argument('--about')
    a.add_argument('--file-version')
    a.add_argument('--sign', help='a secret key file to sign the manifest with')
    a.set_defaults(run=cmd_pack)
    return p


def main(argv=None):
    args = parser().parse_args(argv)
    try:
        return args.run(args)
    except (mncf.MncfError, keys.KeyFileError, ModuleError, atmosphere.AtmosphereError, ValueError, OSError) as e:
        print(f'mistel {args.command}: {e}', file=sys.stderr)
        return 1


if __name__ == '__main__':
    sys.exit(main())
