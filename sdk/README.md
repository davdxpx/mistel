# SDK

Helper code in C for companions and code mods that run inside a game's process.

- **Hooks** at places the emulator left as the game wrote them, never over code it rewrote.
- **A mailbox** between the game's process and the launcher, so a companion can ask the game for a value or send it a command.
- **Guards** that keep a mod from taking the game down with it: pointer and vtable checks before a call, state gates (only in a menu, only in the field), a build gate (only on the build the mod was made for), work only on the game's own thread, and a quarantine that turns a mod off after crash loops.

It builds on the guest helper interface that Banana Native and Peanuts Native offer to companions.

Status: planned. Under the GNU General Public License v3.0 ([LICENSE](../LICENSE)).
