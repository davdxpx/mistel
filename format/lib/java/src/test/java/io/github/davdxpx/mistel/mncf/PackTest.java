package io.github.davdxpx.mistel.mncf;

import io.github.davdxpx.mistel.mncf.testing.Check;
import java.io.ByteArrayOutputStream;
import java.io.IOException;
import java.util.Arrays;
import java.util.zip.ZipEntry;
import java.util.zip.ZipOutputStream;

/** Tests of reading packs (section 13). */
public final class PackTest {

    private PackTest() {
    }

    private static byte[] deflated(Object... namesAndData) throws IOException {
        ByteArrayOutputStream buf = new ByteArrayOutputStream();
        try (ZipOutputStream z = new ZipOutputStream(buf)) {
            for (int i = 0; i < namesAndData.length; i += 2) {
                z.putNextEntry(new ZipEntry((String) namesAndData[i]));
                z.write((byte[]) namesAndData[i + 1]);
                z.closeEntry();
            }
        }
        return buf.toByteArray();
    }

    private static String readError(final byte[] zip) {
        return Check.throwsWith(MncfException.class, "", new Check.Action() {
            @Override
            public void run() throws Exception {
                PackReader.read(zip, null);
            }
        }).code();
    }

    public static void testPaths() {
        for (String good : new String[] {"a.mncf", "games/a.mncf"}) {
            Check.isTrue(PackReader.isValidPath(good), good);
        }
        for (String bad : new String[] {"", "/a.mncf", "a/../b.mncf", "./a.mncf", "a\\b.mncf", "a//b.mncf", "a/"}) {
            Check.isTrue(!PackReader.isValidPath(bad), "not a pack path: " + bad);
        }
    }

    public static void testRead() throws Exception {
        byte[] manifest = Fixtures.utf8(Fixtures.MANIFEST_TEXT);
        for (byte[] zip : new byte[][] {Fixtures.storedZip("pack.toml", manifest, "a.mncf", Fixtures.FILE),
            deflated("pack.toml", manifest, "a.mncf", Fixtures.FILE)}) {
            Pack pack = PackReader.read(zip, null);
            Check.isTrue(pack.usable(), "an unsigned pack is usable");
            Check.equal("P", pack.name());
            Check.equal(1, pack.files().size());
            PackFile f = pack.file("a.mncf");
            Check.equal(PackFile.OK, f.status());
            Check.isTrue(Arrays.equals(Fixtures.FILE, f.data()), "the file's bytes");
        }
        byte[] signed = Fixtures.concat(manifest, Fixtures.utf8(Fixtures.MANIFEST_SIGNATURE));
        Pack verified = PackReader.read(Fixtures.storedZip("pack.toml", signed, "a.mncf", Fixtures.FILE),
                Fixtures.trustFirstKey());
        Check.equal(SignatureResult.VERIFIED, verified.signature().result());
        byte[] changed = Fixtures.utf8(new String(signed, "UTF-8").replace("name = \"P\"", "name = \"Q\""));
        Pack bad = PackReader.read(Fixtures.storedZip("pack.toml", changed, "a.mncf", Fixtures.FILE),
                Fixtures.trustFirstKey());
        Check.isTrue(!bad.usable(), "a manifest changed after signing is not usable");
    }

    public static void testStatusesAndWarnings() throws Exception {
        String text = Fixtures.MANIFEST_TEXT + "size = 3\n\n[[file]]\npath = \"b.mncf\"\nsha256 = \""
                + Fixtures.FILE_SHA256 + "\"\n\n[[file]]\npath = \"c.mncf\"\nsha256 = \"" + Fixtures.FILE_SHA256 + "\"\n";
        byte[] zip = Fixtures.storedZip("pack.toml", Fixtures.utf8(text), "a.mncf", Fixtures.FILE,
                "c.mncf", Fixtures.utf8("other"));
        Pack pack = PackReader.read(zip, null);
        Check.equal(PackFile.OK, pack.file("a.mncf").status());
        Check.equal(PackFile.MISSING, pack.file("b.mncf").status());
        Check.equal(PackFile.HASH, pack.file("c.mncf").status());
        Check.isTrue(pack.file("c.mncf").data() == null, "a file with the wrong hash has no data");
        Check.equal(Arrays.asList("unknown-key:file.size"), pack.warnings());
    }

    public static void testErrors() {
        byte[] manifest = Fixtures.utf8(Fixtures.MANIFEST_TEXT);
        Check.equal("zip", readError(Fixtures.utf8("this is not a ZIP file")));
        Check.equal("zip", readError(Fixtures.storedZip("pack.toml", manifest, "pack.toml", manifest)));
        Check.equal("manifest", readError(Fixtures.storedZip("a.mncf", Fixtures.FILE)));
        Check.equal("manifest", readError(Fixtures.storedZip("pack.toml", Fixtures.utf8("format = "))));
        Check.equal("manifest", readError(Fixtures.storedZip("pack.toml", Fixtures.utf8(
                Fixtures.MANIFEST_TEXT.replace("\n", "\r\n")))));
        Check.equal("format", readError(Fixtures.storedZip("pack.toml", Fixtures.utf8(
                Fixtures.MANIFEST_TEXT.replace("mncf-pack/1", "mncf-pack/2")))));
        Check.equal("path:../a.mncf", readError(Fixtures.storedZip("pack.toml", Fixtures.utf8(
                Fixtures.MANIFEST_TEXT.replace("\"a.mncf\"", "\"../a.mncf\"")))));
        Check.equal("schema:file.path", readError(Fixtures.storedZip("pack.toml", Fixtures.utf8(
                Fixtures.MANIFEST_TEXT.replace("\"a.mncf\"", "5")))));
        Check.equal("schema:file.path", readError(Fixtures.storedZip("pack.toml", Fixtures.utf8(
                Fixtures.MANIFEST_TEXT.replace("\"a.mncf\"", "[\"a\"]")))));
        Check.equal("schema:file.path", readError(Fixtures.storedZip("pack.toml", Fixtures.utf8(
                Fixtures.MANIFEST_TEXT.replace("path = \"a.mncf\"\n", "")))));
        Check.equal("schema:file.sha256", readError(Fixtures.storedZip("pack.toml", Fixtures.utf8(
                Fixtures.MANIFEST_TEXT.replace(Fixtures.FILE_SHA256, "ABC")))));
    }

    public static void testBrokenEntries() {
        byte[] manifest = Fixtures.utf8(Fixtures.MANIFEST_TEXT);
        byte[] zip = Fixtures.storedZip("pack.toml", manifest, "a.mncf", Fixtures.FILE);
        byte[] badCrc = zip.clone();
        badCrc[30 + "pack.toml".length()] ^= 1;
        Check.equal("zip", readError(badCrc));
        byte[] prepended = Fixtures.concat(Fixtures.utf8("some data before the archive"), zip);
        try {
            Check.equal(PackFile.OK, PackReader.read(prepended, null).file("a.mncf").status());
        } catch (MncfException e) {
            Check.fail("data before the archive is allowed: " + e.getMessage());
        }
    }
}
