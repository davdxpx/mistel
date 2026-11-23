# Mistel

Mistel is an open-source toolkit for modding Switch games that run natively on Android, through compatibility layers built on [nxnative](https://github.com/seedds/nxnative), such as Banana Native and [Peanuts Native](https://github.com/igawa6/peanuts-native). It makes cheats, mods and companions that reach into a game's own code easier to build, safer to run, and able to survive a game update.

A mistletoe grows into a host tree and lives from it. Mistel grows into a game's code in the same way: it never replaces the game, it attaches to it.

## Parts

| Part | Folder | What it does | Status | License |
|---|---|---|---|---|
| Mistel Native Cheat Format (`.mncf`) | [`format/`](format/) | One cheat file per game for all its versions and every way it runs, made for native recompilations and native runtimes on any platform: guest memory and native code as separate address spaces, byte patterns instead of fixed addresses, a check of the original bytes before every write, typed values, groups, presets, extensions and signatures | [Spec draft](format/spec/mncf-1.md) | MIT |
| Tools | [`tools/`](tools/) | The `mistel` command for authors: check a file, convert cheats from other formats, make patterns, port cheats to a new version, sign and pack | Planned | GPL-3.0 |
| Muninn | [`muninn/`](muninn/) | Analysis: memory scans, pointer paths, call traces and watchpoints, the ground work for cheats, signatures and adapters | Planned | GPL-3.0 |
| Adapters | [`adapters/`](adapters/) | What Mistel knows about a game engine, so that one finding holds for every game on it. Unreal Engine 4.23 and later first | Planned | GPL-3.0 |
| SDK | [`sdk/`](sdk/) | Helper code in C for companions and code mods: hooks, a mailbox to the launcher, and guards (pointer and vtable checks, state gates, build gates, crash-loop quarantine) | Planned | GPL-3.0 |
| Lab | [`lab/`](lab/) | The test loop on a real Android handheld: start a test build, run a scenario, collect the log | Planned | GPL-3.0 |

How the parts fit together: [docs/architecture.md](docs/architecture.md).

## What Mistel never does

- No game files, keys, firmware or anything extracted from a game, in this repository or in an issue. Mistel works on the dump folder that a player made from their own console and keeps everything it extracts there.
- Nothing that asks for keys or explains how to obtain games.
- No cheats for real games in this repository. Mistel's own cheat packs are signed with Mistel's key and published separately; an emulator that reads `.mncf` files only loads what the player brings.

## Used by

- **Peanuts Native** (planned): reads `.mncf` files, checks their signatures against Mistel's public key and shows the result.

Other projects are welcome to read `.mncf` too. That is why the format and its readers are MIT.

## License

Two licenses, by folder:

- [`format/`](format/): the `.mncf` spec and the reader libraries, under the [MIT License](format/LICENSE), so that any emulator can read the format, whatever its own license.
- Everything else: the [GNU General Public License v3.0](LICENSE).

## Contributing

Read [CONTRIBUTING.md](CONTRIBUTING.md) before you open a pull request. [SECURITY.md](SECURITY.md) says how to report a security problem, such as a signature check that can be fooled.
