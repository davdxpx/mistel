# The Mistel Native Mod Format, version 1

Version lines: `format = "mnmf/1"` for a mod, `format = "mnmf-pack/1"` for a pack. File extensions: `.mnmf` (Mistel Native Mod Format) for one mod, `.mnmp` (Mistel Native Mod Pack) for a pack. Status: **draft**. Until it is marked final, the format can still change.

`.mnmf` is the sibling of the cheat format [`.mncf`](mncf-1.md). It takes over the conventions, the platform registry, the signatures and the trust lists of `mncf/1`, and it carries every change to a game's code as an `.mncf` file.

## 1. Purpose

A `.mnmf` file is one mod for one game: the files it changes, the code it patches, and everything a player needs to choose it, for every version of the game it knows. A program that reads it knows from the file itself:

- what the mod is: its name, authors, kinds, pictures, description and versions, for a list and for a full page, without unpacking anything,
- which versions of the game it was made for or tested with, and for each part of the mod whether it works there,
- which game files each part replaces, adds or removes, and for which original file each replacement was made,
- which choices and options the player has,
- which other mods it needs or cannot run with,
- who made it, and whether it was changed since.

When the game is updated, every part of a mod whose original files are unchanged keeps working, and the runtime knows that it does. A part whose original files changed, or that the update breaks otherwise, is switched off with a reason. The other parts, the other mods and the game keep running.

The container is made for speed: the metadata and a thumbnail are the first bytes of the file, files that are already compressed are stored as they are, every file has a hash, and a runtime can install a mod while it downloads, or read it in place. Nothing in a mod is ever converted with loss.

## 2. Conventions

- The key words, and the words **reader** and **runtime**, are used as in `mncf/1` section 2. A runtime here is a program that applies mods to a game: an emulator, a compatibility layer, a recompiled game, or a mod manager that prepares files for one of them.
- A **manifest** (`mod.toml`, `pack.toml`) is TOML 1.0 with the rules of `mncf/1` section 2: UTF-8 without a byte order mark, LF line ends, `format` as the first key, no date or time values, tables and arrays nested at most 128 levels deep.
- An **id**, a **build key**, integers and hexadecimal digits are as in `mncf/1` section 2.
- A **mod id** names a mod wherever it is: an owner and a name, each an id, joined by `/`: `"ann/crisp-ui"`. The owner is the handle of the author or the group. Two different mods MUST NOT share a mod id, and a new version of a mod keeps it. Packs (section 17) have mod ids too, from the same names.
- A **version** is one or more whole numbers joined by `.`, optionally followed by `-` and a label of letters, digits and `.`: `"1.2.0"`, `"2.0-beta.3"`. Two versions compare number by number, a missing number counting as 0, so `"1.2"` and `"1.2.0"` are the same version. A version with a label comes before the same version without one, and two labels compare as text, byte by byte. A version that is not of this form is shown as it is and compares with nothing.
- A **path** names a file inside a target or a container: parts separated by `/`, none of them empty, `.` or `..`, with no `\`, no control character (below U+0020, and U+007F) and at most 768 bytes of UTF-8. Paths compare byte for byte.
- A **hash** is a SHA-256 hash, written as 64 lower-case hexadecimal digits.
- A **date** is a string of the form `YYYY-MM-DD`.
- A **picture** is a reference to an image in the container: an inline table with `file`, the entry's name, and `sha256`, its hash (section 9).

## 3. A first look

A mod is a ZIP file. Its first entry is the manifest, `mod.toml`:

```toml
format = "mnmf/1"
id = "ann/crisp-ui"
name = "Crisp UI"
version = "1.2.0"
authors = ["Ann"]
summary = "Sharper menus for Example Quest."
kinds = ["ui"]
table = { sha256 = "2e4060e6ca029435f78cfee0a4124dee0cadedf874b0ac54564840911e9d830d", files = 1, bytes = 48213 }

[game]
platform = "switch"
id = "0100000000ABC000"
name = "Example Quest"

[builds."1.0.2"]
modules = { main = "A1B2C3D4E5F60718" }
status = "made-for"

[[part]]
id = "menus"
name = "Menus"
```

Its file table, `files.tsv`, lists each file the mod changes, with the hash of the original game file it was made for (the hashes are shortened here):

```
part	target	path	size	sha256	expect
menus	romfs	ui/layout/menu.blarc	48213	c00b72b0...	1edcdfcf...
```

On build 1.0.2 the part works, as its author says. On a build the mod does not list, the runtime compares the game's own `ui/layout/menu.blarc` with `expect`. If it is still the same file, the part works there too, and the runtime says so. If the update changed it, the part is off with the reason `file-changed:romfs/ui/layout/menu.blarc`, and the game starts without it.

## 4. The container

### 4.1 A ZIP file with a fixed layout

A `.mnmf` file is a ZIP file as PKWARE's APPNOTE (version 6.3.10) describes it, for the same reason that an `.apk` or an `.mncp` pack is one: archive programs can open it (its Zstandard entries need one that knows method 93), and its extension lets a phone or a desktop open it in the right program. It keeps these rules:

- `mod.toml` is the first entry, stored without compression, and its local header is at offset 0: no byte comes before it.
- A mod with a logo has its icon (section 9.2) as the second entry, stored.
- The file table, `files.tsv`, comes before every entry of a part.
- Each entry is stored (method 0), deflated (method 8, RFC 1951) or compressed with Zstandard (method 93, RFC 8878). Nothing is encrypted: general purpose bit 0 is clear. Each local header carries the entry's sizes and CRC-32, so there are no data descriptors: bit 3 is clear.
- No name appears twice. Names are paths (section 2) in UTF-8, and general purpose bit 11 is set when a name has a byte above 0x7F. An entry whose name ends in `/` is a folder and is ignored.
- A reader MUST support ZIP64 (APPNOTE section 4.5.3), which a mod above 4 GiB or with more than 65,535 entries needs.
- Each local header agrees with its central directory record in the name, the method, the flags, the sizes and the CRC-32. A reader that reads local headers first, for a list or a streaming install, checks this as soon as it reads the central directory, and refuses a mod where they differ.
- A Zstandard entry holds one or more frames one after the other, without a dictionary, without skippable frames, and with a window of at most 32 MiB.

A writer SHOULD store each entry whose data is already compressed (Zstandard and other compressed game archives, PNG, WebP, audio and video streams) and each entry that compression shrinks by less than 3 %, and SHOULD compress the others with Zstandard. A writer that has no Zstandard MAY deflate them instead. A writer MUST NOT change the bytes of any file: the compression is the container's alone, and it is lossless.

A writer SHOULD align the data of each stored entry of 64 KiB or more to a multiple of 4,096 bytes from the start of the file. It pads the local header with an extra field of ID 0xD935 (two bytes that give the alignment, then zero bytes), the field that Android's own tools write. A runtime can then map such an entry straight into memory.

### 4.2 Entries and hashes

| Entry | What | Section |
|---|---|---|
| `mod.toml` | The manifest | 5 |
| `files.tsv` | The file table | 11 |
| `images/...` | The logo, the icon, the banner, the gallery and the other pictures | 9 |
| `parts/<part>/<target>/<path>` | The files of each part | 11 |

Every entry is covered by a hash: a signature (section 16) covers `mod.toml`; `mod.toml` holds the hash of the file table and of each picture; the file table holds the hash of each file of the parts. A hash is always of the entry's bytes after decompression, so it does not depend on how the entry is stored. Entries that neither the manifest nor the file table names are ignored.

A writer SHOULD write the entries in this order: `mod.toml`, the icon, `files.tsv`, the logo, the banner, the parts' pictures, the kinds' icons, the gallery's thumbnails, the gallery's images, then the parts' files in the order of the file table.

### 4.3 What a runtime reads when

| Moment | What the runtime reads | Cost |
|---|---|---|
| A list of mods | The start of the file: `mod.toml` and the icon | One read. A writer SHOULD keep both within the first 64 KiB |
| A mod's page | The central directory, then the logo, the banner and the gallery's thumbnails; a full gallery image when the player opens it | A few reads |
| Checking a build | `mod.toml`, the file table, and the hashes of the game's original files (section 15.3) | No file of the mod |
| Installing | Each entry once, in file order, hashing it while it writes it | One pass, which can start while the file still downloads |
| Starting the game | Nothing, once the mod is installed. A runtime that uses the mod in place reads stored entries where they are | None |

## 5. The manifest

| Key | Type | Required | Meaning |
|---|---|---|---|
| `format` | string | yes | `"mnmf/1"`, the first key of the manifest |
| `id` | mod id | yes | Section 2 |
| `name` | string | yes | The mod's name, shown to the player |
| `version` | version | yes | The mod's own version |
| `authors` | array of strings | no | Who made the mod, each with the platform where they are known, as in `mncf/1`: `"Ann (example forum)"` |
| `credits` | array of inline tables | no | Others who helped, each with `name` and `role`: `{ name = "Cara", role = "Fonts" }` |
| `license` | string | no | An SPDX license expression, such as `"CC-BY-4.0"` or `"MIT OR CC0-1.0"`. A license SPDX does not list is `"LicenseRef-"` and a name, explained in `about` |
| `summary` | string | no | One line for lists, at most 160 characters, plain text |
| `about` | string | no | The description, in the markup of Appendix E |
| `changes` | array of tables | no | What changed in each version, newest first: `version` and `about` (markup) |
| `released` | date | no | The day this version came out |
| `links` | inline table | no | Addresses for people, each starting with `https://`: `home`, `source`, `support`, `donate` and `video` |
| `language` | string | no | The language of the plain strings, a BCP 47 tag. Default `"en"` |
| `kinds` | array of strings | no | What the mod is about, section 8. The first is its main kind |
| `tags` | array of strings | no | Section 8.3 |
| `origin` | inline table | no | Where a converter took the mod from: `format`, `name` and `site`, as in `mncf/1` section 4 |
| `extensions` | table | no | `required` and `optional`, section 18 |
| `game` | table | yes | Section 6 |
| `builds` | table | no | Section 6 |
| `needs` | table | no | Section 14.2 |
| `requires`, `conflicts`, `load_after` | arrays | no | Section 14.1 |
| `table` | inline table | yes | The file table: `sha256`, its hash; `files`, its number of rows; `bytes`, the sum of the sizes in its rows, a row that removes a file counting 0. A page can show how big the mod is without reading the table. A reader checks all three when it reads the table |
| `images` | table | no | Section 9 |
| `kind` | table | no | The mod's own kinds, section 8.2 |
| `part` | array of tables | yes | Section 10 |
| `choice` | array of tables | no | Section 10.2 |
| `i18n` | table | no | Translations, section 7.2 |

## 6. The game and its builds

```toml
[game]
platform = "switch"
id = "0100000000ABC000"
name = "Example Quest"

[builds."1.0.2"]
modules = { main = "A1B2C3D4E5F60718" }
status = "made-for"

[builds."1.0.3"]
version = "1.0.3"
modules = { main = "1122334455667788" }
status = "works"
parts = { "bold-fonts" = "broken" }
note = "The bold fonts cut off the new quest log of 1.0.3."
```

- `game`, and the `version` and `modules` of each build, are as in `mncf/1` section 5, and so is the way a runtime identifies the build that runs. A recompilation names the build key itself.
- `status` is what the authors say about the whole mod on that build:

| `status` | Meaning |
|---|---|
| `"made-for"` | The mod was made for this build |
| `"works"` | The authors tested the mod on this build, and it works |
| `"broken"` | The mod does not work on this build |

- `parts` maps part ids to a status for that build, which holds for that part instead of the build's `status`. `note` says more about the build, for people.
- A build without `status` says nothing about the mod as a whole. It names a version for other reasons: a status of single parts, or the cheats of a part.
- A mod MAY list no builds at all. Then its file table alone decides where it works (section 15.3).

## 7. Text

### 7.1 Descriptions

`summary` is plain text. `about`, the `about` of `changes`, of a part and of a choice, are written in the markup of Appendix E: a small part of CommonMark, so that any Markdown renderer shows them as their author meant, and a runtime without one can show them as plain text.

### 7.2 Translations

Plain strings are in the mod's `language`. Translations live in `i18n`, by BCP 47 tag, as in `mncf/1` section 9:

```toml
[i18n.de]
name = "Klare Menüs"
summary = "Schärfere Menüs für Example Quest."

[i18n.de.part.menus]
name = "Menüs"

[i18n.de.choice.scale]
name = "Menügröße"

[i18n.de.gallery.menu]
caption = "Das Hauptmenü in 150 %"
```

`i18n.<tag>` MAY hold `name`, `summary`, `about` and `changes` (an array as long as the mod's `changes`, of tables with `about`). `i18n.<tag>.part.<id>` and `i18n.<tag>.choice.<id>` MAY hold `name` and `about`, `i18n.<tag>.gallery.<id>` `caption`, and `i18n.<tag>.kind.<id>` the `name` of one of the mod's own kinds. A runtime uses the translation for the player's language and falls back to the plain strings.

## 8. Kinds and tags

### 8.1 Standard kinds

A kind says what a mod or a part is about, so that a runtime can show it with an icon, and a player can filter by it. The first group says what the mod changes for the player, the second what it carries. A mod names as many as fit, the most important first: its first kind is its main kind, whose icon stands for the mod where there is room for one.

A runtime SHOULD show each standard kind with its own icon. The last column names a symbol of Google's Material Symbols for each, as a suggestion, so that runtimes look alike; a runtime MAY draw its own.

| Kind | What the mod changes | Symbol |
|---|---|---|
| `performance` | The frame rate, the resolution, loading times | `speed` |
| `graphics` | Lighting, shadows, effects, sharpness | `auto_awesome` |
| `characters` | Characters and how they look | `person` |
| `outfits` | Clothes, armour and accessories | `checkroom` |
| `gameplay` | Rules, balance, difficulty and items | `sports_esports` |
| `convenience` | Fewer steps, faster menus and other quality of life | `done_all` |
| `camera` | The camera's distance, field of view and control | `videocam` |
| `controls` | Button layouts and input | `gamepad` |
| `ui` | Menus, the HUD and icons | `dashboard` |
| `text` | The game's text | `notes` |
| `translation` | The game in another language | `translate` |
| `audio` | Sound effects | `volume_up` |
| `music` | Music | `music_note` |
| `voice` | Voices | `record_voice_over` |
| `video` | Cutscenes and movies | `movie` |
| `levels` | Maps, worlds and layouts | `map` |
| `content` | New items, quests, characters or modes | `add_box` |
| `fixes` | Fixes for bugs of the game | `build` |
| `restoration` | Cut or changed content brought back | `history` |

| Kind | What the mod carries | Symbol |
|---|---|---|
| `models` | 3D models | `view_in_ar` |
| `textures` | Textures | `texture` |
| `animation` | Animations | `animation` |
| `effects` | Particles and other visual effects | `flare` |
| `shaders` | Shaders | `gradient` |
| `fonts` | Fonts | `font_download` |
| `scripts` | Game logic and event scripts | `data_object` |
| `data` | Tables and parameters | `table_chart` |
| `code` | Changes to the game's code (section 12) | `code` |
| `cheats` | Cheats in `.mncf` files | `bolt` |

### 8.2 A mod's own kinds

A mod MAY define kinds of its own, with an icon:

```toml
[kind.x-photo-mode]
name = "Photo mode"
icon = { file = "images/kinds/x-photo-mode.png", sha256 = "f56c02056286d4806cc562eb6ed9d6703398215262ed97f7d8ab202dbf09d528" }
```

- The id of such a kind starts with `x-`. `name` is required, `icon` optional.
- The icon is 96 × 96 pixels, white on a transparent background, so that a runtime can tint it in the colours of its interface (section 9).
- A kind that is neither standard nor defined in `kind` is a warning (`kind:<id>`) and is left out.
- New standard kinds are added to this spec by a pull request.

### 8.3 Tags

The tags of `mncf/1` section 7.8 (`touches_save`, `may_crash`, `cosmetic`, `online_unsafe`) mean the same for a mod or a part. Two more are defined:

| Tag | Meaning, and what a runtime SHOULD do |
|---|---|
| `adult` | The mod shows sexual content or nudity. Hide its pictures until the player chooses to see them, and offer a setting that hides such mods from lists |
| `spoilers` | Its pictures or text give away later parts of the game. Hide its pictures until the player chooses to see them |

Unknown tags are shown as they are.

## 9. Pictures

### 9.1 Formats

Every picture is lossless: a PNG (ISO/IEC 15948) without animation, that is without an `acTL` chunk, or a lossless WebP (RFC 9649): a `VP8L` bitstream, without a `VP8 ` bitstream and without animation. Colours are sRGB. Each picture is at most 16 MiB and has the size its key gives.

One exception keeps screenshots as they were taken: a gallery picture (its `image`, never its `thumb`) MAY be a JPEG (ISO/IEC 10918-1, baseline or progressive) exactly as the console or the game captured it. Turning it into a PNG would only make it larger, never better. A packer MUST NOT re-encode a JPEG, and MUST NOT turn any picture into one.

A picture that breaks a rule of this section is a warning (`image:<key>`) and is not shown. Pictures never stop a mod from working.

### 9.2 Logo, icon and banner

```toml
[images]
logo = { file = "images/logo.png", sha256 = "329d2cce2fab124467930b42879e4459a09bc7cea4ba07c4d49f52baf51f16c9" }
icon = { file = "images/icon.webp", sha256 = "6f9155c33f1f50a093145d8ac0eee396fba93775e287a5f6c5fc192ad2fe14e3" }
banner = { file = "images/banner.webp", sha256 = "d8b81a97d7d44ac0e3edc1a02246a38b3ccd2cc6a01cde3ea882a9bada58a093" }
```

| Key | Size in pixels | Meaning |
|---|---|---|
| `logo` | 512 × 512 | The mod's square picture: its cover in a grid and on its page. It MAY be transparent |
| `icon` | 128 × 128 | The logo, scaled down for lists. Required where there is a logo, and the second entry of the container. The packer makes it from the logo |
| `banner` | 1920 × 1080 | A wide picture for the top of the mod's page |

A mod without a logo is shown with the icon of its main kind.

### 9.3 Gallery

```toml
[[images.gallery]]
id = "menu"
image = { file = "images/gallery/menu.png", sha256 = "a6e95a8d0711d7648de29c3afc55dbd9262253f619f965eaf4a387e8edcfb8c1" }
thumb = { file = "images/gallery/menu.thumb.webp", sha256 = "3d8f660b5fa63a68ed3c5136b9dc58801c0747360bcb7bb70782c54afc9578a0" }
width = 1920
height = 1080
caption = "The main menu at 150 %"
part = "scale-150"
```

| Key | Required | Meaning |
|---|---|---|
| `id` | yes | An id, unique in the gallery |
| `image` | yes | The picture. Each side from 256 to 3840 pixels. A screenshot SHOULD keep the game's own resolution |
| `thumb` | yes | The picture scaled so that its longer side is 384 pixels. The packer makes it |
| `width`, `height` | yes | The size of `image`, so that a page can lay itself out before it loads the picture |
| `caption` | no | Plain text |
| `tags` | no | `adult` and `spoilers` (section 8.3) for this picture alone |
| `part` | no | The part the picture shows, so that a page can show it beside that option |

A gallery has at most 64 pictures, in the order the page shows them.

### 9.4 A part's picture

A part MAY have a picture of its own, `part.image`: 256 × 256 pixels, shown beside its name, so that a player sees an option (a colour, an outfit) before choosing it.

## 10. Parts, choices and options

### 10.1 Parts

A **part** is one unit of a mod: files, cheats or both. A part is applied whole or not at all, the way a cheat is.

```toml
[[part]]
id = "menus"
name = "Menus"
kinds = ["ui"]

[[part]]
id = "bold-fonts"
name = "Bold fonts"
about = "Thicker letters, easier to read on a small screen."
kinds = ["fonts"]
optional = true
```

| Key | Type | Required | Meaning |
|---|---|---|---|
| `id` | id | yes | Unique among the mod's parts. It also names the part's entries in the container |
| `name` | string | for options | Shown to the player. Required for a part that is optional or the option of a choice |
| `about` | string | no | Markup (Appendix E) |
| `image` | picture | no | Section 9.4 |
| `kinds` | array of strings | no | Section 8 |
| `tags` | array of strings | no | Section 8.3 |
| `optional` | boolean | no | The player switches this part on and off. Default `false` |
| `default` | boolean | no | For an optional part: whether it is on the first time. Default `false` |
| `requires` | array of ids | no | Other parts this one needs |
| `excludes` | array of ids | no | Other parts that cannot be on together with this one |
| `cheats` | array of ids | no | The cheats of the part's `.mncf` files that are on while the part is, section 12 |
| `values` | inline table | no | Values for those cheats, section 12 |
| `indexes` | array of strings | no | The index rules the part needs, section 13 |

A part that is neither optional nor the option of a choice is a **base** part. The base parts are on whenever the mod is. Every other part needs all of them: while a base part is refused, every other part is refused with `requires:<id>`, naming the first base part that is refused.

Each part has at least one row in the file table; a part without one is refused with `invalid`.

### 10.2 Choices

A **choice** offers parts of which exactly one is on, such as a resolution, a colour or a frame rate:

```toml
[[choice]]
id = "scale"
name = "Menu size"
options = ["scale-125", "scale-150"]
default = "scale-150"
```

| Key | Type | Required | Meaning |
|---|---|---|---|
| `id` | id | yes | Unique among the mod's choices |
| `name` | string | yes | Shown to the player |
| `about` | string | no | Markup |
| `options` | array of ids | yes | The parts to choose from, in the order they are shown |
| `default` | id | see below | The option that is on the first time |
| `none` | boolean | no | `true`: the player may also choose none of them. Default `false` |

- Without `none`, a choice has at least two options and a `default`. With `none = true`, it has at least one, and without a `default` none is on the first time.
- A part is the option of at most one choice, and an option is never `optional`.
- The options of a choice exclude each other.
- When the option that is on is refused, the runtime does not switch to another one by itself: the option is off with its reason, and the runtime SHOULD offer the player the options that work.

### 10.3 Switching parts

The first time a player switches a mod on, its base parts, its optional parts with `default = true` and the `default` of each choice are switched on. `requires` and `excludes` work as in `mncf/1` section 7.8: switching a part on switches the parts it requires on and the parts it excludes off; a part that can never be on is refused with `excludes:<id>`; a part that requires a refused part is refused with `requires:<id>`.

A runtime keeps, for each game, which mods are on, the option of each choice, whether each optional part is on, and the load order (section 14.3). What the player changes takes effect when the game starts the next time, because the files of a running game cannot change under it. The cheats of a part MAY follow at once.

## 11. Files

### 11.1 Targets

A **target** is where a file goes: a part of the game's own files, such as the RomFS of a Switch game, or `cheats`, the runtime's place for the part's `.mncf` files (section 12). Appendix A lists the targets of each platform.

### 11.2 The file table

The file table is the entry `files.tsv`. It is the one part of a mod that grows with the mod's size, so it is plain tab-separated text that a reader takes apart at the speed of memory, line by line, also while the file streams in. It is UTF-8 without a byte order mark, every line, the last one too, ends with LF, and no line holds a CR.

```
part	target	path	size	sha256	expect
menus	romfs	ui/layout/menu.blarc	48213	c00b72b0bc87b0848ca8572d3cc31ce0fd350a3c1153d05bcafab7ee0f3c6a5e	1edcdfcfdc6a5096d6bd5ed6122ee78eb36a898339f76ad32f6598869dffc341,d99c2adccfe2c596032377357b25538a83c3fd9fd413f49b10928abc9acdf27e
```

- The first line names the columns, separated by single tabs. It names `part`, `target`, `path`, `size`, `sha256` and `expect`, each once, in any order. `builds` and `entry` are optional. A column of an extension is named by that extension, or starts with `x-`; a reader ignores the columns it does not know.
- Each further line is one row, with exactly as many fields as the first line has names, separated by single tabs. A field holds no tab and no line end.
- `part` is the id of a part of the manifest, and `target` a target of the game's platform (Appendix A). `path` is a path (section 2) inside the target, from its root, without a leading `/`.
- `size` is the size of the file in bytes, in decimal digits, and `sha256` its hash. In a row that removes a file, both are `-`.
- `expect` says what the original file in the game is (section 11.3).
- `builds` names the builds the row is for: build keys of the manifest, joined by `,`. Where it is empty or the table has no such column, the row is for every build. A mod whose files differ between versions of the game (a folder per version, as many mods ship) keeps one row per version of the file, each for its builds.
- `entry` is the container entry that holds the file's bytes. Where it is empty or the table has no such column, that is `parts/<part>/<target>/<path>`, followed by `@<n>` for the n-th row (from 2) of the part with that target and path. Otherwise it is the entry of an earlier row with the same hash, so that a file that several parts or builds share is stored once.
- Two rows of one part with the same target and path are for builds that do not overlap, each naming its builds; this also holds for paths that differ only in the case of ASCII letters, since many file systems cannot hold both.
- The container SHOULD hold the parts' entries in the order of the table, so that a streaming install meets them in that order.

A file table that breaks one of these rules makes the mod unusable (`table:<line>`, Appendix D).

### 11.3 Expect

| `expect` | The row fits when |
|---|---|
| One or more hashes, joined by `,` | The game has a file at the path, and its bytes have one of these hashes: the originals the file was made for or tested with |
| `-` | The game has no file at the path: the mod adds it |
| `*` | Always: the file works whatever the game has at the path, or nothing |
| `?` | Not known: nobody checked which original the file was made for |

- The hash of an original is the hash of the file's bytes as the game reads them: in its target, or through the game's own archives where an index rule (section 13) says so.
- When an update changes an original, the rows that name it no longer fit, and their part is off (section 15.3). When the authors check that the file still works with the new original, they add the new original's hash to `expect`.
- Hashes of originals hold nothing of the game. `mistel` fills them in from the player's own dump, and a runtime that converts a mod from the game it runs.
- `*` is for files that do not depend on the original at all. `mistel check` warns about every `*` and every `?`. A runtime that converts a mod from the game it runs fills in the hash of the original it finds there, not `*`: then an update that changes the original is noticed.
- In a row of the `cheats` and `exefs-patch` targets, which hold no game file, `expect` is `-`.

### 11.4 Adding, replacing and removing

- A row with a hash in `sha256` puts its file at its path: it replaces the original file there, or adds the file where the game has none.
- A row with `-` in `size` and `sha256` removes the original: the game then finds no file at the path. Such a row names its original in `expect`, by hashes or `*`; `-` and `?` are not allowed there. It needs the capability to remove files from its target (section 15.1).

### 11.5 Several parts and mods at one path

Within one mod, a part later in the manifest wins over an earlier one at the same target and path. Between mods, the mod later in the load order wins (section 14.3). A runtime SHOULD show, for each file at such a path, which mod's file the game gets.

## 12. Cheats and code

Changes to a game's code go into `.mncf` files, as rows of the `cheats` target in a part. A mod that makes the game run at 60 frames per second is a part with one `.mncf` file whose cheat patches the code, with the original bytes checked before every patch. The same mod then works on every build where its patterns find their places.

```toml
[[part]]
id = "fps"
name = "60 frames per second"
kinds = ["performance", "code"]
optional = true
cheats = ["fps-60"]
values = { "fps-60.target" = 60 }
```

- The part's `.mncf` files are read and checked as `mncf/1` says, for the game and the build that run. Their `game` is the mod's game. An `.mncf` file for another game, or one with an error of `mncf/1` Appendix D, refuses the part with `invalid:cheats`.
- `cheats` names the cheats that are on while the part is on. Without it, the cheats with `default = true` are. The ids of the cheats are unique across the part's `.mncf` files, and `cheats` names only cheats of these files (`invalid:cheats`).
- `values` sets the player values of these cheats, keyed by a cheat id and a value id joined by a dot, as a preset does in `mncf/1` section 8. A value that does not fit refuses the part with `invalid:values`.
- Before the game starts, the runtime gives each cheat of `cheats` its state for the build that runs (`mncf/1` section 11.2, step 4). A cheat refused there refuses the part with `cheat:<cheat id>:<reason>`, the reason being the cheat's own (`mncf/1` Appendix B). A cheat refused later, while the game runs, is shown with its reason; the part's files stay until the game ends, and the part is checked again at the next start.
- While the part is on, its cheats run as `mncf/1` section 11.5 says, except load patches (below). A runtime MAY show them on the mod's page and let the player set their values. It SHOULD NOT list them as switches of their own among the game's cheats.

### 12.1 Load patches

Many code mods change code that runs only while the game starts, before a runtime's first pass. So a cheat of a part is a **load patch** when it runs `"on"`, has no `hold`, `toggle`, `when` or `interval`, and does its work only through patches, overrides, and writes that have an `expect` and a place in a module without a pointer path (bytes in a module's data, as a patch file changes them):

- The runtime applies it while the game loads, before the game's first instruction runs, and it stays applied until the game ends. Its locations are resolved, and its `expect` checked, against the original code module. Its writes happen once, at that moment, and are not repeated in passes: they change the module's data as it is loaded.
- A runtime that changes the original code for its own reasons (`mncf/1` section 11.3) applies load patches before it does, so that they meet the original bytes, as a patch applied by the loader does.
- A runtime that cannot change code before the game's first instruction refuses the part with `capability-missing:code-load`.
- Values the player sets for a load patch take effect at the next start.

The other cheats of a part run pass by pass, as `mncf/1` says.

### 12.2 Code in other forms

A code module replaced whole (the `exefs` target of the Switch) holds code made for one build, needs a runtime that can load it (`capability-missing:exefs`), and does not survive an update.

A patch file as its author shipped it (the `exefs-patch` target of the Switch: an IPS or IPS32 file, or a pchtxt file) is kept where a converter could not turn it into an `.mncf` file (section 20), so that no patch is lost. Its name is the build ID of the module it patches and its extension, `.ips` or `.pchtxt`, as Atmosphère names them. The runtime applies it as a loader does, to the module with that build ID, before the game's first instruction, and only while that module runs: on any other build, the row is not for the build (section 15.3). It needs the capability `exefs-patch`.

The format carries both forms for the runtimes that use them. An `.mncf` load patch is the form that survives updates and is checked before it writes.

## 13. Indexes the runtime builds

Some games read their files through an index of their own: a table of every file and where it lies. A mod that adds or replaces such files needs an index that knows about them, and an index made for one build breaks another build. So a mod never carries an index. It names an **index rule**, and the runtime builds the index for the build that runs, from the game's own index and the files of every part that is on, in every mod that names the rule.

- A part's `indexes` names rules with their version: `["trinity-trpfd/1"]`. Rules are defined in this section and added to it by a pull request; until then their names start with `x-`.
- A part that names a rule has no row for the index file that the rule builds. Such a row refuses the part with `index-shipped:<target>/<path>`.
- A runtime that lacks a rule refuses the parts that name it with `index-unsupported:<rule>`.
- The runtime builds each index once for a build and a set of files that are on, and SHOULD keep it until either changes.
- A rule says how a runtime tells whether a path is in the game, for `-` and hashes in `expect`, and where the bytes of an original come from.

### 13.1 `trinity-trpfd/1`

For games on the Trinity engine, which keep their RomFS files in packs inside `arc/data.trpfs` and find them through the index `arc/data.trpfd`. Such a game reads a loose RomFS file in place of a packed one only when its index holds no entry for that path. A mod made the usual way therefore carries a `data.trpfd`: the index of the build it was made for, with the entries of its own files removed. On another build, that index lacks the files the other build added, and the game crashes when it starts.

The runtime makes `arc/data.trpfd` for the build that runs:

1. It reads the build's own `arc/data.trpfd`, a FlatBuffer whose root table has these fields:
   - 0: the hashes of the files' paths, unsigned 64-bit numbers, in ascending order, each once;
   - 1: the names of the packs, strings of the form `arc/<name>.trpak`;
   - 2: one table per file, in the order of field 0: its field 0 is the index of the file's pack in field 1 (unsigned 64-bit), its field 1 is four bytes;
   - 3: one table per pack: its field 0 is the pack's size, its field 1 the number of files in it (unsigned 64-bit each).
2. For each row of the `romfs` target in every part that is on and names the rule, it takes the hash of the row's path: FNV-1a with 64 bits over the path's UTF-8 bytes, from the RomFS root without a leading `/` and without any change of case, with the offset basis 0xCBF29CE484222645 (not the usual 0xCBF29CE484222325) and the prime 0x100000001B3.
3. It removes each of these hashes from field 0, and the entry at the same position from field 2, keeping the order of the others. Field 1, field 3 and any other field of the build's index stay as they are. A hash that field 0 does not hold is a file the mod adds; nothing is removed for it.
4. It puts the result in place of the game's own `arc/data.trpfd`, in the way it puts a mod's file.

Under this rule, a path is in the game when its hash is in field 0 of the build's own index, or when the RomFS holds a loose file at that path. A runtime that can read the files in the packs takes an original's bytes from there. A runtime that cannot checks only whether the path is in the game, and treats the hashes in `expect` as unchecked (section 15.3).

## 14. Mods together

### 14.1 Requires, conflicts and order

```toml
requires = [{ id = "ann/ui-base", min = "1.1" }]
conflicts = ["ben/big-menus"]
load_after = ["ann/ui-base"]
```

| Key | Meaning |
|---|---|
| `requires` | Mods this one needs, each an inline table with `id`, and optionally `min`, the lowest version that works (included), and `below`, the first version that no longer does (not included). Switching this mod on switches them on. A required mod the runtime does not have refuses the mod with `mod-missing:<id>`; one whose version is outside the range, or does not compare, with `version:<id>` |
| `conflicts` | Mods that cannot be on together with this one. Switching it on switches them off. A conflict holds both ways, whichever of the two names it |
| `load_after` | Mods this one comes after in the load order when both are on, so that its files win (section 14.3) |

A mod that can never be on, because the mods it requires conflict with each other or with it, is refused with `conflict:<id>`.

### 14.2 Needs

```toml
[needs]
dlc = ["0100000000ABD001"]
```

`dlc` names the add-on content the mod needs, by its ID on the platform (on the Switch, the add-on content's title ID). A runtime that knows which add-on content the player has refuses the mod with `dlc-missing:<id>` while one of them is missing. A runtime that cannot tell shows what the mod needs and does not refuse it.

### 14.3 Load order

A runtime keeps one load order per game: the mods that are on, from the first to the last. Where several mods write the same target and path, the game gets the file of the last of them; where one of them removes the file, the game gets what the mods after it write, or nothing.

A newly added mod goes to the end, after the mods its `load_after` names; a pack (section 17) brings its own order. The player MAY change the order. A `load_after` that the player's order breaks is shown, and does not refuse the mod.

An index rule takes the files of all mods that are on, whatever their order.

## 15. Runtimes

### 15.1 Capabilities

Each target is a capability of the same name, and removing files from a target is the capability `<target>-remove`:

| Capability | Needed by |
|---|---|
| `romfs`, `romfs-remove` | Rows of the `romfs` target, and the rows that remove from it |
| `exefs` | Rows of the `exefs` target |
| `exefs-patch` | Rows of the `exefs-patch` target |
| `game`, `game-remove` | Rows of the `game` target, and the rows that remove from it |
| `cheats` | Rows of the `cheats` target. Their cheats need the capabilities of `mncf/1` section 11.1 on top |
| `code-load` | Load patches (section 12.1) |

A part that needs a capability the runtime lacks is refused with `capability-missing:<capability>`, naming the first one its rows need, in table order, and `code-load` after the capabilities of the rows.

### 15.2 Applying mods

1. Read the manifest. Check `format`, the signature (section 16), the game and the required extensions.
2. Identify the build that runs (section 6).
3. Check the relations to other mods: `requires`, `conflicts` and `needs` (section 14).
4. Decide, for each part in manifest order, whether it fits the build (section 15.3), and give it a state.
5. Build the indexes (section 13).
6. Start the game with the files of every part that is on, in load order, and run their cheats.

A part is `on`, `off`, or `refused` with a reason (Appendix B). A mod is `on` while any of its parts is on, `off` when the player switched it off, and `refused` with a reason when it cannot be used at all. A runtime SHOULD show the reasons, on the mod's page for each part.

The reason is the first problem found in this order: the mod itself (`game-mismatch`, `signature`, a missing required extension, `hash`), its relations (`mod-missing`, `version`, `conflict`, `dlc-missing`), then for each part: its own text (`unknown-key` before `invalid`), its status on the build (`build-broken`), whether any row is for the build (`build-unknown`), capabilities, optional extensions, index rules, its rows in table order, its cheats in the order of `cheats`, and its relations to other parts. `unchecked` is given only where no other problem is found.

### 15.3 Whether a part fits a build

For the build that runs, the runtime takes the part's **stated status**: the part's own one under `builds.<key>.parts`, else the build's `status`, else none (also when the build is not in the mod). It takes the part's rows that are for this build: the rows without `builds`, the rows whose `builds` name the build that runs, and of the `exefs-patch` rows only those whose module runs. Then:

1. A stated `broken` refuses the part with `build-broken`.
2. A part none of whose rows is for this build is refused with `build-unknown`.
3. For each row that is for this build, in table order, the runtime compares the game's file at the row's target and path with `expect`:
   - hashes: no such file refuses the part with `file-missing:<target>/<path>`; a file whose hash is none of them with `file-changed:<target>/<path>`; a file whose bytes the runtime cannot learn leaves the row unchecked;
   - `-`: a file at the path refuses the part with `file-exists:<target>/<path>`;
   - `*`: the row fits;
   - `?`: the row is unchecked.
4. If no row refused the part but a row is unchecked, and the stated status is neither `made-for` nor `works`, the part is refused with `unchecked:<target>/<path>`, naming its first unchecked row.
5. Otherwise the part fits, in one of these ways, which a runtime SHOULD show:

| Fit | When |
|---|---|
| `stated` | The authors say `made-for` or `works` for this build, and no row contradicts them |
| `checked` | Every row fits by its `expect`: the runtime checked each original, and none is unchecked |
| `forced` | The player switched the part on although it was refused for one of the reasons below |

- A stated status never outweighs a row that does not fit: a file changed under a build that the authors call `works` still refuses the part.
- Every refusal switches the part off: nothing that may not fit runs by itself. A part refused for `build-broken`, `file-changed`, `file-missing`, `file-exists` or `unchecked`, and for no other reason, MAY be switched on by the player at their own risk, after the runtime has said why it is off. The runtime keeps that choice for that build only, so that the next update asks again, and shows the part as `forced`. A part refused for any other reason cannot be forced: the reason is a broken file, a missing capability or a rule of the mod itself.
- A runtime that converts a mod from the game it runs (section 20) MAY count the player's adding of the mod as that choice for the build it runs, for the parts refused only for `unchecked`: the player adds the mod for the game in front of them. After the next update, those parts are held off again until the player chooses.
- A runtime learns the hash of an original in the cheapest way it has: from the records of the game's own package where they hold one, through an index rule, or by hashing the original's bytes once. It SHOULD keep the hashes for each build, and the result for each mod and build, so that the check costs nothing at the next start.

### 15.4 Game updates

When the build that runs changes, the runtime decides again for every part, as section 15.3 says. What fits stays on, and shows how it fits. What no longer fits is off with its reason, and comes back by itself as soon as a build fits again: after a new version of the mod, or when the player goes back to an older build. The player's choices, options and load order are kept throughout.

### 15.5 Installing

- A runtime MAY install a mod by extracting its entries, or use the container where it lies: reading stored entries in place and extracting only the compressed ones.
- It checks the hash of each entry as it reads it. An entry whose bytes differ from its hash makes the whole mod unusable (`hash:<entry>`). An install leaves nothing half done: it writes into a new folder, which takes the old one's place only when every hash matched.
- It can install a mod while the file still downloads, since the manifest and the file table come first, and every local header holds its entry's sizes. The entries are independent, so it MAY decompress several at once.
- It MUST NOT write anywhere but its own folders, whatever a path says.
- Installing a new version of a mod, with the same mod id, keeps the player's choices for the parts and choices that are still there.

### 15.6 Keeping the player safe

- A runtime SHOULD back up the save before the game first starts with a part tagged `touches_save`, and offer to restore it.
- If the game ends abnormally soon after it started with a part that was newly switched on, a runtime SHOULD ask at the next start whether that part stays off.
- A runtime MUST NOT change the game's own files that the player gave it, such as a dump or an installed game: a mod changes only what the game sees while it runs, and the files a runtime makes from the game's (such as a rewritten code module) are its own.

## 16. Signatures and trust

Signatures work as `mncf/1` section 12 says, for the manifest. `mod.toml` MAY end with signature lines of the same form, with the prefix `# mnmf-signature: `:

```toml
# mnmf-signature: ed25519 3f2a9c4e8b1d7a60 kT9v...base64...==
```

- A signature covers every byte of `mod.toml` before the first signature line. Through the hashes in the manifest and in the file table, it covers every picture and every file of the mod.
- The results (`verified`, `unsigned`, `unknown-key`, `invalid`, `revoked`), the trust lists (`mncf-keys/1`) and the keys are those of `mncf/1` section 12. A mod that is `invalid` or `revoked` is refused whole, with `signature`.
- A signature is optional. A runtime MUST NOT warn about, hide or treat differently a mod only because it is unsigned; only `verified` gets a mark.
- The `.mncf` files of a part MAY carry signatures of their own, which the runtime checks as `mncf/1` says.

## 17. Packs

A pack bundles several mods for one game, with the setup its authors chose: which mods, in which order, with which choices and options, and which cheats. It is a ZIP file with the extension **`.mnmp`** (Mistel Native Mod Pack), laid out as section 4.1 says for a mod: its manifest `pack.toml` is the first entry, stored, at offset 0, and its icon, where it has a logo, the second.

```toml
format = "mnmf-pack/1"
id = "ann/quest-plus"
name = "Quest Plus"
version = "2.0.0"
authors = ["Ann"]
summary = "Clear menus, smooth play and a few helpers for Example Quest."
kinds = ["ui", "performance"]

[game]
platform = "switch"
id = "0100000000ABC000"
name = "Example Quest"

[[mod]]
file = "mods/crisp-ui.mnmf"
head = "b195b2c157f1c6cf92fad10caf750c195d16cada5dfc4e978cc0305efd519b47"
choices = { scale = "scale-125" }
options = { "bold-fonts" = true, fps = true }

[[mod]]
id = "ben/hd-trees"
min = "1.1"

[[cheats]]
file = "cheats/example-quest.mncf"
sha256 = "ebdb5568d6e4424ac97ba1c7e323664195fc7be0fb16b6394f2dd93eb79e5854"
on = ["money"]
```

- `format = "mnmf-pack/1"` is the first key. `id`, `name` and `version` are required. `authors`, `credits`, `license`, `summary`, `about`, `changes`, `released`, `links`, `language`, `kinds`, `tags`, `images` and `i18n` are as in a mod, so that a pack has a page of its own. `game` is required: every mod of the pack is for that game.
- Each `[[mod]]` is one mod of the pack. There is at least one, and their order is the pack's load order, the last one winning.
  - `file` names a `.mnmf` file inside the pack. It is stored, so that a reader reads it in place as a range of the pack's bytes, and `head` is the hash of its `mod.toml` (with its signature lines), which covers the whole mod.
  - Instead of `file`, `id` with `min` and `below` (section 14.1) names a mod the pack does not carry, which the player adds. Until then, it is missing (`mod-missing:<id>`).
  - `choices` maps the mod's choice ids to the option the pack chooses; `options` maps the mod's optional parts to `true` or `false`; `on = false` keeps the mod in the pack but off at first.
- Each `[[cheats]]` is an `.mncf` file of the pack, with `file`, `sha256`, and `on`, the cheats to switch on the first time.
- A runtime shows the pack as one thing with its mods inside, each with its own page. It applies the pack's setup the first time; the player can change it afterwards.
- `pack.toml` is signed as `mod.toml` is (section 16). Through `head` and `sha256`, its signature covers every mod and every cheat file of the pack.
- A mod inside the pack whose `mod.toml` does not have the hash `head`, or a cheat file that does not have its `sha256`, is not used (`hash:<file>`). A pack whose manifest is `invalid` or `revoked` is not used at all.

## 18. Extensions

Extensions work as `mncf/1` section 10 says, with names such as `x-merge-msbt/1`, listed in `extensions` as `required` or `optional`. A key in a part that neither the core nor a listed extension defines refuses that part with `unknown-key:<key>`; anywhere else it is a warning. The columns of the file table count as keys. `mnmf/1` defines no extension yet.

## 19. Versions

- The version line names the major version. Within `mnmf/1`, a newer reader reads every older mod and pack.
- New features come as extensions (section 18), index rules (section 13), standard kinds, tags and targets, never as new meanings of existing keys.
- A change that breaks a rule of this section is `mnmf/2`.

## 20. Converting other mods

A converter (`mistel mod convert`, and the MIT libraries for runtimes that import mods) keeps every file of the mod it reads. With the game's own files at hand (the player's dump, or the game a runtime runs), it fills `expect` from the originals and names the build they come from; without them, `expect` is `?`. A runtime that detects mods in folders of other layouts splits them by the same rules, so that a folder and its converted `.mnmf` file give the same mods, parts and choices.

### 20.1 Layouts

| Source | Recognized by | Becomes |
|---|---|---|
| A RomFS folder | A folder `romfs` | Rows of the `romfs` target |
| Atmosphère's layout | `atmosphere/contents/<title ID>/romfs` and `.../exefs` | The game's ID from the folder name. Code modules in `exefs` become rows of the `exefs` target |
| Atmosphère's code patches | `atmosphere/exefs_patches/<name>/<build ID>.ips` | Code patches (section 20.3) |
| An emulator's mod folder | `<name>/romfs`, `<name>/exefs` and `<name>/cheats`, inside a folder named by the title ID | One mod for each `<name>` folder |
| IPS and IPS32 patches | `.ips` files in `exefs` or `exefs_patches`, named by a build ID | Code patches (section 20.3) |
| pchtxt patches | `.pchtxt` files with an `@nsobid-<build ID>` line | Code patches (section 20.3) |
| A Trinity mod | `romfs/arc/data.trpfd` beside other RomFS files | The index is left out, and the parts name `trinity-trpfd/1` |
| Cheats | `cheats/<build ID>.txt` | An `.mncf` file, as `mncf/1` section 15 says, in an optional part |
| Pictures and texts | Images, and readme or credits text files, beside the mod's folders | Gallery pictures, where they are PNG, lossless WebP, or JPEG as captured; a square one of at least 512 pixels as the logo, scaled and stored losslessly; `about` and `authors` from the texts |

### 20.2 Splitting an archive into mods, parts and choices

A **unit** is a folder that holds a RomFS folder, code patches or cheats in one of the layouts above.

1. **Wrappers.** A folder that is the only child of its parent, apart from readmes and pictures, is stepped into.
2. **Versions.** Units named by a version of the game (`1.0.0`, `For 2.0.2`), and patch sets for several builds, are the same mod for different builds: their files become rows for those builds (`builds`, section 11.2), not a choice.
3. **The main unit.** Units named `Optional`, `Extras`, `Add-ons` or `Addons`, or numbered after a unit numbered 1 (`1 Main`, `2 Bigger text`), belong to the main unit: the one numbered 1, or the one that has none of these names. The main unit's files become the base part, and each of the others an optional part, off at first.
4. **Mods side by side.** Among the units at the top that are left, those that share no path with any other become separate mods, and with more than one of them, the archive becomes a pack (section 17), its mods in the order of their names. Inside one mod's folder, units that share no path with any other become optional parts, off at first.
5. **Variants.** Units that write some of the same paths, at the top or inside one mod, are variants: they become the options of one choice, in the order of their names, the first its default. Where the choice lies over a base part, it gets `none = true`, since the base works without an option.
6. **Layers.** A unit that holds only some of the base part's paths is an option over the base: its files win, as a later part's do (section 11.5).
7. **Names.** Parts, choices and mods take their names from the folders, and their ids from the names in lower case, with every run of other characters than letters and digits made into one `-`.

A converter MAY ask the person who converts where these rules leave a doubt, and says in its report how it split the archive.

### 20.3 Code patches

With the game's code module at hand, which gives each patch its original bytes for `expect`, code patches become `.mncf` files as `mncf/1` section 15 says for patch files (IPS, IPS32 and pchtxt): one `.mncf` file for each patch set, with a build for each build ID the patches name, and patterns where that section allows them, so that the patches also work on later builds. The cheats are load patches (section 12.1).

- An IPS or IPS32 patch set becomes one cheat, `default = true`, in an optional part that is on at first.
- Each patch of a pchtxt file becomes one cheat and one optional part that switches it on, on at first where the patch is `@enabled`.
- Without the module, a converter cannot give `expect`, which a patch in an `.mncf` file needs (`mncf/1` section 7.4). It then keeps each patch file as it is, as a row of the `exefs-patch` target, in an optional part that is on at first, and names the files in its report.

### 20.4 What else a converter writes

- The mod id is `local/<name>`, with the id made from the mod's name, unless the person who converts names an owner.
- `origin` and `authors`, as `mncf/1` section 15 says. Credit lines and links from the source's texts go into `about`.
- A converter MAY set kinds of the second group of section 8.1 from the files it sees, such as `textures` for texture files, and names them as guesses in its report. Files it cannot classify give no kind.
- A converter never changes the bytes of a file, and never recompresses one.

## 21. Conformance

The conformance suite in [`format/tests/`](../tests/) holds made-up mods and packs, written as text: manifests, file tables and the bytes of each entry, from which each test runner builds the containers itself. With each case go the results every reader gives: errors, warnings, the build, signature results, the fit, state and reason of each part, and the index a rule builds. A reader conforms to `mnmf/1` when it gives those results for every case. A change to a rule of this spec comes with the cases that show it.

## Appendix A. Targets

The platforms are those of `mncf/1` Appendix A; a new platform is added there first. Their targets:

| `platform` | Targets | Paths |
|---|---|---|
| `switch` | `romfs`, `exefs`, `exefs-patch` | `romfs`: the game's RomFS, from its root. `exefs`: the code modules by their names in `mncf/1` Appendix A, and `main.npdm`. `exefs-patch`: patch files named by a module's build ID (section 12.2) |
| `switch2` | As `switch` | As `switch` |
| `n64` | None yet | Mods of an N64 game carry `.mncf` files only |
| `pc` | `game` | The game's install folder, from its root |
| Every platform | `cheats` | The part's `.mncf` files, by any name ending in `.mncf` |

New targets are added by a pull request to this table.

## Appendix B. Reasons

Every reason switches its part, or its mod, off. The player may switch a part on that is refused only for `build-broken`, `file-changed`, `file-missing`, `file-exists` or `unchecked` (section 15.3).

| Reason | Meaning |
|---|---|
| `game-mismatch` | The mod is for another platform or another game |
| `signature` | The manifest's signature is invalid or revoked |
| `hash:<entry>` | An entry's bytes differ from its hash: the mod or the pack's file is not used |
| `extension-missing:<extension>` | Section 18 |
| `unknown-key:<key>` | Section 18 |
| `invalid`, `invalid:<key>` | The part has no rows (section 10.1), or breaks a rule of this spec at that key |
| `build-broken` | The authors say that the part does not work on the build that runs |
| `build-unknown` | None of the part's rows is for the build that runs (section 15.3) |
| `file-missing:<target>/<path>` | The part replaces or removes a file the game does not have |
| `file-changed:<target>/<path>` | The game's file is none of the originals the part's file was made for |
| `file-exists:<target>/<path>` | The part adds a file at a path where the game now has one |
| `unchecked:<target>/<path>` | The runtime cannot tell whether the original fits, and the authors do not say that the build works |
| `capability-missing:<capability>` | Section 15.1 |
| `index-unsupported:<rule>` | The runtime does not know the index rule |
| `index-shipped:<target>/<path>` | The part carries the index file that its rule builds |
| `cheat:<cheat id>:<reason>` | A cheat of the part is refused, with the cheat's reason (`mncf/1` Appendix B) |
| `requires:<id>`, `excludes:<id>` | A relation between parts stops the part |
| `mod-missing:<mod id>` | A mod this one or the pack requires is not there |
| `version:<mod id>` | A required mod has a version outside the range |
| `conflict:<mod id>` | The mod can never be on together with the mods it requires |
| `dlc-missing:<id>` | Add-on content the mod needs is not there |
| `runtime:<reason>` | A reason of the runtime's own |

In `unknown-key:<key>` and `invalid:<key>`, `<key>` is the key's path from the part, as in `mncf/1` Appendix B: `name`, `cheats`, `values`.

## Appendix C. A complete example

Every ID, build ID, hash and file below is made up.

The manifest, `mod.toml`:

```toml
format = "mnmf/1"
id = "ann/crisp-ui"
name = "Crisp UI"
version = "1.2.0"
authors = ["Ann (example forum)"]
credits = [{ name = "Cara", role = "Bold fonts" }]
license = "CC-BY-4.0"
summary = "Sharper menus, a menu size of your choice, and 60 frames per second."
about = """
Crisp UI redraws every menu of **Example Quest** at full resolution.

- Choose the menu size that fits your screen.
- *Bold fonts* make text easier to read on a handheld.
- An optional patch lets the game run at 60 frames per second.

Questions and bug reports: [the mod's page](https://example.org/mods/crisp-ui).
"""
released = "2026-11-20"
links = { home = "https://example.org/mods/crisp-ui", support = "https://example.org/mods/crisp-ui/issues" }
kinds = ["ui", "performance", "fonts", "textures", "code"]
tags = ["cosmetic"]
table = { sha256 = "2e4060e6ca029435f78cfee0a4124dee0cadedf874b0ac54564840911e9d830d", files = 6, bytes = 2215496 }

[[changes]]
version = "1.2.0"
about = "Works on 1.0.3. The bold fonts are off there until they fit the new quest log."

[[changes]]
version = "1.1.0"
about = "Adds the 60 frames per second patch."

[game]
platform = "switch"
id = "0100000000ABC000"
name = "Example Quest"

[builds."1.0.2"]
modules = { main = "A1B2C3D4E5F60718" }
status = "made-for"

[builds."1.0.3"]
modules = { main = "1122334455667788" }
status = "works"
parts = { "bold-fonts" = "broken" }
note = "The bold fonts cut off the new quest log of 1.0.3."

[images]
logo = { file = "images/logo.png", sha256 = "329d2cce2fab124467930b42879e4459a09bc7cea4ba07c4d49f52baf51f16c9" }
icon = { file = "images/icon.webp", sha256 = "6f9155c33f1f50a093145d8ac0eee396fba93775e287a5f6c5fc192ad2fe14e3" }
banner = { file = "images/banner.webp", sha256 = "d8b81a97d7d44ac0e3edc1a02246a38b3ccd2cc6a01cde3ea882a9bada58a093" }

[[images.gallery]]
id = "menu"
image = { file = "images/gallery/menu.png", sha256 = "a6e95a8d0711d7648de29c3afc55dbd9262253f619f965eaf4a387e8edcfb8c1" }
thumb = { file = "images/gallery/menu.thumb.webp", sha256 = "3d8f660b5fa63a68ed3c5136b9dc58801c0747360bcb7bb70782c54afc9578a0" }
width = 1920
height = 1080
caption = "The main menu at 150 %"
part = "scale-150"

[[images.gallery]]
id = "bag"
image = { file = "images/gallery/bag.webp", sha256 = "f51cb2d0e2e6f62c8ded83859019761c1ef8af295e5160ebf18589b4d1a9197f" }
thumb = { file = "images/gallery/bag.thumb.webp", sha256 = "d37b05356dbc494c83fd15f571b7035f9381e183fae5a4ca4c902c324267bf62" }
width = 1280
height = 720
caption = "The bag with bold fonts, in handheld mode"
part = "bold-fonts"

[[part]]
id = "menus"
name = "Menus"
kinds = ["ui", "textures"]

[[part]]
id = "scale-125"
name = "125 %"
image = { file = "images/parts/scale-125.webp", sha256 = "3798fb485214dae4b14cf54cac4000a4ee49bbdb7dfcbaa42e61a659fcadcafc" }

[[part]]
id = "scale-150"
name = "150 %"
image = { file = "images/parts/scale-150.webp", sha256 = "ed36f6dc20c6c9cc69dd25c4e2cc3e6a26de86464beda46af862668d9950cfd5" }

[[part]]
id = "bold-fonts"
name = "Bold fonts"
about = "Thicker letters, easier to read on a small screen."
kinds = ["fonts"]
optional = true

[[part]]
id = "fps"
name = "60 frames per second"
about = "Lets the game run at up to 60 frames per second. It needs a fast device."
kinds = ["performance", "code"]
tags = ["may_crash"]
optional = true
cheats = ["fps-60"]

[[choice]]
id = "scale"
name = "Menu size"
options = ["scale-125", "scale-150"]
default = "scale-150"

[i18n.de]
name = "Klare Menüs"
summary = "Schärfere Menüs, eine Menügröße nach Wahl und 60 Bilder pro Sekunde."

[i18n.de.part.bold-fonts]
name = "Fette Schrift"

[i18n.de.part.fps]
name = "60 Bilder pro Sekunde"

[i18n.de.choice.scale]
name = "Menügröße"

[i18n.de.gallery.menu]
caption = "Das Hauptmenü in 150 %"
```

The file table, `files.tsv` (columns separated by tabs, shown here as they line up):

```
part	target	path	size	sha256	expect
menus	romfs	ui/layout/menu.blarc	48213	c00b72b0bc87b0848ca8572d3cc31ce0fd350a3c1153d05bcafab7ee0f3c6a5e	1edcdfcfdc6a5096d6bd5ed6122ee78eb36a898339f76ad32f6598869dffc341,d99c2adccfe2c596032377357b25538a83c3fd9fd413f49b10928abc9acdf27e
menus	romfs	ui/font/main.bfttf	1842210	abdefe1afade3e79bccfcd8b9e80249c64b9471ebf8643756837d869395468b9	74db6b25db71f554e3c0929bb7125a0a4ad0d871f8a3392af6a961e4d2921d52
scale-125	romfs	ui/layout/scale.bin	96	105c0dc2461c1103510cf25b7bf04f9657e587785ccda8904ebafca9444aae28	ad3bd83419bd6409d24e8b192461323d4f4c2a902da3dd539dd14a30c334067b,3b85752e71cf84f86641def7a31dff26e382c7c7490ff234e7e7527c5cbd3dc5
scale-150	romfs	ui/layout/scale.bin	96	8512f70fe890cfe1d592ac12377f145648b47ef68c6c34c91ab23407d70b981c	ad3bd83419bd6409d24e8b192461323d4f4c2a902da3dd539dd14a30c334067b,3b85752e71cf84f86641def7a31dff26e382c7c7490ff234e7e7527c5cbd3dc5
bold-fonts	romfs	ui/font/bold.bfttf	322143	f4ff51b22b8c6eafc3ef0441e36bb950d37532986d03fdb26e9d01c0aa0aac16	-
fps	cheats	fps.mncf	2738	3341ae56c0d9e343297bf82b283dcda21e4e70f895d09e64fb2c57329fbaa432	-
```

The part `fps` carries `fps.mncf`, an `.mncf` file for the same game whose cheat `fps-60` patches the frame limit by a pattern, as `mncf/1` Appendix C shows for other cheats.

The container, in the order of its entries:

| Entry | Stored as |
|---|---|
| `mod.toml` | Stored, at offset 0 |
| `images/icon.webp` | Stored |
| `files.tsv` | Zstandard |
| `images/logo.png`, `images/banner.webp`, `images/parts/scale-125.webp`, `images/parts/scale-150.webp`, `images/gallery/menu.thumb.webp`, `images/gallery/bag.thumb.webp`, `images/gallery/menu.png`, `images/gallery/bag.webp` | Stored |
| `parts/menus/romfs/ui/layout/menu.blarc` | Zstandard |
| `parts/menus/romfs/ui/font/main.bfttf` | Stored, aligned to 4,096 bytes: compression saves less than 3 % |
| `parts/scale-125/romfs/ui/layout/scale.bin`, `parts/scale-150/romfs/ui/layout/scale.bin` | Stored: too small to gain |
| `parts/bold-fonts/romfs/ui/font/bold.bfttf` | Stored, aligned to 4,096 bytes |
| `parts/fps/cheats/fps.mncf` | Zstandard |

What a runtime makes of it:

| Build that runs | `menus` | `scale-150` | `bold-fonts` (if on) | `fps` (if on) |
|---|---|---|---|---|
| 1.0.2 | on, `stated` | on, `stated` | on, `stated` | on if its cheat fits |
| 1.0.3 | on, `stated` | on, `stated` | refused, `build-broken` | on if its cheat fits |
| A later build that changed `ui/font/main.bfttf` | refused, `file-changed:romfs/ui/font/main.bfttf` | refused, `requires:menus` | refused, `requires:menus` | refused, `requires:menus` |
| A later build that changed none of the originals | on, `checked` | on, `checked` | on, `checked` | on if its cheat fits |

## Appendix D. Errors

A mod or a pack with one of these errors is not used at all. The reader says the code and a message.

| Code | Meaning |
|---|---|
| `zip` | Not a ZIP file; a name twice; an encrypted entry; a method other than stored, deflate and Zstandard; a data descriptor; a local header that differs from its central directory record; a Zstandard frame with a dictionary, a skippable frame or a window above 32 MiB |
| `layout` | The manifest is not the first entry at offset 0, or is compressed; the icon is not the second entry, or is compressed; an entry of a part comes before the file table |
| `size` | The manifest is above 1 MiB, the file table above 64 MiB, or the container has more than 1,048,576 entries |
| `manifest` | No `mod.toml` (or `pack.toml`) in the container |
| `encoding`, `toml` | The manifest breaks the rules of section 2, as in `mncf/1` Appendix D |
| `format` | `format` is not the first key, or not `"mnmf/1"` (`"mnmf-pack/1"` for a pack) |
| `platform` | `game.platform` is not in `mncf/1` Appendix A |
| `schema:<key>` | A key outside the parts is missing or has the wrong type or form, such as `schema:id`, `schema:version`, `schema:table` or `schema:part` (no part) |
| `duplicate-id:<id>` | Two parts, two choices or two pictures of the gallery have that id |
| `path:<path>` | A path that section 2 does not allow, in the container or in the file table |
| `table:<line>` | The file table breaks a rule of section 11.2 on that line, counted from 1; `table:0` when its hash, number of rows or sum of sizes differs from `table` |
| `hash:<entry>` | An entry's bytes differ from its hash (section 15.5) |

Warnings: `unknown-key:<key>` for a key outside the parts that neither the core nor a listed extension defines, with its path from the top of the manifest; `image:<key>` for a picture that breaks section 9; `kind:<id>` for an unknown kind; `link:<key>` for a link that does not start with `https://`; `i18n` for an `i18n` table that is not a table of tables, which is then ignored.

## Appendix E. Markup

Descriptions (`about`) are written in a small part of CommonMark (version 0.31.2):

- **Paragraphs**, separated by blank lines. A single line end inside a paragraph is a space.
- **Headings**: a line that starts with `# ` or `## `.
- **Lists**: lines that start with `- `, or with a number and `. ` (`1. `). A line indented by at least two spaces continues the item before it. Lists do not nest.
- **Emphasis**: `**bold**` and `*italic*`.
- **Code**: `` `text` `` shows text as it is, in a fixed-width font.
- **Links**: `[text](https://example.org/page)`, to `https://` addresses only.
- A `\` before one of `` \ ` * _ [ ] ( ) # - . ! `` shows that character as it is.

Anything else, such as HTML, pictures, tables, quotes, code blocks and other forms of links, is shown as plain text. A runtime that does not render the markup shows the text as it is. A runtime MUST NOT load anything from an address in a description by itself: a link opens only when the player follows it.
