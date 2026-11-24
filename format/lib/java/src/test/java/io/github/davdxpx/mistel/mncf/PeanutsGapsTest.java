package io.github.davdxpx.mistel.mncf;

import io.github.davdxpx.mistel.mncf.testing.Check;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.Collections;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;

/** What the first runtime that used the library found, on made-up modules and cheat files only. */
public final class PeanutsGapsTest {

    private PeanutsGapsTest() {
    }

    private static Source source(CodeModule mod, String text) {
        return source(mod, text, Synthetic.BUILD_ID, "1.0.0");
    }

    private static Source source(CodeModule mod, String text, String buildId, String version) {
        return Source.builder(buildId, text).version(version).module(mod).build();
    }

    private static Converter.Options options() {
        return Converter.Options.forGame(Synthetic.GAME_ID);
    }

    private static MncfFile convert(Source src) throws Exception {
        return Mncf.load(Fixtures.utf8(Converter.fromAtmosphere(src, options()).text()));
    }

    public static void testMasterOnlyWhereACheatDependsOnIt() throws Exception {
        String text = "{Setup}\n04000000 00001FE8 D503201F\n400A0000 00000000 00000000\n\n"
                + "[Uses the cave]\n04000000 00001008 140003F8\n\n"
                + "[Uses its register]\n040A0000 00002010 00000001\n\n"
                + "[On its own]\n04000000 00001008 B9001269\n";
        MncfFile f = convert(source(ModuleTest.module(true), text));
        List<List<String>> requires = new ArrayList<>();
        for (Cheat c : f.cheats()) {
            if (!c.isMaster()) {
                requires.add(c.requires());
            }
        }
        Check.equal(Arrays.asList(Collections.singletonList("setup"), Collections.singletonList("setup"),
                Collections.<String>emptyList()), requires);
    }

    public static void testLinesThatAreNotCode() throws Exception {
        final String text = "// cheats for Example Quest\n[Money]\n04000000 00001008 B9001269\nmade for 1.0.0\n\n"
                + "[Broken]\n04000000 00001008 GGGGGGGG\n";
        Check.throwsWith(AtmosphereException.class, "not a word", new Check.Action() {
            @Override
            public void run() throws Exception {
                Atmosphere.parse(text);
            }
        });
        List<Atmosphere.Note> notes = new ArrayList<>();
        List<Atmosphere.Entry> entries = Atmosphere.parse(text, notes);
        Check.equal(Arrays.asList(new Atmosphere.Note(1, "// cheats for Example Quest"),
                new Atmosphere.Note(4, "made for 1.0.0")), notes);
        List<List<String>> broken = new ArrayList<>();
        for (Atmosphere.Entry e : entries) {
            broken.add(e.broken());
        }
        Check.equal(Arrays.asList(Collections.<String>emptyList(),
                Collections.singletonList("04000000 00001008 GGGGGGGG")), broken);
        Conversion conv = Converter.fromAtmosphere(source(ModuleTest.module(true), text), options());
        MncfFile f = Mncf.load(Fixtures.utf8(conv.text()));
        List<String> ids = new ArrayList<>();
        for (Cheat c : f.cheats()) {
            ids.add(c.id());
        }
        Check.equal(Collections.singletonList("money"), ids);
        Check.isTrue(f.about().contains(
                "Lines in the source that are not cheat code: // cheats for Example Quest; made for 1.0.0"), f.about());
        Check.isTrue(f.about().contains("Entries whose code could not be read, left out: Broken"), f.about());
        Check.equal(1, conv.report().count(CheatReport.UNREADABLE));
        Check.equal(Collections.singletonList("Broken"), conv.report().unreadable());
    }

    public static void testEdizonSectionsAndNamesThatAreNoPerson() throws Exception {
        String text = "[--SectionStart:Items--]\n[Money]\n04000000 00001008 B9001269\n[--SectionEnd:Items--]\n"
                + "[Heap]\n04100000 00000010 00000001\n[Cheats created by All Credited, enjoy]\n";
        MncfFile f = convert(source(null, text));
        List<List<String>> groups = new ArrayList<>();
        for (Cheat c : f.cheats()) {
            groups.add(Arrays.asList(c.id(), c.group()));
        }
        Check.equal(Arrays.asList(Arrays.asList("money", "Items"), Arrays.asList("heap", null)), groups);
        Check.equal(Collections.<String>emptyList(), f.authors());
        Check.equal(Collections.singletonList("Ann"),
                Converter.authorsOf(Arrays.asList("by All Credited", "by everyone", "by Ann")));
    }

    public static void testBuildsWithTheSameVersion() throws Exception {
        final Source a = source(null, "[Heap]\n04100000 00000010 00000001\n");
        Source b = source(null, "[Heap]\n04100000 00000010 00000002\n", "1122334455667788", "1.0.0");
        MncfFile f = Mncf.load(Fixtures.utf8(Converter.fromAtmosphere(Arrays.asList(a, b), options()).text()));
        Map<String, String> versions = new LinkedHashMap<>();
        for (Map.Entry<String, Build> e : f.builds().entrySet()) {
            versions.put(e.getKey(), e.getValue().version());
        }
        Map<String, String> expected = new LinkedHashMap<>();
        expected.put(Synthetic.BUILD_ID, "1.0.0");
        expected.put("1122334455667788", "1.0.0");
        Check.equal(expected, versions);
        ConvertException e = Check.throwsWith(ConvertException.class, "same build ID", new Check.Action() {
            @Override
            public void run() throws Exception {
                Converter.fromAtmosphere(Arrays.asList(a, a), options());
            }
        });
        Check.equal("source", e.code());
    }

    public static void testBackWithThePlayersValues() throws Exception {
        String text = "[Tickets]\n040A0000 00001008 52986A08\n";
        MncfFile f = convert(source(ModuleTest.module(true), text));
        Converter.Export back = Converter.toAtmosphere(f, "1.0.0", ModuleTest.module(true), "core",
                Choices.builder().value("tickets.value", 7).build());
        Check.isTrue(back.text().contains("04000000 00001008 528000E8"), back.text());     // movz w8, #7
        // A value the player cannot choose is refused as the reader refuses it; the raw entry is
        // written instead. A text value cannot be chosen in Java at all: Choices refuses it.
        for (Object bad : new Object[] {true, -1L, 1L << 40, 7.0}) {
            back = Converter.toAtmosphere(f, "1.0.0", ModuleTest.module(true), "core",
                    Choices.builder().value("tickets.value", bad).build());
            Check.equal(Collections.singletonMap("tickets", "raw"), back.written());
            Check.isTrue(back.text().contains("040A0000 00001008 52986A08"), back.text());
        }
        String text2 = Converter.fromAtmosphere(source(ModuleTest.module(true), text), options()).text();
        MncfFile noRaw = Mncf.load(Fixtures.utf8(text2.substring(0, text2.indexOf("  [[cheat.raw]]"))));
        back = Converter.toAtmosphere(noRaw, "1.0.0", ModuleTest.module(true), "core",
                Choices.builder().value("tickets.value", -1L).build());
        Check.equal(Collections.<String, String>emptyMap(), back.written());
        Check.equal(Collections.singletonMap("tickets", "a chosen value the cheat cannot take"), back.skipped());
    }

    public static void testCompareReadsTheOriginalLeniently() throws Exception {
        String original = "// notes\n[Heap]\n04100000 00000010 00000001\n\n[Broken]\n04100000 00000010 GGGGGGGG\n";
        Converter.Comparison cmp = Converter.compare(original, "[Heap]\n04100000 00000010 00000001\n", false);
        Check.equal(Arrays.asList(Collections.singletonList("Heap"), Collections.<String>emptyList(),
                Collections.<String>emptyList()), Arrays.asList(cmp.same(), cmp.differ(), cmp.missing()));
    }

    public static void testRuntimeLacksPartsOfTheCoreAndRewroteCode() throws Exception {
        CodeModule mod = ModuleTest.module(true);
        MncfFile f = convert(source(mod, Synthetic.CHEATS));
        Runtime rt = Runtime.builder("switch").module("main", mod.toModule(0)).region("heap", 0, 0x10000)
                .unsupported("hold", "run.action").rewritten("main", Synthetic.STR_W8, 4).build();
        Evaluation ev = Evaluator.evaluate(f, rt);
        Check.equal("runtime:unsupported:hold", ev.cheat("hold-a").reason());
        Check.equal("runtime:unsupported:run.action", ev.cheat("speed").reason());
        Check.equal("runtime:rewritten", ev.cheat("money").reason());
        Check.equal("", ev.cheat("heap-value").reason());
    }

    public static void testUses() throws Exception {
        String text = "format = \"mncf/1\"\nname = \"Uses\"\n\n[game]\nplatform = \"switch\"\nid = \"0100000000ABC000\"\n\n"
                + "[[cheat]]\nid = \"timed\"\nname = \"Timed\"\nrun = \"timed\"\nduration = 2\nhold = \"A\"\n"
                + "match = \"any\"\ntrigger = \"enter\"\ninterval = 2\n\n"
                + "  [[cheat.when]]\n  at = { region = \"heap\", offset = 0x10 }\n  type = \"u8\"\n  op = \"==\"\n"
                + "  value = 1\n\n"
                + "  [[cheat.when]]\n  at = { region = \"heap\", offset = 0x11 }\n  type = \"u8\"\n  op = \"==\"\n"
                + "  value = 1\n\n"
                + "  [[cheat.write]]\n  at = { region = \"heap\", offset = 0x20 }\n  type = \"u8\"\n  value = 1\n";
        MncfFile f = Mncf.load(Fixtures.utf8(text));
        Check.equal(Arrays.asList("run.timed", "hold", "when", "when.many", "match.any", "trigger.enter", "interval"),
                Evaluator.uses(f.cheat("timed")));
    }
}
