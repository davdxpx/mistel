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

    /** The kinds of cases, by folder: files the reader reads, packs, and conversions. */
    private static final String[] KINDS = {"cases", "packs", "convert"};

    public static void testSuite() throws Exception {
        List<String> problems = new ArrayList<>();
        int total = 0;
        int failed = 0;
        for (Case c : cases()) {
            total++;
            List<String> found = compare(map(c.table.get("expect")), results(c), c.folder);
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
            List<String> found = compare(map(c.table.get("expect")), got, c.folder);
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

    private static Map<String, Object> results(Case c) throws IOException {
        if (c.kind.equals("packs")) {
            return packResults(c.table, c.folder);
        }
        if (c.kind.equals("convert")) {
            return convertResults(c.table);
        }
        return fileResults(c.table, c.folder);
    }

    /** The text the converter writes for a conversion case, or the code of the error it gives. */
    static Map<String, Object> convertResults(Map<String, Object> c) {
        List<Source> sources = new ArrayList<>();
        for (Object item : list(c.get("source"))) {
            Map<String, Object> src = map(item);
            CodeModule module = null;
            Map<String, Object> m = src.containsKey("module") ? map(src.get("module")) : null;
            if (m != null) {
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
                module = new CodeModule(image, sections, (String) src.get("build_id"), (Long) m.get("text_size"), exclude);
            }
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
        final List<Chunk> chunks = new ArrayList<>();
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
            chunks.add(new Chunk(base, data.clone()));
        }
        for (Map.Entry<String, Object> e : map(env.get("regions")).entrySet()) {
            Map<String, Object> r = map(e.getValue());
            long base = (Long) r.get("base");
            long size = (Long) r.get("size");
            b.region(e.getKey(), base, size);
            chunks.add(new Chunk(base, new byte[(int) size]));
        }
        for (Object piece : list(env.get("memory"))) {
            Map<String, Object> p = map(piece);
            byte[] raw = p.containsKey("hex") ? hex((String) p.get("hex")) : new byte[((Long) p.get("size")).intValue()];
            long at = (Long) p.get("at");
            boolean placed = false;
            for (Chunk c : chunks) {
                if (c.holds(at, raw.length)) {
                    System.arraycopy(raw, 0, c.buf, (int) (at - c.start), raw.length);
                    placed = true;
                    break;
                }
            }
            if (!placed) {
                chunks.add(new Chunk(at, raw));
            }
        }
        b.memory(new GuestMemory() {
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
        });
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

    static Map<String, Object> packResults(Map<String, Object> c, File folder) throws IOException {
        Map<String, Object> z = map(c.get("zip"));
        byte[] data;
        if (z.containsKey("text")) {
            data = ((String) z.get("text")).getBytes(StandardCharsets.UTF_8);
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
            {"warnings", new ArrayList<>()}};
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
