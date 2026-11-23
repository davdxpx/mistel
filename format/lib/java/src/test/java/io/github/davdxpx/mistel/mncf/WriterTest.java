package io.github.davdxpx.mistel.mncf;

import io.github.davdxpx.mistel.mncf.testing.Check;
import java.math.BigInteger;
import java.util.ArrayList;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;

/** Tests of the writer: the suite's files written and read again, and the layout. */
public final class WriterTest {

    private WriterTest() {
    }

    /** Whether two documents hold the same values of the same types, a NaN equal to a NaN of the same sign. */
    static boolean sameDoc(Object a, Object b) {
        if (a instanceof Double && b instanceof Double && ((Double) a).isNaN() && ((Double) b).isNaN()) {
            return Double.doubleToRawLongBits((Double) a) < 0 == Double.doubleToRawLongBits((Double) b) < 0;
        }
        if (a instanceof Map && b instanceof Map) {
            Map<?, ?> x = (Map<?, ?>) a;
            Map<?, ?> y = (Map<?, ?>) b;
            if (!x.keySet().equals(y.keySet())) {
                return false;
            }
            for (Object k : x.keySet()) {
                if (!sameDoc(x.get(k), y.get(k))) {
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
                if (!sameDoc(x.get(i), y.get(i))) {
                    return false;
                }
            }
            return true;
        }
        return a != null && b != null && a.getClass() == b.getClass() && a.equals(b);
    }

    public static void testRoundTripOfTheSuite() throws Exception {
        int count = 0;
        for (ConformanceTest.Case c : ConformanceTest.cases()) {
            if (!c.kind.equals("cases")) {
                continue;
            }
            MncfFile f;
            try {
                f = Mncf.load(ConformanceTest.caseBytes(c.table, c.folder));
            } catch (MncfException e) {
                continue;
            }
            String text = Writer.write(f);
            MncfFile again = Mncf.load(Fixtures.utf8(text));
            Check.isTrue(sameDoc(again.doc(), f.doc()), c.name + ": the document read again differs");
            Check.equal(text, Writer.dumps(again.doc()));
            count++;
        }
        Check.isTrue(count > 40, "only " + count + " files were written");
    }

    public static void testLayout() {
        Map<String, Object> at = new LinkedHashMap<>();
        at.put("offset", 0x1200L);
        at.put("region", "heap");
        Map<String, Object> write = new LinkedHashMap<>();
        write.put("value", 999999L);
        write.put("type", "u32");
        write.put("at", at);
        List<Object> writes = new ArrayList<>();
        writes.add(write);
        Map<String, Object> cheat = new LinkedHashMap<>();
        cheat.put("name", "Money");
        cheat.put("id", "money");
        cheat.put("write", writes);
        List<Object> cheats = new ArrayList<>();
        cheats.add(cheat);
        Map<String, Object> game = new LinkedHashMap<>();
        game.put("id", "0100000000ABC000");
        game.put("platform", "switch");
        Map<String, Object> doc = new LinkedHashMap<>();
        doc.put("cheat", cheats);
        doc.put("game", game);
        doc.put("name", "Example");
        doc.put("format", "mncf/1");
        Check.equal("format = \"mncf/1\"\n"
                + "name = \"Example\"\n"
                + "\n"
                + "[game]\n"
                + "platform = \"switch\"\n"
                + "id = \"0100000000ABC000\"\n"
                + "\n"
                + "[[cheat]]\n"
                + "id = \"money\"\n"
                + "name = \"Money\"\n"
                + "\n"
                + "  [[cheat.write]]\n"
                + "  at = { region = \"heap\", offset = 0x1200 }\n"
                + "  type = \"u32\"\n"
                + "  value = 0xF423F\n", Writer.dumps(doc));
        Check.equal("-5", Writer.scalar(-5L, "path", false));
        Check.equal("\"0xFFFFFFFF80000000\"", Writer.scalar(new BigInteger("FFFFFFFF80000000", 16), null, false));
        Check.equal("inf", Writer.scalar(Double.POSITIVE_INFINITY, null, false));
        Check.equal("\"\"\"\na\nb\\\n\"\"\"", Writer.multiline("a\nb"));
        Check.equal("\"a\\u0001\\\"\"", Writer.string("a\u0001\""));
        Check.equal("\"a b\"", Writer.key("a b"));
    }

    @SuppressWarnings("unchecked")
    public static void testDocumentsThatLoadButBreakRules() throws Exception {
        Map<String, Object> game = new LinkedHashMap<>();
        game.put("platform", "switch");
        game.put("id", "0100000000ABC000");
        Map<String, Object> a = new LinkedHashMap<>();
        a.put("id", "a");
        a.put("name", "a");
        a.put("values", 5L);
        Map<String, Object> n = new LinkedHashMap<>();
        n.put("n", 3L);
        Map<String, Object> b = new LinkedHashMap<>();
        b.put("id", "b");
        b.put("name", "b");
        b.put("values", n);
        List<Object> cheats = new ArrayList<>();
        cheats.add(a);
        cheats.add(b);
        Map<String, Object> z = new LinkedHashMap<>();
        z.put("name", "z");
        Map<String, Object> cheatNames = new LinkedHashMap<>();
        cheatNames.put("a", z);
        Map<String, Object> de = new LinkedHashMap<>();
        de.put("name", "y");
        de.put("cheat", cheatNames);
        Map<String, Object> i18n = new LinkedHashMap<>();
        i18n.put("de", de);
        i18n.put("note", "q");
        Map<String, Object> doc = new LinkedHashMap<>();
        doc.put("format", "mncf/1");
        doc.put("name", "x");
        doc.put("game", game);
        doc.put("cheat", cheats);
        doc.put("i18n", i18n);
        doc.put("extra\n", 1L);
        String text = Writer.dumps(doc);
        Check.isTrue(sameDoc(doc, io.github.davdxpx.mistel.mncf.toml.Toml.parse(text)), "read again:\n" + text);
        // The layout's own tables in shapes it does not write as sections become plain values.
        Map<String, Object> items = new LinkedHashMap<>();
        items.put("items", 3L);
        Object[][] odd = {{"preset", new ArrayList<Object>(java.util.Collections.singletonList(5L))}, {"i18n", 5L},
            {"preset", new ArrayList<Object>()}, {"choices", items}};
        for (Object[] o : odd) {
            Map<String, Object> doc2 = new LinkedHashMap<>(doc);
            doc2.put((String) o[0], o[1]);
            String text2 = Writer.dumps(doc2);
            Check.isTrue(sameDoc(doc2, io.github.davdxpx.mistel.mncf.toml.Toml.parse(text2)), "read again:\n" + text2);
        }
    }

    public static void testFloats() {
        // Floats are written as Python's repr() writes them: the values that occur, and some others.
        Object[][] cases = {{1.5, "1.5"}, {0.1, "0.1"}, {2.0, "2.0"}, {-1.0, "-1.0"}, {0.125, "0.125"}, {31.0, "31.0"},
            {1.0625, "1.0625"}, {-0.1875, "-0.1875"}, {1e16, "1e+16"}, {1e-5, "1e-05"}, {123456789012345680.0,
                "1.2345678901234568e+17"}, {Double.NaN, "nan"}, {-Double.NaN, "nan"},
            {Double.POSITIVE_INFINITY, "inf"}, {Double.NEGATIVE_INFINITY, "-inf"}, {-0.0, "-0.0"}};
        for (Object[] c : cases) {
            Check.equal(c[1], Writer.scalar(c[0], null, false));
        }
    }
}
