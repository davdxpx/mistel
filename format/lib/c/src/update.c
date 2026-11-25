/* update.c: new versions of a file (spec, section 17): a file's identity, its versions, the address of its
 * newest version and its change entries; whether a file or pack is a new version of the one a runtime
 * holds, the entries to show, the cheats to mark, what the player set moved to the new version, and the
 * files of an old pack matched with a new one. The checks run in the order of the Python reference
 * (format/lib/python/mncf/update.py), so that the results are the same. MIT License. */
#include "model.h"

#include <stdio.h>
#include <stdlib.h>

#define UP_MAX_PARTS 8
#define UP_MAX_DIGITS 9

static const char *const UP_CHANGE_KEYS[] = {"version", "date", "text", "added", "fixed", "removed", "renamed"};

/* ---- The forms of section 17 ------------------------------------------------------------------------ */

static int up_digit(char c)
{
    return c >= '0' && c <= '9';
}

int mn_version_valid(const char *s, size_t n)
{
    size_t i, parts = 1, digits = 0;
    if (s == NULL || n == 0) {
        return 0;
    }
    for (i = 0; i < n; i++) {
        if (s[i] == '.') {
            if (digits == 0 || ++parts > UP_MAX_PARTS) {
                return 0;
            }
            digits = 0;
        } else if (up_digit(s[i]) && digits < UP_MAX_DIGITS) {
            digits++;
        } else {
            return 0;
        }
    }
    return digits > 0;
}

/* The numbers of a valid version; returns how many. Each has at most 9 digits, so it fits. */
static size_t up_numbers(const char *s, size_t n, uint32_t out[UP_MAX_PARTS])
{
    size_t i, k = 0;
    uint32_t v = 0;
    for (i = 0; i <= n && k < UP_MAX_PARTS; i++) {
        if (i == n || s[i] == '.') {
            out[k++] = v;
            v = 0;
        } else {
            v = v * 10u + (uint32_t)(s[i] - '0');
        }
    }
    return k;
}

int mn_version_cmp(const char *a, size_t an, const char *b, size_t bn)
{
    uint32_t x[UP_MAX_PARTS], y[UP_MAX_PARTS];
    size_t xn = up_numbers(a, an, x), yn = up_numbers(b, bn, y), i;
    for (i = 0; i < UP_MAX_PARTS; i++) {
        uint32_t u = i < xn ? x[i] : 0, v = i < yn ? y[i] : 0;
        if (u != v) {
            return u < v ? -1 : 1;
        }
    }
    return 0;
}

int mn_file_id_valid(const char *s, size_t n)
{
    size_t i, start = 0;
    if (s == NULL || n == 0 || n > MNCF_MAX_FILE_ID) {
        return 0;
    }
    for (i = 0; i <= n; i++) {
        if (i == n || s[i] == '/') {
            size_t len = i - start;
            if (len == 0 || (len == 1 && s[start] == '.') || (len == 2 && s[start] == '.' && s[start + 1] == '.')) {
                return 0;
            }
            start = i + 1;
        } else if (!((s[i] >= 'a' && s[i] <= 'z') || up_digit(s[i]) || s[i] == '.' || s[i] == '-' || s[i] == '_')) {
            return 0;
        }
    }
    return 1;
}

int mn_date_valid(const char *s, size_t n)
{
    static const int DAYS[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    size_t i;
    int y, m, d, days;
    if (s == NULL || n != 10 || s[4] != '-' || s[7] != '-') {
        return 0;
    }
    for (i = 0; i < 10; i++) {
        if (i != 4 && i != 7 && !up_digit(s[i])) {
            return 0;
        }
    }
    y = (s[0] - '0') * 1000 + (s[1] - '0') * 100 + (s[2] - '0') * 10 + (s[3] - '0');
    m = (s[5] - '0') * 10 + (s[6] - '0');
    d = (s[8] - '0') * 10 + (s[9] - '0');
    if (m < 1 || m > 12) {
        return 0;
    }
    days = DAYS[m - 1] + (m == 2 && y % 4 == 0 && (y % 100 != 0 || y % 400 == 0));
    return d >= 1 && d <= days;
}

static int up_host_char(char c)
{
    return (c >= 'a' && c <= 'z') || up_digit(c) || c == '-' || c == '.';
}

/* A character of a path or a query: ASCII letters, digits and -._~!$&'()*+,;=:@/% */
static int up_path_char(char c)
{
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || up_digit(c) ||
           (c != 0 && strchr("-._~!$&'()*+,;=:@/%", c) != NULL);
}

size_t mn_url_host(const char *s, size_t n, size_t *start)
{
    size_t i = 8, host_end, label = 8, last = 8, parts = 0, k;
    if (s == NULL || n > MNCF_MAX_URL || n < 8 || memcmp(s, "https://", 8) != 0) {
        return 0;
    }
    /* The host runs up to the first character that is not one of a name: a port, a path, a query or the
     * end must follow it. */
    while (i < n && up_host_char(s[i])) {
        i++;
    }
    host_end = i;
    for (k = 8; k <= host_end; k++) {
        if (k == host_end || s[k] == '.') {
            size_t len = k - label;
            if (len == 0 || len > 63 || s[label] == '-' || s[k - 1] == '-') {
                return 0;
            }
            last = label;
            parts++;
            label = k + 1;
        }
    }
    if (parts < 2) {
        return 0;
    }
    for (k = last; k < host_end && up_digit(s[k]); k++) {
    }
    if (k == host_end) {
        return 0;               /* the last part only digits: an IP address, not a name */
    }
    if (i < n && s[i] == ':') {
        size_t first = ++i;
        uint32_t port = 0;
        while (i < n && up_digit(s[i]) && i - first < 6) {
            port = port * 10u + (uint32_t)(s[i] - '0');
            i++;
        }
        if (i == first || i - first > 5 || port < 1 || port > 65535) {
            return 0;
        }
    }
    if (i < n && s[i] == '/') {
        for (i++; i < n && up_path_char(s[i]); i++) {
        }
    }
    if (i < n && s[i] == '?') {
        for (i++; i < n && (up_path_char(s[i]) || s[i] == '?'); i++) {
        }
    }
    if (i != n) {
        return 0;
    }
    for (k = 0; k < n; k++) {
        if (s[k] == '%' && (k + 2 >= n || !mn_is_hex((unsigned char)s[k + 1]) || !mn_is_hex((unsigned char)s[k + 2]))) {
            return 0;
        }
    }
    *start = 8;
    return host_end - 8;
}

int mncf_version_valid(const char *text)
{
    return text != NULL && mn_version_valid(text, strlen(text));
}

mncf_status mncf_version_compare(const char *a, const char *b, int *result)
{
    if (!mncf_version_valid(a) || !mncf_version_valid(b)) {
        return MNCF_ERR_ARGUMENT;
    }
    if (result != NULL) {
        *result = mn_version_cmp(a, strlen(a), b, strlen(b));
    }
    return MNCF_OK;
}

int mncf_file_id_valid(const char *text)
{
    return text != NULL && mn_file_id_valid(text, strlen(text));
}

int mncf_date_valid(const char *text)
{
    return text != NULL && mn_date_valid(text, strlen(text));
}

size_t mncf_update_url_host(const char *url, char *host, size_t size)
{
    size_t start = 0, n;
    if (host != NULL && size > 0) {
        host[0] = 0;
    }
    if (url == NULL) {
        return 0;
    }
    n = mn_url_host(url, strlen(url), &start);
    if (n > 0 && host != NULL && size > 0) {
        size_t k = n < size - 1 ? n : size - 1;
        memcpy(host, url + start, k);
        host[k] = 0;
    }
    return n;
}

/* Whether version a is above b, where b missing or not a version counts as below every version. */
static int up_above(const char *a, size_t an, const char *b, size_t bn)
{
    if (a == NULL || !mn_version_valid(a, an)) {
        return 0;
    }
    return b == NULL || !mn_version_valid(b, bn) || mn_version_cmp(a, an, b, bn) > 0;
}

/* ---- Reading the keys ------------------------------------------------------------------------------- */

static int up_cmp_str(const void *a, const void *b)
{
    const mn_str *x = (const mn_str *)a, *y = (const mn_str *)b;
    size_t n = x->n < y->n ? x->n : y->n;
    int c = n > 0 ? memcmp(x->p, y->p, n) : 0;
    if (c != 0) {
        return c;
    }
    return x->n < y->n ? -1 : x->n > y->n;
}

/* Whether a list of names holds one twice: 1, 0, or -1 on no memory. The list is sorted in a copy. */
static int up_twice(const mn_alloc *alloc, const mn_str *items, size_t n)
{
    mn_str *copy;
    size_t i;
    int twice = 0;
    if (n < 2) {
        return 0;
    }
    copy = (mn_str *)mn_malloc(alloc, n * sizeof *copy);
    if (copy == NULL) {
        return -1;
    }
    memcpy(copy, items, n * sizeof *copy);
    qsort(copy, n, sizeof *copy, up_cmp_str);
    for (i = 1; i < n && !twice; i++) {
        twice = up_cmp_str(&copy[i - 1], &copy[i]) == 0;
    }
    mn_free(alloc, copy, n * sizeof *copy);
    return twice;
}

static int up_cmp_key(const void *a, const void *b)
{
    return memcmp(*(const uint8_t *const *)a, *(const uint8_t *const *)b, 32);
}

int mn_read_signers(mn_arena *ar, const mncf_toml *doc, mn_warnings *w, const uint8_t (**out)[32], size_t *count)
{
    const mncf_toml *v = mn_toml_getz(doc, "signers");
    *out = NULL;
    *count = 0;
    if (v == NULL) {
        return 1;
    }
    if (mn_toml_is(v, MNCF_TOML_ARRAY)) {
        size_t n = v->u.a.n, i;
        uint8_t(*keys)[32] = (uint8_t(*)[32])mn_arena_alloc(ar, (n + 1) * sizeof *keys);
        const uint8_t **order;
        int good = 1;
        if (keys == NULL) {
            return 0;
        }
        for (i = 0; good && i < n; i++) {
            const mncf_toml *s = v->u.a.items[i];
            good = mn_toml_is(s, MNCF_TOML_STRING) && mn_public_key_of(s->u.s.p, s->u.s.n, keys[i]);
        }
        /* Each key once: sorted in a copy of pointers to them. */
        if (good && n > 1) {
            order = (const uint8_t **)mn_malloc(&ar->alloc, n * sizeof *order);
            if (order == NULL) {
                ar->failed = 1;
                return 0;
            }
            for (i = 0; i < n; i++) {
                order[i] = keys[i];
            }
            qsort(order, n, sizeof *order, up_cmp_key);
            for (i = 1; good && i < n; i++) {
                good = memcmp(order[i - 1], order[i], 32) != 0;
            }
            mn_free(&ar->alloc, order, n * sizeof *order);
        }
        if (good) {
            *out = (const uint8_t(*)[32])keys;
            *count = n;
            return 1;
        }
    }
    return mn_warn(ar, w, "signers");
}

/* A version as its number of parts with the trailing zeros taken off, for a set of the versions seen. */
static uint64_t up_version_hash(const char *s, size_t n)
{
    uint32_t parts[UP_MAX_PARTS];
    size_t k = up_numbers(s, n, parts);
    while (k > 1 && parts[k - 1] == 0) {
        k--;
    }
    return mn_hash(parts, k * sizeof parts[0]);
}

/* What reading the change entries of a document needs. */
typedef struct up_reader {
    mn_arena *ar;
    int (*item)(const char *s, size_t n);
    mn_str file_version;
    mncf_release *list;         /* the entries taken so far, in file order */
    size_t count;
    size_t *slots;              /* a set of their versions: index + 1, 0 for none */
    size_t slot_cap;            /* a power of two */
} up_reader;

/* The entry with the same version as s, as a number, or NULL. */
static const mncf_release *up_seen(const up_reader *rd, const char *s, size_t n, size_t *slot)
{
    size_t mask = rd->slot_cap - 1, at = (size_t)up_version_hash(s, n) & mask;
    while (rd->slots[at] != 0) {
        const mncf_release *r = &rd->list[rd->slots[at] - 1];
        if (mn_version_cmp(r->version, strlen(r->version), s, n) == 0) {
            return r;
        }
        at = (at + 1) & mask;
    }
    *slot = at;
    return NULL;
}

/* A list of names in a change entry: an array of names that item allows, each once. 1, 0 where it
 * breaks a rule, -1 on no memory. *strs gets the names with their lengths. */
static int up_names(up_reader *rd, const mncf_toml *v, const char *const **out, size_t *count, mn_str **strs)
{
    size_t i, n;
    const char **names;
    mn_str *items;
    int twice;
    if (!mn_toml_is(v, MNCF_TOML_ARRAY)) {
        return 0;
    }
    n = v->u.a.n;
    names = (const char **)mn_arena_alloc(rd->ar, (n + 1) * sizeof *names);
    items = (mn_str *)mn_arena_alloc(rd->ar, (n + 1) * sizeof *items);
    if (names == NULL || items == NULL) {
        return -1;
    }
    for (i = 0; i < n; i++) {
        const mncf_toml *x = v->u.a.items[i];
        if (!mn_toml_is(x, MNCF_TOML_STRING) || !rd->item(x->u.s.p, x->u.s.n)) {
            return 0;
        }
        names[i] = x->u.s.p;
        items[i].p = x->u.s.p;
        items[i].n = x->u.s.n;
    }
    twice = up_twice(&rd->ar->alloc, items, n);
    if (twice != 0) {
        return twice < 0 ? -1 : 0;
    }
    *out = names;
    *count = n;
    *strs = items;
    return 1;
}

/* One change entry: 1, 0 where it breaks a rule of section 17.1, -1 on no memory. */
static int up_release(up_reader *rd, const mncf_toml *e, mncf_release *r)
{
    static const char *const LISTS[] = {"added", "fixed", "removed"};
    const mncf_toml *v = mn_toml_getz(e, "version");
    mn_str *strs[3] = {NULL, NULL, NULL};
    size_t counts[3] = {0, 0, 0}, i, k, total;
    size_t slot = 0;
    int got;
    if (!mn_toml_is(v, MNCF_TOML_STRING) || !mn_version_valid(v->u.s.p, v->u.s.n) ||
        mn_version_cmp(v->u.s.p, v->u.s.n, rd->file_version.p, rd->file_version.n) > 0 ||
        up_seen(rd, v->u.s.p, v->u.s.n, &slot) != NULL) {
        return 0;
    }
    memset(r, 0, sizeof *r);
    r->version = v->u.s.p;
    v = mn_toml_getz(e, "date");
    if (v != NULL) {
        if (!mn_toml_is(v, MNCF_TOML_STRING) || !mn_date_valid(v->u.s.p, v->u.s.n)) {
            return 0;
        }
        r->date = v->u.s.p;
    }
    v = mn_toml_getz(e, "text");
    if (v != NULL) {
        if (!mn_toml_is(v, MNCF_TOML_STRING)) {
            return 0;
        }
        r->text = v->u.s.p;
    }
    for (i = 0; i < 3; i++) {
        const char *const **list = i == 0 ? &r->added : i == 1 ? &r->fixed : &r->removed;
        size_t *count = i == 0 ? &r->added_count : i == 1 ? &r->fixed_count : &r->removed_count;
        v = mn_toml_getz(e, LISTS[i]);
        if (v != NULL) {
            got = up_names(rd, v, list, count, &strs[i]);
            if (got <= 0) {
                return got;
            }
            counts[i] = *count;
        }
    }
    /* An id is in at most one of the lists. Each list has its names once, so a name twice in all three
     * is one in two lists. */
    total = counts[0] + counts[1] + counts[2];
    if (total > 1) {
        mn_str *all = (mn_str *)mn_arena_alloc(rd->ar, total * sizeof *all);
        if (all == NULL) {
            return -1;
        }
        for (i = 0, k = 0; i < 3; i++) {
            if (counts[i] > 0) {
                memcpy(all + k, strs[i], counts[i] * sizeof *all);
                k += counts[i];
            }
        }
        got = up_twice(&rd->ar->alloc, all, total);
        if (got != 0) {
            return got < 0 ? -1 : 0;
        }
    }
    v = mn_toml_getz(e, "renamed");
    if (v != NULL) {
        size_t n;
        mncf_name_pair *pairs;
        mn_str *to, *from;
        if (!mn_toml_is(v, MNCF_TOML_TABLE)) {
            return 0;
        }
        n = v->u.t.n;
        pairs = (mncf_name_pair *)mn_arena_alloc(rd->ar, (n + 1) * sizeof *pairs);
        to = (mn_str *)mn_arena_alloc(rd->ar, (n + 1) * sizeof *to);
        from = (mn_str *)mn_arena_alloc(rd->ar, (n + counts[2] + 1) * sizeof *from);
        if (pairs == NULL || to == NULL || from == NULL) {
            return -1;
        }
        for (i = 0; i < n; i++) {
            const struct mn_entry *en = &v->u.t.e[i];
            if (!rd->item(en->key.p, en->key.n) || !mn_toml_is(en->val, MNCF_TOML_STRING) ||
                !rd->item(en->val->u.s.p, en->val->u.s.n)) {
                return 0;
            }
            pairs[i].from = en->key.p;
            pairs[i].to = en->val->u.s.p;
            from[i] = en->key;
            to[i].p = en->val->u.s.p;
            to[i].n = en->val->u.s.n;
        }
        /* Two cheats are not renamed to the same id, and a removed cheat is not renamed: the keys of a
         * table are each there once, and so are the removed ids. */
        got = up_twice(&rd->ar->alloc, to, n);
        if (got == 0 && counts[2] > 0) {
            memcpy(from + n, strs[2], counts[2] * sizeof *from);
            got = up_twice(&rd->ar->alloc, from, n + counts[2]);
        }
        if (got != 0) {
            return got < 0 ? -1 : 0;
        }
        r->renamed = pairs;
        r->renamed_count = n;
    }
    rd->slots[slot] = rd->count + 1;
    return 1;
}

/* Newest first. The versions of the entries differ, so the order is the same whatever the sort. */
static int up_cmp_release(const void *a, const void *b)
{
    const mncf_release *x = (const mncf_release *)a, *y = (const mncf_release *)b;
    return mn_version_cmp(y->version, strlen(y->version), x->version, strlen(x->version));
}

static int up_changes(up_reader *rd, const mncf_toml *entries, mn_warnings *w, mn_update_keys *out)
{
    size_t n = entries->u.a.n, i, k;
    rd->list = (mncf_release *)mn_arena_zalloc(rd->ar, (n + 1) * sizeof *rd->list);
    if (rd->list == NULL) {
        return 0;
    }
    rd->slot_cap = 16;
    while (rd->slot_cap < n * 2) {
        rd->slot_cap *= 2;
    }
    rd->slots = (size_t *)mn_malloc(&rd->ar->alloc, rd->slot_cap * sizeof *rd->slots);
    if (rd->slots == NULL) {
        rd->ar->failed = 1;
        return 0;
    }
    memset(rd->slots, 0, rd->slot_cap * sizeof *rd->slots);
    for (i = 0; i < n; i++) {
        const mncf_toml *e = entries->u.a.items[i];
        int got;
        for (k = 0; k < e->u.t.n; k++) {
            int known = 0;
            size_t j;
            for (j = 0; j < sizeof UP_CHANGE_KEYS / sizeof UP_CHANGE_KEYS[0]; j++) {
                known |= mn_streqz(e->u.t.e[k].key.p, e->u.t.e[k].key.n, UP_CHANGE_KEYS[j]);
            }
            if (!known && !mn_warn(rd->ar, w, "unknown-key:changes.%s", e->u.t.e[k].key.p)) {
                goto nomem;
            }
        }
        got = up_release(rd, e, &rd->list[rd->count]);
        if (got < 0) {
            goto nomem;
        }
        if (got > 0) {
            rd->count++;
        } else {
            const mncf_toml *v = mn_toml_getz(e, "version");
            int ok = mn_toml_is(v, MNCF_TOML_STRING) ? mn_warn(rd->ar, w, "changes:%s", v->u.s.p)
                                                     : mn_warn(rd->ar, w, "changes:%lu", (unsigned long)(i + 1));
            if (!ok) {
                goto nomem;
            }
        }
    }
    mn_free(&rd->ar->alloc, rd->slots, rd->slot_cap * sizeof *rd->slots);
    if (rd->count > 1) {
        qsort(rd->list, rd->count, sizeof *rd->list, up_cmp_release);
    }
    out->changes = rd->list;
    out->change_count = rd->count;
    return 1;
nomem:
    mn_free(&rd->ar->alloc, rd->slots, rd->slot_cap * sizeof *rd->slots);
    return 0;
}

int mn_read_update_keys(mn_arena *ar, const mncf_toml *doc, int (*item)(const char *s, size_t n), mn_warnings *w,
                        mn_update_keys *out)
{
    const mncf_toml *v, *version = mn_toml_getz(doc, "file_version");
    int versioned = mn_toml_is(version, MNCF_TOML_STRING) && mn_version_valid(version->u.s.p, version->u.s.n);
    size_t i;
    memset(out, 0, sizeof *out);
    v = mn_toml_getz(doc, "id");
    if (v != NULL) {
        if (mn_toml_is(v, MNCF_TOML_STRING) && mn_file_id_valid(v->u.s.p, v->u.s.n)) {
            out->id = v->u.s.p;
        } else if (!mn_warn(ar, w, "id")) {
            return 0;
        }
    }
    v = mn_toml_getz(doc, "update");
    if (v != NULL) {
        const mncf_toml *url = NULL;
        size_t start = 0, host = 0;
        if (mn_toml_is(v, MNCF_TOML_TABLE)) {
            for (i = 0; i < v->u.t.n; i++) {
                if (!mn_streqz(v->u.t.e[i].key.p, v->u.t.e[i].key.n, "url") &&
                    !mn_warn(ar, w, "unknown-key:update.%s", v->u.t.e[i].key.p)) {
                    return 0;
                }
            }
            url = mn_toml_getz(v, "url");
            if (mn_toml_is(url, MNCF_TOML_STRING)) {
                host = mn_url_host(url->u.s.p, url->u.s.n, &start);
            }
        }
        if (host == 0 || out->id == NULL || !versioned) {
            if (!mn_warn(ar, w, "update")) {
                return 0;
            }
        } else {
            out->update_url = url->u.s.p;
            out->update_host = mn_arena_strndup(ar, url->u.s.p + start, host);
            if (out->update_host == NULL) {
                return 0;
            }
        }
    }
    v = mn_toml_getz(doc, "changes");
    if (v != NULL) {
        int good = mn_toml_is(v, MNCF_TOML_ARRAY);
        for (i = 0; good && i < v->u.a.n; i++) {
            good = mn_toml_is(v->u.a.items[i], MNCF_TOML_TABLE);
        }
        if (good && v->u.a.n > 0 && !versioned) {
            good = 0;
        }
        if (!good) {
            return mn_warn(ar, w, "changes");
        }
        if (v->u.a.n > 0) {
            up_reader rd;
            memset(&rd, 0, sizeof rd);
            rd.ar = ar;
            rd.item = item;
            rd.file_version = version->u.s;
            if (!up_changes(&rd, v, w, out)) {
                return 0;
            }
        }
    }
    return 1;
}

/* ---- What to show ----------------------------------------------------------------------------------- */

size_t mncf_releases_since(const mncf_release *changes, size_t count, const char *version)
{
    size_t i, vn;
    if (changes == NULL) {
        return 0;
    }
    vn = version != NULL ? strlen(version) : 0;
    for (i = 0; i < count; i++) {
        if (!up_above(changes[i].version, strlen(changes[i].version), version, vn)) {
            break;
        }
    }
    return i;
}

const char *mncf_release_text(const mncf_file *file, const mncf_release *release, const char *language)
{
    if (release == NULL) {
        return NULL;
    }
    if (file != NULL && language != NULL && language[0] != 0 && file->info.i18n != NULL) {
        const mncf_toml *tr = mn_toml_getz(file->info.i18n, language);
        const mncf_toml *entry = mn_toml_getz(mn_toml_getz(tr, "changes"), release->version);
        const mncf_toml *text = mn_toml_getz(entry, "text");
        if (mn_toml_is(text, MNCF_TOML_STRING)) {
            return text->u.s.p;
        }
    }
    return release->text;
}

/* ---- A new version (section 17.2) ------------------------------------------------------------------- */

struct mncf_update {
    mn_alloc alloc;
    mn_arena arena;
    mncf_update_info info;
    mncf_file *file;
    mncf_pack *pack;
};

/* What the steps of section 17.2 look at, of a file or a pack. */
typedef struct up_side {
    const mncf_signature *sig;
    const char *id;
    mn_str version;             /* file_version as the document has it; p NULL where it has none */
    const char *platform;       /* NULL for a pack, which has no game */
    const char *game_id;
} up_side;

static mn_str up_version_of(const mncf_toml *doc)
{
    const mncf_toml *v = mn_toml_getz(doc, "file_version");
    mn_str s;
    s.p = NULL;
    s.n = 0;
    if (mn_toml_is(v, MNCF_TOML_STRING)) {
        s = v->u.s;
    }
    return s;
}

static void up_file_side(const mncf_file *f, up_side *out)
{
    out->sig = &f->info.signature;
    out->id = f->info.id;
    out->version = up_version_of(f->info.document);
    out->platform = f->info.platform;
    out->game_id = f->info.game_id;
}

static void up_pack_side(const mncf_pack *p, up_side *out)
{
    out->sig = &p->info.signature;
    out->id = p->info.id;
    out->version = up_version_of(p->doc);
    out->platform = NULL;
    out->game_id = NULL;
}

static int up_lower_eq(const char *a, const char *b)
{
    for (;; a++, b++) {
        int x = (unsigned char)*a, y = (unsigned char)*b;
        if (x >= 'A' && x <= 'Z') {
            x += 'a' - 'A';
        }
        if (y >= 'A' && y <= 'Z') {
            y += 'a' - 'A';
        }
        if (x != y) {
            return 0;
        }
        if (x == 0) {
            return 1;
        }
    }
}

/* Step 6: whether a signature line of the new file verifies with a key that signed the old one. A line of
 * the old file that could not be checked counts by its key id. */
static int up_signed_by(const mncf_signature *old, const mncf_signature *fresh)
{
    size_t i, k;
    if (old->result == MNCF_SIGNATURE_UNSIGNED) {
        return 1;
    }
    for (i = 0; i < old->key_id_count; i++) {
        const uint8_t *key = NULL;
        for (k = 0; k < old->verified_count && key == NULL; k++) {
            if (strcmp(old->verified_ids[k], old->key_ids[i]) == 0) {
                key = old->verified_keys[k];
            }
        }
        for (k = 0; k < fresh->verified_count; k++) {
            if (key != NULL ? memcmp(fresh->verified_keys[k], key, 32) == 0
                            : strcmp(fresh->verified_ids[k], old->key_ids[i]) == 0) {
                return 1;
            }
        }
    }
    return 0;
}

static const char *up_steps(const up_side *old, const up_side *fresh)
{
    if (fresh->sig->result == MNCF_SIGNATURE_INVALID || fresh->sig->result == MNCF_SIGNATURE_REVOKED) {
        return "update:signature";
    }
    if (old->platform != NULL &&
        (strcmp(old->platform, fresh->platform) != 0 || !up_lower_eq(old->game_id, fresh->game_id))) {
        return "update:game";
    }
    if (old->id == NULL || fresh->id == NULL || strcmp(old->id, fresh->id) != 0) {
        return "update:id";
    }
    if (!up_above(fresh->version.p, fresh->version.n, old->version.p, old->version.n)) {
        return "update:version";
    }
    if (!up_signed_by(old->sig, fresh->sig)) {
        return "update:signer";
    }
    return "";
}

static mncf_update *up_update_new(const mncf_allocator *alloc, const void *installed, const void *data, size_t size,
                                  mncf_error *err)
{
    mn_alloc a = mn_alloc_of(alloc);
    mncf_update *u;
    mn_error_clear(err);
    if (installed == NULL || (data == NULL && size > 0)) {
        mn_error(err, MNCF_ERR_ARGUMENT, "argument", "no installed file or no bytes given");
        return NULL;
    }
    u = (mncf_update *)mn_calloc(&a, sizeof *u);
    if (u == NULL) {
        mn_error_nomem(err);
        return NULL;
    }
    u->alloc = a;
    mn_arena_init(&u->arena, a);
    return u;
}

/* The result once the new side was read: the reason for a side that did not load, or the steps. */
static mncf_update *up_update_done(mncf_update *u, const mncf_error *read_err, const up_side *old,
                                   const up_side *fresh, const char *to_version, mncf_error *err)
{
    if (fresh == NULL) {
        if (read_err->status == MNCF_ERR_MEMORY) {
            mncf_update_free(u);
            mn_error_nomem(err);
            return NULL;
        }
        u->info.reason = mn_arena_printf(&u->arena, "update:error:%s", read_err->code);
    } else {
        u->info.reason = up_steps(old, fresh);
        u->info.to_version = mn_arena_strdup(&u->arena, to_version);
    }
    if (u->arena.failed) {
        mncf_update_free(u);
        mn_error_nomem(err);
        return NULL;
    }
    return u;
}

mncf_update *mncf_update_check(const mncf_file *installed, const void *data, size_t size, const mncf_trust *trust,
                               const mncf_allocator *alloc, mncf_error *err)
{
    mncf_update *u = up_update_new(alloc, installed, data, size, err);
    mncf_error read_err;
    up_side old, fresh;
    if (u == NULL) {
        return NULL;
    }
    u->info.from_version = mn_arena_strdup(&u->arena, installed->info.file_version);
    u->file = mncf_load(data, size, trust, alloc, &read_err);
    if (u->file == NULL) {
        return up_update_done(u, &read_err, NULL, NULL, NULL, err);
    }
    u->info.file = u->file;
    up_file_side(installed, &old);
    up_file_side(u->file, &fresh);
    return up_update_done(u, &read_err, &old, &fresh, u->file->info.file_version, err);
}

mncf_update *mncf_pack_update_check(const mncf_pack *installed, const void *data, size_t size,
                                    const mncf_trust *trust, const mncf_allocator *alloc, mncf_error *err)
{
    mncf_update *u = up_update_new(alloc, installed, data, size, err);
    mncf_error read_err;
    up_side old, fresh;
    if (u == NULL) {
        return NULL;
    }
    u->info.from_version = mn_arena_strdup(&u->arena, installed->info.file_version);
    u->pack = mncf_pack_read(data, size, trust, alloc, &read_err);
    if (u->pack == NULL) {
        return up_update_done(u, &read_err, NULL, NULL, NULL, err);
    }
    u->info.pack = u->pack;
    up_pack_side(installed, &old);
    up_pack_side(u->pack, &fresh);
    return up_update_done(u, &read_err, &old, &fresh, u->pack->info.file_version, err);
}

const mncf_update_info *mncf_update_info_of(const mncf_update *u)
{
    return u != NULL ? &u->info : NULL;
}

mncf_file *mncf_update_take_file(mncf_update *u)
{
    mncf_file *f;
    if (u == NULL) {
        return NULL;
    }
    f = u->file;
    u->file = NULL;
    u->info.file = NULL;
    return f;
}

mncf_pack *mncf_update_take_pack(mncf_update *u)
{
    mncf_pack *p;
    if (u == NULL) {
        return NULL;
    }
    p = u->pack;
    u->pack = NULL;
    u->info.pack = NULL;
    return p;
}

void mncf_update_free(mncf_update *u)
{
    mn_alloc a;
    if (u == NULL) {
        return;
    }
    a = u->alloc;
    mncf_free(u->file);
    mncf_pack_free(u->pack);
    mn_arena_free(&u->arena);
    mn_free(&a, u, sizeof *u);
}

/* ---- Following the ids (section 17.5) --------------------------------------------------------------- */

/* A change entry above the version the player had, with its removed and renamed sorted for lookups. */
typedef struct up_step {
    const char **removed;
    size_t removed_count;
    const mncf_name_pair **renamed;     /* by from */
    size_t renamed_count;
} up_step;

static int up_cmp_z(const void *a, const void *b)
{
    return strcmp(*(const char *const *)a, *(const char *const *)b);
}

static int up_cmp_pair(const void *a, const void *b)
{
    return strcmp((*(const mncf_name_pair *const *)a)->from, (*(const mncf_name_pair *const *)b)->from);
}

/* The entries above `version`, oldest first, in the arena tmp. 0 on no memory. */
static int up_steps_of(mn_arena *tmp, const mncf_release *changes, size_t count, const char *version, up_step **out,
                       size_t *n)
{
    size_t since = mncf_releases_since(changes, count, version), i, k;
    up_step *steps = (up_step *)mn_arena_zalloc(tmp, (since + 1) * sizeof *steps);
    if (steps == NULL) {
        return 0;
    }
    for (i = 0; i < since; i++) {
        const mncf_release *r = &changes[since - 1 - i];
        up_step *s = &steps[i];
        s->removed = (const char **)mn_arena_alloc(tmp, (r->removed_count + 1) * sizeof *s->removed);
        s->renamed = (const mncf_name_pair **)mn_arena_alloc(tmp, (r->renamed_count + 1) * sizeof *s->renamed);
        if (s->removed == NULL || s->renamed == NULL) {
            return 0;
        }
        for (k = 0; k < r->removed_count; k++) {
            s->removed[k] = r->removed[k];
        }
        for (k = 0; k < r->renamed_count; k++) {
            s->renamed[k] = &r->renamed[k];
        }
        s->removed_count = r->removed_count;
        s->renamed_count = r->renamed_count;
        qsort(s->removed, s->removed_count, sizeof *s->removed, up_cmp_z);
        qsort(s->renamed, s->renamed_count, sizeof *s->renamed, up_cmp_pair);
    }
    *out = steps;
    *n = since;
    return 1;
}

/* What a name becomes in one entry: NULL where it is removed, its new name where it is renamed, else
 * itself. */
static const char *up_step_name(const up_step *s, const char *name)
{
    mncf_name_pair key;
    const mncf_name_pair *kp = &key;
    const mncf_name_pair *const *found;
    if (s->removed_count > 0 && bsearch(&name, s->removed, s->removed_count, sizeof *s->removed, up_cmp_z) != NULL) {
        return NULL;
    }
    key.from = name;
    key.to = NULL;
    found = s->renamed_count > 0 ? (const mncf_name_pair *const *)bsearch(&kp, s->renamed, s->renamed_count,
                                                                          sizeof *s->renamed, up_cmp_pair)
                                 : NULL;
    return found != NULL ? (*found)->to : name;
}

/* The name that a name of the version the player had has after the entries, or NULL where it is gone. */
static const char *up_follow(const up_step *steps, size_t n, const char *name)
{
    size_t i;
    for (i = 0; i < n && name != NULL; i++) {
        name = up_step_name(&steps[i], name);
    }
    return name;
}

/* The file's own string for a cheat id, or NULL where the file has no such cheat. */
static const char *up_cheat(const mncf_file *f, const char *id)
{
    size_t i;
    if (id == NULL) {
        return NULL;
    }
    i = mn_cheat_index(f, id, strlen(id));
    return i == (size_t)-1 ? NULL : f->info.cheats[i].id;
}

mncf_status mncf_ids(const mncf_file *file, const char *version, const char *const *names, size_t count,
                     const char **out, mncf_error *err)
{
    mn_arena tmp;
    up_step *steps;
    size_t n, i;
    mn_error_clear(err);
    if (file == NULL || (count > 0 && (names == NULL || out == NULL))) {
        mn_error(err, MNCF_ERR_ARGUMENT, "argument", "no file, names or room for the ids");
        return MNCF_ERR_ARGUMENT;
    }
    mn_arena_init(&tmp, file->alloc);
    if (!up_steps_of(&tmp, file->info.changes, file->info.change_count, version, &steps, &n)) {
        mn_arena_free(&tmp);
        mn_error_nomem(err);
        return MNCF_ERR_MEMORY;
    }
    for (i = 0; i < count; i++) {
        out[i] = names[i] != NULL ? up_cheat(file, up_follow(steps, n, names[i])) : NULL;
    }
    mn_arena_free(&tmp);
    return MNCF_OK;
}

/* ---- Marks (section 17.4) --------------------------------------------------------------------------- */

/* A growable list of names on an allocator, for sets that are sorted and made unique after each entry. */
typedef struct up_list {
    const char **items;
    size_t count, cap;
} up_list;

static int up_push(const mn_alloc *a, up_list *l, const char *s)
{
    if (l->count == l->cap) {
        size_t cap = l->cap == 0 ? 16 : l->cap * 2;
        const char **grown;
        if (cap < l->cap || cap > (size_t)-1 / sizeof *grown) {
            return 0;
        }
        grown = (const char **)mn_realloc(a, (void *)l->items, l->cap * sizeof *grown, cap * sizeof *grown);
        if (grown == NULL) {
            return 0;
        }
        l->items = grown;
        l->cap = cap;
    }
    l->items[l->count++] = s;
    return 1;
}

static void up_list_free(const mn_alloc *a, up_list *l)
{
    mn_free(a, (void *)l->items, l->cap * sizeof *l->items);
    memset(l, 0, sizeof *l);
}

static void up_unique(up_list *l)
{
    size_t i, k = 0;
    if (l->count > 1) {
        qsort(l->items, l->count, sizeof *l->items, up_cmp_z);
    }
    for (i = 0; i < l->count; i++) {
        if (k == 0 || strcmp(l->items[k - 1], l->items[i]) != 0) {
            l->items[k++] = l->items[i];
        }
    }
    l->count = k;
}

static int up_has(const up_list *l, const char *s)
{
    return l->count > 0 && bsearch(&s, l->items, l->count, sizeof *l->items, up_cmp_z) != NULL;
}

/* One entry for a set of marks: what is in it follows the entry's removed and renamed, and the names of
 * the list `add` join it. 0 on no memory. */
static int up_mark_step(const mn_alloc *a, const up_step *s, const char *const *add, size_t add_count, up_list *cur,
                        up_list *next)
{
    size_t i;
    next->count = 0;
    for (i = 0; i < cur->count; i++) {
        const char *name = up_step_name(s, cur->items[i]);
        if (name != NULL && !up_push(a, next, name)) {
            return 0;
        }
    }
    for (i = 0; i < add_count; i++) {
        if (!up_push(a, next, add[i])) {
            return 0;
        }
    }
    up_unique(next);
    return 1;
}

typedef struct up_marks {
    mncf_marks pub;             /* first, so that a pointer to it is one to the whole */
    mn_alloc alloc;
    mn_arena arena;
} up_marks;

void mncf_marks_free(mncf_marks *found)
{
    up_marks *m = (up_marks *)found;
    mn_alloc a;
    if (m == NULL) {
        return;
    }
    a = m->alloc;
    mn_arena_free(&m->arena);
    mn_free(&a, m, sizeof *m);
}

mncf_marks *mncf_mark(const mncf_file *file, const char *version, mncf_error *err)
{
    up_marks *m;
    mn_alloc a;
    mn_arena tmp;
    up_step *steps;
    up_list lists[4];
    up_list *added = &lists[0], *fixed = &lists[1], *spare_added = &lists[2], *spare_fixed = &lists[3];
    size_t n = 0, i;
    const char **new_ids, **fixed_ids;
    int ok = 1;
    mn_error_clear(err);
    if (file == NULL) {
        mn_error(err, MNCF_ERR_ARGUMENT, "argument", "no file given");
        return NULL;
    }
    a = file->alloc;
    m = (up_marks *)mn_calloc(&a, sizeof *m);
    if (m == NULL) {
        mn_error_nomem(err);
        return NULL;
    }
    m->alloc = a;
    mn_arena_init(&m->arena, a);
    mn_arena_init(&tmp, a);
    memset(lists, 0, sizeof lists);
    ok = up_steps_of(&tmp, file->info.changes, file->info.change_count, version, &steps, &n);
    for (i = 0; ok && i < n; i++) {
        const mncf_release *r = &file->info.changes[n - 1 - i];
        up_list *t;
        ok = up_mark_step(&a, &steps[i], r->added, r->added_count, added, spare_added) &&
             up_mark_step(&a, &steps[i], r->fixed, r->fixed_count, fixed, spare_fixed);
        t = added;
        added = spare_added;
        spare_added = t;
        t = fixed;
        fixed = spare_fixed;
        spare_fixed = t;
    }
    new_ids = (const char **)mn_arena_alloc(&m->arena, (file->info.cheat_count + 1) * sizeof *new_ids);
    fixed_ids = (const char **)mn_arena_alloc(&m->arena, (file->info.cheat_count + 1) * sizeof *fixed_ids);
    if (ok && new_ids != NULL && fixed_ids != NULL) {
        for (i = 0; i < file->info.cheat_count; i++) {
            const char *id = file->info.cheats[i].id;
            if (up_has(added, id)) {
                new_ids[m->pub.new_count++] = id;
            } else if (up_has(fixed, id)) {
                fixed_ids[m->pub.fixed_count++] = id;
            }
        }
        m->pub.new_ids = new_ids;
        m->pub.fixed_ids = fixed_ids;
    }
    for (i = 0; i < 4; i++) {
        up_list_free(&a, &lists[i]);
    }
    mn_arena_free(&tmp);
    if (!ok || new_ids == NULL || fixed_ids == NULL || m->arena.failed) {
        mncf_marks_free(&m->pub);
        mn_error_nomem(err);
        return NULL;
    }
    return &m->pub;
}

/* ---- What the player set (section 17.5) ------------------------------------------------------------- */

typedef struct up_carried {
    mncf_carried pub;           /* first, so that a pointer to it is one to the whole */
    mn_alloc alloc;
    mn_arena arena;
    /* the arrays of pub while they grow */
    const char **on, **off, **dropped, **gone;
    mncf_choice_value *values;
    mncf_name_pair *ids;
    mncf_value_reset *reset;
    size_t on_cap, off_cap, dropped_cap, gone_cap, value_cap, id_cap, reset_cap;
} up_carried;

void mncf_carried_free(mncf_carried *carried)
{
    up_carried *c = (up_carried *)carried;
    mn_alloc a;
    if (c == NULL) {
        return;
    }
    a = c->alloc;
    mn_arena_free(&c->arena);
    mn_free(&a, c, sizeof *c);
}

/* The new id of a cheat id seen, from the ids found; NULL where it is gone. */
static const char *up_new_id(const up_carried *c, const char *id, size_t n)
{
    size_t i;
    for (i = 0; i < c->pub.id_count; i++) {
        if (mn_streqz(id, n, c->ids[i].from)) {
            return c->ids[i].to;
        }
    }
    return NULL;
}

/* Notes a cheat id the first time it is seen, with where it is in the new version. 0 on no memory. */
static int up_see(up_carried *c, const mncf_file *f, const up_step *steps, size_t n, const char *id, size_t len)
{
    size_t i;
    char *from;
    const char *to;
    for (i = 0; i < c->pub.id_count; i++) {
        if (mn_streqz(id, len, c->ids[i].from)) {
            return 1;
        }
    }
    from = mn_arena_strndup(&c->arena, id, len);
    if (from == NULL || MN_GROW(&c->arena, c->ids, c->pub.id_count, c->id_cap) == NULL) {
        return 0;
    }
    to = up_cheat(f, up_follow(steps, n, from));
    c->ids[c->pub.id_count].from = from;
    c->ids[c->pub.id_count].to = to != NULL ? mn_arena_strdup(&c->arena, to) : NULL;
    if (to != NULL && c->ids[c->pub.id_count].to == NULL) {
        return 0;
    }
    if (to == NULL) {
        if (MN_GROW(&c->arena, c->dropped, c->pub.dropped_count, c->dropped_cap) == NULL) {
            return 0;
        }
        c->dropped[c->pub.dropped_count++] = from;
    }
    c->pub.id_count++;
    return 1;
}

/* Adds a new id to the cheats switched on or off, once. 0 on no memory. */
static int up_switch(mn_arena *ar, const char ***list, size_t *count, size_t *cap, const char *id)
{
    size_t i;
    if (id == NULL) {
        return 1;
    }
    for (i = 0; i < *count; i++) {
        if (strcmp((*list)[i], id) == 0) {
            return 1;
        }
    }
    if (MN_GROW(ar, *list, *count, *cap) == NULL) {
        return 0;
    }
    (*list)[(*count)++] = id;
    return 1;
}

/* The length of the cheat id in a value key "cheat.value": up to the first dot. */
static size_t up_cheat_part(const char *key)
{
    const char *dot = strchr(key, '.');
    return dot != NULL ? (size_t)(dot - key) : strlen(key);
}

static const mncf_value_spec *up_value_spec(const mncf_cheat *ch, const char *vid)
{
    size_t i;
    for (i = 0; i < ch->value_count; i++) {
        if (strcmp(ch->values[i].id, vid) == 0) {
            return &ch->values[i];
        }
    }
    return NULL;
}

/* One value the player chose: kept under its new key where it fits, else back at its default, or gone. */
static int up_carry_value(up_carried *c, const mncf_file *f, const mncf_choice_value *cv)
{
    static const char *const WHY[] = {"", "type", "range", "choice"};
    size_t cn = up_cheat_part(cv->key), i;
    const char *to = up_new_id(c, cv->key, cn);
    const char *vid = cv->key[cn] == '.' ? cv->key + cn + 1 : "";
    const mncf_value_spec *spec;
    char *key;
    int why;
    if (to == NULL) {
        return 1;
    }
    spec = up_value_spec(mncf_file_cheat(f, to), vid);
    if (spec == NULL) {
        key = mn_arena_strdup(&c->arena, cv->key);
        if (key == NULL || MN_GROW(&c->arena, c->gone, c->pub.gone_count, c->gone_cap) == NULL) {
            return 0;
        }
        c->gone[c->pub.gone_count++] = key;
        return 1;
    }
    key = mn_arena_printf(&c->arena, "%s.%s", to, vid);
    if (key == NULL) {
        return 0;
    }
    for (i = 0; i < c->pub.choices.value_count; i++) {
        if (strcmp(c->values[i].key, key) == 0) {
            return 1;
        }
    }
    for (i = 0; i < c->pub.reset_count; i++) {
        if (strcmp(c->reset[i].key, key) == 0) {
            return 1;
        }
    }
    why = mn_value_fit(spec, &cv->value);
    if (why != MN_FIT_OK) {
        if (MN_GROW(&c->arena, c->reset, c->pub.reset_count, c->reset_cap) == NULL) {
            return 0;
        }
        c->reset[c->pub.reset_count].key = key;
        c->reset[c->pub.reset_count].why = WHY[why];
        c->pub.reset_count++;
        return 1;
    }
    if (MN_GROW(&c->arena, c->values, c->pub.choices.value_count, c->value_cap) == NULL) {
        return 0;
    }
    c->values[c->pub.choices.value_count].key = key;
    c->values[c->pub.choices.value_count].value = cv->value;
    c->pub.choices.value_count++;
    return 1;
}

/* Whether a value key is given again later in the choices, so that the last one counts. */
static int up_later(const mncf_choices *ch, size_t i)
{
    size_t k;
    for (k = i + 1; k < ch->value_count; k++) {
        if (ch->values[k].key != NULL && strcmp(ch->values[k].key, ch->values[i].key) == 0) {
            return 1;
        }
    }
    return 0;
}

static int up_carry_all(up_carried *c, const mncf_file *f, const up_step *steps, size_t n, const mncf_choices *ch,
                        const char *chosen, const char *const *kept, size_t kept_count)
{
    size_t i;
    /* Every cheat id seen, in the order of on, off, the values and kept. */
    for (i = 0; i < ch->on_count; i++) {
        if (ch->on[i] != NULL && !up_see(c, f, steps, n, ch->on[i], strlen(ch->on[i]))) {
            return 0;
        }
    }
    for (i = 0; i < ch->off_count; i++) {
        if (ch->off[i] != NULL && !up_see(c, f, steps, n, ch->off[i], strlen(ch->off[i]))) {
            return 0;
        }
    }
    for (i = 0; i < ch->value_count; i++) {
        const char *key = ch->values[i].key;
        if (key != NULL && !up_see(c, f, steps, n, key, up_cheat_part(key))) {
            return 0;
        }
    }
    for (i = 0; i < kept_count; i++) {
        if (kept[i] != NULL && !up_see(c, f, steps, n, kept[i], strlen(kept[i]))) {
            return 0;
        }
    }
    for (i = 0; i < ch->on_count; i++) {
        if (ch->on[i] != NULL &&
            !up_switch(&c->arena, &c->on, &c->pub.choices.on_count, &c->on_cap,
                       up_new_id(c, ch->on[i], strlen(ch->on[i])))) {
            return 0;
        }
    }
    for (i = 0; i < ch->off_count; i++) {
        if (ch->off[i] != NULL &&
            !up_switch(&c->arena, &c->off, &c->pub.choices.off_count, &c->off_cap,
                       up_new_id(c, ch->off[i], strlen(ch->off[i])))) {
            return 0;
        }
    }
    for (i = 0; i < ch->value_count; i++) {
        if (ch->values[i].key != NULL && !up_later(ch, i) && !up_carry_value(c, f, &ch->values[i])) {
            return 0;
        }
    }
    for (i = 0; chosen != NULL && i < f->info.preset_count; i++) {
        if (strcmp(f->info.presets[i].id, chosen) == 0) {
            c->pub.preset = mn_arena_strdup(&c->arena, chosen);
            return c->pub.preset != NULL;
        }
    }
    return 1;
}

mncf_carried *mncf_carry(const mncf_file *file, const char *version, const mncf_choices *choices, const char *chosen,
                         const char *const *kept, size_t kept_count, mncf_error *err)
{
    static const mncf_choices NONE = {NULL, 0, NULL, 0, NULL, 0};
    up_carried *c;
    mn_alloc a;
    mn_arena tmp;
    up_step *steps;
    size_t n = 0;
    int ok;
    mn_error_clear(err);
    if (file == NULL || (kept_count > 0 && kept == NULL) ||
        (choices != NULL && ((choices->on_count > 0 && choices->on == NULL) ||
                             (choices->off_count > 0 && choices->off == NULL) ||
                             (choices->value_count > 0 && choices->values == NULL)))) {
        mn_error(err, MNCF_ERR_ARGUMENT, "argument", "no file, or a list without its items");
        return NULL;
    }
    a = file->alloc;
    c = (up_carried *)mn_calloc(&a, sizeof *c);
    if (c == NULL) {
        mn_error_nomem(err);
        return NULL;
    }
    c->alloc = a;
    mn_arena_init(&c->arena, a);
    mn_arena_init(&tmp, a);
    ok = up_steps_of(&tmp, file->info.changes, file->info.change_count, version, &steps, &n) &&
         up_carry_all(c, file, steps, n, choices != NULL ? choices : &NONE, chosen, kept, kept_count);
    mn_arena_free(&tmp);
    if (!ok || c->arena.failed) {
        mncf_carried_free(&c->pub);
        mn_error_nomem(err);
        return NULL;
    }
    c->pub.choices.on = c->on;
    c->pub.choices.off = c->off;
    c->pub.choices.values = c->values;
    c->pub.ids = c->ids;
    c->pub.dropped = c->dropped;
    c->pub.gone = c->gone;
    c->pub.reset = c->reset;
    return &c->pub;
}

/* ---- The files of a pack (section 17.6) ------------------------------------------------------------- */

/* A file of a pack: its id and game, where it loads and has an id. */
typedef struct up_identity {
    const char *id;             /* NULL: none */
    const char *platform;
    char *game_id;              /* in lower case */
} up_identity;

/* The identities of a pack's files, in the arena tmp. 0 on no memory. */
static int up_identities(mn_arena *tmp, const mncf_pack *p, up_identity **out)
{
    mncf_allocator alloc;
    size_t i;
    up_identity *ids = (up_identity *)mn_arena_zalloc(tmp, (p->info.file_count + 1) * sizeof *ids);
    if (ids == NULL) {
        return 0;
    }
    alloc.fn = tmp->alloc.fn;
    alloc.ctx = tmp->alloc.ctx;
    for (i = 0; i < p->info.file_count; i++) {
        const mncf_pack_file *pf = &p->info.files[i];
        mncf_error e;
        mncf_file *f;
        if (pf->status != MNCF_PACK_OK) {
            continue;
        }
        f = mncf_load(pf->data, pf->size, NULL, &alloc, &e);
        if (f == NULL) {
            if (e.status == MNCF_ERR_MEMORY) {
                return 0;
            }
            continue;
        }
        if (f->info.id != NULL) {
            char *g;
            ids[i].id = mn_arena_strdup(tmp, f->info.id);
            ids[i].platform = f->info.platform;      /* the platform table's own string */
            ids[i].game_id = g = mn_arena_strdup(tmp, f->info.game_id);
            for (; g != NULL && *g != 0; g++) {
                if (*g >= 'A' && *g <= 'Z') {
                    *g = (char)(*g - 'A' + 'a');
                }
            }
            if (ids[i].id == NULL || ids[i].game_id == NULL) {
                mncf_free(f);
                return 0;
            }
        }
        mncf_free(f);
    }
    *out = ids;
    return 1;
}

static int up_same_identity(const up_identity *a, const up_identity *b)
{
    return a->id != NULL && b->id != NULL && strcmp(a->id, b->id) == 0 && strcmp(a->platform, b->platform) == 0 &&
           strcmp(a->game_id, b->game_id) == 0;
}

typedef struct up_pairs {
    mncf_pairs pub;             /* first, so that a pointer to it is one to the whole */
    mn_alloc alloc;
    mn_arena arena;
} up_pairs;

void mncf_pairs_free(mncf_pairs *pairs)
{
    up_pairs *p = (up_pairs *)pairs;
    mn_alloc a;
    if (p == NULL) {
        return;
    }
    a = p->alloc;
    mn_arena_free(&p->arena);
    mn_free(&a, p, sizeof *p);
}

/* The file of the new pack at a path, or (size_t)-1. */
static size_t up_pack_file(const mncf_pack *p, const char *path)
{
    size_t i;
    for (i = 0; path != NULL && i < p->info.file_count; i++) {
        if (strcmp(p->info.files[i].path, path) == 0) {
            return i;
        }
    }
    return (size_t)-1;
}

static int up_pair_all(up_pairs *out, mn_arena *tmp, const mncf_pack *old_pack, const mncf_pack *new_pack)
{
    size_t no = old_pack->info.file_count, nn = new_pack->info.file_count, i, k, steps_n;
    up_identity *olds, *news;
    up_step *steps;
    size_t *match = (size_t *)mn_arena_alloc(tmp, (no + 1) * sizeof *match);
    unsigned char *taken = (unsigned char *)mn_arena_zalloc(tmp, nn + 1);
    mncf_name_pair *items = (mncf_name_pair *)mn_arena_alloc(&out->arena, (no + 1) * sizeof *items);
    if (match == NULL || taken == NULL || items == NULL || !up_identities(tmp, old_pack, &olds) ||
        !up_identities(tmp, new_pack, &news) ||
        !up_steps_of(tmp, new_pack->info.changes, new_pack->info.change_count, old_pack->info.file_version, &steps,
                     &steps_n)) {
        return 0;
    }
    /* First by id and game. */
    for (i = 0; i < no; i++) {
        match[i] = (size_t)-1;
        for (k = 0; olds[i].id != NULL && k < nn; k++) {
            if (!taken[k] && up_same_identity(&olds[i], &news[k])) {
                match[i] = k;
                taken[k] = 1;
                break;
            }
        }
    }
    /* Then by path, following the renamed of the new pack's entries, where one of the two has no id. */
    for (i = 0; i < no; i++) {
        if (match[i] == (size_t)-1) {
            size_t at = up_pack_file(new_pack, up_follow(steps, steps_n, old_pack->info.files[i].path));
            if (at != (size_t)-1 && !taken[at] && (olds[i].id == NULL || news[at].id == NULL)) {
                match[i] = at;
                taken[at] = 1;
            }
        }
    }
    for (i = 0; i < no; i++) {
        items[i].from = mn_arena_strdup(&out->arena, old_pack->info.files[i].path);
        items[i].to = match[i] != (size_t)-1 ? mn_arena_strdup(&out->arena, new_pack->info.files[match[i]].path) : NULL;
        if (items[i].from == NULL || (match[i] != (size_t)-1 && items[i].to == NULL)) {
            return 0;
        }
    }
    out->pub.items = items;
    out->pub.count = no;
    return 1;
}

mncf_pairs *mncf_pack_pairs(const mncf_pack *old_pack, const mncf_pack *new_pack, mncf_error *err)
{
    up_pairs *p;
    mn_alloc a;
    mn_arena tmp;
    int ok;
    mn_error_clear(err);
    if (old_pack == NULL || new_pack == NULL) {
        mn_error(err, MNCF_ERR_ARGUMENT, "argument", "two packs are needed");
        return NULL;
    }
    a = old_pack->alloc;
    p = (up_pairs *)mn_calloc(&a, sizeof *p);
    if (p == NULL) {
        mn_error_nomem(err);
        return NULL;
    }
    p->alloc = a;
    mn_arena_init(&p->arena, a);
    mn_arena_init(&tmp, a);
    ok = up_pair_all(p, &tmp, old_pack, new_pack);
    mn_arena_free(&tmp);
    if (!ok || p->arena.failed) {
        mncf_pairs_free(&p->pub);
        mn_error_nomem(err);
        return NULL;
    }
    return &p->pub;
}
