package io.github.davdxpx.mistel.mncf;

import java.util.ArrayList;
import java.util.Arrays;
import java.util.Collection;
import java.util.Collections;
import java.util.HashSet;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;
import java.util.Set;

/**
 * Updates (section 17): a file's identity, its versions, the address of its newest version and
 * its change entries, and how a runtime takes a new version and keeps what the player set.
 *
 * <pre>{@code
 * UpdateCheck check = Updates.check(installed, bytes, trust);    // is it a new version of the installed file?
 * check.reason();                                                // "" when it is, else a reason of Appendix E
 * MncfFile next = check.file();
 * for (Release r : Updates.since(next, installed.fileVersion())) {        // the entries to show, newest first
 *     show(r.version(), r.date(), Updates.text(next, r, "de"));
 * }
 * Marks marks = Updates.marks(next, installed.fileVersion());              // cheats to mark as new or fixed
 * Carry carried = Updates.carry(next, installed.fileVersion(), choices, preset, kept);
 * }</pre>
 *
 * <p>The library makes no connection itself. A runtime asks for {@link Update#url()} as section
 * 17.3 says, with {@link #USER_AGENT} and at most {@link #MAX_REDIRECTS} redirects, and hands the
 * answer to {@link #check}.
 */
public final class Updates {

    /** The User-Agent a runtime sends when it looks for a new version, the same in every runtime. */
    public static final String USER_AGENT = "mncf-update/1";

    /** The most redirects a runtime follows when it looks for a new version, each to an https address. */
    public static final int MAX_REDIRECTS = 5;

    /** The most characters a file's id may have. */
    public static final int MAX_ID = UpdateKeys.MAX_ID;

    /** The most characters an update address may have. */
    public static final int MAX_URL = UpdateKeys.MAX_URL;

    private Updates() {
    }

    // Versions and the forms of the keys

    /**
     * Returns whether the text is a version of section 17.1: 1 to 8 whole numbers of 1 to 9
     * decimal digits, separated by dots.
     */
    public static boolean isVersion(String v) {
        return UpdateKeys.isVersion(v);
    }

    /**
     * Compares two versions number by number from the left, a number that is missing counting
     * as 0, so that "2" and "2.0" are the same version.
     *
     * @return -1, 0 or 1 as version a is below, the same as, or above version b
     * @throws IllegalArgumentException when either is not a version
     */
    public static int compare(String a, String b) {
        return UpdateKeys.compare(a, b);
    }

    /**
     * Returns whether version a is above version b. A b that is null or not a version counts as
     * below every version; an a that is not a version is above none.
     */
    public static boolean above(String a, String b) {
        return UpdateKeys.above(a, b);
    }

    /**
     * Returns whether the text is a file id of section 17.1: 1 to {@link #MAX_ID} characters,
     * lower-case ASCII letters, digits, ".", "-" and "_", in parts separated by "/", none of them
     * empty, "." or "..".
     */
    public static boolean isFileId(String v) {
        return UpdateKeys.isFileId(v);
    }

    /**
     * Returns the host of an address that section 17.3 allows for [update], or null where it
     * does not allow it: https only, a host name of lower-case parts and no IP address, a port
     * from 1 to 65535, a path and a query of the allowed characters, no user name and no "#",
     * and at most {@link #MAX_URL} characters.
     */
    public static String urlHost(String v) {
        return UpdateKeys.urlHost(v);
    }

    /** Returns whether the text is a date of section 17.1: "YYYY-MM-DD", a day of the Gregorian calendar. */
    public static boolean isDate(String v) {
        return UpdateKeys.isDate(v);
    }

    // A new version (section 17.2)

    /**
     * Returns whether the bytes are a new version of the loaded file the runtime holds (section
     * 17.2). Load the installed file with the same trust list, so that both signatures are
     * checked against the keys the runtime trusts now.
     *
     * @param installed the file the runtime holds
     * @param data the bytes of the new file, such as the answer to a request for its update url
     * @param trust the keys the reader trusts, or null for none
     */
    public static UpdateCheck check(MncfFile installed, byte[] data, TrustList trust) {
        MncfFile next;
        try {
            next = Mncf.load(data, trust);
        } catch (MncfException e) {
            return new UpdateCheck("update:error:" + e.code(), null, null, installed.fileVersion(), null);
        }
        boolean otherGame = !installed.platform().name().equals(next.platform().name())
                || !Py.lower(installed.gameId()).equals(Py.lower(next.gameId()));
        String reason = steps(installed.signature(), installed.id(), installed.fileVersion(), next.signature(),
                next.id(), next.fileVersion(), otherGame);
        return new UpdateCheck(reason, next, null, installed.fileVersion(), next.fileVersion());
    }

    /**
     * Returns whether the bytes are a new version of the pack the runtime holds (section 17.6):
     * the steps of section 17.2 for its manifest, without the game.
     *
     * @param installed the pack the runtime holds
     * @param data the bytes of the new .mncp file
     * @param trust the keys the reader trusts, or null for none
     */
    public static UpdateCheck checkPack(Pack installed, byte[] data, TrustList trust) {
        Pack next;
        try {
            next = PackReader.read(data, trust);
        } catch (MncfException e) {
            return new UpdateCheck("update:error:" + e.code(), null, null, installed.fileVersion(), null);
        }
        String reason = steps(installed.signature(), installed.id(), installed.fileVersion(), next.signature(),
                next.id(), next.fileVersion(), false);
        return new UpdateCheck(reason, null, next, installed.fileVersion(), next.fileVersion());
    }

    private static String steps(SignatureResult oldSig, String oldId, String oldVersion, SignatureResult newSig,
            String newId, String newVersion, boolean otherGame) {
        String result = newSig.result();
        if (result.equals(SignatureResult.INVALID) || result.equals(SignatureResult.REVOKED)) {
            return "update:signature";
        }
        if (otherGame) {
            return "update:game";
        }
        if (oldId == null || !oldId.equals(newId)) {
            return "update:id";
        }
        if (!above(newVersion, oldVersion)) {
            return "update:version";
        }
        if (!signedBy(oldSig, newSig)) {
            return "update:signer";
        }
        return "";
    }

    /** Step 6: whether a signature line of the new file verifies with a key that signed the old one. */
    private static boolean signedBy(SignatureResult old, SignatureResult next) {
        if (old.result().equals(SignatureResult.UNSIGNED)) {
            return true;
        }
        Map<String, byte[]> newKeys = next.verifiedKeys();
        Map<String, byte[]> oldKeys = old.verifiedKeys();
        for (String kid : old.keyIds()) {
            byte[] key = oldKeys.get(kid);
            if (key != null) {
                for (byte[] k : newKeys.values()) {
                    if (Arrays.equals(k, key)) {
                        return true;
                    }
                }
            } else if (newKeys.containsKey(kid)) {
                return true;
            }
        }
        return false;
    }

    // What to show and what the player set (sections 17.4 and 17.5)

    /** Returns the change entries of a file above a version (null: all of them), newest first. */
    public static List<Release> since(MncfFile f, String version) {
        return since(f.changes(), version);
    }

    /** Returns the change entries of a pack above a version (null: all of them), newest first. */
    public static List<Release> since(Pack p, String version) {
        return since(p.changes(), version);
    }

    private static List<Release> since(List<Release> changes, String version) {
        List<Release> out = new ArrayList<>();
        for (Release r : changes) {
            if (above(r.version(), version)) {
                out.add(r);
            }
        }
        return Collections.unmodifiableList(out);
    }

    /**
     * Returns the text of a change entry in a language, from the file's translations
     * (i18n.&lt;tag&gt;.changes."&lt;version&gt;".text), or else the entry's own text, which may
     * be null.
     *
     * @param language a BCP 47 tag, or null for the entry's own text
     */
    public static String text(MncfFile f, Release r, String language) {
        if (language != null && !language.isEmpty()) {
            Object tr = f.i18n().get(language);
            Object changes = tr instanceof Map ? ((Map<?, ?>) tr).get("changes") : null;
            Object entry = changes instanceof Map ? ((Map<?, ?>) changes).get(r.version()) : null;
            if (entry instanceof Map && ((Map<?, ?>) entry).get("text") instanceof String) {
                return (String) ((Map<?, ?>) entry).get("text");
            }
        }
        return r.text();
    }

    /**
     * Returns where each cheat id of the version the player has is in the new version f: the
     * ids follow the change entries above that version, in version order.
     *
     * @param version the version the player has, or null for none
     * @param ids cheat ids of that version
     * @return each id, in the order given, with its id in f, or null where it is gone
     */
    public static Map<String, String> ids(final MncfFile f, String version, Collection<String> ids) {
        return Collections.unmodifiableMap(follow(f.changes(), version, ids, new UpdateKeys.Name() {
            @Override
            public boolean ok(Object name) {
                return f.cheat((String) name) != null;
            }
        }));
    }

    private static Map<String, String> follow(List<Release> changes, String version, Collection<String> names,
            UpdateKeys.Name have) {
        Map<String, String> out = new LinkedHashMap<>();
        for (String n : names) {
            out.put(n, n);
        }
        List<Release> shown = since(changes, version);
        for (int i = shown.size() - 1; i >= 0; i--) {
            Release r = shown.get(i);
            for (Map.Entry<String, String> e : out.entrySet()) {
                String cur = e.getValue();
                if (cur == null) {
                    continue;
                }
                if (r.removed().contains(cur)) {
                    e.setValue(null);
                } else if (r.renamed().containsKey(cur)) {
                    e.setValue(r.renamed().get(cur));
                }
            }
        }
        for (Map.Entry<String, String> e : out.entrySet()) {
            if (e.getValue() != null && !have.ok(e.getValue())) {
                e.setValue(null);
            }
        }
        return out;
    }

    /**
     * Returns the cheats a runtime may mark in the new version f for a player who had a version
     * (section 17.4): those in added and those in fixed of the entries above it, under their ids
     * in f. A cheat that is new and fixed is marked new only.
     *
     * @param version the version the player has, or null for none
     */
    public static Marks marks(MncfFile f, String version) {
        Set<String> added = new HashSet<>();
        Set<String> fixed = new HashSet<>();
        List<Release> shown = since(f, version);
        for (int i = shown.size() - 1; i >= 0; i--) {
            Release r = shown.get(i);
            added = follow(added, r, r.added());
            fixed = follow(fixed, r, r.fixed());
        }
        List<String> marksNew = new ArrayList<>();
        List<String> marksFixed = new ArrayList<>();
        for (Cheat c : f.cheats()) {
            if (added.contains(c.id())) {
                marksNew.add(c.id());
            } else if (fixed.contains(c.id())) {
                marksFixed.add(c.id());
            }
        }
        return new Marks(marksNew, marksFixed);
    }

    private static Set<String> follow(Set<String> names, Release r, List<String> more) {
        Set<String> out = new HashSet<>();
        for (String x : names) {
            if (!r.removed().contains(x)) {
                out.add(r.renamed().containsKey(x) ? r.renamed().get(x) : x);
            }
        }
        out.addAll(more);
        return out;
    }

    /**
     * Returns whether a value the player chose fits a value of the file, by the rules the
     * evaluator refuses a cheat by (value-out-of-range): the empty string where it fits, else
     * why not: {@link Carry#TYPE} for another kind of value (a Boolean is the only value of type
     * bool, a whole number the only value of an integer type), NaN, or one too wide for the type;
     * else {@link Carry#RANGE} outside min and max; else {@link Carry#CHOICE} where it is not
     * among the choices.
     */
    public static String fits(ValueSpec spec, Object value) {
        Object v;
        try {
            v = Choices.number(value);
        } catch (IllegalArgumentException e) {
            return Carry.TYPE;
        }
        return Evaluator.misfit(spec, v);
    }

    /**
     * Returns what the player set for a version of a file, moved to the new version f (section
     * 17.5): the cheats switched on and off and the values, under their new ids, and the preset.
     * What the new version does not have is dropped; a value that no longer fits goes back to its
     * default.
     *
     * @param version the version the player has, or null for none
     * @param choices what the player chose for that version, or null for nothing
     * @param preset the preset the player chose, or null
     * @param kept cheats of that version for which the runtime keeps something else, such as the
     *     answer to a crash question, so that their new ids are in {@link Carry#ids()} too; or null
     */
    public static Carry carry(MncfFile f, String version, Choices choices, String preset, Collection<String> kept) {
        Choices chosen = choices == null ? Choices.none() : choices;
        List<String> seen = new ArrayList<>();
        List<String> look = new ArrayList<>(chosen.on());
        look.addAll(chosen.off());
        for (String key : chosen.values().keySet()) {
            look.add(cheatOf(key));
        }
        if (kept != null) {
            look.addAll(kept);
        }
        for (String cid : look) {
            if (!seen.contains(cid)) {
                seen.add(cid);
            }
        }
        Map<String, String> m = ids(f, version, seen);
        List<String> dropped = new ArrayList<>();
        for (String cid : seen) {
            if (m.get(cid) == null) {
                dropped.add(cid);
            }
        }
        List<String> on = moved(chosen.on(), m);
        List<String> off = moved(chosen.off(), m);
        Map<String, Object> values = new LinkedHashMap<>();
        List<String> gone = new ArrayList<>();
        Map<String, String> reset = new LinkedHashMap<>();
        for (Map.Entry<String, Object> e : chosen.values().entrySet()) {
            String key = e.getKey();
            String to = m.get(cheatOf(key));
            if (to == null) {
                continue;
            }
            int dot = key.indexOf('.');
            String vid = dot < 0 ? "" : key.substring(dot + 1);
            ValueSpec spec = f.cheat(to).values().get(vid);
            if (spec == null) {
                gone.add(key);
                continue;
            }
            String newKey = to + "." + vid;
            if (values.containsKey(newKey) || reset.containsKey(newKey)) {
                continue;
            }
            String why = Evaluator.misfit(spec, e.getValue());
            if (why.isEmpty()) {
                values.put(newKey, e.getValue());
            } else {
                reset.put(newKey, why);
            }
        }
        String keptPreset = null;
        for (Preset p : f.presets()) {
            if (p.id().equals(preset)) {
                keptPreset = preset;
            }
        }
        Choices.Builder b = Choices.builder().on(on).off(off);
        for (Map.Entry<String, Object> e : values.entrySet()) {
            b.value(e.getKey(), e.getValue());
        }
        return new Carry(b.build(), keptPreset, m, dropped, gone, reset);
    }

    private static String cheatOf(String key) {
        int dot = key.indexOf('.');
        return dot < 0 ? key : key.substring(0, dot);
    }

    private static List<String> moved(List<String> ids, Map<String, String> m) {
        List<String> out = new ArrayList<>();
        for (String cid : ids) {
            String to = m.get(cid);
            if (to != null && !out.contains(to)) {
                out.add(to);
            }
        }
        return out;
    }

    /**
     * Matches the files of an old pack with those of a new one (section 17.6): first by the id
     * and game of the files that load and have an id, then by path, following the renamed of the
     * new pack's change entries, where one of the two has no id.
     *
     * @param trust the keys the reader trusts, or null for none, for loading the files
     * @return each path of the old pack, in its order, with the path of its match in the new
     *     pack, or null where it has none and is gone
     */
    public static Map<String, String> pairs(Pack old, Pack next, TrustList trust) {
        Map<String, List<String>> olds = new LinkedHashMap<>();
        for (PackFile pf : old.files()) {
            olds.put(pf.path(), identity(pf, trust));
        }
        final Map<String, List<String>> news = new LinkedHashMap<>();
        for (PackFile pf : next.files()) {
            news.put(pf.path(), identity(pf, trust));
        }
        Map<String, String> out = new LinkedHashMap<>();
        Set<String> taken = new HashSet<>();
        for (Map.Entry<String, List<String>> e : olds.entrySet()) {
            if (e.getValue() == null) {
                continue;
            }
            for (Map.Entry<String, List<String>> n : news.entrySet()) {
                if (e.getValue().equals(n.getValue()) && !taken.contains(n.getKey())) {
                    out.put(e.getKey(), n.getKey());
                    taken.add(n.getKey());
                    break;
                }
            }
        }
        List<String> rest = new ArrayList<>();
        for (String path : olds.keySet()) {
            if (!out.containsKey(path)) {
                rest.add(path);
            }
        }
        Map<String, String> paths = follow(next.changes(), old.fileVersion(), rest, new UpdateKeys.Name() {
            @Override
            public boolean ok(Object name) {
                return news.containsKey(name);
            }
        });
        for (Map.Entry<String, String> e : paths.entrySet()) {
            String target = e.getValue();
            if (target != null && !taken.contains(target)
                    && (olds.get(e.getKey()) == null || news.get(target) == null)) {
                out.put(e.getKey(), target);
                taken.add(target);
            } else {
                out.put(e.getKey(), null);
            }
        }
        Map<String, String> ordered = new LinkedHashMap<>();
        for (String path : olds.keySet()) {
            ordered.put(path, out.get(path));
        }
        return Collections.unmodifiableMap(ordered);
    }

    /** The id, platform and game of a pack's file that loads and has an id, or null. */
    private static List<String> identity(PackFile pf, TrustList trust) {
        if (!pf.status().equals(PackFile.OK)) {
            return null;
        }
        MncfFile f;
        try {
            f = Mncf.load(pf.data(), trust);
        } catch (MncfException e) {
            return null;
        }
        return f.id() == null ? null : Arrays.asList(f.id(), f.platform().name(), Py.lower(f.gameId()));
    }
}
