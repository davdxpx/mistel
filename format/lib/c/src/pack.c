/* pack.c: packs (spec, section 13): a ZIP file .mncp with a manifest pack.toml at its root. MIT License. */
#include "model.h"
#include "zip.h"

#include <stdio.h>

struct mncf_pack {
    mn_alloc alloc;
    mn_arena arena;
    mncf_pack_info info;
    uint8_t **datas;              /* the bytes of each file whose status is ok, on the allocator */
    size_t *sizes;
    size_t count;
};

void mncf_pack_free(mncf_pack *p)
{
    mn_alloc a;
    size_t i;
    if (p == NULL) {
        return;
    }
    a = p->alloc;
    for (i = 0; p->datas != NULL && i < p->count; i++) {
        mn_free(&a, p->datas[i], p->sizes[i]);
    }
    mn_arena_free(&p->arena);
    mn_free(&a, p, sizeof *p);
}

const mncf_pack_info *mncf_pack_info_of(const mncf_pack *p)
{
    return p != NULL ? &p->info : NULL;
}

static int valid_path(const char *s, size_t n)
{
    size_t i, start = 0;
    if (n == 0 || memchr(s, '\\', n) != NULL) {
        return 0;
    }
    for (i = 0; i <= n; i++) {
        if (i == n || s[i] == '/') {
            size_t len = i - start;
            if (len == 0 || (len == 1 && s[start] == '.') || (len == 2 && s[start] == '.' && s[start + 1] == '.')) {
                return 0;
            }
            start = i + 1;
        }
    }
    return 1;
}

static int lower_hex64(const char *s, size_t n)
{
    size_t i;
    if (n != 64) {
        return 0;
    }
    for (i = 0; i < n; i++) {
        if (!((s[i] >= '0' && s[i] <= '9') || (s[i] >= 'a' && s[i] <= 'f'))) {
            return 0;
        }
    }
    return 1;
}

static int cmp_entry_names(const void *a, const void *b)
{
    const mn_zip_entry *x = *(const mn_zip_entry *const *)a, *y = *(const mn_zip_entry *const *)b;
    size_t n = x->name.n < y->name.n ? x->name.n : y->name.n;
    int c = n > 0 ? memcmp(x->name.p, y->name.p, n) : 0;
    if (c != 0) {
        return c;
    }
    return x->name.n < y->name.n ? -1 : x->name.n > y->name.n;
}

typedef struct budget {
    uint64_t used;
    uint64_t max;
} budget;

/* The bytes of one entry: only stored or deflated and not encrypted, and none of more than 16 MiB,
 * whatever its header claims. */
static int read_entry(const mn_zip *z, const mn_zip_entry *e, budget *b, mn_alloc *alloc, uint8_t **out, size_t *n,
                      mncf_error *err)
{
    if (!mn_zip_readable(e)) {
        mn_error(err, MNCF_ERR_ZIP, "zip", "%s: neither stored nor deflated, or encrypted", e->name.p);
        return 0;
    }
    if (e->size > MNCF_MAX_FILE) {
        mn_error(err, MNCF_ERR_SIZE, "size", "%s: more than %u bytes", e->name.p, (unsigned)MNCF_MAX_FILE);
        return 0;
    }
    if (!mn_zip_read(z, e, (size_t)MNCF_MAX_FILE + 1, alloc, out, n, err)) {
        if (err != NULL && err->status == MNCF_ERR_ZIP) {
            char message[sizeof err->message];
            memcpy(message, err->message, sizeof message);
            mn_error(err, MNCF_ERR_ZIP, "zip", "%s cannot be read (%s)", e->name.p, message);
        }
        return 0;
    }
    if (*n > MNCF_MAX_FILE) {
        mn_free(alloc, *out, *n);
        *out = NULL;
        mn_error(err, MNCF_ERR_SIZE, "size", "%s: more than %u bytes", e->name.p, (unsigned)MNCF_MAX_FILE);
        return 0;
    }
    b->used += *n;
    if (b->used > b->max) {
        mn_free(alloc, *out, *n);
        *out = NULL;
        mn_error(err, MNCF_ERR_SIZE, "size", "the pack holds more than %lu bytes", (unsigned long)b->max);
        return 0;
    }
    return 1;
}

static const mn_zip_entry *entry_named(mn_zip_entry *const *sorted, size_t count, const char *name, size_t n)
{
    size_t lo = 0, hi = count;
    mn_zip_entry key;
    const mn_zip_entry *kp = &key;
    key.name.p = name;
    key.name.n = n;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        int c = cmp_entry_names(&sorted[mid], &kp);
        if (c == 0) {
            return sorted[mid];
        }
        if (c < 0) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    return NULL;
}

static int schema_error(mncf_error *err, const char *key, const char *message)
{
    char code[300];
    snprintf(code, sizeof code, "schema:%s", key);
    mn_error(err, MNCF_ERR_SCHEMA, code, "%s", message);
    return 0;
}

/* Reads a pack; the limit of all bytes read is max_total. */
mncf_pack *mn_pack_read(const void *data, size_t size, const mncf_trust *trust, const mncf_allocator *alloc,
                        uint64_t max_total, mncf_error *err)
{
    static const char *const PACK_KEYS[] = {"format", "name", "authors", "about", "file_version", "file"};
    mn_alloc a = mn_alloc_of(alloc);
    mncf_pack *p;
    mn_zip z;
    mn_zip_entry **sorted;
    size_t count = 0, i, k;
    const mn_zip_entry *manifest;
    uint8_t *raw = NULL;
    size_t raw_len = 0;
    budget b;
    const mncf_toml *doc, *v, *files;
    const char **warnings = NULL;
    size_t warning_count = 0, warning_cap = 0;
    mncf_pack_file *list;
    uint32_t *seen_paths = NULL;
    size_t seen_cap = 0;
    mn_error_clear(err);
    if (data == NULL && size > 0) {
        mn_error(err, MNCF_ERR_ARGUMENT, "argument", "no bytes given");
        return NULL;
    }
    p = (mncf_pack *)mn_calloc(&a, sizeof *p);
    if (p == NULL) {
        mn_error_nomem(err);
        return NULL;
    }
    p->alloc = a;
    mn_arena_init(&p->arena, a);
    if (!mn_zip_open(&z, &p->arena, data != NULL ? (const uint8_t *)data : (const uint8_t *)"", size, err)) {
        if (err != NULL && err->status == MNCF_ERR_ZIP) {
            char message[sizeof err->message];
            memcpy(message, err->message, sizeof message);
            mn_error(err, MNCF_ERR_ZIP, "zip", "not a ZIP file (%s)", message);
        }
        goto fail;
    }
    sorted = (mn_zip_entry **)mn_arena_alloc(&p->arena, (z.count + 1) * sizeof *sorted);
    if (sorted == NULL) {
        goto nomem;
    }
    for (i = 0; i < z.count; i++) {
        if (z.entries[i].name.n == 0 || z.entries[i].name.p[z.entries[i].name.n - 1] != '/') {
            sorted[count++] = &z.entries[i];
        }
    }
    if (count > MNCF_MAX_PACK_ENTRIES) {
        mn_error(err, MNCF_ERR_SIZE, "size", "the pack has more than %u entries", (unsigned)MNCF_MAX_PACK_ENTRIES);
        goto fail;
    }
    b.used = 0;
    b.max = max_total;
    qsort(sorted, count, sizeof *sorted, cmp_entry_names);
    for (i = 1; i < count; i++) {
        if (cmp_entry_names(&sorted[i - 1], &sorted[i]) == 0) {
            mn_error(err, MNCF_ERR_ZIP, "zip", "two entries have the same name");
            goto fail;
        }
    }
    manifest = entry_named(sorted, count, "pack.toml", 9);
    if (manifest == NULL) {
        mn_error(err, MNCF_ERR_MANIFEST, "manifest", "there is no pack.toml at the root");
        goto fail;
    }
    if (!read_entry(&z, manifest, &b, &a, &raw, &raw_len, err)) {
        goto fail;
    }
    if (!mn_check_text(raw, raw_len, MNCF_ERR_MANIFEST, "manifest", err)) {
        goto fail;
    }
    doc = mn_parse_document(&p->arena, raw, raw_len, MNCF_ERR_MANIFEST, "manifest", err);
    if (doc == NULL) {
        goto fail;
    }
    if (doc->u.t.n == 0 || !mn_streqz(doc->u.t.e[0].key.p, doc->u.t.e[0].key.n, "format") ||
        !mn_toml_is(doc->u.t.e[0].val, MNCF_TOML_STRING) ||
        !mn_streqz(doc->u.t.e[0].val->u.s.p, doc->u.t.e[0].val->u.s.n, "mncf-pack/1")) {
        mn_error(err, MNCF_ERR_FORMAT, "format", "`format = \"mncf-pack/1\"` must be the first key of pack.toml");
        goto fail;
    }
    for (i = 0; i < doc->u.t.n; i++) {
        int known = 0;
        for (k = 0; k < sizeof PACK_KEYS / sizeof PACK_KEYS[0]; k++) {
            known |= mn_streqz(doc->u.t.e[i].key.p, doc->u.t.e[i].key.n, PACK_KEYS[k]);
        }
        if (!known) {
            char *w = mn_arena_printf(&p->arena, "unknown-key:%s", doc->u.t.e[i].key.p);
            if (w == NULL || MN_GROW(&p->arena, warnings, warning_count, warning_cap) == NULL) {
                goto nomem;
            }
            warnings[warning_count++] = w;
        }
    }
    v = mn_toml_getz(doc, "name");
    if (!mn_toml_is(v, MNCF_TOML_STRING)) {
        schema_error(err, "name", "`name` must be a string");
        goto fail;
    }
    p->info.name = v->u.s.p;
    if (!mn_signature_check(raw, raw_len, trust, &p->arena, &p->info.signature)) {
        goto nomem;
    }
    p->info.usable = p->info.signature.result != MNCF_SIGNATURE_INVALID &&
                     p->info.signature.result != MNCF_SIGNATURE_REVOKED;
    for (i = 0; i < 2; i++) {
        const char *key = i == 0 ? "about" : "file_version";
        v = mn_toml_getz(doc, key);
        if (v == NULL) {
            continue;
        }
        if (!mn_toml_is(v, MNCF_TOML_STRING)) {
            schema_error(err, key, "this key must be a string");
            goto fail;
        }
        if (i == 0) {
            p->info.about = v->u.s.p;
        } else {
            p->info.file_version = v->u.s.p;
        }
    }
    v = mn_toml_getz(doc, "authors");
    if (v != NULL) {
        const char **authors;
        int good = mn_toml_is(v, MNCF_TOML_ARRAY);
        for (i = 0; good && i < v->u.a.n; i++) {
            good = mn_toml_is(v->u.a.items[i], MNCF_TOML_STRING);
        }
        if (!good) {
            schema_error(err, "authors", "`authors` must be an array of strings");
            goto fail;
        }
        authors = (const char **)mn_arena_alloc(&p->arena, (v->u.a.n + 1) * sizeof *authors);
        if (authors == NULL) {
            goto nomem;
        }
        for (i = 0; i < v->u.a.n; i++) {
            authors[i] = v->u.a.items[i]->u.s.p;
        }
        p->info.authors = authors;
        p->info.author_count = v->u.a.n;
    }
    files = mn_toml_getz(doc, "file");
    {
        int good = mn_toml_is(files, MNCF_TOML_ARRAY) && files->u.a.n > 0;
        for (i = 0; good && i < files->u.a.n; i++) {
            good = mn_toml_is(files->u.a.items[i], MNCF_TOML_TABLE);
        }
        if (!good) {
            schema_error(err, "file", "the manifest needs at least one [[file]]");
            goto fail;
        }
    }
    list = (mncf_pack_file *)mn_arena_zalloc(&p->arena, (files->u.a.n + 1) * sizeof *list);
    p->datas = (uint8_t **)mn_arena_zalloc(&p->arena, (files->u.a.n + 1) * sizeof *p->datas);
    p->sizes = (size_t *)mn_arena_zalloc(&p->arena, (files->u.a.n + 1) * sizeof *p->sizes);
    if (list == NULL || p->datas == NULL || p->sizes == NULL) {
        goto nomem;
    }
    p->info.files = list;
    seen_cap = 16;
    while (seen_cap < files->u.a.n * 2) {
        seen_cap *= 2;
    }
    seen_paths = (uint32_t *)mn_arena_zalloc(&p->arena, seen_cap * sizeof *seen_paths);
    if (seen_paths == NULL) {
        goto nomem;
    }
    for (i = 0; i < files->u.a.n; i++) {
        const mncf_toml *e = files->u.a.items[i];
        const mncf_toml *path = mn_toml_getz(e, "path"), *digest = mn_toml_getz(e, "sha256");
        const mn_zip_entry *entry;
        size_t j;
        if (!mn_toml_is(path, MNCF_TOML_STRING)) {
            schema_error(err, "file.path", "`path` must be a string");
            goto fail;
        }
        {
            int seen = 0;
            size_t slot = (size_t)mn_hash(path->u.s.p, path->u.s.n) & (seen_cap - 1);
            while (seen_paths[slot] != 0) {
                const mncf_toml *other = mn_toml_getz(files->u.a.items[seen_paths[slot] - 1], "path");
                if (mn_streq(other->u.s.p, other->u.s.n, path->u.s.p, path->u.s.n)) {
                    seen = 1;
                    break;
                }
                slot = (slot + 1) & (seen_cap - 1);
            }
            if (!valid_path(path->u.s.p, path->u.s.n) || mn_streqz(path->u.s.p, path->u.s.n, "pack.toml") || seen) {
                char code[300];
                snprintf(code, sizeof code, "path:%s", path->u.s.p);
                mn_error(err, MNCF_ERR_PATH, code, "a path that is not usable in a pack");
                goto fail;
            }
        }
        if (!mn_toml_is(digest, MNCF_TOML_STRING) || !lower_hex64(digest->u.s.p, digest->u.s.n)) {
            schema_error(err, "file.sha256", "`sha256` must be 64 lower-case hexadecimal digits");
            goto fail;
        }
        for (j = 0; j < e->u.t.n; j++) {
            if (!mn_streqz(e->u.t.e[j].key.p, e->u.t.e[j].key.n, "path") &&
                !mn_streqz(e->u.t.e[j].key.p, e->u.t.e[j].key.n, "sha256")) {
                char *w = mn_arena_printf(&p->arena, "unknown-key:file.%s", e->u.t.e[j].key.p);
                if (w == NULL || MN_GROW(&p->arena, warnings, warning_count, warning_cap) == NULL) {
                    goto nomem;
                }
                warnings[warning_count++] = w;
            }
        }
        {
            size_t slot = (size_t)mn_hash(path->u.s.p, path->u.s.n) & (seen_cap - 1);
            while (seen_paths[slot] != 0) {
                slot = (slot + 1) & (seen_cap - 1);
            }
            seen_paths[slot] = (uint32_t)(i + 1);
        }
        list[i].path = path->u.s.p;
        list[i].sha256 = digest->u.s.p;
        p->info.file_count = i + 1;
        p->count = i + 1;
        entry = entry_named(sorted, count, path->u.s.p, path->u.s.n);
        if (entry == NULL) {
            list[i].status = MNCF_PACK_MISSING;
            continue;
        }
        {
            uint8_t *bytes = NULL;
            size_t n = 0;
            uint8_t hash[32];
            char hex[65];
            static const char HEX[] = "0123456789abcdef";
            if (!read_entry(&z, entry, &b, &a, &bytes, &n, err)) {
                goto fail;
            }
            mncf_sha256(bytes, n, hash);
            for (j = 0; j < 32; j++) {
                hex[2 * j] = HEX[hash[j] >> 4];
                hex[2 * j + 1] = HEX[hash[j] & 15];
            }
            hex[64] = 0;
            if (memcmp(hex, digest->u.s.p, 64) != 0) {
                list[i].status = MNCF_PACK_HASH;
                mn_free(&a, bytes, n);
            } else {
                list[i].status = MNCF_PACK_OK;
                list[i].data = bytes;
                list[i].size = n;
                p->datas[i] = bytes;
                p->sizes[i] = n;
            }
        }
    }
    p->info.warnings = warnings;
    p->info.warning_count = warning_count;
    mn_free(&a, raw, raw_len);
    if (p->arena.failed) {
        p->arena.failed = 0;
        mn_error_nomem(err);
        mncf_pack_free(p);
        return NULL;
    }
    return p;
nomem:
    mn_error_nomem(err);
fail:
    mn_free(&a, raw, raw_len);
    mncf_pack_free(p);
    return NULL;
}

mncf_pack *mncf_pack_read(const void *data, size_t size, const mncf_trust *trust, const mncf_allocator *alloc,
                          mncf_error *err)
{
    return mn_pack_read(data, size, trust, alloc, MNCF_MAX_PACK_TOTAL, err);
}
