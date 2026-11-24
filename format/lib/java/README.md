# Java library for `.mncf`

A Java library that reads, writes and converts files of the Mistel Native Cheat Format, version 1 (`mncf/1`), for Android apps and other Java programs. It does what the Python library in [`../python/`](../python/) does, with the same results:

- **Read:** it reads an `.mncf` file and checks it against the [spec](../../spec/mncf-1.md). A file that cannot be used at all gives an error code of Appendix D; a cheat that breaks a rule is refused with its reason. It checks the file's signature lines (Ed25519) against the keys the caller trusts.
- **Evaluate:** for a runtime, it identifies the build that runs, resolves every place (module offsets, patterns, regions, absolute addresses, pointer paths) and gives each cheat its state, reason and the bytes to write.
- **Run:** a reference pass engine (spec, section 11.5): it takes what the player does and the guest memory of each pass, and gives the patches, restores, hooks and writes of the pass as one batch, each cheat's state and what the monitors read.
- **Packs:** it reads packs (`.mncp`): the manifest, its signature and the hash of each file.
- **Write:** it writes any document in one fixed layout. Writing a file that was read gives the same file again, without its signature lines.
- **Convert:** it converts Atmosphère cheat files to `.mncf` (section 15), with or without the code module of each build, and a loaded file back to an Atmosphère cheat file for one build.

The Python library is the reference. Both libraries pass the conformance suite in [`../../tests/`](../../tests/) with identical results: the file cases, the pack cases, the pass cases, and the conversion cases, whose expected `.mncf` text the Java converter writes byte for byte.

## Build and test

```sh
sh build.sh
```

`build.sh` needs a JDK 9 or newer, from `JAVA_HOME` or the `PATH`. It compiles the library, compiles the tests and runs them, the conformance suite among them. It works in Git Bash on Windows and on Linux. Two variables make it check more:

- `ANDROID_JAR`: the `android.jar` of an Android platform. The library then compiles against it, so that no Java API that Android lacks can slip in.
- `D8`: Android's `d8` tool, together with `ANDROID_JAR`. The library's classes are then dexed for API 33.

```sh
ANDROID_JAR=<sdk>/platforms/android-33/android.jar D8=<sdk>/build-tools/<version>/d8 sh build.sh
```

The folder is also a Gradle build (`settings.gradle`, `build.gradle`, no wrapper): `gradle build` compiles the library and runs the same tests through `runTests`, which `check` depends on. The tests make their NSO files, LZ4 blocks and ZIP files themselves; the repository holds no binary.

## Using it in an Android app

The library needs Java 8 and Android 8 (API 26) or newer; it is checked against API 33. Include this folder as a build of its own in the app's `settings.gradle`:

```groovy
includeBuild('../mistel/format/lib/java')
```

and depend on it in the app module:

```groovy
dependencies {
    implementation 'io.github.davdxpx.mistel:mncf'
}
```

## The API in short

Reading and evaluating:

```java
TrustList trust = TrustList.fromToml(keysText);         // a trust list file, or new TrustList().trust(name, key)
MncfFile file = Mncf.load(bytes, trust);                // throws MncfException with code()

Module main = Module.builder("A1B2C3D4E5F60718")        // the build ID of the module that runs
        .base(0x8000000L)
        .image(mainBytes)                               // the module file's bytes, for patterns and expect
        .section("code", 0x0, 0x1000).section("rodata", 0x1000, 0x2000).section("data", 0x2000, 0x3000)
        .build();
Runtime runtime = Runtime.builder("switch")
        .module("main", main)
        .region("heap", heapBase, heapSize)
        .memory((address, size) -> readGuestMemory(address, size))   // null outside guest memory
        .unsupported("override")                        // parts of the core it lacks yet, out of Mncf.FEATURES
        .rewritten("main", 0x1000, 0x40)                // code it rewrote itself: no patch goes over it
        .build();
Choices choices = Choices.builder().on("money").value("money.count", 5000).build();

Evaluation ev = Evaluator.evaluate(file, runtime, choices);
for (CheatResult c : ev.cheats()) {
    // c.state(): off, on, waiting or refused; c.reason() says why
    for (ResolvedOp op : c.ops()) {
        // op.kind(), op.address(), op.bytes(), op.expect(), ...
    }
}

Pack pack = PackReader.read(zipBytes, trust);           // pack.usable(), pack.files() with status()
```

Running the cheats pass by pass:

```java
Engine engine = new Engine(file, runtime, choices, 60);   // 60 passes per second, for timed cheats
engine.switchCheat("speed", true);                        // between passes: what the player does
engine.setValue("money.count", 5000);
engine.hold("ZL");                                        // the buttons held now
engine.press("ZL+R");                                     // a press shorter than a pass
Pass pass = engine.tick(memory, null);                    // one pass; or tick(memory, seconds) with a clock
for (Change c : pass.changes()) {
    // c.kind(): patch, restore, hook, unhook (code, all in one stop), then write and raw
    // c.address(), c.bytes(), c.action(), c.value(), c.cheat()
}
pass.cheats();                                            // every cheat's state after the pass
pass.shown();                                             // what the monitors read, by cheat
List<Change> back = engine.stop();                        // puts back every place the cheats changed
```

The engine reads guest memory only through the `GuestMemory` it is given and changes nothing itself. One thread calls it.

Writing and converting:

```java
String text = Writer.write(file);                       // or Writer.dumps(document)

CodeModule module = CodeModule.fromBytes(nsoBytes);     // an NSO with or without LZ4, or a plain image
Source source = Source.builder("0123456789ABCDEF", atmosphereText).version("1.0.0").module(module).build();
Conversion conv = Converter.fromAtmosphere(source,      // or a list of sources, one per build; throws
        Converter.Options.forGame("0100000000ABC000")  // ConvertException with code() "atmosphere" or "source"
                .gameName("Example").origin("cheats.zip"));
conv.text();                                            // the .mncf text
conv.report().cheats();                                 // per entry: pattern, build, raw, restore or unreadable, and why

Converter.Export back = Converter.toAtmosphere(Mncf.load(bytes), "1.0.0", module, "raw");
back.fileName();                                        // "0123456789ABCDEF.txt"
back.text();
Converter.toAtmosphere(file, "1.0.1", newModule, "core", choices);   // a build the file does not name: from
                                                                      // its patterns, with the player's values
back.written();                                         // per cheat id: "raw" or "core"
back.reasons();                                         // per cheat left out: "build-unknown", "atmosphere:toggle"...
Converter.toAtmosphere(file, "1.0.0", module, "raw", choices,
        Collections.singletonMap("money", "core"));    // core for one cheat: the value the player set

for (Converter.BuildExport b : Converter.toAtmosphereAll(file, modulesByBuild, "raw", choices)) {
    b.build();                                          // every build of the file, in its order
    b.export();                                         // its Export, or null with reason() "atmosphere:module"
}
Converter.toAtmosphere(file, "1.0.0", module, "raw", choices, null,
        new Converter.AtmosphereLimits(256, 0x2000, null, null));   // a host that takes more; or NONE

for (Converter.MergedExport m : Converter.toAtmosphereMerged(Arrays.asList(
        Converter.BackSource.of(shop), new Converter.BackSource(speed, choices, "core", null)),
        modulesByBuildId, null)) {                       // null: Atmosphère's limits
    m.fileName();                                       // one file a build, with every source's cheats
    m.builds();                                         // each source's key of the build, or null
    m.reasons();                                        // for each source, the cheats left out
}
```

Back to Atmosphère, with the build's main module a converter finds the patterns and checks each patch against `expect`; without it, it writes what it can place as it is. Each cheat it leaves out gets a stable reason, the same as in Python (spec, section 15): the reader's reason for a cheat with a problem of its own, `build-unknown`, `pattern-not-found`, `pattern-ambiguous`, `expect-mismatch`, `outside`, `value-out-of-range`, or `atmosphere:<what>` for what Atmosphère cannot say. The file keeps Atmosphère's limits: one master entry with the code of every master cheat, at most 127 other entries with code of at most 256 words, less than 64 KiB, names of at most 63 bytes. What does not fit is left out with `atmosphere:too-many`, `atmosphere:too-long` or `atmosphere:too-big`, and a cheat that requires a cheat left out with `requires:<id>`. A host that takes more gives its own `Converter.AtmosphereLimits`, where null is no limit, or `AtmosphereLimits.NONE`. Groups are EdiZon section markers, which take no slot and show no switch; names lose the bracket that would end them early and control characters (`Converter.atmosphereName`), and a name the file has already gets " (2)". `toAtmosphereMerged` writes several files of one game into one file for each build, as a host reads one a build, matched by the build ID of main.

A converted cheat requires the master code only where it depends on it. Lines of a source that are neither names nor code go into the file's `about`, and an entry whose code mixes words with other text is left out and named there.

Patch files of a code module, IPS, IPS32 and IPSwitch text (`.pchtxt`):

```java
PatchSource patch = PatchSource.builder(ipsBytes).module(module)
        .fileName("exefs_patches/60fps/A1B2C3D4E5F60718.ips").version("1.0.2").build();
Conversion conv = Converter.fromPatches(patch,          // or a list of sources; throws ConvertException with
        Converter.Options.forGame("0100000000ABC000")  // code() "patch" (PatchFileException) or "source"
                .origin("60fps.zip"));
PatchFile file = Patches.read(ipsBytes);                // the file itself: its patches and their records
```

A patch file holds no original bytes, so the module it patches is required: `expect` comes from it. `moduleName` names a module other than `main`; the build ID comes from the file (`@nsobid-`), its name, `buildId` or the module. An IPS file becomes one cheat that is on by default, named by its patch set (`setName`, or the folder in `exefs_patches/`); each patch of a `.pchtxt` file becomes one cheat, named by its comment and on where it is `@enabled`. Patches get patterns and player values as Atmosphère cheats do, and what a file writes into data becomes a write with the original bytes as `expect`.

A file may have at most 16 MiB (`Mncf.MAX_FILE`). A pack may have at most 4096 entries, none of more than 16 MiB whatever its header claims, and a reader reads at most 256 MiB from it; beyond that the error code is `size`.

Addresses and offsets are unsigned 64-bit numbers in a `long`. Values are a `Long`, a `BigInteger` for a whole number of 2^63 or more, a `Double` or a `Boolean`; `longValue()` gives the 64 bits of any whole number. All result types are immutable. A `Module` and a `CodeModule` keep the image array they are given without copying it, so the caller must not change it afterwards. Pattern search is fast on large modules: it looks for the pattern's longest run of fixed bytes with a skip table and then checks the rest.

The converter never writes `asm` fields: the Python library fills them through a disassembler where one is installed, and this library has none.

## Floats in written files

The writer writes a float as the shortest text that reads back as the same double, as Python's `repr()` does; `nan`, `inf` and `-inf` are written as such, and a NaN loses its sign. The values that occur in converted files (the `fmov` immediates) and the suite's values come out exactly as in Python, and so did 187,000 other doubles in a comparison. Where two texts of the same shortest length both read back as the double, both libraries take the one nearest to it; the method is the same, so no difference is known.

## Why it has no dependencies

The library brings its own TOML 1.0 parser (`toml/`), its own Ed25519 verification and its own LZ4 block decoder:

- No TOML 1.0 library for Java is small, compatible with the MIT License and free of dependencies: tomlj needs ANTLR, toml4j reads TOML 0.4, and night-config is under the LGPL.
- Android 13 has the classes of the Ed25519 API but no provider that is sure to implement them. Verification here is plain Java with `BigInteger` and SHA-512, with the same rules as the Python library: S must be below L, y below p, points on the curve, and the check is [S]B = R + [k]A without the cofactor.
- LZ4 block decoding is a few dozen lines, smaller than any library for it.

The library only verifies signatures; packs and files are signed offline with the Mistel tools.
