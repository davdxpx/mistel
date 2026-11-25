package io.github.davdxpx.mistel.mncf;

import io.github.davdxpx.mistel.mncf.testing.Check;
import java.io.File;
import java.io.IOException;
import java.math.BigInteger;
import java.nio.file.Files;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.Collections;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;

/** Tests of updates (spec, section 17): the parts the conformance suite does not reach on its own. */
public final class UpdatesTest {

    private UpdatesTest() {
    }

    /** The public key of the first RFC 8032 key, "Test key one" of the suite. */
    private static final byte[] KEY_ONE = Fixtures.PUBLIC_KEY;

    private static byte[] file(String name) throws IOException {
        return Files.readAllBytes(new File(new File(ConformanceTest.suite(), "updates/files"), name).toPath());
    }

    private static List<String> versions(List<Release> changes) {
        List<String> out = new ArrayList<>();
        for (Release r : changes) {
            out.add(r.version());
        }
        return out;
    }

    // Versions

    public static void testCompareNeedsVersions() {
        Check.throwsWith(IllegalArgumentException.class, "not a version", new Check.Action() {
            @Override
            public void run() {
                Updates.compare("v3", "3");
            }
        });
        Check.equal(1, Updates.compare("2.10", "2.9"));
        Check.equal(0, Updates.compare("2", "2.0.0"));
        Check.equal(-1, Updates.compare("999999999", "999999999.0.0.0.0.0.0.1"));
    }

    public static void testAbove() {
        Check.isTrue(Updates.above("1", null), "any version is above none");
        Check.isTrue(Updates.above("1", "v3"), "any version is above a file_version that is not one");
        Check.isTrue(!Updates.above("v4", "3"), "what is not a version is above nothing");
        Check.isTrue(!Updates.above("3.0", "3"), "3.0 is the same version as 3");
        Check.isTrue(Updates.above("3.0.1", "3"), "3.0.1 is above 3");
    }

    public static void testForms() {
        Check.equal(null, Updates.urlHost(null));
        Check.equal("example.org", Updates.urlHost("https://example.org:443/x.mncf?a=1"));
        Check.isTrue(!Updates.isDate(null) && !Updates.isFileId(null) && !Updates.isVersion(null), "null is no form");
        StringBuilder id = new StringBuilder();
        while (id.length() < Updates.MAX_ID) {
            id.append('a');
        }
        Check.isTrue(Updates.isFileId(id.toString()), "an id of " + Updates.MAX_ID + " characters");
        Check.isTrue(!Updates.isFileId(id + "a"), "an id of more than " + Updates.MAX_ID + " characters");
        Check.equal("mncf-update/1", Updates.USER_AGENT);
        Check.equal(5, Updates.MAX_REDIRECTS);
    }

    // The keys

    public static void testRoundTrip() throws Exception {
        MncfFile f = Mncf.load(file("v5.mncf"));
        Check.equal(Collections.emptyList(), f.warnings());
        Check.equal("example.org/someone/update-example", f.id());
        Check.equal("example.org", f.update().host());
        Check.equal("https://example.org/someone/update-example.mncf", f.update().url());
        Check.equal(Arrays.asList("5", "4", "3"), versions(f.changes()));
        Release five = f.changes().get(0);
        Check.equal("2026-12-02", five.date());
        Check.equal(Collections.singletonMap("max-iv", "max-ivs"), five.renamed());
        Check.equal(Arrays.asList("shiny-odds"), five.added());
        MncfFile again = Mncf.load(Fixtures.utf8(Writer.write(f)));
        Check.equal(Writer.write(f), Writer.write(again));
        Check.equal(Arrays.asList("5", "4", "3"), versions(again.changes()));
    }

    public static void testLayout() {
        Map<String, Object> game = new LinkedHashMap<>();
        game.put("platform", "switch");
        game.put("id", "0100000000ABC000");
        Map<String, Object> renamed = new LinkedHashMap<>();
        renamed.put("a", "b");
        Map<String, Object> change = new LinkedHashMap<>();
        change.put("renamed", renamed);
        change.put("version", "2");
        Map<String, Object> at = new LinkedHashMap<>();
        at.put("region", "heap");
        at.put("offset", 0L);
        Map<String, Object> write = new LinkedHashMap<>();
        write.put("at", at);
        write.put("type", "u8");
        write.put("value", 1L);
        Map<String, Object> cheat = new LinkedHashMap<>();
        cheat.put("id", "b");
        cheat.put("name", "B");
        cheat.put("write", Collections.<Object>singletonList(write));
        Map<String, Object> doc = new LinkedHashMap<>();
        doc.put("format", "mncf/1");
        doc.put("name", "N");
        doc.put("game", game);
        doc.put("changes", Collections.<Object>singletonList(change));
        doc.put("file_version", "2");
        doc.put("update", Collections.<String, Object>singletonMap("url", "https://example.org/x.mncf"));
        doc.put("id", "example.org/x");
        doc.put("cheat", Collections.<Object>singletonList(cheat));
        List<String> lines = Arrays.asList(Writer.dumps(doc).split("\n", -1));
        Check.equal(Arrays.asList("format = \"mncf/1\"", "name = \"N\"", "id = \"example.org/x\"", "file_version = \"2\"",
                "", "[update]", "url = \"https://example.org/x.mncf\"", "", "[[changes]]", "version = \"2\"",
                "renamed = { a = \"b\" }", ""), lines.subList(0, 12));
        Check.equal("[game]", lines.get(12));
    }

    public static void testVerifiedKeys() throws Exception {
        MncfFile f = Mncf.load(file("v5-one.mncf"));
        String kid = Signatures.keyId(KEY_ONE);
        Check.equal(SignatureResult.UNKNOWN_KEY, f.signature().result());
        Check.equal(1, f.signers().size());
        Check.isTrue(Arrays.equals(KEY_ONE, f.signers().get(0)), "the key of signers");
        Map<String, byte[]> verified = f.signature().verified();
        Check.equal(Collections.singleton(kid), verified.keySet());
        Check.isTrue(Arrays.equals(KEY_ONE, verified.get(kid)), "the key that verified");
        verified.get(kid)[0] ^= 1;
        f.signers().get(0)[0] ^= 1;
        Check.isTrue(Arrays.equals(KEY_ONE, f.signature().verified().get(kid)), "verified gives copies");
        Check.isTrue(Arrays.equals(KEY_ONE, f.signers().get(0)), "signers gives copies");
        MncfFile bare = Mncf.load(file("v5-one-bare.mncf"));
        Check.equal(Collections.emptySet(), bare.signature().verified().keySet());
        Check.equal(Collections.singletonList(kid), bare.signature().keyIds());
        MncfFile trusted = Mncf.load(file("v5-one-bare.mncf"), new TrustList().trust("one", KEY_ONE));
        Check.equal(SignatureResult.VERIFIED, trusted.signature().result());
        Check.isTrue(Arrays.equals(KEY_ONE, trusted.signature().verified().get(kid)), "a trusted key verified");
    }

    private static byte[] packV2(String manifest) throws IOException {
        return Fixtures.storedZip("pack.toml", file(manifest), "a-new.mncf", file("pa.mncf"), "b.mncf", file("pb.mncf"),
                "d.mncf", file("pd.mncf"));
    }

    public static void testPackKeys() throws Exception {
        Pack pack = PackReader.read(packV2("pack-v2-one.toml"), null);
        Check.equal(Collections.emptyList(), pack.warnings());
        Check.equal("example.org/someone/update-pack", pack.id());
        Check.equal("example.org", pack.update().host());
        Map<String, String> renamed = new LinkedHashMap<>();
        renamed.put("old/b.mncf", "b.mncf");
        renamed.put("a.mncf", "a-new.mncf");
        Check.equal(renamed, pack.changes().get(0).renamed());
        Check.equal(new ArrayList<>(renamed.keySet()), new ArrayList<>(pack.changes().get(0).renamed().keySet()));
        Check.equal(1, pack.signers().size());
        Check.isTrue(Arrays.equals(KEY_ONE, pack.signers().get(0)), "the key of the manifest's signers");
        Check.equal(SignatureResult.UNKNOWN_KEY, pack.signature().result());
        Check.equal(Arrays.asList("2"), versions(Updates.since(pack, "1")));
    }

    public static void testPackCheck() throws Exception {
        Pack old = PackReader.read(packV2("pack-v2.toml"), null);
        UpdateCheck check = Updates.checkPack(old, packV2("pack-v2.toml"), null);
        Check.equal("update:version", check.reason());
        Check.isTrue(check.pack() != null && check.file() == null, "a pack check gives the new pack");
        UpdateCheck broken = Updates.checkPack(old, Fixtures.utf8("Not found"), null);
        Check.equal("update:error:zip", broken.reason());
        Check.isTrue(broken.pack() == null && broken.toVersion() == null, "nothing of a pack that does not load");
        Check.equal("2", broken.fromVersion());
    }

    // What the player set

    public static void testKeptAndHostData() throws Exception {
        MncfFile next = Mncf.load(file("v5.mncf"));
        Carry c = Updates.carry(next, "3", Choices.none(), null, Arrays.asList("max-iv", "speed", "money"));
        Map<String, String> ids = new LinkedHashMap<>();
        ids.put("max-iv", "max-ivs");
        ids.put("speed", null);
        ids.put("money", "money");
        Check.equal(ids, c.ids());
        Check.equal(Arrays.asList("speed"), c.dropped());
        Check.equal(ids, Updates.ids(next, "3", Arrays.asList("max-iv", "speed", "money")));
    }

    public static void testPresetKept() throws Exception {
        MncfFile next = Mncf.load(file("v5.mncf"));
        Check.equal("relaxed", Updates.carry(next, "3", Choices.none(), "relaxed", null).preset());
        Check.equal(null, Updates.carry(next, "3", Choices.none(), "race", null).preset());
        Check.equal(null, Updates.carry(next, "3", null, null, null).preset());
    }

    private static final String FLOAT_FILE = "format = \"mncf/1\"\nname = \"F\"\nid = \"example.org/f\"\n"
            + "file_version = \"2\"\n\n[game]\nplatform = \"switch\"\nid = \"0100000000ABC000\"\n\n[[cheat]]\n"
            + "id = \"speed\"\nname = \"Speed\"\n\n  [cheat.values.x]\n  type = \"f32\"\n  min = 0.5\n  max = 4.0\n"
            + "  default = 1.0\n\n  [[cheat.write]]\n  at = { region = \"heap\", offset = 0x10 }\n  type = \"f32\"\n"
            + "  value = \"$x\"\n";

    public static void testFloatValues() throws Exception {
        MncfFile next = Mncf.load(Fixtures.utf8(FLOAT_FILE));
        Carry c = Updates.carry(next, "1", Choices.builder().value("speed.x", 2).value("speed.y", 1).build(), null,
                null);
        Check.equal(Collections.<String, Object>singletonMap("speed.x", 2L), c.choices().values());
        Check.equal(Arrays.asList("speed.y"), c.gone());
        Check.equal(Collections.singletonMap("speed.x", Carry.TYPE),
                Updates.carry(next, "1", Choices.builder().value("speed.x", true).build(), null, null).reset());
        Check.equal(Collections.singletonMap("speed.x", Carry.TYPE),
                Updates.carry(next, "1", Choices.builder().value("speed.x", Double.NaN).build(), null, null).reset());
        Check.equal(Collections.singletonMap("speed.x", Carry.RANGE),
                Updates.carry(next, "1", Choices.builder().value("speed.x", 4.5).build(), null, null).reset());
    }

    private static final String VALUES_FILE = "format = \"mncf/1\"\nname = \"V\"\n\n[game]\nplatform = \"switch\"\n"
            + "id = \"0100000000ABC000\"\n\n[[cheat]]\nid = \"c\"\nname = \"C\"\n\n"
            + "  [cheat.values.small]\n  type = \"u8\"\n  min = 2\n  max = 200\n  default = 2\n\n"
            + "  [cheat.values.signed]\n  type = \"s16\"\n  default = -3\n\n"
            + "  [cheat.values.big]\n  type = \"u64\"\n  default = 0\n\n"
            + "  [cheat.values.real]\n  type = \"f32\"\n  min = -1.5\n  max = 1.0e30\n  default = 0.0\n\n"
            + "  [cheat.values.flag]\n  type = \"bool\"\n  default = false\n\n"
            + "  [cheat.values.item]\n  type = \"u16\"\n  choices = { \"1\" = \"One\", \"7\" = \"Seven\" }\n  default = 1\n\n"
            + "  [[cheat.write]]\n  at = { region = \"heap\", offset = 0x10 }\n  type = \"u8\"\n  value = \"$small\"\n";

    public static void testFitsAgreesWithTheEvaluator() throws Exception {
        Cheat c = Mncf.load(Fixtures.utf8(VALUES_FILE)).cheat("c");
        Object[] tries = {0L, 1L, 2L, 7L, 200L, 201L, -3L, -40000L, 2.0, 0.5, -2.0, 1.0e31, 1.0e39, Double.NaN,
            Double.POSITIVE_INFINITY, true, false, new BigInteger("18446744073709551615"),
            new BigInteger("18446744073709551616"), 3, (short) 7};
        int misfits = 0;
        for (String vid : c.values().keySet()) {
            ValueSpec spec = c.values().get(vid);
            for (Object v : tries) {
                Choices choices = Choices.builder().value("c." + vid, v).build();
                boolean taken = Evaluator.chosenValue(c, new Model.ValueRef(vid), choices) != null;
                String why = Updates.fits(spec, v);
                Check.isTrue(taken == why.isEmpty(), vid + " = " + v + ": the evaluator and fits differ (" + why + ")");
                misfits += why.isEmpty() ? 0 : 1;
            }
        }
        Check.isTrue(misfits > 20, "only " + misfits + " values did not fit");
        ValueSpec small = c.values().get("small");
        Check.equal("", Updates.fits(small, 2));
        Check.equal(Carry.RANGE, Updates.fits(small, 1L));
        Check.equal(Carry.TYPE, Updates.fits(small, 256L));
        Check.equal(Carry.TYPE, Updates.fits(small, 2.0));
        Check.equal(Carry.TYPE, Updates.fits(small, "2"));
        Check.equal(Carry.TYPE, Updates.fits(small, null));
        Check.equal(Carry.CHOICE, Updates.fits(c.values().get("item"), 2L));
        Check.equal(Carry.TYPE, Updates.fits(c.values().get("flag"), 1L));
        Check.equal("", Updates.fits(c.values().get("real"), 1));
    }

    // A new version

    public static void testCheckKeepsTheNewFile() throws Exception {
        MncfFile old = Mncf.load(file("v3.mncf"));
        UpdateCheck check = Updates.check(old, file("v5.mncf"), null);
        Check.isTrue(check.ok(), "v5 is a new version of v3: " + check);
        Check.equal("5", check.file().fileVersion());
        Check.equal("3", check.fromVersion());
        Check.equal("5", check.toVersion());
        Check.equal(null, check.pack());
        UpdateCheck refused = Updates.check(old, file("v3.mncf"), null);
        Check.isTrue(!refused.ok(), "the same version is no new version");
        Check.isTrue(refused.file() != null, "a file that is not taken is still given");
        UpdateCheck broken = Updates.check(old, Fixtures.utf8("<html>Not found</html>"), null);
        Check.equal("update:error:toml", broken.reason());
        Check.isTrue(broken.file() == null && broken.toVersion() == null, "nothing of a file that does not load");
    }

    public static void testRevokedInstalledKey() throws Exception {
        TrustList trust = new TrustList().revoke(Signatures.keyId(KEY_ONE));
        MncfFile old = Mncf.load(file("v3-one.mncf"), trust);
        Check.equal(SignatureResult.REVOKED, old.signature().result());
        Check.equal("update:signature", Updates.check(old, file("v5-one.mncf"), trust).reason());
    }

    public static void testShownAndMarked() throws Exception {
        MncfFile old = Mncf.load(file("v3.mncf"));
        MncfFile next = Updates.check(old, file("v5.mncf"), null).file();
        List<Release> shown = Updates.since(next, old.fileVersion());
        Check.equal(Arrays.asList("5", "4"), versions(shown));
        Check.equal(Arrays.asList("5", "4", "3"), versions(Updates.since(next, null)));
        Check.equal("Passt zu Spielversion 1.0.3. Geld geht wieder.", Updates.text(next, shown.get(0), "de"));
        Check.equal("The speed cheat is gone: it crashed in races.", Updates.text(next, shown.get(1), "de"));
        Check.equal("Fits game version 1.0.3. Money works again.", Updates.text(next, shown.get(0), null));
        Marks marks = Updates.marks(next, old.fileVersion());
        Check.equal(Arrays.asList("shiny-odds"), marks.added());
        Check.equal(Arrays.asList("money"), marks.fixed());
    }
}
