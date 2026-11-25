# Tools

The `mistel` command for people who make cheats and mods. Python 3.11 or later; it reads, writes and converts `.mncf` files with the MIT library in [`format/lib/python/`](../format/lib/python/). Under the GNU General Public License v3.0 ([LICENSE](../LICENSE)).

| Command | What it does | Status |
|---|---|---|
| `mistel check` | Checks an `.mncf` file against the spec. With `--module`, also that every pattern matches exactly once in the game's code module, that `expect` is there, and that `asm` says what the bytes are | Version 1 |
| `mistel convert` | Turns an Atmosphère cheat file into `.mncf`, keeping every cheat, and an `.mncf` file back into an Atmosphère file for one build. Also turns the IPS, IPS32 and IPSwitch patch files of a code module into `.mncf` | Version 1 |
| `mistel pattern` | Makes the shortest byte pattern that finds one place in a code module | Version 1 |
| `mistel sign`, `mistel verify` | Signs a file or a pack with an author's key, and checks signatures | Version 1 |
| `mistel pack` | Puts `.mncf` files into an `.mncp` pack, with its manifest signed if a key is given, and makes a new version of a pack | Version 1 |
| `mistel release` | Makes a new version of an `.mncf` file: the next version number, a change entry with the cheats added, removed and renamed since the version before, the signatures, and a check that runtimes take it as a new version | Version 1 |
| `mistel update-check` | Fetches the newest version from a file's `[update]` address, as a runtime does, and says whether runtimes would take it and what players would see | Version 1 |
| `mistel keygen` | Makes an Ed25519 key pair, never inside a repository | Version 1 |
| `mistel port` | Tries a file's patterns on a new build and says which cheats still fit | Planned |
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

`mistel sign` writes the key into the file's `signers` before it signs a file that has no signature yet, so that a runtime can check the signature and tell later versions by the same key apart from others without trusting the key ([spec, section 12](../format/spec/mncf-1.md#12-signatures-and-trust)). A file that is signed already keeps its bytes, as a new line would break its signatures; `mistel release` lists every key before it signs.

## New versions

When a game update needs a new version of a file ([spec, section 17](../format/spec/mncf-1.md#17-updates)), change the cheats in the file, then:

```bash
mistel release cheats.mncf --previous released/cheats.mncf --text "Fits game version 1.0.4." \
    --renamed max-iv=max-ivs --fixed money --sign <folder>/Mine.key
mistel update-check released/cheats.mncf --new cheats.mncf
```

- `mistel release` changes the file as text, so that comments and layout stay: it sets `file_version` to the version after the previous one (or `--version`), and adds a `[[changes]]` entry with the date and `--text`. With `--previous`, the version players have now, the entry names the cheats added and removed by their ids, and it says where a removed and an added cheat have the same name, which is often one cheat with a new id: `--renamed old=new` then lets what players set for it follow it. `--fixed` names cheats that work again. An entry that the file has for the version already is kept as the author wrote it.
- The first time, `--id` gives the file its identity and `--url` the address where its newest version will be. Put the released file at that address after each release: a file in a repository, a release asset, or a site's fixed address.
- `--sign` signs the new version, with every key in `signers`. Once a version is signed, each later one must be signed by one of its keys; to change keys, give `--sign` twice, with the old key and the new one, for one version. With `--previous`, the new version is checked as a runtime checks it, and not written where a runtime would not take it (`--force` writes it anyway).
- `mistel update-check` asks the file's address for the newest version, as a runtime does: `https` only, the User-Agent `mncf-update/1`, nothing else about the player, at most 5 redirects, no private addresses. It says whether the version there is newer, the same or older, and what players would see. `--new` checks a local file instead.
- A pack gets new versions with `mistel pack`: `--id`, `--url` and `--file-version` the first time, then `--previous <old pack>` with `--text` and `--renamed old=new` for files that got a new path. Its id, address and change entries carry over, and the new entry names the files added and removed.

```bash
mistel pack cheats.mncp a.mncf b.mncf --name "My cheats" --id github.com/someone/cheats-pack \
    --url https://github.com/someone/cheats/releases/latest/download/cheats.mncp --file-version 1 --sign <key>
mistel pack cheats-2.mncp a.mncf c.mncf --name "My cheats" --previous cheats.mncp --text "C instead of B." --sign <key>
```

## Tests

```bash
python -m unittest discover -s tools/tests
```

The tests make their own code modules and cheat files: NSO files with LZ4 segments built by a small compressor in the tests, and made-up cheats. Nothing of a real game is in the repository.
