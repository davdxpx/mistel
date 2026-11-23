# Mistel's signing keys

Mistel signs its own cheat packs, so that a runtime can show that a file comes from Mistel and was not changed since ([spec, section 12](../format/spec/mncf-1.md#12-signatures-and-trust)). A signature is only a mark of origin: an unsigned file is used like any other, without a warning.

The keys a runtime may trust are in [`keys.toml`](keys.toml), a trust list in the format `mncf-keys/1`. A runtime can build it in or read it from here. Each key there has the name a runtime shows and its key id; the `[[revoked]]` entries are keys that are no longer trusted.

| Name | Key id | Since | State |
|---|---|---|---|
| | | | No key is published yet |

## How a key is made and used

The maintainer makes and keeps the secret key offline. It never enters this repository, a pull request, an issue, a log or a CI secret ([AGENTS.md](../AGENTS.md)).

1. On the maintainer's own machine, in a folder outside every repository (an external drive is best):

   ```bash
   mistel keygen "Mistel" --out <that folder>
   ```

   This writes `Mistel.key` (secret) and `Mistel.pub`. `mistel keygen` refuses to write the secret key into this repository, or into any git work tree that does not ignore it.
2. The public key goes into `keys.toml` as a `[[key]]` with `name`, `public` and `id`, the values in `Mistel.pub`, and into the table above, in a pull request.
3. Packs are signed offline: `mistel pack <pack>.mncp <files> --name "<pack name>" --sign <folder>/Mistel.key`, or `mistel sign` for one file. `mistel verify --trust <folder>/Mistel.pub` checks them.

## When a key must go

A key that was exposed, or whose secret file was lost, is revoked. Its id goes into `keys.toml` as a `[[revoked]]` entry with the next release, and the table above says since when. A runtime then refuses every file signed with it (`revoked`). A new key is made as above. [SECURITY.md](../SECURITY.md) says how to report an exposed key.
