# The conformance suite

Made-up `.mncf` files and `.mncp` packs, each with the result every reader of `mncf/1` must give. A reader conforms when it gives these results for every case ([spec, section 16](../spec/mncf-1.md#16-conformance)). Every game ID, build ID, address and byte here is made up.

| Folder | What |
|---|---|
| `cases/` | One case per `.toml` file: a file, the runtime it is applied in, and the expected results |
| `packs/` | One pack case per `.toml` file; `packs/files/` holds what goes into the ZIP files |
| `convert/` | One conversion case per `.toml` file: Atmosphère files and made-up code modules, and next to it the exact `.mncf` text every converter writes |
| `passes/` | One pass case per `.toml` file: a file, a runtime, and steps of what the player and the game do, each with the changes of one pass and the states after it ([spec, section 11.5](../spec/mncf-1.md#115-a-pass)) |
| `sign.py` | Writes the signed files from their unsigned sources |

The libraries run the whole suite: `python format/lib/python/tests/conformance.py`, `sh format/lib/java/build.sh` and, for C, `mncf_conformance format/tests` (see [`lib/c/`](../lib/c/), which has no converter yet and leaves the conversion cases out). Their `--dump` output is the same, case for case; `format/lib/c/tests/compare_dump.py` compares the C runner's with Python's byte for byte.

## A file case

```toml
about = "What the case shows."
text = '''
format = "mncf/1"
...
'''                          # or file = "signed-one.mncf" (next to the case), or hex = "EF BB BF ..." for exact bytes

[env]                        # the runtime the file is applied in
platform = "switch"
game = "0100000000ABC000"    # optional: the game that runs
arch = "x86-64"              # optional: default the platform's first architecture
capabilities = ["guest-read", "guest-write"]   # default: all six of section 11.1
extensions = ["raw/1"]       # default: raw/1 and arm64-imm/1
raw = ["atmosphere"]         # the raw syntaxes the runtime runs; this is the default
build = "1.0.3"              # optional: a runtime that names its build itself
held = ["ZL"]                # buttons held
unsupported = ["run.timed"]  # parts of the core the runtime does not have yet (spec, section 11.1)
on = ["money"]               # cheats the player switched on
off = ["heal"]               # cheats with default = true that the player switched off
values = { "money.count" = 5000 }   # values the player chose

[env.modules.main]           # a module that runs: its identity, base address and bytes
id = "A1B2C3D4E5F60718"
base = 0x8000000
size = 0x3000                # the image is this many zero bytes ...
code = [0x0, 0x1000]         # ... with these sections, as [start, end) offsets
rodata = [0x1000, 0x2000]
data = [0x2000, 0x3000]
bytes = [ { at = 0x100, hex = "68 12 00 B9" } ]   # ... and these bytes in it

[env.rewritten]              # code the runtime rewrote for its own reasons: (offset, length) runs per module
main = [[0x102, 4]]

[env.regions.heap]           # a region: zero bytes at that base
base = 0x10000000
size = 0x10000

[[env.memory]]               # guest memory: written into a module or region it lies in, else memory of its own
at = 0x10000118
hex = "00 02 00 10 00 00 00 00"   # or size = 0x100 for zero bytes

[[trust.key]]                # keys the reader trusts, by name
name = "Test key one"
public = "11qYAYKxCrfVS/7TyWQHOg7hcvPapiMlrwIaaPcHURo="
[trust]
revoked = ["39f713d0a644253f"]

[expect]
error = "format"             # a file error of Appendix D; then nothing else is compared
signature = "verified"       # default "unsigned"
signers = ["Test key one"]   # default []
build = "1.0.2"              # default "": no build
warnings = ["unknown-key:colour"]   # default []; compared without regard to order

[expect.cheat.money]         # every cheat of the file is listed
state = "off"                # off, on, waiting or refused
reason = ""                  # Appendix B, or ""
uses = "core"                # core or raw, compared where given
ops = [ { kind = "patch", address = 0x8000100, bytes = "69 12 00 B9" } ]
```

The memory of a case is the modules' images, the regions and the `[[env.memory]]` pieces. A read outside them is outside guest memory. Pattern searches and `expect` checks use the module images as given, never memory pieces written over them.

`ops` lists the cheat's resolved operations in file order: patches, writes, overrides, reads and conditions, or the one `raw` entry it uses. Only the keys a case gives are compared:

| Key | For | What |
|---|---|---|
| `kind` | all | `patch`, `write`, `override`, `read`, `when` or `raw` |
| `address` | all but `raw` | The absolute guest address, after a pointer path |
| `module`, `offset` | places in a module without a path | The module and the offset in it |
| `type` | writes, reads, conditions | The type |
| `expect`, `bytes` | patches, writes, overrides | The bytes compared and written; an override's `bytes` is its stub, and absent where the runtime hooks the function. Hexadecimal digits in any case |
| `action`, `value` | overrides | The action and the value it returns |
| `op`, `value` | conditions | The operator and the value: a number, `[low, high]`, or `{ address, type }` for another place |
| `label` | reads | The label |
| `syntax`, `build`, `code` | `raw` | The entry used |

A case shows at most one problem per cheat, so that the reason does not depend on the order in which a reader checks. Where the order is the rule, as unknown keys before broken values, the case says so in `about`.

## A pass case

```toml
about = "What the case shows."
text = '''
format = "mncf/1"
...
'''                          # or file = ..., or hex = ..., as in a file case
rate = 4                     # optional: passes per second, for timed cheats; default 60

[env]                        # the runtime, as in a file case: modules, regions, memory, held, on, off, values ...

[expect]                     # optional: states after loading, for the cheats listed
cheat.money = { state = "on", reason = "" }

[[step]]                     # what happens before one pass, in this order:
about = "What the step shows."                    # optional
memory = [ { at = 0x10000010, hex = "05 00" } ]  # the game changes guest memory
values = { "money.count" = 500 }                 # the player sets values
off = ["speed"]                                  # the player switches cheats off
on = ["money"]                                   # ... and on
held = ["ZL"]                                    # the buttons held from now on
press = ["ZL+R"]                                 # presses shorter than a pass
now = 1.5                                        # optional: the time of the pass, in seconds
pass = false                                     # optional: no pass in this step
stop = true                                      # optional: instead of a pass, the engine stops
  [step.expect]
  changes = [ { kind = "patch", address = 0x8000100, bytes = "69 12 00 B9" } ]   # default []
  shown.hp = [ { label = "HP", type = "u16", value = 42 } ]                      # default {}
  cheat.money = { state = "on", reason = "" }    # states after the pass, for the cheats listed
```

The memory of a pass case is the same as a file case's. The changes of each pass are made in it before the next step, as a runtime makes them: `patch`, `restore` and `write` put their bytes at their address; `hook`, `unhook` and `raw` change no memory.

`changes` lists every change of the step's pass, in order, and `shown` every monitor that read values in it; both are compared whenever the step has a pass or stops. As with operations, only the keys a change or a value gives are compared:

| Key | For | What |
|---|---|---|
| `kind` | all | `patch`, `restore`, `hook`, `unhook`, `write` or `raw` |
| `address` | all but `raw` | The address, after a pointer path |
| `bytes` | `patch`, `restore`, `write` | The bytes to write. Hexadecimal digits in any case |
| `action`, `value` | `hook` | The action and the value the function returns, absent for `"skip"` |
| `cheat` | `write`, `raw` | The cheat the change is for |
| `syntax`, `build`, `code` | `raw` | The raw entry to run |
| `label`, `type`, `value` | shown values | What a monitor read; `label` is absent where the `read` has none |

A step lists only the keys above; a reader's runner refuses any other, so that a mistyped key cannot pass unnoticed.

## A pack case

```toml
about = "What the case shows."

[zip]                        # the entries of the ZIP file, and the files they are made from
"pack.toml" = "files/pack.toml"
"example.mncf" = "files/example.mncf"
# or text = "..." for bytes that are not a ZIP file, or hex = "50 4B ..." for the exact bytes of a broken one

[expect]
error = "path:../example.mncf"
signature = "unsigned"
signers = []
files = { "example.mncf" = "ok" }   # ok, hash or missing, for every listed file
```

## A conversion case

```toml
about = "What the case shows."
game = "0100000000ABC000"     # the title ID
name = "Example"              # optional: the file's title
origin = "example.zip"        # optional: the source's name
authors = ["Player"]          # optional: the authors to name where the source credits nobody
values = false                # optional: movz and fmov patches stay bytes (default: they become values)

[[source]]                    # one Atmosphère file for each build
build_id = "0123456789ABCDEF"
version = "1.0.0"
text = '''
[Money]
04000000 00001008 B9001269
'''

  [source.module]             # optional: the main module of that build, made as a file case's modules are
  size = 0x3800
  code = [0x0, 0x2000]
  rodata = [0x2000, 0x3000]
  data = [0x3000, 0x3800]
  text_size = 0x1FE0          # where the code ends; after it, up to the next page, a code cave
  bytes = [ { at = 0x1008, hex = "68 12 00 B9" } ]
  exclude = [[0x1008, 4]]     # optional: runs whose bytes are not the module's own

[expect]
file = "convert-module.mncf"  # the exact text, written without `asm` fields
# or error = "atmosphere" (a file that is not an Atmosphère cheat file) or "source" (files that cannot be one)
```

A case back to Atmosphère (`back-*.toml`) has `to = "atmosphere"` and writes one build of an `.mncf` file:

```toml
to = "atmosphere"
mncf = "convert-module.mncf"     # the file, in this folder; or text = '''...''' for the file itself
build = "1.0.0"                  # the build to write: one the file names, or another one with its module
prefer = "core"                  # optional: core operations where a cheat has them (default "raw")
values = { "speed.value" = 4.0 } # optional: values the player chose
prefer_cheats = { money = "core" }     # optional: "raw" or "core" for single cheats
module_build_id = "1122334455667788"   # optional: the build ID of the module, for a build the file does not name

[module]                         # optional: the main module of that build, as in a conversion case
size = 0x3800
code = [0x0, 0x2000]

[expect]
name = "0123456789ABCDEF.txt"    # the file name
file = "back-core.txt"           # the exact text
written = { money = "core", loop = "raw" }          # how each cheat was written
reasons = { once = "atmosphere:run" }               # the reason each other cheat was left out (spec, section 15)
```

A conversion case of patch files (`patch-*.toml`) has `from = "patches"`, and each source is one patch file with the module it patches:

```toml
from = "patches"
game = "0100000000ABC000"

[[source]]
file_name = "0123456789ABCDEF.ips"   # optional: the file's name, which can give the build ID
set = "Faster"                 # optional: the patch set, which names an IPS file's cheat
module_name = "main"           # optional: the module it patches (default main)
build_id = "0123456789ABCDEF"  # optional: where neither the file nor its name gives it
version = "1.0.0"
hex = "50 41 54 43 48 ..."     # the bytes of an IPS or IPS32 file; or text = '''...''' for IPSwitch text

  [source.module]              # as above; without it, the conversion fails with "source"
  size = 0x3800
  code = [0x0, 0x2000]

[expect]
file = "patch-ips.mncf"
# or error = "patch" (a file that is no patch file) or "source"
```

## Signed cases

`sign.py` signs `cases/signed.mncf` and `packs/files/pack.toml` into the `signed-*.mncf` and `pack-signed*.toml` files, some of them changed after signing on purpose. It signs with the keys of RFC 8032, section 7.1, tests 1 and 2. Those keys are public test data: anyone can sign with them, so they prove nothing outside this suite.

| Name | Key id | Public key (base64) |
|---|---|---|
| Test key one | `21fe31dfa154a261` | `11qYAYKxCrfVS/7TyWQHOg7hcvPapiMlrwIaaPcHURo=` |
| Test key two | `39f713d0a644253f` | `PUAXw+hDiVqStwqnTRt+vJyYLM8uxJaMwM1V8Sr0Zgw=` |

After changing `packs/files/example.mncf`, put its new SHA-256 hash into the `packs/files/pack*.toml` manifests that list it and run `sign.py` again.
