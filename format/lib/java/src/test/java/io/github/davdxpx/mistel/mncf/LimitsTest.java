package io.github.davdxpx.mistel.mncf;

import io.github.davdxpx.mistel.mncf.testing.Check;
import java.io.ByteArrayOutputStream;
import java.io.IOException;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.List;
import java.util.zip.ZipEntry;
import java.util.zip.ZipOutputStream;

/** Tests of the size limits: of a file, of a pack's entries, and of all a reader reads from one pack. */
public final class LimitsTest {

    private LimitsTest() {
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

    private static String code(Check.Action action) {
        return Check.throwsWith(MncfException.class, "", action).code();
    }

    private static byte[] padded(int size) {
        byte[] out = Arrays.copyOf(Fixtures.FILE, size);
        Arrays.fill(out, Fixtures.FILE.length, size - 1, (byte) '#');
        out[size - 1] = '\n';
        return out;
    }

    public static void testFileSize() throws MncfException {
        final byte[] big = padded(Mncf.MAX_FILE + 1);
        Check.equal("size", code(new Check.Action() {
            @Override
            public void run() throws Exception {
                Mncf.load(big);
            }
        }));
        Check.equal("Example", Mncf.load(padded(Mncf.MAX_FILE)).name());
    }

    public static void testPackEntryThatInflatesPastTheLimit() throws IOException {
        final byte[] pack = deflated("pack.toml", new byte[Mncf.MAX_FILE + 1]);   // zeros deflate to a few kilobytes
        Check.isTrue(pack.length < 100000, "the pack has " + pack.length + " bytes");
        Check.equal("size", code(new Check.Action() {
            @Override
            public void run() throws Exception {
                PackReader.read(pack, null);
            }
        }));
    }

    public static void testPackEntriesAndTotal() throws Exception {
        List<Object> entries = new ArrayList<>();
        for (int i = 0; i < 4097; i++) {
            entries.add("f" + i + ".mncf");
            entries.add(new byte[0]);
        }
        entries.add("pack.toml");
        entries.add(new byte[0]);
        final byte[] many = Fixtures.storedZip(entries.toArray());
        Check.equal("size", code(new Check.Action() {
            @Override
            public void run() throws Exception {
                PackReader.read(many, null);
            }
        }));
        StringBuilder text = new StringBuilder("format = \"mncf-pack/1\"\nname = \"P\"\n");
        for (int i = 0; i < 3; i++) {
            text.append("\n[[file]]\npath = \"f").append(i).append(".mncf\"\nsha256 = \"").append(Fixtures.FILE_SHA256)
                    .append("\"\n");
        }
        final byte[] manifest = Fixtures.utf8(text.toString());
        final byte[] pack = deflated("pack.toml", manifest, "f0.mncf", Fixtures.FILE, "f1.mncf", Fixtures.FILE,
                "f2.mncf", Fixtures.FILE);
        Check.equal("size", code(new Check.Action() {
            @Override
            public void run() throws Exception {
                PackReader.read(pack, null, manifest.length + 2L * Fixtures.FILE.length);
            }
        }));
        List<String> statuses = new ArrayList<>();
        for (PackFile f : PackReader.read(pack, null).files()) {
            statuses.add(f.status());
        }
        Check.equal(Arrays.asList(PackFile.OK, PackFile.OK, PackFile.OK), statuses);
    }
}
