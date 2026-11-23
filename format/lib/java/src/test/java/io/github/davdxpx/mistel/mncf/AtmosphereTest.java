package io.github.davdxpx.mistel.mncf;

import io.github.davdxpx.mistel.mncf.testing.Check;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.Collections;
import java.util.HashSet;
import java.util.List;

/** Tests of reading, decoding, understanding and writing Atmosphère cheat code. */
public final class AtmosphereTest {

    private AtmosphereTest() {
    }

    private static Atmosphere.Entry entry(long... words) {
        return new Atmosphere.Entry("x", false, 1, Collections.<String>emptyList(), words);
    }

    private static List<String> stores(Atmosphere.Derived d) {
        List<String> out = new ArrayList<>();
        for (Atmosphere.Store s : d.stores) {
            out.add(describe(s.place) + " " + s.width + " " + Long.toHexString(s.value));
        }
        return out;
    }

    private static String describe(Atmosphere.Place p) {
        return p.region + "+" + Long.toHexString(p.offset) + (p.path == null ? "" : Arrays.toString(p.path));
    }

    public static void testParse() throws Exception {
        List<Atmosphere.Entry> entries =
                Atmosphere.parse("[A] 04000000 00000010 00000001\n{M}\n04000000 00000020\n 00000002\n[Empty]\n");
        Check.equal(3, entries.size());
        Check.equal("A", entries.get(0).name());
        Check.isTrue(!entries.get(0).master() && entries.get(1).master(), "{M} is the master entry");
        Check.isTrue(Arrays.equals(new long[] {0x04000000L, 0x10, 1}, entries.get(0).words()), "words of A");
        Check.isTrue(Arrays.equals(new long[] {0x04000000L, 0x20, 2}, entries.get(1).words()), "words of M");
        Check.equal(0, entries.get(2).words().length);
        Check.equal("04000000 00000020\n00000002\n", entries.get(1).code());
        Check.equal(3, entries.get(1).line() + 1);
        for (final String bad : new String[] {"04000000", "[A\n", "[A]\n0400 0000"}) {
            Check.throwsWith(AtmosphereException.class, "line", new Check.Action() {
                @Override
                public void run() throws Exception {
                    Atmosphere.parse(bad);
                }
            });
        }
    }

    public static void testDecodeLengths() throws Exception {
        long[] ws = {0x08000000L, 0x10, 1, 2, 0x40F00000L, 1, 2, 0x580F0000L, 0x10, 0x780F0000L, 8, 0x80000001L,
            0x20000000L, 0x9409F100L, 5, 0x300F0000L, 2, 0x310F0000L, 0xA4F00000L, 0xFF000000L, 0xC4000000L, 0, 2};
        List<String> kinds = new ArrayList<>();
        for (Atmosphere.Op op : Atmosphere.decode(ws)) {
            kinds.add(op.kind);
        }
        Check.equal(Arrays.asList("store", "load-const", "load-mem", "arith", "keys", "end-cond", "arith-reg", "loop",
                "loop", "store-reg", "pause", "keys"), kinds);
        Check.throwsWith(Atmosphere.Unclear.class, "code type B", new Check.Action() {
            @Override
            public void run() throws Exception {
                Atmosphere.decode(new long[] {0xB0000000L});
            }
        });
    }

    public static void testDerive() throws Exception {
        Atmosphere.Derived d = Atmosphere.derive(entry(0x580F0000L, 0x3020, 0x580F1000L, 0x10, 0x780F0000L, 8,
                0x640F0000L, 0, 0x63));
        Check.equal(Collections.singletonList("main+3020[16, 8] 4 63"), stores(d));
        d = Atmosphere.derive(entry(0x80000001L, 0x04000000L, 0x100C, 0xD503201FL, 0x21000000L, 0x04000000L, 0x100C,
                0x94000040L, 0x20000000L));
        Check.equal(1L, d.keys);
        Check.equal(1, d.stores.size());
        Check.equal(1, d.released.size());
        d = Atmosphere.derive(entry(0x14150000L, 0x100, 5, 0x02100000L, 0x104, 1, 0x20000000L));
        Check.equal("heap+100", describe(d.condition.place));
        Check.equal("==", d.condition.op);
        Check.equal(5L, d.condition.value);
        Check.throwsWith(Atmosphere.Unclear.class, "a loop", new Check.Action() {
            @Override
            public void run() throws Exception {
                Atmosphere.derive(entry(0x300F0000L, 4, 0x310F0000L));
            }
        });
        d = Atmosphere.derive(entry(0x040A0000L, 0x10, 1));
        Check.equal(new HashSet<>(Collections.singletonList(0xAL)), d.reads);
    }

    public static void testStoreWordsRoundTrip() throws Exception {
        Object[][] cases = {
            {new Atmosphere.Place("main", 0x1234), 4, 0xD503201FL},
            {new Atmosphere.Place("heap", 0x10_0000_0000L), 8, 0x1122334455667788L},
            {new Atmosphere.Place("main", 0x1234560, new long[] {0x18, 0x20}), 4, 0x94L},
            {new Atmosphere.Place("alias", 0x10, new long[] {-8L, 0}), 2, 7L},
        };
        for (Object[] c : cases) {
            Atmosphere.Place place = (Atmosphere.Place) c[0];
            int width = (Integer) c[1];
            long value = (Long) c[2];
            List<Long> words = Atmosphere.storeWords(place, width, value);
            long[] w = new long[words.size()];
            for (int i = 0; i < w.length; i++) {
                w[i] = words.get(i);
            }
            Atmosphere.Derived d = Atmosphere.derive(entry(w));
            Check.equal(1, d.stores.size());
            Check.isTrue(d.stores.get(0).place.equals(place), "the place of " + describe(place));
            Check.equal(width, d.stores.get(0).width);
            Check.equal(value, d.stores.get(0).value);
        }
    }

    public static void testWidePathSteps() throws Exception {
        Atmosphere.Place[] places = {new Atmosphere.Place("main", 0x10, new long[] {0x18, 1L << 32}),
            new Atmosphere.Place("heap", 0x20, new long[] {-0x20L, 5L << 40, 8})};
        for (Atmosphere.Place place : places) {
            List<Long> words = Atmosphere.storeWords(place, 4, 7);
            long[] w = new long[words.size()];
            for (int i = 0; i < w.length; i++) {
                w[i] = words.get(i);
            }
            Check.isTrue(Atmosphere.derive(entry(w)).stores.get(0).place.equals(place), "the path of " + describe(place));
        }
        Check.throwsWith(Atmosphere.Unclear.class, "40 bits of a store", new Check.Action() {
            @Override
            public void run() throws Exception {
                Atmosphere.storeWords(new Atmosphere.Place("main", 1L << 40), 4, 0);
            }
        });
        Check.throwsWith(Atmosphere.Unclear.class, "40 bits of a condition", new Check.Action() {
            @Override
            public void run() throws Exception {
                Atmosphere.conditionWords(new Atmosphere.Place("heap", 1L << 40), 4, "==", 0);
            }
        });
    }

    public static void testOddStoreWidths() {
        for (final long[] words : new long[][] {{0x580F0000L, 0x10, 0x630F0000L, 0, 1},
            {0x40000000L, 0, 5, 0x580F0000L, 0x10, 0xA30F0000L}}) {
            Check.throwsWith(Atmosphere.Unclear.class, "a store of width 3", new Check.Action() {
                @Override
                public void run() throws Exception {
                    Atmosphere.derive(entry(words));
                }
            });
        }
    }

    public static void testKeysAndRegisters() {
        Check.equal("B+ZL", Atmosphere.keysToButtons(0x102));
        Check.equal(0x102L, Atmosphere.buttonsToKeys("B+ZL"));
        Check.isTrue(Atmosphere.keysToButtons(0x10000000) == null, "a bit without a name");
        Check.isTrue(Atmosphere.keysToButtons(0) == null, "no buttons");
        Check.throwsWith(IllegalArgumentException.class, "key mask", new Check.Action() {
            @Override
            public void run() {
                Atmosphere.buttonsToKeys("A+C");
            }
        });
        Check.equal(new HashSet<>(Arrays.asList(0xEL, 0xFL)),
                Atmosphere.registersWritten(entry(0x300F0000L, 4, 0x400E0000L, 0, 1, 0x310F0000L)));
        Check.equal(16, Atmosphere.registersWritten(entry(0xB0000000L)).size());
    }

    public static void testFormatAndWriteFile() throws Exception {
        Check.equal(Arrays.asList("04000000 00001008 B9001269", "20000000"),
                Atmosphere.formatWords(Arrays.asList(0x04000000L, 0x1008L, 0xB9001269L, 0x20000000L)));
        List<Atmosphere.Out> entries = Arrays.asList(new Atmosphere.Out("M", true, Collections.singletonList("20000000")),
                new Atmosphere.Out("A", false, Collections.<String>emptyList()));
        Check.equal("{M}\n20000000\n\n[A]\n", Atmosphere.writeFile(entries));
    }
}
