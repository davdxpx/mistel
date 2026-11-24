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
- `to_atmosphere(f, build, module=None, prefer='raw', choices=None)` writes one build back: one the file names, or, with its module, a build it does not name, from the cheats' patterns. `prefer='core'` writes core operations where a cheat has them; `choices` (`mncf.Choices`) gives the values the player chose, and a value the cheat cannot take falls back to the raw entry or skips the cheat.

## Tests

```bash
python -m unittest discover -s format/lib/python/tests
python format/lib/python/tests/conformance.py
```

The first runs the unit tests and the [conformance suite](../../tests/), the pass cases among them; the second runs only the suite and prints one line for each difference. `conformance.py --dump` prints every case's results as JSON, to compare with another library. The tests make their own code modules (NSO files with LZ4 segments from a small compressor in `tests/synthetic.py`) and cheat files; nothing of a real game is in them.
