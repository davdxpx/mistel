package io.github.davdxpx.mistel.mncf;

import java.util.ArrayList;
import java.util.Arrays;
import java.util.Collections;
import java.util.Comparator;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;
import java.util.Set;
import java.util.function.IntConsumer;
import java.util.stream.IntStream;

/**
 * The shortest byte pattern that finds one place in a module exactly once (section 6.3).
 *
 * <p>The bytes that change when the code moves in another build (branch and page offsets) become
 * ??, so that a pattern keeps finding its place after an update as long as the code around it
 * stays the same.
 */
final class Patterns {

    private static final int STEP = 4;
    /** Candidates left after which a search looks at its last window first. */
    private static final int CROWD = 64;

    private Patterns() {
    }

    /** A pattern: its items (-1 for ??), where it matches, and the place's offset from the match. */
    static final class Found {
        final int[] items;
        final long start;
        final long place;

        Found(int[] items, long start, long place) {
            this.items = items;
            this.start = start;
            this.place = place;
        }

        String text() {
            StringBuilder b = new StringBuilder();
            for (int i = 0; i < items.length; i++) {
                if (i > 0) {
                    b.append(' ');
                }
                b.append(items[i] < 0 ? "??" : String.format("%02X", items[i]));
            }
            return b.toString();
        }

        /** The location of the place: the module, the pattern, and the offset unless it is 0. */
        Map<String, Object> location(String module) {
            Map<String, Object> loc = new LinkedHashMap<>();
            loc.put("module", module);
            loc.put("find", text());
            if (place != 0) {
                loc.put("offset", place);
            }
            return loc;
        }
    }

    /** The default search: code and read-only data, cut where the image ends. */
    private static List<long[]> searchRanges(CodeModule m) {
        long size = m.image().length;
        List<long[]> ranges = new ArrayList<>();
        for (String n : new String[] {"code", "rodata"}) {
            if (m.hasSection(n)) {
                ranges.add(new long[] {Math.min(m.sectionStart(n), size), Math.min(m.sectionEnd(n), size)});
            }
        }
        return ranges;
    }

    /** The window's bytes, -1 where they move between builds or are not the module's own. */
    private static int[] items(CodeModule m, int start, int end, Set<Integer> moving) {
        int[] out = new int[end - start];
        byte[] image = m.image();
        for (int i = start; i < end; i++) {
            out[i - start] = moving.contains(i) || m.excluded(i, 1) ? -1 : image[i] & 0xFF;
        }
        return out;
    }

    /** A word of fixed bytes of the window: its distance from the place, its offset and its bytes. */
    private static final class Anchor {
        final long distance;
        final long start;
        final byte[] piece;

        Anchor(long distance, long start, byte[] piece) {
            this.distance = distance;
            this.start = start;
            this.piece = piece;
        }
    }

    /** The words of fixed bytes in the window, aligned as the place is, nearest the place first. */
    private static List<Anchor> anchors(int[] items, long base, long site) {
        int first = (int) Math.floorMod(site - base, (long) STEP);
        List<Anchor> words = new ArrayList<>();
        for (int i = first; i < items.length - STEP + 1; i += STEP) {
            byte[] piece = new byte[STEP];
            boolean fixed = true;
            for (int k = 0; k < STEP; k++) {
                fixed &= items[i + k] >= 0;
                piece[k] = (byte) items[i + k];
            }
            if (fixed) {
                words.add(new Anchor(Math.abs(base + i - site), base + i, piece));
            }
        }
        Collections.sort(words, new Comparator<Anchor>() {
            @Override
            public int compare(Anchor a, Anchor b) {
                int c = Long.compare(a.distance, b.distance);
                return c != 0 ? c : Long.compare(a.start, b.start);
            }
        });
        return words;
    }

    /**
     * Where each word of STEP bytes lies in the ranges, overlapping: a list for each word, in the image's
     * order, or null for a word that lies there more than limit times. One pass finds every word, so a
     * place's anchors cost one look at the image, not one each.
     */
    static List<List<Long>> occurrences(byte[] image, List<byte[]> needles, List<long[]> ranges, int limit) {
        int n = needles.size();
        int[] words = new int[n];
        for (int j = 0; j < n; j++) {
            byte[] w = needles.get(j);
            words[j] = (w[0] & 0xFF) | (w[1] & 0xFF) << 8 | (w[2] & 0xFF) << 16 | (w[3] & 0xFF) << 24;
        }
        List<List<Long>> found = new ArrayList<>();
        boolean[] over = new boolean[n];
        for (int j = 0; j < n; j++) {
            found.add(new ArrayList<Long>());
        }
        for (long[] r : ranges) {
            final int start = (int) Math.max(0, Math.min(r[0], image.length));
            final int end = (int) Math.max(0, Math.min(r[1], image.length));
            final int parts = parts(end - start);
            final int size = (end - start + parts - 1) / parts;
            final int[][][] got = new int[parts][][];
            IntStream each = IntStream.range(0, parts);
            (parts > 1 ? each.parallel() : each).forEach(new IntConsumer() {
                @Override
                public void accept(int i) {
                    int from = start + i * size;
                    got[i] = words(image, words, from, Math.min(end, from + size + STEP - 1), limit);
                }
            });
            for (int i = 0; i < parts; i++) {
                for (int j = 0; j < n; j++) {
                    if (got[i][j] == null) {
                        over[j] = true;
                    }
                    for (int k = 0; !over[j] && k < got[i][j].length; k++) {
                        found.get(j).add((long) got[i][j][k]);
                        over[j] = found.get(j).size() > limit;
                    }
                }
            }
        }
        for (int j = 0; j < n; j++) {
            if (over[j]) {
                found.set(j, null);
            }
        }
        return found;
    }

    /** Where the words start in image[from, to): their offsets for each word, or null past limit of them. */
    private static int[][] words(byte[] image, int[] words, int from, int to, int limit) {
        int n = words.length;
        int[][] at = new int[n][16];
        int[] count = new int[n];
        if (to - from >= STEP && n > 0) {
            int w0 = words[0];
            int w1 = n > 1 ? words[1] : w0;
            int w2 = n > 2 ? words[2] : w0;
            int w = (image[from] & 0xFF) << 8 | (image[from + 1] & 0xFF) << 16 | (image[from + 2] & 0xFF) << 24;
            for (int q = from; q + STEP <= to; q++) {
                w = w >>> 8 | (image[q + 3] & 0xFF) << 24;      // the four bytes from q, as the needles are read
                if (w != w0 && w != w1 && w != w2 && n <= 3) {
                    continue;
                }
                for (int j = 0; j < n; j++) {
                    if (w == words[j] && count[j] <= limit) {
                        if (count[j] == at[j].length) {
                            at[j] = Arrays.copyOf(at[j], 2 * count[j]);
                        }
                        at[j][count[j]++] = q;
                    }
                }
            }
        }
        int[][] out = new int[n][];
        for (int j = 0; j < n; j++) {
            out[j] = count[j] > limit ? null : Arrays.copyOf(at[j], count[j]);
        }
        return out;
    }

    /** How many parts a search of this many bytes is split into, for the processor's cores; 1 for a small one. */
    static int parts(long length) {
        int cores = java.lang.Runtime.getRuntime().availableProcessors();
        long most = length / (1 << 20);                        // a part of at least 1 MiB is worth a thread
        return (int) Math.max(1, Math.min(2L * cores, most));
    }

    /**
     * The shortest pattern that finds an offset exactly once and covers the first cover bytes of
     * the place, or null when the place is not in code or read-only data, or no pattern up to 64
     * bytes is unique.
     */
    static Found make(CodeModule m, long offset, int cover) {
        int longest = 64;
        int limit = 200000;
        String section = m.sectionOf(offset);
        if (!"code".equals(section) && !"rodata".equals(section)) {
            return null;
        }
        long secStart = m.sectionStart(section);
        long secEnd = Math.min(m.sectionEnd(section), m.image().length);
        if (section.equals("code") && m.textSize() != null) {
            if (offset >= m.textSize()) {
                return null;                      // the zero bytes after the code: nothing there to find
            }
            secEnd = Math.min(secEnd, m.textSize());
        }
        byte[] image = m.image();
        int lo = (int) Math.max(secStart, offset - longest);
        int hi = (int) Math.min(secEnd, offset + longest);
        Set<Integer> moving = Arm64.movingBytes(image, lo, hi);
        int[] window = items(m, lo, hi, moving);
        cover = (int) Math.max(1, Math.min(cover, hi - offset));
        List<long[]> ranges = searchRanges(m);
        Found best = null;
        List<Anchor> all = anchors(window, lo, offset);
        List<Anchor> first = all.subList(0, Math.min(3, all.size()));
        List<byte[]> pieces = new ArrayList<>();
        for (Anchor a : first) {
            pieces.add(a.piece);
        }
        List<List<Long>> where = occurrences(image, pieces, ranges, limit);
        for (int i = 0; i < first.size(); i++) {
            Anchor a = first.get(i);
            List<Long> hits = where.get(i);
            if (hits == null) {
                continue;
            }
            for (String order : new String[] {"forward", "backward", "both"}) {
                Found found = grow(image, offset, cover, lo, hi, window, a.start, hits, ranges, order, longest);
                if (found != null && (best == null || found.items.length < best.items.length
                        || (found.items.length == best.items.length && Math.abs(found.place) < Math.abs(best.place)))) {
                    best = found;
                }
            }
        }
        if (best == null) {
            return null;
        }
        List<Long> check = Evaluator.findAll(m.toModuleWithoutId(), best.items, null, 2);
        if (!check.equals(Collections.singletonList(best.start))) {
            return null;
        }
        return best;
    }

    private static Found grow(byte[] image, long offset, int cover, int lo, int hi, int[] window, long a0,
            List<Long> hits, List<long[]> ranges, String order, int longest) {
        long start = Math.min(offset, a0);
        if (offset % STEP == 0) {
            start -= Math.floorMod(start, (long) STEP);
        }
        long end = Math.max(offset + cover, a0 + 1);
        end = Math.min(hi, start + ((end - start + STEP - 1) / STEP) * STEP);
        List<Long> alive = null;
        long[] verified = null;     // the module offsets of the pattern whose bytes every candidate was checked for
        boolean lookedAhead = false;
        int toggle = 0;
        while (true) {
            int[] items = slice(window, start - lo, end - lo);
            if (items.length > 0 && items[0] >= 0 && items[items.length - 1] >= 0
                    && start <= a0 && a0 < end && end - start >= 4) {
                long shift = a0 - start;
                if (alive == null) {
                    alive = new ArrayList<>();
                    for (long h : hits) {
                        alive.add(h - shift);
                    }
                }
                boolean whole = items.length == end - start;
                // The pattern only grows: a candidate needs checking only for the bytes added since it was checked.
                int doneFrom = 0;
                int doneTo = 0;
                if (verified != null && whole) {
                    doneFrom = (int) (verified[0] - start);
                    doneTo = (int) (verified[1] - start);
                }
                List<Long> kept = new ArrayList<>();
                for (long q : alive) {
                    if (matches(image, q, items, ranges, doneFrom, doneTo)) {
                        kept.add(q);
                    }
                }
                alive = kept;
                verified = whole ? new long[] {start, end} : null;
                if (alive.size() == 1 && alive.get(0) == start) {
                    return new Found(items, start, offset - start);
                }
                if (alive.size() > CROWD && !lookedAhead) {
                    // Many candidates are left. Every later window lies inside the last one this order grows to,
                    // so where two candidates match that one, none of the windows before it can be unique.
                    lookedAhead = true;
                    long[] last = lastWindow(start, end, lo, hi, order, longest, toggle);
                    int[] lastItems = slice(window, last[0] - lo, last[1] - lo);
                    if (lastItems.length == last[1] - last[0]) {
                        int matching = 0;
                        for (long q : alive) {
                            if (matches(image, q - (start - last[0]), lastItems, ranges, 0, 0) && ++matching == 2) {
                                return null;
                            }
                        }
                    }
                }
            }
            boolean growBack = start - STEP >= lo;
            boolean growAhead = end + STEP <= hi;
            if (end - start >= longest || !(growBack || growAhead)) {
                return null;
            }
            boolean ahead;
            if (order.equals("forward")) {
                ahead = growAhead;
            } else if (order.equals("backward")) {
                ahead = !growBack;
            } else {
                ahead = growAhead && (toggle % 2 == 0 || !growBack);
                toggle++;
            }
            if (ahead) {
                end += STEP;
            } else {
                start -= STEP;
                if (alive != null) {
                    List<Long> moved = new ArrayList<>();
                    for (long q : alive) {
                        moved.add(q - STEP);
                    }
                    alive = moved;
                }
            }
        }
    }

    /** Python's list slice: indices below 0 count from the end, and both ends stay inside the list. */
    private static int[] slice(int[] list, long from, long to) {
        int n = list.length;
        long a = from < 0 ? Math.max(0, from + n) : Math.min(from, n);
        long b = to < 0 ? Math.max(0, to + n) : Math.min(to, n);
        return b <= a ? new int[0] : Arrays.copyOfRange(list, (int) a, (int) b);
    }

    /** The window that growing from (start, end) in this order ends with. */
    private static long[] lastWindow(long start, long end, int lo, int hi, String order, int longest, int toggle) {
        while (true) {
            boolean growBack = start - STEP >= lo;
            boolean growAhead = end + STEP <= hi;
            if (end - start >= longest || !(growBack || growAhead)) {
                return new long[] {start, end};
            }
            boolean ahead;
            if (order.equals("forward")) {
                ahead = growAhead;
            } else if (order.equals("backward")) {
                ahead = !growBack;
            } else {
                ahead = growAhead && (toggle % 2 == 0 || !growBack);
                toggle++;
            }
            if (ahead) {
                end += STEP;
            } else {
                start -= STEP;
            }
        }
    }

    /** Whether the pattern matches at q, inside the search ranges; the items from doneFrom to doneTo are known to. */
    private static boolean matches(byte[] image, long q, int[] items, List<long[]> ranges, int doneFrom, int doneTo) {
        int n = items.length;
        boolean inside = false;
        for (long[] r : ranges) {
            inside |= r[0] <= q && q + n <= r[1];
        }
        if (!inside || q < 0 || q + n > image.length) {
            return false;
        }
        for (int i = 0; i < n; i++) {
            if (i == doneFrom && doneTo > doneFrom) {
                i = doneTo - 1;
                continue;
            }
            if (items[i] >= 0 && (image[(int) q + i] & 0xFF) != items[i]) {
                return false;
            }
        }
        return true;
    }
}
