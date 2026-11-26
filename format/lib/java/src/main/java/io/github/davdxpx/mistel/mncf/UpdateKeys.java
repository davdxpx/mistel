package io.github.davdxpx.mistel.mncf;

import java.util.ArrayList;
import java.util.Arrays;
import java.util.Base64;
import java.util.Collections;
import java.util.Comparator;
import java.util.HashSet;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;
import java.util.Set;
import java.util.regex.Matcher;
import java.util.regex.Pattern;

/**
 * The keys of section 17.1 that a file or a pack manifest carries, as a reader reads them with
 * the file: id, file_version, update, changes and signers, and the forms of their values. The
 * public calls are in {@link Updates}. This class is apart from the checks between two versions
 * there, so that a runtime that reads files and never takes a new version needs none of them.
 */
final class UpdateKeys {

    static final int MAX_ID = 128;
    static final int MAX_URL = 2048;

    private static final Pattern VERSION = Pattern.compile("[0-9]{1,9}(\\.[0-9]{1,9}){0,7}");
    private static final Pattern FILE_ID = Pattern.compile("[a-z0-9._-]+(/[a-z0-9._-]+)*");
    private static final Pattern DATE = Pattern.compile("([0-9]{4})-([0-9]{2})-([0-9]{2})");
    private static final String LABEL = "[a-z0-9](?:[a-z0-9-]{0,61}[a-z0-9])?";
    private static final Pattern URL = Pattern.compile("https://(" + LABEL + "(?:\\." + LABEL + ")+)(?::([0-9]{1,5}))?"
            + "(/[A-Za-z0-9\\-._~!$&'()*+,;=:@/%]*)?(\\?[A-Za-z0-9\\-._~!$&'()*+,;=:@/?%]*)?");
    private static final Pattern BAD_ESCAPE = Pattern.compile("%(?![0-9A-Fa-f]{2})");
    private static final List<String> UPDATE_KEYS = Values.list("url");
    private static final List<String> CHANGE_KEYS =
            Values.list("version", "date", "text", "added", "fixed", "removed", "renamed");
    private static final int[] DAYS = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};

    private UpdateKeys() {
    }

    // Versions and the forms of the keys (section 17.1). The public calls and their docs are in Updates.

    static boolean isVersion(String v) {
        return v != null && VERSION.matcher(v).matches();
    }

    private static int[] numbers(String v) {
        List<String> parts = Py.split(v, '.');
        int[] out = new int[parts.size()];
        for (int i = 0; i < out.length; i++) {
            out[i] = Integer.parseInt(parts.get(i));
        }
        return out;
    }

    static int compare(String a, String b) {
        if (!isVersion(a) || !isVersion(b)) {
            throw new IllegalArgumentException("not a version");
        }
        int[] x = numbers(a);
        int[] y = numbers(b);
        for (int i = 0; i < Math.max(x.length, y.length); i++) {
            int p = i < x.length ? x[i] : 0;
            int q = i < y.length ? y[i] : 0;
            if (p != q) {
                return p < q ? -1 : 1;
            }
        }
        return 0;
    }

    static boolean above(String a, String b) {
        if (!isVersion(a)) {
            return false;
        }
        return !isVersion(b) || compare(a, b) > 0;
    }

    // The forms of the keys

    static boolean isFileId(String v) {
        if (v == null || v.isEmpty() || v.length() > MAX_ID || !FILE_ID.matcher(v).matches()) {
            return false;
        }
        for (String part : Py.split(v, '/')) {
            if (part.equals(".") || part.equals("..")) {
                return false;
            }
        }
        return true;
    }

    static String urlHost(String v) {
        if (v == null || v.length() > MAX_URL) {
            return null;
        }
        Matcher m = URL.matcher(v);
        if (!m.matches() || BAD_ESCAPE.matcher(v).find()) {
            return null;
        }
        String host = m.group(1);
        String port = m.group(2);
        if (digits(host.substring(host.lastIndexOf('.') + 1))) {
            return null;
        }
        if (port != null && (Integer.parseInt(port) < 1 || Integer.parseInt(port) > 65535)) {
            return null;
        }
        return host;
    }

    private static boolean digits(String s) {
        if (s.isEmpty()) {
            return false;
        }
        for (int i = 0; i < s.length(); i++) {
            if (s.charAt(i) < '0' || s.charAt(i) > '9') {
                return false;
            }
        }
        return true;
    }

    static boolean isDate(String v) {
        Matcher m = Py.match(DATE, v);
        if (m == null) {
            return false;
        }
        int year = Integer.parseInt(m.group(1));
        int month = Integer.parseInt(m.group(2));
        int day = Integer.parseInt(m.group(3));
        if (month < 1 || month > 12) {
            return false;
        }
        boolean leap = year % 4 == 0 && (year % 100 != 0 || year % 400 == 0);
        int days = month == 2 && leap ? 29 : DAYS[month - 1];
        return day >= 1 && day <= days;
    }

    private static String str(Object v) {
        return v instanceof String ? (String) v : null;
    }

    // Reading the keys

    /** A check of one name in the lists of a change entry: a cheat id in a file, a path in a pack. */
    interface Name {
        boolean ok(Object name);
    }

    /** The names of a file's change entries: cheat ids (section 2). */
    static final Name CHEAT_ID = new Name() {
        @Override
        public boolean ok(Object name) {
            return Py.matches(Values.ID, name);
        }
    };

    /** The keys of section 17.1 of a file or a pack manifest, as {@link #readKeys} reads them. */
    static final class Keys {
        String id;
        Update update;
        List<Release> changes = new ArrayList<>();
    }

    /**
     * The public keys of the signers key (section 12), or none with the warning signers when the
     * key is not an array of 32-byte keys in standard base64, each once.
     */
    @SuppressWarnings("unchecked")
    static List<byte[]> readSigners(Map<String, Object> doc, List<String> warnings) {
        List<byte[]> keys = new ArrayList<>();
        if (!doc.containsKey("signers")) {
            return keys;
        }
        Object v = doc.get("signers");
        boolean good = v instanceof List;
        if (good) {
            for (Object s : (List<Object>) v) {
                byte[] raw = new byte[0];
                if (s instanceof String) {
                    try {
                        raw = Base64.getDecoder().decode((String) s);
                    } catch (IllegalArgumentException e) {
                        raw = new byte[0];
                    }
                }
                if (raw.length != 32 || !Base64.getEncoder().encodeToString(raw).equals(s) || holds(keys, raw)) {
                    good = false;
                    break;
                }
                keys.add(raw);
            }
        }
        if (!good) {
            warnings.add("signers");
            return new ArrayList<>();
        }
        return keys;
    }

    private static boolean holds(List<byte[]> keys, byte[] key) {
        for (byte[] k : keys) {
            if (Arrays.equals(k, key)) {
                return true;
            }
        }
        return false;
    }

    /**
     * The keys of section 17.1 of a file or a pack manifest: id, update and changes. What breaks
     * a rule is left out with a warning: id, update, changes, changes:&lt;version or position&gt;,
     * unknown-key:update.&lt;key&gt; and unknown-key:changes.&lt;key&gt;.
     *
     * @param item checks one name in the lists of a change entry
     */
    @SuppressWarnings("unchecked")
    static Keys readKeys(Map<String, Object> doc, List<String> warnings, Name item) {
        Keys out = new Keys();
        if (doc.containsKey("id")) {
            if (isFileId(str(doc.get("id")))) {
                out.id = (String) doc.get("id");
            } else {
                warnings.add("id");
            }
        }
        String version = str(doc.get("file_version"));
        boolean versioned = isVersion(version);
        if (doc.containsKey("update")) {
            Object u = doc.get("update");
            String host = null;
            if (u instanceof Map) {
                for (String k : ((Map<String, Object>) u).keySet()) {
                    if (!UPDATE_KEYS.contains(k)) {
                        warnings.add("unknown-key:update." + k);
                    }
                }
                host = urlHost(str(((Map<String, Object>) u).get("url")));
            }
            if (host == null || out.id == null || !versioned) {
                warnings.add("update");
            } else {
                out.update = new Update((String) ((Map<String, Object>) u).get("url"), host);
            }
        }
        if (doc.containsKey("changes")) {
            Object entries = doc.get("changes");
            boolean good = entries instanceof List;
            if (good) {
                for (Object e : (List<Object>) entries) {
                    good &= e instanceof Map;
                }
                good &= ((List<Object>) entries).isEmpty() || versioned;
            }
            if (!good) {
                warnings.add("changes");
            } else {
                List<Object> tables = (List<Object>) entries;
                List<Release> releases = new ArrayList<>();
                for (int i = 0; i < tables.size(); i++) {
                    Map<String, Object> e = (Map<String, Object>) tables.get(i);
                    for (String k : e.keySet()) {
                        if (!CHANGE_KEYS.contains(k)) {
                            warnings.add("unknown-key:changes." + k);
                        }
                    }
                    Release r = release(e, version, releases, item);
                    if (r == null) {
                        Object v = e.get("version");
                        warnings.add("changes:" + (v instanceof String ? (String) v : String.valueOf(i + 1)));
                    } else {
                        releases.add(r);
                    }
                }
                Collections.sort(releases, new Comparator<Release>() {
                    @Override
                    public int compare(Release a, Release b) {
                        return UpdateKeys.compare(b.version(), a.version());
                    }
                });
                out.changes = releases;
            }
        }
        return out;
    }

    /** One change entry, or null where it breaks a rule of section 17.1. */
    @SuppressWarnings("unchecked")
    private static Release release(Map<String, Object> e, String fileVersion, List<Release> seen, Name item) {
        String v = str(e.get("version"));
        if (!isVersion(v) || compare(v, fileVersion) > 0) {
            return null;
        }
        for (Release r : seen) {
            if (compare(v, r.version()) == 0) {
                return null;
            }
        }
        String date = null;
        if (e.containsKey("date")) {
            if (!isDate(str(e.get("date")))) {
                return null;
            }
            date = (String) e.get("date");
        }
        String text = null;
        if (e.containsKey("text")) {
            if (!(e.get("text") instanceof String)) {
                return null;
            }
            text = (String) e.get("text");
        }
        List<List<String>> lists = new ArrayList<>();
        for (String key : new String[] {"added", "fixed", "removed"}) {
            List<String> names = new ArrayList<>();
            if (e.containsKey(key)) {
                names = names(e.get(key), item);
                if (names == null) {
                    return null;
                }
            }
            lists.add(names);
        }
        Set<String> all = new HashSet<>();
        int count = 0;
        for (List<String> names : lists) {
            all.addAll(names);
            count += names.size();
        }
        if (all.size() != count) {
            return null;
        }
        List<String> removed = lists.get(2);
        Map<String, String> renamed = new LinkedHashMap<>();
        if (e.containsKey("renamed")) {
            Object ren = e.get("renamed");
            if (!(ren instanceof Map)) {
                return null;
            }
            for (Map.Entry<String, Object> x : ((Map<String, Object>) ren).entrySet()) {
                if (!item.ok(x.getKey()) || !item.ok(x.getValue())) {
                    return null;
                }
                renamed.put(x.getKey(), (String) x.getValue());
            }
            if (new HashSet<>(renamed.values()).size() != renamed.size()) {
                return null;
            }
            for (String from : renamed.keySet()) {
                if (removed.contains(from)) {
                    return null;
                }
            }
        }
        return new Release(v, date, text, lists.get(0), lists.get(1), removed, renamed);
    }

    private static List<String> names(Object v, Name item) {
        if (!(v instanceof List)) {
            return null;
        }
        List<String> out = new ArrayList<>();
        for (Object x : (List<?>) v) {
            if (!item.ok(x)) {
                return null;
            }
            out.add((String) x);
        }
        return new HashSet<>(out).size() == out.size() ? out : null;
    }
}
