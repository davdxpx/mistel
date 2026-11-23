package io.github.davdxpx.mistel.mncf;

import io.github.davdxpx.mistel.mncf.testing.Check;
import java.math.BigInteger;
import java.util.Arrays;

/** Tests of Ed25519 verification against RFC 8032 and the rules the format adds. */
public final class Ed25519Test {

    private static final BigInteger L =
            BigInteger.ONE.shiftLeft(252).add(new BigInteger("27742317777372353535851937790883648493"));

    private Ed25519Test() {
    }

    private static byte[] littleEndian(BigInteger v) {
        byte[] big = v.toByteArray();
        byte[] out = new byte[32];
        for (int i = 0; i < big.length && i < 32; i++) {
            out[i] = big[big.length - 1 - i];
        }
        return out;
    }

    public static void testRfc8032Vectors() {
        for (String[] v : Fixtures.RFC8032) {
            byte[] pub = Fixtures.hex(v[1]);
            byte[] msg = Fixtures.hex(v[2]);
            byte[] sig = Fixtures.hex(v[3]);
            Check.isTrue(Ed25519.verify(pub, msg, sig), "vector with message " + v[2] + " verifies");
        }
    }

    public static void testRejectsChanges() {
        String[] v = Fixtures.RFC8032[1];
        byte[] pub = Fixtures.hex(v[1]);
        byte[] msg = Fixtures.hex(v[2]);
        byte[] sig = Fixtures.hex(v[3]);
        Check.isTrue(!Ed25519.verify(pub, Fixtures.concat(msg, new byte[1]), sig), "a longer message fails");
        byte[] lastBit = sig.clone();
        lastBit[63] ^= 1;
        Check.isTrue(!Ed25519.verify(pub, msg, lastBit), "a changed S fails");
        byte[] inR = sig.clone();
        inR[5] ^= 0x10;
        Check.isTrue(!Ed25519.verify(pub, msg, inR), "a changed R fails");
        Check.isTrue(!Ed25519.verify(Arrays.copyOf(pub, 31), msg, sig), "a short key fails");
        Check.isTrue(!Ed25519.verify(pub, msg, Arrays.copyOf(sig, 63)), "a short signature fails");
    }

    public static void testRejectsNonCanonicalS() {
        String[] v = Fixtures.RFC8032[0];
        byte[] pub = Fixtures.hex(v[1]);
        byte[] msg = Fixtures.hex(v[2]);
        byte[] sig = Fixtures.hex(v[3]);
        byte[] s = Arrays.copyOfRange(sig, 32, 64);
        byte[] reversed = new byte[32];
        for (int i = 0; i < 32; i++) {
            reversed[i] = s[31 - i];
        }
        BigInteger high = new BigInteger(1, reversed).add(L);
        byte[] bad = Fixtures.concat(Arrays.copyOf(sig, 32), littleEndian(high));
        Check.isTrue(!Ed25519.verify(pub, msg, bad), "S + L is refused although [S + L]B = [S]B");
    }

    public static void testRejectsYNotBelowP() {
        BigInteger p = BigInteger.ONE.shiftLeft(255).subtract(BigInteger.valueOf(19));
        String[] v = Fixtures.RFC8032[0];
        byte[] msg = Fixtures.hex(v[2]);
        byte[] sig = Fixtures.hex(v[3]);
        // y = p + 1 encodes the same field element as y = 1, a point on the curve.
        byte[] key = littleEndian(p.add(BigInteger.ONE));
        Check.isTrue(!Ed25519.verify(key, msg, sig), "a key with y >= p is refused");
        byte[] r = Fixtures.concat(littleEndian(p.add(BigInteger.ONE)), Arrays.copyOfRange(sig, 32, 64));
        Check.isTrue(!Ed25519.verify(Fixtures.hex(v[1]), msg, r), "an R with y >= p is refused");
    }

    public static void testRejectsPointsNotOnTheCurve() {
        String[] v = Fixtures.RFC8032[0];
        byte[] msg = Fixtures.hex(v[2]);
        byte[] sig = Fixtures.hex(v[3]);
        // y = 2 has no x on the curve: (y^2 - 1) / (d y^2 + 1) is not a square.
        byte[] key = littleEndian(BigInteger.valueOf(2));
        Check.isTrue(!Ed25519.verify(key, msg, sig), "a key that is not on the curve is refused");
        // x = 0 with the sign bit set is not a valid encoding.
        byte[] zero = littleEndian(BigInteger.ONE);
        zero[31] |= (byte) 0x80;
        Check.isTrue(!Ed25519.verify(zero, msg, sig), "x = 0 with a sign bit is refused");
    }
}
