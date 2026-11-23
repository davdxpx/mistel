package io.github.davdxpx.mistel.mncf;

import io.github.davdxpx.mistel.mncf.testing.Check;
import java.util.Arrays;
import java.util.Collections;
import java.util.List;

/** Tests of reading a file, evaluating it, and searching patterns. */
public final class LoadTest {

    private LoadTest() {
    }

    private static String loadError(final byte[] data) {
        MncfException e = Check.throwsWith(MncfException.class, "", new Check.Action() {
            @Override
            public void run() throws Exception {
                Mncf.load(data);
            }
        });
        return e.code();
    }

    private static String replace(String text, String from, String to) {
        return text.replace(from, to);
    }

    public static void testLoadAndEvaluate() throws MncfException {
        MncfFile f = Mncf.load(Fixtures.FILE);
        Check.equal("Example", f.name());
        Check.equal("0100000000ABC000", f.gameId());
        Check.equal("switch", f.platform().name());
        Check.equal(SignatureResult.UNSIGNED, f.signature().result());
        Runtime rt = Runtime.builder("switch").region("heap", 0x1000, 0x100).build();
        Evaluation ev = Evaluator.evaluate(f, rt, Choices.builder().on("money").build());
        CheatResult r = ev.cheat("money");
        Check.equal(CheatResult.ON, r.state());
        Check.equal("core", r.uses());
        Check.equal(0x1010L, r.ops().get(0).address());
        Check.isTrue(Arrays.equals(new byte[] {5, 0, 0, 0}, r.ops().get(0).bytes()), "the value in little-endian order");
        Check.equal(CheatResult.OFF, Evaluator.evaluate(f, rt).cheat("money").state());
    }

    public static void testErrors() {
        Check.equal("encoding", loadError(Fixtures.concat(new byte[] {(byte) 0xEF, (byte) 0xBB, (byte) 0xBF}, Fixtures.FILE)));
        Check.equal("encoding", loadError(Fixtures.utf8(replace(Fixtures.FILE_TEXT, "\n", "\r\n"))));
        Check.equal("encoding", loadError(Fixtures.utf8(replace(Fixtures.FILE_TEXT, "Money", "Mo\rney"))));
        Check.equal("encoding", loadError(Fixtures.concat(Fixtures.FILE, new byte[] {(byte) 0xED, (byte) 0xA0, (byte) 0x80})));
        Check.equal("encoding", loadError(Fixtures.concat(Fixtures.FILE, new byte[] {(byte) 0xC0, (byte) 0x80})));
        Check.equal("toml", loadError(Fixtures.utf8("format = ")));
        Check.equal("format", loadError(Fixtures.utf8(replace(Fixtures.FILE_TEXT, "mncf/1", "mncf/9"))));
        Check.equal("format", loadError(Fixtures.utf8("name = \"x\"\n" + Fixtures.FILE_TEXT.replace("name = \"Example\"\n", ""))));
        Check.equal("schema:game.id", loadError(Fixtures.utf8(replace(Fixtures.FILE_TEXT, "0100000000ABC000", "12"))));
        Check.equal("platform", loadError(Fixtures.utf8(replace(Fixtures.FILE_TEXT, "\"switch\"", "\"wii\""))));
        Check.equal("duplicate-id:money", loadError(Fixtures.concat(Fixtures.FILE,
                Fixtures.utf8("\n[[cheat]]\nid = \"money\"\nname = \"Again\"\n"))));
    }

    public static void testDateTimesAreRefused() {
        for (String value : new String[] {"1979-05-27", "07:32:00", "1979-05-27T07:32:00", "1979-05-27T07:32:00Z",
            "[1979-05-27]", "{ a = 07:32:00 }"}) {
            Check.equal("toml", loadError(Fixtures.utf8(replace(Fixtures.FILE_TEXT, "name = \"Example\"",
                    "name = \"Example\"\nwhen = " + value))));
        }
    }

    private static String nested(int arrays, boolean inner) {
        StringBuilder b = new StringBuilder("deep = ");
        for (int i = 0; i < arrays; i++) {
            b.append('[');
        }
        b.append(inner ? "1" : "");
        for (int i = 0; i < arrays; i++) {
            b.append(']');
        }
        return replace(Fixtures.FILE_TEXT, "name = \"Example\"", "name = \"Example\"\n" + b);
    }

    public static void testDepth() throws MncfException {
        // The root table is depth 0 and each table or array one more; above 128 is too deep.
        Mncf.load(Fixtures.utf8(nested(128, false)));
        Check.equal("toml", loadError(Fixtures.utf8(nested(128, true))));
        Mncf.load(Fixtures.utf8(nested(127, true)));
        Check.equal("toml", loadError(Fixtures.utf8(nested(129, false))));
        StringBuilder header = new StringBuilder("t");
        for (int i = 0; i < 128; i++) {
            header.append(".t");
        }
        Check.equal("toml", loadError(Fixtures.utf8(Fixtures.FILE_TEXT + "\n[" + header + "]\n")));
        Mncf.load(Fixtures.utf8(Fixtures.FILE_TEXT + "\n[" + header.substring(2) + "]\n"));
    }

    public static void testNanIsRefused() throws MncfException {
        String text = replace(replace(Fixtures.FILE_TEXT, "\"u32\"", "\"f64\""), "value = 5", "value = -nan");
        Check.equal("invalid:write.value", Mncf.load(Fixtures.utf8(text)).cheat("money").problem());
        String inf = replace(replace(Fixtures.FILE_TEXT, "\"u32\"", "\"f32\""), "value = 5", "value = -inf");
        MncfFile f = Mncf.load(Fixtures.utf8(inf));
        Runtime rt = Runtime.builder("switch").region("heap", 0x1000, 0x100).build();
        Check.equal("00 00 80 FF", Values.hexBytes(Evaluator.evaluate(f, rt).cheat("money").ops().get(0).bytes()));
    }

    public static void testWarningsAndModel() throws MncfException {
        String text = replace(Fixtures.FILE_TEXT, "[game]", "colour = \"red\"\n\n[game]")
                + "\n[[preset]]\nid = \"rich\"\nname = \"Rich\"\ncheats = [\"money\"]\n";
        MncfFile f = Mncf.load(Fixtures.utf8(text));
        Check.equal(Collections.singletonList("unknown-key:colour"), f.warnings());
        Check.equal(1, f.presets().size());
        Check.equal(Collections.singletonList("money"), f.presets().get(0).cheats());
        Cheat c = f.cheat("money");
        Check.equal("Money", c.name());
        Check.equal("on", c.run());
        Check.isTrue(c.problem() == null, "the cheat has no problem");
        Check.isTrue(f.doc().containsKey("colour"), "the document is kept");
    }

    public static void testFindAllOverlappingAndSections() {
        byte[] data = Fixtures.concat(new byte[4], fill((byte) 0xAA, 5), new byte[7], fill((byte) 0xBB, 4), new byte[12]);
        Module m = Module.builder("00").image(data).section("code", 0, 18).section("rodata", 18, 32).build();
        int[] aa = {0xAA, 0xAA, 0xAA, 0xAA};
        Check.equal(Arrays.asList(4L, 5L), Evaluator.findAll(m, aa, null, 0));
        Check.equal(Collections.<Long>emptyList(), Evaluator.findAll(m, aa, "rodata", 0));
        Check.equal(Collections.singletonList(4L), Evaluator.findAll(m, "AA ?? AA AA", null, 1));
        Check.equal(Collections.<Long>emptyList(), Evaluator.findAll(m, "BB BB BB BB", null, 0));
        Module whole = Module.builder("00").image(data).build();
        Check.equal(Collections.singletonList(16L), Evaluator.findAll(whole, "BB BB BB BB", null, 0));
        Check.equal(Collections.<Long>emptyList(), Evaluator.findAll(whole, "BB BB BB BB", "data", 0));
    }

    public static void testFindAllInALargeModule() {
        byte[] data = new byte[64 * 1024 * 1024];
        for (int i = 0; i < data.length; i++) {
            data[i] = (byte) (i * 31 + (i >>> 13));
        }
        byte[] needle = Fixtures.hex("68 12 00 B9 11 22 33 94 DE AD");
        int at = data.length - 1000;
        System.arraycopy(needle, 0, data, at, needle.length);
        System.arraycopy(needle, 0, data, 12345, needle.length);
        Module m = Module.builder("00").image(data).build();
        List<Long> found = Evaluator.findAll(m, "68 12 00 B9 ?? ?? ?? 94 DE AD", null, 0);
        Check.equal(Arrays.asList(12345L, (long) at), found);
        Check.equal(Collections.singletonList(12345L), Evaluator.findAll(m, "68 12 00 B9 ?? ?? ?? 94 DE AD", null, 1));
    }

    public static void testIdentifyPrefersRuntimeBuild() throws MncfException {
        String text = replace(Fixtures.FILE_TEXT, "[[cheat]]", "[builds.\"1.0\"]\nmodules = { main = \"AB\" }\n\n[[cheat]]");
        MncfFile f = Mncf.load(Fixtures.utf8(text));
        Runtime byModule = Runtime.builder("switch").module("main", Module.builder("abcd").build()).build();
        Check.equal("1.0", Evaluator.identify(f, byModule));
        Check.isTrue(Evaluator.identify(f, Runtime.builder("switch").build("2.0").build()) == null, "an unknown build");
        Check.isTrue(Evaluator.identify(f, Runtime.builder("switch").module("main", Module.builder(null).build()).build())
                == null, "a module without identity matches nothing");
    }

    public static void testUnsignedAddresses() throws MncfException {
        String text = "format = \"mncf/1\"\nname = \"x\"\n[game]\nplatform = \"switch\"\nid = \"0100000000ABC000\"\n"
                + "[[cheat]]\nid = \"far\"\nname = \"far\"\n  [[cheat.write]]\n"
                + "  at = { region = \"heap\", offset = 0x8, path = [\"0xFFFFFFFFFFFFFFF0\"] }\n  type = \"u8\"\n  value = 1\n";
        MncfFile f = Mncf.load(Fixtures.utf8(text));
        final long heap = 0xFFFFFFFF00000000L;
        GuestMemory memory = new GuestMemory() {
            @Override
            public byte[] read(long address, int size) {
                if (address == heap + 8 && size == 8) {
                    return Fixtures.hex("2000000000000000");
                }
                if (address == 0x10 && size == 1) {
                    return new byte[1];
                }
                return null;
            }
        };
        Runtime rt = Runtime.builder("switch").region("heap", heap, 0x100).memory(memory).build();
        ResolvedOp op = Evaluator.evaluate(f, rt).cheat("far").ops().get(0);
        Check.equal(0x10L, op.address());
        Check.equal(0x10L, op.toMap().get("address"));
    }

    public static void testValueOutOfRangeFromChoices() throws MncfException {
        String text = "format = \"mncf/1\"\nname = \"x\"\n[game]\nplatform = \"switch\"\nid = \"0100000000ABC000\"\n"
                + "[[cheat]]\nid = \"big\"\nname = \"big\"\n  [cheat.values.n]\n  type = \"u64\"\n  default = 1\n"
                + "  [[cheat.write]]\n  at = { region = \"heap\", offset = 0 }\n  type = \"u64\"\n  value = \"$n\"\n";
        MncfFile f = Mncf.load(Fixtures.utf8(text));
        Runtime rt = Runtime.builder("switch").region("heap", 0x1000, 0x100).build();
        Choices top = Choices.builder().unsignedValue("big.n", -1L).build();
        Check.equal("FF FF FF FF FF FF FF FF", Values.hexBytes(Evaluator.evaluate(f, rt, top).cheat("big").ops().get(0).bytes()));
        Choices negative = Choices.builder().value("big.n", -1).build();
        Check.equal("value-out-of-range", Evaluator.evaluate(f, rt, negative).cheat("big").reason());
        Check.throwsWith(IllegalArgumentException.class, "whole number", new Check.Action() {
            @Override
            public void run() {
                Choices.builder().value("big.n", "1");
            }
        });
    }

    private static byte[] fill(byte b, int n) {
        byte[] out = new byte[n];
        Arrays.fill(out, b);
        return out;
    }
}
