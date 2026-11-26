# Mistel Native Cheat Format (`.mncf`)

One cheat file per game, for all of its versions and every way it runs. An emulator, a native port or a recompiled PC build that reads `.mncf` files knows from the file itself what each cheat is, what it may change and whether it fits what runs, so that it never has to guess from raw opcodes.

The format is made for the new era of native games: recompilations that turn a console game into a native program, and compatibility layers that run a game's own code natively, as nxnative does on Android. It is not tied to one console, one CPU or one runtime, and grows through versioned extensions instead of new formats.

This folder is under the [MIT License](LICENSE), so that any emulator or port can read the format, whatever its own license. Nothing in it depends on code outside this folder.

## What a file carries

- **The game and its builds:** the platform and the game's ID on it, and a table of the builds the file knows, with their version names.
- **Two address spaces:**
  - **Guest memory:** the original game's memory, at the addresses the original console used. An emulator, nxnative and a recompilation all keep it, so a data cheat ("99 lives") works the same in each.
  - **Code:** in a build that runs the original machine code, a place in a code module; in a recompilation, where that code no longer exists as bytes, a function, named by its original address, its symbol or a pattern. The runtime turns a code cheat into a byte patch or into a hook in the recompiled function.
- **Where to write:** a byte pattern found in the code, a fixed address per build, or a pointer path, for any CPU (ARM64, x86-64, PowerPC, MIPS and others).
- **What must be there before:** the original bytes, or the original instruction, checked before every write. A cheat whose check fails stays off and says why.
- **What to write:** instructions or bytes, and as an escape hatch, raw code of an existing cheat format such as Atmosphère's.
- **How it runs:** a switch, a frozen value, a value set once, an action on a button, a timed effect, a value that is only shown.
- **Values the player sets:** typed numbers with a range, step and unit, lists of named choices, bit flags. Code cheats get values too, by re-encoding the instruction.
- **Order and meaning:** groups, a description, tags such as `touches_save`, presets, cheats that need or exclude others, groups where only one can be on.
- **Who made it:** the author, and a signature that a reader checks against keys it trusts.
- **Its versions:** an identity that stays the same across versions, the address of the newest version, and notes on what each version changed, so that a runtime can bring the player a version made for a new game update, signed by the same author, with everything the player set kept.

The complete schema is the spec.

## Mods: the Mistel Native Mod Format (`.mnmf`)

The sibling of `.mncf`, for mods: one `.mnmf` file per mod, and `.mnmp` packs of several mods. It shares the conventions, platforms, signatures and keys of `.mncf`, and carries code changes as `.mncf` files.

- **Survives updates:** a mod ships only the files it changes, each with the hash of the original game file it was made for. On a new build, every part whose originals are unchanged keeps working and is shown as checked; a part whose originals changed is switched off with a reason.
- **Indexes built by the runtime:** a mod never ships a game's archive index made for one build; it names an index rule, and the runtime builds the index for the build that runs.
- **A full page:** name, version, authors and credits, license, a description with a small markup, kinds with standard icons, tags, a square logo, a banner, a gallery with thumbnails, supported builds with a status for each part, choices and options, translations.
- **Built for speed:** a ZIP whose first entries are the manifest and a thumbnail, files that are already compressed stored as they are, Zstandard for the rest, a hash for every file, streaming installs and reading in place.
- **Packs:** load order, dependencies, conflicts, the chosen setup of each mod, and `.mncf` files.

## Layout

| Folder | What | Status |
|---|---|---|
| [`spec/`](spec/) | The specifications, versioned: [`mncf-1.md`](spec/mncf-1.md) for `mncf/1`, [`mnmf-1.md`](spec/mnmf-1.md) for `mnmf/1` | Drafts |
| [`guide.md`](guide.md) | How to write `.mncf` files by hand, step by step, and what the reasons for a refused cheat mean | Version 1 |
| [`lib/python/`](lib/python/) | The Python library: read and check a file, identify the build, resolve locations, give each cheat its state and reason, check signatures and packs; write files; convert Atmosphère cheats | Version 1 |
| [`lib/java/`](lib/java/) | The same in Java, for Android (API 33) and the JVM, without dependencies | Version 1 |
| [`lib/c/`](lib/c/) | Reading in C99, for native emulators, recompilations and anything with a C ABI: check a file, its signature and packs, evaluate, and the pass engine; one file to drop into a build, without dependencies | Version 1, no writer or converter yet |
| [`tests/`](tests/) | The conformance suite: made-up files and packs with the results every reader must give, and pass cases with the changes every runtime makes | Version 1 |

## Versions and extensions

A file starts with its format version, `format = "mncf/1"`. Within one major version, a newer reader reads every older file. A change that breaks this is a new major version.

The core stays small: the target, the checks, the writes, the values and the signature. Everything else is a named, versioned extension. A file lists the extensions it needs and a reader says which it supports, so a reader that lacks one turns only the cheats that use it off, with the reason, and the rest of the file still works.
