# Muninn

Mistel's analysis part, named after one of Odin's two ravens: Muninn flies out over the world each day and comes back to tell what it saw.

Muninn looks into a running game and reports back:

- **Memory scans:** find a value, keep playing, narrow the candidates down (equal, changed, bigger, smaller) until one address is left.
- **Pointer paths:** a path from the game's code module to that address that still holds after a restart.
- **Watchpoints and call traces:** which code reads or writes an address, and how it got there.
- **Signatures:** byte patterns for the places it found, so that a finding survives a game update.

What Muninn finds feeds the cheat format, the engine adapters and the SDK ([docs/architecture.md](../docs/architecture.md)). Everything it reads from a game stays in that game's work folder.

Status: planned. Under the GNU General Public License v3.0 ([LICENSE](../LICENSE)).
