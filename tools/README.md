# Tools

The `mistel` command for people who make cheats and mods. Python 3.11 or later; it reads, writes and converts `.mncf` files with the MIT library in [`format/lib/python/`](../format/lib/python/). Under the GNU General Public License v3.0 ([LICENSE](../LICENSE)).

| Command | What it does | Status |
|---|---|---|
| `mistel check` | Checks an `.mncf` file against the spec. With `--module`, also that every pattern matches exactly once in the game's code module, that `expect` is there, and that `asm` says what the bytes are | Version 1 |
| `mistel convert` | Turns an Atmosphère cheat file into `.mncf`, keeping every cheat, and an `.mncf` file back into an Atmosphère file for one build | Version 1 |
| `mistel pattern` | Makes the shortest byte pattern that finds one place in a code module | Version 1 |
| `mistel sign`, `mistel verify` | Signs a file or a pack with an author's key, and checks signatures | Version 1 |
| `mistel pack` | Puts `.mncf` files into an `.mncp` pack, with its manifest signed if a key is given | Version 1 |
| `mistel keygen` | Makes an Ed25519 key pair, never inside a repository | Version 1 |
| `mistel port` | Tries a file's patterns on a new build and says which cheats still fit | Planned |
| `mistel convert` from other emulators' formats and PC cheat tables | | Planned |

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

## Tests

```bash
python -m unittest discover -s tools/tests
```

The tests make their own code modules and cheat files: NSO files with LZ4 segments built by a small compressor in the tests, and made-up cheats. Nothing of a real game is in the repository.
