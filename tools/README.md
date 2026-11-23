# Tools

The `mistel` command for people who make cheats and mods. Planned commands:

| Command | What it does |
|---|---|
| `mistel check` | Checks an `.mcf` file against the spec, and its patterns and original bytes against a game's code module |
| `mistel convert` | Turns Atmosphère cheat files into `.mcf`, and `.mcf` back into Atmosphère for other emulators |
| `mistel pattern` | Makes the shortest byte pattern that finds one place in a code module |
| `mistel port` | Tries a file's patterns on a new build and says which cheats still fit |
| `mistel sign`, `mistel verify` | Signs a file or pack with an author's key, and checks a signature |
| `mistel pack` | Puts several `.mcf` files into one signed pack |

Status: planned. Under the GNU General Public License v3.0 ([LICENSE](../LICENSE)).
