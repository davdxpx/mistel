package io.github.davdxpx.mistel.mncf;

import io.github.davdxpx.mistel.mncf.testing.Check;
import io.github.davdxpx.mistel.mncf.toml.Toml;
import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.IOException;
import java.math.BigInteger;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.Base64;
import java.util.Collections;
import java.util.HashSet;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;
import java.util.TreeMap;
import java.util.zip.ZipEntry;
import java.util.zip.ZipOutputStream;

/**
 * Runs the conformance suite in format/tests against this reader, with the comparison rules of
 * the Python suite runner: an expected operation lists only the fields it checks, hexadecimal
 * bytes compare without regard to case, and warnings compare sorted.
 *
 * <p>The suite folder is the system property mncf.tests, or format/tests found from the working
 * folder. Run as a program with --dump, it prints each case's results as JSON in the layout of
 * the Python runner's --dump, so that the two readers can be compared line by line.
 */
public final class ConformanceTest {

    private ConformanceTest() {
    }

    /** One case of the suite. */
    static final class Case {
        final String name;
        final Map<String, Object> table;
        final File folder;
        final String kind;

        Case(String name, Map<String, Object> table, File folder, String kind) {
            this.name = name;
            this.table = table;
            this.folder = folder;
            this.kind = kind;
        }
    }

    /** The kinds of cases, by folder: files the reader reads, packs, conversions, and passes. */
    private static final String[] KINDS = {"cases", "packs", "convert", "passes"};

    public static void testSuite() throws Exception {
        List<String> problems = new ArrayList<>();
        int total = 0;
        int failed = 0;
        for (Case c : cases()) {
            total++;
            List<String> found = check(c, results(c));
            if (!found.isEmpty()) {
                failed++;
                for (String p : found) {
                    problems.add(c.name + ": " + p);
                }
            }
        }
        Check.isTrue(total > 0, "the suite has no cases");
        if (failed > 0) {
            problems.add("conformance: " + (total - failed) + " of " + total + " cases passed");
            Check.fail(String.join("\n", problems));
        }
    }

    public static void testEveryCaseFileIsRun() throws Exception {
        File suite = suite();
        int files = 0;
        for (String kind : KINDS) {
            File[] all = new File(suite, kind).listFiles();
            for (File f : all == null ? new File[0] : all) {
                files += f.getName().endsWith(".toml") ? 1 : 0;
            }
        }
        Check.equal(files, cases().size());
    }

    /** Runs the suite and prints one line per failure and a summary, or with --dump the results as JSON. */
    public static void main(String[] args) throws Exception {
        boolean dump = Arrays.asList(args).contains("--dump");
        int total = 0;
        int failed = 0;
        Map<String, Object> all = new TreeMap<>();
        for (Case c : cases()) {
            total++;
            Map<String, Object> got = results(c);
            all.put(c.name, got);
            List<String> found = check(c, got);
            if (!found.isEmpty()) {
                failed++;
                for (String p : found) {
                    System.out.println(c.name + ": " + p);
                }
            }
        }
        if (dump) {
            StringBuilder b = new StringBuilder();
            Json.write(b, all, 0);
            System.out.println(b);
        }
        System.out.println("conformance: " + (total - failed) + " of " + total + " cases passed");
        System.exit(failed == 0 ? 0 : 1);
    }

    // The suite

    static File suite() {
        String property = System.getProperty("mncf.tests");
        if (property != null && !property.isEmpty()) {
            return new File(property);
        }
        File dir = new File(System.getProperty("user.dir")).getAbsoluteFile();
        for (File d = dir; d != null; d = d.getParentFile()) {
            for (String candidate : new String[] {"format/tests", "tests"}) {
                File f = new File(d, candidate);
                if (new File(f, "cases").isDirectory() && new File(f, "packs").isDirectory()) {
                    return f;
                }
            }
        }
        throw new IllegalStateException("cannot find format/tests; set the system property mncf.tests");
    }

    static List<Case> cases() throws Exception {
        List<Case> out = new ArrayList<>();
        File suite = suite();
        for (String kind : KINDS) {
            File folder = new File(suite, kind);
            File[] files = folder.listFiles();
            List<String> names = new ArrayList<>();
            for (File f : files == null ? new File[0] : files) {
                if (f.getName().endsWith(".toml")) {
                    names.add(f.getName());
                }
            }
            Collections.sort(names);
            for (String n : names) {
                String text = new String(Files.readAllBytes(new File(folder, n).toPath()), StandardCharsets.UTF_8);
                String stem = n.substring(0, n.length() - ".toml".length());
                out.add(new Case(kind + "/" + stem, Toml.parse(text), folder, kind));
            }
        }
        return out;
    }

    /** The differences between a case's expected results and what this library gives, as lines. */
    static List<String> check(Case c, Map<String, Object> got) throws IOException {
        if (c.kind.equals("passes")) {
            return comparePasses(c.table, got);
        }
        return compare(map(c.table.get("expect")), got, c.folder);
    }

    private static Map<String, Object> results(Case c) throws IOException {
        if (c.kind.equals("passes")) {
            return passResults(c.table, c.folder);
        }
        if (c.kind.equals("packs")) {
            return packResults(c.table, c.folder);
        }
        if (c.kind.equals("convert")) {
            return convertResults(c.table, c.folder);
        }
        return fileResults(c.table, c.folder);
    }

    /** The code module a conversion case describes: zero bytes with the given pieces, its sections and runs. */
    static CodeModule caseModule(Map<String, Object> m, String buildId) {
        byte[] image = new byte[((Long) m.get("size")).intValue()];
        for (Object piece : list(m.get("bytes"))) {
            Map<String, Object> p = map(piece);
            byte[] raw = hex((String) p.get("hex"));
            System.arraycopy(raw, 0, image, ((Long) p.get("at")).intValue(), raw.length);
        }
        Map<String, long[]> sections = new LinkedHashMap<>();
        for (String k : new String[] {"code", "rodata", "data"}) {
            if (m.containsKey(k)) {
                List<Object> range = list(m.get(k));
                sections.put(k, new long[] {(Long) range.get(0), (Long) range.get(1)});
            }
        }
        List<CodeModule.Run> exclude = new ArrayList<>();
        for (Object r : list(m.get("exclude"))) {
            List<Object> run = list(r);
            exclude.add(new CodeModule.Run((Long) run.get(0), (Long) run.get(1)));
        }
        return new CodeModule(image, sections, buildId, (Long) m.get("text_size"), exclude);
    }

    /**
     * What the converter writes back to Atmosphère for one build: the file's name and text,
     * how each cheat was written, and the reason each other cheat was left out.
     */
    static Map<String, Object> backResults(Map<String, Object> c, File folder) throws IOException {
        String text = c.containsKey("text") ? (String) c.get("text")
                : readText(new File(folder, (String) c.get("mncf")));
        Map<String, Object> out = new LinkedHashMap<>();
        MncfFile f;
        try {
            f = Mncf.load(text.getBytes(StandardCharsets.UTF_8));
        } catch (MncfException e) {
            out.put("error", e.code());
            return out;
        }
        CodeModule module = c.containsKey("module")
                ? caseModule(map(c.get("module")), (String) c.get("module_build_id")) : null;
        Choices choices = null;
        if (c.containsKey("values")) {
            Choices.Builder b = Choices.builder();
            for (Map.Entry<String, Object> e : map(c.get("values")).entrySet()) {
                b.value(e.getKey(), e.getValue());
            }
            choices = b.build();
        }
        try {
            Map<String, String> preferCheats = null;
            if (c.containsKey("prefer_cheats")) {
                preferCheats = new LinkedHashMap<>();
                for (Map.Entry<String, Object> e : map(c.get("prefer_cheats")).entrySet()) {
                    preferCheats.put(e.getKey(), (String) e.getValue());
                }
            }
            Converter.Export export = Converter.toAtmosphere(f, (String) c.get("build"), module,
                    c.containsKey("prefer") ? (String) c.get("prefer") : "raw", choices, preferCheats);
            out.put("name", export.fileName());
            out.put("text", export.text());
            out.put("written", new LinkedHashMap<Object, Object>(export.written()));
            out.put("reasons", new LinkedHashMap<Object, Object>(export.reasons()));
        } catch (IllegalArgumentException e) {
            out.put("error", "error");
        }
        return out;
    }

    /** The text the converter writes for a conversion case, or the code of the error it gives. */
    static Map<String, Object> convertResults(Map<String, Object> c, File folder) throws IOException {
        if ("atmosphere".equals(c.get("to"))) {
            return backResults(c, folder);
        }
        List<Source> sources = new ArrayList<>();
        for (Object item : list(c.get("source"))) {
            Map<String, Object> src = map(item);
            CodeModule module = src.containsKey("module")
                    ? caseModule(map(src.get("module")), (String) src.get("build_id")) : null;
            sources.add(Source.builder((String) src.get("build_id"), (String) src.get("text"))
                    .version((String) src.get("version")).module(module).buildKey((String) src.get("build_key")).build());
        }
        Converter.Options options = Converter.Options.forGame((String) c.get("game"))
                .platform(c.containsKey("platform") ? (String) c.get("platform") : "switch")
                .gameName((String) c.get("game_name")).name((String) c.get("name")).origin((String) c.get("origin"))
                .authors(c.containsKey("authors") ? strings(c.get("authors")) : null)
                .values(!Boolean.FALSE.equals(c.get("values")));
        Map<String, Object> out = new LinkedHashMap<>();
        try {
            out.put("text", Converter.fromAtmosphere(sources, options).text());
        } catch (ConvertException e) {
            out.put("error", e.code());
        }
        return out;
    }

    @SuppressWarnings("unchecked")
    static Map<String, Object> map(Object v) {
        return v == null ? new LinkedHashMap<String, Object>() : (Map<String, Object>) v;
    }

    @SuppressWarnings("unchecked")
    static List<Object> list(Object v) {
        return v == null ? new ArrayList<>() : (List<Object>) v;
    }

    static List<String> strings(Object v) {
        List<String> out = new ArrayList<>();
        for (Object x : list(v)) {
            out.add((String) x);
        }
        return out;
    }

    /** Bytes written as hexadecimal pairs; white space between them is skipped. */
    static byte[] hex(String text) {
        String s = text.replaceAll("\\s", "");
        byte[] out = new byte[s.length() / 2];
        for (int i = 0; i < out.length; i++) {
            out[i] = (byte) Integer.parseInt(s.substring(2 * i, 2 * i + 2), 16);
        }
        return out;
    }

    static byte[] caseBytes(Map<String, Object> c, File folder) throws IOException {
        if (c.containsKey("file")) {
            return Files.readAllBytes(new File(folder, (String) c.get("file")).toPath());
        }
        if (c.containsKey("hex")) {
            return hex((String) c.get("hex"));
        }
        return ((String) c.get("text")).getBytes(StandardCharsets.UTF_8);
    }

    private static TrustList trustList(Map<String, Object> c) {
        TrustList trust = new TrustList();
        Map<String, Object> t = map(c.get("trust"));
        for (Object key : list(t.get("key"))) {
            Map<String, Object> k = map(key);
            trust.trust((String) k.get("name"), Base64.getDecoder().decode((String) k.get("public")));
        }
        for (Object kid : list(t.get("revoked"))) {
            trust.revoke((String) kid);
        }
        return trust;
    }

    /** The guest memory of a case: the modules' images, the regions, and pieces of their own. */
    static final class Memory implements GuestMemory {
        final List<Chunk> chunks = new ArrayList<>();

        void add(long start, byte[] data) {
            chunks.add(new Chunk(start, data.clone()));
        }

        /** Writes bytes into the piece they lie in, or makes a piece of their own. */
        void put(long at, byte[] raw) {
            for (Chunk c : chunks) {
                if (c.holds(at, raw.length)) {
                    System.arraycopy(raw, 0, c.buf, (int) (at - c.start), raw.length);
                    return;
                }
            }
            chunks.add(new Chunk(at, raw.clone()));
        }

        @Override
        public byte[] read(long address, int size) {
            for (Chunk c : chunks) {
                if (c.holds(address, size)) {
                    int from = (int) (address - c.start);
                    return Arrays.copyOfRange(c.buf, from, from + size);
                }
            }
            return null;
        }
    }

    static void putPieces(Memory memory, Object pieces) {
        for (Object piece : list(pieces)) {
            Map<String, Object> p = map(piece);
            byte[] raw = p.containsKey("hex") ? hex((String) p.get("hex")) : new byte[((Long) p.get("size")).intValue()];
            memory.put((Long) p.get("at"), raw);
        }
    }

    /** A piece of guest memory: where it starts and its bytes. */
    private static final class Chunk {
        final long start;
        final byte[] buf;

        Chunk(long start, byte[] buf) {
            this.start = start;
            this.buf = buf;
        }

        boolean holds(long address, long size) {
            return Long.compareUnsigned(start, address) <= 0 && Long.compareUnsigned(address - start, buf.length) <= 0
                    && buf.length - (address - start) >= size;
        }
    }

    static Runtime runtime(Map<String, Object> env) {
        return runtime(env, new Memory());
    }

    /** The runtime of a case's [env] table, with its guest memory in the memory given. */
    static Runtime runtime(Map<String, Object> env, Memory memory) {
        Runtime.Builder b = Runtime.builder(env.containsKey("platform") ? (String) env.get("platform") : "switch");
        for (Map.Entry<String, Object> e : map(env.get("modules")).entrySet()) {
            Map<String, Object> m = map(e.getValue());
            int size = ((Long) m.get("size")).intValue();
            byte[] data = new byte[size];
            for (Object piece : list(m.get("bytes"))) {
                Map<String, Object> p = map(piece);
                byte[] raw = hex((String) p.get("hex"));
                System.arraycopy(raw, 0, data, ((Long) p.get("at")).intValue(), raw.length);
            }
            long base = m.containsKey("base") ? (Long) m.get("base") : 0;
            Module.Builder mb = Module.builder((String) m.get("id")).base(base).image(data).size(size);
            for (String k : new String[] {"code", "rodata", "data"}) {
                if (m.containsKey(k)) {
                    List<Object> range = list(m.get(k));
                    mb.section(k, (Long) range.get(0), (Long) range.get(1));
                }
            }
            b.module(e.getKey(), mb.build());
            memory.add(base, data);
        }
        for (Map.Entry<String, Object> e : map(env.get("regions")).entrySet()) {
            Map<String, Object> r = map(e.getValue());
            long base = (Long) r.get("base");
            long size = (Long) r.get("size");
            b.region(e.getKey(), base, size);
            memory.add(base, new byte[(int) size]);
        }
        putPieces(memory, env.get("memory"));
        b.memory(memory);
        b.gameId((String) env.get("game"));
        b.arch((String) env.get("arch"));
        if (env.containsKey("capabilities")) {
            b.capabilities(strings(env.get("capabilities")));
        }
        if (env.containsKey("extensions")) {
            b.extensions(strings(env.get("extensions")));
        }
        if (env.containsKey("raw")) {
            b.rawSyntaxes(strings(env.get("raw")));
        }
        b.build((String) env.get("build"));
        b.held(strings(env.get("held")));
        b.unsupported(strings(env.get("unsupported")));
        for (Map.Entry<String, Object> e : map(env.get("rewritten")).entrySet()) {
            for (Object run : list(e.getValue())) {
                List<Object> r = list(run);
                b.rewritten(e.getKey(), (Long) r.get(0), (Long) r.get(1));
            }
        }
        return b.build();
    }

    static Map<String, Object> fileResults(Map<String, Object> c, File folder) throws IOException {
        byte[] data = caseBytes(c, folder);
        Map<String, Object> out = new LinkedHashMap<>();
        MncfFile f;
        try {
            f = Mncf.load(data, trustList(c));
        } catch (MncfException e) {
            out.put("error", e.code());
            return out;
        }
        Map<String, Object> env = map(c.get("env"));
        Choices.Builder choices = Choices.builder().on(strings(env.get("on"))).off(strings(env.get("off")));
        for (Map.Entry<String, Object> v : map(env.get("values")).entrySet()) {
            choices.value(v.getKey(), v.getValue());
        }
        Evaluation ev = Evaluator.evaluate(f, runtime(env), choices.build());
        out.put("signature", f.signature().result());
        out.put("signers", new ArrayList<Object>(f.signature().signers()));
        out.put("build", ev.build() == null ? "" : ev.build());
        List<String> warnings = new ArrayList<>(f.warnings());
        Collections.sort(warnings);
        out.put("warnings", new ArrayList<Object>(warnings));
        Map<String, Object> cheats = new LinkedHashMap<>();
        for (CheatResult r : ev.cheats()) {
            Map<String, Object> one = new LinkedHashMap<>();
            one.put("state", r.state());
            one.put("reason", r.reason());
            one.put("uses", r.uses());
            List<Object> ops = new ArrayList<>();
            for (ResolvedOp op : r.ops()) {
                ops.add(op.toMap());
            }
            one.put("ops", ops);
            cheats.put(r.id(), one);
        }
        out.put("cheat", cheats);
        return out;
    }

    /** The keys a step of a pass case may have, and the keys of its expect table. */
    static final List<String> STEP_KEYS = Arrays.asList("about", "memory", "values", "off", "on", "held", "press",
            "now", "pass", "stop", "expect");
    static final List<String> STEP_EXPECT_KEYS = Arrays.asList("changes", "shown", "cheat");

    private static Map<String, Object> states(List<CheatState> states) {
        Map<String, Object> out = new LinkedHashMap<>();
        for (CheatState c : states) {
            Map<String, Object> one = new LinkedHashMap<>();
            one.put("state", c.state());
            one.put("reason", c.reason());
            out.put(c.id(), one);
        }
        return out;
    }

    private static List<Object> changes(List<Change> changes) {
        List<Object> out = new ArrayList<>();
        for (Change ch : changes) {
            out.add(ch.toMap());
        }
        return out;
    }

    /**
     * What the engine does in a pass case: the states after loading, and for each step the
     * changes of its pass, every cheat's state after it, and what the monitors read. The changes
     * of each pass are made in the memory.
     */
    static Map<String, Object> passResults(Map<String, Object> c, File folder) throws IOException {
        Map<String, Object> out = new LinkedHashMap<>();
        MncfFile f;
        try {
            f = Mncf.load(caseBytes(c, folder), trustList(c));
        } catch (MncfException e) {
            out.put("error", e.code());
            return out;
        }
        Map<String, Object> env = map(c.get("env"));
        Memory memory = new Memory();
        Runtime rt = runtime(env, memory);
        Choices.Builder choices = Choices.builder().on(strings(env.get("on"))).off(strings(env.get("off")));
        for (Map.Entry<String, Object> v : map(env.get("values")).entrySet()) {
            choices.value(v.getKey(), v.getValue());
        }
        double rate = c.containsKey("rate") ? ((Number) c.get("rate")).doubleValue() : 60;
        Engine engine = new Engine(f, rt, choices.build(), rate);
        out.put("load", states(engine.states()));
        List<Object> steps = new ArrayList<>();
        for (Object item : list(c.get("step"))) {
            Map<String, Object> step = map(item);
            putPieces(memory, step.get("memory"));
            for (Map.Entry<String, Object> v : map(step.get("values")).entrySet()) {
                engine.setValue(v.getKey(), v.getValue());
            }
            for (String id : strings(step.get("off"))) {
                engine.switchCheat(id, false);
            }
            for (String id : strings(step.get("on"))) {
                engine.switchCheat(id, true);
            }
            if (step.containsKey("held")) {
                engine.hold(strings(step.get("held")));
            }
            for (String combo : strings(step.get("press"))) {
                engine.press(combo);
            }
            Map<String, Object> got = new LinkedHashMap<>();
            if (Boolean.TRUE.equals(step.get("stop"))) {
                got.put("changes", changes(engine.stop()));
                got.put("shown", new LinkedHashMap<String, Object>());
            } else if (!Boolean.FALSE.equals(step.get("pass"))) {
                Object now = step.get("now");
                Pass pass = engine.tick(memory, now == null ? null : ((Number) now).doubleValue());
                got.put("changes", changes(pass.changes()));
                Map<String, Object> shown = new LinkedHashMap<>();
                for (Map.Entry<String, List<Shown>> e : pass.shown().entrySet()) {
                    List<Object> values = new ArrayList<>();
                    for (Shown v : e.getValue()) {
                        values.add(v.toMap());
                    }
                    shown.put(e.getKey(), values);
                }
                got.put("shown", shown);
            }
            for (Object ch : list(got.get("changes"))) {
                Map<String, Object> m = map(ch);
                String kind = (String) m.get("kind");
                if (kind.equals("patch") || kind.equals("restore") || kind.equals("write")) {
                    memory.put(((Number) m.get("address")).longValue(), hex((String) m.get("bytes")));
                }
            }
            got.put("cheat", states(engine.states()));
            steps.add(got);
        }
        out.put("steps", steps);
        return out;
    }

    /** The differences between a pass case's expected results and what the engine did, as lines. */
    static List<String> comparePasses(Map<String, Object> c, Map<String, Object> got) {
        List<String> problems = new ArrayList<>();
        if (got.containsKey("error")) {
            problems.add("error: the file does not load (" + got.get("error") + ")");
            return problems;
        }
        compareStates(problems, "load: ", map(map(c.get("expect")).get("cheat")), map(got.get("load")));
        List<Object> steps = list(c.get("step"));
        List<Object> gotSteps = list(got.get("steps"));
        for (int n = 1; n <= steps.size() && n <= gotSteps.size(); n++) {
            Map<String, Object> step = map(steps.get(n - 1));
            Map<String, Object> g = map(gotSteps.get(n - 1));
            Map<String, Object> want = map(step.get("expect"));
            List<String> unknown = new ArrayList<>();
            for (String key : step.keySet()) {
                if (!STEP_KEYS.contains(key)) {
                    unknown.add(key);
                }
            }
            Collections.sort(unknown);
            List<String> unknownExpect = new ArrayList<>();
            for (String key : want.keySet()) {
                if (!STEP_EXPECT_KEYS.contains(key)) {
                    unknownExpect.add(key);
                }
            }
            Collections.sort(unknownExpect);
            unknown.addAll(unknownExpect);
            for (String key : unknown) {
                problems.add("step " + n + ": unknown key " + key);
            }
            if (g.containsKey("changes")) {
                compareItems(problems, "step " + n + ": changes", list(want.get("changes")), list(g.get("changes")));
                Map<String, Object> shownWant = map(want.get("shown"));
                Map<String, Object> shownGot = map(g.get("shown"));
                if (!new HashSet<>(shownWant.keySet()).equals(new HashSet<>(shownGot.keySet()))) {
                    List<String> a = new ArrayList<>(shownWant.keySet());
                    List<String> b = new ArrayList<>(shownGot.keySet());
                    Collections.sort(a);
                    Collections.sort(b);
                    problems.add("step " + n + ": shown: expected " + a + ", got " + b);
                }
                for (Map.Entry<String, Object> e : shownWant.entrySet()) {
                    if (shownGot.containsKey(e.getKey())) {
                        compareItems(problems, "step " + n + ": shown." + e.getKey(), list(e.getValue()),
                                list(shownGot.get(e.getKey())));
                    }
                }
            } else if (want.containsKey("changes") || want.containsKey("shown")) {
                problems.add("step " + n + ": expects changes, but has no pass");
            }
            compareStates(problems, "step " + n + ": ", map(want.get("cheat")), map(g.get("cheat")));
        }
        return problems;
    }

    private static void compareStates(List<String> problems, String where, Map<String, Object> want,
            Map<String, Object> have) {
        for (Map.Entry<String, Object> e : want.entrySet()) {
            String id = e.getKey();
            if (!have.containsKey(id)) {
                problems.add(where + id + ": no such cheat");
                continue;
            }
            Map<String, Object> w = map(e.getValue());
            Map<String, Object> h = map(have.get(id));
            for (String key : new String[] {"state", "reason"}) {
                if (w.containsKey(key) && !pyEquals(w.get(key), h.get(key))) {
                    problems.add(where + id + "." + key + ": expected " + Py.repr(w.get(key)) + ", got "
                            + Py.repr(h.get(key)));
                }
            }
        }
    }

    private static void compareItems(List<String> problems, String where, List<Object> want, List<Object> have) {
        if (want.size() != have.size()) {
            problems.add(where + ": expected " + want.size() + ", got " + have.size() + ": " + have);
            return;
        }
        for (int i = 0; i < want.size(); i++) {
            Map<String, Object> w = map(want.get(i));
            Map<String, Object> g = map(have.get(i));
            for (Map.Entry<String, Object> k : w.entrySet()) {
                if (!g.containsKey(k.getKey()) || !same(k.getValue(), g.get(k.getKey()), k.getKey())) {
                    problems.add(where + "[" + i + "]." + k.getKey() + ": expected " + Py.repr(k.getValue()) + ", got "
                            + Py.repr(g.get(k.getKey())));
                }
            }
        }
    }

    static Map<String, Object> packResults(Map<String, Object> c, File folder) throws IOException {
        Map<String, Object> z = map(c.get("zip"));
        byte[] data;
        if (z.containsKey("text")) {
            data = ((String) z.get("text")).getBytes(StandardCharsets.UTF_8);
        } else if (z.containsKey("hex")) {
            data = hex((String) z.get("hex"));
        } else {
            ByteArrayOutputStream buf = new ByteArrayOutputStream();
            try (ZipOutputStream archive = new ZipOutputStream(buf)) {
                for (Map.Entry<String, Object> e : z.entrySet()) {
                    archive.putNextEntry(new ZipEntry(e.getKey()));
                    archive.write(Files.readAllBytes(new File(folder, (String) e.getValue()).toPath()));
                    archive.closeEntry();
                }
            }
            data = buf.toByteArray();
        }
        Map<String, Object> out = new LinkedHashMap<>();
        Pack pack;
        try {
            pack = PackReader.read(data, trustList(c));
        } catch (MncfException e) {
            out.put("error", e.code());
            return out;
        }
        out.put("signature", pack.signature().result());
        out.put("signers", new ArrayList<Object>(pack.signature().signers()));
        Map<String, Object> files = new LinkedHashMap<>();
        for (PackFile f : pack.files()) {
            files.put(f.path(), f.status());
        }
        out.put("files", files);
        return out;
    }

    // Comparing, as the Python runner does

    /** Python's == for the values of a TOML document and of the results. */
    static boolean pyEquals(Object a, Object b) {
        if (a instanceof Map && b instanceof Map) {
            Map<?, ?> x = (Map<?, ?>) a;
            Map<?, ?> y = (Map<?, ?>) b;
            if (!x.keySet().equals(y.keySet())) {
                return false;
            }
            for (Object k : x.keySet()) {
                if (!pyEquals(x.get(k), y.get(k))) {
                    return false;
                }
            }
            return true;
        }
        if (a instanceof List && b instanceof List) {
            List<?> x = (List<?>) a;
            List<?> y = (List<?>) b;
            if (x.size() != y.size()) {
                return false;
            }
            for (int i = 0; i < x.size(); i++) {
                if (!pyEquals(x.get(i), y.get(i))) {
                    return false;
                }
            }
            return true;
        }
        if (isPyNumber(a) && isPyNumber(b)) {
            return Values.same(a, b);
        }
        return a == null ? b == null : a.equals(b);
    }

    private static boolean isPyNumber(Object v) {
        return v instanceof Long || v instanceof BigInteger || v instanceof Double || v instanceof Boolean;
    }

    static boolean same(Object expected, Object actual, String key) {
        if (("bytes".equals(key) || "expect".equals(key)) && expected instanceof String && actual instanceof String) {
            return ((String) expected).toUpperCase().equals(((String) actual).toUpperCase());
        }
        if (expected instanceof Map) {
            if (!(actual instanceof Map)) {
                return false;
            }
            Map<?, ?> e = (Map<?, ?>) expected;
            Map<?, ?> a = (Map<?, ?>) actual;
            if (!e.keySet().equals(a.keySet())) {
                return false;
            }
            for (Map.Entry<?, ?> entry : e.entrySet()) {
                if (!same(entry.getValue(), a.get(entry.getKey()), (String) entry.getKey())) {
                    return false;
                }
            }
            return true;
        }
        if (expected instanceof List) {
            if (!(actual instanceof List)) {
                return false;
            }
            List<?> e = (List<?>) expected;
            List<?> a = (List<?>) actual;
            if (e.size() != a.size()) {
                return false;
            }
            for (int i = 0; i < e.size(); i++) {
                if (!same(e.get(i), a.get(i), null)) {
                    return false;
                }
            }
            return true;
        }
        if (expected instanceof Boolean || actual instanceof Boolean) {
            return expected instanceof Boolean && actual instanceof Boolean && expected.equals(actual);
        }
        return pyEquals(expected, actual);
    }

    /** A text file as Python's read_text reads it: UTF-8, with each line end turned into a line feed. */
    static String readText(File file) throws IOException {
        String text = new String(Files.readAllBytes(file.toPath()), StandardCharsets.UTF_8);
        return text.replace("\r\n", "\n").replace('\r', '\n');
    }

    static List<String> compare(Map<String, Object> expect, Map<String, Object> got, File folder) throws IOException {
        List<String> problems = new ArrayList<>();
        if (expect.containsKey("error") || got.containsKey("error")) {
            if (!pyEquals(expect.get("error"), got.get("error"))) {
                problems.add("error: expected " + Py.repr(expect.get("error")) + ", got " + Py.repr(got.get("error")));
            }
            return problems;
        }
        Object[][] defaults = {{"signature", "unsigned"}, {"signers", new ArrayList<>()}, {"build", ""},
            {"warnings", new ArrayList<>()}, {"name", null}, {"written", new LinkedHashMap<>()},
            {"reasons", new LinkedHashMap<>()}};
        for (Object[] d : defaults) {
            String key = (String) d[0];
            if (!got.containsKey(key)) {
                continue;
            }
            Object want = expect.containsKey(key) ? expect.get(key) : d[1];
            if (key.equals("warnings")) {
                List<String> sorted = strings(want);
                Collections.sort(sorted);
                want = new ArrayList<Object>(sorted);
            }
            if (!pyEquals(want, got.get(key))) {
                problems.add(key + ": expected " + Py.repr(want) + ", got " + Py.repr(got.get(key)));
            }
        }
        if (got.containsKey("text")) {
            String want = folder != null && expect.containsKey("file")
                    ? readText(new File(folder, (String) expect.get("file"))) : null;
            String text = (String) got.get("text");
            if (!text.equals(want)) {
                List<String> a = want == null ? new ArrayList<String>() : Arrays.asList(want.split("\n", -1));
                List<String> b = Arrays.asList(text.split("\n", -1));
                int n = 0;
                while (n < a.size() && n < b.size() && a.get(n).equals(b.get(n))) {
                    n++;
                }
                problems.add("text differs at line " + (n + 1) + ": expected " + Py.repr(n < a.size() ? a.get(n) : null)
                        + ", got " + Py.repr(n < b.size() ? b.get(n) : null));
            }
            return problems;
        }
        if (got.containsKey("files")) {
            Object want = expect.containsKey("files") ? expect.get("files") : new LinkedHashMap<>();
            if (!pyEquals(want, got.get("files"))) {
                problems.add("files: expected " + Py.repr(expect.get("files")) + ", got " + Py.repr(got.get("files")));
            }
        }
        if (got.containsKey("cheat")) {
            Map<String, Object> want = map(expect.get("cheat"));
            Map<String, Object> have = map(got.get("cheat"));
            if (!new HashSet<>(want.keySet()).equals(new HashSet<>(have.keySet()))) {
                List<String> a = new ArrayList<>(want.keySet());
                List<String> b = new ArrayList<>(have.keySet());
                Collections.sort(a);
                Collections.sort(b);
                problems.add("cheats: expected " + a + ", got " + b);
            }
            for (Map.Entry<String, Object> entry : want.entrySet()) {
                String cid = entry.getKey();
                Map<String, Object> w = map(entry.getValue());
                Map<String, Object> g = map(have.get(cid));
                if (!have.containsKey(cid)) {
                    continue;
                }
                for (String key : new String[] {"state", "reason", "uses"}) {
                    if (w.containsKey(key) && !pyEquals(w.get(key), g.get(key))) {
                        problems.add(cid + "." + key + ": expected " + Py.repr(w.get(key)) + ", got " + Py.repr(g.get(key)));
                    }
                }
                if (w.containsKey("ops")) {
                    List<Object> wo = list(w.get("ops"));
                    List<Object> go = list(g.get("ops"));
                    if (wo.size() != go.size()) {
                        problems.add(cid + ".ops: expected " + wo.size() + " operations, got " + go.size());
                        continue;
                    }
                    for (int i = 0; i < wo.size(); i++) {
                        Map<String, Object> one = map(wo.get(i));
                        Map<String, Object> theirs = map(go.get(i));
                        for (Map.Entry<String, Object> k : one.entrySet()) {
                            if (!theirs.containsKey(k.getKey()) || !same(k.getValue(), theirs.get(k.getKey()), k.getKey())) {
                                problems.add(cid + ".ops[" + i + "]." + k.getKey() + ": expected " + Py.repr(k.getValue())
                                        + ", got " + Py.repr(theirs.get(k.getKey())));
                            }
                        }
                    }
                }
            }
        }
        return problems;
    }

    /** JSON in the layout of Python's json.dumps with indent=1, sort_keys=True and ASCII only. */
    static final class Json {
        private Json() {
        }

        static void write(StringBuilder b, Object v, int level) {
            if (v instanceof Map) {
                Map<String, Object> sorted = new TreeMap<>(map(v));
                if (sorted.isEmpty()) {
                    b.append("{}");
                    return;
                }
                b.append('{');
                boolean first = true;
                for (Map.Entry<String, Object> e : sorted.entrySet()) {
                    b.append(first ? "" : ",").append('\n');
                    indent(b, level + 1);
                    string(b, e.getKey());
                    b.append(": ");
                    write(b, e.getValue(), level + 1);
                    first = false;
                }
                b.append('\n');
                indent(b, level);
                b.append('}');
            } else if (v instanceof List) {
                List<Object> items = list(v);
                if (items.isEmpty()) {
                    b.append("[]");
                    return;
                }
                b.append('[');
                boolean first = true;
                for (Object item : items) {
                    b.append(first ? "" : ",").append('\n');
                    indent(b, level + 1);
                    write(b, item, level + 1);
                    first = false;
                }
                b.append('\n');
                indent(b, level);
                b.append(']');
            } else if (v instanceof String) {
                string(b, (String) v);
            } else if (v instanceof Boolean) {
                b.append(((Boolean) v) ? "true" : "false");
            } else if (v instanceof Double) {
                double d = (Double) v;
                b.append(Double.isNaN(d) ? "NaN" : Double.isInfinite(d) ? (d > 0 ? "Infinity" : "-Infinity")
                        : Py.floatRepr(d));
            } else if (v == null) {
                b.append("null");
            } else {
                b.append(v);
            }
        }

        private static void indent(StringBuilder b, int level) {
            for (int i = 0; i < level; i++) {
                b.append(' ');
            }
        }

        private static void string(StringBuilder b, String s) {
            b.append('"');
            for (int i = 0; i < s.length(); i++) {
                char c = s.charAt(i);
                switch (c) {
                    case '"':
                        b.append("\\\"");
                        break;
                    case '\\':
                        b.append("\\\\");
                        break;
                    case '\n':
                        b.append("\\n");
                        break;
                    case '\r':
                        b.append("\\r");
                        break;
                    case '\t':
                        b.append("\\t");
                        break;
                    case '\b':
                        b.append("\\b");
                        break;
                    case '\f':
                        b.append("\\f");
                        break;
                    default:
                        if (c < ' ' || c > '~') {
                            b.append(String.format("\\u%04x", (int) c));
                        } else {
                            b.append(c);
                        }
                }
            }
            b.append('"');
        }
    }
}
