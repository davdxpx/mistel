# mncf: the Python library

Reads, writes and converts `.mncf` files as [the spec](../../spec/mncf-1.md) says:

- **Read:** parse and check a file, identify the build that runs, resolve every location, and give each cheat its state, its reason and the bytes a runtime writes. Signatures and `.mncp` packs too.
- **Run:** a reference pass engine (spec, section 11.5): it takes what the player does and the guest memory of each pass, and gives the patches, restores, hooks and writes of the pass as one batch, each cheat's state and what the monitors read.
- **Write:** any file in one fixed layout, so that a program can save what it read or made.
- **Convert:** Atmosphère cheat files to `.mncf`, with all a file and its code module say, and `.mncf` back to Atmosphère for one build (spec, section 15).

Python 3.11 or later, and nothing outside the standard library: TOML through `tomllib`, Ed25519 in plain Python (`mncf/ed25519.py`, checked against the test vectors of RFC 8032), LZ4 in plain Python. Two optional packages make it better where they are installed: `capstone` (BSD) fills the `asm` fields of converted patches, `lz4` reads NSO segments faster. Under the MIT License, like everything in [`format/`](../../).

## Read

```python
import mncf

f = mncf.load(data, trust)                 # the bytes of a file; raises mncf.MncfError (code, message)
rt = mncf.Runtime(
    'switch',
    modules={'main': mncf.Module('A1B2C3D4E5F60718', base, image,
                                 sections={'code': (0, text_end), 'rodata': (ro, ro_end), 'data': (data, data_end)})},
    regions={'heap': (heap_base, heap_size)},
    memory=read_guest_memory,              # (address, size) -> bytes, or None outside guest memory
)
ev = mncf.evaluate(f, rt, mncf.Choices(on=['money']))
for c in ev.cheats:
    print(c.id, c.state, c.reason, [op.as_dict() for op in c.ops])
```

- `f.signature` is `verified`, `unsigned`, `unknown-key`, `invalid` or `revoked`. A `TrustList` holds the keys a runtime trusts (`trust(name, public_key)`) and the key ids it no longer trusts (`revoke(key_id)`); `mncf.read_trust_list(text)` reads one from a trust list file (`mncf-keys/1`, such as Mistel's own [`docs/keys.toml`](../../../docs/keys.toml)).
- Files and pack entries of more than 16 MiB, packs of more than 4,096 entries and more than 256 MiB in all are refused with `size`, whatever a ZIP header claims, so that a hostile pack cannot fill a runtime's memory.
- `Runtime` says what the runtime can do: `capabilities` (section 11.1), `extensions` (default `raw/1` and `arm64-imm/1`), `raw_syntaxes`, `arch`, `held` buttons, and `build` for a runtime that names its build itself.
- A runtime that does not have a part of the core yet names it in `unsupported`, out of `mncf.FEATURES` (`run.once`, `hold`, `override` and so on); the cheats that use it are refused with `runtime:unsupported:<key>`. A runtime that rewrote code for its own reasons gives those places in `rewritten`, as `{'main': [(offset, length)]}`; a patch or override stub over them is refused with `runtime:rewritten`.
- Each operation comes back resolved: a patch's address, `expect` and `bytes` (for `arm64-imm/1`, the encoded instruction), a write's bytes in the platform's byte order, an override's stub, a condition's address and value.
- `mncf.pack.read_pack(data, trust)` reads a pack: its manifest's signature result and each listed file with its status `ok`, `hash` or `missing`.

## Run

```python
eng = mncf.Engine(f, rt, mncf.Choices(on=['money']), rate=60)   # rate: passes per second, for timed cheats
eng.switch('speed', True)                  # between passes: what the player does
eng.set_value('money.count', 5000)
eng.hold(['ZL'])                           # the buttons held now
eng.press('ZL+R')                          # a press shorter than a pass
p = eng.tick(read_guest_memory)            # one pass; tick(memory, now=seconds) for a clock of the runtime's
for ch in p.changes:                       # code changes first (one stop), then writes and raw entries
    print(ch.as_dict())                    # {'kind': 'patch', 'address': ..., 'bytes': '69 12 00 B9'}
p.cheats, p.shown                          # every cheat's state after the pass; monitor values by cheat
changes = eng.stop()                       # puts back every place the cheats changed
```

- The engine reads guest memory only through the reader it is given and changes nothing itself: the caller makes the changes of each pass, in order. It keeps the original bytes of every place it changed and puts them back when no cheat that is on covers the place any more.
- `patch` and `restore` come with an address and bytes, `hook` with an address, action and value (a runtime without `code-bytes` hooks overrides), `unhook` with an address, `write` with an address and bytes, `raw` with the raw entry to run in this pass.
- A cheat's state after a pass is `off` (reason `done` for a once or timed cheat that switched itself off), `on`, `waiting` (for buttons, or `path-broken`) or `refused` with a reason, which can come up while the game runs: `expect-mismatch` when the code at a place changed, `value-out-of-range` for a value the player set.
- Timed cheats measure time from the `now` the caller gives each pass, or, where it gives none, from the number of passes divided by `rate`.

## Updates

A file's identity, its versions and the address of its newest version (spec, section 17):

```python
from mncf import update

f.id, f.file_version, f.update, f.changes  # example.org/someone/x, "5", Update(url, host), [Release, ...] newest first
check = update.check(installed, data, trust)   # is `data` a new version of the loaded file `installed`?
check.reason                               # '' or update:error:<code>, update:signature, update:game, update:id,
                                           # update:version, update:signer (Appendix E)
new = check.file                           # the new file, also where it is not taken, so a manual import can use it
for r in update.since(new, installed.file_version):            # the change entries to show, newest first
    print(r.version, r.date, update.text(new, r, 'de'))         # in the player's language where the file has it
marked_new, marked_fixed = update.marks(new, installed.file_version)
carried = update.carry(new, installed.file_version, choices, preset='relaxed', kept=['max-iv'])
carried.choices, carried.preset, carried.ids   # what the player set, under the new ids
carried.reset, carried.gone, carried.dropped   # values back at their default and why, values and cheats that are gone
```

- `check` follows section 17.2: the new file loads, its signature is not `invalid` or `revoked`, it is for the same game and has the same `id`, its version is above the installed one, and where the installed file is signed, a key that signed it signs the new one. Load `installed` with the same trust list, so that both signatures are checked against the keys the runtime trusts now.
- A key in a file's `signers` checks a signature line by a key that is not trusted: `f.signature.verified` maps the key id of each line that verified to its public key, and the result stays `unknown-key`. A line that names such a key and does not verify makes the file `invalid`.
- `carry` takes the `Choices` the player made for the installed version and gives them for the new one, following `renamed` and `removed` of each change entry above the installed version. A value that no longer fits goes back to its default (`reset`, with `type`, `range` or `choice`). `kept` names cheats the runtime keeps something else for, such as the answer to a crash question; `carried.ids` gives their new ids, `None` where they are gone.
- Packs: `update.check_pack(installed_pack, data, trust)`, and `update.pairs(old_pack, new_pack)`, which matches each file of the old pack with one of the new pack, by its `id` and game or else by its path.
- `update.url_host(url)` is the host of an address section 17.3 allows, or `None`; `update.USER_AGENT` is the User-Agent a runtime sends. The library makes no connection itself.
- What is wrong with these keys is a warning, and the file is still used: `id`, `update`, `changes`, `changes:<version>` and `signers`.

## Write

```python
from mncf import write

text = write.write_file(f)        # a file that was read, without its signature lines
text = write.dumps(doc)           # a document: a dict of TOML values in the shape load reads
```

Reading what was written gives the same document again, and writing it again gives the same text.

## Convert

```python
from mncf import convert
from mncf.module import module_from_bytes

module = module_from_bytes(nso_bytes)          # an NSO, compressed or not, or a plain image
module.exclude = [(0x1008, 8)]                 # runs whose bytes are not the game's own, such as rewritten code
conv = convert.from_atmosphere(
    [convert.Source('A1B2C3D4E5F60718', text, version='1.0.2', module=module)],
    game_id='0100000000ABC000', game_name='Example Quest', origin='example-cheats.zip', authors=['the player'])
conv.text, conv.report
name, text, back = convert.to_atmosphere(mncf.load(conv.text.encode()), '1.0.2')
```

- Every cheat keeps its Atmosphère code as a `raw/1` entry, and gets core operations where its code is plain writes: patches, writes, pointer paths, `hold`, `when`, master cheats, groups. With the module, patches get `expect` from it and a pattern wherever the same bytes mean the same at another place; `movz` and `fmov` patches become values the player sets (`arm64-imm/1`; `values=False` keeps them as bytes).
- Several files, one per build, become one file with all their builds; cheats that do the same in each are one cheat.
- `authors` are the people the source credits; the caller's `authors` are only used where it credits nobody. `origin` records the source's format and name.
- A cheat requires the master code only where it depends on it, so that the cheats that find their place by pattern keep working on a build the master does not fit. Lines of a source that are neither names nor code go into the file's `about`; an entry whose code mixes words with other text is left out and named there. EdiZon's `[--SectionStart:X--]` and `[--SectionEnd:X--]` mark groups.
- `conv.report` says for each cheat what it became (`pattern`, `build`, `raw` with the reason, `restore` for an entry that was dropped, or `unreadable` for one that was left out) and why; `conv.report.notes` holds the lines that are not code.
- `to_atmosphere(f, build, module=None, prefer='raw', choices=None, prefer_cheats=None, limits=ATMOSPHERE_LIMITS)` writes one build back: one the file names, or, with its module, a build it does not name, from the cheats' patterns. `prefer='core'` writes core operations where a cheat has them, and `prefer_cheats` (`{'money': 'core'}`) chooses for single cheats, such as one whose value the player set; `choices` (`mncf.Choices`) gives the values the player chose. With the module, patches are checked against `expect`. `report.written` says how each cheat was written, `report.reasons` the stable reason each other cheat was left out (`build-unknown`, `pattern-not-found`, `atmosphere:toggle`, `requires:<id>` and so on: spec, section 15).
- The file keeps Atmosphère's limits: one master entry with the code of every master cheat, at most 127 other entries with code of at most 256 words, less than 64 KiB, names of at most 63 bytes. What does not fit is left out with `atmosphere:too-many`, `atmosphere:too-long` or `atmosphere:too-big`, and a cheat that requires a cheat left out with `requires:<id>`. A host that takes more gives its own `convert.AtmosphereLimits(entries, words, size, name)`, where `None` is no limit, or `convert.NO_LIMITS`.
- Groups are EdiZon section markers, which take no slot and show no switch. Names lose the bracket that would end them early and control characters (`convert.atmosphere_name`), and a name the file has already gets ` (2)`.
- `to_atmosphere_all(f, modules=None, prefer='raw', choices=None, prefer_cheats=None, limits=ATMOSPHERE_LIMITS)` writes every build of the file, for an export in the Atmosphère layout: a list of `BackFile` with `build`, `name`, `text` and `report`, or `reason` (`atmosphere:module`) for a build without a main module. `modules` maps build keys to their main modules.
- `to_atmosphere_merged(sources, modules=None, limits=ATMOSPHERE_LIMITS)` writes several files of one game into one file for each build, as a host reads one a build: `sources` are `BackSource(file, choices=None, prefer='raw', prefer_cheats=None)`, `modules` maps build IDs to main modules. It gives a list of `MergedBackFile` with `build_id`, `name`, `text`, a `BackReport` for each source in `reports`, and the build key of each source in `builds` (`None` where a source does not name the build), or `reason` for a build without a main module.

```python
for b in convert.to_atmosphere_merged([convert.BackSource(shop), convert.BackSource(speed, choices, 'core')],
                                      {'0123456789ABCDEF': module}):
    b.name, b.text, [r.reasons for r in b.reports]
```

Patch files of a code module, IPS, IPS32 and IPSwitch text:

```python
conv = convert.from_patches(
    [convert.PatchSource(ips_bytes, module, file_name='exefs_patches/60fps/A1B2C3D4E5F60718.ips', version='1.0.2')],
    game_id='0100000000ABC000', origin='60fps.zip')
```

- A patch file holds no original bytes, so the module it patches is required: `expect` comes from it. `module_name` names a module other than `main`; the build ID comes from the file (`@nsobid-`), its name, `build_id` or the module.
- An IPS file becomes one cheat with `default = true`, named by its patch set (`set_name`, or the folder in `exefs_patches/`); each patch of a `.pchtxt` becomes one cheat, named by its comment and on where it is `@enabled`. Patches get patterns and player values as Atmosphère cheats do; what a file writes into data becomes a write with the original bytes as `expect`. `mncf.patches.read` reads the files themselves.

## Tests

```bash
python -m unittest discover -s format/lib/python/tests
python format/lib/python/tests/conformance.py
```

The first runs the unit tests and the [conformance suite](../../tests/), the pass cases among them; the second runs only the suite and prints one line for each difference. `conformance.py --dump` prints every case's results as JSON, to compare with another library. The tests make their own code modules (NSO files with LZ4 segments from a small compressor in `tests/synthetic.py`) and cheat files; nothing of a real game is in them.
