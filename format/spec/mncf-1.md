# The Mistel Native Cheat Format, version 1

Version line: `format = "mncf/1"`. File extension: `.mncf`. Status: **draft**. The libraries in [`../lib/`](../lib/) and the conformance suite in [`../tests/`](../tests/) follow it. Until it is marked final, the format can still change.

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
- A file is [TOML 1.0](https://toml.io/en/v1.0.0), UTF-8 without a byte order mark, with LF line ends. `format` MUST be the first key of the file. The format has no date or time values, so a file holds none of TOML's date-times, and its tables and arrays nest at most 128 levels deep (the file itself is level 0). A reader refuses a file that breaks one of these rules (Appendix D).
- Integers are TOML integers; addresses and offsets are usually written in hexadecimal (`0x3A1C2D0`). A value above `0x7FFFFFFFFFFFFFFF` does not fit a TOML integer and is written as a string of `0x` and 1 to 16 hexadecimal digits (`"0xFFFFFFFF80000000"`). That form is allowed wherever an address, an offset, a path step or an integer value is.
- **Bytes** are written as a string of hexadecimal pairs separated by single spaces: `"68 12 00 B9"`. In a pattern, `??` stands for any byte. Hexadecimal digits may be upper or lower case, here and in module identities.
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
| `id` | string | no | The identity of the file across all its versions, section 17 |
| `authors` | array of strings | no | Who made the cheats |
| `about` | string | no | A description of the file, credits |
| `file_version` | string | no | The authors' own version of this file, so a runtime can say that a newer one was imported. With `[update]` or `[[changes]]`, numbers separated by dots (section 17.1) |
| `language` | string | no | The language of the file's plain strings, a BCP 47 tag. Default `"en"` |
| `origin` | inline table | no | Where the file's cheats came from when a program converted them: `format`, the source's format (`"atmosphere"`, `"ips"` for IPS and IPS32, `"pchtxt"` for IPSwitch text, or a name starting with `x-`; for several sources, the first one's); `name`, the name of the source as the player had it, such as a ZIP file or a folder; and `site`, the website the source names, as a domain (`"example.org"`). For people and for support; a reader does not act on it |
| `extensions` | table | no | `required` and `optional`, arrays of extension names with their version (`"arm64-imm/1"`), section 10 |
| `signers` | array of strings | no | The public keys that sign the file, section 12 |
| `update` | table | no | Where the newest version of the file is, section 17 |
| `changes` | array of tables | no | What each version of the file changed, section 17 |
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

- `game` needs `platform` and `id`; `name` is for people and optional.
- `builds` maps build keys to the identity of that version. `modules` is required and maps module names of the platform to their identity, in the form the platform gives (Appendix A): for the Switch, the code module's build ID in hexadecimal; for most other platforms, `sha256:` followed by the hash of the module file.
- A runtime identifies the build that runs by comparing the identities of the modules that run with each build's `modules`, without regard to case. A build matches when every module it lists matches; the first build in file order that matches is the one that runs. A Switch build ID matches when the shorter of the two hexadecimal strings is a prefix of the longer one.
- A recompilation identifies the build by the original game version it was made from: it names the build key itself. It MUST NOT compare the identities of its own recompiled modules.
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

Offsets from a module or a region start (`offset` without `find`, and the values of `offsets`) are not negative. The `offset` after a pattern may be negative. `offsets` names only build keys of the file. `section` is only for patterns.

Any base MAY be followed by a pointer path, `path`: a non-empty array of offsets, which may be negative. The runtime reads a pointer at the base, adds the first offset, reads a pointer there, adds the next offset, and so on; the last offset is added without a read. The pointer size and byte order are the platform's (Appendix A).

A place MUST lie inside its module or region, with all the bytes the operation reads or writes. A place outside them turns the cheat off with `outside`. After a pointer path, the place MUST lie in guest memory; where it does not, the path does not resolve.

A fixed offset in a region:

```toml
at = { region = "heap", offset = 0x1200 }
```

A pointer path from an offset in `main` that differs per build:

```toml
at = { module = "main", offsets = { "1.0.2" = 0x3A1C2D0, "1.0.3" = 0x3A1C350 }, path = [0x130, 0x10] }
```

The place 4 bytes after a pattern in `main`'s code:

```toml
at = { module = "main", find = "E8 03 00 B9 ?? ?? ?? 94", offset = 4, section = "code" }
```

A pointer path that meets a null pointer, or an address outside guest memory, does not resolve at that moment. The runtime tries again at the next pass; the cheat waits (Appendix B, `path-broken`).

### 6.3 Patterns

- A pattern is bytes with `??` for any byte. It MUST be at least 4 bytes long and MUST NOT start or end with `??`.
- Without `section`, a pattern is searched in the module's code and read-only data. `section = "data"` searches its data as the module file gives it, before the game changed it. A match lies inside one section.
- A pattern MUST match exactly once in the searched module (or section). Every byte offset counts, so matches may overlap: `"AA AA AA AA"` matches twice in five bytes `AA`. No match turns the cheat off with `pattern-not-found`, more than one with `pattern-ambiguous`.
- A runtime searches the original bytes. Where it has changed bytes itself (section 11.3), it searches with the original bytes it kept.
- A runtime searches each pattern once per build and MAY keep the result for later runs of the same build.

## 7. Cheats

### 7.1 Identity and meaning

| Key | Type | Required | Meaning |
|---|---|---|---|
| `id` | id | yes | Unique among the file's cheats |
| `name` | string | yes | Shown to the player |
| `about` | string | no | What the cheat does, in a sentence or two |
| `group` | string | no | Where it is listed. `/` separates levels, none of them empty: `"Items/Potions"` |
| `tags` | array of strings | no | Section 7.8 |
| `default` | boolean | no | Whether it is on the first time the file is used. Default `false` |
| `master` | boolean | no | A cheat other cheats need, such as a setup code. It is not shown as a switch; it is on while any cheat that requires it is on |
| `run` | string | no | How it runs, section 7.2. Default `"on"` |

A cheat does its work through one or more operations: `patch` (section 7.4), `write` (7.4), `override` (7.5) and `read` (7.7), or through `raw` entries (section 10.1). Conditions (section 7.9) are not operations. A cheat with no operation is an error (`invalid`).

### 7.2 How a cheat runs

| `run` | Meaning |
|---|---|
| `"on"` | While the cheat is on, its patches and overrides stay applied and its writes repeat at every pass. Switching it off restores what its patches and overrides changed |
| `"once"` | Its writes happen once, in the first pass in which it works after it is switched on. It then switches itself off and shows as done (reason `done`); switching it on again runs it again. It has no patches or overrides, which work only while a cheat is on (`invalid:run`) |
| `"action"` | Its writes happen once each time its `press` buttons are pressed; its patches and overrides are applied from the first press until the cheat is switched off. Until the first press it waits. Needs `press`; `press` is only for this mode |
| `"timed"` | As `"on"`, for `duration` seconds (a number above 0) from its first pass, then it switches itself off and shows as done. `duration` is only for this mode |
| `"monitor"` | It only reads and shows values (section 7.7): it has `read` operations and no others. `read` is only for this mode |

A **pass** is one round of the runtime's cheat work, about once per frame the game presents. Section 11.5 says what happens in one.

### 7.3 Buttons

| Key | Meaning |
|---|---|
| `hold` | The cheat works only while these buttons are held: patches and overrides are applied while held and restored when released |
| `toggle` | Pressing these buttons together switches the cheat on or off |
| `press` | For `run = "action"`: the buttons that trigger it |

Buttons are written as names joined by `+`, each name once: `"ZL"`, `"L+DUP"`. The names are the platform's (Appendix A). A runtime SHOULD let the player change them. `toggle` does not hold a cheat back: a cheat with `toggle` that is on works at once.

A combination is **pressed** in a pass when all of its buttons are held in that pass and were not all held in the pass before, or when the runtime saw them pressed together since the last pass, for a press shorter than a pass. Other buttons held at the same time do not matter, so pressing `L+DUP` also presses `DUP`. `hold` and `press` are for a cheat's core operations: a cheat that uses its `raw` entry tests buttons in its raw code (section 10.1).

### 7.4 Patches and writes

A **patch** changes the original code, or read-only data, at one place:

```toml
  [[cheat.patch]]
  at = { module = "main", find = "68 12 00 B9 ?? ?? ?? 94" }
  expect = "68 12 00 B9"
  bytes = "69 12 00 B9"
  asm = { expect = "str w8, [x19, #0x10]", bytes = "str w9, [x19, #0x10]" }
```

- `expect` is REQUIRED and has the same length as `bytes`. Before each write, the runtime compares the bytes at the place with `expect` and, if they differ and are not `bytes` either, turns the cheat off with `expect-mismatch`. A place that already holds `bytes` is a patch that is already applied, not a mismatch. The bytes compared are the ones there when the patch is applied, or, where the runtime changed them itself, the original bytes it kept (section 11.5).
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
- `value` is a number of that type, or for `bytes` a bytes string, or a reference to a value the player sets: `"$count"` (section 7.6). A number that the type cannot hold is an error in the cheat. An integer may be written into a float type; a float, or a player value of a float type, MUST NOT be written into an integer type. A `bool` player value is written as 1 or 0. A player value that does not fit the write's type turns the cheat off with `value-out-of-range`.
- `expect` is OPTIONAL for writes: a number of the write's type, or for `bytes` a bytes string of the same length. If present, the write happens only while the place holds that value.
- Writes are not restored when the cheat is switched off: they changed the game's state, not its code.

### 7.5 Overrides

An override changes what a function does, at the level of the whole function. It works in every runtime: one that runs the original code writes a short stub at the function's entry, a recompilation replaces or wraps the recompiled function.

```toml
  [[cheat.override]]
  function = { module = "main", find = "FD 7B BF A9 FD 03 00 91 ?? ?? ?? 94" }
  action = "return"
  value = 1
  expect = "FD 7B BF A9 FD 03 00 91"
```

| Key | Meaning |
|---|---|
| `function` | A location (section 6.2) that resolves to the function's first instruction. In a recompilation, the runtime maps that original address to its recompiled function |
| `action` | `"return"`: the function returns `value` at once. `"skip"`: the function returns at once without a value |
| `value` | For `"return"`: an integer, or a reference to a player value of an integer type or `bool`. Not allowed for `"skip"` |
| `expect` | The function's first bytes. REQUIRED where the runtime patches bytes; a recompilation MAY check it against the original code it was made from |

A runtime that runs the original code writes the stub that Appendix A gives for its architecture, and only where `expect` is present: the stub is checked against `expect` as a patch is, and `expect` MUST be at least as long as the stub. With a longer stub, the cheat is off with `invalid:override.expect`, or with `value-out-of-range` when a player value made it longer. Where it cannot write a stub (no `expect`, or no stub for its architecture), the override needs `code-hook`.

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
| `type` | One of the number types of writes, or `bool` (written as 1 or 0) |
| `min`, `max`, `step` | The range a player can choose from, and the step of a stepper. Numbers of the value's type, `min` not above `max`, `step` above 0. Not for `bool` |
| `default` | REQUIRED. The value until the player changes it: of the value's type, inside `min` and `max`, and one of the `choices` where there are choices |
| `unit` | Shown after the number |
| `display` | `"dec"` (default) or `"hex"` |
| `choices` | A list of named values instead of a number: the name of a shared list in the top-level `choices` table, or an inline table. Only for integer types |
| `flags` | Bit flags shown as switches: an inline table from bit number (as a string) to name. Only for integer types, with bit numbers below the type's width |

A value id is an id (section 2). Keys of a choice list are whole numbers written as strings: an optional `-`, then 1 to 20 decimal digits, or `0x` and 1 to 16 hexadecimal digits. Nothing else, not even a space.

```toml
[choices.items]
"1" = "Potion"
"2" = "Ether"
"17" = "Phoenix feather"
```

A runtime keeps the values a player set, per game, and uses them again at the next start. A value set while the game runs is used from the next pass on (section 11.5).

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

`requires` and `excludes` name other cheats of the file. A cheat that requires a refused cheat is refused with `requires:<id>`. A cheat that can never be on, because the cheats it requires, with itself, exclude each other or share a radio, is refused with `excludes:<id>`, naming one of them other than itself.

Exclusion works both ways: two cheats cannot be on together when either excludes the other. Switching a cheat on also switches off every cheat that requires a cheat it switched off.

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
| `at`, `type` | The value to test, read as a write's place is (section 7.4). `type` is a number type |
| `op` | `==`, `!=`, `<`, `<=`, `>`, `>=`; `&` (the two share a set bit), `!&` (they share none), for integer types only; `between` (`value` is `[low, high]`, two numbers of the type, `low` not above `high`, both included); `changed` (the bytes at the place differ from the ones read at the cheat's test before; at its first test after it is switched on, it does not hold; no `value`) |
| `value` | A number of the type, a player value (`"$limit"`), or another place to compare with: `{ at = { ... }, type = "u16" }`. For `&` and `!&`, a player value is of an integer type or `bool` (`invalid:when.value`) and another place has an integer type (`invalid:when.value.type`) |

Numbers compare by their value, whatever their types: a `u8` of 3 is below an `f32` of 3.5. A `bool` player value counts as 1 or 0. A float that is not a number is unequal to everything and neither below nor above anything.

Keys on the cheat:

| Key | Meaning |
|---|---|
| `match` | `"all"` (default): every condition must hold. `"any"`: one is enough |
| `trigger` | `"while"` (default): the cheat acts in every pass in which the conditions hold. `"enter"`: it acts once each time the conditions start to hold, that is, they did not hold at its test before, or this is its first test after it was switched on |
| `interval` | Test the conditions, and work, only in every this many passes instead of every pass, counted from the cheat's first pass after it is switched on: a whole number of at least 1. Default 1. In the passes in between, nothing of the cheat changes. It works the same for a cheat without conditions |

Conditions gate everything a cheat does. Writes happen only while they hold, and `trigger` says in which of those passes. Patches and overrides are applied while they hold and restored when they stop holding, whatever `trigger` says; the runtime changes code only when the result flips, never in a pass where it stays the same. A cheat whose conditions do not hold is still on.

So that conditions cost next to nothing, a runtime SHOULD:

- read each place at most once per pass, even when several conditions or cheats read it, and resolve each pointer path at most once per pass;
- stop testing a cheat's conditions as soon as the result is known, but still read the place of each `changed` condition, which its next test compares with;
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

A preset that names a cheat or a value the file does not have, or a value that does not fit, is dropped with a warning (`preset:<id>`); the rest of the file is used.


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

`i18n.<tag>.cheat.<id>` MAY hold `name`, `about` and, under `values.<id>`, `unit`. `i18n.<tag>.choices.<list>` translates a choice list, and `i18n.<tag>.changes."<version>"` MAY hold the `text` of a change entry (section 17.1). A runtime uses the translation for the player's language and falls back to the plain strings.

## 10. Extensions

Everything beyond the core is an extension, named with its version: `arm64-imm/1`.

```toml
extensions = { optional = ["arm64-imm/1"] }
```

- A file lists every extension it uses. `required`: the file's author does not want the file used without it. `optional`: the file works without it, with the cheats that use it off.
- A reader that lacks a required extension MUST NOT apply any cheat of the file and says so (`extension-missing`). A reader that lacks an optional one turns off only the cheats that use it.
- Keys of an extension are written where the extension's section says. A key that neither the core nor a listed extension defines is an error in that cheat: the reader turns the cheat off with `unknown-key` and keeps the others. An unknown key outside a cheat is a warning. A key of an extension the file does not list is an unknown key.
- A reader cannot tell the keys of an extension it lacks from mistakes. So when the file lists an extension the reader lacks, a key in a cheat that the reader does not know turns that cheat off with `extension-missing:<name>`, naming the first such extension of the file's lists, `required` before `optional`.
- A runtime that lacks an optional extension the reader knows turns off the cheats that use it with `extension-missing:<name>`.
- New extensions are added to this spec by a pull request; until then their names start with `x-` and belong to whoever made them. An extension name is lower-case letters, digits and `-`, a `/` and its version number.

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

Defined syntaxes: `atmosphere` (the Atmosphère dmnt cheat code of the Switch, words of 8 hexadecimal digits separated by white space). `build` is a build key of the file. A runtime supports `raw/1` for the syntaxes it runs; for another syntax it is as if it lacked `raw/1`.

A cheat MAY have both `raw` entries and core operations. A runtime that can run the core operations uses them and ignores `raw`. It uses a `raw` entry only for the build that runs, and only when the cheat has no core operations, or when the runtime lacks a capability or an extension that the core operations need. A cheat whose `raw` entries are all for other builds is off with `build-unknown`.

A cheat that uses its `raw` entry runs it in every pass while it is on, as the format it came from runs it (section 11.5). The raw code tests buttons and memory itself, so `hold`, `press`, `when` and `interval` are for core operations only and do not hold such a cheat back; `run = "once"` runs the entry in one pass, and `run = "timed"` for its duration.

### 10.2 `arm64-imm/1`: a player value inside an ARM64 instruction

Lets a patch write an instruction whose immediate is a player value, so that a code cheat gets a number field.

```toml
  [[cheat.patch]]
  at = { module = "main", find = "08 00 80 52 ?? ?? ?? 94" }
  expect = "08 00 80 52"
  encode = { insn = "movz", width = 32, rd = 8, value = "$count" }
```

`encode` replaces `bytes`, and `expect` is as long as the instructions. Keys: `insn`, `width` (32 or 64: a W or X register for `movz` and `movz-movk`, an S or D register for `fmov`), `rd` (the register number, 0 to 31) and `value` (a number or a player value). Defined instructions:

| `insn` | Bytes | Instructions |
|---|---|---|
| `movz` | 4 | `movz rd, #value`: an unsigned 16-bit value |
| `movz-movk` | 8 | `movz rd, #(value & 0xFFFF)` and `movk rd, #(value >> 16), lsl #16`: an unsigned 32-bit value |
| `fmov` | 4 | `fmov rd, #value`: a value of the form ±(16 + m) / 16 × 2^e, with m from 0 to 15 and e from -3 to 4 |

A static value the instruction cannot hold is an error in the cheat (`invalid:patch.encode.value`); a player value it cannot hold turns the cheat off with `value-out-of-range`.

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

A cheat that needs a capability the runtime lacks is off with `capability-missing`, naming the first missing one in the order of this table. A pointer path in any location and every condition need `guest-read`.

Capabilities are what a runtime's platform allows. The rest of the core is not optional: a conforming runtime has all of it, and the reference engines of the libraries do the work of a pass (section 11.5). A runtime that does not have a part of it yet refuses the cheats that use it with `runtime:unsupported:<key>`, naming the first such part in this order: `run.once`, `run.action`, `run.timed`, `run.monitor`, `hold`, `toggle`, `press`, `when` (any condition), `when.many` (more than one), `match.any`, `trigger.enter`, `interval`, `override`.

### 11.2 Applying a file

1. Check the signature, if there is one (section 12). An invalid signature stops here.
2. Check `format` and the required extensions.
3. Identify the build (section 5).
4. For each cheat: resolve its locations, check `expect`, and give it a state.
5. On each pass: apply the cheats that are on, in file order (section 11.5).

States: `off`, `on`, `waiting` (for a button, or for a pointer path to resolve), `refused` with a reason (Appendix B). A runtime SHOULD show the reason to the player.

A cheat is applied whole or not at all: if one of its locations does not resolve or one `expect` differs, the whole cheat is refused. The reason is the first problem found in this order: the file (`game-mismatch`, `signature`, a missing required extension), the cheat's own text (unknown keys before broken values), capabilities and extensions, then each operation in file order (its location, its value, its `expect`). A pointer path that does not resolve does not refuse the cheat: a cheat that is on waits with `path-broken`.

When a file is loaded, the cheats that are on are the ones with `default = true` and the ones the player switched on, without the ones the player switched off, switched on one after the other in file order as section 7.8 says, so that a later one wins over an earlier one it excludes. A cheat that is on and has `hold` waits until its buttons are held, and one that runs as an action waits for its first press. A cheat that uses its `raw` entry does not wait for buttons (section 10.1).


### 11.3 Writing and restoring

- The first time a patch or an override changes a place, the runtime keeps the original bytes. When the cheat is switched off, refused, waits, its conditions stop holding, or it is gone after the file is loaded again, the runtime puts them back, unless another cheat whose patch is applied writes the same place.
- A runtime that must stop the game's threads to change code SHOULD change everything of one pass in one stop, and SHOULD compare before writing, so that a cheat that stays on costs no stop.
- A runtime that changes the original code for its own reasons (a rewriter) refuses cheats whose patches or override stubs write over what it changed, with `runtime:rewritten`.

### 11.4 Keeping the player safe

- A runtime SHOULD back up the save before a cheat tagged `touches_save` is first switched on for a game, and offer to restore it.
- If the game ends abnormally soon after a cheat was switched on, a runtime SHOULD ask at the next start whether that cheat stays off.
- A runtime MUST NOT write outside guest memory and the game's own code, whatever a file says.

### 11.5 A pass

Between two passes the player switches cheats on and off (section 7.8), sets values (section 7.6), and holds and presses buttons. A value that does not fit refuses its cheat with `value-out-of-range`; once a value that fits is set, the cheat can be used again and is off. Each pass then goes through these steps, each for all cheats in file order before the next step:

1. **Buttons.** The runtime finds the combinations of the file's `toggle` and `press` keys that were pressed (section 7.3).
2. **Toggles.** Each cheat whose `toggle` was pressed is switched on or off. A master cheat and a refused cheat are not switched.
3. **Time.** A `timed` cheat's time starts at its first pass after it is switched on. In the first pass that is at least `duration` seconds later, it is switched off and shows as done, before it works. The time of a pass is the one the runtime gives, in seconds; a runtime that gives none counts passes: the time of a pass is then the number of passes before it divided by the runtime's number of passes per second. A runtime gives the time at every pass or at none.
4. **Work.** Each cheat that is on:
   - If it uses its `raw` entry, it runs the entry in this pass, and nothing else of this step applies (section 10.1).
   - With `interval = n`, it does the rest of this step only in its first pass after it is switched on and in every n-th pass after it. In the passes in between, nothing of it changes.
   - It waits, with no reason, while not all of its `hold` buttons are held, and, if it runs as an action, until its first press.
   - It waits with `path-broken` while a pointer path of one of its places does not resolve (section 6.2), or a place it reads cannot be read.
   - Otherwise it tests all of its conditions; without conditions they hold. Every cheat reads guest memory as it is at the start of the pass, before any change of the pass.
   - It **works** when the conditions hold, with `trigger = "enter"` only when they did not hold at its test before, and as an action only in a pass in which its `press` buttons were pressed. Working means: each of its writes is made, where the write has an `expect` only while its place holds that value; a monitor reads its values. From the first pass in which an action works, it counts as pressed until it is switched off.
   - Its patches and overrides are **applied** when the conditions hold and, for an action, once it was pressed. A cheat that waits has nothing applied.
5. **Expect.** For each applied patch, and each applied override that the runtime writes as a stub, that covers a place it did not cover in the pass before, the runtime compares the place with `expect` (section 7.4): where it changed bytes there itself, the original bytes it kept, elsewhere the bytes there now. Where the place holds the patch's bytes already, they are not written again, and `expect` is kept as the original. Where it holds anything else, the cheat is refused with `expect-mismatch` (with `outside` where the place cannot be read) and switched off with what that switches off (section 7.8); nothing of those cheats happens in this pass.
6. **Code.** Each code byte that an applied patch or stub covers gets the byte of the last such cheat in file order; each other byte the runtime changed gets its original back. A patch or stub is written whole when one of its bytes is not what it should be. The bytes that no applied patch or stub covers any more are put back whole, in runs of consecutive bytes, except a run whose bytes all hold their originals already. An override that the runtime hooks (`code-hook`) is hooked while it is applied, hooked again when the value it returns changes, and unhooked when it is no longer applied.
7. **Once.** A `once` cheat that worked in this pass switches itself off and shows as done. What that switches off is put back in the next pass.

The changes of a pass come as one batch, in this order:

| Change | Keys | Meaning |
|---|---|---|
| `patch` | address, bytes | Write these bytes into code at the address |
| `restore` | address, bytes | Put these original bytes back into code at the address |
| `hook` | address, action, value | Make the function at the address do the action, returning the value for `"return"` |
| `unhook` | address | Let the function at the address run as it was |
| `write` | cheat, address, bytes | Write these bytes into guest memory |
| `raw` | cheat, syntax, build, code | Run this raw entry once |

The code changes come first, in the order of their addresses, a byte change before a hook at the same address; then the writes and raw entries, in file order of their cheats and operations. A runtime that stops the game's threads for code changes makes all code changes of a batch in one stop. After a pass, each cheat has a state (section 11.2), and each monitor that worked has its values, in the order of its `read` operations.

A runtime that stops, or loads the file again, first puts back every place it changed and unhooks every function it hooked.

A runtime SHOULD make its first pass after the game's modules are loaded and before the game's code first runs, so that a patch of code that runs only at the start, such as one a converted patch file brings (section 15), works as it did in the loader. Where it cannot, such a patch works from its first pass on.

## 12. Signatures and trust

A file MAY end with one or more signature lines:

```toml
# mncf-signature: ed25519 3f2a9c4e8b1d7a60 kT9v...base64...==
```

- Each line has the algorithm (`ed25519`), the key id, and the Ed25519 signature in standard base64, separated by single spaces: `# mncf-signature: `, `ed25519`, a space, the key id, a space, 88 characters of base64 ending in `==`, and a line end.
- The **key id** is the first 16 hexadecimal digits of the SHA-256 hash of the 32-byte public key, in lower case.
- A signature line starts at the beginning of a line. A signature covers every byte of the file before the first signature line, exactly as stored. After the first signature line, only signature lines follow; anything else after it makes the file `invalid`, because those bytes are not covered.
- Signature lines are TOML comments, so a reader that does not check signatures still reads the file.
- The signature is Ed25519 as RFC 8032 defines it. A reader refuses a signature whose S is not below the group order, and a public key or R that is not the canonical encoding of a point on the curve, and checks [S]B = R + [k]A.

A reader checks each signature against the keys it trusts: keys built into the runtime, and keys the player chose to trust. The result for the file:

| Result | Meaning |
|---|---|
| `verified` | A trusted key signed it. The runtime shows who, by the name its trust list gives the key |
| `unsigned` | No signature. Loaded and applied like any file, without a warning or a mark |
| `unknown-key` | Signed, by a key the reader does not trust. The same as `unsigned` |
| `invalid` | A signature does not match the bytes: the file was changed after signing. The reader MUST NOT apply the file |
| `revoked` | Signed by a key on the revocation list. The reader MUST NOT apply the file |

A file with several signature lines gets one result: `invalid` if any line is invalid, else `revoked` if any key is revoked, else `verified` if any trusted key verified, else `unknown-key`. An `ed25519` line that is not well formed is invalid. A line of an algorithm the reader does not know cannot be checked and counts as `unknown-key`, so that new algorithms can be added. A signature that names a trusted key's id but does not verify with that key is invalid. The cheats of a file that is `invalid` or `revoked` are refused with `signature`.

A file MAY list the public keys that sign it in `signers`: an array of 32-byte Ed25519 public keys in standard base64, each once. A key there is not trusted. It lets a reader check a signature by a key that is not on its trust list, so that a runtime can tell that a new version of a file was signed by the same key as the one before (section 17.2). A signature line whose key id is that of a key in `signers`, and of no trusted key, is checked with that key: where it verifies, the line counts as `unknown-key`, as before; where it does not, the line is invalid, because the file was changed after signing. A `signers` key that breaks these rules is ignored, with the warning `signers` (Appendix D). Signing tools (`mistel release`) write the key into `signers` before they sign, so that it is covered by the signature.

A signature is optional. It is a mark of origin for files someone wants to vouch for, such as a curated pack; a runtime MUST NOT warn about, hide or treat differently a file only because it is unsigned. Only `verified` gets a mark in the runtime's interface.

A runtime keeps the keys it trusts and the keys it no longer trusts in a **trust list**, a TOML file of its own format:

```toml
format = "mncf-keys/1"

[[key]]
name = "Example signer"
public = "11qYAYKxCrfVS/7TyWQHOg7hcvPapiMlrwIaaPcHURo="
id = "21fe31dfa154a261"

[[revoked]]
id = "39f713d0a644253f"
```

- `format = "mncf-keys/1"` is the first key.
- Each `[[key]]` has the `name` a runtime shows, and `public`, the 32-byte public key in standard base64. `id` is optional; when it is there, it MUST be the key id of `public`.
- Each `[[revoked]]` has the `id` of a key that is no longer trusted. It is 16 lower-case hexadecimal digits.
- A trust list that breaks these rules is refused as a whole, with the codes of Appendix D (`format`, `schema:key`, `schema:revoked`).

Mistel publishes its public keys and its revocation list as such a file, `docs/keys.toml`, and explains them in `docs/keys.md`. A signature says who made a file and that it is unchanged. It does not say that the cheats are harmless; the checks of section 11 do that.

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

- `format = "mncf-pack/1"` is the first key of the manifest; `name` and at least one `[[file]]` are required; `authors`, `about`, `file_version`, `id`, `signers`, `update` and `changes` are as in a file, `id`, `update` and `changes` for the pack as a whole (section 17.6).
- `path` is a path inside the pack: parts separated by `/`, none of them empty, `.` or `..`, no `\`, not `pack.toml`, and no path twice. `sha256` is the SHA-256 hash of the file in 64 lower-case hexadecimal digits.
- A pack whose ZIP has two entries of the same name is refused. Files the manifest does not list are ignored.
- A reader reads only entries that are stored or deflated, not encrypted, and need a ZIP version of at most 6.3 to extract, so that every reader reads the same packs. An entry that breaks one of these rules, or whose data does not decompress, makes the pack `zip`.
- Each listed file is `ok`, `hash` (its bytes differ from the manifest: not applied) or `missing`. A pack whose manifest is `invalid` or `revoked` is not applied at all.

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
| `[Name]` without code, or with only `20000000` | A group: the cheats after it get `group = "Name"`. An entry with only `20000001` ends the group |
| `[--SectionStart:Name--]` and `[--SectionEnd:Name--]` (EdiZon) | The start and the end of the group `Name` |
| `{Name}` | A master cheat. The cheats that depend on it require it (below) |
| A line that is neither a name nor code | Kept in `about` as it was. An entry whose code lines mix code words with other text is left out, and named in `about` |
| An entry that only restores code | Dropped: runtimes restore code themselves (section 11.3) |
| Credits, links, the title line | `authors`, `about` |
| A key mask (code type 8, or C4 without repeat) around the whole cheat | `hold`, where the block only writes data, or where an else branch writes back the original bytes of what it patches (the else branch is dropped, as runtimes restore patches on release). A block that patches code without such an else branch leaves its patch when the buttons are let go: it becomes `run = "action"` with `press`. One that patches without an else branch and also writes data keeps only its `raw` entry |
| A memory condition (code type 1) around the whole cheat | `when` |

Where the code is only plain writes, the converter writes core operations: a write into a module's code becomes a patch, one into its data or a region a write, a pointer chain (code types 5, 7 and 6) a `path`. With the game's code module at hand, the converter fills `expect` from it and replaces fixed offsets by patterns, so that a cheat made for one build works on every build where its place can be found. Without the module, it keeps fixed offsets per build and the `raw` entry.

- An entry only restores code when, with the module, every byte it writes is the original byte there. Without the module, when its name says so (`restore`, `original`, or `off` at the end) and other entries write each of its places; its bytes then give `expect` to those entries, as an else branch that restores gives it to its own button block.
- Without the module, a write into the main module becomes a patch only where the file gives the original bytes; a converter cannot tell code from data otherwise, and a patch needs `expect`.
- A pattern does not hold the bytes that move between builds: branch and literal offsets, page addresses, and the low 12 bits that follow a page address are `??`.
- A patch keeps its offsets per build where the same bytes would mean something else at another place: new bytes with a branch or address relative to themselves that points outside them, or code written after the end of the code (a code cave in the padding of the last page).
- Registers carry over from one Atmosphère cheat to the next in the same frame. A cheat that reads a register an earlier cheat sets keeps only its `raw` entry.
- A cheat requires the master only where it depends on it, so that the cheats that find their place by pattern keep working on a build the master does not fit. A cheat depends on the master when it reads a register the master sets, or writes, reads through or branches into bytes the master writes, and also when it keeps only its `raw` entry or when what the master does is not known: in Atmosphère the master always runs first.

Back to Atmosphère, a converter writes each build of the file as its own `<build ID>.txt`, named by the build's main module; a build that names no main module cannot be one. It can also write a build the file does not name, from the cheats' patterns, given that build's main module. For each cheat it writes the `raw` entry for that build where there is one, and its core operations otherwise, or where the caller prefers them, for all cheats or for one, such as a cheat whose value the player set: a patch as stores of its bytes, a pointer path as loads, a `hold` as a key block whose else branch writes back the original bytes of the patches, an action as a key block, a single `when` on a fixed place as a memory condition, and each value as the player chose it or as its default. Where the core operations cannot be written, a cheat with a `raw` entry for the build falls back to it.

The file keeps the limits of the host it is for, beyond which the host refuses the whole file. Atmosphère's dmnt holds one master entry, which gets the code of every master cheat in order and the first one's name; at most 127 other entries with code, of at most 256 words each; and a file below 64 KiB, so at most 65,535 bytes of UTF-8. An entry without code takes no slot there. A host that takes more, such as an emulator's own cheat engine, MAY give its own limits, or none.

The cheats go in file order, the master cheats into the master entry, except that a cheat comes after the cheats it requires. A group starts with an EdiZon section marker, `[--SectionStart:Name--]`, and ends with `[--SectionEnd:Name--]` where another group or a cheat in no group follows; the last group ends with the file. The markers have no code, so they take no slot in dmnt and no switch in the overlays that list its cheats.

Names are written as dmnt reads them. The bracket that would end a name early, `]`, or `}` for the master, becomes `)`; a control character or a line break becomes a space; and a name is cut to 63 bytes of UTF-8 at a character boundary, as dmnt keeps 64 bytes without the end of the string. A host that keeps longer names MAY have them whole. In the section markers, the group's name is cut alike for the start and the end. A name that an entry of the file already has gets ` (2)`, ` (3)` and so on, as dmnt keeps the state of each switch by its name.

A host reads one cheat file a build, so several files of one game, such as the sources a runtime holds for it, MAY go into one file for each build. Builds are matched by the build ID of their main module, its first 16 digits. The master cheats of every file go into the one master entry, then each file's other cheats in turn, in one set of limits. A file that does not name a build gives it what it can write without its own offsets for that build, as the reader would run it there.

With the build's main module, the converter finds the patterns and checks each patch and override stub against `expect`, as a runtime would, because Atmosphère code writes without looking; without the module, it writes what it can place as it is. Each cheat it leaves out gets a stable reason, the same in every library: the reader's reason where the cheat has a problem of its own (Appendix B); otherwise the first of these that it meets, in the order of section 11.2, an entry of more words than the host takes among them; then `requires:<id>` where it requires a cheat that is left out, as the reader refuses it; and last what the whole file holds, in the order the cheats go: the words of the master entry, the entries and the size:

| Reason | The cheat |
|---|---|
| `build-unknown` | Has neither an offset nor a `raw` entry for this build |
| `pattern-not-found`, `pattern-ambiguous` | Has a pattern that does not match exactly once in the module |
| `expect-mismatch`, `outside` | Patches other bytes in the module than `expect`, or has a place in it, or the pointer at the base of a pointer path, past its end |
| `value-out-of-range` | Has a value, as the player chose it or as its default, that it cannot take or that its instruction or write cannot hold |
| `atmosphere:no-module` | Has a pattern, and no module was given to search |
| `atmosphere:run`, `atmosphere:monitor` | Runs `"once"` or `"timed"`, or is a monitor |
| `atmosphere:toggle` | Has `toggle` buttons |
| `atmosphere:condition` | Has conditions Atmosphère cannot say: more than one, `match`, `trigger` or `interval`, or one on a pointer path or of a float type |
| `atmosphere:action` | Is an action that writes data or has `hold` |
| `atmosphere:override` | Has an override without `expect`, or one whose stub is longer than it |
| `atmosphere:module`, `atmosphere:region`, `atmosphere:address` | Has a place Atmosphère cannot name: in a module other than main, in a region without a memory code, or at an absolute address |
| `atmosphere:empty` | Has nothing to write |
| `requires:<id>` | Requires a cheat that is left out |
| `atmosphere:too-long` | Would be an entry of more words than the host takes (256 in Atmosphère), or make the master entry longer than that |
| `atmosphere:too-many` | Would be an entry with code past the last the host holds (the 127th in Atmosphère) |
| `atmosphere:too-big` | Would make the file larger than the host reads (65,535 bytes in Atmosphère), with the section markers before its entry |

For the patch files that Atmosphère and the emulators apply to a code module when they load it (IPS and IPS32 in `exefs_patches/<set>/`, and IPSwitch text, `.pchtxt`):

| Patch file | `.mncf` |
|---|---|
| The file name, `<build ID>.ips`, or `@nsobid-<build ID>` in a `.pchtxt` | A build with `modules = { <module> = "<build ID>" }`, for the module whose build ID it is; the zero bytes at the end of the ID are left out, down to 16 digits |
| An IPS or IPS32 file | One cheat, named by its patch set, with `default = true`: a loader applies the file whenever it is there |
| A patch of a `.pchtxt`, from `@enabled` or `@disabled` to the next of them or `@stop` | One cheat, named by the comment on a line of its own before it, with `default = true` where it is `@enabled`. The patches of a file with more than one get `group` = the patch set |
| A record that writes into code or read-only data | A patch, `expect` from the module |
| A record that writes into data | A write of `bytes` with `expect` = the original bytes: a loader sets them once, and the write is made only while the place still holds them |
| `#` lines, and lines of a `.pchtxt` that are no patch | Kept in `about`. Comments that credit someone give `authors` |

- A record's offset counts from the start of the module's NSO file: its 0x100-byte header, then the image. The offset in the module is the record's offset less 0x100. The part of a record that lies in the header, or past the end of the module, is left out, as loaders do.
- IPS: `PATCH`, then records up to `EOF`. A record is an offset (3 bytes, big-endian), a size (2 bytes, big-endian) and that many bytes; a size of 0 is a run of one byte: a count (2 bytes) and the byte. IPS32 is the same with `IPS32`, 4-byte offsets and `EEOF`. A later record wins over an earlier one at the same byte.
- IPSwitch text: a patch line is an offset of 8 hexadecimal digits, a space, and a value: hexadecimal digits in pairs, in the order they are written, or reversed after `@little-endian` until `@big-endian`; or a string in double quotes, with the escapes `\n`, `\t`, `\r`, `\0`, `\\`, `\"` and `\'`, written as UTF-8. `@flag offset_shift <number>` adds the number to every offset after it. `//` starts a comment outside a string. A file stops at `@stop`.
- A patch file holds no original bytes, so a converter needs the module it patches; without it, it cannot convert the file. A patch whose records lie where the module does not hold the game's own bytes cannot be converted either; it is left out and named in `about`.
- As in an Atmosphère file, a patch gets a pattern where its place can be found by one, `movz` and `fmov` patches MAY become values the player sets, and files for several builds become one file, a cheat with the same name that does the same in each becoming one cheat.

Converters for the cheat and patch files of other emulators and for PC cheat tables follow the same rule: keep every cheat, and write core operations where the meaning is clear.

A converter also writes down what it knows about the source:

- `origin` names the source's format, where the caller knows it the source's name, and the website the source's links name. `authors` are the people the source credits, each with the platform the source names for them in parentheses (`"Ann (example forum)"` for "Ann on example forum"); where it credits nobody, the caller MAY name a default author, such as the player who imported the file, and otherwise `authors` is left out. Credit lines, links and title lines go into `about` as they were.

- A patch whose new bytes are one `movz`, a `movz` and a `movk` of the same register, or one `fmov` with an immediate, MAY become an `arm64-imm/1` patch whose value the player sets, with the instruction's own number as the default.
- Files for several builds of the same game become one file with all of those builds. A cheat with the same name that does the same in two builds, apart from fixed offsets, becomes one cheat with offsets for each build and a `raw` entry for each. A build is named by its version, or by its build ID where two files give the same version; two files for the same build ID cannot be one file.
- A credit that names no person ("All Credited", "everyone", "unknown") gives no author.
- A converter MUST NOT take original bytes from places where the module it was given does not hold the game's own bytes, such as code a runtime rewrote; there a patch keeps fixed offsets and takes `expect` only from the source itself.


## 16. Conformance

The conformance suite in [`format/tests/`](../tests/) holds made-up files and packs with the results every reader must give: errors, warnings, the build, signature results, and each cheat's state, reason and resolved operations. Its pass cases give a file, a runtime and a sequence of what the player and the game do between passes, with the changes of each pass and the states after it. A reader conforms to `mncf/1` when it gives those results for every case; a runtime's pass engine, when it gives them for every pass case. A change to a rule of this spec comes with the cases that show it.

## 17. Updates

A file can name the address of its newest version. When the game gets an update and the authors make a new version of the file for it, a runtime can then bring that version to the player, with the authors' notes on what changed, and keep everything the player set.

```toml
format = "mncf/1"
name = "Example Quest cheats"
id = "example.org/someone/example-quest"
file_version = "5"

[update]
url = "https://example.org/someone/example-quest/example-quest.mncf"

[[changes]]
version = "5"
date = "2026-12-02"
text = "Fits game version 1.0.4. Money and the first bag slot work again."
added = ["shiny-odds"]
fixed = ["money", "first-slot"]
renamed = { "max-iv" = "max-ivs" }

[[changes]]
version = "4"
date = "2026-10-11"
text = "The old speed cheat is gone: it crashed in races."
removed = ["speed"]
```

A reader of an earlier draft reads such a file: a key outside a cheat that it does not know is only a warning (section 10).

### 17.1 The keys

| Key | Type | Meaning |
|---|---|---|
| `id` | string | The identity of the file across all its versions. Two files with the same `id` and the same game are versions of one file. 1 to 128 characters: lower-case ASCII letters, digits, `.`, `-` and `_`, in parts separated by `/`, none of them empty, `.` or `..`. A domain or a repository the authors control, followed by a path, keeps it unique: `"github.com/someone/example-quest"`. It names the file and is not an address: it has no scheme, and a runtime never connects to it |
| `file_version` | string | Section 4. In a file with `[update]` or `[[changes]]`, a **version**: 1 to 8 whole numbers of 1 to 9 decimal digits each, separated by dots: `"5"`, `"2.1"`, `"2026.12.2"` |
| `update` | table | `url`: the address of the newest version of the file (section 17.3) |
| `changes` | array of tables | One change entry for each version, below |

Versions are compared number by number from the left, as numbers: `"2.10"` is above `"2.9"`, and a number that is missing counts as 0, so `"2"` and `"2.0"` are the same version.

A change entry:

| Key | Type | Meaning |
|---|---|---|
| `version` | string | Required. The version it describes, a version as above, not above the file's `file_version` |
| `date` | string | The day the version was made, as `"YYYY-MM-DD"`, a day of the Gregorian calendar. It is a string because the format has no date values (section 2) |
| `text` | string | What changed, for the player. Translations go in `i18n.<tag>.changes."<version>".text` (section 9) |
| `added` | array of ids | Cheats that are new in this version |
| `fixed` | array of ids | Cheats that work again in this version, or work better |
| `removed` | array of ids | Cheats of the version before that this one no longer has |
| `renamed` | inline table | From the id a cheat had in the version before to the id it has in this one |

- Each version has at most one entry. The entries MAY come in any order; a runtime shows them newest first.
- The ids in the lists are cheat ids (section 2), each at most once in an entry. An id is in at most one of `added`, `fixed` and `removed`, and a cheat in `removed` is not renamed in the same entry. Two cheats are not renamed to the same id.
- The newest entry SHOULD be for the file's `file_version`. A tool that makes a new version of a file, such as `mistel release`, writes it.

A reader keeps the file usable whatever is wrong with these keys. It leaves out what is wrong and warns (Appendix D):

- `id`: `id` is not an id as above. The file is read without one.
- `update`: `[update]` cannot be used: it is not a table, has no `url` that section 17.3 allows, or is in a file without `id` or without a `file_version` that is a version. The file is read without it.
- `changes`: `changes` is not an array of tables, or is in a file without a `file_version` that is a version. The file is read without change entries.
- `changes:<version>`: one entry breaks a rule above. It is left out; `<version>` is its `version`, or its position from 1 where it has no `version` string.

A key inside `update` or a change entry that this section does not define is the warning `unknown-key:update.<key>` or `unknown-key:changes.<key>`, as for any key outside a cheat.

### 17.2 A new version

A runtime takes a file as a new version of a file it holds only when all of the following holds, checked in this order. The first that does not hold is the reason it gives (Appendix E):

1. The new file loads (Appendix D). Otherwise: `update:error:<code>`, with the code of the error.
2. Its signature is not `invalid` or `revoked` (section 12). Otherwise: `update:signature`.
3. It is for the same game: the same `game.platform`, and the same `game.id` without regard to case. Otherwise: `update:game`.
4. It has the same `id`. Otherwise, also where it has none: `update:id`.
5. Its `file_version` is a version, above the version of the file the runtime holds. A file held without a version counts as below every version. Otherwise: `update:version`. So an old file, signed or not, can never be played back in place of a newer one.
6. **The same signer.** Where the file the runtime holds has signature lines, a signature line of the new file verifies with a key that signed the file it holds. Otherwise: `update:signer`.

The keys that signed a file are the keys of its signature lines that verify, with a key the reader trusts or with a key in the file's `signers` (section 12). A line that cannot be checked that way gives only its key id: a line of the new file then counts where it names that key id and verifies with a key of the new file. A line of an algorithm the reader does not know gives nothing, so a file signed only that way gets no new version from that reader. A revoked key never counts: a file signed with one is `revoked`.

To change keys, the authors sign one version with the old key and the new one, using two signature lines, and later versions with the new key only.

A file without signature lines gets new versions the same way, without step 6. It is not warned about or marked for being unsigned (section 12). Its new version MAY be signed, and from then on step 6 holds.

When the player adds a file by hand that has the `id` and the game of a file the runtime holds, it is a new version of that file too, and the runtime keeps what the player set (section 17.5). Because the player chose it, the runtime MAY take it even where step 5 or 6 does not hold, after saying so: it is an older version, or it was signed by another key.

### 17.3 Looking for a new version

`update.url` is the address of the newest version of the file itself, not of a list of versions: one `.mncf` file covers every build of its game, so no list is needed. A raw file in a repository, a release asset, or a fixed address that a site gives each file is enough. The address:

- starts with `https://`;
- then names a host: two or more parts of lower-case ASCII letters, digits and `-`, separated by dots, each part 1 to 63 characters that neither start nor end with `-`, and the last part not only digits, so that it is a name and not an IP address;
- then MAY give a port, `:` and 1 to 5 digits, a number from 1 to 65535;
- then MAY give a path, starting with `/`, and a query, starting with `?`, of ASCII letters, digits, the characters `-._~!$&'()*+,;=:@/?` and `%` followed by two hexadecimal digits;
- has no user name and no `#`, and is at most 2,048 characters long.

A runtime:

- **asks first.** When a file with `[update]` is added, it asks the player once whether it may look for new versions of the file at the host, naming the host. Without a yes, it looks only when the player asks for it, such as with a button "Check for updates". It keeps the answer for the file, by its `id`, and the host; a new version whose `url` names another host is asked about again.
- **sends nothing about the player.** It asks for the address as it is written, with `GET`, without cookies, credentials, cache validators (`If-None-Match`, `If-Modified-Since`) or a `Referer`, and with the User-Agent `mncf-update/1`, the same in every runtime. It follows at most 5 redirects, each to an `https` address; the host of a redirect needs no answer of its own. It SHOULD NOT connect where the host's address is a loopback, private or link-local one.
- reads at most the size a file may have (Appendix D), and takes only an answer with status 200, whatever its content type, as the new file. It then checks it as section 17.2 says.
- where the player allowed it, looks at most once a day for a file, and also **at the moment a game update stops the cheats**: when cheats of the file are refused with `build-unknown`, `pattern-not-found` or `expect-mismatch`, or the build that runs is none of the file's builds. Where the player did not allow it, the runtime offers to look at that moment. Where the new version names the build that runs, the runtime says that a new version of the file knows this version of the game.

### 17.4 Installing a new version

- A runtime MUST NOT put a new version in place of the old one without the player's yes. It first shows the `text` of each change entry above the version the player has, up to the new version, newest first, in the player's language (section 9), and the host the new version came from. One yes installs it.
- A runtime that installs new versions keeps the version it replaced, so that the player can go back to it. Going back is the player's choice and is not checked as section 17.2 says. After going back, the runtime does not offer the version the player left again, only a version above it.
- In the cheat list, a runtime MAY mark the cheats in `added` of those entries as new, and those in `fixed` as fixed, under their ids in the new version (section 17.5), until the player has seen them. A cheat that is new and fixed is marked new.

### 17.5 What the player set

A runtime keeps everything the player set for a file across its versions. For each cheat, by its id: whether the player switched it on or off, its values, and what else the runtime keeps for a cheat, such as the answer whether it stays off after the game ended abnormally (section 11.4), or that the save was backed up before it was first switched on. For the file: the preset the player chose, the answer to the question of section 17.3, and what else the runtime keeps for the file.

The ids follow the change entries above the version the player has, up to the new version, in version order. In each entry, an id in `removed` is gone, an id in `renamed` takes its new id, and the others keep theirs. Then:

- What the player set for a cheat that the new version does not have is dropped.
- A value of a cheat that is still there keeps what the player chose where it still fits the value in the new version: of its type, inside `min` and `max`, and one of its `choices`. Otherwise it goes back to its default, and the runtime names these values, calmly, when it shows the new version. A value whose id the cheat no longer has is dropped.
- A preset that the new version does not have is dropped.
- Where two cheats come to one id, which renames in different entries can do, the cheat keeps what the player set for the first of them.
- The same holds when the player adds a new version of a file by hand (section 17.2).

The libraries in [`../lib/`](../lib/) give this mapping, the check of section 17.2 and the change entries to show, so that every runtime does the same.

### 17.6 Packs

A pack manifest (section 13) has the same keys for the pack as a whole: `id`, `signers`, `file_version`, `[update]` and `[[changes]]`, with the same warnings. Its `update.url` is the address of the newest `.mncp` file, and the lists of its change entries name the pack's files by their `path`, which then need not be ids.

- A runtime checks a new pack as section 17.2 says, for the manifest: the pack's errors (Appendix D) for step 1, the manifest's signature for steps 2 and 6, and no step 3, as a pack has no game of its own.
- It matches each file of the old pack with the file of the new pack that has the same `id` and game. A file without such a match is matched with the file at the same `path`, following the `renamed` of the pack's change entries, where one of the two has no `id`; a file that does not load, or is not `ok`, has none. For each pair, it keeps what the player set as section 17.5 says, with that file's own change entries. A file of the old pack without a match is gone, with what the player set for it.
- A file in a pack gets new versions only with its pack: a runtime ignores the `[update]` of a file it holds from a pack.

Other formats of Mistel use the keys of this section as it says, with the ids of what their files hold in the lists of the change entries.

## Appendix A. Platforms


New platforms are added by a pull request to this table.

| `platform` | Game `id` | Architecture, byte order, pointer size | Modules and their identity | Regions | Buttons |
|---|---|---|---|---|---|
| `switch` | Title ID, 16 hexadecimal digits | ARM64, little-endian, 8 | `main`, `sdk`, `subsdk0` to `subsdk9`; the build ID in hexadecimal, 1 to 64 digits | `heap`, `alias`, `aslr` | `A B X Y L R ZL ZR PLUS MINUS LSTICK RSTICK DUP DDOWN DLEFT DRIGHT`, the stick directions `LSTICK_UP LSTICK_DOWN LSTICK_LEFT LSTICK_RIGHT RSTICK_UP RSTICK_DOWN RSTICK_LEFT RSTICK_RIGHT`, and `SL SR` |
| `switch2` | As `switch` | As `switch` | As `switch` | As `switch` | As `switch`, plus `C` |
| `n64` | The ROM header's game code, 4 printable ASCII characters | MIPS, big-endian, 4 | `rom`; `sha256:` of the ROM | Absolute addresses (`address`) in RDRAM | `A B Z START L R CUP CDOWN CLEFT CRIGHT DUP DDOWN DLEFT DRIGHT` |
| `pc` | `<store>:<id>`, for example `steam:123456` | x86-64 or ARM64, as the runtime says, little-endian, 8 | The executable's and libraries' file names; `sha256:` of the file | None: data is reached by pointer paths from a module | Upper-case key names (`F1`, `CTRL+F1`) and gamepad names (`PAD_A`, `PAD_LB`): letters, digits and `_` |

A module identity of the form `sha256:` has 64 hexadecimal digits.

### Override stubs

A runtime that runs the original code applies an override (section 7.5) by writing this stub at the function's entry. The value is taken as a 64-bit two's complement number.

| Architecture | `"skip"` | `"return"` |
|---|---|---|
| ARM64 | `ret` (`C0 03 5F D6`) | `movz x0, #value` if the value is below 0x10000; else `movn x0, #~value` if `~value` is below 0x10000; else `movz x0, #(bits 0 to 15)` and a `movk x0, #(bits 16n to 16n+15), lsl #16n` for each n from 1 to 3 whose 16 bits are not zero. Then `ret`. 8 to 20 bytes |
| x86-64 | `ret` (`C3`) | `mov eax, imm32` (`B8`) if the value is below 2^32; else `mov rax, simm32` (`48 C7 C0`) if it is a negative number from -2^31; else `mov rax, imm64` (`48 B8`). Then `ret`. 6 to 11 bytes |

Other architectures have no stub yet: their runtimes apply overrides with `code-hook`.

## Appendix B. Reasons

| Reason | Meaning |
|---|---|
| `game-mismatch` | The file is for another platform or another game |
| `invalid` | The cheat has no operation (section 7.1) |
| `invalid:<key>` | The cheat breaks a rule of this spec at that key (below) |
| `build-unknown` | The cheat needs a build (per-build offsets, `raw`), and the running build is not in the file, or not among the builds the location or the `raw` entries name |
| `module-unknown`, `region-unknown` | The runtime has no module or region of that name |
| `outside` | A place is not inside its module, region or guest memory (section 6.2) |
| `pattern-not-found`, `pattern-ambiguous` | Section 6.3 |
| `expect-mismatch` | The original bytes are not there |
| `path-broken` | A pointer path does not resolve at the moment, or a place the cheat reads cannot be read; the cheat waits |
| `done` | A cheat that runs once or for a time did its work and switched itself off; it is off (section 11.5) |
| `capability-missing:<capability>` | Section 11.1 |
| `extension-missing:<extension>` | Section 10 |
| `unknown-key:<key>` | Section 10 |
| `value-out-of-range` | A player value does not fit where it is written |
| `requires:<id>`, `excludes:<id>` | A relation stops the cheat |
| `signature` | The file's signature is invalid or revoked |
| `runtime:unsupported:<key>` | The runtime does not have that part of the core yet (section 11.1) |
| `runtime:rewritten` | The runtime rewrote that code for its own reasons (section 11.3) |
| `runtime:<reason>` | Another reason of the runtime's own |


In `unknown-key:<key>` and `invalid:<key>`, `<key>` is the key's path from the cheat: the TOML keys from the cheat's table down to the key, joined by dots, without array positions. Examples: `name`, `values.count.default`, `patch.at.find`, `when.value.at`. Where a rule ties two keys, the key named is the one the rule's sentence is about: `patch.expect` when `expect` and `bytes` differ in length, `patch.bytes` when a patch has neither `bytes` nor `encode`, or both.

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

## Appendix D. Errors

A file with one of these errors is not used at all. The reader says the code and a message.

| Code | Meaning |
|---|---|
| `size` | More than 16 MiB (16,777,216 bytes) |
| `encoding` | Not UTF-8, a byte order mark, or a CR character (section 2) |
| `toml` | Not TOML 1.0, a date or time value, or tables and arrays nested deeper than 128 levels |

| `format` | `format` is not the first key, or not `"mncf/1"` |
| `platform` | `game.platform` is not in Appendix A |
| `schema:<key>` | A key outside the cheats is missing or has the wrong type or form, such as `schema:name`, `schema:game.id`, `schema:builds`, `schema:cheat` (no cheat) or `schema:cheat.id` |
| `duplicate-id:<id>` | Two cheats have that id |

A pack with one of these errors is not used at all:

| Code | Meaning |
|---|---|
| `zip` | Not a ZIP file, two entries of the same name, or an entry that is neither stored nor deflated, is encrypted, needs a ZIP version above 6.3, or whose data does not decompress |
| `size` | More than 4,096 entries, an entry of more than 16 MiB (whatever its header says), or more than 256 MiB in all the entries a reader reads |

| `manifest` | No `pack.toml` at the root, or it is not UTF-8 TOML with LF line ends |
| `format` | `format` is not the first key of the manifest, or not `"mncf-pack/1"` |
| `schema:<key>` | A key of the manifest is missing or has the wrong type or form |
| `path:<path>` | A file path that section 13 does not allow |

A converter (section 15) gives these errors for sources that cannot become a file:

| Code | Meaning |
|---|---|
| `atmosphere` | Not an Atmosphère cheat file, or one without a cheat that has code |
| `patch` | Not an IPS, IPS32 or IPSwitch patch file |
| `source` | Sources that cannot be one file: two for the same build, a patch file without its module or without a build ID, or none of whose patches could be converted |

Warnings: `unknown-key:<key>` for a key outside the cheats that neither the core nor a listed extension defines, with its path from the top of the file; `preset:<id>` for a dropped preset (section 8); `i18n` for an `i18n` table that is not a table of tables, which is then ignored; `signers` for a `signers` key that is not an array of public keys, each once (section 12); and `id`, `update`, `changes` and `changes:<version>` for the keys of section 17.1, in a file or a pack manifest, which are then left out as that section says.

## Appendix E. Update reasons

The reasons a runtime gives when it does not take a file as a new version of one it holds (section 17.2), in the order it checks them:

| Reason | Meaning |
|---|---|
| `update:error:<code>` | The new file, or pack, does not load: `<code>` is its error (Appendix D) |
| `update:signature` | Its signature is `invalid` or `revoked` |
| `update:game` | It is for another platform or another game |
| `update:id` | It has another `id`, or none |
| `update:version` | Its `file_version` is not a version, or not above the version the runtime holds |
| `update:signer` | The file the runtime holds is signed, and no signature line of the new one verifies with a key that signed it |
