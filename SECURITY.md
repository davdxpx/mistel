# Security

Mistel reads files that people share, checks signatures, and writes into a running game's memory. A bug in any of these can harm a player, so please report it privately first.

## What to report

- A file that makes a reader crash, hang, or read or write outside what it should, in any `.mncf` reader or in the `mistel` tools.
- A way to make a signature check pass for a file that was not signed, or that was changed after signing.
- A cheat or mod that the checks should have stopped from writing somewhere: past its region, over code the emulator rewrote, or after a failed check of the original bytes.

## How to report

Use GitHub's private vulnerability reporting on this repository (Security, Report a vulnerability). Please do not open a public issue for it. Say what you found, how to reproduce it with a made-up file, and which version you tested.

## Signing keys

- Mistel signs its own packs with keys the maintainer keeps offline. A private key never enters this repository, an issue, a log or CI.
- The public keys and their fingerprints will be listed in `docs/keys.md` once the first key exists.
- A key that was exposed goes on a revocation list with the next release, and readers stop trusting it.
