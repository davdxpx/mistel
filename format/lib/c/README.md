# C library for `.mncf`

A C library that reads files of the Mistel Native Cheat Format, version 1 (`mncf/1`), for emulators, compatibility layers and recompiled games written in C or C++, and for anything that can call a C ABI. It does what the reading part of the Python library in [`../python/`](../python/) does, with the same results:

- **Read:** it reads an `.mncf` file and checks it against the [spec](../../spec/mncf-1.md). A file that cannot be used at all gives an error code of Appendix D and a message; a cheat that breaks a rule is refused with its reason. It checks the file's signature lines (Ed25519) against the keys the caller trusts, and reads trust lists (`mncf-keys/1`).
- **Evaluate:** for a runtime, it identifies the build that runs, resolves every place (module offsets, offsets per build, patterns, regions, absolute addresses, pointer paths through the caller's memory reader) and gives each cheat its state, reason and the bytes to write: patches, writes in the platform's byte order, override stubs, `arm64-imm/1` instructions.
- **Run:** the pass engine of spec section 11.5: it takes what the player does and the guest memory of each pass, and gives the patches, restores, hooks and writes of the pass as one batch, each cheat's state and what the monitors read.
- **Packs:** it reads packs (`.mncp`): the ZIP file, stored or deflated, the manifest, its signature and the hash of each file, within the size limits.

It does not write files or convert other cheat formats yet; that is for a later version. The Python library is the reference, and this library passes the conformance suite in [`../../tests/`](../../tests/) with the same results: every file case, pack case and pass case. Its `--dump` output is the same as the Python runner's, byte for byte (the conversion cases aside).

C99, no dependencies, no global state. SHA-256, SHA-512, Ed25519 verification, the TOML 1.0 parser, the ZIP reader and the inflater are written for this library. Under the MIT License, like everything in [`format/`](../../).

## Build

With CMake (3.10 or newer):

```sh
cmake -S format/lib/c -B build
cmake --build build
ctest --test-dir build
```

The options:

| Option | Default | What |
|---|---|---|
| `MNCF_TESTS` | `ON` | Build the unit tests and the conformance runner, and register them with CTest |
| `MNCF_AMALGAMATION` | `OFF` | Build the library from its amalgamation (below) instead of the sources |
| `MNCF_SANITIZE` | empty | Sanitizers for GCC and Clang, such as `address,undefined` |
| `BUILD_SHARED_LIBS` | `OFF` | A shared library; its API is exported with `MNCF_API` |

`cmake --install build` installs `mncf.h`, the library and a CMake package: `find_package(mncf)` and `target_link_libraries(app PRIVATE mncf::mncf)`.

For Android, with the NDK's toolchain file:

```sh
cmake -S format/lib/c -B build-android -DCMAKE_TOOLCHAIN_FILE=<ndk>/build/cmake/android.toolchain.cmake \
    -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-33
cmake --build build-android
```

### One file

The amalgamation is the whole library in two files, `mncf.c` and `mncf.h`, for a project that adds sources to its own build:

```sh
cmake -DSOURCE_DIR=format/lib/c -DOUTPUT_DIR=out -P format/lib/c/tools/amalgamate.cmake
cc -std=c99 -O2 -c out/mncf.c -o mncf.o
```

The build also makes it, in `<build>/amalgamation/`. Link with `-lm` where the C library keeps its math functions apart.

## The API in short

Everything is in [`include/mncf.h`](include/mncf.h). Objects are made by the library and freed by their `*_free` function; what an object gives out lives as long as it. Every function that can fail takes an `mncf_error *`, which gets the status, the code (`"schema:game.id"`) and a message.

Reading and evaluating:

```c
mncf_error err;
mncf_trust *trust = mncf_trust_read(keys_text, keys_size, NULL, &err);   /* or mncf_trust_new + mncf_trust_add */
mncf_file *file = mncf_load(data, size, trust, NULL, &err);
if (file == NULL) {
    /* err.code: size, encoding, toml, format, platform, schema:<key>, duplicate-id:<id> */
}
const mncf_file_info *info = mncf_file_info_of(file);    /* name, game, builds, cheats, presets, signature, ... */

mncf_module main_module = {0};
main_module.name = "main";
main_module.id = "A1B2C3D4E5F60718";                     /* the build ID of the module that runs */
main_module.base = 0x8000000;
main_module.data = main_bytes;                           /* the module file's bytes, for patterns and expect */
main_module.data_size = main_size;
main_module.size = main_size;
main_module.has_size = 1;
main_module.has_code = 1;                                /* sections: [start, end) offsets */
main_module.code_start = 0x0;
main_module.code_end = 0x1000;

mncf_region heap = {"heap", heap_base, heap_size};

mncf_runtime rt;
mncf_runtime_init(&rt, "switch");                        /* every capability, raw/1, arm64-imm/1, atmosphere */
rt.modules = &main_module;
rt.module_count = 1;
rt.regions = &heap;
rt.region_count = 1;
rt.read = read_guest_memory;                             /* int (*)(void *ctx, uint64_t address, void *out, size_t size) */
rt.read_ctx = emulator;

mncf_evaluation *ev = mncf_evaluate(file, &rt, NULL, &err);
for (size_t i = 0; i < mncf_evaluation_count(ev); i++) {
    const mncf_cheat_result *c = mncf_evaluation_cheat(ev, i);
    /* c->state: off, on, waiting or refused; c->reason says why; c->ops are the resolved operations */
}
mncf_evaluation_free(ev);

mncf_pack *pack = mncf_pack_read(zip, zip_size, trust, NULL, &err);    /* mncf_pack_info_of(pack)->files */
```

Running the cheats pass by pass:

```c
mncf_engine *eng = mncf_engine_new(file, &rt, NULL, 60, &err);   /* 60 passes per second, for timed cheats */
mncf_engine_switch(eng, "speed", 1, NULL);              /* between passes: what the player does */
mncf_engine_set_value(eng, "money.count", mncf_number_int(5000));
const char *held[] = {"ZL"};
mncf_engine_hold(eng, held, 1);                         /* the buttons held now */
mncf_engine_press(eng, "ZL+R");                         /* a press shorter than a pass */

mncf_pass pass;
mncf_engine_tick(eng, read_guest_memory, emulator, NULL, &pass);   /* or a time in seconds instead of NULL */
for (size_t i = 0; i < pass.change_count; i++) {
    const mncf_change *ch = &pass.changes[i];
    /* ch->kind: patch, restore, hook, unhook (code, all in one stop), then write and raw */
}
/* mncf_engine_state(eng, index, &reason) for each cheat; pass.shown for what the monitors read */

mncf_engine_stop(eng, &pass);                           /* the changes that put back every place */
mncf_engine_free(eng);
mncf_free(file);
mncf_trust_free(trust);
```

- The engine reads guest memory only through the reader it is given and changes nothing itself: the caller makes the changes of each pass, in order. The changes of a pass stay valid until the next call on the engine.
- An engine keeps pointers to the file and to the runtime's arrays and module bytes, so those must outlive it. The runtime structure itself is copied.
- A runtime that does not have a part of the core yet lists it in `rt.unsupported` (`"run.timed"`, `"when.many"`, ...; section 11.1): the cheats that use it are refused with `runtime:unsupported:<part>`. Code it rewrote for its own reasons goes into `rt.rewritten` as runs of a module: a patch or a stub over one is refused with `runtime:rewritten` (section 11.3).
- No object is shared between threads by the library, and it has no global state: different objects can be used in different threads at once, and one object by one thread at a time.
- Memory comes from the allocator given to `mncf_load`, `mncf_pack_read`, `mncf_trust_new` and `mncf_toml_parse`, or from `malloc` where it is `NULL`: one Lua-style function (`ptr`, old size, new size). Evaluations and engines use the allocator of their file. A file, a pack and a TOML document each keep their memory in one arena. When the allocator gives no memory, a call fails with `MNCF_ERR_MEMORY` and frees what it took; an engine that failed so should be freed.
- Numbers are `mncf_number`: a whole number from -(2^64 - 1) to 2^64 - 1 as a sign and a magnitude, a float or a boolean. Whole numbers and floats compare by their exact values, as the format says.
- A file may have at most 16 MiB (`MNCF_MAX_FILE`). A pack may have at most 4096 entries, none of more than 16 MiB whatever its header claims, and the reader reads at most 256 MiB from it; beyond that the error code is `size`.
- The TOML parser is in the API too (`mncf_toml_parse`), and a file's whole document is at `info->document`, for the `i18n` table and anything else a runtime wants to show.
- Game IDs compare without regard to ASCII case; the Python reference also folds other letters, which no game ID of Appendix A has.

## Tests

```sh
ctest --test-dir build --output-on-failure
python format/lib/c/tests/compare_dump.py build/mncf_conformance
```

- `mncf_unit` holds the unit tests: the hashes against FIPS 180-4's examples, Ed25519 against the vectors of RFC 8032 and the rules the format adds, base64 and CRC-32, the inflater against zlib's output, ZIP files stored, deflated, broken and too large, the TOML parser, exact number comparisons, the override stubs and `arm64-imm/1` instructions, trust lists, signature results, the API's errors, and a run of loading, evaluating, the engine and a pack in which every allocation fails in turn, without a crash or a leak.
- `mncf_conformance <format/tests>` runs the whole conformance suite; with `--dump` it prints each case's results as JSON, as `format/lib/python/tests/conformance.py --dump` does.
- `tests/compare_dump.py` runs the Python runner and this runner and compares their JSON byte for byte.

CI builds the library with GCC, Clang and MSVC, from the sources and from the amalgamation, runs both test programs with AddressSanitizer and UndefinedBehaviorSanitizer, compares the dump with Python's, and builds it with the Android NDK for arm64.
