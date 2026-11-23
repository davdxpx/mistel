# How Mistel fits together

This is the plan for the parts and how data moves between them. Each part's own `README.md` says what of it exists.

## Where Mistel sits

The games run natively on Android: an nxnative-based compatibility layer (Banana Native, Peanuts Native) runs the game's own ARM64 code on the phone's CPU, answers its system calls and translates its graphics. Mistel does not replace any of that. It adds what a player or a modder needs on top:

- **Cheats** that an emulator loads from `.mcf` files.
- **Mods** that change a game's code, with knowledge that survives an update.
- **Companions**: extra programs and screens that talk to a running game, for example on a handheld's second screen.

## The flow

1. **A dump folder.** A player's own dump of their own game: base, updates, DLC. Mistel reads it and keeps everything it extracts in that folder's work folder.
2. **Muninn finds things.** It scans memory while the game runs, follows pointer paths, traces calls and sets watchpoints. What it finds becomes a byte pattern or a pointer path that holds across versions, not a fixed address.
3. **Adapters know engines.** What holds for every game on one engine (Unreal Engine first) lives in its adapter, so that one finding helps many games.
4. **The results.**
   - A cheat becomes an entry in an `.mcf` file ([format/](../format/)).
   - A code mod or a companion uses the SDK's hooks, its mailbox to the launcher, and its guards ([sdk/](../sdk/)).
5. **The tools** check, convert, port, sign and pack what authors make ([tools/](../tools/)).
6. **The emulator loads it.** It reads `.mcf` files with the MIT reader, checks the signature, compares the original bytes before every write, and shows the player what is on.
7. **The lab tests it** on a real handheld, with a test build and a copied save ([lab/](../lab/)).

## Principles

- **Survive updates.** Patterns and pointer paths instead of fixed addresses; a check of the original bytes before every write, so that a cheat for another version stays off instead of breaking the game.
- **Fail safe.** A failed check turns one cheat or one hook off and says why; the game and the other cheats keep running.
- **One rule for many games.** Prefer engine-wide knowledge to one game's exceptions.
- **Nothing from the game in the repository.** Only Mistel's own code, specs and made-up test data.
