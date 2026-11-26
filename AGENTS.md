# Notes for agents

Rules for automated tools that read, change or test code in this repository. They come on top of [CONTRIBUTING.md](CONTRIBUTING.md), which holds for every change; where the two seem to differ, CONTRIBUTING.md wins.

- **What Mistel is.** A toolkit for modding Switch games that run natively on Android through nxnative-based compatibility layers. The parts, their folders and their status are in the [README](README.md); how they fit together is in [docs/architecture.md](docs/architecture.md). The cheat format is one part of it, not the whole project.
- **Two licenses.** [`format/`](format/) is MIT, so that any emulator can read `.mncf` and `.mnmf` files; everything else is GPL-3.0. Never move code from a GPL folder into `format/`, and never make `format/` depend on anything outside it.
- **Inputs.** Mistel works only on a dump folder a player made from their own console. Everything extracted from it (code modules, RomFS files, memory dumps, traces) stays in that dump folder's work folder, never in this repository. Nothing in Mistel asks for keys or explains how to obtain games.
- **Test data is made up.** Tests use made-up title IDs, build IDs, addresses and bytes. No cheat for a real game goes into the repository, also not as a test.
- **Commits.** Commit only when asked, with this checkout's git identity. The person who asked is the author; never add an AI tool as author or co-author, never a `Co-Authored-By` or "Generated with" line, in commits, pull request text, comments or code. Run `sh scripts/check-publish.sh` before every push.
- **Pull requests.** One work package per pull request; split a feature only when each part stands on its own. Test everything the change touches first, on a device where it changes what happens on one. Do not open a pull request with a list of things that were not tested; keep it as a draft until they are. Review fixes are new commits on the same branch, not a new pull request.
- **Text.** English everywhere. Plain, calm sentences. No em dash, no emoji, no decorative separators. `scripts/check_text.py` checks changed files.
- **No private data** in any committed file or picture: no local paths, LAN or adb addresses, device serials, personal e-mail addresses.
- **Devices.** Test runs use a test build, never the player's own install, and a copy of a save, never the only one. Where a device is shared, take and release it with the project's own lock; never take it by force.
- **Signing keys.** Mistel's private signing keys never enter this repository, a pull request, an issue, a log or a CI secret. Packs are signed offline by the maintainer.
- **Where things go.** A finding about one game goes in that game's note once its part has notes. A finding about an engine goes in its adapter's docs. A change to the `.mncf` or `.mnmf` format goes in its spec first ([CONTRIBUTING.md](CONTRIBUTING.md#before-you-start)).
