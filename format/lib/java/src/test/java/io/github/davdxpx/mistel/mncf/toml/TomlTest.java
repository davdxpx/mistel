package io.github.davdxpx.mistel.mncf.toml;

import io.github.davdxpx.mistel.mncf.testing.Check;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;

/** Tests of the TOML parser: one test for each feature and for each kind of error. */
public final class TomlTest {

    private TomlTest() {
    }

    // Features

    public static void testEmptyDocumentsAndComments() {
        Check.equal(0, parse("").size());
        Check.equal(0, parse(lines("", "  \t", "# a comment", "   # another one")).size());
        Check.equal(1L, parse("a = 1 # after a value").get("a"));
        Check.equal(1L, parse("a = 1").get("a"));
        Check.equal(0, parse("\uFEFF# a byte order mark at the start is skipped\n").size());
        Check.equal(0, parse("# a tab\tand non-ASCII text: caf\u00e9 \uD834\uDD1E").size());
    }

    public static void testBareQuotedAndDottedKeys() {
        Map<String, Object> doc = parse(lines(
                "bare_key-1 = 1",
                "\"quoted key\" = 2",
                "'literal \"key\"' = 3",
                "\"\" = 4",
                "1234 = 5",
                "site . \"made.up\".name = 6",
                "3.14 = 7",
                "\"\\u0061\\t\" = 8",
                "true = 9"));
        Check.equal(1L, doc.get("bare_key-1"));
        Check.equal(2L, doc.get("quoted key"));
        Check.equal(3L, doc.get("literal \"key\""));
        Check.equal(4L, doc.get(""));
        Check.equal(5L, doc.get("1234"));
        Check.equal(6L, table(table(doc.get("site")).get("made.up")).get("name"));
        Check.equal(7L, table(doc.get("3")).get("14"));
        Check.equal(8L, doc.get("a\t"));
        Check.equal(9L, doc.get("true"));
    }

    public static void testKeysKeepDocumentOrder() {
        Map<String, Object> doc = parse(lines(
                "zebra = 1", "apple = 2", "[middle.inner]", "[first]", "[middle]", "x = 3"));
        Check.isTrue(doc instanceof LinkedHashMap, "tables are LinkedHashMaps");
        Check.equal(Arrays.asList("zebra", "apple", "middle", "first"), new ArrayList<>(doc.keySet()));
        Check.equal(Arrays.asList("inner", "x"), new ArrayList<>(table(doc.get("middle")).keySet()));
    }

    public static void testTables() {
        Map<String, Object> doc = parse(lines(
                "[game]",
                "title = \"0100000000001000\"",
                "[game.build]",
                "version = \"1.0.0\"",
                "[ spaced . \"quoted part\" ]",
                "k = 1",
                "[x.y.z]",
                "[x]",
                "w = 2",
                "[empty]"));
        Check.equal("0100000000001000", table(doc.get("game")).get("title"));
        Check.equal("1.0.0", table(table(doc.get("game")).get("build")).get("version"));
        Check.equal(1L, table(table(doc.get("spaced")).get("quoted part")).get("k"));
        Check.equal(2L, table(doc.get("x")).get("w"));
        Check.equal(0, table(table(table(doc.get("x")).get("y")).get("z")).size());
        Check.equal(0, table(doc.get("empty")).size());
    }

    public static void testDottedKeys() {
        Map<String, Object> doc = parse(lines(
                "[fruit]",
                "apple.color = \"red\"",
                "apple.taste.sweet = true",
                "[fruit.apple.texture]",
                "smooth = true"));
        Map<String, Object> apple = table(table(doc.get("fruit")).get("apple"));
        Check.equal("red", apple.get("color"));
        Check.equal(Boolean.TRUE, table(apple.get("taste")).get("sweet"));
        Check.equal(Boolean.TRUE, table(apple.get("texture")).get("smooth"));

        Map<String, Object> named = parse(lines("[a.b.c]", "[a]", "b.d = 1"));
        Check.equal(1L, table(table(named.get("a")).get("b")).get("d"));
    }

    public static void testArraysOfTables() {
        Map<String, Object> doc = parse(lines(
                "[[cheat]]",
                "name = \"one\"",
                "[cheat.at]",
                "offset = 0x10",
                "[[cheat.write]]",
                "value = 1",
                "[[cheat.write]]",
                "value = 2",
                "[[cheat]]",
                "name = \"two\"",
                "[[cheat.write]]",
                "value = 3",
                "[[empty]]"));
        List<Object> cheats = list(doc.get("cheat"));
        Check.equal(2, cheats.size());
        Map<String, Object> first = table(cheats.get(0));
        Check.equal("one", first.get("name"));
        Check.equal(16L, table(first.get("at")).get("offset"));
        Check.equal(2L, table(list(first.get("write")).get(1)).get("value"));
        Map<String, Object> second = table(cheats.get(1));
        Check.equal("two", second.get("name"));
        Check.equal(1, list(second.get("write")).size());
        Check.equal(3L, table(list(second.get("write")).get(0)).get("value"));
        Check.equal(1, list(doc.get("empty")).size());
        Check.isTrue(doc.get("cheat") instanceof ArrayList, "arrays are ArrayLists");
    }

    public static void testInlineTables() {
        Map<String, Object> at = table(value("{ module = \"main\", offset = 0x10, ptr.base = 8, ptr.steps = [1, 2] }"));
        Check.equal(Arrays.asList("module", "offset", "ptr"), new ArrayList<>(at.keySet()));
        Check.equal("main", at.get("module"));
        Check.equal(8L, table(at.get("ptr")).get("base"));
        Check.equal(Arrays.asList(1L, 2L), table(at.get("ptr")).get("steps"));
        Check.equal(0, table(value("{}")).size());
        Check.equal(1L, table(table(table(value("{a={b={c=1}}}")).get("a")).get("b")).get("c"));
        List<Object> rows = list(value("[ { a = 1 }, { a = 2 } ]"));
        Check.equal(2L, table(rows.get(1)).get("a"));
        Check.isTrue(value("{}") instanceof LinkedHashMap, "inline tables are LinkedHashMaps");
    }

    public static void testBasicStrings() {
        Check.equal("plain text", value("\"plain text\""));
        Check.equal("", value("\"\""));
        Check.equal("\b\t\n\f\r\"\\", value("\"\\b\\t\\n\\f\\r\\\"\\\\\""));
        Check.equal("\u00e9\u00e9", value("\"\\u00E9\\u00e9\""));
        Check.equal("\uD834\uDD1E", value("\"\\U0001D11E\""));
        Check.equal("\0", value("\"\\u0000\""));
        Check.equal("tab\there", value("\"tab\there\""));
        Check.equal("caf\u00e9 \uD834\uDD1E", value("\"caf\u00e9 \uD834\uDD1E\""));
        Check.equal("# no comment", value("\"# no comment\""));
        Check.equal("'single' quotes", value("\"'single' quotes\""));
    }

    public static void testMultiLineBasicStrings() {
        Check.equal("line one\nline two", value("\"\"\"\nline one\nline two\"\"\""));
        Check.equal("kept\n", value("\"\"\"kept\n\"\"\""));
        Check.equal("crlf\nbecomes lf", value("\"\"\"\r\ncrlf\r\nbecomes lf\"\"\""));
        Check.equal("joined words", value("\"\"\"joined \\\n\n   \t words\"\"\""));
        Check.equal("spaces after the backslash", value("\"\"\"spaces \\  \t\r\n  after the backslash\"\"\""));
        Check.equal("\tA", value("\"\"\"\\t\\u0041\"\"\""));
        String quotes = "\"\"\"";
        Check.equal("one \"two\" \"\"three\"\"", value(quotes + "one \"two\" \"\"three\"\"" + quotes));
        Check.equal("", value(quotes + quotes));
        Check.equal("\"", value(quotes + "\"" + quotes));
        Check.equal("\"\"", value(quotes + "\"\"" + quotes));
    }

    public static void testLiteralStrings() {
        Check.equal("\\d+ and \\n stay as written", value("'\\d+ and \\n stay as written'"));
        Check.equal("quote \" inside", value("'quote \" inside'"));
        Check.equal("", value("''"));
        Check.equal("first line\n  'one' ''two''", value("'''\nfirst line\n  'one' ''two'''''"));
        Check.equal("a\\\nb", value("'''a\\\nb'''"));
        Check.equal("crlf\nbecomes lf", value("'''crlf\r\nbecomes lf'''"));
        Check.equal("\t\u00e9", value("'''\t\u00e9'''"));
    }

    public static void testIntegers() {
        Check.equal(0L, value("0"));
        Check.equal(0L, value("+0"));
        Check.equal(0L, value("-0"));
        Check.equal(99L, value("+99"));
        Check.equal(-17L, value("-17"));
        Check.equal(1000000L, value("1_000_000"));
        Check.equal(0xDEADBEEFL, value("0xDEADbeef"));
        Check.equal(0xFFL, value("0x00ff"));
        Check.equal(493L, value("0o755"));
        Check.equal(13L, value("0b1101"));
        Check.equal(Long.MAX_VALUE, value("9223372036854775807"));
        Check.equal(Long.MIN_VALUE, value("-9223372036854775808"));
        Check.equal(Long.MAX_VALUE, value("0x7FFF_FFFF_FFFF_FFFF"));
        Check.equal(Long.MAX_VALUE, value("0o777777777777777777777"));
        Check.equal(Long.MAX_VALUE, value("0b" + repeat('1', 63)));
    }

    public static void testFloats() {
        Check.equal(1.0, value("1.0"));
        Check.equal(3.1415, value("+3.1415"));
        Check.equal(-0.01, value("-0.01"));
        Check.equal(5e22, value("5e+22"));
        Check.equal(1e6, value("1e06"));
        Check.equal(-2e-2, value("-2E-2"));
        Check.equal(6.626e-34, value("6.626e-34"));
        Check.equal(224617.445991, value("224_617.445_991"));
        Check.equal(1e10, value("1e1_0"));
        Check.equal(0.0, value("0e0"));
        Check.equal(-0.0, value("-0.0"));
        Check.equal(0.0, value("+0.0"));
        Check.equal(Double.POSITIVE_INFINITY, value("inf"));
        Check.equal(Double.POSITIVE_INFINITY, value("+inf"));
        Check.equal(Double.NEGATIVE_INFINITY, value("-inf"));
        Check.equal(Double.NaN, value("nan"));
        Check.equal(Double.NaN, value("+nan"));
        Check.equal(Double.NaN, value("-nan"));
        Check.equal(0x7FF8000000000000L, Double.doubleToRawLongBits((Double) value("nan")));
        Check.equal(0xFFF8000000000000L, Double.doubleToRawLongBits((Double) value("-nan")));
    }

    public static void testBooleans() {
        Check.equal(Boolean.TRUE, value("true"));
        Check.equal(Boolean.FALSE, value("false"));
        Check.equal(Arrays.asList(true, false), value("[true,false]"));
    }

    public static void testDateTimes() {
        TomlDateTime utc = (TomlDateTime) value("1979-05-27T07:32:00Z");
        Check.equal(TomlDateTime.Kind.OFFSET_DATE_TIME, utc.getKind());
        Check.equal("1979-05-27T07:32:00Z", utc.getText());
        Check.equal("1979-05-27T07:32:00Z", utc.toString());
        Check.equal(1979, utc.getYear());
        Check.equal(5, utc.getMonth());
        Check.equal(27, utc.getDay());
        Check.equal(7, utc.getHour());
        Check.equal(32, utc.getMinute());
        Check.equal(0, utc.getSecond());
        Check.equal(0, utc.getNanosecond());
        Check.equal(0, utc.getOffsetMinutes());
        Check.equal(utc, value("1979-05-27t07:32:00z"));
        Check.equal(utc, value("1979-05-27 07:32:00+00:00"));

        TomlDateTime west = (TomlDateTime) value("1979-05-27T00:32:00.999999-07:00");
        Check.equal(-420, west.getOffsetMinutes());
        Check.equal(999999000, west.getNanosecond());
        Check.equal(330, ((TomlDateTime) value("1979-05-27 07:32:00+05:30")).getOffsetMinutes());

        TomlDateTime local = (TomlDateTime) value("1979-05-27T07:32:00");
        Check.equal(TomlDateTime.Kind.LOCAL_DATE_TIME, local.getKind());
        Check.isTrue(!local.equals(utc), "a local date-time differs from an offset one");

        TomlDateTime date = (TomlDateTime) value("1979-05-27");
        Check.equal(TomlDateTime.Kind.LOCAL_DATE, date.getKind());
        Check.equal(0, date.getHour());
        Check.equal(TomlDateTime.Kind.LOCAL_DATE, ((TomlDateTime) parse("d = 1979-05-27 # comment").get("d")).getKind());

        TomlDateTime time = (TomlDateTime) value("00:32:00.123456789123");
        Check.equal(TomlDateTime.Kind.LOCAL_TIME, time.getKind());
        Check.equal(123456789, time.getNanosecond());
        Check.equal(0, time.getYear());

        Check.equal(29, ((TomlDateTime) value("2024-02-29")).getDay());
        Check.equal(29, ((TomlDateTime) value("2000-02-29")).getDay());
        Check.equal(60, ((TomlDateTime) value("1990-12-31T23:59:60Z")).getSecond());
        List<Object> mixed = list(value("[1979-05-27, 07:32:00, 1979-05-27 07:32:00]"));
        Check.equal(TomlDateTime.Kind.LOCAL_TIME, ((TomlDateTime) mixed.get(1)).getKind());
        Check.equal(TomlDateTime.Kind.LOCAL_DATE_TIME, ((TomlDateTime) mixed.get(2)).getKind());
    }

    public static void testArrays() {
        Check.equal(new ArrayList<Object>(), value("[]"));
        Check.equal(new ArrayList<Object>(), value("[ ]"));
        Check.equal(Arrays.asList(1L, 2L, 3L), value("[1,2,3]"));
        Check.equal(Arrays.asList("a", 1L, 1.5, true), value("[\"a\", 1, 1.5, true]"));
        Check.equal(Arrays.asList(Arrays.asList(1L, 2L), Arrays.asList("x"), new ArrayList<Object>()),
                value("[[1, 2], ['x'], []]"));
        Map<String, Object> doc = parse(lines(
                "v = [  # opening",
                "  1,",
                "  # between values",
                "",
                "  2,  # after the last one",
                "]"));
        Check.equal(Arrays.asList(1L, 2L), doc.get("v"));
        Check.equal(Arrays.asList(1L, 2L), value("[\r\n1,\r\n2\r\n]"));
    }

    public static void testWhitespaceAndLineEnds() {
        Map<String, Object> doc = parse("a\t=\t1\r\n[t]\r\n  b = 2 # c\r\n\r\n\t[ u ]\n");
        Check.equal(1L, doc.get("a"));
        Check.equal(2L, table(doc.get("t")).get("b"));
        Check.equal(0, table(doc.get("u")).size());
    }

    public static void testNesting() {
        Check.equal(1, list(value(repeat('[', 100) + repeat(']', 100))).size());
        invalid("a = " + repeat('[', 200) + repeat(']', 200), "nest deeper than 128 levels");
        StringBuilder tables = new StringBuilder("a = ");
        for (int i = 0; i < 200; i++) {
            tables.append("{ b = ");
        }
        invalid(tables.toString(), "nest deeper than 128 levels");
    }

    // Errors

    public static void testErrorPosition() {
        TomlException e = invalid(lines("a = 1", "b = @"), "expected a value");
        Check.equal(2, e.getLine());
        Check.equal(5, e.getColumn());
        Check.equal("expected a value, found '@'", e.getReason());
        Check.equal("line 2, column 5: expected a value, found '@'", e.getMessage());
        TomlException wide = invalid("k = \"\uD834\uDD1E\" x", "expected the end of the line");
        Check.equal(1, wide.getLine());
        Check.equal(9, wide.getColumn());
        Check.equal(3, invalid(lines("[a]", "x = 1", "x = 2"), "duplicate key").getLine());
    }

    public static void testDuplicateKeys() {
        invalid(lines("a = 1", "a = 2"), "duplicate key a");
        invalid(lines("a = 1", "\"a\" = 2"), "duplicate key a");
        invalid(lines("a.b = 1", "a.b = 2"), "duplicate key a.b");
        invalid(lines("\"x y\".z = 1", "'x y'.z = 2"), "duplicate key \"x y\".z");
        invalid("t = { x = 1, x = 2 }", "duplicate key x");
        invalid(lines("a = 1", "a.b = 2"), "cannot add keys to a, it is a value");
        invalid(lines("[a.b]", "[a]", "b = 1"), "duplicate key b");
        invalid(lines("a = { b = 1 }", "a = 2"), "duplicate key a");
    }

    public static void testRedefinedTables() {
        invalid(lines("[a]", "[a]"), "cannot define table a, it is already a table defined by a header");
        invalid(lines("[a.b]", "[a]", "[a]"), "cannot define table a, it is already a table defined by a header");
        invalid(lines("[a]", "b = 1", "[a.b]"), "cannot define table a.b, it is already a value");
        invalid(lines("[a]", "b = 1", "[a.b.c]"), "cannot add a table to a.b, it is a value");
        invalid(lines("[fruit]", "apple.color = \"red\"", "[fruit.apple]"),
                "cannot define table fruit.apple, it is already a table defined by dotted keys");
        invalid(lines("[fruit]", "apple.taste.sweet = true", "[fruit.apple.taste]"),
                "it is already a table defined by dotted keys");
        invalid(lines("[a.b]", "x = 1", "[a]", "b.y = 2"), "cannot add keys to b, it is a table defined by a header");
        invalid(lines("[a.b.c]", "[a]", "b.d = 1", "[a.b]"), "it is already a table defined by dotted keys");
    }

    public static void testArrayOfTablesErrors() {
        invalid(lines("a = []", "[[a]]"), "cannot append a table to a, it is an array");
        invalid(lines("a = [{ b = 1 }]", "[[a]]"), "cannot append a table to a, it is an array");
        invalid(lines("a = [{ b = 1 }]", "[a.c]"), "cannot add a table to a, it is an array");
        invalid(lines("[[a]]", "[a]"), "cannot define table a, it is already an array of tables");
        invalid(lines("[a]", "[[a]]"), "cannot append a table to a, it is a table defined by a header");
        invalid(lines("[a.b]", "[[a]]"), "cannot append a table to a, it is a table");
        invalid(lines("[[a]]", "[a]", "b.c = 1"), "already an array of tables");
        invalid(lines("[[a.b]]", "[a]", "b.y = 2"), "cannot add keys to b, it is an array of tables");
    }

    public static void testInlineTablesAreClosed() {
        invalid(lines("a = {}", "[a]"), "cannot define table a, it is already an inline table");
        invalid(lines("a = { b = 1 }", "a.c = 2"), "cannot add keys to a, it is an inline table");
        invalid(lines("a = { b = 1 }", "[a.c]"), "cannot add a table to a, it is an inline table");
        invalid(lines("a = { b = {} }", "[a.b]"), "cannot add a table to a, it is an inline table");
        invalid(lines("a = { b = {} }", "[[a.b]]"), "cannot add a table to a, it is an inline table");
        invalid("a = { b = { c = 1 }, b.d = 2 }", "cannot add keys to b, it is an inline table");
        invalid("a = { b = 1, }", "expected a key, found '}'");
        invalid("a = { b = 1\n}", "expected ',' or '}', found the end of the line");
        invalid("a = {\nb = 1 }", "expected a key, found the end of the line");
        invalid("a = { b = 1 c = 2 }", "expected ',' or '}', found 'c'");
    }

    public static void testControlCharacters() {
        invalid("a = \"x\u0001y\"", "control character U+0001 is not allowed");
        invalid("a = 'x\u007Fy'", "control character U+007F is not allowed");
        invalid("a = \"\"\"x\0y\"\"\"", "control character U+0000 is not allowed");
        invalid("a = '''x\u001Fy'''", "control character U+001F is not allowed");
        invalid("a = \"x\u000by\"", "control character U+000B is not allowed");
        invalid("# comment \0 here", "control character U+0000 is not allowed");
        invalid("a = 1 # \u007F", "control character U+007F is not allowed");
        invalid("a = 1 # cr\r", "control character U+000D is not allowed");
        invalid("a = \0", "expected a value, found U+0000");
        Check.equal("a\tb", value("'a\tb'"));
    }

    public static void testSurrogates() {
        invalid("a = \"\\uD800\"", "escape \\uD800 is not a Unicode scalar value");
        invalid("a = \"\\uDFFF\"", "is not a Unicode scalar value");
        invalid("a = \"\\uD834\\uDD1E\"", "is not a Unicode scalar value");
        invalid("a = \"\\U0000D800\"", "is not a Unicode scalar value");
        invalid("a = \"\\U00110000\"", "is not a Unicode scalar value");
        invalid("a = \"\\UFFFFFFFF\"", "is not a Unicode scalar value");
        invalid("a = \"\uD800\"", "unpaired surrogate U+D800");
        invalid("a = '\uDC00'", "unpaired surrogate U+DC00");
        invalid("# \uD834", "unpaired surrogate U+D834");
        Check.equal("\uD7FF\uE000\uDBFF\uDFFF", value("\"\\uD7FF\\uE000\\U0010FFFF\""));
    }

    public static void testCarriageReturns() {
        invalid("a = 1\rb = 2", "a carriage return must be followed by a line feed");
        invalid("[a]\r", "a carriage return must be followed by a line feed");
        invalid("a = \"\"\"x\ry\"\"\"", "a carriage return must be followed by a line feed");
        invalid("a = '''x\ry'''", "a carriage return must be followed by a line feed");
        invalid("a = \"\"\"x\\\r\"\"\"", "a carriage return must be followed by a line feed");
        invalid("a = [1,\r2]", "a carriage return must be followed by a line feed");
        invalid("a = \"x\ry\"", "unterminated string");
    }

    public static void testInvalidEscapes() {
        invalid("a = \"\\x41\"", "invalid escape sequence \\x");
        invalid("a = \"\\e\"", "invalid escape sequence \\e");
        invalid("a = \"\\ \"", "invalid escape sequence");
        invalid("a = \"\\u12\"", "a \\u escape needs 4 hex digits");
        invalid("a = \"\\uGGGG\"", "a \\u escape needs 4 hex digits");
        invalid("a = \"\\U0001D11\"", "a \\U escape needs 8 hex digits");
        invalid("a = \"\"\"x\\ y\"\"\"", "invalid escape sequence");
        invalid("a = \"\\", "invalid escape sequence");
    }

    public static void testStringErrors() {
        invalid("a = \"open", "unterminated string");
        invalid(lines("a = 'open", "b = 1"), "unterminated string");
        invalid("a = \"\"\"open\"\"", "unterminated multi-line string");
        invalid("a = '''open", "unterminated multi-line string");
        invalid("\"\"\"key\"\"\" = 1", "a key cannot be a multi-line string");
        invalid("'''key''' = 1", "a key cannot be a multi-line string");
        invalid("a = \"\"\"x\"\"\"\"\"\"", "expected the end of the line, found '\"'");
        invalid("a = '''x''''''", "expected the end of the line, found '''");
        invalid("a = \"x\" \"y\"", "expected the end of the line, found '\"'");
    }

    public static void testIntegerErrors() {
        for (String token : new String[] {"01", "00", "+0x1", "-0x1", "1__0", "1_", "_1", "0X1F",
                "0x", "0x_1", "0b2", "0o8", "0B1", "0O7", "1_000_", "- 1", "++1"}) {
            invalid("a = " + token, "invalid value");
        }
        invalid("a = 9223372036854775808", "integer 9223372036854775808 does not fit in 64 bits");
        invalid("a = -9223372036854775809", "does not fit in 64 bits");
        invalid("a = 0x8000000000000000", "does not fit in 64 bits");
        invalid("a = 0o1000000000000000000000", "does not fit in 64 bits");
        invalid("a = 0b1" + repeat('0', 63), "does not fit in 64 bits");
    }

    public static void testFloatErrors() {
        for (String token : new String[] {"1.", ".5", "-.5", "1._5", "1.5_", "01.5", "1e", "1e_5",
                "1e5.5", "1.e5", "1__0.0", "Inf", "NaN", "infinity", "+-1.0", "1.5e+", "0x1.0"}) {
            invalid("a = " + token, "invalid value");
        }
    }

    public static void testDateTimeErrors() {
        invalid("a = 1979-13-27", "invalid month 13");
        invalid("a = 1979-00-27", "invalid month 0");
        invalid("a = 1979-05-32", "invalid day 32");
        invalid("a = 1979-05-00", "invalid day 0");
        invalid("a = 1979-04-31", "invalid day 31 in month 4");
        invalid("a = 2023-02-29", "invalid day 29 in month 2 of 2023");
        invalid("a = 1900-02-29", "invalid day 29 in month 2 of 1900");
        invalid("a = 1979-05-27T24:00:00", "invalid hour 24");
        invalid("a = 1979-05-27T23:60:00", "invalid minute 60");
        invalid("a = 07:32:61", "invalid second 61");
        invalid("a = 1979-05-27T07:32:00+24:00", "invalid offset +24:00");
        invalid("a = 1979-05-27T07:32:00-05:60", "invalid offset -05:60");
        invalid("a = 1979-05-27T07:32", "expected ':', found the end of the file");
        invalid("a = 07:32", "expected ':', found the end of the file");
        invalid("a = 1979-05-27T", "expected a digit, found the end of the file");
        invalid("a = 1979-05-27T07:32:00.", "expected a digit, found the end of the file");
        invalid("a = 1979-5-27", "expected a digit, found '-'");
        invalid("a = 1979-05-27T07:32:00+0700", "expected ':', found '0'");
        invalid("a = 07:32:00Z", "expected the end of the line, found 'Z'");
        invalid("a = 07:32:00+01:00", "expected the end of the line, found '+'");
        invalid("a = 1979-05-27X07:32:00", "expected the end of the line, found 'X'");
    }

    public static void testStructureErrors() {
        invalid("a", "expected '=', found the end of the file");
        invalid("a =", "expected a value, found the end of the file");
        invalid(lines("a =", "1"), "expected a value, found the end of the line");
        invalid("= 1", "expected a key, found '='");
        invalid("a = 1 b = 2", "expected the end of the line, found 'b'");
        invalid("a.= 1", "expected a key, found '='");
        invalid("a..b = 1", "expected a key, found '.'");
        invalid("[a", "expected ']', found the end of the file");
        invalid("[[a]", "expected ']]', found ']'");
        invalid("[[a] ]", "expected ']]', found ']'");
        invalid("[ [a]]", "expected a key, found '['");
        invalid("[]", "expected a key, found ']'");
        invalid("[a] b = 1", "expected the end of the line, found 'b'");
        invalid("a = [1 2]", "expected ',' or ']', found '2'");
        invalid("a = [1,,2]", "expected a value, found ','");
        invalid("a = [,]", "expected a value, found ','");
        invalid("a = [1", "unterminated array");
        invalid("a = [", "expected a value, found the end of the file");
        invalid("a = yes", "invalid value yes");
        invalid("a = trueish", "expected the end of the line, found 'i'");
        invalid("k\u00A0= 1", "expected '=', found U+00A0");
        invalid("a = 1\u3000", "expected the end of the line, found U+3000");
        invalid("caf\u00e9 = 1", "expected '=', found U+00E9");
        invalid("\uFEFF\uFEFFa = 1", "expected a key, found U+FEFF");
        invalid("a = \uFEFF1", "expected a value, found U+FEFF");
    }

    // Helpers

    private static String lines(String... lines) {
        StringBuilder out = new StringBuilder();
        for (String line : lines) {
            out.append(line).append('\n');
        }
        return out.toString();
    }

    private static String repeat(char c, int count) {
        StringBuilder out = new StringBuilder();
        for (int i = 0; i < count; i++) {
            out.append(c);
        }
        return out.toString();
    }

    private static Map<String, Object> parse(String text) {
        try {
            return Toml.parse(text);
        } catch (TomlException e) {
            throw new AssertionError("unexpected error: " + e.getMessage(), e);
        }
    }

    /** Parses one value, written as TOML. */
    private static Object value(String toml) {
        return parse("v = " + toml).get("v");
    }

    private static TomlException invalid(String text, String fragment) {
        return Check.throwsWith(TomlException.class, fragment, () -> Toml.parse(text));
    }

    @SuppressWarnings("unchecked")
    private static Map<String, Object> table(Object value) {
        return (Map<String, Object>) value;
    }

    @SuppressWarnings("unchecked")
    private static List<Object> list(Object value) {
        return (List<Object>) value;
    }
}
