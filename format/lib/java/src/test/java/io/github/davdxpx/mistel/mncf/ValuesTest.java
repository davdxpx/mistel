package io.github.davdxpx.mistel.mncf;

import io.github.davdxpx.mistel.mncf.testing.Check;
import java.math.BigInteger;
import java.util.Arrays;

/** Tests of the scalars of the format and of the Python text rules they depend on. */
public final class ValuesTest {

    private ValuesTest() {
    }

    public static void testPatterns() {
        Check.isTrue(Arrays.equals(new int[] {0x68, -1, 0, 0xB9}, Values.parsePattern("68 ?? 00 B9")), "a pattern");
        for (String bad : new String[] {"68 ?? 00", "?? 12 00 B9", "68 12 00 ??", "68 12 00 B9 ", "6812 00 B9", ""}) {
            Check.isTrue(Values.parsePattern(bad) == null, "not a pattern: \"" + bad + "\"");
        }
        Check.isTrue(Values.parsePattern(5L) == null, "a number is not a pattern");
    }

    public static void testBigIntegers() {
        Check.equal(new BigInteger("FFFFFFFF80000000", 16), Values.integer("0xFFFFFFFF80000000"));
        Check.equal(0x7FFFFFFFFFFFFFFFL, Values.integer("0x7FFFFFFFFFFFFFFF"));
        Check.equal(-5L, Values.integer(-5L));
        Check.isTrue(Values.integer("0x10000000000000000") == null, "17 digits are too many");
        Check.isTrue(Values.integer("12") == null, "a decimal string is not an integer");
        Check.isTrue(Values.integer(true) == null, "a bool is not an integer");
        Check.isTrue(Values.integer(1.0) == null, "a float is not an integer");
        Check.equal(0xFFFFFFFF80000000L, Values.integer("0xFFFFFFFF80000000").longValue());
    }

    public static void testNothingFollowsTheText() {
        Check.isTrue(Values.integer("0x10\n") == null, "no line feed after an integer");
        Check.isTrue(Values.parseBytes("01 02\n") == null, "no line feed after bytes");
        Check.isTrue(Values.parsePattern("68 12 00 ??\n") == null, "no line feed after a pattern");
        Check.isTrue(!Py.matches(Values.ID, "money\n"), "no line feed after an id");
        Check.isTrue(!Platform.forName("switch").isValidGameId("0100000000ABC000\n"), "no line feed after a game ID");
    }

    public static void testFits() {
        Check.isTrue(Values.fits("s8", -128L), "s8 holds -128");
        Check.isTrue(!Values.fits("s8", 128L), "s8 does not hold 128");
        Check.isTrue(!Values.fits("u8", true), "a bool is not a u8");
        Check.isTrue(!Values.fits("f32", 1e39), "1e39 is too big for f32");
        Check.isTrue(Values.fits("f32", Double.POSITIVE_INFINITY), "inf is a f32");
        Check.isTrue(Values.fits("f32", Double.NaN), "nan is a f32");
        Check.isTrue(Values.fits("f32", 3.4028235677973362e38), "rounds down to the largest float");
        Check.isTrue(!Values.fits("f32", 3.4028235677973366e38), "rounds up to infinity");
        Check.isTrue(Values.fits("f64", 1e300), "f64 holds 1e300");
        Check.isTrue(Values.fits("u64", new BigInteger("18446744073709551615")), "u64 holds 2^64 - 1");
        Check.isTrue(!Values.fits("u64", -1L), "u64 does not hold -1");
        Check.isTrue(!Values.fits("s64", new BigInteger("9223372036854775808")), "s64 does not hold 2^63");
    }

    public static void testEncode() {
        Check.isTrue(Arrays.equals(new byte[] {0, 0, 0, 1}, Values.encode("u32", 1L, true)), "big-endian u32");
        Check.isTrue(Arrays.equals(new byte[] {-1, -1}, Values.encode("s16", -1L, false)), "s16 -1");
        Check.isTrue(Arrays.equals(Fixtures.hex("0000c03f"), Values.encode("f32", 1.5, false)), "f32 1.5");
        Check.isTrue(Arrays.equals(Fixtures.hex("000000000000f0bf"), Values.encode("f64", -1L, false)), "f64 from an integer");
        Check.isTrue(Arrays.equals(Fixtures.hex("000000000000f043"),
                Values.encode("f64", new BigInteger("18446744073709551615"), false)), "2^64 - 1 rounds to 2^64");
        Check.isTrue(Arrays.equals(Fixtures.hex("ffffffffffffffff"),
                Values.encode("u64", new BigInteger("18446744073709551615"), false)), "u64 keeps all bits");
    }

    public static void testCompareIsExact() {
        Check.isTrue(Values.less(9007199254740992.0, 9007199254740993L), "a float below a larger integer");
        Check.isTrue(Values.same(2L, 2.0), "2 == 2.0");
        Check.isTrue(!Values.less(Double.NaN, 1L) && !Values.lessOrEqual(Double.NaN, 1L), "nan compares false");
        Check.isTrue(Values.less(new BigInteger("18446744073709551615"), Double.POSITIVE_INFINITY), "inf is above all");
        Check.isTrue(Values.same(-0.0, 0L), "-0.0 == 0");
    }

    public static void testPythonInt() {
        Check.equal(BigInteger.valueOf(31), Py.parseInt("0x_1f", 16));
        Check.equal(BigInteger.valueOf(-31), Py.parseInt("-0X1F", 16));
        Check.equal(BigInteger.valueOf(12), Py.parseInt(" 12 ", 10));
        Check.equal(BigInteger.valueOf(12), Py.parseInt("1_2", 10));
        Check.equal(BigInteger.valueOf(12), Py.parseInt("\u00A012", 10));
        Check.equal(BigInteger.valueOf(3), Py.parseInt("\u0663", 10));
        Check.equal(BigInteger.valueOf(1), Py.parseInt("0x1 ", 16));
        for (String bad : new String[] {"1_", "_1", "1__2", "0x", "0x__1", "\u001c12", "\uFF21", "", " ", "+"}) {
            Check.isTrue(Py.parseInt(bad, 16) == null, "int() refuses \"" + bad + "\"");
        }
        StringBuilder many = new StringBuilder();
        for (int i = 0; i < 4301; i++) {
            many.append('1');
        }
        Check.isTrue(Py.parseInt(many.toString(), 10) == null, "more than 4300 decimal digits are refused");
        Check.isTrue(Py.parseInt(many.toString(), 16) != null, "hexadecimal digits have no limit");
    }

    public static void testPythonRepr() {
        Check.equal("1e+16", Py.floatRepr(1e16));
        Check.equal("1000000000000000.0", Py.floatRepr(1e15));
        Check.equal("1e-05", Py.floatRepr(1e-5));
        Check.equal("0.0001", Py.floatRepr(1e-4));
        Check.equal("0.1", Py.floatRepr(0.1));
        Check.equal("1.2345678901234568e+17", Py.floatRepr(123456789012345680.0));
        Check.equal("5e-324", Py.floatRepr(Double.MIN_VALUE));
        Check.equal("-0.0", Py.floatRepr(-0.0));
        Check.equal("1.7976931348623157e+308", Py.floatRepr(Double.MAX_VALUE));
        Check.equal("'a'", Py.repr("a"));
        Check.equal("\"it's\"", Py.repr("it's"));
        Check.equal("['a', 1, True, 1.5]", Py.repr(Arrays.<Object>asList("a", 1L, true, 1.5)));
        Check.equal("'\\n\\x00\\u2028'", Py.repr("\n\u0000\u2028"));
    }

    public static void testPythonSpace() {
        Check.equal("a b", Py.strip("\u3000 a b\u0085"));
        Check.equal(Arrays.asList("04000000", "00100000"), Py.splitSpace(" 04000000\u00A000100000 "));
        Check.equal(Arrays.asList("a", "", "b", ""), Py.split("a//b/", '/'));
    }
}
