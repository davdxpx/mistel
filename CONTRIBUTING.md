# Contributing

Issues and pull requests are welcome. This page says how a change gets into Mistel and what a pull request has to bring. It holds for everyone who sends a change, and for any tool someone uses to write one.

What the parts are and how they fit: [README.md](README.md) and [docs/architecture.md](docs/architecture.md). Rules for automated tools working in this repository: [AGENTS.md](AGENTS.md).

## Never game files or keys

Never commit or attach game files, keys, title keys, firmware or anything extracted from a game: RomFS, code modules (NSO, NRO), shaders, dumps. Game files and their extractions stay in the game's dump folder. Title IDs, build IDs, offsets, byte patterns and logs are fine. Do not add anything that asks for keys or explains how to obtain games.

No cheats for real games in this repository either. Tests use made-up games: a made-up title ID, build ID, addresses and bytes.

## Before you start

- **Agree on the scope first** for anything large: a new part, a new engine adapter, a change to the `.mncf` or `.mnmf` format, or more than about 1,000 lines. Open an issue and say what you want to build and why.
- **The `.mncf` and `.mnmf` formats change only through their specs.** A change to the format is first a change to [`format/spec/`](format/spec/), with the version rule it follows, and then the readers in every language in the same pull request.
- Prefer a rule that holds for every game, or every game on one engine, to a rule for one game. Fix a bug at its cause, not by hiding the symptom.

## A pull request

- **One work package per pull request**, on a branch from the current `main`. Rebase on `main` instead of merging it in. Split a feature into several pull requests only when each part stands on its own and the split makes the review easier; a series of small parts for one feature reads as noise.
- **Commits** say what changed and why, in plain English sentences: the need or the symptom, the cause, the change, and what was checked. When there is an issue, the title ends with its number, for example `Read the pattern table of an .mncf file (#12)`.
- **The author is the person who sends the change.** No AI tool is ever named as an author or co-author, in a `Co-Authored-By` trailer, as "Generated with", or anywhere else in commits, the pull request text, comments or code. `scripts/check-publish.sh` refuses such commits.
- **Source only.** No binaries: no build output, no `.so`, `.apk` or `.jar`, no screenshots in the repository. A test that needs a binary file makes it itself.
- **No private data** in any file, commit or picture: no local paths, home folders, LAN or adb addresses, device serials, personal e-mail addresses.
- **Language and text.** English everywhere: docs, code comments, log lines, tool output, user-facing text. Plain, calm sentences. No em dash, no emoji, no decorative separators such as a middle dot between items. `scripts/check_text.py` checks this.
- **Test everything the change touches before you open the pull request**: the unit tests of each part it touches, and a run on a real device when it changes what happens on one. A pull request with a list of things that were not tested is not ready yet: keep it as a draft until they are.
- **Device runs** use a test build, never the install you play with, and a copy of a save, never your only one.

### The pull request text

Fill in the [template](.github/pull_request_template.md): what and why, what was checked (tests, device, game and version), and which docs changed.

Pictures only where they show something new or a visible difference: a new screen, a changed layout, a clip of something that moves. A fix with no visible state gets bullet points, not a picture. Pictures go into the pull request text, never into the repository:

- Number them (`01-…`, `02-…`) and put them in tables whose header row says what each column shows, several small pictures side by side.
- Crop status bars. No private data in a picture, and no cheat file of a real game.

### Review

Fixes asked for in a review are new commits on the same branch, with a short comment in the pull request that says what changed. Do not rewrite a branch under review unless the reviewer asks for it, and do not open a new pull request for review fixes.

## Checking a change

Before every push:

```bash
sh scripts/check-publish.sh
```

It checks every commit since `main` for AI attribution and every changed file for private data, binaries and the text rules. `sh scripts/install-hooks.sh` installs it as a pre-push hook. The same checks run on every pull request.

Each part's `README.md` names its own tests once the part has code. The pull request says which ran.

## How changes get in

The maintainer reviews every pull request, builds and tests it, runs it on a device where it matters, and merges it. The pull request's author stays the author of the commit.

## Licensing of contributions

A contribution is under the license of the folder it goes into: the MIT License in [`format/`](format/), the GNU General Public License v3.0 everywhere else ([README.md](README.md#license)).
