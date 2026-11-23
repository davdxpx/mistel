# Tools

The `mistel` command for people who make cheats and mods. Planned commands:

| Command | What it does |
|---|---|
| `mistel check` | Checks an `.mncf` file against the spec, and its patterns and original bytes against a game's code module |
| `mistel convert` | Turns cheat files of other formats into `.mncf`, keeping every cheat and adding what can be read from its code (what it writes, which button it waits for, its values). Atmosphère first, then the cheat and patch files of other emulators and PC cheat tables. Writes `.mncf` back into an older format for a cheat that fits it |
| `mistel pattern` | Makes the shortest byte pattern that finds one place in a code module |
| `mistel port` | Tries a file's patterns on a new build and says which cheats still fit |
| `mistel sign`, `mistel verify` | Signs a file or pack with an author's key, and checks a signature |
| `mistel pack` | Puts several `.mncf` files into one signed pack |

Status: planned. Under the GNU General Public License v3.0 ([LICENSE](../LICENSE)).
