# mncf: the Python library

Reads, writes and converts `.mncf` files as [the spec](../../spec/mncf-1.md) says:

- **Read:** parse and check a file, identify the build that runs, resolve every location, and give each cheat its state, its reason and the bytes a runtime writes. Signatures and `.mncp` packs too.
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

- `f.signature` is `verified`, `unsigned`, `unknown-key`, `invalid` or `revoked`. A `TrustList` holds the keys a runtime trusts (`trust(name, public_key)`) and the key ids it no longer trusts (`revoke(key_id)`).
- `Runtime` says what the runtime can do: `capabilities` (section 11.1), `extensions` (default `raw/1` and `arm64-imm/1`), `raw_syntaxes`, `arch`, `held` buttons, and `build` for a runtime that names its build itself.
- Each operation comes back resolved: a patch's address, `expect` and `bytes` (for `arm64-imm/1`, the encoded instruction), a write's bytes in the platform's byte order, an override's stub, a condition's address and value.
- `mncf.pack.read_pack(data, trust)` reads a pack: its manifest's signature result and each listed file with its status `ok`, `hash` or `missing`.

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
- `conv.report` says for each cheat what it became (`pattern`, `build`, `raw` with the reason, or `restore` for an entry that was dropped) and why.

## Tests

```bash
python -m unittest discover -s format/lib/python/tests
python format/lib/python/tests/conformance.py
```

The first runs the unit tests and the [conformance suite](../../tests/); the second runs only the suite and prints one line for each difference. `conformance.py --dump` prints every case's results as JSON, to compare with another library. The tests make their own code modules (NSO files with LZ4 segments from a small compressor in `tests/synthetic.py`) and cheat files; nothing of a real game is in them.
