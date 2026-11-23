# The Mistel Native Cheat Format, version 1

Version line: `format = "mncf/1"`. File extension: `.mncf`. Status: **draft**. Until this spec is merged, the format can still change.

## 1. Purpose

An `.mncf` file describes the cheats for one game, for all of its versions and every way it runs: in an emulator, in a compatibility layer that runs the game's own code natively (such as nxnative on Android), and in a recompilation that turns the game into a native program. A program that reads the file knows from the file itself:

- which versions of the game it knows, and whether the one that runs is one of them,
- where each cheat writes, and what must be there before it writes,
- what each cheat is for, how it runs, which values a player can set, and how it relates to the other cheats,
- who made the file, and whether it was changed since.

A cheat whose checks fail stays off and says why. The game and the other cheats keep running.

The format has a small core and grows through versioned extensions (section 10). A reader that lacks an extension turns off only the cheats that use it.

## 2. Conventions

- The key words MUST, MUST NOT, SHOULD, SHOULD NOT and MAY are used as in RFC 2119.
- A **reader** is a program that parses a file. A **runtime** is a program that applies cheats to a running game: an emulator, a compatibility layer, or a recompiled game. Most runtimes contain a reader.
- A file is [TOML 1.0](https://toml.io/en/v1.0.0), UTF-8 without a byte order mark, with LF line ends. `format` MUST be the first key of the file.
- Integers are TOML integers; addresses and offsets are usually written in hexadecimal (`0x47D27A0`). A value above `0x7FFFFFFFFFFFFFFF` does not fit a TOML integer and is written as a string (`"0xFFFFFFFF80000000"`).
- **Bytes** are written as a string of hexadecimal pairs separated by single spaces: `"68 12 00 B9"`. In a pattern, `??` stands for any byte.
- An **id** is lower-case ASCII letters, digits and `-`, starting with a letter: `money`, `speed-x4`. Ids are unique within their kind in one file.
- A **build key** is the name a file uses for one version of the game, any non-empty string: `"1.0.2"`.

## 3. A first look

```toml
format = "mncf/1"
name = "Example Quest cheats"
authors = ["someone"]

[game]
platform = "switch"
id = "0100000000ABC000"
name = "Example Quest"

[builds."1.0.2"]
modules = { main = "A1B2C3D4E5F60718" }

[builds."1.0.3"]
modules = { main = "1122334455667788" }

[[cheat]]
id = "money"
name = "Money never goes down"
group = "Items"
about = "Buying something keeps your money where it is."
tags = ["touches_save"]

  [[cheat.patch]]
  at = { module = "main", find = "68 12 00 B9 ?? ?? ?? 94" }
  expect = "68 12 00 B9"
  bytes = "69 12 00 B9"
  asm = { expect = "str w8, [x19, #0x10]", bytes = "str w9, [x19, #0x10]" }
```

The cheat finds its place by a byte pattern, so it works on both builds and on a later one the file does not list, as long as the pattern finds exactly one place and the original bytes are there.

## 4. The file

| Key | Type | Required | Meaning |
|---|---|---|---|
| `format` | string | yes | `"mncf/1"`, the first key of the file |
| `name` | string | yes | The file's title, shown to the player |
| `authors` | array of strings | no | Who made the cheats |
| `about` | string | no | A description of the file, credits |
| `file_version` | string | no | The authors' own version of this file, so a runtime can say that a newer one was imported |
| `language` | string | no | The language of the file's plain strings, a BCP 47 tag. Default `"en"` |
| `extensions` | table | no | `required` and `optional`, arrays of extension names with their version (`"arm64-imm/1"`), section 10 |
| `game` | table | yes | Section 5 |
| `builds` | table | no | Section 5 |
| `cheat` | array of tables | yes | Section 7 |
| `choices` | table | no | Named lists of choices that values share, section 7.6 |
| `preset` | array of tables | no | Section 8 |
| `i18n` | table | no | Translations, section 9 |

## 5. The game and its builds

```toml
[game]
platform = "switch"          # Appendix A
id = "0100000000ABC000"      # the game's ID on that platform, in the form Appendix A gives
name = "Example Quest"

[builds."1.0.2"]
version = "1.0.2"            # shown to the player; default: the build key
modules = { main = "A1B2C3D4E5F60718", subsdk0 = "0F0E0D0C0B0A0908" }
```

- `builds` maps build keys to the identity of that version. `modules` maps module names to their identity, in the form the platform gives (Appendix A): for the Switch, the code module's build ID in hexadecimal; for most other platforms, `sha256:` followed by the hash of the module file.
- A runtime identifies the build that runs by comparing the identities of the modules that run with each build's `modules`. A build matches when every module it lists matches. A Switch build ID matches when the shorter of the two hexadecimal strings is a prefix of the longer one.
- A recompilation identifies the build by the original game version it was made from. It MUST NOT compare the identities of its own recompiled modules.
- A file MAY list no builds at all. Cheats that need no build (patterns, region offsets) still work then.

## 6. Address spaces and locations

### 6.1 Two address spaces

- **Guest memory** is the original game's memory, at the addresses the original console used. Emulators, compatibility layers and recompilations all keep it, so a write to guest memory works the same in each.
- **Code** is the original game's machine code. Where the original code runs (an emulator, a compatibility layer), a place in code is bytes that can be patched. In a recompilation, where the original code no longer exists as bytes, a place in code names a function by its original address, and the runtime acts on the recompiled function (section 7.5).

A **module** is one of the game's code modules, as loaded: its code, its read-only data and its data. A place in a module's data is guest memory. A **region** is any other area of guest memory that the platform names, such as the heap (Appendix A).

### 6.2 Locations

A location is a TOML inline table, written as `at`. It has exactly one base:

| Base | Keys | Meaning |
|---|---|---|
| Module, fixed | `module`, `offset` | An offset from the start of a module, the same in every build |
| Module, per build | `module`, `offsets` | `offsets` maps build keys to offsets. In a build it does not list, the location does not resolve |
| Module, pattern | `module`, `find`, and optionally `offset`, `section` | The place where `find` matches, plus `offset` (default 0). `section` limits the search: `"code"`, `"rodata"` or `"data"` |
| Region | `region`, `offset` or `offsets` | An offset from the start of a region, fixed or per build |
| Address | `address` | An absolute guest address, on platforms where guest addresses are fixed (Appendix A) |

Any base MAY be followed by a pointer path, `path`: an array of offsets. The runtime reads a pointer at the base, adds the first offset, reads a pointer there, adds the next offset, and so on; the last offset is added without a read. The pointer size is the platform's (Appendix A).

A fixed offset in a region:

```toml
at = { region = "heap", offset = 0x1200 }
```

A pointer path from an offset in `main` that differs per build:

```toml
at = { module = "main", offsets = { "1.0.2" = 0x47D27A0, "1.0.3" = 0x47D2820 }, path = [0x248, 0x10] }
```

The place 4 bytes after a pattern in `main`'s code:

```toml
at = { module = "main", find = "E8 03 00 B9 ?? ?? ?? 94", offset = 4, section = "code" }
```

A pointer path that meets a null pointer, or an address outside guest memory, does not resolve at that moment. The runtime tries again at the next pass; the cheat waits (Appendix B, `path-broken`).

### 6.3 Patterns

- A pattern is bytes with `??` for any byte. It MUST be at least 4 bytes long and MUST NOT start or end with `??`.
- A pattern MUST match exactly once in the searched module (or section). No match turns the cheat off with `pattern-not-found`, more than one with `pattern-ambiguous`.
- A runtime searches each pattern once per build and MAY keep the result for later runs of the same build.

## 7. Cheats

### 7.1 Identity and meaning

| Key | Type | Required | Meaning |
|---|---|---|---|
| `id` | id | yes | Unique among the file's cheats |
| `name` | string | yes | Shown to the player |
| `about` | string | no | What the cheat does, in a sentence or two |
| `group` | string | no | Where it is listed. `/` separates levels: `"Items/Potions"` |
| `tags` | array of strings | no | Section 7.8 |
| `default` | boolean | no | Whether it is on the first time the file is used. Default `false` |
| `master` | boolean | no | A cheat other cheats need, such as a setup code. It is not shown as a switch; it is on while any cheat that requires it is on |
| `run` | string | no | How it runs, section 7.2. Default `"on"` |

A cheat does its work through one or more operations: `patch` (section 7.4), `write` (7.4), `override` (7.5) and `read` (7.7). A cheat with no operation is an error.

### 7.2 How a cheat runs

| `run` | Meaning |
|---|---|
| `"on"` | While the cheat is on, its patches and overrides stay applied and its writes repeat at every pass. Switching it off restores what its patches and overrides changed |
| `"once"` | Its writes happen once, when it is switched on. It then shows as done and can be run again |
| `"action"` | Its writes happen once each time its `press` buttons are pressed. Needs `press` |
| `"timed"` | As `"on"`, for `duration` seconds (a number), then it switches itself off |
| `"monitor"` | It only reads and shows values (section 7.7) |

A **pass** is one round of the runtime's cheat work, about once per frame the game presents.

### 7.3 Buttons

| Key | Meaning |
|---|---|
| `hold` | The cheat works only while these buttons are held: patches and overrides are applied while held and restored when released |
| `toggle` | Pressing these buttons together switches the cheat on or off |
| `press` | For `run = "action"`: the buttons that trigger it |

Buttons are written as names joined by `+`: `"ZL"`, `"L+DUP"`. The names are the platform's (Appendix A). A runtime SHOULD let the player change them.

### 7.4 Patches and writes

A **patch** changes the original code, or read-only data, at one place:

```toml
  [[cheat.patch]]
  at = { module = "main", find = "68 12 00 B9 ?? ?? ?? 94" }
  expect = "68 12 00 B9"
  bytes = "69 12 00 B9"
  asm = { expect = "str w8, [x19, #0x10]", bytes = "str w9, [x19, #0x10]" }
```

- `expect` is REQUIRED and has the same length as `bytes`. Before each write, the runtime compares the bytes at the place with `expect` and, if they differ and are not `bytes` either, turns the cheat off with `expect-mismatch`.
- `asm` is for people: the same as `expect` and `bytes`, as instructions. Readers MUST use `expect` and `bytes` and MAY ignore `asm`. `mistel check` checks that they agree.
- A patch needs a runtime that can change code bytes (section 11.1). Where code exists only as recompiled functions, a patch does not apply (`capability-missing`); an override (section 7.5) can.

A **write** puts a value into guest memory:

```toml
  [[cheat.write]]
  at = { region = "heap", offset = 0x1200 }
  type = "u32"
  value = 999999
```

- `type` is one of `u8`, `u16`, `u32`, `u64`, `s8`, `s16`, `s32`, `s64`, `f32`, `f64`, or `bytes`. Values are stored in the platform's byte order (Appendix A).
- `value` is a number of that type, or for `bytes` a bytes string, or a reference to a value the player sets: `"$count"` (section 7.6).
- `expect` is OPTIONAL for writes. If present, the write happens only while the place holds that value.
- Writes are not restored when the cheat is switched off: they changed the game's state, not its code.

### 7.5 Overrides

An override changes what a function does, at the level of the whole function. It works in every runtime: one that runs the original code writes a short stub at the function's entry, a recompilation replaces or wraps the recompiled function.

```toml
  [[cheat.override]]
  function = { module = "main", find = "FD 7B BF A9 FD 03 00 91 ?? ?? ?? 94" }
  action = "return"
  value = 1
  expect = "FD 7B BF A9"
```

| Key | Meaning |
|---|---|
| `function` | A location (section 6.2) that resolves to the function's first instruction. In a recompilation, the runtime maps that original address to its recompiled function |
| `action` | `"return"`: the function returns `value` at once. `"skip"`: the function returns at once without a value |
| `value` | For `"return"`: an integer, or a reference to a player value |
| `expect` | The function's first bytes. REQUIRED where the runtime patches bytes; a recompilation MAY check it against the original code it was made from |

Overrides are restored when the cheat is switched off, as patches are.

### 7.6 Values the player sets

```toml
  [cheat.values.count]
  type = "u32"
  min = 0
  max = 9999999
  default = 999999
  step = 1000
  unit = "coins"

  [[cheat.write]]
  at = { region = "heap", offset = 0x1200 }
  type = "u32"
  value = "$count"
```

| Key | Meaning |
|---|---|
| `type` | As for writes, or `bool` (written as 1 or 0) |
| `min`, `max`, `step` | The range a player can choose from, and the step of a stepper |
| `default` | REQUIRED. The value until the player changes it |
| `unit` | Shown after the number |
| `display` | `"dec"` (default) or `"hex"` |
| `choices` | A list of named values instead of a number: the name of a shared list in the top-level `choices` table, or an inline table |
| `flags` | Bit flags shown as switches: an inline table from bit number (as a string) to name |

```toml
[choices.items]
"1" = "Potion"
"2" = "Ether"
"17" = "Phoenix feather"
```

A runtime keeps the values a player set, per game, and uses them again at the next start.

### 7.7 Monitors

A monitor reads values and shows them, for example on a handheld's second screen.

```toml
[[cheat]]
id = "hp-view"
name = "Health"
run = "monitor"

  [[cheat.read]]
  at = { region = "heap", offset = 0x1300 }
  type = "u16"
  label = "HP"
```

### 7.8 Relations and tags

| Key | Meaning |
|---|---|
| `requires` | Ids of cheats this one needs. Switching it on switches them on; a master cheat (section 7.1) is switched on and off with the cheats that need it |
| `excludes` | Ids of cheats that cannot be on together with this one. Switching it on switches them off |
| `radio` | A name shared by cheats of which at most one can be on, such as `"speed"` for x2, x4 and x8 |

Tags tell a runtime and a player what to expect. Unknown tags are shown as they are. Defined tags:

| Tag | Meaning, and what a runtime SHOULD do |
|---|---|
| `touches_save` | The cheat changes what the game saves. Back up the save before the cheat is first switched on for that game |
| `may_crash` | The cheat can crash the game in some situations. Say so before it is switched on |
| `cosmetic` | The cheat changes only what is shown |
| `online_unsafe` | The cheat must not be used online |

### 7.9 Conditions

Conditions make a cheat act only when the game is in a certain state: heal when health is low, refill only in a menu, freeze a timer only in a race.

```toml
  [[cheat.when]]
  at = { region = "heap", offset = 0x1300 }
  type = "u16"
  op = "<"
  value = 10
```

| Key | Meaning |
|---|---|
| `at`, `type` | The value to test, read as a write's place is (section 7.4) |
| `op` | `==`, `!=`, `<`, `<=`, `>`, `>=`; `&` (the two share a set bit), `!&` (they share none); `between` (`value` is `[low, high]`, both included); `changed` (differs from the last pass; no `value`) |
| `value` | A number, a player value (`"$limit"`), or another place to compare with: `{ at = { ... }, type = "u16" }` |

Keys on the cheat:

| Key | Meaning |
|---|---|
| `match` | `"all"` (default): every condition must hold. `"any"`: one is enough |
| `trigger` | `"while"` (default): the cheat acts in every pass in which the conditions hold. `"enter"`: it acts once each time the conditions start to hold, that is, they did not hold in the pass before |
| `interval` | Test the conditions every this many passes instead of every pass. Default 1 |

Conditions gate everything a cheat does. Writes happen only while they hold. Patches and overrides are applied while they hold and restored when they stop holding; the runtime changes code only when the result flips, never in a pass where it stays the same.

So that conditions cost next to nothing, a runtime SHOULD:

- read each place at most once per pass, even when several conditions or cheats read it, and resolve each pointer path at most once per pass;
- stop testing a cheat's conditions as soon as the result is known;
- test all conditions before it stops any of the game's threads, and stop them only to change code;
- with `interval`, do nothing for that cheat in the passes in between.

## 8. Presets

A preset switches several cheats on at once, with values.

```toml
[[preset]]
id = "relaxed"
name = "Relaxed play"
cheats = ["money", "hp-full"]
values = { "money.count" = 5000 }
```

Switching a preset on switches the listed cheats on and sets the listed values; `values` keys are a cheat id and a value id joined by a dot. A runtime MAY show which preset matches the cheats that are on.

## 9. Translations

Plain strings are in the file's `language`. Translations of the file's strings live in `i18n`, by BCP 47 tag:

```toml
[i18n.de]
name = "Example-Quest-Cheats"
groups = { "Items" = "Gegenstände" }

[i18n.de.cheat.money]
name = "Geld bleibt gleich"
about = "Beim Einkaufen bleibt dein Geld, wo es ist."
```

`i18n.<tag>.cheat.<id>` MAY hold `name`, `about` and, under `values.<id>`, `unit`. `i18n.<tag>.choices.<list>` translates a choice list. A runtime uses the translation for the player's language and falls back to the plain strings.

## 10. Extensions

Everything beyond the core is an extension, named with its version: `arm64-imm/1`.

```toml
extensions = { optional = ["arm64-imm/1"] }
```

- A file lists every extension it uses. `required`: the file's author does not want the file used without it. `optional`: the file works without it, with the cheats that use it off.
- A reader that lacks a required extension MUST NOT apply any cheat of the file and says so (`extension-missing`). A reader that lacks an optional one turns off only the cheats that use it.
- Keys of an extension are written where the extension's section says. A key that neither the core nor a listed extension defines is an error in that cheat: the reader turns the cheat off with `unknown-key` and keeps the others. An unknown key outside a cheat is a warning.
- New extensions are added to this spec by a pull request; until then their names start with `x-` and belong to whoever made them.

### 10.1 `raw/1`: code of another cheat format

Keeps a cheat in the code of the format it came from, so a converter never loses a cheat. A runtime that runs that format applies it for the given build.

```toml
  [[cheat.raw]]
  syntax = "atmosphere"
  build = "1.0.2"
  code = """
04000000 0120A4B0 52800029
"""
```

Defined syntaxes: `atmosphere` (the Atmosphère dmnt cheat code of the Switch). A cheat MAY have both `raw` entries and core operations; a runtime that can run the core operations SHOULD use them and ignore `raw`.

### 10.2 `arm64-imm/1`: a player value inside an ARM64 instruction

Lets a patch write an instruction whose immediate is a player value, so that a code cheat gets a number field.

```toml
  [[cheat.patch]]
  at = { module = "main", find = "08 00 80 52 ?? ?? ?? 94" }
  expect = "08 00 80 52"
  encode = { insn = "movz", width = 32, rd = 8, value = "$count" }
```

`encode` replaces `bytes`. Defined instructions: `movz` (an unsigned 16-bit value), `movz-movk` (a 32-bit value over two instructions; `expect` is then 8 bytes), and `fmov` (one of the floating point values that `fmov` can encode). A value the instruction cannot hold is refused with `value-out-of-range`.

## 11. Runtimes

### 11.1 Capabilities

| Capability | Needed by |
|---|---|
| `guest-read` | pointer paths, conditions, monitors |
| `guest-write` | writes |
| `code-bytes` | patches; overrides where the original code runs |
| `code-hook` | overrides in a recompilation |
| `buttons` | `hold`, `toggle`, `press` |
| `display` | monitors |

A cheat that needs a capability the runtime lacks is off with `capability-missing`.

### 11.2 Applying a file

1. Check the signature, if there is one (section 12). An invalid signature stops here.
2. Check `format` and the required extensions.
3. Identify the build (section 5).
4. For each cheat: resolve its locations, check `expect`, and give it a state.
5. On each pass: apply the cheats that are on, in file order.

States: `off`, `on`, `waiting` (for a button, or for a pointer path to resolve), `refused` with a reason (Appendix B). A runtime SHOULD show the reason to the player.

### 11.3 Writing and restoring

- The first time a patch or an override changes a place, the runtime keeps the original bytes. When the cheat is switched off, refused, or gone after the file is loaded again, the runtime puts them back, unless another cheat that is on writes the same place.
- A runtime that must stop the game's threads to change code SHOULD change everything of one pass in one stop, and SHOULD compare before writing, so that a cheat that stays on costs no stop.
- A runtime that changes the original code for its own reasons (a rewriter) refuses cheats that write over what it changed, with `runtime:<reason>`.

### 11.4 Keeping the player safe

- A runtime SHOULD back up the save before a cheat tagged `touches_save` is first switched on for a game, and offer to restore it.
- If the game ends abnormally soon after a cheat was switched on, a runtime SHOULD ask at the next start whether that cheat stays off.
- A runtime MUST NOT write outside guest memory and the game's own code, whatever a file says.

## 12. Signatures and trust

A file MAY end with one or more signature lines:

```toml
# mncf-signature: ed25519 3f2a9c4e8b1d7a60 kT9v...base64...==
```

- Each line has the algorithm (`ed25519`), the key id, and the Ed25519 signature in standard base64.
- The **key id** is the first 16 hexadecimal digits of the SHA-256 hash of the 32-byte public key.
- A signature covers every byte of the file before the first signature line, exactly as stored.
- Signature lines are TOML comments, so a reader that does not check signatures still reads the file.

A reader checks each signature against the keys it trusts: keys built into the runtime, and keys the player chose to trust. The result for the file:

| Result | Meaning |
|---|---|
| `verified` | A trusted key signed it. The runtime shows who, by the name its trust list gives the key |
| `unsigned` | No signature. Loaded and applied like any file, without a warning or a mark |
| `unknown-key` | Signed, by a key the reader does not trust. The same as `unsigned` |
| `invalid` | A signature does not match the bytes: the file was changed after signing. The reader MUST NOT apply the file |
| `revoked` | Signed by a key on the revocation list. The reader MUST NOT apply the file |

A signature is optional. It is a mark of origin for files someone wants to vouch for, such as a curated pack; a runtime MUST NOT warn about, hide or treat differently a file only because it is unsigned. Only `verified` gets a mark in the runtime's interface.

Mistel publishes its public keys and its revocation list in `docs/keys.md`. A signature says who made a file and that it is unchanged. It does not say that the cheats are harmless; the checks of section 11 do that.

## 13. Packs

A pack bundles several `.mncf` files, for one game or many. It is a ZIP file with the extension **`.mncp`** (Mistel Native Cheat Pack), the same way an `.apk` or a `.docx` is a ZIP: the extension lets a phone or a desktop open the pack in the right program instead of an archive app. Its manifest, `pack.toml`, is at the root:

```toml
format = "mncf-pack/1"
name = "Example pack"
authors = ["someone"]
file_version = "2"

[[file]]
path = "example-quest.mncf"
sha256 = "9f86d081884c7d659a2feaa0c55ad015a3bf4f1b2b0b822cd15d6c15b0f00a08"
```

The manifest is signed as a file is (section 12). A reader checks each listed file's hash and MUST NOT apply a file whose hash differs. Files in a pack MAY also carry their own signatures.

## 14. Versions

- The version line names the major version. Within `mncf/1`, a newer reader reads every older file.
- New features come as extensions (section 10), never as new meanings of existing keys.
- A change that breaks a rule of this section is `mncf/2`.

## 15. Converting other formats

A converter (`mistel convert`) keeps every cheat of the file it reads. For an Atmosphère cheat file:

| Atmosphère | `.mncf` |
|---|---|
| The file name, `<build ID>.txt` | A build with `modules = { main = "<build ID>" }` |
| `[Name]` with code | A cheat with a `raw` entry (`raw/1`) for that build, plus core operations where the code allows (below) |
| `[Name]` without code | A group: the cheats after it get `group = "Name"` |
| `{Name}` | A master cheat that the other cheats require |
| An entry that only restores code | Dropped: runtimes restore code themselves (section 11.3) |
| Credits, links | `authors`, `about` |
| A key mask (code type 8) | `hold` |

Where the code is only plain writes, the converter writes core operations: a write into a module's code becomes a patch. With the game's code module at hand, the converter fills `expect` from it and replaces fixed offsets by patterns, so that a cheat made for one build works on every build where its place can be found. Without the module, it keeps fixed offsets per build and the `raw` entry.

Converters for the cheat and patch files of other emulators and for PC cheat tables follow the same rule: keep every cheat, and write core operations where the meaning is clear.

## Appendix A. Platforms

New platforms are added by a pull request to this table.

| `platform` | Game `id` | Architecture, byte order, pointer size | Modules and their identity | Regions | Buttons |
|---|---|---|---|---|---|
| `switch` | Title ID, 16 hexadecimal digits | ARM64, little-endian, 8 | `main`, `sdk`, `subsdk0` to `subsdk9`; the build ID in hexadecimal | `heap`, `alias`, `aslr` | `A B X Y L R ZL ZR PLUS MINUS LSTICK RSTICK DUP DDOWN DLEFT DRIGHT` |
| `switch2` | As `switch` | As `switch` | As `switch` | As `switch` | As `switch`, plus `C` |
| `n64` | The ROM header's game code, 4 characters | MIPS, big-endian, 4 | `rom`; `sha256:` of the ROM | Absolute addresses (`address`) in RDRAM | `A B Z START L R CUP CDOWN CLEFT CRIGHT DUP DDOWN DLEFT DRIGHT` |
| `pc` | `<store>:<id>`, for example `steam:123456` | x86-64 or ARM64, little-endian, 8 | The executable's and libraries' file names; `sha256:` of the file | None: data is reached by pointer paths from a module | Keyboard key names (`F1`, `CTRL+F1`) and gamepad names (`PAD_A`, `PAD_LB`) |

## Appendix B. Reasons

| Reason | Meaning |
|---|---|
| `build-unknown` | The cheat needs a build (per-build offsets, `raw`), and the running build is not in the file |
| `module-unknown`, `region-unknown` | The runtime has no module or region of that name |
| `pattern-not-found`, `pattern-ambiguous` | Section 6.3 |
| `expect-mismatch` | The original bytes are not there |
| `path-broken` | A pointer path does not resolve at the moment; the cheat waits |
| `capability-missing:<capability>` | Section 11.1 |
| `extension-missing:<extension>` | Section 10 |
| `unknown-key:<key>` | Section 10 |
| `value-out-of-range` | A player value does not fit where it is written |
| `requires:<id>`, `excludes:<id>` | A relation stops the cheat |
| `signature` | The file's signature is invalid or revoked |
| `runtime:<reason>` | A reason of the runtime's own, such as code it rewrote |

## Appendix C. A complete example

Every ID, build ID, address and byte below is made up.

```toml
format = "mncf/1"
name = "Example Quest cheats"
authors = ["someone"]
file_version = "3"

[game]
platform = "switch"
id = "0100000000ABC000"
name = "Example Quest"

[builds."1.0.2"]
modules = { main = "A1B2C3D4E5F60718" }

[builds."1.0.3"]
modules = { main = "1122334455667788" }

[choices.items]
"1" = "Potion"
"2" = "Ether"
"17" = "Phoenix feather"

[[cheat]]
id = "money"
name = "Money never goes down"
group = "Items"
tags = ["touches_save"]

  [[cheat.patch]]
  at = { module = "main", find = "68 12 00 B9 ?? ?? ?? 94" }
  expect = "68 12 00 B9"
  bytes = "69 12 00 B9"

[[cheat]]
id = "first-slot"
name = "First bag slot"
group = "Items"
run = "once"
tags = ["touches_save"]

  [cheat.values.item]
  type = "u16"
  choices = "items"
  default = 1

  [cheat.values.count]
  type = "u16"
  min = 1
  max = 99
  default = 99

  [[cheat.write]]
  at = { module = "main", offsets = { "1.0.2" = 0x3BC2000, "1.0.3" = 0x3BC2080 }, path = [0x18] }
  type = "u16"
  value = "$item"

  [[cheat.write]]
  at = { module = "main", offsets = { "1.0.2" = 0x3BC2000, "1.0.3" = 0x3BC2080 }, path = [0x1A] }
  type = "u16"
  value = "$count"

[[cheat]]
id = "no-damage"
name = "Take no damage"
group = "Battle"

  [[cheat.override]]
  function = { module = "main", find = "FD 7B BF A9 FD 03 00 91 ?? ?? ?? 94" }
  action = "skip"
  expect = "FD 7B BF A9"

[[cheat]]
id = "heal-low"
name = "Heal when low"
group = "Battle"
trigger = "enter"

  [[cheat.when]]
  at = { region = "heap", offset = 0x1300 }
  type = "u16"
  op = "<"
  value = 10

  [[cheat.write]]
  at = { region = "heap", offset = 0x1300 }
  type = "u16"
  value = 100

[[cheat]]
id = "speed-x2"
name = "Run twice as fast"
group = "Moving"
radio = "speed"
hold = "ZL"

  [[cheat.patch]]
  at = { module = "main", find = "00 10 2E 1E ?? ?? ?? 1E" }
  expect = "00 10 2E 1E"
  bytes = "00 10 20 1E"

[[cheat]]
id = "hp-view"
name = "Health"
group = "Battle"
run = "monitor"

  [[cheat.read]]
  at = { region = "heap", offset = 0x1300 }
  type = "u16"
  label = "HP"

[[preset]]
id = "relaxed"
name = "Relaxed play"
cheats = ["money", "no-damage"]

[i18n.de.cheat.money]
name = "Geld bleibt gleich"
```
