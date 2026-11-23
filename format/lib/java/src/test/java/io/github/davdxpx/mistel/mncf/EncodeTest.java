package io.github.davdxpx.mistel.mncf;

import io.github.davdxpx.mistel.mncf.testing.Check;
import java.math.BigInteger;
import java.util.Arrays;

/** Tests of the machine code the reader makes: override stubs and arm64-imm/1 instructions. */
public final class EncodeTest {

    private EncodeTest() {
    }

    private static void bytes(String expectedHex, byte[] actual) {
        Check.equal(Values.hexBytes(Fixtures.hex(expectedHex)), actual == null ? null : Values.hexBytes(actual));
    }

    public static void testArm64Stubs() {
        bytes("c0035fd6", Encode.arm64Stub("skip", 0));
        bytes("000080d2c0035fd6", Encode.arm64Stub("return", 0));
        bytes("00008092c0035fd6", Encode.arm64Stub("return", -1));
        Check.equal(20, Encode.arm64Stub("return", 0x123456789ABCDEF0L).length);
        // movz x0, #0x5678 then movk x0, #0x1234, lsl #16 and ret.
        bytes("00cf8ad28046a2f2c0035fd6", Encode.arm64Stub("return", 0x12345678L));
    }

    public static void testX86Stubs() {
        bytes("c3", Encode.x86Stub("skip", 0));
        bytes("b8ffffffffc3", Encode.x86Stub("return", 0xFFFFFFFFL));
        bytes("48c7c000000080c3", Encode.x86Stub("return", Integer.MIN_VALUE));
        Check.equal("48 B8", Values.hexBytes(Arrays.copyOf(Encode.x86Stub("return", Integer.MIN_VALUE - 1L), 2)));
        Check.isTrue(Encode.stub("mips", "skip", 0) == null, "no stub for mips");
        Check.isTrue(!Encode.hasStub("mips") && Encode.hasStub("arm64") && Encode.hasStub("x86-64"), "stub archs");
    }

    public static void testFmov() {
        Check.equal(0x70, Encode.fmovImm8(1.0));
        Check.equal(0x00, Encode.fmovImm8(2.0));
        Check.equal(0x00, Encode.fmovImm8(2L));
        Check.equal(0x3F, Encode.fmovImm8(31.0));
        Check.equal(0x40, Encode.fmovImm8(0.125));
        Check.equal(0xF0, Encode.fmovImm8(-1.0));
        Check.equal(0x60, Encode.fmovImm8(0.5));
        for (Object v : new Object[] {0.0, -0.0, 0.1, 32.0, 1.0 / 16, Double.NaN, Double.POSITIVE_INFINITY, true,
            Double.MIN_VALUE, new BigInteger("18446744073709551615")}) {
            Check.equal(-1, Encode.fmovImm8(v));
        }
    }

    public static void testMovzRange() {
        Check.isTrue(Encode.arm64Imm("movz", 32, 0, 0x10000L) == null, "movz holds 16 bits");
        Check.isTrue(Encode.arm64Imm("movz", 32, 0, -1L) == null, "movz holds no negative value");
        Check.isTrue(Encode.arm64Imm("movz", 32, 0, 1.0) == null, "movz holds no float");
        Check.isTrue(Encode.arm64Imm("movz", 32, 0, true) == null, "movz holds no bool");
        Check.isTrue(Encode.arm64Imm("movz-movk", 32, 0, 0x100000000L) == null, "movz-movk holds 32 bits");
        Check.isTrue(Encode.arm64Imm("add", 32, 0, 1L) == null, "no other instruction");
        bytes("68 0C 80 52", Encode.arm64Imm("movz", 32, 8, 99L));
        bytes("68 0C 80 D2", Encode.arm64Imm("movz", 64, 8, 99L));
        bytes("08 CF 8A 52 88 46 A2 72", Encode.arm64Imm("movz-movk", 32, 8, 0x12345678L));
        bytes("01 10 7E 1E", Encode.arm64Imm("fmov", 64, 1, -1.0));
    }
}
