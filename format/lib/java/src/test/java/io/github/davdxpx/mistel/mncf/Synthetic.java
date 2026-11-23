package io.github.davdxpx.mistel.mncf;

import java.io.ByteArrayOutputStream;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.HashMap;
import java.util.List;
import java.util.Map;

/**
 * Made-up code modules and cheat files for the tests, the same as the Python tests make: an LZ4
 * block compressor, NSO files, a small module with known places, and an Atmosphère cheat file
 * for it. The module's filler comes from Python's random number generator with the same seed,
 * so that both libraries see the same bytes.
 */
final class Synthetic {

    private Synthetic() {
    }

    static final String BUILD_ID = "0123456789ABCDEF";
    static final String GAME_ID = "0100000000ABC000";

    static final long[] FILLER = {0xD503201FL, 0xA9BF7BFDL, 0x910003FDL, 0xD65F03C0L, 0xAA1303E0L, 0xF9400260L,
        0x52800000L, 0x2A1F03E0L};

    // Places in the made-up module.
    static final int FUNCTION = 0x1000;     // stp; mov; str w8, [x19, #0x10]; bl; adrp; ldr; ldp; ret
    static final int STR_W8 = 0x1008;
    static final int BL = 0x100C;
    static final int TEXT_SIZE = 0x1FE0;    // the code ends here; up to 0x2000 the page is zero: a cave
    static final int DATA = 0x3000;
    static final int SHIFT = 0x40;

    /** Python's random.Random: the Mersenne Twister, seeded and drawn as Python does. */
    static final class PyRandom {
        private final int[] mt = new int[624];
        private int index = 624;

        PyRandom(int seed) {
            initGenrand(19650218);
            int[] key = {seed};
            int i = 1;
            int j = 0;
            for (int k = Math.max(624, key.length); k > 0; k--) {
                mt[i] = (mt[i] ^ ((mt[i - 1] ^ (mt[i - 1] >>> 30)) * 1664525)) + key[j] + j;
                i++;
                j++;
                if (i >= 624) {
                    mt[0] = mt[623];
                    i = 1;
                }
                if (j >= key.length) {
                    j = 0;
                }
            }
            for (int k = 623; k > 0; k--) {
                mt[i] = (mt[i] ^ ((mt[i - 1] ^ (mt[i - 1] >>> 30)) * 1566083941)) - i;
                i++;
                if (i >= 624) {
                    mt[0] = mt[623];
                    i = 1;
                }
            }
            mt[0] = 0x80000000;
        }

        private void initGenrand(int s) {
            mt[0] = s;
            for (int i = 1; i < 624; i++) {
                mt[i] = 1812433253 * (mt[i - 1] ^ (mt[i - 1] >>> 30)) + i;
            }
        }

        private long next32() {
            if (index >= 624) {
                for (int k = 0; k < 624; k++) {
                    int y = (mt[k] & 0x80000000) | (mt[(k + 1) % 624] & 0x7FFFFFFF);
                    mt[k] = mt[(k + 397) % 624] ^ (y >>> 1) ^ ((y & 1) != 0 ? 0x9908B0DF : 0);
                }
                index = 0;
            }
            int y = mt[index++];
            y ^= y >>> 11;
            y ^= (y << 7) & 0x9D2C5680;
            y ^= (y << 15) & 0xEFC60000;
            y ^= y >>> 18;
            return y & 0xFFFFFFFFL;
        }

        /** Python's choice: an index below n, drawn with as few bits as n needs. */
        int below(int n) {
            int k = 32 - Integer.numberOfLeadingZeros(n);
            long r = next32() >>> (32 - k);
            while (r >= n) {
                r = next32() >>> (32 - k);
            }
            return (int) r;
        }
    }

    static byte[] words(long... ws) {
        byte[] out = new byte[4 * ws.length];
        for (int i = 0; i < ws.length; i++) {
            for (int k = 0; k < 4; k++) {
                out[4 * i + k] = (byte) (ws[i] >>> (8 * k));
            }
        }
        return out;
    }

    /** A valid LZ4 block with matches, greedy over a table of 4-byte prefixes. The last 12 bytes stay literals. */
    static byte[] lz4Compress(byte[] data) {
        ByteArrayOutputStream out = new ByteArrayOutputStream();
        Map<Integer, Integer> table = new HashMap<>();
        int i = 0;
        int anchor = 0;
        int end = data.length;
        int limit = end - 12;
        while (i < limit) {
            int key = (data[i] & 0xFF) | (data[i + 1] & 0xFF) << 8 | (data[i + 2] & 0xFF) << 16 | (data[i + 3] & 0xFF) << 24;
            Integer cand = table.get(key);
            table.put(key, i);
            if (cand == null || i - cand > 0xFFFF) {
                i++;
                continue;
            }
            int m = 4;
            while (i + m < limit && data[cand + m] == data[i + m]) {
                m++;
            }
            int lit = i - anchor;
            out.write(Math.min(lit, 15) << 4 | Math.min(m - 4, 15));
            if (lit >= 15) {
                lengths(out, lit - 15);
            }
            out.write(data, anchor, i - anchor);
            out.write((i - cand) & 0xFF);
            out.write((i - cand) >> 8);
            if (m - 4 >= 15) {
                lengths(out, m - 4 - 15);
            }
            i += m;
            anchor = i;
        }
        int lit = end - anchor;
        out.write(Math.min(lit, 15) << 4);
        if (lit >= 15) {
            lengths(out, lit - 15);
        }
        out.write(data, anchor, end - anchor);
        return out.toByteArray();
    }

    private static void lengths(ByteArrayOutputStream out, int n) {
        while (n >= 255) {
            out.write(255);
            n -= 255;
        }
        out.write(n);
    }

    private static int page(int n) {
        return (n + 0xFFF) & ~0xFFF;
    }

    private static void put32(byte[] b, int at, long v) {
        for (int k = 0; k < 4; k++) {
            b[at + k] = (byte) (v >>> (8 * k));
        }
    }

    /** An NSO file whose segments lie at page-aligned memory offsets one after another. */
    static byte[] makeNso(byte[] text, byte[] ro, byte[] data, int bss, String buildId, boolean compress) {
        int[] mems = {0, page(text.length), page(text.length) + page(ro.length)};
        byte[][] segs = {text, ro, data};
        byte[][] stored = new byte[3][];
        for (int i = 0; i < 3; i++) {
            stored[i] = compress ? lz4Compress(segs[i]) : segs[i];
        }
        byte[] header = new byte[0x100];
        header[0] = 'N';
        header[1] = 'S';
        header[2] = 'O';
        header[3] = '0';
        put32(header, 0x0C, compress ? 0b111 : 0);
        int pos = 0x100;
        for (int i = 0; i < 3; i++) {
            put32(header, 0x10 + 0x10 * i, pos);
            put32(header, 0x14 + 0x10 * i, mems[i]);
            put32(header, 0x18 + 0x10 * i, segs[i].length);
            put32(header, 0x60 + 4 * i, stored[i].length);
            pos += stored[i].length;
        }
        put32(header, 0x3C, bss);
        byte[] id = ConformanceTest.hex(buildId);
        System.arraycopy(id, 0, header, 0x40, Math.min(id.length, 0x20));
        return Fixtures.concat(header, stored[0], stored[1], stored[2]);
    }

    /** The code, read-only data and data of the made-up module. */
    static byte[][] sampleCode() {
        PyRandom rng = new PyRandom(7);
        long[] filler = new long[TEXT_SIZE / 4];
        for (int i = 0; i < filler.length; i++) {
            filler[i] = FILLER[rng.below(FILLER.length)];
        }
        byte[] text = words(filler);
        byte[] function = words(0xA9BF7BFDL, 0x910003FDL, 0xB9001268L, 0x94000040L, 0x90000008L, 0xB9402100L,
                0xA8C17BFDL, 0xD65F03C0L);
        System.arraycopy(function, 0, text, FUNCTION, function.length);
        byte[] fmov = words(0x1E2E1000L, 0x1E202008L);          // fmov s0, #1.0; fcmp s0, s0
        System.arraycopy(fmov, 0, text, 0x1800, fmov.length);
        byte[] ro = new byte[0x800];
        System.arraycopy(Fixtures.utf8("Example\0"), 0, ro, 0x100, 8);
        byte[] data = new byte[0x400];
        byte[] pointer = {0x00, 0x01, 0x00, 0x10, 0, 0, 0, 0};
        System.arraycopy(pointer, 0, data, 0x20, 8);
        return new byte[][] {text, ro, data};
    }

    static byte[] sampleNso(boolean compress) {
        byte[][] c = sampleCode();
        return makeNso(c[0], c[1], c[2], 0x400, BUILD_ID, compress);
    }

    static final String CHEATS = String.join("\n", Arrays.asList(
            "[--- Section A ---]", "20000000", "",
            "[Cheats by someone, www.example.org]", "",
            "{Master setup}", "04000000 00001FF0 D503201F", "",
            "[Money]", "04000000 00001008 B9001269", "",
            "[Money off]", "04000000 00001008 B9001268", "",
            "[Branch away]", "04000000 0000100C 14000100", "",
            "[--- Data ---]", "",
            "[Data flag]", "04000000 00003010 00000001", "",
            "[Heap value]", "02100000 00000040 0000FFFF", "",
            "[Pointer]", "580F0000 00003020", "580F1000 00000010", "780F0000 00000008", "640F0000 00000000 00000063", "",
            "[Hold A]", "80000001", "04000000 0000100C D503201F", "21000000", "04000000 0000100C 94000040", "20000000", "",
            "[Condition]", "14150000 00000100 00000005", "02100000 00000104 00000001", "20000000", "",
            "[Items]", "20000000", "",
            "[Cave jump]", "80000002", "080A0000 00001FE8 D503201F 52800020", "040A0000 00001010 140003F6", "21000000",
            "040A0000 00001010 90000008", "20000000", "",
            "[Items]", "20000001", "",
            "[Speed]", "C4000000 00000000 00000100", "040A0000 00001800 1E201000", "20000000", "",
            "[Loop]", "300F0000 00000004", "04000000 00003000 00000000", "310F0000", "")) ;

    /** The same code as sampleCode, SHIFT bytes later: a later build of the made-up game. */
    static byte[][] shiftedCode() {
        byte[][] c = sampleCode();
        long[] lead = new long[SHIFT / 4];
        for (int i = 0; i < lead.length; i++) {
            lead[i] = FILLER[i % FILLER.length];
        }
        byte[] text = Fixtures.concat(words(lead), Arrays.copyOf(c[0], TEXT_SIZE - SHIFT));
        return new byte[][] {text, c[1], c[2]};
    }

    /** CHEATS for the shifted build: every place in the code before the cave moves by SHIFT. */
    static String shiftedCheats() {
        List<String> out = new ArrayList<>();
        for (String line : Py.splitLines(CHEATS)) {
            List<String> parts = Py.splitSpace(line);
            if (parts.size() >= 3 && (parts.get(0).equals("04000000") || parts.get(0).equals("040A0000"))) {
                long at = Long.parseLong(parts.get(1), 16);
                if (FUNCTION <= at && at < TEXT_SIZE) {
                    parts.set(1, String.format("%08X", at + SHIFT));
                    line = String.join(" ", parts);
                }
            }
            out.add(line);
        }
        return String.join("\n", out) + "\n";
    }
}
