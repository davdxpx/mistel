package io.github.davdxpx.mistel.mncf;

import io.github.davdxpx.mistel.mncf.testing.Check;
import java.io.ByteArrayOutputStream;
import java.math.BigInteger;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.Collections;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;

/** IPS, IPS32 and IPSwitch files of a code module, all made up. */
public final class PatchFilesTest {

    private PatchFilesTest() {
    }

    /** A record of an IPS file: bytes, or a run of count times one byte when bytes is null. */
    private static Object[] rec(long offset, byte[] bytes) {
        return new Object[] {offset, bytes, 0, 0};
    }

    private static Object[] run(long offset, int count, int value) {
        return new Object[] {offset, null, count, value};
    }

    private static byte[] ips(boolean ips32, byte[] tail, Object[]... records) {
        int width = ips32 ? 4 : 3;
        ByteArrayOutputStream out = new ByteArrayOutputStream();
        byte[] magic = (ips32 ? "IPS32" : "PATCH").getBytes(StandardCharsets.US_ASCII);
        out.write(magic, 0, magic.length);
        for (Object[] r : records) {
            long off = (Long) r[0];
            for (int k = width - 1; k >= 0; k--) {
                out.write((int) (off >> (8 * k)) & 0xFF);
            }
            byte[] data = (byte[]) r[1];
            if (data == null) {
                int count = (Integer) r[2];
                out.write(0);
                out.write(0);
                out.write(count >> 8);
                out.write(count & 0xFF);
                out.write((Integer) r[3]);
            } else {
                out.write(data.length >> 8);
                out.write(data.length & 0xFF);
                out.write(data, 0, data.length);
            }
        }
        byte[] end = (ips32 ? "EEOF" : "EOF").getBytes(StandardCharsets.US_ASCII);
        out.write(end, 0, end.length);
        out.write(tail, 0, tail.length);
        return out.toByteArray();
    }

    private static byte[] ips(Object[]... records) {
        return ips(false, new byte[0], records);
    }

    private static byte[] hex(String text) {
        return Fixtures.hex(text);
    }

    private static List<Object> records(Patch p) {
        List<Object> out = new ArrayList<>();
        for (Patch.Record r : p.records()) {
            out.add(Arrays.asList(r.offset().longValue(), Values.hexBytes(r.data())));
        }
        return out;
    }

    private static List<Object> pair(long offset, String hexBytes) {
        return Arrays.<Object>asList(offset, hexBytes);
    }

    private static Converter.Options options() {
        return Converter.Options.forGame(Synthetic.GAME_ID);
    }

    public static void testIps() throws Exception {
        PatchFile f = Patches.read(ips(false, hex("00 10 00"), rec(0x1108, hex("69 12 00 B9")), run(0x1200, 3, 0xAA),
                run(0x1300, 0, 1)));
        Check.equal("ips", f.format());
        Check.isTrue(f.patches().get(0).enabled(), "an IPS file is always applied");
        Check.equal(Arrays.asList(pair(0x1108, "69 12 00 B9"), pair(0x1200, "AA AA AA")), records(f.patches().get(0)));
        f = Patches.read(ips(true, new byte[0], rec(0x1000000, hex("01"))));
        Check.equal("ips32", f.format());
        Check.equal(Collections.singletonList(pair(0x1000000, "01")), records(f.patches().get(0)));
        for (final String bad : new String[] {"50 41 54 43 48", "50 41 54 43 48 00 10 00 00 04 01 02",
            "49 50 53 33 32 00 00 00 10 00 00 00", "4E 4F 50 45 21"}) {
            Check.throwsWith(PatchFileException.class, "", new Check.Action() {
                @Override
                public void run() throws Exception {
                    Patches.readIps(hex(bad));
                }
            });
        }
        PatchFileException e = Check.throwsWith(PatchFileException.class, "UTF-8", new Check.Action() {
            @Override
            public void run() throws Exception {
                Patches.read(hex("FF FE 20 6E 6F 74 20 74 65 78 74"));
            }
        });
        Check.equal("patch", e.code());
    }

    public static void testPchtxt() throws Exception {
        String text = "@nsobid-0123456789abcdef00000000\n"
                + "// Patches by Ann\n"
                + "// First\n"
                + "@enabled\n"
                + "00001008 691200B9 // str w9: this comment names nothing\n"
                + "00001010 \"a\\\"b // c\\n\" // a string with a quote, slashes and a new line\n"
                + "@flag offset_shift 0x100\n"
                + "// Second\n"
                + "@disabled\n"
                + "@little-endian\n"
                + "00001010 B9402100\n"
                + "nonsense\n"
                + "#forced\n"
                + "@stop\n"
                + "@enabled\n"
                + "00000000 00\n";
        PatchFile f = Patches.read(text.getBytes(StandardCharsets.UTF_8));
        Check.equal("pchtxt", f.format());
        Check.equal("0123456789ABCDEF", f.buildId());
        List<Object> patches = new ArrayList<>();
        for (Patch p : f.patches()) {
            patches.add(Arrays.<Object>asList(p.name(), p.enabled()));
        }
        Check.equal(Arrays.asList(Arrays.<Object>asList("First", true), Arrays.<Object>asList("Second", false)),
                patches);
        Check.equal(Arrays.asList(pair(0x1008, "69 12 00 B9"),
                pair(0x1010, Values.hexBytes("a\"b // c\n".getBytes(StandardCharsets.UTF_8)))),
                records(f.patches().get(0)));
        Check.equal(Collections.singletonList(pair(0x1110, "00 21 40 B9")), records(f.patches().get(1)));
        List<String> notes = new ArrayList<>();
        for (PatchFile.Line line : f.notes()) {
            notes.add(line.text());
        }
        Check.equal(Arrays.asList("nonsense", "forced"), notes);
        Check.isTrue(f.comments().contains(new PatchFile.Line(2, "Patches by Ann")), "the credit comment");
        Check.throwsWith(PatchFileException.class, "@nsobid-", new Check.Action() {
            @Override
            public void run() throws Exception {
                Patches.readPchtxt("@nsobid-XYZ\n");
            }
        });
        Check.throwsWith(PatchFileException.class, "offset_shift", new Check.Action() {
            @Override
            public void run() throws Exception {
                Patches.readPchtxt("@flag offset_shift 010\n");
            }
        });
    }

    public static void testIntegersAsPythonReadsThem() {
        String[][] cases = {{"0x100", "256"}, {"00", "0"}, {"0_0", "0"}, {"0x_1", "1"}, {"+0x10", "16"},
            {"-0o7", "-7"}, {"0X1F", "31"}, {"0B11", "3"}, {"0O17", "15"}, {"1_000", "1000"}, {"0x1_f", "31"},
            {"-0", "0"}, {"0b_1", "1"}, {"010", null}, {"0x", null}, {"0b2", null}, {"1__0", null}, {"_1", null},
            {"0_1", null}, {"0x_", null}, {"00x1", null}, {"0_", null}, {"0x1_", null}};
        for (String[] c : cases) {
            BigInteger v = Py.parseIntAuto(c[0]);
            Check.equal(c[1], v == null ? null : v.toString());
        }
    }

    public static void testBuildIdsAndRuns() {
        Check.equal("0123456789ABCDEF", Patches.buildIdOf("exefs_patches/x/0123456789ABCDEF0000000000000000.ips"));
        Check.equal("0123456789ABCDEF12", Patches.buildIdOf("0123456789abcdef1200.IPS32"));
        Check.equal(null, Patches.buildIdOf("main.ips"));
        Check.equal(null, Patches.buildIdOf("0123456789ABCDEF0.ips"));
        Patch p = new Patch("", true, Arrays.asList(new Patch.Record(BigInteger.valueOf(0xF8), new byte[12]),
                new Patch.Record(BigInteger.valueOf(0x108), hex("01 02")),
                new Patch.Record(BigInteger.valueOf(0x109), hex("03")),
                new Patch.Record(BigInteger.valueOf(0x200), hex("04 05 06"))));
        Patches.ImageRuns written = Patches.imageRuns(p, 0x102);
        List<Object> runs = new ArrayList<>();
        for (Patches.Run r : written.runs()) {
            runs.add(pair(r.offset(), Values.hexBytes(r.data())));
        }
        Check.equal(Arrays.asList(pair(0, "00 00 00 00"), pair(8, "01 03"), pair(0x100, "04 05")), runs);
        Check.equal(Arrays.asList("0xF8: 8 bytes in the NSO header", "0x200: 1 byte past the module"),
                written.dropped());
        List<String> sets = new ArrayList<>();
        for (String n : new String[] {"atmosphere/exefs_patches/60fps/0123456789ABCDEF.ips",
            "load/0100000000ABC000/My mod/exefs/a.pchtxt", "atmosphere/contents/0100000000ABC000/exefs/b.pchtxt",
            "0123456789ABCDEF.ips", null}) {
            sets.add(PatchConverter.setName(n));
        }
        Check.equal(Arrays.asList("60fps", "My mod", "b", "Patch", "Patch"), sets);
        Check.equal("x", PatchConverter.setName("x/.ips"));
        Check.equal("Patch", PatchConverter.setName(".ips"));
        Patch neg = new Patch("", true, Collections.singletonList(new Patch.Record(BigInteger.valueOf(-5), hex("01"))));
        Check.equal(Collections.singletonList("-0x5: 1 byte in the NSO header"), Patches.imageRuns(neg, 0x10).dropped());
    }

    public static void testConvert() throws Exception {
        final CodeModule mod = ModuleTest.module(true);
        final byte[] data = ips(rec(0x1108, hex("69 12 00 B9")));
        Check.throwsWith(ConvertException.class, "needs the module", new Check.Action() {
            @Override
            public void run() throws Exception {
                PatchConverter.fromPatches(PatchSource.builder(data).fileName(Synthetic.BUILD_ID + ".ips").build(),
                        options());
            }
        });
        Check.throwsWith(ConvertException.class, "no build ID", new Check.Action() {
            @Override
            public void run() throws Exception {
                Map<String, long[]> sections = new LinkedHashMap<>();
                for (String name : mod.sectionNames()) {
                    sections.put(name, new long[] {mod.sectionStart(name), mod.sectionEnd(name)});
                }
                CodeModule bare = new CodeModule(mod.image(), sections, null, null, null);
                PatchConverter.fromPatches(PatchSource.builder(data).module(bare).build(), options());
            }
        });
        ConvertException e = Check.throwsWith(ConvertException.class, "no patch", new Check.Action() {
            @Override
            public void run() throws Exception {      // nothing of it lands in the module
                PatchConverter.fromPatches(PatchSource.builder(ips(rec(0x10, hex("01")))).module(mod).build(), options());
            }
        });
        Check.equal("source", e.code());
        Conversion conv = PatchConverter.fromPatches(PatchSource.builder(data).module(mod)
                .fileName("atmosphere/exefs_patches/Faster/" + Synthetic.BUILD_ID + ".ips").build(),
                options().origin("faster.zip"));
        MncfFile f = Mncf.load(Fixtures.utf8(conv.text()));
        Check.equal("faster", f.cheats().get(0).id());
        Check.isTrue(f.cheats().get(0).isDefault(), "on by default");
        Map<String, Object> origin = new LinkedHashMap<>();
        origin.put("format", "ips");
        origin.put("name", "faster.zip");
        Check.equal(origin, f.origin());
        Check.equal(Collections.singletonMap("main", "0123456789ABCDEF"), f.builds().get("0123456789ABCDEF").modules());
        Runtime rt = Runtime.builder("switch").module("main", mod.toModule(0x8000000L)).build();
        Evaluation ev = Evaluator.evaluate(f, rt);
        Check.equal(CheatResult.ON, ev.cheats().get(0).state());
        Check.equal(0x8000000L + Synthetic.STR_W8, ev.cheats().get(0).ops().get(0).address());
        // A module whose build ID differs from the file's is named in a warning; the file's ID names the build.
        Conversion other = PatchConverter.fromPatches(PatchSource.builder(data).module(mod)
                .fileName("1122334455667788.ips").build(), options());
        Check.equal(Collections.singletonList("the module of build 1122334455667788 is "
                + mod.buildId().toUpperCase(java.util.Locale.ROOT) + ", its patch file 1122334455667788"),
                other.report().warnings());
    }

    public static void testSeveralBuildsAndModules() throws Exception {
        CodeModule mod = ModuleTest.module(true);
        byte[] data = ips(rec(0x1108, hex("69 12 00 B9")));
        final PatchSource a = PatchSource.builder(data).module(mod).fileName(Synthetic.BUILD_ID + ".ips")
                .setName("Faster").version("1.0.0").build();
        PatchSource b = PatchSource.builder(data).module(mod).fileName("0F0E0D0C0B0A0908.ips").setName("Sub")
                .version("1.0.0").moduleName("subsdk0").build();
        PatchSource c = PatchSource.builder(data).module(mod).fileName("1122334455667788.ips").setName("Faster")
                .version("1.0.0").build();
        MncfFile f = Mncf.load(Fixtures.utf8(PatchConverter.fromPatches(Arrays.asList(a, b), options()).text()));
        Map<String, String> modules = new LinkedHashMap<>();
        modules.put("main", Synthetic.BUILD_ID);
        modules.put("subsdk0", "0F0E0D0C0B0A0908");
        Check.equal(modules, f.builds().get("1.0.0").modules());
        List<String> at = new ArrayList<>();
        for (Cheat x : f.cheats()) {
            at.add(((Model.Patch) x.ops().get(0)).at.module);
        }
        Check.equal(Arrays.asList("main", "subsdk0"), at);
        f = Mncf.load(Fixtures.utf8(PatchConverter.fromPatches(Arrays.asList(a, c), options()).text()));   // one version, two builds
        Check.equal(Arrays.asList(Synthetic.BUILD_ID, "1122334455667788"), new ArrayList<>(f.builds().keySet()));
        Check.equal(1, f.cheats().size());
        PatchSource noVersion = PatchSource.builder(data).module(mod).fileName(Synthetic.BUILD_ID + ".ips").version("")
                .build();
        Check.equal(Collections.singletonList(Synthetic.BUILD_ID), new ArrayList<>(Mncf.load(Fixtures.utf8(
                PatchConverter.fromPatches(noVersion, options()).text())).builds().keySet()));
        final PatchSource sameKey = PatchSource.builder(data).module(mod).fileName("1122334455667788.ips")
                .buildKey("1.0.0").build();
        Check.throwsWith(ConvertException.class, "two build IDs", new Check.Action() {
            @Override
            public void run() throws Exception {                  // one build, two IDs for main
                PatchConverter.fromPatches(Arrays.asList(a, sameKey), options());
            }
        });
    }

    public static void testCreditsNameNoPatchAndFailuresKeepNoCounts() throws Exception {
        CodeModule base = ModuleTest.module(true);
        Map<String, long[]> sections = new LinkedHashMap<>();
        for (String name : base.sectionNames()) {
            sections.put(name, new long[] {base.sectionStart(name), base.sectionEnd(name)});
        }
        CodeModule mod = new CodeModule(base.image(), sections, Synthetic.BUILD_ID, null,
                Collections.singletonList(new CodeModule.Run(0x1010, 4)));
        String text = "@flag offset_shift 0x100\n// Patches by Ann\n@enabled\n00001008 691200B9\n"
                + "// Half\n@enabled\n00001008 691200B9\n00001010 1F2003D5\n";
        Conversion conv = PatchConverter.fromPatches(PatchSource.builder(text.getBytes(StandardCharsets.UTF_8)).module(mod)
                .fileName("p.pchtxt").setName("S").build(), options());
        List<Object> reps = new ArrayList<>();
        for (CheatReport c : conv.report().cheats()) {
            reps.add(Arrays.<Object>asList(c.name(), c.outcome(), c.patterns(), c.fixed(), c.notes()));
        }
        Check.equal(Arrays.asList(Arrays.<Object>asList("S 1", "pattern", 1, 0, Collections.emptyList()),
                Arrays.<Object>asList("Half", "unreadable", 0, 0, Collections.emptyList())), reps);
        Check.equal(Collections.singletonList("Ann"), Mncf.load(Fixtures.utf8(conv.text())).authors());
    }

    public static void testValuesAndData() throws Exception {
        CodeModule mod = ModuleTest.module(true);
        byte[] fmov = ips(rec(0x1900, hex("00 10 20 1E")), rec(0x3110, hex("07")));     // fmov s0, #2.0; a data byte
        PatchSource src = PatchSource.builder(fmov).module(mod).fileName(Synthetic.BUILD_ID + ".ips").setName("S")
                .build();
        MncfFile f = Mncf.load(Fixtures.utf8(PatchConverter.fromPatches(src, options()).text()));
        Cheat c = f.cheats().get(0);
        Check.equal("f32", c.values().get("value").type());
        Check.equal(2.0, c.values().get("value").defaultValue());
        Model.Write w = (Model.Write) c.ops().get(1);
        Check.equal("bytes", w.type);
        Check.equal("07", Values.hexBytes((byte[]) w.value));
        Check.equal("00", Values.hexBytes((byte[]) w.expect));
        String plain = PatchConverter.fromPatches(src, options().values(false)).text();
        Check.isTrue(!plain.contains("encode"), "no values");
        Check.isTrue(!plain.contains("extensions"), "no extensions without values");
    }
}
