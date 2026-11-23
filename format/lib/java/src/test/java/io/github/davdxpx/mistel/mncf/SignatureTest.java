package io.github.davdxpx.mistel.mncf;

import io.github.davdxpx.mistel.mncf.testing.Check;
import java.util.Arrays;
import java.util.Collections;

/** Tests of signature lines and trust (section 12). */
public final class SignatureTest {

    private SignatureTest() {
    }

    private static byte[] signedFile() {
        return Fixtures.concat(Fixtures.FILE, Fixtures.utf8(Fixtures.FILE_SIGNATURE));
    }

    public static void testLineAndCheck() {
        SignatureResult result = Signatures.check(signedFile(), Fixtures.trustFirstKey());
        Check.equal(SignatureResult.VERIFIED, result.result());
        Check.equal(Collections.singletonList("one"), result.signers());
        Check.equal(SignatureResult.UNKNOWN_KEY, Signatures.check(signedFile(), null).result());
        Check.equal(SignatureResult.UNSIGNED, Signatures.check(Fixtures.FILE, null).result());
    }

    public static void testSplitNeedsLineStart() {
        byte[] text = Fixtures.concat(Fixtures.FILE, Fixtures.utf8("about = \"no # mncf-signature: here\"\n"));
        Signatures.Split split = Signatures.split(text);
        Check.isTrue(Arrays.equals(text, split.signed), "the whole file is signed bytes");
        Check.equal(0, split.lines.size());
        Check.isTrue(split.clean, "no line is not a signature line");
    }

    public static void testKeyId() {
        byte[] hash = Signatures.sha256(Fixtures.PUBLIC_KEY);
        StringBuilder b = new StringBuilder();
        for (int i = 0; i < 8; i++) {
            b.append(String.format("%02x", hash[i] & 0xFF));
        }
        Check.equal(b.toString(), Signatures.keyId(Fixtures.PUBLIC_KEY));
        Check.equal("21fe31dfa154a261", Signatures.keyId(Fixtures.PUBLIC_KEY));
    }

    public static void testSignedFileLoads() throws MncfException {
        MncfFile f = Mncf.load(signedFile());
        Check.isTrue(Arrays.equals(Fixtures.FILE, f.signedBytes()), "the signed bytes end before the line");
        Check.equal(Collections.singletonList("21fe31dfa154a261"), f.signature().keyIds());
        Check.isTrue(Arrays.equals(Fixtures.FILE, Signatures.signedBytes(signedFile())), "signedBytes agrees");
    }

    public static void testNonCanonicalBase64() {
        String line = Fixtures.FILE_SIGNATURE;
        String sig64 = line.split(" ")[4].trim();
        String alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        char last = sig64.charAt(sig64.length() - 3);
        char bumped = alphabet.charAt(alphabet.indexOf(last) ^ 1);
        String bad = line.replace(sig64, sig64.substring(0, sig64.length() - 3) + bumped + "==");
        byte[] data = Fixtures.concat(Fixtures.FILE, Fixtures.utf8(bad));
        Check.equal(SignatureResult.INVALID, Signatures.check(data, Fixtures.trustFirstKey()).result());
    }

    public static void testOtherAlgorithmAndBadLines() {
        byte[] other = Fixtures.concat(Fixtures.FILE, Fixtures.utf8("# mncf-signature: rsa 0123 abc\n"));
        Check.equal(SignatureResult.UNKNOWN_KEY, Signatures.check(other, null).result());
        byte[] broken = Fixtures.concat(Fixtures.FILE, Fixtures.utf8("# mncf-signature: ed25519 0123 abc\n"));
        Check.equal(SignatureResult.INVALID, Signatures.check(broken, null).result());
        byte[] trailing = Fixtures.concat(signedFile(), Fixtures.utf8("x = 1\n"));
        Check.equal(SignatureResult.INVALID, Signatures.check(trailing, Fixtures.trustFirstKey()).result());
    }

    public static void testPrecedence() {
        byte[] twice = Fixtures.concat(signedFile(), Fixtures.utf8(Fixtures.FILE_SIGNATURE));
        SignatureResult both = Signatures.check(twice, Fixtures.trustFirstKey());
        Check.equal(SignatureResult.VERIFIED, both.result());
        Check.equal(Arrays.asList("one", "one"), both.signers());
        Check.equal(SignatureResult.REVOKED,
                Signatures.check(twice, Fixtures.trustFirstKey().revoke("21FE31DFA154A261")).result());
        byte[] withUnknown = Fixtures.concat(signedFile(), Fixtures.utf8("# mncf-signature: rsa 0123 abc\n"));
        Check.equal(SignatureResult.VERIFIED, Signatures.check(withUnknown, Fixtures.trustFirstKey()).result());
        byte[] withBad = Fixtures.concat(signedFile(), Fixtures.utf8("# mncf-signature: ed25519 0123 abc\n"));
        SignatureResult bad = Signatures.check(withBad, Fixtures.trustFirstKey());
        Check.equal(SignatureResult.INVALID, bad.result());
        Check.equal(0, bad.signers().size());
    }

    public static void testTrustListNeeds32Bytes() {
        Check.throwsWith(IllegalArgumentException.class, "32 bytes", new Check.Action() {
            @Override
            public void run() {
                new TrustList().trust("x", new byte[] {0, 0, 0});
            }
        });
    }
}
