package io.github.davdxpx.mistel.mncf;

import io.github.davdxpx.mistel.mncf.testing.Check;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.Collections;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;

/** Tests of the converter, on made-up modules and cheat files only. */
public final class ConvertTest {

    private ConvertTest() {
    }

    private static Source source(CodeModule mod, String text, String buildId, String version) {
        return Source.builder(buildId, text).version(version).module(mod).build();
    }

    private static Source source(CodeModule mod) {
        return source(mod, Synthetic.CHEATS, Synthetic.BUILD_ID, "1.0.0");
    }

    private static Converter.Options options() {
        return Converter.Options.forGame(Synthetic.GAME_ID);
    }

    /** The outcome and why of each cheat that became a cheat, by id. */
    private static Map<String, List<String>> outcomes(Conversion conv) {
        Map<String, List<String>> out = new LinkedHashMap<>();
        for (CheatReport c : conv.report().cheats()) {
            if (!c.id().isEmpty()) {
                out.put(c.id(), Arrays.asList(c.outcome(), c.why()));
            }
        }
        return out;
    }

    private static Evaluation evaluate(MncfFile f, CodeModule mod, long base, long heapBase) {
        Runtime rt = Runtime.builder("switch").module("main", mod.toModule(base)).region("heap", heapBase, 0x10000)
                .build();
        return Evaluator.evaluate(f, rt);
    }

    private static void noneRefused(Evaluation ev, String what) {
        for (CheatResult c : ev.cheats()) {
            Check.isTrue(!c.state().equals(CheatResult.REFUSED), what + " " + c.id() + ": " + c.reason());
        }
    }

    public static void testWithModule() throws Exception {
        CodeModule mod = ModuleTest.module(true);
        Conversion conv = Converter.fromAtmosphere(source(mod), options().name("Example").origin("example.zip"));
        Map<String, List<String>> outcome = outcomes(conv);
        Check.equal("pattern", outcome.get("money").get(0));
        Check.equal("build", outcome.get("branch-away").get(0));
        Check.equal("build", outcome.get("master-setup").get(0));
        Check.equal("pattern", outcome.get("hold-a").get(0));
        Check.equal("build", outcome.get("cave-jump").get(0));
        Check.equal("pattern", outcome.get("speed").get(0));
        for (String cid : new String[] {"data-flag", "heap-value", "pointer", "condition"}) {
            Check.equal("build", outcome.get(cid).get(0));
        }
        Check.equal(Arrays.asList("raw", "a loop"), outcome.get("loop"));
        Check.isTrue(!outcome.containsKey("money-off"), "the restore entry is no cheat");
        Check.equal(1, conv.report().count(CheatReport.RESTORE));
        Check.equal(Arrays.asList("Section A", "Data", "Items"), conv.report().groups());
        MncfFile f = Mncf.load(Fixtures.utf8(conv.text()));
        Evaluation ev = evaluate(f, mod, 0x8000000L, 0x10000000L);
        Check.equal("1.0.0", ev.build());
        noneRefused(ev, "");
        ResolvedOp money = ev.cheat("money").ops().get(0);
        Check.equal((long) Synthetic.STR_W8, money.offset());
        Check.equal("68 12 00 B9", Values.hexBytes(money.expect()));
        Check.equal("69 12 00 B9", Values.hexBytes(money.bytes()));
        Check.equal("00 10 20 1E", Values.hexBytes(ev.cheat("speed").ops().get(0).bytes()));
        Check.equal("f32", f.cheat("speed").values().get("value").type());
        Check.equal(2.0, f.cheat("speed").values().get("value").defaultValue());
        Check.equal("Section A", f.cheat("money").group());
        Check.equal("Data", f.cheat("heap-value").group());
        Check.equal("A", f.cheat("hold-a").hold());
        Check.equal("B", f.cheat("cave-jump").hold());
        Cheat speed = f.cheat("speed");      // patches without an else branch: the patch stays after the press
        Check.isTrue(speed.hold() == null, "an action has no hold");
        Check.equal("action", speed.run());
        Check.equal("ZL", speed.press());
        Check.equal("Items", f.cheat("cave-jump").group());
        Check.isTrue(f.cheat("speed").group() == null, "the group ended before Speed");
        // Only a cheat that depends on the master requires it; Loop stays raw code, which may.
        Check.equal(Collections.<String>emptyList(), f.cheat("money").requires());
        Check.equal(Collections.singletonList("master-setup"), f.cheat("loop").requires());
        Check.isTrue(f.cheat("master-setup").isMaster(), "the master");
        Check.equal(Collections.singletonList("someone"), f.authors());
        Map<String, Object> origin = new LinkedHashMap<>();
        origin.put("format", "atmosphere");
        origin.put("name", "example.zip");
        origin.put("site", "example.org");
        Check.equal(origin, f.origin());
        Check.equal(Arrays.asList("raw/1", "arm64-imm/1"), f.optionalExtensions());
        Check.isTrue(conv.text().contains("find = \"68 12 00 B9\""), "a pattern in the text");
        Check.isTrue(!conv.text().contains("asm"), "no asm fields without a disassembler");
    }

    public static void testWithoutModule() throws Exception {
        Conversion conv = Converter.fromAtmosphere(source(null), options());
        Map<String, List<String>> outcome = outcomes(conv);
        Check.equal("build", outcome.get("money").get(0));           // expect from the restore entry
        Check.equal("build", outcome.get("branch-away").get(0));     // expect from the release branch of Hold A
        Check.equal("build", outcome.get("hold-a").get(0));
        Check.equal("raw", outcome.get("cave-jump").get(0));         // no original bytes after the end of the code
        Check.equal("raw", outcome.get("speed").get(0));
        Check.equal("build", outcome.get("heap-value").get(0));
        MncfFile f = Mncf.load(Fixtures.utf8(conv.text()));
        Model.Patch money = (Model.Patch) f.cheat("money").ops().get(0);
        Check.equal("68 12 00 B9", Values.hexBytes(money.expect));
        Check.equal("Converted cheats", f.name());
    }

    public static void testAuthors() throws Exception {
        Check.equal(Arrays.asList("Some Forum", "Ann", "Bo"), Converter.authorsOf(Arrays.asList("credits: Some Forum",
                "www.example.org", "This set is created by Ann & Bo, enjoy!")));
        Check.equal(Arrays.asList("Ann (Some Forum)", "Bo (Other)", "Cy (a forum)"),
                Converter.authorsOf(Arrays.asList("Cheats by Ann on Some Forum & Bo @ Other", "by Cy (a forum)")));
        Check.equal("example.org", Converter.siteOf(Arrays.asList("credits: Some Forum", "https://www.Example.org/cheats")));
        Check.isTrue(Converter.siteOf(Collections.singletonList("credits: Some Forum")) == null, "no site");
        String plain = "[Money]\n04100000 00000010 00000001\n";
        Converter.Options player = options().authors(Collections.singletonList("Player"));
        MncfFile f = Mncf.load(Fixtures.utf8(Converter.fromAtmosphere(source(null, plain, Synthetic.BUILD_ID, "1.0.0"),
                player).text()));
        Check.equal(Collections.singletonList("Player"), f.authors());
        f = Mncf.load(Fixtures.utf8(Converter.fromAtmosphere(source(null, plain, Synthetic.BUILD_ID, "1.0.0"),
                options()).text()));
        Check.equal(Collections.<String>emptyList(), f.authors());
        f = Mncf.load(Fixtures.utf8(Converter.fromAtmosphere(source(null), player).text()));
        Check.equal(Collections.singletonList("someone"), f.authors());
    }

    public static void testExcludedRuns() throws Exception {
        CodeModule mod = ModuleTest.module(true)
                .withExclude(Collections.singletonList(new CodeModule.Run(Synthetic.STR_W8, 4)));
        MncfFile f = Mncf.load(Fixtures.utf8(Converter.fromAtmosphere(source(mod), options()).text()));
        Model.Patch money = (Model.Patch) f.cheat("money").ops().get(0);
        Check.equal("module-builds", money.at.kind);
        Check.equal("68 12 00 B9", Values.hexBytes(money.expect));
    }

    public static void testSameBytesStayAPatch() throws Exception {
        CodeModule mod = ModuleTest.module(true);
        String text = "[Same]\n04000000 00001008 B9001268\n04100000 00000010 00000001\n";
        Conversion conv = Converter.fromAtmosphere(source(mod, text, Synthetic.BUILD_ID, "1.0.0"), options());
        CheatReport rep = conv.report().cheats().get(0);
        Check.equal(Collections.singletonList("main+0x1008 already holds those bytes; the patch stays, as the source "
                + "writes them"), rep.notes());
        Model.Patch p = (Model.Patch) Mncf.load(Fixtures.utf8(conv.text())).cheat("same").ops().get(0);
        Check.equal("68 12 00 B9", Values.hexBytes(p.bytes));
    }

    public static void testTwoBuilds() throws Exception {
        Source a = source(ModuleTest.module(true), Synthetic.CHEATS, Synthetic.BUILD_ID, "1.0.0");
        byte[][] c = Synthetic.shiftedCode();
        CodeModule bMod = CodeModule.parseNso(Synthetic.makeNso(c[0], c[1], c[2], 0x400, "1122334455667788", true));
        Source b = source(bMod, Synthetic.shiftedCheats(), "1122334455667788", "1.0.1");
        MncfFile f = Mncf.load(Fixtures.utf8(Converter.fromAtmosphere(Arrays.asList(a, b), options()).text()));
        Check.equal(Arrays.asList("1.0.0", "1.0.1"), new ArrayList<>(f.builds().keySet()));
        @SuppressWarnings("unchecked")
        List<Object> single = (List<Object>) Converter.fromAtmosphere(a, options()).doc().get("cheat");
        Check.equal(single.size(), f.cheats().size());
        List<String> rawBuilds = new ArrayList<>();
        for (Model.Raw r : f.cheat("money").raw()) {
            rawBuilds.add(r.build);
        }
        Check.equal(Arrays.asList("1.0.0", "1.0.1"), rawBuilds);
        Map<String, Long> offsets = new LinkedHashMap<>();
        offsets.put("1.0.0", 0x100CL);
        offsets.put("1.0.1", 0x104CL);
        Check.equal(offsets, ((Model.Patch) f.cheat("branch-away").ops().get(0)).at.offsets);
        for (Object[] bm : new Object[][] {{ModuleTest.module(true), "1.0.0"}, {bMod, "1.0.1"}}) {
            Evaluation ev = evaluate(f, (CodeModule) bm[0], 0, 0);
            Check.equal(bm[1], ev.build());
            noneRefused(ev, (String) bm[1]);
        }
    }

    public static void testErrorCodes() {
        final Source a = source(null);
        ConvertException e = Check.throwsWith(ConvertException.class, "same build ID", new Check.Action() {
            @Override
            public void run() throws Exception {
                Converter.fromAtmosphere(Arrays.asList(a, a), options());
            }
        });
        Check.equal("source", e.code());
        final Source b = Source.builder("1122334455667788", Synthetic.CHEATS).buildKey("1.0.0").build();
        e = Check.throwsWith(ConvertException.class, "two files name the build", new Check.Action() {
            @Override
            public void run() throws Exception {
                Converter.fromAtmosphere(Arrays.asList(a, b), options());
            }
        });
        Check.equal("source", e.code());
        e = Check.throwsWith(AtmosphereException.class, "no cheat with code", new Check.Action() {
            @Override
            public void run() throws Exception {
                Converter.fromAtmosphere(source(null, "[Heading]\n", Synthetic.BUILD_ID, "1.0.0"), options());
            }
        });
        Check.equal("atmosphere", e.code());
        e = Check.throwsWith(AtmosphereException.class, "line 2", new Check.Action() {
            @Override
            public void run() throws Exception {
                Converter.fromAtmosphere(source(null, "[Heap]\n[Money\n04100000 00000010 00000001\n",
                        Synthetic.BUILD_ID, "1.0.0"), options());
            }
        });
        Check.equal("atmosphere", e.code());
    }

    public static void testBackRefusesWhatAtmosphereCannotSay() throws Exception {
        String text = "format = \"mncf/1\"\nname = \"x\"\nextensions = { optional = [\"arm64-imm/1\"] }\n\n"
                + "[game]\nplatform = \"switch2\"\nid = \"0100000000ABC000\"\n\n"
                + "[builds.one]\nmodules = { main = \"0123456789ABCDEF\" }\n\n"
                + "[[cheat]]\nid = \"c-button\"\nname = \"C button\"\nhold = \"C\"\n\n"
                + "  [[cheat.write]]\n  at = { region = \"heap\", offset = 0x10 }\n  type = \"u8\"\n  value = 1\n\n"
                + "[[cheat]]\nid = \"too-wide\"\nname = \"Too wide\"\n\n"
                + "  [cheat.values.n]\n  type = \"u32\"\n  default = 70000\n\n"
                + "  [[cheat.patch]]\n  at = { module = \"main\", offset = 0x1000 }\n  expect = \"08 00 80 52\"\n"
                + "  encode = { insn = \"movz\", width = 32, rd = 8, value = \"$n\" }\n";
        MncfFile f = Mncf.load(Fixtures.utf8(text));
        Converter.Export back = Converter.toAtmosphere(f, "one", null, "core");
        Check.equal(Arrays.asList("c-button", "too-wide"), new ArrayList<>(back.skipped().keySet()));
        Check.equal("a value its instruction cannot hold", back.skipped().get("too-wide"));
    }

    public static void testBackToAtmosphere() throws Exception {
        CodeModule mod = ModuleTest.module(true);
        MncfFile f = Mncf.load(Fixtures.utf8(Converter.fromAtmosphere(source(mod), options()).text()));
        Converter.Export back = Converter.toAtmosphere(f, "1.0.0", mod, "raw");
        Check.equal(Synthetic.BUILD_ID + ".txt", back.fileName());
        Converter.Comparison cmp = Converter.compare(Synthetic.CHEATS, back.text(), false);
        Check.equal(Collections.<String>emptyList(), cmp.differ());
        Check.equal(Collections.singletonList("Money off"), cmp.missing());
        for (String how : back.written().values()) {
            Check.equal("raw", how);
        }
        Converter.Export core = Converter.toAtmosphere(f, "1.0.0", mod, "core");
        cmp = Converter.compare(Synthetic.CHEATS, core.text(), true);
        Check.equal(Collections.<String>emptyList(), cmp.differ());
        Check.equal("raw", core.written().get("loop"));
        Check.equal("core", core.written().get("money"));
        Converter.Export other = Converter.toAtmosphere(f, "9.9.9", mod, "core");
        Check.equal(Synthetic.BUILD_ID + ".txt", other.fileName());     // a build the file does not name: from patterns
        Check.isTrue(other.written().containsKey("money"), "money from its pattern");
        Check.isTrue(!other.written().containsKey("branch-away"), "offsets for 1.0.0 only");
        final MncfFile file = f;
        Check.throwsWith(IllegalArgumentException.class, "no build", new Check.Action() {
            @Override
            public void run() throws Exception {
                Converter.toAtmosphere(file, "9.9.9", null, "raw");
            }
        });
    }

    public static void testSlug() {
        java.util.Set<String> taken = new java.util.HashSet<>();
        Check.equal("money", Converter.slug("Money!", taken));
        Check.equal("money-2", Converter.slug("money", taken));
        Check.equal("cheat-99-lives", Converter.slug("99 Lives", taken));
        Check.equal("cheat", Converter.slug("***", taken));
    }
}
