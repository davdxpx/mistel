# Mistel Cheat Format (`.mcf`)

One cheat file per game, for all of its versions. An emulator that reads `.mcf` files knows from the file itself what each cheat is, what it may change and whether it fits the version that runs, so that it never has to guess from raw opcodes.

This folder is under the [MIT License](LICENSE), so that any emulator can read the format, whatever its own license. Nothing in it depends on code outside this folder.

## What a file carries

- **The game and its builds:** the title ID, and a table of the builds the file knows, with their version names.
- **Where to write:** a byte pattern that Mistel finds in the game's code, a fixed address per build, or a pointer path.
- **What must be there before:** the original bytes, or the original instruction, checked before every write. A cheat whose check fails stays off and says why.
- **What to write:** ARM64 instructions or bytes, and as an escape hatch, raw Atmosphère cheat code.
- **How it runs:** a switch, a frozen value, a value set once, an action on a button, a timed effect, a value that is only shown.
- **Values the player sets:** typed numbers with a range, step and unit, lists of named choices, bit flags. Code cheats get values too, by re-encoding the instruction.
- **Order and meaning:** groups, a description, tags such as `touches_save`, presets, cheats that need or exclude others, groups where only one can be on.
- **Who made it:** the author, and a signature that a reader checks against keys it trusts.

The complete schema is the spec.

## Layout

| Folder | What | Status |
|---|---|---|
| [`spec/`](spec/) | The specification, versioned (`mistel/1`) | In progress |
| `readers/<language>/` | Reader libraries: parse, check, resolve a file for one build | Planned, Java and Python first |

## Versions

A file starts with its format version, `format = "mistel/1"`. Within one major version, a newer reader reads every older file, and a reader skips what it does not know only where the spec allows it. A change that breaks this is a new major version.
