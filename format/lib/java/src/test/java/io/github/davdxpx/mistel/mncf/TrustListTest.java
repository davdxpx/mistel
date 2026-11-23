package io.github.davdxpx.mistel.mncf;

import io.github.davdxpx.mistel.mncf.testing.Check;
import java.util.Arrays;
import java.util.Base64;
import java.util.Collections;
import java.util.List;

/** Tests of reading trust list files (format mncf-keys/1). */
public final class TrustListTest {

    private TrustListTest() {
    }

    private static final byte[] PUB = Fixtures.PUBLIC_KEY;
    private static final String B64 = Base64.getEncoder().encodeToString(PUB);

    private static String text(String body) {
        return "format = \"mncf-keys/1\"\n" + body;
    }

    public static void testRead() throws MncfException {
        String kid = Signatures.keyId(PUB);
        TrustList trust = TrustList.fromToml(text("\n[[key]]\nname = \"One\"\npublic = \"" + B64 + "\"\nid = \"" + kid
                + "\"\n\n[[revoked]]\nid = \"39f713d0a644253f\"\n"));
        List<TrustList.Key> keys = trust.keys(kid);
        Check.equal(1, keys.size());
        Check.equal("One", keys.get(0).name);
        Check.isTrue(Arrays.equals(PUB, keys.get(0).publicKey), "the public key");
        Check.equal(Collections.singleton("39f713d0a644253f"), trust.revoked());
        Check.equal(Collections.<String>emptySet(), TrustList.fromToml(text("")).revoked());
        byte[] signed = Fixtures.concat(Fixtures.FILE, Fixtures.utf8(Fixtures.FILE_SIGNATURE));
        Check.equal(SignatureResult.VERIFIED, Signatures.check(signed, trust).result());
    }

    public static void testErrors() {
        String[][] cases = {
            {"[[key]]\nname = \"One\"\npublic = \"" + B64 + "\"\n", "format"},
            {text("\n[[key]]\nname = \"One\"\npublic = \"" + B64.substring(0, B64.length() - 2) + "\"\n"), "schema:key"},
            {text("\n[[key]]\npublic = \"" + B64 + "\"\n"), "schema:key"},
            {text("\n[[key]]\nname = \"One\"\npublic = \"" + B64 + "\"\nid = \"0000000000000000\"\n"), "schema:key"},
            {text("\n[[revoked]]\nid = \"ABC\"\n"), "schema:revoked"},
            {text("key = 5\n"), "schema:key"},
            {text("revoked = [1]\n"), "schema:revoked"},
            {text("\n[[key]]\nname = \"\"\npublic = \"" + B64 + "\"\n"), "schema:key"},
            {text("\n[[key]]\nname = \"One\"\npublic = \"" + B64.replace('=', 'A') + "\"\n"), "schema:key"},
            {text("when = 1979-05-27\n"), "toml"},
        };
        for (final String[] c : cases) {
            MncfException e = Check.throwsWith(MncfException.class, "", new Check.Action() {
                @Override
                public void run() throws Exception {
                    TrustList.fromToml(c[0]);
                }
            });
            Check.equal(c[1], e.code());
        }
    }
}
