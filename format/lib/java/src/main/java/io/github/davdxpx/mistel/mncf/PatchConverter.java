package io.github.davdxpx.mistel.mncf;

import io.github.davdxpx.mistel.mncf.Converter.Item;
import io.github.davdxpx.mistel.mncf.Converter.Op;
import io.github.davdxpx.mistel.mncf.Converter.Options;
import io.github.davdxpx.mistel.mncf.Converter.Rep;
import io.github.davdxpx.mistel.mncf.Converter.Report;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.Collections;
import java.util.HashMap;
import java.util.HashSet;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Locale;
import java.util.Map;
import java.util.Set;
import java.util.regex.Pattern;

import static io.github.davdxpx.mistel.mncf.Converter.CREDIT_NAME;
import static io.github.davdxpx.mistel.mncf.Converter.authorsOf;
import static io.github.davdxpx.mistel.mncf.Converter.mergeInto;
import static io.github.davdxpx.mistel.mncf.Converter.mergeable;
import static io.github.davdxpx.mistel.mncf.Converter.nonEmpty;
import static io.github.davdxpx.mistel.mncf.Converter.patch;
import static io.github.davdxpx.mistel.mncf.Converter.siteOf;
import static io.github.davdxpx.mistel.mncf.Converter.slug;
import static io.github.davdxpx.mistel.mncf.Converter.table;

/**
 * Patch files (IPS, IPS32 and IPSwitch text) converted into an .mncf file, as {@link Converter}
 * converts Atmosphère cheats. It is a class of its own so that a runtime that converts only
 * Atmosphère cheats needs none of the patch-file classes.
 */
public final class PatchConverter {

    private PatchConverter() {
    }

    private static final Pattern TITLE_ID = Pattern.compile("[0-9A-Fa-f]{16}");
    private static final List<String> PATCH_FOLDERS = Arrays.asList("exefs", "exefs_patches", "atmosphere", "contents");

    /**
     * The patch set a file belongs to, from its path: the folder it is in
     * ("exefs_patches/&lt;set&gt;/&lt;build ID&gt;.ips", or a mod's folder above "exefs/"), else its own
     * name where that is no build ID, else "Patch".
     */
    static String setName(String fileName) {
        List<String> parts = new ArrayList<>();
        for (String part : Py.split((fileName == null ? "" : fileName).replace('\\', '/'), '/')) {
            if (!part.isEmpty()) {
                parts.add(part);
            }
        }
        if (parts.isEmpty()) {
            return "Patch";
        }
        for (int i = parts.size() - 2; i >= 0; i--) {
            String folder = parts.get(i);
            if (!PATCH_FOLDERS.contains(folder.toLowerCase(Locale.ROOT)) && !TITLE_ID.matcher(folder).matches()) {
                return folder;
            }
        }
        String last = parts.get(parts.size() - 1);
        int dot = last.lastIndexOf('.');
        String stem = dot < 0 ? last : last.substring(0, dot);
        return stem.isEmpty() || Patches.buildIdOf(last) != null ? "Patch" : stem;
    }

    private static Converter.CheatState patchCheat(Item item, String key, Rep rep, List<Patches.Run> runs, CodeModule module,
            String mname, boolean values) throws Atmosphere.Unclear {
        Converter.CheatState c = new Converter.CheatState(item, key, rep);
        for (Patches.Run run : runs) {
            long off = run.offset();
            byte[] data = run.dataShared();
            List<Object[]> pieces = new ArrayList<>();
            long at = off;
            while (at < off + data.length) {                  // one run for each section it crosses
                String sec = module.sectionOf(at);
                long stop = Math.min(off + data.length, sec != null ? module.sectionEnd(sec) : at + 1);
                pieces.add(new Object[] {sec, at, Arrays.copyOfRange(data, (int) (at - off), (int) (stop - off))});
                at = stop;
            }
            for (Object[] piece : pieces) {
                String sec = (String) piece[0];
                long where = (Long) piece[1];
                byte[] bytes = (byte[]) piece[2];
                if (sec == null) {
                    throw new Atmosphere.Unclear(String.format("%s+0x%X is in no section of the module", mname, where));
                }
                if (module.excluded(where, bytes.length)) {
                    throw new Atmosphere.Unclear(String.format("%s+0x%X: the module's bytes there are not its own, so "
                            + "there is no `expect`", mname, where));
                }
                byte[] expect = Arrays.copyOfRange(module.image(), (int) where, (int) where + bytes.length);
                if (Arrays.equals(expect, bytes)) {
                    rep.notes.add(String.format("%s+0x%X already holds those bytes; the patch stays, as the source "
                            + "writes them", mname, where));
                }
                if ("data".equals(sec)) {
                    c.ops.add(new Op("write", table("at", table("module", mname, "offsets",
                            table(key, Values.unsigned(where))), "type", "bytes", "value", Values.hexBytes(bytes),
                            "expect", Values.hexBytes(expect))));
                    rep.fixed++;
                } else {
                    c.ops.add(new Op("patch", patch(c, where, bytes, expect, module, values, mname)));
                }
            }
        }
        return c;
    }

    /** Returns a conversion of one patch file. */
    public static Conversion fromPatches(PatchSource source, Options options) throws ConvertException {
        return fromPatches(Collections.singletonList(source), options);
    }

    /**
     * Returns a conversion of patch files (IPS, IPS32 or IPSwitch text) of the same game, for one
     * or more builds.
     *
     * <p>Each IPS file becomes one cheat, named by its patch set, that is on by default, as a
     * loader applies it whenever it is there. Each patch of an IPSwitch file becomes one cheat,
     * named by its comment, on by default where it is @enabled. A write into code or read-only
     * data becomes a patch, one into data a write that is made only while the place holds its
     * original bytes, as a loader sets them only once.
     *
     * @throws ConvertException with the code "patch" for a file that is no patch file
     *     ({@link PatchFileException}), or "source" for a file without its module or build ID,
     *     a build with two build IDs for one module, or files of which no patch could be converted
     */
    public static Conversion fromPatches(List<PatchSource> sources, Options options) throws ConvertException {
        Report report = new Report();
        Map<String, Map<String, Object>> builds = new LinkedHashMap<>();
        List<List<Converter.CheatState>> perSource = new ArrayList<>();
        List<String> formats = new ArrayList<>();
        List<PatchFile> files = new ArrayList<>();
        List<String> bids = new ArrayList<>();
        for (PatchSource src : sources) {
            PatchFile f = Patches.read(src.dataShared());
            formats.add(f.format());
            String bid = f.buildId();
            if (!nonEmpty(bid)) {
                bid = nonEmpty(src.fileName()) ? Patches.buildIdOf(src.fileName()) : null;
            }
            if (!nonEmpty(bid)) {
                bid = nonEmpty(src.buildId()) ? Patches.trimBuildId(src.buildId()) : null;
            }
            if (!nonEmpty(bid)) {
                bid = src.module() != null && nonEmpty(src.module().buildId())
                        ? Patches.trimBuildId(src.module().buildId()) : null;
            }
            String what = nonEmpty(src.fileName()) ? src.fileName() : "a patch file";
            if (src.module() == null) {
                throw new ConvertException(what + ": a patch needs the module it patches, for `expect`");
            }
            if (!nonEmpty(bid)) {
                throw new ConvertException(what + ": no build ID in the file, its name or the module");
            }
            files.add(f);
            bids.add(bid);
        }
        Map<List<String>, Set<String>> versions = new HashMap<>();
        for (int k = 0; k < sources.size(); k++) {
            PatchSource src = sources.get(k);
            if (nonEmpty(src.version())) {
                List<String> vkey = Arrays.asList(src.version(), src.moduleName());
                if (!versions.containsKey(vkey)) {
                    versions.put(vkey, new HashSet<String>());
                }
                versions.get(vkey).add(bids.get(k));
            }
        }
        for (int k = 0; k < sources.size(); k++) {
            PatchSource src = sources.get(k);
            PatchFile f = files.get(k);
            String bid = bids.get(k);
            Set<String> same = versions.get(Arrays.asList(src.version(), src.moduleName()));
            boolean shared = nonEmpty(src.version()) && same != null && same.size() > 1;
            String named = shared ? bid : src.version();
            String key = nonEmpty(src.buildKey()) ? src.buildKey() : nonEmpty(named) ? named : bid;
            CodeModule module = src.module();
            String mid = module.buildId();
            if (nonEmpty(mid)) {
                String upper = mid.toUpperCase(Locale.ROOT);
                if (!(upper.startsWith(bid) || bid.startsWith(upper))) {
                    report.warnings.add("the module of build " + key + " is " + upper + ", its patch file " + bid);
                }
            }
            Map<String, Object> build = builds.get(key);
            if (build == null) {
                build = new LinkedHashMap<>();
                if (nonEmpty(src.version()) && !src.version().equals(key)) {
                    build.put("version", src.version());
                }
                builds.put(key, build);
            }
            if (!build.containsKey("modules")) {
                build.put("modules", new LinkedHashMap<String, Object>());
            }
            @SuppressWarnings("unchecked")
            Map<String, Object> mods = (Map<String, Object>) build.get("modules");
            if (mods.containsKey(src.moduleName()) && !bid.equals(mods.get(src.moduleName()))) {
                throw new ConvertException("build " + Py.repr(key) + " has two build IDs for " + src.moduleName());
            }
            mods.put(src.moduleName(), bid);
            String set = nonEmpty(src.setName()) ? src.setName() : setName(src.fileName());
            for (PatchFile.Line line : f.notes()) {
                if (!report.notes.contains(line.text())) {
                    report.notes.add(line.text());
                }
            }
            for (PatchFile.Line line : f.comments()) {
                if (CREDIT_NAME.matcher(line.text()).find() && !report.credits.contains(line.text())) {
                    report.credits.add(line.text());
                }
            }
            int count = f.patches().size();
            String group = count > 1 ? set : null;
            List<Converter.CheatState> out = new ArrayList<>();
            for (int i = 1; i <= count; i++) {
                Patch p = f.patches().get(i - 1);
                String own = nonEmpty(p.name()) && !CREDIT_NAME.matcher(p.name()).find() ? p.name() : "";
                String title = !own.isEmpty() ? own : count == 1 ? set : set + " " + i;
                Rep rep = new Rep("", title, key, CheatReport.PATTERN);
                Patches.ImageRuns written = Patches.imageRuns(p, module.image().length);
                for (String d : written.dropped()) {
                    rep.notes.add("left out " + d);
                }
                Converter.CheatState c;
                try {
                    if (written.runs().isEmpty()) {
                        throw new Atmosphere.Unclear("it writes nothing into the module");
                    }
                    c = patchCheat(new Item(title, false, group, null), key, rep, written.runs(), module,
                            src.moduleName(), options.values);
                } catch (Atmosphere.Unclear u) {
                    rep.outcome = CheatReport.UNREADABLE;
                    rep.why = u.why;
                    rep.patterns = 0;
                    rep.fixed = 0;
                    rep.values = 0;
                    rep.notes = new ArrayList<>();
                    for (String d : written.dropped()) {
                        rep.notes.add("left out " + d);
                    }
                    report.unreadable.add(title + " (" + u.why + ")");
                    report.cheats.add(rep);
                    continue;
                }
                c.defaultOn = p.enabled();
                rep.outcome = rep.patterns > 0 && rep.fixed == 0 ? CheatReport.PATTERN : CheatReport.BUILD;
                out.add(c);
                report.cheats.add(rep);
            }
            perSource.add(out);
        }

        List<List<Converter.CheatState>> merged = new ArrayList<>();
        Map<String, List<List<Converter.CheatState>>> byName = new HashMap<>();
        for (List<Converter.CheatState> cheats : perSource) {
            for (Converter.CheatState c : cheats) {
                List<Converter.CheatState> target = null;
                List<List<Converter.CheatState>> named = byName.get(c.item.name);
                for (List<Converter.CheatState> g : named == null ? Collections.<List<Converter.CheatState>>emptyList() : named) {
                    boolean keyTaken = false;
                    for (Converter.CheatState x : g) {
                        keyTaken |= x.key.equals(c.key);
                    }
                    if (!keyTaken && mergeable(g.get(0), c) && g.get(0).defaultOn == c.defaultOn
                            && java.util.Objects.equals(g.get(0).item.group, c.item.group)) {
                        target = g;
                        break;
                    }
                }
                if (target == null) {
                    target = new ArrayList<>();
                    target.add(c);
                    merged.add(target);
                    if (named == null) {
                        named = new ArrayList<>();
                        byName.put(c.item.name, named);
                    }
                    named.add(target);
                } else {
                    target.add(c);
                }
            }
        }
        Set<String> taken = new HashSet<>();
        List<Object> cheats = new ArrayList<>();
        for (List<Converter.CheatState> g : merged) {
            Converter.CheatState first = g.get(0);
            String cid = slug(first.item.name, taken);
            for (Converter.CheatState c : g) {
                c.rep.id = cid;
                if (c != first) {
                    mergeInto(first, c);
                }
            }
            Map<String, Object> t = table("id", cid, "name", first.item.name);
            if (nonEmpty(first.item.group)) {
                t.put("group", first.item.group);
            }
            if (first.defaultOn) {
                t.put("default", true);
            }
            if (!first.values.isEmpty()) {
                t.put("values", new LinkedHashMap<>(first.values));
            }
            for (Op op : first.ops) {
                if (!t.containsKey(op.kind)) {
                    t.put(op.kind, new ArrayList<Object>());
                }
                @SuppressWarnings("unchecked")
                List<Object> list = (List<Object>) t.get(op.kind);
                list.add(op.table);
            }
            cheats.add(t);
        }
        if (cheats.isEmpty()) {
            throw new ConvertException("no patch of the files could be converted: " + String.join("; ",
                    report.unreadable.isEmpty() ? Collections.singletonList("none") : report.unreadable));
        }

        Map<String, Object> doc = new LinkedHashMap<>();
        doc.put("format", "mncf/1");
        doc.put("name", nonEmpty(options.name) ? options.name
                : nonEmpty(options.gameName) ? options.gameName + " patches" : "Converted patches");
        List<String> found = authorsOf(report.credits);
        if (!found.isEmpty() || (options.authors != null && !options.authors.isEmpty())) {
            doc.put("authors", new ArrayList<Object>(!found.isEmpty() ? found : options.authors));
        }
        List<String> about = new ArrayList<>();
        if (!report.credits.isEmpty()) {
            about.add("Credits in the source: " + String.join("; ", report.credits));
        }
        if (!report.notes.isEmpty()) {
            about.add("Lines in the source that are not patches: " + String.join("; ", report.notes));
        }
        if (!report.unreadable.isEmpty()) {
            about.add("Patches that could not be converted, left out: " + String.join("; ", report.unreadable));
        }
        if (!about.isEmpty()) {
            doc.put("about", String.join("\n", about));
        }
        Map<String, Object> origin = table("format", formats.get(0).equals("ips32") ? "ips" : formats.get(0));
        if (nonEmpty(options.origin)) {
            origin.put("name", options.origin);
        }
        String site = siteOf(report.credits);
        if (site != null && !site.isEmpty()) {
            origin.put("site", site);
        }
        doc.put("origin", origin);
        for (Object c : cheats) {
            if (((Map<?, ?>) c).containsKey("values")) {
                doc.put("extensions", table("optional", new ArrayList<Object>(Collections.singletonList("arm64-imm/1"))));
                break;
            }
        }
        Map<String, Object> game = table("platform", options.platform, "id", options.gameId.toUpperCase(Locale.ROOT));
        if (nonEmpty(options.gameName)) {
            game.put("name", options.gameName);
        }
        doc.put("game", game);
        doc.put("builds", builds);
        doc.put("cheat", cheats);
        @SuppressWarnings("unchecked")
        Map<String, Object> frozen = (Map<String, Object>) Values.freeze(doc);
        List<CheatReport> reports = new ArrayList<>();
        for (Rep r : report.cheats) {
            reports.add(r.done());
        }
        return new Conversion(frozen, Writer.dumps(frozen),
                new ConversionReport(reports, report.groups, report.credits, report.warnings, report.notes,
                        report.unreadable));
    }
}
