package io.github.davdxpx.mistel.mncf;

import io.github.davdxpx.mistel.mncf.testing.Check;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.Collections;
import java.util.HashSet;
import java.util.List;
import java.util.Set;

/**
 * Tests of the searches of a large module, which run in parts on the processor's cores: the places they find, their order
 * and their limits are those of one scan of the whole module, also where a place lies across two parts.
 */
public final class SearchPartsTest {

    private static final int SIZE = 8 << 20;

    private SearchPartsTest() {
    }

    /** Made-up bytes that hardly repeat a word by chance, the same in every run. */
    private static byte[] image(int size) {
        byte[] out = new byte[size];
        long x = 0x9E3779B97F4A7C15L;
        for (int i = 0; i < size; i++) {
            x ^= x << 13;
            x ^= x >>> 7;
            x ^= x << 17;
            out[i] = (byte) x;
        }
        return out;
    }

    private static void put(byte[] image, int at, int[] items) {
        for (int k = 0; k < items.length; k++) {
            if (items[k] >= 0) {
                image[at + k] = (byte) items[k];
            }
        }
    }

    /** Where the pattern lies in [start, end), one byte at a time. */
    private static List<Long> naive(byte[] image, int[] items, long start, long end) {
        List<Long> out = new ArrayList<>();
        for (long q = start; q + items.length <= end; q++) {
            boolean same = true;
            for (int k = 0; k < items.length && same; k++) {
                same = items[k] < 0 || (image[(int) q + k] & 0xFF) == items[k];
            }
            if (same) {
                out.add(q);
            }
        }
        return out;
    }

    /** Places around every boundary a split into 2 to 64 parts can have, and near the ends. */
    private static List<Integer> spots(int size, int length) {
        Set<Integer> out = new HashSet<>();
        for (int parts = 2; parts <= 64; parts++) {
            int part = (size + parts - 1) / parts;
            for (int i = 1; i < parts; i++) {
                for (int d = -length; d <= 1; d++) {
                    out.add(i * part + d);
                }
            }
        }
        out.add(0);
        out.add(1);
        out.add(size - length);
        List<Integer> list = new ArrayList<>(out);
        Collections.sort(list);
        List<Integer> kept = new ArrayList<>();
        int last = Integer.MIN_VALUE;
        for (int at : list) {
            if (at >= 0 && at + length <= size && at >= last + length) {   // places that do not overlap each other
                kept.add(at);
                last = at;
            }
        }
        return kept;
    }

    public static void testWordsAcrossParts() {
        Check.isTrue(Patterns.parts(SIZE) > 1, "a module of 8 MiB is searched in parts");
        byte[] image = image(SIZE);
        byte[] a = {0x11, 0x22, 0x33, 0x44};
        byte[] b = {0x55, 0x66, 0x77, (byte) 0x88};
        List<Integer> spots = spots(SIZE, 4);
        for (int i = 0; i < spots.size(); i++) {
            System.arraycopy(i % 2 == 0 ? a : b, 0, image, spots.get(i), 4);
        }
        int cut = SIZE / 2 + 2;                                    // a word across the end of a range is not in it
        System.arraycopy(a, 0, image, cut - 2, 4);
        List<long[]> ranges = Arrays.asList(new long[] {0, cut}, new long[] {cut, SIZE});
        List<List<Long>> got = Patterns.occurrences(image, Arrays.asList(a, b, a), ranges, 100000);
        int[] ia = {0x11, 0x22, 0x33, 0x44};
        int[] ib = {0x55, 0x66, 0x77, 0x88};
        List<Long> wantA = new ArrayList<>(naive(image, ia, 0, cut));
        wantA.addAll(naive(image, ia, cut, SIZE));
        List<Long> wantB = new ArrayList<>(naive(image, ib, 0, cut));
        wantB.addAll(naive(image, ib, cut, SIZE));
        Check.equal(wantA, got.get(0));
        Check.equal(wantB, got.get(1));
        Check.equal(wantA, got.get(2));
        Check.isTrue(!wantA.contains((long) (cut - 2)), "the word across the cut is in no range");
    }

    public static void testWordLimit() {
        byte[] image = image(SIZE);
        byte[] a = {0x11, 0x22, 0x33, 0x44};
        List<Integer> spots = spots(SIZE, 4);
        for (int at : spots) {
            System.arraycopy(a, 0, image, at, 4);
        }
        List<long[]> whole = Collections.singletonList(new long[] {0, SIZE});
        int n = naive(image, new int[] {0x11, 0x22, 0x33, 0x44}, 0, SIZE).size();
        Check.equal(n, Patterns.occurrences(image, Collections.singletonList(a), whole, n).get(0).size());
        Check.equal(null, Patterns.occurrences(image, Collections.singletonList(a), whole, n - 1).get(0));
    }

    public static void testPatternAcrossPartsAndSections() {
        byte[] image = image(SIZE);
        int[] items = {0x11, 0x22, -1, 0x44, 0x55, 0x66, 0x77, -1, 0x99};
        for (int at : spots(SIZE, items.length)) {
            put(image, at, items);
        }
        int aEnd = 6 << 20;
        int bStart = 4 << 20;                                       // code and rodata overlap here: a place in both counts once
        Module m = Module.builder(null).image(image).size(image.length)
                .section("code", 0, aEnd).section("rodata", bStart, SIZE).build();
        List<Long> want = new ArrayList<>(naive(image, items, 0, aEnd));
        for (long q : naive(image, items, bStart, SIZE)) {
            if (!want.contains(q)) {
                want.add(q);
            }
        }
        Check.isTrue(want.size() > 100, "places across the parts: " + want.size());
        Check.equal(want, Evaluator.findAll(m, items, null, 0));
        for (int limit : new int[] {1, 2, 7, want.size() - 1}) {
            Check.equal(want.subList(0, limit), Evaluator.findAll(m, items, null, limit));
        }
        Check.equal(naive(image, items, bStart, SIZE), Evaluator.findAll(m, items, "rodata", 0));
    }
}
