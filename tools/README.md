# Tools

The `mistel` command for people who make cheats and mods. Python 3.11 or later; it reads, writes and converts `.mncf` files with the MIT library in [`format/lib/python/`](../format/lib/python/). Under the GNU General Public License v3.0 ([LICENSE](../LICENSE)).

| Command | What it does | Status |
|---|---|---|
| `mistel check` | Checks an `.mncf` file against the spec. With `--module`, also that every pattern matches exactly once in the game's code module, that `expect` is there, and that `asm` says what the bytes are | Version 1 |
| `mistel convert` | Turns an Atmosphère cheat file into `.mncf`, keeping every cheat, and an `.mncf` file back into an Atmosphère file for one build. Also turns the IPS, IPS32 and IPSwitch patch files of a code module into `.mncf` | Version 1 |
| `mistel pattern` | Makes the shortest byte pattern that finds one place in a code module | Version 1 |
| `mistel sign`, `mistel verify` | Signs a file or a pack with an author's key, and checks signatures | Version 1 |
| `mistel pack` | Puts `.mncf` files into an `.mncp` pack, with its manifest signed if a key is given | Version 1 |
| `mistel keygen` | Makes an Ed25519 key pair, never inside a repository | Version 1 |
| `mistel port` | Gives a file's cheats their places in a new build of the game: the places in code, found again by the code around them in the old build, and the patterns checked | Version 1: places in code |
| `mistel convert` from the cheat formats of other platforms' emulators and PC cheat tables | | Planned |

## Install

From a checkout, without installing anything:

```bash
cd tools
python -m mistel --help
```

Or installed, with the optional parts:

```bash
pip install ./format/lib/python "./tools[asm,fast]"
```

- `asm` installs [Capstone](https://www.capstone-engine.org/) (BSD license), which fills the `asm` fields of patches and lets `mistel check` compare them. Without it, the tools leave `asm` out.
- `fast` installs the `lz4` package, which reads the compressed segments of an NSO much faster than the plain Python decoder.

## Convert

`mistel convert` runs the converter of the MIT library ([`format/lib/python/`](../format/lib/python/#convert)), so that the tools and every program that reads `.mncf` convert the same way.

```bash
mistel convert <title ID>/cheats/<build ID>.txt --version <build ID>=1.0.2 --module <dump>/exefs/main -v
mistel convert 1.0.2/<build ID>.txt 1.0.3/<other build ID>.txt --game <title ID> --module <main of 1.0.2> --module <main of 1.0.3>
```

- The title ID and the build ID come from the path where it has the Atmosphère layout; otherwise `--game` and `--build-id` give them. `--version <build ID>=<version>` names a build as the player knows it. Several files, one per build, become one file.
- `--module` takes a main NSO, compressed or not; it serves the file of its own build. `--exclude` lists runs of the module whose bytes are not the game's own (hexadecimal offset and length, separated by a tab, one run per line): no `expect` and no pattern come from them.
- `--origin` names the source (default: the file name), `--author` the authors to name where the file credits nobody. `--no-asm` leaves out `asm`, `--no-values` keeps `movz` and `fmov` patches as bytes.
- `-v` says for each cheat what it became and why: `pattern`, `build` (offsets for its build), `raw` (with the reason), `restore`.

Patch files (`exefs_patches/<set>/<build ID>.ips`, IPS32, and IPSwitch `.pchtxt`), with the module they patch:

```bash
mistel convert atmosphere/exefs_patches/60fps/<build ID>.ips --game <title ID> --module <dump>/exefs/main --version <build ID>=1.0.2
mistel convert 60fps.pchtxt --game <title ID> --module subsdk0=<dump>/exefs/subsdk0 --set "60 FPS"
```

- A patch file holds no original bytes, so `--module` is required: `expect` comes from it. `NAME=PATH` names a module other than `main`. Each file takes the module of its build ID (from `@nsobid-` or the file name), or the only one given.
- An IPS file becomes one cheat, on by default, named by its folder in `exefs_patches/` or by `--set`. Each patch of a `.pchtxt` becomes one cheat, named by its comment and on where it is `@enabled`.
- What a file writes into data becomes a write that is made only while the place holds its original bytes. A patch over bytes that `--exclude` names is left out and named.

Back to Atmosphère for one build:

```bash
mistel convert cheats.mncf --build 1.0.2 -o A1B2C3D4E5F60718.txt
mistel convert cheats.mncf --build 1.0.2 --module <dump>/exefs/main --from-core
```

The first writes each cheat's `raw` entry for that build where it has one, word for word, and the core operations otherwise; `--from-core` writes the core operations even where a raw entry exists, with the module to find the patterns.

## Check, pattern, sign and pack

```bash
mistel check cheats.mncf --module <dump>/exefs/main
mistel pattern <dump>/exefs/main 0x1008
mistel keygen "Mistel" --out <a folder outside every repository>
mistel sign cheats.mncf --key <folder>/Mistel.key
mistel verify cheats.mncf --trust <folder>/Mistel.pub
mistel pack example.mncp cheats.mncf other.mncf --name "Example pack" --sign <folder>/Mistel.key
```

A secret key file stays on its author's machine. `mistel keygen` refuses to write one inside the Mistel repository or inside any git work tree that does not ignore it.

## Port to a new build

When a game update moves the code, a cheat that names its place by an offset for one build stops working on the new one. With the main module of a build the file knows and the main module of the new build, both NSO files from your own dump (`<dump>/<title ID>-work`, never from anywhere else), `mistel port` finds each place again:

```bash
mistel port cheats.mncf --old <dump>/<title ID>-work/1.0.2/main --new <dump>/<title ID>-work/1.0.3/main --version 1.0.3
```

- For each place in main's code or read-only data that a cheat names by an offset for the old build, the original bytes around it in the old main, with the bytes that move between builds left open (branch and page offsets), are searched in the new main. Where they are found exactly once, the cheat gets its offset for the new build, and a patch or override is checked against its `expect` there. A fixed offset that moved becomes an offset for each build.
- Where the code around the place is found but the instruction itself changed, the cheat is not given the place: the report says that the code there is not what it expects.
- A place found by a pattern needs nothing; the tool checks that the pattern still finds one place.
- Not ported yet, and named in the report: places in data (a pointer base, a value in main's data), places in regions with an offset for each build, fixed addresses, places in other modules, and raw code that is only for the old build.
- The new build is added to `builds` with its build ID and `--version`. The file is written beside the old one as `<file>.ported.mncf`, so that you can compare it; `-v` lists every place. Nothing of the game is read but the two main modules, and no keys.

## Tests

```bash
python -m unittest discover -s tools/tests
```

The tests make their own code modules and cheat files: NSO files with LZ4 segments built by a small compressor in the tests, and made-up cheats. Nothing of a real game is in the repository.
