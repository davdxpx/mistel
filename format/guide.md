# Writing `.mncf` files

A practical guide for people who make cheats. The [spec](spec/mncf-1.md) is the full set of rules; this page shows how to use them, step by step, with a made-up game. Every ID, address and byte below is made up.

## 1. The smallest file

```toml
format = "mncf/1"
name = "Example Quest cheats"
authors = ["your name"]

[game]
platform = "switch"
id = "0100000000ABC000"
name = "Example Quest"

[[cheat]]
id = "money"
name = "Lots of money"

  [[cheat.write]]
  at = { region = "heap", offset = 0x1200 }
  type = "u32"
  value = 999999
```

- `format` comes first, always.
- A cheat needs an `id` (lower-case letters, digits and `-`) and a `name`, and at least one operation.
- Save the file as UTF-8 with LF line ends (any code editor does this; set it in Notepad++ or VS Code if needed).
- Check it: `mistel check example-quest.mncf`. It names every problem with the key it is at, such as `fail money: invalid:write.type`.

## 2. Say which builds the file knows

```toml
[builds."1.0.2"]
modules = { main = "A1B2C3D4E5F60718" }

[builds."1.0.3"]
modules = { main = "1122334455667788" }
```

The build ID is the name of the Atmosphère cheat file (`<build ID>.txt`), or what `mistel check --module <main>` prints. A file without builds works too, as long as no cheat needs one.

## 3. Find the place: patterns beat offsets

An offset is right for one build only. A pattern finds the same code in every build where that code has not changed:

```toml
  [[cheat.patch]]
  at = { module = "main", find = "68 12 00 B9 ?? ?? ?? 94" }
  expect = "68 12 00 B9"
  bytes = "69 12 00 B9"
  asm = { expect = "str w8, [x19, #0x10]", bytes = "str w9, [x19, #0x10]" }
```

- Let the tools make the pattern: `mistel pattern <dump>/exefs/main 0x1008` prints the shortest one that matches exactly once, with the bytes that move between builds (branch targets, page addresses) as `??`. Add `offset = 4` when it says so: the place is then 4 bytes after where the pattern starts.
- `expect` is the original bytes. A runtime checks them before it writes, so a cheat that does not fit a build stays off instead of breaking the game.
- `asm` is for people; `mistel check --module` tells you when it does not say what the bytes are.
- Where a pattern is not possible (code you add after the end of the code, a branch to such a place), give offsets per build: `at = { module = "main", offsets = { "1.0.2" = 0x1FF0 } }`.

Data that the game allocates while it runs is reached through a pointer path: a fixed place that holds a pointer, then the offsets to follow.

```toml
  [[cheat.write]]
  at = { module = "main", offsets = { "1.0.2" = 0x3A1C2D0 }, path = [0x130, 0x10] }
  type = "u16"
  value = 99
```

This reads the pointer at `main + 0x3A1C2D0`, adds `0x130`, reads the pointer there and writes at that pointer plus `0x10`. While a pointer on the way is null (before a save is loaded), the cheat waits.

## 4. Let the player choose the number

```toml
[[cheat]]
id = "money"
name = "Money"

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

A code cheat gets a number field too, when its new instruction is a `movz` or an `fmov` (extension `arm64-imm/1`, listed at the top as `extensions = { optional = ["arm64-imm/1"] }`):

```toml
  [[cheat.patch]]
  at = { module = "main", find = "08 00 80 52 ?? ?? ?? 94" }
  expect = "08 00 80 52"
  encode = { insn = "movz", width = 32, rd = 8, value = "$count" }
```

Named choices instead of a number: `choices = { "1" = "Potion", "2" = "Ether" }` in the value, or a shared list under `[choices.items]` and `choices = "items"`.

## 5. Buttons and how a cheat runs

| You want | Write |
|---|---|
| The cheat works while ZL is held, and the code is put back when it is let go | `hold = "ZL"` |
| A button combination switches the cheat on and off | `toggle = "L+DUP"` |
| Pressing B does it once (a jump, a heal), or applies a patch that stays until the cheat is switched off | `run = "action"` and `press = "B"` |
| It works for 30 seconds, then switches itself off | `run = "timed"` and `duration = 30` |
| Its writes happen once when it is switched on | `run = "once"` |
| It only shows a value, for example on a second screen | `run = "monitor"` and `[[cheat.read]]` |

Button names are the platform's: `A B X Y L R ZL ZR PLUS MINUS LSTICK RSTICK DUP DDOWN DLEFT DRIGHT`, the stick directions and `SL SR` on the Switch.

## 6. Act only when the game is in a state

```toml
[[cheat]]
id = "heal-low"
name = "Heal when low"
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
```

`trigger = "enter"` acts once each time the condition starts to hold; without it, the cheat acts in every frame in which it holds. Several conditions all must hold, or any one with `match = "any"`.

## 7. Change what a function does

An override makes a function return at once, with a value or without one. It works in emulators, in nxnative and in recompilations alike:

```toml
  [[cheat.override]]
  function = { module = "main", find = "FD 7B BF A9 FD 03 00 91 ?? ?? ?? 94" }
  action = "return"
  value = 1
  expect = "FD 7B BF A9 FD 03 00 91"
```

`expect` must be at least as long as what a runtime writes there: 4 bytes for `"skip"`, 8 for a `"return"` of a value up to 0xFFFF on ARM64.

## 8. Order, meaning and safety for the player

- `group = "Items/Potions"` sorts a cheat into a group and a subgroup.
- `tags = ["touches_save"]` makes a runtime back up the save before the cheat is first switched on; `may_crash`, `cosmetic` and `online_unsafe` are the other tags it knows.
- `requires = ["setup"]` switches another cheat on with this one; `master = true` makes `setup` a cheat the player does not see as a switch.
- `excludes = ["slow"]` and `radio = "speed"` keep cheats that do not go together apart.
- `[[preset]]` switches several cheats on at once, `[i18n.de]` translates names and descriptions.

## 9. From an Atmosphère file

You do not have to start from nothing:

```bash
mistel convert <title ID>/cheats/<build ID>.txt --version <build ID>=1.0.2 --module <dump>/exefs/main -v
```

Every cheat keeps its Atmosphère code as a `raw` entry, so nothing is lost; plain code also becomes patches, writes, `hold`, values and patterns, and `-v` says for each cheat what it became and why. Files of several builds go into one `.mncf` file at once. Then edit the result: give cheats an `about`, tags and better names, and check it with `mistel check --module` for each build you have.

## 10. When a cheat is off, and why

A runtime shows the reason a cheat is refused. The ones you will meet most:

| Reason | What it means | What to do |
|---|---|---|
| `pattern-not-found` | The code the pattern looks for is not in this build | The game changed there: make a new pattern for this build, or give offsets for it |
| `pattern-ambiguous` | The pattern matches more than once | Make it longer: `mistel pattern` picks one that matches once |
| `expect-mismatch` | The original bytes are not there | The cheat was made for another build |
| `build-unknown` | The cheat has offsets or raw code for other builds only | Add the build to `builds` and its offset |
| `invalid:<key>` | The file breaks a rule at that key | `mistel check` says which rule |
| `unknown-key:<key>` | A key the spec does not define, often a typo | Fix the key, or list the extension it belongs to |
| `capability-missing:<what>` | The runtime cannot do this kind of cheat | Nothing in the file; another runtime may |

## 11. Share it

- One file per game, for all its builds.
- Several files in one pack: `mistel pack cheats.mncp *.mncf --name "My cheats"`.
- A signature says who made a file and that it was not changed: `mistel sign cheats.mncf --key <your key>`. It is optional, and an unsigned file is used like any other. A runtime marks a file as verified only for keys it trusts.

## 12. Keep it up to date

When the game gets an update, players need the version of your file that knows the new build. Give the file an identity and an address, and a runtime can bring them each new version ([spec, section 17](spec/mncf-1.md#17-updates)):

```toml
id = "github.com/someone/example-quest"
file_version = "4"

[update]
url = "https://raw.githubusercontent.com/someone/example-quest/main/example-quest.mncf"

[[changes]]
version = "4"
date = "2026-12-02"
text = "Fits game version 1.0.4. Money works again."
added = ["shiny-odds"]
fixed = ["money"]
renamed = { "max-iv" = "max-ivs" }
```

- `id` never changes. A domain or a repository you control, with a path, keeps it yours.
- `file_version` is numbers with dots, and each new version is above the one before: a runtime never takes a version that is not.
- `url` is the address of the file itself, always the newest version: a file in a repository, or a release asset. Use `https`.
- Write one `[[changes]]` entry for each version. Keep cheat ids the same where you can. Where you rename a cheat, say so in `renamed`, so that what players set for it follows it.
- Once you sign a version, sign every later one with the same key, and list your public key in `signers`, so that a runtime can tell the new version comes from you. To change keys, sign one version with both keys.
- `mistel release cheats.mncf --previous <the version players have> --text "..." --sign <your key>` does all of this: the next version, the entry with the cheats added and removed, `signers`, the signature, and a check that runtimes take it. `mistel update-check` then fetches your address as a runtime does ([tools](../tools/README.md#new-versions)).
