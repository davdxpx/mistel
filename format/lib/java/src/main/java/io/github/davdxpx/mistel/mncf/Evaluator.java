package io.github.davdxpx.mistel.mncf;

import java.math.BigDecimal;
import java.util.ArrayDeque;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.Collections;
import java.util.Deque;
import java.util.HashMap;
import java.util.HashSet;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;
import java.util.Objects;
import java.util.Set;

/**
 * Identifies the build that runs and gives every cheat its state, reason and resolved
 * operations (sections 5, 6, 11 and Appendix B).
 */
public final class Evaluator {

    private Evaluator() {
    }

    /** A cheat cannot be used here, for the reason given. */
    private static final class Refused extends Exception {
        private static final long serialVersionUID = 1L;
        final String reason;

        Refused(String reason) {
            super(reason, null, false, false);
            this.reason = reason;
        }
    }

    /** A pointer path cannot be followed yet. */
    private static final class Waiting extends Exception {
        private static final long serialVersionUID = 1L;

        Waiting() {
            super(null, null, false, false);
        }
    }

    /** The result of one cheat while the evaluation decides its state. */
    private static final class Result {
        final String id;
        String state;
        String reason;
        final String uses;
        final List<ResolvedOp> ops;
        boolean pathBroken;

        Result(String id, String state, String reason, String uses, List<ResolvedOp> ops) {
            this.id = id;
            this.state = state;
            this.reason = reason;
            this.uses = uses;
            this.ops = ops;
        }
    }

    /** A resolved place: its address and, for a place given in a module, the module and offset. */
    private static final class Place {
        final long address;
        final String module;
        final Long offset;
        final Module inModule;

        Place(long address, String module, Long offset, Module inModule) {
            this.address = address;
            this.module = module;
            this.offset = offset;
            this.inModule = inModule;
        }
    }

    private static final class SearchKey {
        final Module module;
        final int[] items;
        final String section;

        SearchKey(Module module, int[] items, String section) {
            this.module = module;
            this.items = items;
            this.section = section;
        }

        @Override
        public boolean equals(Object o) {
            if (!(o instanceof SearchKey)) {
                return false;
            }
            SearchKey k = (SearchKey) o;
            return module == k.module && Arrays.equals(items, k.items) && Objects.equals(section, k.section);
        }

        @Override
        public int hashCode() {
            return System.identityHashCode(module) * 31 + Arrays.hashCode(items) * 7 + Objects.hashCode(section);
        }
    }

    private static Result refused(String id, String reason) {
        return new Result(id, CheatResult.REFUSED, reason, "", Collections.<ResolvedOp>emptyList());
    }

    /** Returns the key of the build that runs, or null (section 5). */
    public static String identify(MncfFile f, Runtime rt) {
        if (rt.build() != null) {
            return f.builds().containsKey(rt.build()) ? rt.build() : null;
        }
        for (Build b : f.builds().values()) {
            boolean all = true;
            for (Map.Entry<String, String> e : b.modules().entrySet()) {
                Module m = rt.modules().get(e.getKey());
                if (m == null || m.id() == null || !f.platform().moduleIdMatches(e.getValue(), m.id())) {
                    all = false;
                    break;
                }
            }
            if (all) {
                return b.key();
            }
        }
        return null;
    }

    /** Evaluates a file in a runtime with every cheat as its default. */
    public static Evaluation evaluate(MncfFile f, Runtime rt) {
        return evaluate(f, rt, Choices.none());
    }

    /** Returns the build that runs and every cheat's state, reason and resolved operations. */
    public static Evaluation evaluate(MncfFile f, Runtime rt, Choices choices) {
        if (choices == null) {
            choices = Choices.none();
        }
        String build = identify(f, rt);
        String arch = rt.arch() != null && !rt.arch().isEmpty() ? rt.arch() : f.platform().archs().get(0);
        Map<String, Result> results = new LinkedHashMap<>();
        String fileReason = fileReason(f, rt);
        for (Cheat c : f.cheats()) {
            if (fileReason != null) {
                results.put(c.id(), refused(c.id(), fileReason));
            } else if (c.problem() != null) {
                results.put(c.id(), refused(c.id(), c.problem()));
            } else {
                results.put(c.id(), cheat(f, c, rt, build, arch, choices));
            }
        }
        relations(f, results);
        switchOn(f, results, choices, rt);
        List<CheatResult> out = new ArrayList<>();
        for (Cheat c : f.cheats()) {
            Result r = results.get(c.id());
            out.add(new CheatResult(r.id, r.state, r.reason, r.uses, r.ops));
        }
        return new Evaluation(build, out);
    }

    private static String fileReason(MncfFile f, Runtime rt) {
        if (!f.platform().name().equals(rt.platform())
                || (rt.gameId() != null && !Py.lower(rt.gameId()).equals(Py.lower(f.gameId())))) {
            return "game-mismatch";
        }
        String sig = f.signature().result();
        if (sig.equals(SignatureResult.INVALID) || sig.equals(SignatureResult.REVOKED)) {
            return "signature";
        }
        for (String name : f.requiredExtensions()) {
            if (!rt.extensions().contains(name)) {
                return "extension-missing:" + name;
            }
        }
        return null;
    }

    /** Whether the runtime applies this override by writing a stub over the function's first bytes. */
    private static boolean byBytes(Model.Override op, Runtime rt, String arch) {
        return rt.capabilities().contains("code-bytes") && op.expect != null && Encode.hasStub(arch);
    }

    private static Result cheat(MncfFile f, Cheat c, Runtime rt, String build, String arch, Choices choices) {
        List<String> buttonCaps = c.hold() != null || c.toggle() != null || c.press() != null
                ? Collections.singletonList("buttons") : Collections.<String>emptyList();
        List<String> caps = new ArrayList<>();
        List<String> exts = new ArrayList<>();
        for (Model.Op op : c.ops()) {
            boolean path = op.location().hasPath();
            if (op instanceof Model.Condition && ((Model.Condition) op).value instanceof Model.Place) {
                path |= ((Model.Place) ((Model.Condition) op).value).at.hasPath();
            }
            if (path || op instanceof Model.Condition || op instanceof Model.Read) {
                caps.add("guest-read");
            }
            if (op instanceof Model.Write) {
                caps.add("guest-write");
            }
            if (op instanceof Model.Patch) {
                caps.add("code-bytes");
                if (((Model.Patch) op).encode != null) {
                    exts.add("arm64-imm/1");
                }
            }
            if (op instanceof Model.Read) {
                caps.add("display");
            }
            if (op instanceof Model.Override && !byBytes((Model.Override) op, rt, arch)) {
                caps.add("code-hook");
            }
        }
        caps.addAll(buttonCaps);
        List<String> missing = new ArrayList<>();
        for (String cap : Mncf.CAPABILITIES) {
            if (caps.contains(cap) && !rt.capabilities().contains(cap)) {
                missing.add(cap);
            }
        }
        List<String> missingExt = new ArrayList<>();
        for (String e : exts) {
            if (!rt.extensions().contains(e)) {
                missingExt.add(e);
            }
        }
        boolean runsRaw = false;
        if (rt.extensions().contains("raw/1")) {
            for (Model.Raw r : c.raw()) {
                runsRaw |= rt.rawSyntaxes().contains(r.syntax);
            }
        }
        Model.Raw entry = null;
        for (Model.Raw r : c.raw()) {
            if (runsRaw && r.build.equals(build) && rt.rawSyntaxes().contains(r.syntax)) {
                entry = r;
                break;
            }
        }
        List<String> buttonsMissing = new ArrayList<>();
        for (String cap : buttonCaps) {
            if (!rt.capabilities().contains(cap)) {
                buttonsMissing.add(cap);
            }
        }
        if (!c.coreOps().isEmpty()) {
            if (missing.isEmpty() && missingExt.isEmpty()) {
                return core(f, c, rt, build, arch, choices);
            }
            if (entry != null && buttonsMissing.isEmpty()) {
                return raw(c, entry);
            }
            return refused(c.id(), !missing.isEmpty() ? "capability-missing:" + missing.get(0)
                    : "extension-missing:" + missingExt.get(0));
        }
        if (!runsRaw) {
            return refused(c.id(), "extension-missing:raw/1");
        }
        if (!buttonsMissing.isEmpty()) {
            return refused(c.id(), "capability-missing:" + buttonsMissing.get(0));
        }
        if (entry == null) {
            return refused(c.id(), "build-unknown");
        }
        return raw(c, entry);
    }

    private static Result raw(Cheat c, Model.Raw r) {
        ResolvedOp.Builder op = new ResolvedOp.Builder("raw");
        op.syntax = r.syntax;
        op.build = r.build;
        op.code = r.code;
        return new Result(c.id(), CheatResult.OFF, "", "raw", Collections.singletonList(op.done()));
    }

    /**
     * The value the player chose, or the default. A chosen value that the player cannot choose
     * refuses the cheat: not of the value's kind, NaN, too wide, outside min and max, or not
     * among the choices.
     */
    private static Object value(Cheat c, Model.ValueRef ref, Choices choices) throws Refused {
        ValueSpec spec = c.values().get(ref.name);
        String key = c.id() + "." + ref.name;
        if (!choices.values().containsKey(key)) {
            return spec.defaultValue();
        }
        Object v = choices.values().get(key);
        boolean ok;
        if (spec.type().equals("bool")) {
            ok = v instanceof Boolean;
        } else if (Values.isFloatType(spec.type())) {
            ok = Values.isNumber(v) && !Schema.isNan(v) && Values.fits(spec.type(), v);
        } else {
            ok = Values.isInt(v) && Values.fits(spec.type(), v);
        }
        if (!ok || !Schema.inSpec(spec, v)) {
            throw new Refused("value-out-of-range");
        }
        return v;
    }

    private static Result core(MncfFile f, Cheat c, Runtime rt, String build, String arch, Choices choices) {
        List<ResolvedOp> ops = new ArrayList<>();
        boolean waiting = false;
        Map<SearchKey, List<Long>> cache = new HashMap<>();
        try {
            for (Model.Op op : c.ops()) {
                try {
                    ops.add(op(f, c, op, rt, build, arch, choices, cache));
                } catch (Waiting w) {
                    waiting = true;
                    ops.add(new ResolvedOp.Builder(op.kind()).done());
                }
            }
        } catch (Refused r) {
            return refused(c.id(), r.reason);
        }
        Result result = new Result(c.id(), CheatResult.OFF, "", "core", ops);
        result.pathBroken = waiting;
        return result;
    }

    private static ResolvedOp op(MncfFile f, Cheat c, Model.Op op, Runtime rt, String build, String arch,
            Choices choices, Map<SearchKey, List<Long>> cache) throws Refused, Waiting {
        boolean big = f.platform().bigEndian();
        if (op instanceof Model.Patch) {
            Model.Patch p = (Model.Patch) op;
            Place place = resolve(f, p.at, rt, build, p.expect.length, cache);
            byte[] data;
            if (p.encode != null) {
                Model.Encoding e = p.encode;
                Object v = e.value instanceof Model.ValueRef ? value(c, (Model.ValueRef) e.value, choices) : e.value;
                data = Encode.arm64Imm(e.insn, e.width, e.rd, v);
                if (data == null) {
                    throw new Refused("value-out-of-range");
                }
            } else {
                data = p.bytes;
            }
            checkExpect(rt, place, p.expect, data);
            ResolvedOp.Builder out = new ResolvedOp.Builder("patch").place(place.address, place.module, place.offset);
            out.expect = p.expect;
            out.bytes = data;
            return out.done();
        }
        if (op instanceof Model.Write) {
            Model.Write w = (Model.Write) op;
            boolean isBytes = w.type.equals("bytes");
            int size = isBytes ? ((byte[]) w.value).length : Values.typeSize(w.type);
            Place place = resolve(f, w.at, rt, build, size, cache);
            byte[] data;
            byte[] expect;
            if (isBytes) {
                data = (byte[]) w.value;
                expect = (byte[]) w.expect;
            } else {
                Object v = w.value;
                if (v instanceof Model.ValueRef) {
                    v = value(c, (Model.ValueRef) v, choices);
                    if (v instanceof Boolean) {
                        v = ((Boolean) v) ? 1L : 0L;
                    }
                    if (!Values.fits(w.type, v)) {
                        throw new Refused("value-out-of-range");
                    }
                }
                data = Values.encode(w.type, v, big);
                expect = w.expect != null ? Values.encode(w.type, w.expect, big) : null;
            }
            ResolvedOp.Builder out = new ResolvedOp.Builder("write").place(place.address, place.module, place.offset);
            out.type = w.type;
            out.expect = expect;
            out.bytes = data;
            return out.done();
        }
        if (op instanceof Model.Override) {
            Model.Override o = (Model.Override) op;
            Object v = o.value;
            if (v instanceof Model.ValueRef) {
                v = whole(value(c, (Model.ValueRef) v, choices));
            }
            boolean patches = byBytes(o, rt, arch);
            byte[] stub = patches ? Encode.stub(arch, o.action, v == null ? 0 : Values.bits(v)) : null;
            int size = o.expect != null ? o.expect.length : 4;
            Place place = resolve(f, o.function, rt, build, size, cache);
            if (patches) {
                if (stub.length > o.expect.length) {
                    throw new Refused(o.value instanceof Model.ValueRef ? "value-out-of-range" : "invalid:override.expect");
                }
                checkExpect(rt, place, o.expect, null);
            }
            ResolvedOp.Builder out = new ResolvedOp.Builder("override").place(place.address, place.module, place.offset);
            out.action = o.action;
            out.value = v;
            out.expect = o.expect;
            out.bytes = stub;
            return out.done();
        }
        if (op instanceof Model.Read) {
            Model.Read r = (Model.Read) op;
            Place place = resolve(f, r.at, rt, build, Values.typeSize(r.type), cache);
            ResolvedOp.Builder out = new ResolvedOp.Builder("read").place(place.address, place.module, place.offset);
            out.type = r.type;
            out.label = r.label;
            return out.done();
        }
        Model.Condition cond = (Model.Condition) op;
        Place place = resolve(f, cond.at, rt, build, Values.typeSize(cond.type), cache);
        Object v = cond.value;
        if (v instanceof Model.ValueRef) {
            v = value(c, (Model.ValueRef) v, choices);
        } else if (v instanceof Model.Place) {
            Model.Place p = (Model.Place) v;
            Place other = resolve(f, p.at, rt, build, Values.typeSize(p.type), cache);
            Map<String, Object> m = new LinkedHashMap<>();
            m.put("address", Values.unsigned(other.address));
            m.put("type", p.type);
            v = Collections.unmodifiableMap(m);
        } else if (v instanceof Model.Between) {
            v = ResolvedOp.pair(((Model.Between) v).low, ((Model.Between) v).high);
        }
        ResolvedOp.Builder out = new ResolvedOp.Builder("when").place(place.address, place.module, place.offset);
        out.type = cond.type;
        out.op = cond.op;
        out.value = v;
        return out.done();
    }

    /** A value as a whole number, as Python's int() makes one: a float loses its fraction. */
    private static Object whole(Object v) throws Refused {
        if (v instanceof Boolean) {
            return ((Boolean) v) ? 1L : 0L;
        }
        if (v instanceof Double) {
            double d = (Double) v;
            if (Double.isNaN(d) || Double.isInfinite(d)) {
                throw new Refused("value-out-of-range");
            }
            return Values.normalize(new BigDecimal(d).toBigInteger());
        }
        return v;
    }

    /** Whether size bytes from offset lie inside a range of the given length, all unsigned. */
    private static boolean inside(long offset, long need, long length) {
        return Long.compareUnsigned(offset, length) <= 0 && Long.compareUnsigned(length - offset, need) >= 0;
    }

    private static Place resolve(MncfFile f, Model.Location loc, Runtime rt, String build, int size,
            Map<SearchKey, List<Long>> cache) throws Refused, Waiting {
        Platform plat = f.platform();
        int pointer = plat.pointerSize();
        int need = loc.hasPath() ? pointer : size;
        Place place;
        if (loc.kind.equals("address")) {
            place = new Place(loc.address, null, null, null);
            if (rt.read(loc.address, need) == null) {
                throw new Refused("outside");
            }
        } else if (loc.kind.equals("module") || loc.kind.equals("module-builds") || loc.kind.equals("pattern")) {
            Module m = rt.modules().get(loc.module);
            if (m == null) {
                throw new Refused("module-unknown");
            }
            long off;
            boolean negative = false;
            if (loc.kind.equals("module")) {
                off = loc.offset;
            } else if (loc.kind.equals("module-builds")) {
                if (build == null || !loc.offsets.containsKey(build)) {
                    throw new Refused("build-unknown");
                }
                off = loc.offsets.get(build);
            } else {
                long found = search(m, loc, cache);
                off = found + loc.offset;
                negative = loc.offset < 0 && off < 0;
            }
            if (negative || (m.size() != null && !inside(off, need, m.size()))) {
                throw new Refused("outside");
            }
            place = new Place(m.base() + off, loc.module, off, m);
        } else {
            long[] r = rt.regionOrNull(loc.region);
            if (r == null) {
                throw new Refused("region-unknown");
            }
            long off;
            if (loc.kind.equals("region")) {
                off = loc.offset;
            } else {
                if (build == null || !loc.offsets.containsKey(build)) {
                    throw new Refused("build-unknown");
                }
                off = loc.offsets.get(build);
            }
            if (!inside(off, need, r[1])) {
                throw new Refused("outside");
            }
            place = new Place(r[0] + off, null, null, null);
        }
        if (!loc.hasPath()) {
            return place;
        }
        long address = place.address;
        long mask = pointer >= 8 ? -1L : (1L << (8 * pointer)) - 1;
        for (long step : loc.path) {
            byte[] raw = rt.read(address, pointer);
            if (raw == null || isZero(raw)) {
                throw new Waiting();
            }
            address = (number(raw, plat.bigEndian()) + step) & mask;
        }
        if (rt.read(address, size) == null) {
            throw new Waiting();
        }
        return new Place(address, null, null, null);
    }

    private static boolean isZero(byte[] raw) {
        for (byte b : raw) {
            if (b != 0) {
                return false;
            }
        }
        return true;
    }

    private static long number(byte[] raw, boolean bigEndian) {
        long v = 0;
        int n = Math.min(raw.length, 8);
        for (int i = 0; i < n; i++) {
            int b = raw[bigEndian ? raw.length - 1 - i : i] & 0xFF;
            v |= (long) b << (8 * i);
        }
        return v;
    }

    private static long search(Module m, Model.Location loc, Map<SearchKey, List<Long>> cache) throws Refused {
        SearchKey key = new SearchKey(m, loc.find, loc.section);
        List<Long> found = cache.get(key);
        if (found == null) {
            found = findAll(m, loc.find, loc.section, 2);
            cache.put(key, found);
        }
        if (found.isEmpty()) {
            throw new Refused("pattern-not-found");
        }
        if (found.size() > 1) {
            throw new Refused("pattern-ambiguous");
        }
        return found.get(0);
    }

    /**
     * Returns every module offset where a pattern matches, within the searched sections, up to
     * a limit. Matches may overlap, and a match lies inside one section. Without a section, the
     * code and rodata sections are searched, in that order.
     *
     * @param pattern a pattern such as "68 ?? 00 B9": at least four items, fixed bytes at both ends
     * @param section code, rodata or data, or null for code and rodata
     * @param limit the most matches to return, or 0 for all
     * @throws IllegalArgumentException if the pattern is not valid
     */
    public static List<Long> findAll(Module module, String pattern, String section, int limit) {
        int[] items = Values.parsePattern(pattern);
        if (items == null) {
            throw new IllegalArgumentException("not a valid pattern: " + pattern);
        }
        return findAll(module, items, section, limit);
    }

    static List<Long> findAll(Module m, int[] items, String section, int limit) {
        List<Long> found = new ArrayList<>();
        // Sections that overlap count a place once.
        Set<Integer> seen = new HashSet<>();
        byte[] data = m.image();
        if (data == null) {
            return found;
        }
        // The longest run of fixed bytes is searched for, with a Horspool skip table, and
        // each place where it lies is then checked against the whole pattern.
        int anchorAt = 0;
        int anchorLength = 0;
        for (int i = 0; i < items.length; ) {
            if (items[i] < 0) {
                i++;
                continue;
            }
            int j = i;
            while (j < items.length && items[j] >= 0) {
                j++;
            }
            if (j - i > anchorLength) {
                anchorAt = i;
                anchorLength = j - i;
            }
            i = j;
        }
        byte[] anchor = new byte[anchorLength];
        for (int k = 0; k < anchorLength; k++) {
            anchor[k] = (byte) items[anchorAt + k];
        }
        int[] shift = new int[256];
        Arrays.fill(shift, anchorLength);
        for (int k = 0; k < anchorLength - 1; k++) {
            shift[anchor[k] & 0xFF] = anchorLength - 1 - k;
        }
        int lastAnchorByte = anchor[anchorLength - 1];
        int n = items.length;
        for (long[] range : m.searchRanges(section)) {
            long start = Math.max(0, Math.min(range[0], data.length));
            long end = Math.max(0, Math.min(range[1], data.length));
            if (end - start < n) {
                continue;
            }
            int q = (int) start + anchorAt;
            int last = (int) end - n + anchorAt;
            while (q <= last) {
                byte b = data[q + anchorLength - 1];
                if (b == lastAnchorByte && headMatches(data, q, anchor) && matches(data, q - anchorAt, items)
                        && seen.add(q - anchorAt)) {
                    found.add((long) (q - anchorAt));
                    if (limit > 0 && found.size() >= limit) {
                        return found;
                    }
                }
                int next = q + shift[b & 0xFF];
                if (next < q) {
                    break;
                }
                q = next;
            }
        }
        return found;
    }

    /** Whether the run's bytes but its last, which the caller has checked, lie at the position. */
    private static boolean headMatches(byte[] data, int at, byte[] run) {
        for (int k = 0; k < run.length - 1; k++) {
            if (data[at + k] != run[k]) {
                return false;
            }
        }
        return true;
    }

    private static boolean matches(byte[] data, int at, int[] items) {
        for (int k = 0; k < items.length; k++) {
            if (items[k] >= 0 && (data[at + k] & 0xFF) != items[k]) {
                return false;
            }
        }
        return true;
    }

    private static void checkExpect(Runtime rt, Place place, byte[] expect, byte[] data) throws Refused {
        byte[] actual;
        if (place.inModule != null && place.inModule.image() != null) {
            byte[] image = place.inModule.image();
            long start = place.offset;
            if (Long.compareUnsigned(start, image.length) >= 0) {
                actual = new byte[0];
            } else {
                int s = (int) start;
                int e = (int) Math.min(image.length, s + (long) expect.length);
                actual = Arrays.copyOfRange(image, s, e);
            }
        } else {
            actual = rt.read(place.address, expect.length);
            if (actual == null) {
                throw new Refused("outside");
            }
        }
        if (!Arrays.equals(actual, expect) && (data == null || !Arrays.equals(actual, data))) {
            throw new Refused("expect-mismatch");
        }
    }

    // Relations between cheats (section 9)

    /** The cheat and every cheat it requires, directly or not, in file order. */
    private static List<String> closure(MncfFile f, String id) {
        Set<String> seen = new HashSet<>();
        Deque<String> todo = new ArrayDeque<>();
        todo.push(id);
        while (!todo.isEmpty()) {
            String x = todo.pop();
            if (!seen.add(x)) {
                continue;
            }
            for (String r : f.cheat(x).requires()) {
                todo.push(r);
            }
        }
        List<String> out = new ArrayList<>();
        for (Cheat c : f.cheats()) {
            if (seen.contains(c.id())) {
                out.add(c.id());
            }
        }
        return out;
    }

    private static void relations(MncfFile f, Map<String, Result> results) {
        boolean changed = true;
        while (changed) {
            changed = false;
            for (Cheat c : f.cheats()) {
                if (results.get(c.id()).state.equals(CheatResult.REFUSED)) {
                    continue;
                }
                for (String req : c.requires()) {
                    if (results.get(req).state.equals(CheatResult.REFUSED)) {
                        results.put(c.id(), refused(c.id(), "requires:" + req));
                        changed = true;
                        break;
                    }
                }
            }
        }
        for (Cheat c : f.cheats()) {
            if (results.get(c.id()).state.equals(CheatResult.REFUSED)) {
                continue;
            }
            String conflict = conflictIn(f, closure(f, c.id()), c.id());
            if (conflict != null) {
                results.put(c.id(), refused(c.id(), "excludes:" + conflict));
            }
        }
    }

    /** A cheat of the group that can never be on together with the rest: what excludes:&lt;id&gt; names. */
    private static String conflictIn(MncfFile f, List<String> group, String own) {
        Set<String> members = new HashSet<>(group);
        for (String x : group) {
            for (String y : f.cheat(x).excludes()) {
                if (members.contains(y)) {
                    return !y.equals(own) ? y : x;
                }
            }
        }
        for (int i = 0; i < group.size(); i++) {
            String radio = f.cheat(group.get(i)).radio();
            if (radio == null) {
                continue;
            }
            for (int j = i + 1; j < group.size(); j++) {
                String y = group.get(j);
                if (radio.equals(f.cheat(y).radio())) {
                    return !y.equals(own) ? y : group.get(i);
                }
            }
        }
        return null;
    }

    private static boolean conflicts(MncfFile f, String a, String b) {
        Cheat ca = f.cheat(a);
        Cheat cb = f.cheat(b);
        return !a.equals(b) && (ca.excludes().contains(b) || cb.excludes().contains(a)
                || (ca.radio() != null && ca.radio().equals(cb.radio())));
    }

    private static void switchOn(MncfFile f, Map<String, Result> results, Choices choices, Runtime rt) {
        List<String> wanted = new ArrayList<>();
        for (Cheat c : f.cheats()) {
            if (!c.isMaster() && ((c.isDefault() && !choices.off().contains(c.id())) || choices.on().contains(c.id()))) {
                wanted.add(c.id());
            }
        }
        List<String> on = new ArrayList<>();
        for (String x : wanted) {
            if (results.get(x).state.equals(CheatResult.REFUSED)) {
                continue;
            }
            List<String> group = closure(f, x);
            List<String> kept = new ArrayList<>();
            for (String y : on) {
                boolean clash = false;
                for (String g : group) {
                    clash |= conflicts(f, y, g);
                }
                if (!clash) {
                    kept.add(y);
                }
            }
            on = dropUnmet(f, kept);
            for (String g : group) {
                if (!on.contains(g)) {
                    on.add(g);
                }
            }
        }
        Set<String> needed = new HashSet<>();
        for (String y : on) {
            if (!f.cheat(y).isMaster()) {
                needed.addAll(closure(f, y));
            }
        }
        List<String> kept = new ArrayList<>();
        for (String y : on) {
            if (needed.contains(y)) {
                kept.add(y);
            }
        }
        on = kept;
        for (Cheat c : f.cheats()) {
            Result r = results.get(c.id());
            if (r.state.equals(CheatResult.REFUSED)) {
                continue;
            }
            if (!on.contains(c.id())) {
                r.state = CheatResult.OFF;
            } else if (r.pathBroken) {
                r.state = CheatResult.WAITING;
                r.reason = "path-broken";
            } else if (c.hold() != null && !rt.held().containsAll(Py.split(c.hold(), '+'))) {
                r.state = CheatResult.WAITING;
            } else {
                r.state = CheatResult.ON;
            }
        }
    }

    private static List<String> dropUnmet(MncfFile f, List<String> on) {
        while (true) {
            List<String> kept = new ArrayList<>();
            for (String y : on) {
                if (on.containsAll(f.cheat(y).requires())) {
                    kept.add(y);
                }
            }
            if (kept.equals(on)) {
                return on;
            }
            on = kept;
        }
    }
}
