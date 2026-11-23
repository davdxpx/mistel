package io.github.davdxpx.mistel.mncf;

import io.github.davdxpx.mistel.mncf.testing.Check;
import java.util.Arrays;
import java.util.Collections;
import java.util.List;
import java.util.Set;
import java.util.TreeSet;

/** Tests of LZ4, code modules, the ARM64 helpers and the pattern finder, on made-up modules only. */
public final class ModuleTest {

    private ModuleTest() {
    }

    static CodeModule module(boolean compress) throws CodeModuleException {
        return CodeModule.parseNso(Synthetic.sampleNso(compress));
    }

    private static byte[] repeat(byte[] piece, int times) {
        byte[] out = new byte[piece.length * times];
        for (int i = 0; i < times; i++) {
            System.arraycopy(piece, 0, out, i * piece.length, piece.length);
        }
        return out;
    }

    private static void lz4Fails(final byte[] block, final int size) {
        Check.throwsWith(Lz4Exception.class, "", new Check.Action() {
            @Override
            public void run() throws Exception {
                Lz4.decompress(block, size);
            }
        });
    }

    public static void testLz4RoundTrip() throws Exception {
        byte[] counting = new byte[256];
        for (int i = 0; i < 256; i++) {
            counting[i] = (byte) i;
        }
        for (byte[] data : new byte[][] {new byte[0], Fixtures.utf8("abc"), new byte[5000],
            Fixtures.concat(repeat(Fixtures.utf8("abcd"), 1000), Fixtures.utf8("xyz")), repeat(counting, 40),
            Synthetic.sampleCode()[0]}) {
            byte[] packed = Synthetic.lz4Compress(data);
            Check.isTrue(Arrays.equals(data, Lz4.decompress(packed, data.length)), "round trip of " + data.length + " bytes");
        }
    }

    public static void testLz4OverlappingMatch() throws Exception {
        // 'a', then 19 more 'a' at offset 1, then the literals 'bcdef'.
        byte[] block = Fixtures.concat(new byte[] {0x1F, 'a', 1, 0, 0, 0x50}, Fixtures.utf8("bcdef"));
        Check.equal(new String(repeat(Fixtures.utf8("a"), 20), "UTF-8") + "bcdef",
                new String(Lz4.decompress(block, 25), "UTF-8"));
    }

    public static void testLz4Errors() {
        lz4Fails(new byte[] {0x10, 'a', 5, 0}, 30);
        lz4Fails(Synthetic.lz4Compress(new byte[100]), 99);
        lz4Fails(new byte[] {(byte) 0xF0}, 30);
        lz4Fails(new byte[] {0x10, 'a', 5}, 30);
    }

    public static void testNso() throws Exception {
        for (boolean compress : new boolean[] {true, false}) {
            CodeModule m = module(compress);
            byte[][] c = Synthetic.sampleCode();
            Check.isTrue(Arrays.equals(c[0], Arrays.copyOf(m.image(), c[0].length)), "the code");
            Check.isTrue(Arrays.equals(c[1], Arrays.copyOfRange(m.image(), 0x2000, 0x2000 + c[1].length)), "rodata");
            Check.isTrue(Arrays.equals(c[2], Arrays.copyOfRange(m.image(), 0x3000, 0x3000 + c[2].length)), "data");
            Check.equal(0x3800, m.image().length);
            Check.equal(Arrays.asList("code", "rodata", "data"), m.sectionNames());
            Check.equal(0L, m.sectionStart("code"));
            Check.equal(0x2000L, m.sectionEnd("code"));
            Check.equal(0x2000L, m.sectionStart("rodata"));
            Check.equal(0x3000L, m.sectionEnd("rodata"));
            Check.equal(0x3000L, m.sectionStart("data"));
            Check.equal(0x3800L, m.sectionEnd("data"));
            Check.equal(Synthetic.BUILD_ID, m.shortId());
            Check.equal((long) Synthetic.TEXT_SIZE, m.textSize());
            Check.isTrue(m.buildId().startsWith(Synthetic.BUILD_ID) && m.buildId().length() == 64, "the whole build ID");
        }
    }

    public static void testPlainImageRunsAndErrors() throws Exception {
        CodeModule m = CodeModule.fromBytes(new byte[64]);
        Check.equal(Collections.singletonList("code"), m.sectionNames());
        Check.equal(64L, m.sectionEnd("code"));
        Check.isTrue(m.buildId() == null, "a plain image has no build ID");
        Check.isTrue(m.toModule(0).id().startsWith("sha256:"), "a plain image is named by its hash");
        Check.equal(Arrays.asList(new CodeModule.Run(0x1000, 8), new CodeModule.Run(0x2000, 0x10)),
                CodeModule.readRuns("# BUILD_ID\tabc\n1000\t8\n\n2000\t10\n"));
        Check.equal(Arrays.asList(new CodeModule.Run(0x1000, 8), new CodeModule.Run(-1L, 1)),
                CodeModule.readRuns(" 0x1000 \t 8 \tnote\nFFFFFFFFFFFFFFFF\t1\n"));
        for (final String bad : new String[] {"1000\n", "1_000\t8\n", "-5\t8\n", "10000000000000000\t1\n",
            "0X10\t8\n", "10\t\n", "+10\t8\n"}) {
            Check.throwsWith(CodeModuleException.class, "line 1", new Check.Action() {
                @Override
                public void run() throws Exception {
                    CodeModule.readRuns(bad);
                }
            });
        }
        CodeModule far = CodeModule.fromBytes(new byte[64]).withExclude(Collections.singletonList(new CodeModule.Run(-16L, 32)));
        Check.isTrue(!far.excluded(0, 8), "a run at the top of the address space does not wrap to 0");
        Check.isTrue(far.excluded(-8L, 4), "unsigned offsets");
        Check.throwsWith(CodeModuleException.class, "not an NSO", new Check.Action() {
            @Override
            public void run() throws Exception {
                CodeModule.parseNso(Fixtures.concat(Fixtures.utf8("NSO0"), new byte[10]));
            }
        });
    }

    public static void testArm64Targets() {
        Check.equal(0x1100L, Arm64.pcRelativeTarget(0x94000040L, 0x1000));      // bl +0x100
        Check.equal(0xFFCL, Arm64.pcRelativeTarget(0x97FFFFFFL, 0x1000));       // bl -4
        Check.equal(0x1010L, Arm64.pcRelativeTarget(0x54000080L, 0x1000));      // b.eq +0x10
        Check.equal(0x1008L, Arm64.pcRelativeTarget(0xB4000040L, 0x1000));      // cbz x0, +8
        Check.equal(0x1008L, Arm64.pcRelativeTarget(0x36000040L, 0x1000));      // tbz w0, #0, +8
        Check.equal(0x1000L, Arm64.pcRelativeTarget(0x90000008L, 0x1234));      // adrp x8, this page
        Check.equal(0x2000L, Arm64.pcRelativeTarget(0xB0000008L, 0x1234));      // adrp x8, next page
        Check.equal(0x1008L, Arm64.pcRelativeTarget(0x58000040L, 0x1000));      // ldr x0, literal
        Check.isTrue(Arm64.pcRelativeTarget(0xD503201FL, 0x1000) == null, "nop");
    }

    public static void testArm64MovingBytes() {
        byte[] code = Synthetic.words(0x94000040L, 0x90000008L, 0xB9402100L, 0xB9402101L, 0xD503201FL);
        Set<Integer> want = new TreeSet<>(Arrays.asList(0, 1, 2, 4, 5, 6, 7, 9, 10, 13, 14));
        Check.equal(want, Arm64.movingBytes(code, 0, code.length));
    }

    public static void testArm64RelativeOutside() {
        Check.isTrue(Arm64.relativeOutside(Synthetic.words(0x14000100L), 0x1000, 4), "a branch away");
        Check.isTrue(!Arm64.relativeOutside(Synthetic.words(0x14000001L, 0xD503201FL), 0x1000, 8), "a branch inside");
        Check.isTrue(!Arm64.relativeOutside(Synthetic.words(0xD503201FL), 0x1000, 4), "no branch");
    }

    public static void testPatternUniqueAndMasked() throws Exception {
        CodeModule m = module(true);
        Patterns.Found p = Patterns.make(m, Synthetic.STR_W8, 4);
        Check.equal("68 12 00 B9", p.text());
        Check.equal(0L, p.place);
        Check.equal((long) Synthetic.STR_W8, p.start);
        p = Patterns.make(m, Synthetic.BL, 4);
        Check.equal("68 12 00 B9 ?? ?? ?? 94", p.text());
        Check.equal(4L, p.place);
        List<Long> found = Evaluator.findAll(m.toModule(0), p.items, null, 0);
        Check.equal(Collections.singletonList((long) Synthetic.STR_W8), found);
    }

    public static void testNoPattern() throws Exception {
        CodeModule m = module(true);
        Check.isTrue(Patterns.make(m, 0x1FF0, 4) == null, "zero bytes of the cave");
        Check.isTrue(Patterns.make(m, Synthetic.DATA + 0x10, 4) == null, "data is not searched");
    }

    public static void testPatternAvoidsExcludedBytes() throws Exception {
        CodeModule m = module(true).withExclude(Collections.singletonList(new CodeModule.Run(Synthetic.STR_W8, 4)));
        Patterns.Found p = Patterns.make(m, Synthetic.BL, 4);
        Check.isTrue(!p.text().contains("68 12 00 B9"), "no pattern from bytes that are not the module's own");
    }
}
