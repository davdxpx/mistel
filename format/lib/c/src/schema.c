/* schema.c: checks a parsed file against the spec and builds the model. MIT License.
 *
 * Problems outside a cheat make the whole file unusable or, for keys the spec does not define, a warning.
 * Problems inside a cheat turn only that cheat off: its problem becomes unknown-key:<key>,
 * extension-missing:<name> or invalid:<key>, where <key> is the key's path from the cheat. The checks run
 * in the order of the Python reference, so that the first problem found is the same.
 */
#include "encode.h"
#include "model.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>

static const char *const KNOWN_EXTENSIONS[] = {"raw/1", "arm64-imm/1"};
static const char *const FILE_KEYS[] = {"format", "name",   "authors", "about",  "file_version", "language", "origin",
                                        "extensions", "game", "builds", "cheat", "choices", "preset", "i18n"};

typedef struct schema_ctx {
    mncf_file *f;
    mn_arena *ar;
    mncf_error *err;
    const mn_platform *plat;
    const char *const *listed;    /* required, then optional */
    size_t listed_count;
    const char *unknown_listed;   /* the first listed extension this reader does not know */
    mn_cheat *cheat;
    const char *problem;
    /* the warnings, as they are found */
    const char **warnings;
    size_t warning_count, warning_cap;
} schema_ctx;

#define COUNT(a) (sizeof(a) / sizeof((a)[0]))

/* ---- Values of the document ------------------------------------------------------------------------- */

static int is_type(const mncf_toml *v, int type)
{
    return v != NULL && v->type == type;
}

static int is_str(const mncf_toml *v)
{
    return is_type(v, MNCF_TOML_STRING);
}

static int is_table(const mncf_toml *v)
{
    return is_type(v, MNCF_TOML_TABLE);
}

static int is_array(const mncf_toml *v)
{
    return is_type(v, MNCF_TOML_ARRAY);
}

static int is_int(const mncf_toml *v)
{
    return is_type(v, MNCF_TOML_INTEGER);
}

static int is_number(const mncf_toml *v)
{
    return is_type(v, MNCF_TOML_INTEGER) || is_type(v, MNCF_TOML_FLOAT);
}

static int str_is(const mncf_toml *v, const char *z)
{
    return is_str(v) && mn_streqz(v->u.s.p, v->u.s.n, z);
}

static const mncf_toml *get(const mncf_toml *t, const char *key)
{
    return mn_toml_getz(t, key);
}

static int has(const mncf_toml *t, const char *key)
{
    return mn_toml_getz(t, key) != NULL;
}

static int str_in(const mncf_toml *v, const char *const *names, size_t count)
{
    size_t i;
    if (!is_str(v)) {
        return 0;
    }
    for (i = 0; i < count; i++) {
        if (mn_streqz(v->u.s.p, v->u.s.n, names[i])) {
            return 1;
        }
    }
    return 0;
}

static int key_in(const mn_str *k, const char *const *names, size_t count)
{
    size_t i;
    for (i = 0; i < count; i++) {
        if (mn_streqz(k->p, k->n, names[i])) {
            return 1;
        }
    }
    return 0;
}

/* ^[a-z][a-z0-9-]*\Z */
static int is_id(const char *s, size_t n)
{
    size_t i;
    if (n == 0 || !(s[0] >= 'a' && s[0] <= 'z')) {
        return 0;
    }
    for (i = 1; i < n; i++) {
        char c = s[i];
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-')) {
            return 0;
        }
    }
    return 1;
}

/* ^(x-)?[a-z0-9][a-z0-9-]*\/[0-9]+\Z */
static int is_extension_name(const char *s, size_t n)
{
    size_t i = 0;
    if (n == 0 || !((s[0] >= 'a' && s[0] <= 'z') || (s[0] >= '0' && s[0] <= '9'))) {
        return 0;
    }
    while (i < n && ((s[i] >= 'a' && s[i] <= 'z') || (s[i] >= '0' && s[i] <= '9') || s[i] == '-')) {
        i++;
    }
    if (i >= n || s[i] != '/') {
        return 0;
    }
    i++;
    if (i >= n) {
        return 0;
    }
    for (; i < n; i++) {
        if (!(s[i] >= '0' && s[i] <= '9')) {
            return 0;
        }
    }
    return 1;
}

/* An integer of the file: a TOML integer, or a string of 0x and 1 to 16 hexadecimal digits. */
static int integer_of(const mncf_toml *v, mn_num *out)
{
    if (is_int(v)) {
        *out = mn_num_i64(v->u.i);
        return 1;
    }
    if (is_str(v) && v->u.s.n >= 3 && v->u.s.n <= 18 && v->u.s.p[0] == '0' && v->u.s.p[1] == 'x') {
        uint64_t x = 0;
        size_t i;
        for (i = 2; i < v->u.s.n; i++) {
            int d = mn_hex_value((unsigned char)v->u.s.p[i]);
            if (d < 0) {
                return 0;
            }
            x = x << 4 | (uint64_t)d;
        }
        *out = mn_num_u64(x);
        return 1;
    }
    return 0;
}

/* ^[0-9A-Fa-f]{2}( [0-9A-Fa-f]{2})*\Z as bytes. */
static int bytes_of(mn_arena *ar, const mncf_toml *v, uint8_t **out, size_t *len)
{
    size_t n, i, k;
    const char *s;
    if (!is_str(v)) {
        return 0;
    }
    s = v->u.s.p;
    n = v->u.s.n;
    if (n < 2 || (n + 1) % 3 != 0) {
        return 0;
    }
    for (i = 0; i < n; i++) {
        if (i % 3 == 2) {
            if (s[i] != ' ') {
                return 0;
            }
        } else if (!mn_is_hex((unsigned char)s[i])) {
            return 0;
        }
    }
    *len = (n + 1) / 3;
    *out = (uint8_t *)mn_arena_alloc(ar, *len);
    if (*out == NULL) {
        return 0;
    }
    for (k = 0; k < *len; k++) {
        (*out)[k] = (uint8_t)(mn_hex_value((unsigned char)s[3 * k]) << 4 | mn_hex_value((unsigned char)s[3 * k + 1]));
    }
    return 1;
}

/* A pattern (section 6.3): pairs or ??, at least 4, not starting or ending with ??. */
static int pattern_of(mn_arena *ar, const mncf_toml *v, int16_t **out, size_t *len)
{
    size_t n, i, k;
    const char *s;
    if (!is_str(v)) {
        return 0;
    }
    s = v->u.s.p;
    n = v->u.s.n;
    if (n < 2 || (n + 1) % 3 != 0) {
        return 0;
    }
    for (i = 0; i < n; i += 3) {
        int wild = s[i] == '?' && s[i + 1] == '?';
        if (!wild && !(mn_is_hex((unsigned char)s[i]) && mn_is_hex((unsigned char)s[i + 1]))) {
            return 0;
        }
        if (i + 2 < n && s[i + 2] != ' ') {
            return 0;
        }
    }
    *len = (n + 1) / 3;
    if (*len < 4 || s[0] == '?' || s[n - 1] == '?') {
        return 0;
    }
    *out = (int16_t *)mn_arena_alloc(ar, *len * sizeof **out);
    if (*out == NULL) {
        return 0;
    }
    for (k = 0; k < *len; k++) {
        const char *p = s + 3 * k;
        (*out)[k] = p[0] == '?' ? (int16_t)-1
                                : (int16_t)(mn_hex_value((unsigned char)p[0]) << 4 | mn_hex_value((unsigned char)p[1]));
    }
    return 1;
}

/* A number of the file as a value of type t, where it is one that fits. */
static int value_number(mncf_type t, const mncf_toml *v, mn_num *out)
{
    mn_num n;
    if (t == MNCF_TYPE_BOOL) {
        if (!is_type(v, MNCF_TOML_BOOLEAN)) {
            return 0;
        }
        *out = mn_num_bool(v->u.b);
        return 1;
    }
    if (mn_type_is_int(t)) {
        if (!integer_of(v, &n) || !mn_fits(t, &n)) {
            return 0;
        }
        *out = n;
        return 1;
    }
    if (mn_type_is_float(t)) {
        if (is_int(v)) {
            n = mn_num_i64(v->u.i);
        } else if (is_type(v, MNCF_TOML_FLOAT) && v->u.f == v->u.f) {
            n = mn_num_f64(v->u.f);
        } else {
            return 0;
        }
        if (!mn_fits(t, &n)) {
            return 0;
        }
        *out = n;
        return 1;
    }
    return 0;
}

static int type_in_values(mncf_type t)
{
    return mn_type_is_number(t) || t == MNCF_TYPE_BOOL;
}

static mncf_type type_of(const mncf_toml *v)
{
    return is_str(v) ? mn_type_parse(v->u.s.p, v->u.s.n) : MNCF_TYPE_NONE;
}

/* ---- Choice lists ----------------------------------------------------------------------------------- */

/* ^(-?)(?:0x([0-9A-Fa-f]{1,16})|([0-9]{1,20}))\Z */
static int choice_key(const char *s, size_t n, mn_num *out)
{
    size_t i = 0;
    int negative = 0;
    uint64_t lo = 0;
    int hi = 0;
    if (i < n && s[i] == '-') {
        negative = 1;
        i++;
    }
    if (n - i >= 3 && s[i] == '0' && s[i + 1] == 'x' && n - i - 2 <= 16) {
        size_t j;
        int ok = 1;
        for (j = i + 2; j < n; j++) {
            int d = mn_hex_value((unsigned char)s[j]);
            if (d < 0) {
                ok = 0;
                break;
            }
            lo = lo << 4 | (uint64_t)d;
        }
        if (ok) {
            *out = mn_num_signed(negative, lo);
            return 1;
        }
        lo = 0;
    }
    if (n - i < 1 || n - i > 20) {
        return 0;
    }
    for (; i < n; i++) {
        int d;
        uint64_t low_times_10_hi, carry;
        if (!(s[i] >= '0' && s[i] <= '9')) {
            return 0;
        }
        d = s[i] - '0';
        /* (hi, lo) = (hi, lo) * 10 + d, with lo the low 64 bits */
        low_times_10_hi = (lo >> 32) * 10;
        carry = (low_times_10_hi >> 32);
        {
            uint64_t low_part = (lo & 0xFFFFFFFFu) * 10;
            uint64_t mid = (low_times_10_hi & 0xFFFFFFFFu) + (low_part >> 32);
            uint64_t new_lo = (mid << 32) | (low_part & 0xFFFFFFFFu);
            carry += mid >> 32;
            hi = hi * 10 + (int)carry;
            lo = new_lo + (uint64_t)d;
            if (lo < (uint64_t)d) {
                hi++;
            }
        }
    }
    *out = mn_num_signed(negative, lo);
    out->huge = hi;
    if (hi != 0) {
        out->negative = negative;
    }
    return 1;
}

static size_t num_hash(const mn_num *n)
{
    uint64_t h = n->magnitude * 0x9E3779B97F4A7C15ULL ^ (uint64_t)n->huge * 31 ^ (uint64_t)n->negative;
    return (size_t)(h ^ (h >> 31));
}

/* A table of whole numbers (as strings) to names, as a list with each number once: a later name of the
 * same number replaces the earlier one. 0 where it is not one. */
static int choice_list(mn_arena *ar, const mncf_toml *table, mncf_choice **out, size_t *count)
{
    size_t i, n, cap = 16;
    mncf_choice *items;
    size_t *slots;
    size_t used = 0;
    if (!is_table(table) || table->u.t.n == 0) {
        return 0;
    }
    n = table->u.t.n;
    while (cap < n * 2) {
        cap *= 2;
    }
    items = (mncf_choice *)mn_arena_alloc(ar, n * sizeof *items);
    slots = (size_t *)mn_arena_zalloc(ar, cap * sizeof *slots);
    if (items == NULL || slots == NULL) {
        return 0;
    }
    for (i = 0; i < n; i++) {
        const struct mn_entry *e = &table->u.t.e[i];
        mn_num key;
        size_t at;
        if (!choice_key(e->key.p, e->key.n, &key) || !is_str(e->val)) {
            return 0;
        }
        at = num_hash(&key) & (cap - 1);
        for (;;) {
            if (slots[at] == 0) {
                items[used].key = key;
                items[used].name = e->val->u.s.p;
                slots[at] = ++used;
                break;
            }
            if (mn_num_eq(&items[slots[at] - 1].key, &key) && items[slots[at] - 1].key.huge == key.huge) {
                items[slots[at] - 1].name = e->val->u.s.p;
                break;
            }
            at = (at + 1) & (cap - 1);
        }
    }
    *out = items;
    *count = used;
    return 1;
}

int mn_choices_have(const mncf_value_spec *s, const mn_num *n)
{
    size_t i;
    for (i = 0; i < s->choice_count; i++) {
        if (mn_num_eq(&s->choices[i].key, n)) {
            return 1;
        }
    }
    return 0;
}

int mn_value_in_spec(const mncf_value_spec *s, const mn_num *n)
{
    if (s->min.kind != MNCF_NUMBER_NONE && mn_num_cmp(n, &s->min) == -1) {
        return 0;
    }
    if (s->max.kind != MNCF_NUMBER_NONE && mn_num_cmp(n, &s->max) == 1) {
        return 0;
    }
    if (s->choices != NULL && !mn_choices_have(s, n)) {
        return 0;
    }
    return 1;
}

/* ---- Problems --------------------------------------------------------------------------------------- */

static int cheat_nomem(schema_ctx *c)
{
    mn_error_nomem(c->err);
    c->problem = NULL;
    return -1;
}

/* A rule broken inside a cheat: returns 0, and the problem is invalid:<path> (invalid for ""). */
static int invalid(schema_ctx *c, const char *path)
{
    c->problem = path[0] != 0 ? mn_arena_printf(c->ar, "invalid:%s", path) : "invalid";
    return c->problem != NULL ? 0 : cheat_nomem(c);
}

static int sub_invalid(schema_ctx *c, const char *path, const char *key)
{
    char *full = mn_arena_printf(c->ar, "%s.%s", path, key);
    if (full == NULL) {
        return cheat_nomem(c);
    }
    return invalid(c, full);
}

/* ---- Combinations of buttons ------------------------------------------------------------------------ */

int mn_combo_parse(mn_arena *ar, const char *text, size_t n, mn_combo *out)
{
    size_t parts = 1, i, start = 0, k = 0;
    const char **names;
    for (i = 0; i < n; i++) {
        if (text[i] == '+') {
            parts++;
        }
    }
    names = (const char **)mn_arena_alloc(ar, parts * sizeof *names);
    if (names == NULL) {
        return 0;
    }
    for (i = 0; i <= n; i++) {
        if (i == n || text[i] == '+') {
            names[k] = mn_arena_strndup(ar, text + start, i - start);
            if (names[k] == NULL) {
                return 0;
            }
            k++;
            start = i + 1;
        }
    }
    qsort(names, parts, sizeof *names, mn_cmp_names);
    k = 0;
    for (i = 0; i < parts; i++) {
        if (k == 0 || strcmp(names[k - 1], names[i]) != 0) {
            names[k++] = names[i];
        }
    }
    out->names = names;
    out->count = k;
    out->cap = 0;
    return 1;
}

int mn_combo_subset(const mn_combo *a, const mn_combo *b)
{
    size_t i, j = 0;
    for (i = 0; i < a->count; i++) {
        while (j < b->count && strcmp(b->names[j], a->names[i]) < 0) {
            j++;
        }
        if (j == b->count || strcmp(b->names[j], a->names[i]) != 0) {
            return 0;
        }
    }
    return 1;
}

int mn_combo_equal(const mn_combo *a, const mn_combo *b)
{
    return a->count == b->count && mn_combo_subset(a, b);
}

/* ---- The keys a cheat may have ---------------------------------------------------------------------- */

enum { NODE_LEAF, NODE_TABLE, NODE_ARRAY, NODE_MAP };

typedef struct node {
    const char *name;
    int kind;
    const struct node *children;   /* ends with a NULL name */
    const char *extension;
} node;

static const node LOC_KEYS[] = {{"module", NODE_LEAF, NULL, NULL}, {"offset", NODE_LEAF, NULL, NULL},
                                {"offsets", NODE_LEAF, NULL, NULL}, {"find", NODE_LEAF, NULL, NULL},
                                {"section", NODE_LEAF, NULL, NULL}, {"region", NODE_LEAF, NULL, NULL},
                                {"address", NODE_LEAF, NULL, NULL}, {"path", NODE_LEAF, NULL, NULL},
                                {NULL, 0, NULL, NULL}};
static const node VALUE_KEYS[] = {{"type", NODE_LEAF, NULL, NULL},   {"min", NODE_LEAF, NULL, NULL},
                                  {"max", NODE_LEAF, NULL, NULL},    {"step", NODE_LEAF, NULL, NULL},
                                  {"default", NODE_LEAF, NULL, NULL}, {"unit", NODE_LEAF, NULL, NULL},
                                  {"display", NODE_LEAF, NULL, NULL}, {"choices", NODE_LEAF, NULL, NULL},
                                  {"flags", NODE_LEAF, NULL, NULL},  {NULL, 0, NULL, NULL}};
static const node ASM_KEYS[] = {{"expect", NODE_LEAF, NULL, NULL}, {"bytes", NODE_LEAF, NULL, NULL},
                                {NULL, 0, NULL, NULL}};
static const node ENCODE_KEYS[] = {{"insn", NODE_LEAF, NULL, NULL}, {"width", NODE_LEAF, NULL, NULL},
                                   {"rd", NODE_LEAF, NULL, NULL},   {"value", NODE_LEAF, NULL, NULL},
                                   {NULL, 0, NULL, NULL}};
static const node PATCH_KEYS[] = {{"at", NODE_TABLE, LOC_KEYS, NULL},          {"expect", NODE_LEAF, NULL, NULL},
                                  {"bytes", NODE_LEAF, NULL, NULL},            {"asm", NODE_TABLE, ASM_KEYS, NULL},
                                  {"encode", NODE_TABLE, ENCODE_KEYS, "arm64-imm/1"}, {NULL, 0, NULL, NULL}};
static const node WRITE_KEYS[] = {{"at", NODE_TABLE, LOC_KEYS, NULL}, {"type", NODE_LEAF, NULL, NULL},
                                  {"value", NODE_LEAF, NULL, NULL},   {"expect", NODE_LEAF, NULL, NULL},
                                  {NULL, 0, NULL, NULL}};
static const node OVERRIDE_KEYS[] = {{"function", NODE_TABLE, LOC_KEYS, NULL}, {"action", NODE_LEAF, NULL, NULL},
                                     {"value", NODE_LEAF, NULL, NULL},         {"expect", NODE_LEAF, NULL, NULL},
                                     {NULL, 0, NULL, NULL}};
static const node READ_KEYS[] = {{"at", NODE_TABLE, LOC_KEYS, NULL}, {"type", NODE_LEAF, NULL, NULL},
                                 {"label", NODE_LEAF, NULL, NULL},   {NULL, 0, NULL, NULL}};
static const node PLACE_KEYS[] = {{"at", NODE_TABLE, LOC_KEYS, NULL}, {"type", NODE_LEAF, NULL, NULL},
                                  {NULL, 0, NULL, NULL}};
static const node WHEN_KEYS[] = {{"at", NODE_TABLE, LOC_KEYS, NULL},     {"type", NODE_LEAF, NULL, NULL},
                                 {"op", NODE_LEAF, NULL, NULL},          {"value", NODE_TABLE, PLACE_KEYS, NULL},
                                 {NULL, 0, NULL, NULL}};
static const node RAW_KEYS[] = {{"syntax", NODE_LEAF, NULL, NULL}, {"build", NODE_LEAF, NULL, NULL},
                                {"code", NODE_LEAF, NULL, NULL},   {NULL, 0, NULL, NULL}};
static const node CHEAT_KEYS[] = {
    {"id", NODE_LEAF, NULL, NULL},       {"name", NODE_LEAF, NULL, NULL},     {"about", NODE_LEAF, NULL, NULL},
    {"group", NODE_LEAF, NULL, NULL},    {"tags", NODE_LEAF, NULL, NULL},     {"default", NODE_LEAF, NULL, NULL},
    {"master", NODE_LEAF, NULL, NULL},   {"run", NODE_LEAF, NULL, NULL},      {"duration", NODE_LEAF, NULL, NULL},
    {"hold", NODE_LEAF, NULL, NULL},     {"toggle", NODE_LEAF, NULL, NULL},   {"press", NODE_LEAF, NULL, NULL},
    {"requires", NODE_LEAF, NULL, NULL}, {"excludes", NODE_LEAF, NULL, NULL}, {"radio", NODE_LEAF, NULL, NULL},
    {"match", NODE_LEAF, NULL, NULL},    {"trigger", NODE_LEAF, NULL, NULL},  {"interval", NODE_LEAF, NULL, NULL},
    {"values", NODE_MAP, VALUE_KEYS, NULL},
    {"patch", NODE_ARRAY, PATCH_KEYS, NULL},
    {"write", NODE_ARRAY, WRITE_KEYS, NULL},
    {"override", NODE_ARRAY, OVERRIDE_KEYS, NULL},
    {"read", NODE_ARRAY, READ_KEYS, NULL},
    {"when", NODE_ARRAY, WHEN_KEYS, NULL},
    {"raw", NODE_ARRAY, RAW_KEYS, "raw/1"},
    {NULL, 0, NULL, NULL}};

static int listed(const schema_ctx *c, const char *name)
{
    size_t i;
    for (i = 0; i < c->listed_count; i++) {
        if (strcmp(c->listed[i], name) == 0) {
            return 1;
        }
    }
    return 0;
}

static int unknown_key(schema_ctx *c, const mn_str *path, size_t depth)
{
    mn_buf b;
    size_t i;
    if (c->unknown_listed != NULL) {
        c->problem = mn_arena_printf(c->ar, "extension-missing:%s", c->unknown_listed);
        return c->problem != NULL ? 0 : cheat_nomem(c);
    }
    mn_buf_init(&b, c->ar->alloc);
    mn_buf_adds(&b, "unknown-key:");
    for (i = 0; i < depth; i++) {
        if (i > 0) {
            mn_buf_addc(&b, '.');
        }
        mn_buf_add(&b, path[i].p, path[i].n);
    }
    c->problem = b.failed ? NULL : mn_arena_strndup(c->ar, b.data, b.size);
    mn_buf_free(&b);
    return c->problem != NULL ? 0 : cheat_nomem(c);
}

/* Finds the first key that neither the core nor a listed extension defines. 1 when there is none. */
static int check_keys(schema_ctx *c, const mncf_toml *table, const node *tree, mn_str *path, size_t depth)
{
    size_t i;
    if (depth >= 6) {
        return 1;
    }
    for (i = 0; i < table->u.t.n; i++) {
        const struct mn_entry *e = &table->u.t.e[i];
        const node *nd = NULL;
        size_t k;
        int r;
        path[depth] = e->key;
        for (k = 0; tree[k].name != NULL; k++) {
            if (mn_streqz(e->key.p, e->key.n, tree[k].name)) {
                nd = &tree[k];
                break;
            }
        }
        if (nd == NULL || (nd->extension != NULL && !listed(c, nd->extension))) {
            return unknown_key(c, path, depth + 1);
        }
        if (nd->kind == NODE_TABLE) {
            if (is_table(e->val)) {
                r = check_keys(c, e->val, nd->children, path, depth + 1);
                if (r != 1) {
                    return r;
                }
            }
        } else if (nd->kind == NODE_ARRAY) {
            if (is_array(e->val)) {
                size_t j;
                for (j = 0; j < e->val->u.a.n; j++) {
                    if (is_table(e->val->u.a.items[j])) {
                        r = check_keys(c, e->val->u.a.items[j], nd->children, path, depth + 1);
                        if (r != 1) {
                            return r;
                        }
                    }
                }
            }
        } else if (nd->kind == NODE_MAP) {
            if (is_table(e->val)) {
                size_t j;
                for (j = 0; j < e->val->u.t.n; j++) {
                    if (is_table(e->val->u.t.e[j].val)) {
                        path[depth + 1] = e->val->u.t.e[j].key;
                        r = check_keys(c, e->val->u.t.e[j].val, nd->children, path, depth + 2);
                        if (r != 1) {
                            return r;
                        }
                    }
                }
            }
        }
    }
    return 1;
}

/* ---- Locations -------------------------------------------------------------------------------------- */

static int build_known(const schema_ctx *c, const char *key, size_t n)
{
    size_t i;
    for (i = 0; i < c->f->info.build_count; i++) {
        if (mn_streqz(key, n, c->f->info.builds[i].key)) {
            return 1;
        }
    }
    return 0;
}

#define CHECK(x)                                                                                                 \
    do {                                                                                                         \
        int r_ = (x);                                                                                            \
        if (r_ != 1) {                                                                                           \
            return r_;                                                                                           \
        }                                                                                                        \
    } while (0)

static int location(schema_ctx *c, const mncf_toml *v, const char *path, mn_loc *loc)
{
    static const char *const SECTIONS[] = {"code", "rodata", "data"};
    int bases = 0;
    const char *base = NULL;
    const mncf_toml *name;
    memset(loc, 0, sizeof *loc);
    if (!is_table(v)) {
        return invalid(c, path);
    }
    if (has(v, "module")) {
        bases++;
        base = "module";
    }
    if (has(v, "region")) {
        bases++;
        base = "region";
    }
    if (has(v, "address")) {
        bases++;
        base = "address";
    }
    if (bases != 1) {
        return invalid(c, path);
    }
    if (has(v, "path")) {
        const mncf_toml *p = get(v, "path");
        size_t i;
        if (!is_array(p) || p->u.a.n == 0) {
            return sub_invalid(c, path, "path");
        }
        loc->path = (uint64_t *)mn_arena_alloc(c->ar, p->u.a.n * sizeof *loc->path);
        if (loc->path == NULL) {
            return cheat_nomem(c);
        }
        for (i = 0; i < p->u.a.n; i++) {
            mn_num step;
            if (!integer_of(p->u.a.items[i], &step)) {
                return sub_invalid(c, path, "path");
            }
            loc->path[i] = mn_num_low64(&step);
        }
        loc->path_len = p->u.a.n;
    }
    if (strcmp(base, "address") == 0) {
        static const char *const NOT_WITH_ADDRESS[] = {"offset", "offsets", "find", "section"};
        size_t i;
        mn_num a;
        if (!c->plat->addresses) {
            return sub_invalid(c, path, "address");
        }
        for (i = 0; i < COUNT(NOT_WITH_ADDRESS); i++) {
            if (has(v, NOT_WITH_ADDRESS[i])) {
                return sub_invalid(c, path, NOT_WITH_ADDRESS[i]);
            }
        }
        if (!integer_of(get(v, "address"), &a) || a.negative) {
            return sub_invalid(c, path, "address");
        }
        loc->kind = LOC_ADDRESS;
        loc->address = a.magnitude;
        return 1;
    }
    name = get(v, base);
    if (!is_str(name) || name->u.s.n == 0) {
        return sub_invalid(c, path, base);
    }
    loc->name = name->u.s;
    loc->kind = strcmp(base, "module") == 0 ? LOC_MODULE : LOC_REGION;
    if (has(v, "find")) {
        if (loc->kind != LOC_MODULE) {
            return sub_invalid(c, path, "find");
        }
        if (has(v, "offsets")) {
            return sub_invalid(c, path, "offsets");
        }
        if (!pattern_of(c->ar, get(v, "find"), &loc->find, &loc->find_len)) {
            return c->ar->failed ? cheat_nomem(c) : sub_invalid(c, path, "find");
        }
        loc->kind = LOC_PATTERN;
        if (has(v, "offset")) {
            if (!is_int(get(v, "offset"))) {
                return sub_invalid(c, path, "offset");
            }
            loc->find_offset = get(v, "offset")->u.i;
        }
        if (has(v, "section")) {
            const mncf_toml *s = get(v, "section");
            if (!str_in(s, SECTIONS, COUNT(SECTIONS))) {
                return sub_invalid(c, path, "section");
            }
            loc->section = str_is(s, "code") ? SEC_CODE : str_is(s, "rodata") ? SEC_RODATA : SEC_DATA;
        }
        return 1;
    }
    if (has(v, "section")) {
        return sub_invalid(c, path, "section");
    }
    if (has(v, "offset") == has(v, "offsets")) {
        return invalid(c, path);
    }
    if (has(v, "offset")) {
        mn_num n;
        if (!integer_of(get(v, "offset"), &n) || n.negative) {
            return sub_invalid(c, path, "offset");
        }
        loc->offset = n.magnitude;
        return 1;
    }
    {
        const mncf_toml *offs = get(v, "offsets");
        size_t i;
        if (!is_table(offs) || offs->u.t.n == 0) {
            return sub_invalid(c, path, "offsets");
        }
        loc->offsets = (mn_offset *)mn_arena_alloc(c->ar, offs->u.t.n * sizeof *loc->offsets);
        if (loc->offsets == NULL) {
            return cheat_nomem(c);
        }
        for (i = 0; i < offs->u.t.n; i++) {
            mn_num n;
            const struct mn_entry *e = &offs->u.t.e[i];
            if (!build_known(c, e->key.p, e->key.n) || !integer_of(e->val, &n) || n.negative) {
                return sub_invalid(c, path, "offsets");
            }
            loc->offsets[i].build = e->key;
            loc->offsets[i].offset = n.magnitude;
        }
        loc->offset_count = offs->u.t.n;
        loc->kind = loc->kind == LOC_MODULE ? LOC_MODULE_BUILDS : LOC_REGION_BUILDS;
    }
    return 1;
}

/* ---- Values the player sets ------------------------------------------------------------------------- */

static int find_value(const mn_cheat *ch, const char *id, size_t n, size_t *out)
{
    size_t i;
    for (i = 0; i < ch->pub.value_count; i++) {
        if (mn_streqz(id, n, ch->pub.values[i].id)) {
            *out = i;
            return 1;
        }
    }
    return 0;
}

enum { KINDS_NUMBER, KINDS_VALUE, KINDS_INT_BOOL };

static int kind_ok(mncf_type t, int kinds)
{
    if (kinds == KINDS_NUMBER) {
        return mn_type_is_number(t);
    }
    if (kinds == KINDS_VALUE) {
        return type_in_values(t);
    }
    return mn_type_is_int(t) || t == MNCF_TYPE_BOOL;
}

/* A reference to a player value, "$count": 1 with *ref set, 2 where v is no reference, else a problem. */
static int value_ref(schema_ctx *c, const mncf_toml *v, const char *path, int kinds, size_t *ref)
{
    if (!is_str(v) || v->u.s.n < 2 || v->u.s.p[0] != '$' || !is_id(v->u.s.p + 1, v->u.s.n - 1)) {
        return 2;
    }
    if (!find_value(c->cheat, v->u.s.p + 1, v->u.s.n - 1, ref) || !kind_ok(c->cheat->pub.values[*ref].type, kinds)) {
        return invalid(c, path);
    }
    return 1;
}

static int value_spec(schema_ctx *c, const mn_str *vid, const mncf_toml *spec, mncf_value_spec *s)
{
    static const char *const LIMITS[] = {"min", "max", "step"};
    char *path;
    mncf_type t;
    size_t i;
    mn_num *limit[3];
    memset(s, 0, sizeof *s);
    s->min = mn_num_none();
    s->max = mn_num_none();
    s->step = mn_num_none();
    path = mn_arena_printf(c->ar, "values.%s", vid->p);
    if (path == NULL) {
        return cheat_nomem(c);
    }
    if (!is_id(vid->p, vid->n) || !is_table(spec)) {
        return invalid(c, path);
    }
    t = type_of(get(spec, "type"));
    if (!type_in_values(t)) {
        return sub_invalid(c, path, "type");
    }
    s->id = vid->p;
    s->type = t;
    limit[0] = &s->min;
    limit[1] = &s->max;
    limit[2] = &s->step;
    for (i = 0; i < 3; i++) {
        if (has(spec, LIMITS[i])) {
            mn_num n;
            mn_num zero = mn_num_i64(0);
            if (t == MNCF_TYPE_BOOL) {
                return sub_invalid(c, path, LIMITS[i]);
            }
            if (!value_number(t, get(spec, LIMITS[i]), &n) || (i == 2 && mn_num_cmp(&n, &zero) <= 0)) {
                return sub_invalid(c, path, LIMITS[i]);
            }
            *limit[i] = n;
        }
    }
    if (s->min.kind != MNCF_NUMBER_NONE && s->max.kind != MNCF_NUMBER_NONE && mn_num_cmp(&s->min, &s->max) == 1) {
        return sub_invalid(c, path, "max");
    }
    if (has(spec, "unit")) {
        if (!is_str(get(spec, "unit"))) {
            return sub_invalid(c, path, "unit");
        }
        s->unit = get(spec, "unit")->u.s.p;
    }
    if (has(spec, "display")) {
        const mncf_toml *d = get(spec, "display");
        if (!str_is(d, "dec") && !str_is(d, "hex")) {
            return sub_invalid(c, path, "display");
        }
        s->hex = str_is(d, "hex");
    }
    if (has(spec, "choices")) {
        const mncf_toml *ch = get(spec, "choices");
        const mncf_choice *items = NULL;
        mncf_choice *made = NULL;
        size_t count = 0, k;
        int ok = 0;
        if (is_str(ch)) {
            for (k = 0; k < c->f->info.choice_list_count; k++) {
                if (mn_streqz(ch->u.s.p, ch->u.s.n, c->f->info.choices[k].name)) {
                    items = c->f->info.choices[k].items;
                    count = c->f->info.choices[k].count;
                    ok = 1;
                    break;
                }
            }
        } else {
            ok = choice_list(c->ar, ch, &made, &count);
            items = made;
            if (!ok && c->ar->failed) {
                return cheat_nomem(c);
            }
        }
        if (ok && mn_type_is_int(t)) {
            for (k = 0; k < count && ok; k++) {
                ok = mn_fits(t, &items[k].key);
            }
        } else {
            ok = 0;
        }
        if (!ok) {
            return sub_invalid(c, path, "choices");
        }
        s->choices = items;
        s->choice_count = count;
    }
    if (has(spec, "flags")) {
        const mncf_toml *fl = get(spec, "flags");
        mncf_choice *items = NULL;
        size_t count = 0, k;
        int ok = is_table(fl) && choice_list(c->ar, fl, &items, &count);
        if (!ok && c->ar->failed) {
            return cheat_nomem(c);
        }
        if (ok && mn_type_is_int(t)) {
            for (k = 0; k < count && ok; k++) {
                const mn_num *b = &items[k].key;
                ok = !b->negative && b->huge == 0 && b->magnitude < 8 * mn_type_size(t);
            }
        } else {
            ok = 0;
        }
        if (!ok) {
            return sub_invalid(c, path, "flags");
        }
        s->flags = items;
        s->flag_count = count;
    }
    if (!has(spec, "default")) {
        return sub_invalid(c, path, "default");
    }
    {
        mn_num d;
        if (!value_number(t, get(spec, "default"), &d) || !mn_value_in_spec(s, &d)) {
            return sub_invalid(c, path, "default");
        }
        s->default_value = d;
    }
    return 1;
}

/* ---- Operations ------------------------------------------------------------------------------------- */

static int patch(schema_ctx *c, const mncf_toml *t, mn_op *op)
{
    op->kind = MNCF_OP_PATCH;
    CHECK(location(c, get(t, "at"), "patch.at", &op->at));
    if (!bytes_of(c->ar, get(t, "expect"), &op->expect, &op->expect_len)) {
        return c->ar->failed ? cheat_nomem(c) : invalid(c, "patch.expect");
    }
    op->has_expect = 1;
    if (has(t, "bytes") == has(t, "encode")) {
        return invalid(c, "patch.bytes");
    }
    if (has(t, "bytes")) {
        if (!bytes_of(c->ar, get(t, "bytes"), &op->bytes, &op->bytes_len)) {
            return c->ar->failed ? cheat_nomem(c) : invalid(c, "patch.bytes");
        }
        if (op->bytes_len != op->expect_len) {
            return invalid(c, "patch.expect");
        }
    } else {
        const mncf_toml *e = get(t, "encode");
        const mncf_toml *insn, *width, *rd, *value;
        int r;
        if (!is_table(e)) {
            return invalid(c, "patch.encode");
        }
        insn = get(e, "insn");
        width = get(e, "width");
        rd = get(e, "rd");
        value = get(e, "value");
        if (!is_str(insn) || mn_insn_parse(insn->u.s.p, insn->u.s.n) < 0) {
            return invalid(c, "patch.encode.insn");
        }
        if (!is_int(width) || (width->u.i != 32 && width->u.i != 64)) {
            return invalid(c, "patch.encode.width");
        }
        if (!is_int(rd) || rd->u.i < 0 || rd->u.i > 31) {
            return invalid(c, "patch.encode.rd");
        }
        op->encode = 1;
        op->insn = mn_insn_parse(insn->u.s.p, insn->u.s.n);
        op->width = (int)width->u.i;
        op->rd = (int)rd->u.i;
        r = value_ref(c, value, "patch.encode.value", KINDS_NUMBER, &op->ref);
        if (r == 1) {
            op->value_kind = VAL_REF;
        } else if (r == 2) {
            uint8_t out[8];
            if (!is_number(value)) {
                return invalid(c, "patch.encode.value");
            }
            op->value = is_int(value) ? mn_num_i64(value->u.i) : mn_num_f64(value->u.f);
            if (mn_arm64_imm(op->insn, op->width, op->rd, &op->value, out) == 0) {
                return invalid(c, "patch.encode.value");
            }
            op->value_kind = VAL_NUMBER;
        } else {
            return r;
        }
        if (op->expect_len != mn_insn_size(op->insn)) {
            return invalid(c, "patch.expect");
        }
    }
    if (has(t, "asm")) {
        const mncf_toml *a = get(t, "asm");
        if (!is_table(a) || (has(a, "expect") && !is_str(get(a, "expect"))) ||
            (has(a, "bytes") && !is_str(get(a, "bytes")))) {
            return invalid(c, "patch.asm");
        }
    }
    return 1;
}

static int write_op(schema_ctx *c, const mncf_toml *t, mn_op *op)
{
    const mncf_toml *v;
    int r;
    op->kind = MNCF_OP_WRITE;
    CHECK(location(c, get(t, "at"), "write.at", &op->at));
    op->type = type_of(get(t, "type"));
    if (!mn_type_is_number(op->type) && op->type != MNCF_TYPE_BYTES) {
        return invalid(c, "write.type");
    }
    if (!has(t, "value")) {
        return invalid(c, "write.value");
    }
    v = get(t, "value");
    if (op->type == MNCF_TYPE_BYTES) {
        if (!bytes_of(c->ar, v, &op->bytes, &op->bytes_len)) {
            return c->ar->failed ? cheat_nomem(c) : invalid(c, "write.value");
        }
        op->value_kind = VAL_BYTES;
        if (has(t, "expect")) {
            if (!bytes_of(c->ar, get(t, "expect"), &op->expect, &op->expect_len) || op->expect_len != op->bytes_len) {
                return c->ar->failed ? cheat_nomem(c) : invalid(c, "write.expect");
            }
            op->has_expect = 1;
        }
        return 1;
    }
    r = value_ref(c, v, "write.value", KINDS_VALUE, &op->ref);
    if (r == 1) {
        if (mn_type_is_int(op->type) && mn_type_is_float(c->cheat->pub.values[op->ref].type)) {
            return invalid(c, "write.value");
        }
        op->value_kind = VAL_REF;
    } else if (r == 2) {
        if (!value_number(op->type, v, &op->value)) {
            return invalid(c, "write.value");
        }
        op->value_kind = VAL_NUMBER;
    } else {
        return r;
    }
    if (has(t, "expect")) {
        if (!value_number(op->type, get(t, "expect"), &op->num_expect)) {
            return invalid(c, "write.expect");
        }
        op->has_num_expect = 1;
    }
    return 1;
}

static int override_op(schema_ctx *c, const mncf_toml *t, mn_op *op)
{
    const mncf_toml *action;
    op->kind = MNCF_OP_OVERRIDE;
    CHECK(location(c, get(t, "function"), "override.function", &op->at));
    action = get(t, "action");
    if (str_is(action, "return")) {
        op->action = MNCF_ACTION_RETURN;
    } else if (str_is(action, "skip")) {
        op->action = MNCF_ACTION_SKIP;
    } else {
        return invalid(c, "override.action");
    }
    if (op->action == MNCF_ACTION_RETURN) {
        int r;
        if (!has(t, "value")) {
            return invalid(c, "override.value");
        }
        r = value_ref(c, get(t, "value"), "override.value", KINDS_INT_BOOL, &op->ref);
        if (r == 1) {
            op->value_kind = VAL_REF;
        } else if (r == 2) {
            if (!integer_of(get(t, "value"), &op->value)) {
                return invalid(c, "override.value");
            }
            op->value_kind = VAL_NUMBER;
        } else {
            return r;
        }
    } else if (has(t, "value")) {
        return invalid(c, "override.value");
    }
    if (has(t, "expect")) {
        if (!bytes_of(c->ar, get(t, "expect"), &op->expect, &op->expect_len)) {
            return c->ar->failed ? cheat_nomem(c) : invalid(c, "override.expect");
        }
        op->has_expect = 1;
    }
    return 1;
}

static int read_op(schema_ctx *c, const mncf_toml *t, mn_op *op)
{
    op->kind = MNCF_OP_READ;
    CHECK(location(c, get(t, "at"), "read.at", &op->at));
    op->type = type_of(get(t, "type"));
    if (!mn_type_is_number(op->type)) {
        return invalid(c, "read.type");
    }
    if (has(t, "label")) {
        if (!is_str(get(t, "label"))) {
            return invalid(c, "read.label");
        }
        op->label = get(t, "label")->u.s.p;
    }
    return 1;
}

static int compare_of(const mncf_toml *v)
{
    static const char *const OPS[] = {"==", "!=", "<", "<=", ">", ">=", "&", "!&", "between", "changed"};
    size_t i;
    if (!is_str(v)) {
        return MNCF_COMPARE_NONE;
    }
    for (i = 0; i < COUNT(OPS); i++) {
        if (mn_streqz(v->u.s.p, v->u.s.n, OPS[i])) {
            return (int)i + 1;
        }
    }
    return MNCF_COMPARE_NONE;
}

static int condition(schema_ctx *c, const mncf_toml *t, mn_op *op)
{
    const mncf_toml *v;
    int bits;
    int r;
    op->kind = MNCF_OP_WHEN;
    CHECK(location(c, get(t, "at"), "when.at", &op->at));
    op->type = type_of(get(t, "type"));
    if (!mn_type_is_number(op->type)) {
        return invalid(c, "when.type");
    }
    op->compare = compare_of(get(t, "op"));
    if (op->compare == MNCF_COMPARE_NONE) {
        return invalid(c, "when.op");
    }
    bits = op->compare == MNCF_COMPARE_AND || op->compare == MNCF_COMPARE_NAND;
    if (bits && !mn_type_is_int(op->type)) {
        return invalid(c, "when.type");
    }
    if (op->compare == MNCF_COMPARE_CHANGED) {
        if (has(t, "value")) {
            return invalid(c, "when.value");
        }
        op->value_kind = VAL_NONE;
        return 1;
    }
    if (!has(t, "value")) {
        return invalid(c, "when.value");
    }
    v = get(t, "value");
    if (op->compare == MNCF_COMPARE_BETWEEN) {
        if (!is_array(v) || v->u.a.n != 2) {
            return invalid(c, "when.value");
        }
        if (!value_number(op->type, v->u.a.items[0], &op->low) || !value_number(op->type, v->u.a.items[1], &op->high) ||
            mn_num_cmp(&op->low, &op->high) == 1) {
            return invalid(c, "when.value");
        }
        op->value_kind = VAL_RANGE;
        return 1;
    }
    if (is_table(v)) {
        if (!has(v, "at")) {
            return invalid(c, "when.value.at");
        }
        CHECK(location(c, get(v, "at"), "when.value.at", &op->other_at));
        op->other_type = type_of(get(v, "type"));
        if (!mn_type_is_number(op->other_type) || (bits && !mn_type_is_int(op->other_type))) {
            return invalid(c, "when.value.type");
        }
        op->value_kind = VAL_PLACE;
        return 1;
    }
    r = value_ref(c, v, "when.value", bits ? KINDS_INT_BOOL : KINDS_VALUE, &op->ref);
    if (r == 1) {
        op->value_kind = VAL_REF;
        return 1;
    }
    if (r != 2) {
        return r;
    }
    if (!value_number(op->type, v, &op->value)) {
        return invalid(c, "when.value");
    }
    op->value_kind = VAL_NUMBER;
    return 1;
}

static int raw_entry(schema_ctx *c, const mncf_toml *t, mn_raw *raw)
{
    const mncf_toml *syntax = get(t, "syntax"), *build = get(t, "build"), *code = get(t, "code");
    size_t i;
    int any = 0;
    if (!is_str(syntax) || syntax->u.s.n == 0) {
        return invalid(c, "raw.syntax");
    }
    if (!is_str(build) || !build_known(c, build->u.s.p, build->u.s.n)) {
        return invalid(c, "raw.build");
    }
    if (!is_str(code)) {
        return invalid(c, "raw.code");
    }
    /* Not only white space; for atmosphere, words of 8 hexadecimal digits between white space. */
    i = 0;
    while (i < code->u.s.n) {
        uint32_t cp;
        size_t len = mn_utf8_decode((const uint8_t *)code->u.s.p + i, code->u.s.n - i, &cp);
        if (!mn_py_space(cp)) {
            any = 1;
            break;
        }
        i += len;
    }
    if (!any) {
        return invalid(c, "raw.code");
    }
    if (str_is(syntax, "atmosphere")) {
        size_t word = 0;
        int in_word = 0, ok = 1;
        i = 0;
        while (i < code->u.s.n && ok) {
            uint32_t cp;
            size_t len = mn_utf8_decode((const uint8_t *)code->u.s.p + i, code->u.s.n - i, &cp);
            if (mn_py_space(cp)) {
                if (in_word && word != 8) {
                    ok = 0;
                }
                in_word = 0;
                word = 0;
            } else {
                in_word = 1;
                word++;
                if (!(cp < 0x80 && mn_is_hex((int)cp))) {
                    ok = 0;
                }
            }
            i += len;
        }
        if (in_word && word != 8) {
            ok = 0;
        }
        if (!ok) {
            return invalid(c, "raw.code");
        }
    }
    raw->syntax = syntax->u.s.p;
    raw->build = build->u.s.p;
    raw->code = code->u.s.p;
    return 1;
}

/* ---- One cheat -------------------------------------------------------------------------------------- */

static int string_key(schema_ctx *c, const mncf_toml *t, const char *key, int required, const char **out)
{
    const mncf_toml *v = get(t, key);
    if (v == NULL) {
        return required ? invalid(c, key) : 1;
    }
    if (!is_str(v)) {
        return invalid(c, key);
    }
    *out = v->u.s.p;
    return 1;
}

static int boolean_key(schema_ctx *c, const mncf_toml *t, const char *key, int *out)
{
    const mncf_toml *v = get(t, key);
    if (v == NULL) {
        return 1;
    }
    if (!is_type(v, MNCF_TOML_BOOLEAN)) {
        return invalid(c, key);
    }
    *out = v->u.b;
    return 1;
}

static int buttons(schema_ctx *c, const mncf_toml *v, const char *key, const char **out, mn_combo *combo)
{
    size_t i, start = 0, parts = 0;
    if (!is_str(v) || v->u.s.n == 0) {
        return invalid(c, key);
    }
    for (i = 0; i <= v->u.s.n; i++) {
        if (i == v->u.s.n || v->u.s.p[i] == '+') {
            if (!mn_platform_button(c->plat, v->u.s.p + start, i - start)) {
                return invalid(c, key);
            }
            parts++;
            start = i + 1;
        }
    }
    if (!mn_combo_parse(c->ar, v->u.s.p, v->u.s.n, combo)) {
        return cheat_nomem(c);
    }
    if (combo->count != parts) {
        return invalid(c, key);
    }
    *out = v->u.s.p;
    return 1;
}

static int id_list(schema_ctx *c, const mncf_toml *v, const char *key, const char *const **names, size_t *count,
                   size_t **indexes)
{
    size_t i;
    const char **out;
    size_t *idx;
    if (!is_array(v)) {
        return invalid(c, key);
    }
    out = (const char **)mn_arena_alloc(c->ar, (v->u.a.n + 1) * sizeof *out);
    idx = (size_t *)mn_arena_alloc(c->ar, (v->u.a.n + 1) * sizeof *idx);
    if (out == NULL || idx == NULL) {
        return cheat_nomem(c);
    }
    for (i = 0; i < v->u.a.n; i++) {
        const mncf_toml *x = v->u.a.items[i];
        size_t at;
        if (!is_str(x)) {
            return invalid(c, key);
        }
        at = mn_cheat_index(c->f, x->u.s.p, x->u.s.n);
        if (at == (size_t)-1 || at == c->cheat->pub.index) {
            return invalid(c, key);
        }
        out[i] = c->f->cheats[at].pub.id;
        idx[i] = at;
    }
    *names = out;
    *count = v->u.a.n;
    *indexes = idx;
    return 1;
}

static int fill_cheat(schema_ctx *c, const mncf_toml *t)
{
    static const char *const RUNS[] = {"on", "once", "action", "timed", "monitor"};
    static const char *const OP_KEYS[] = {"patch", "write", "override", "read", "when", "raw"};
    mn_cheat *ch = c->cheat;
    mncf_cheat *p = &ch->pub;
    size_t i;
    int kinds = 0;
    CHECK(string_key(c, t, "name", 1, &p->name));
    CHECK(string_key(c, t, "about", 0, &p->about));
    if (has(t, "group")) {
        const mncf_toml *g = get(t, "group");
        size_t k, start = 0;
        if (!is_str(g) || g->u.s.n == 0) {
            return invalid(c, "group");
        }
        for (k = 0; k <= g->u.s.n; k++) {
            if (k == g->u.s.n || g->u.s.p[k] == '/') {
                if (k == start) {
                    return invalid(c, "group");
                }
                start = k + 1;
            }
        }
        p->group = g->u.s.p;
    }
    if (has(t, "tags")) {
        const mncf_toml *tags = get(t, "tags");
        const char **out;
        if (!is_array(tags)) {
            return invalid(c, "tags");
        }
        out = (const char **)mn_arena_alloc(c->ar, (tags->u.a.n + 1) * sizeof *out);
        if (out == NULL) {
            return cheat_nomem(c);
        }
        for (i = 0; i < tags->u.a.n; i++) {
            if (!is_str(tags->u.a.items[i])) {
                return invalid(c, "tags");
            }
            out[i] = tags->u.a.items[i]->u.s.p;
        }
        p->tags = out;
        p->tag_count = tags->u.a.n;
    }
    CHECK(boolean_key(c, t, "default", &p->default_on));
    CHECK(boolean_key(c, t, "master", &p->master));
    if (has(t, "run")) {
        const mncf_toml *run = get(t, "run");
        if (!str_in(run, RUNS, COUNT(RUNS))) {
            return invalid(c, "run");
        }
        for (i = 0; i < COUNT(RUNS); i++) {
            if (str_is(run, RUNS[i])) {
                p->run = (mncf_run)i;
            }
        }
    }
    if (has(t, "duration") || p->run == MNCF_RUN_TIMED) {
        const mncf_toml *d = get(t, "duration");
        double value;
        if (p->run != MNCF_RUN_TIMED || !is_number(d)) {
            return invalid(c, "duration");
        }
        value = is_int(d) ? (double)d->u.i : d->u.f;
        if (!isfinite(value) || !(value > 0) || (is_int(d) && d->u.i <= 0)) {
            return invalid(c, "duration");
        }
        p->duration = value;
    }
    if (has(t, "hold")) {
        CHECK(buttons(c, get(t, "hold"), "hold", &p->hold, &ch->hold));
        ch->has_hold = 1;
    }
    if (has(t, "toggle")) {
        CHECK(buttons(c, get(t, "toggle"), "toggle", &p->toggle, &ch->toggle));
        ch->has_toggle = 1;
    }
    if (has(t, "press")) {
        CHECK(buttons(c, get(t, "press"), "press", &p->press, &ch->press));
        ch->has_press = 1;
    }
    if ((p->run == MNCF_RUN_ACTION) != (p->press != NULL)) {
        return invalid(c, "press");
    }
    if (has(t, "requires")) {
        CHECK(id_list(c, get(t, "requires"), "requires", &p->requires, &p->requires_count, &ch->requires));
    }
    if (has(t, "excludes")) {
        CHECK(id_list(c, get(t, "excludes"), "excludes", &p->excludes, &p->excludes_count, &ch->excludes));
    }
    if (has(t, "radio")) {
        const mncf_toml *r = get(t, "radio");
        if (!is_str(r) || r->u.s.n == 0) {
            return invalid(c, "radio");
        }
        p->radio = r->u.s.p;
    }
    if (has(t, "match")) {
        const mncf_toml *m = get(t, "match");
        if (!str_is(m, "all") && !str_is(m, "any")) {
            return invalid(c, "match");
        }
        p->match_any = str_is(m, "any");
    }
    if (has(t, "trigger")) {
        const mncf_toml *m = get(t, "trigger");
        if (!str_is(m, "while") && !str_is(m, "enter")) {
            return invalid(c, "trigger");
        }
        p->trigger_enter = str_is(m, "enter");
    }
    if (has(t, "interval")) {
        const mncf_toml *n = get(t, "interval");
        if (!is_int(n) || n->u.i < 1) {
            return invalid(c, "interval");
        }
        p->interval = n->u.i;
    }
    if (has(t, "values")) {
        const mncf_toml *values = get(t, "values");
        mncf_value_spec *specs;
        if (!is_table(values)) {
            return invalid(c, "values");
        }
        specs = (mncf_value_spec *)mn_arena_zalloc(c->ar, (values->u.t.n + 1) * sizeof *specs);
        if (specs == NULL) {
            return cheat_nomem(c);
        }
        p->values = specs;
        for (i = 0; i < values->u.t.n; i++) {
            CHECK(value_spec(c, &values->u.t.e[i].key, values->u.t.e[i].val, &specs[i]));
            p->value_count = i + 1;
        }
    }
    for (i = 0; i < t->u.t.n; i++) {
        const struct mn_entry *e = &t->u.t.e[i];
        size_t k, j;
        int which = -1;
        for (k = 0; k < COUNT(OP_KEYS); k++) {
            if (mn_streqz(e->key.p, e->key.n, OP_KEYS[k])) {
                which = (int)k;
            }
        }
        if (which < 0) {
            continue;
        }
        if (!is_array(e->val) || e->val->u.a.n == 0) {
            return invalid(c, OP_KEYS[which]);
        }
        for (j = 0; j < e->val->u.a.n; j++) {
            if (!is_table(e->val->u.a.items[j])) {
                return invalid(c, OP_KEYS[which]);
            }
        }
        for (j = 0; j < e->val->u.a.n; j++) {
            const mncf_toml *item = e->val->u.a.items[j];
            if (which == 5) {
                if (MN_GROW(c->ar, ch->raw, ch->raw_count, ch->raw_cap) == NULL) {
                    return cheat_nomem(c);
                }
                memset(&ch->raw[ch->raw_count], 0, sizeof ch->raw[0]);
                CHECK(raw_entry(c, item, &ch->raw[ch->raw_count]));
                ch->raw_count++;
                continue;
            }
            if (MN_GROW(c->ar, ch->ops, ch->op_count, ch->op_cap) == NULL) {
                return cheat_nomem(c);
            }
            {
                mn_op *op = &ch->ops[ch->op_count];
                memset(op, 0, sizeof *op);
                op->index = ch->op_count;
                switch (which) {
                case 0:
                    CHECK(patch(c, item, op));
                    break;
                case 1:
                    CHECK(write_op(c, item, op));
                    break;
                case 2:
                    CHECK(override_op(c, item, op));
                    break;
                case 3:
                    CHECK(read_op(c, item, op));
                    break;
                default:
                    CHECK(condition(c, item, op));
                    break;
                }
                kinds |= 1 << op->kind;
                ch->op_count++;
            }
        }
    }
    {
        int changes = (1 << MNCF_OP_PATCH) | (1 << MNCF_OP_WRITE) | (1 << MNCF_OP_OVERRIDE);
        int code = (1 << MNCF_OP_PATCH) | (1 << MNCF_OP_OVERRIDE);
        int reads = 1 << MNCF_OP_READ;
        if (p->run == MNCF_RUN_MONITOR) {
            if ((kinds & changes) || !(kinds & reads)) {
                return invalid(c, "run");
            }
        } else if (p->run == MNCF_RUN_ONCE && (kinds & code)) {
            return invalid(c, "run");
        } else if (kinds & reads) {
            return invalid(c, "read");
        }
        if ((kinds & ~(1 << MNCF_OP_WHEN)) == 0 && ch->raw_count == 0) {
            return invalid(c, "");
        }
    }
    return 1;
}

static int check_cheat(schema_ctx *c, mn_cheat *ch, const mncf_toml *t)
{
    mn_str path[8];
    int r;
    c->cheat = ch;
    c->problem = NULL;
    ch->pub.run = MNCF_RUN_ON;
    ch->pub.interval = 1;
    r = check_keys(c, t, CHEAT_KEYS, path, 0);
    if (r == 1) {
        r = fill_cheat(c, t);
    }
    if (r < 0) {
        return 0;
    }
    if (r == 0) {
        ch->pub.problem = c->problem;
    }
    return 1;
}

/* ---- The file --------------------------------------------------------------------------------------- */

/* Adds a warning: the prefix and the key, and the second key after a dot where it is given. */
static int warn(schema_ctx *c, const char *prefix, const char *a, const char *b)
{
    char *w = b != NULL ? mn_arena_printf(c->ar, "%s%s.%s", prefix, a, b) : mn_arena_printf(c->ar, "%s%s", prefix, a);
    if (w == NULL || MN_GROW(c->ar, c->warnings, c->warning_count, c->warning_cap) == NULL) {
        return 0;
    }
    c->warnings[c->warning_count++] = w;
    return 1;
}

static int file_fail(mncf_error *err, mncf_status status, const char *code, const char *message)
{
    mn_error(err, status, code, "%s", message);
    return 0;
}

static int schema_fail(mncf_error *err, const char *key, const char *message)
{
    char code[300];
    snprintf(code, sizeof code, "schema:%s", key);
    return file_fail(err, MNCF_ERR_SCHEMA, code, message);
}

static int string_list(mn_arena *ar, const mncf_toml *v, const char *const **out, size_t *count)
{
    size_t i;
    const char **items;
    if (!is_array(v)) {
        return 0;
    }
    items = (const char **)mn_arena_alloc(ar, (v->u.a.n + 1) * sizeof *items);
    if (items == NULL) {
        return 0;
    }
    for (i = 0; i < v->u.a.n; i++) {
        if (!is_str(v->u.a.items[i])) {
            return 0;
        }
        items[i] = v->u.a.items[i]->u.s.p;
    }
    *out = items;
    *count = v->u.a.n;
    return 1;
}

size_t mn_cheat_index(const mncf_file *f, const char *id, size_t n)
{
    size_t at, mask;
    if (f->cheat_index.cap == 0) {
        return (size_t)-1;
    }
    mask = f->cheat_index.cap - 1;
    at = (size_t)mn_hash(id, n) & mask;
    for (;;) {
        uint32_t slot = f->cheat_index.slots[at];
        if (slot == 0) {
            return (size_t)-1;
        }
        if (mn_streq(id, n, f->cheats[slot - 1].pub.id, f->cheats[slot - 1].id_len)) {
            return slot - 1;
        }
        at = (at + 1) & mask;
    }
}

static int preset(schema_ctx *c, const mncf_toml *p, mncf_preset *out, const mncf_preset *seen, size_t seen_count)
{
    static const char *const PRESET_KEYS[] = {"id", "name", "cheats", "values"};
    const mncf_toml *pid = get(p, "id"), *name = get(p, "name"), *ids = get(p, "cheats"), *values = get(p, "values");
    size_t i;
    const char **cheats;
    mncf_preset_value *numbers = NULL;
    if (!is_table(p) || !is_str(pid) || !is_id(pid->u.s.p, pid->u.s.n) || !is_str(name)) {
        return 0;
    }
    for (i = 0; i < seen_count; i++) {
        if (mn_streqz(pid->u.s.p, pid->u.s.n, seen[i].id)) {
            return 0;
        }
    }
    if (ids != NULL && !is_array(ids)) {
        return 0;
    }
    cheats = (const char **)mn_arena_alloc(c->ar, ((ids != NULL ? ids->u.a.n : 0) + 1) * sizeof *cheats);
    if (cheats == NULL) {
        return -1;
    }
    for (i = 0; ids != NULL && i < ids->u.a.n; i++) {
        const mncf_toml *x = ids->u.a.items[i];
        size_t at;
        if (!is_str(x)) {
            return 0;
        }
        at = mn_cheat_index(c->f, x->u.s.p, x->u.s.n);
        if (at == (size_t)-1) {
            return 0;
        }
        cheats[i] = c->f->cheats[at].pub.id;
    }
    if (values != NULL && !is_table(values)) {
        return 0;
    }
    if (values != NULL) {
        numbers = (mncf_preset_value *)mn_arena_alloc(c->ar, (values->u.t.n + 1) * sizeof *numbers);
        if (numbers == NULL) {
            return -1;
        }
        for (i = 0; i < values->u.t.n; i++) {
            const struct mn_entry *e = &values->u.t.e[i];
            const char *dot = (const char *)memchr(e->key.p, '.', e->key.n);
            size_t cn = dot != NULL ? (size_t)(dot - e->key.p) : e->key.n;
            size_t at = mn_cheat_index(c->f, e->key.p, cn);
            size_t vi;
            mn_num n;
            const mn_cheat *ch;
            const char *vid = dot != NULL ? dot + 1 : "";
            size_t vn = dot != NULL ? e->key.n - cn - 1 : 0;
            if (at == (size_t)-1) {
                return 0;
            }
            ch = &c->f->cheats[at];
            if (!find_value(ch, vid, vn, &vi)) {
                return 0;
            }
            if (!value_number(ch->pub.values[vi].type, e->val, &n) || !mn_value_in_spec(&ch->pub.values[vi], &n)) {
                return 0;
            }
            numbers[i].key = e->key.p;
            numbers[i].value = n;
        }
    }
    for (i = 0; i < p->u.t.n; i++) {
        if (!key_in(&p->u.t.e[i].key, PRESET_KEYS, COUNT(PRESET_KEYS))) {
            return 0;
        }
    }
    out->id = pid->u.s.p;
    out->name = name->u.s.p;
    out->cheats = cheats;
    out->cheat_count = ids != NULL ? ids->u.a.n : 0;
    out->values = numbers;
    out->value_count = values != NULL ? values->u.t.n : 0;
    return 1;
}

static int alloc_index(mncf_file *f)
{
    size_t cap = 16;
    while (cap < f->cheat_count * 2) {
        cap *= 2;
    }
    f->cheat_index.slots = (uint32_t *)mn_arena_zalloc(&f->arena, cap * sizeof(uint32_t));
    if (f->cheat_index.slots == NULL) {
        return 0;
    }
    f->cheat_index.cap = cap;
    return 1;
}

int mn_build_file(mncf_file *f, const mncf_toml *doc, mncf_error *err)
{
    schema_ctx c;
    mncf_file_info *info = &f->info;
    mn_arena *ar = &f->arena;
    const mncf_toml *v, *game, *builds, *choices, *cheats, *presets, *i18n;
    size_t i;
    memset(&c, 0, sizeof c);
    c.f = f;
    c.ar = ar;
    c.err = err;
    info->document = doc;
    info->language = "en";

    if (doc->u.t.n == 0 || !mn_streqz(doc->u.t.e[0].key.p, doc->u.t.e[0].key.n, "format")) {
        return file_fail(err, MNCF_ERR_FORMAT, "format", "`format` must be the first key of the file");
    }
    if (!str_is(doc->u.t.e[0].val, "mncf/1")) {
        return file_fail(err, MNCF_ERR_FORMAT, "format", "the format is not \"mncf/1\", which this reader reads");
    }
    for (i = 0; i < doc->u.t.n; i++) {
        if (!key_in(&doc->u.t.e[i].key, FILE_KEYS, COUNT(FILE_KEYS)) &&
            !warn(&c, "unknown-key:", doc->u.t.e[i].key.p, NULL)) {
            goto nomem;
        }
    }

    v = get(doc, "name");
    if (!is_str(v)) {
        return schema_fail(err, "name", "`name` must be a string");
    }
    info->name = v->u.s.p;
    {
        static const char *const KEYS[] = {"about", "file_version", "language"};
        for (i = 0; i < COUNT(KEYS); i++) {
            if (has(doc, KEYS[i])) {
                v = get(doc, KEYS[i]);
                if (!is_str(v)) {
                    return schema_fail(err, KEYS[i], "this key must be a string");
                }
                if (i == 0) {
                    info->about = v->u.s.p;
                } else if (i == 1) {
                    info->file_version = v->u.s.p;
                } else {
                    info->language = v->u.s.p;
                }
            }
        }
    }
    if (has(doc, "authors") && !string_list(ar, get(doc, "authors"), &info->authors, &info->author_count)) {
        if (ar->failed) {
            goto nomem;
        }
        return schema_fail(err, "authors", "`authors` must be an array of strings");
    }

    if (has(doc, "origin")) {
        const mncf_toml *o = get(doc, "origin");
        if (!is_table(o) || !is_str(get(o, "format")) || (has(o, "name") && !is_str(get(o, "name"))) ||
            (has(o, "site") && !is_str(get(o, "site")))) {
            return schema_fail(err, "origin", "`origin` must be a table with a `format` string and maybe `name` "
                                              "and `site` strings");
        }
        for (i = 0; i < o->u.t.n; i++) {
            static const char *const KEYS[] = {"format", "name", "site"};
            if (!key_in(&o->u.t.e[i].key, KEYS, COUNT(KEYS)) &&
                !warn(&c, "unknown-key:origin.", o->u.t.e[i].key.p, NULL)) {
                goto nomem;
            }
        }
        info->origin_format = get(o, "format")->u.s.p;
        info->origin_name = has(o, "name") ? get(o, "name")->u.s.p : NULL;
        info->origin_site = has(o, "site") ? get(o, "site")->u.s.p : NULL;
    }

    info->required = NULL;
    info->optional = NULL;
    if (has(doc, "extensions")) {
        const mncf_toml *ext = get(doc, "extensions");
        static const char *const KEYS[] = {"required", "optional"};
        static const char *const EMPTY[1] = {NULL};
        if (!is_table(ext)) {
            return schema_fail(err, "extensions", "`extensions` must be a table");
        }
        for (i = 0; i < ext->u.t.n; i++) {
            if (!key_in(&ext->u.t.e[i].key, KEYS, COUNT(KEYS)) &&
                !warn(&c, "unknown-key:extensions.", ext->u.t.e[i].key.p, NULL)) {
                goto nomem;
            }
        }
        info->required = EMPTY;
        info->optional = EMPTY;
        if (has(ext, "required") && !string_list(ar, get(ext, "required"), &info->required, &info->required_count)) {
            if (ar->failed) {
                goto nomem;
            }
            return schema_fail(err, "extensions.required", "`extensions.required` must be an array of strings");
        }
        if (has(ext, "optional") && !string_list(ar, get(ext, "optional"), &info->optional, &info->optional_count)) {
            if (ar->failed) {
                goto nomem;
            }
            return schema_fail(err, "extensions.optional", "`extensions.optional` must be an array of strings");
        }
        for (i = 0; i < info->required_count + info->optional_count; i++) {
            const char *name = i < info->required_count ? info->required[i] : info->optional[i - info->required_count];
            if (!is_extension_name(name, strlen(name))) {
                return schema_fail(err, "extensions", "an extension is not a name with its version");
            }
        }
    }
    {
        size_t n = info->required_count + info->optional_count, k;
        const char **all = (const char **)mn_arena_alloc(ar, (n + 1) * sizeof *all);
        if (all == NULL) {
            goto nomem;
        }
        for (i = 0; i < n; i++) {
            all[i] = i < info->required_count ? info->required[i] : info->optional[i - info->required_count];
        }
        c.listed = all;
        c.listed_count = n;
        for (i = 0; i < n && c.unknown_listed == NULL; i++) {
            int known = 0;
            for (k = 0; k < COUNT(KNOWN_EXTENSIONS); k++) {
                known |= strcmp(all[i], KNOWN_EXTENSIONS[k]) == 0;
            }
            if (!known) {
                c.unknown_listed = all[i];
            }
        }
    }

    game = get(doc, "game");
    if (!is_table(game)) {
        return schema_fail(err, "game", "the `game` table is missing");
    }
    for (i = 0; i < game->u.t.n; i++) {
        static const char *const KEYS[] = {"platform", "id", "name"};
        if (!key_in(&game->u.t.e[i].key, KEYS, COUNT(KEYS)) &&
            !warn(&c, "unknown-key:game.", game->u.t.e[i].key.p, NULL)) {
            goto nomem;
        }
    }
    v = get(game, "platform");
    if (!is_str(v)) {
        return schema_fail(err, "game.platform", "`game.platform` must be a string");
    }
    f->platform = c.plat = mn_platform_find(v->u.s.p, v->u.s.n);
    if (c.plat == NULL) {
        return file_fail(err, MNCF_ERR_PLATFORM, "platform", "the platform is not in Appendix A");
    }
    info->platform = c.plat->name;
    v = get(game, "id");
    if (!is_str(v) || !mn_platform_game_id(c.plat, v->u.s.p, v->u.s.n)) {
        return schema_fail(err, "game.id", "`game.id` is not a game ID of the platform");
    }
    info->game_id = v->u.s.p;
    if (has(game, "name")) {
        if (!is_str(get(game, "name"))) {
            return schema_fail(err, "game.name", "`game.name` must be a string");
        }
        info->game_name = get(game, "name")->u.s.p;
    }

    builds = get(doc, "builds");
    if (builds != NULL && !is_table(builds)) {
        return schema_fail(err, "builds", "`builds` must be a table");
    }
    if (builds != NULL) {
        mncf_build *out = (mncf_build *)mn_arena_zalloc(ar, (builds->u.t.n + 1) * sizeof *out);
        if (out == NULL) {
            goto nomem;
        }
        info->builds = out;
        for (i = 0; i < builds->u.t.n; i++) {
            const struct mn_entry *e = &builds->u.t.e[i];
            const mncf_toml *b = e->val, *mods, *version;
            size_t k;
            mncf_module_id *ids;
            if (e->key.n == 0 || !is_table(b)) {
                return schema_fail(err, "builds", "a build must be a table with a non-empty key");
            }
            for (k = 0; k < b->u.t.n; k++) {
                static const char *const KEYS[] = {"version", "modules"};
                if (!key_in(&b->u.t.e[k].key, KEYS, COUNT(KEYS)) &&
                    !warn(&c, "unknown-key:builds.", e->key.p, b->u.t.e[k].key.p)) {
                    goto nomem;
                }
            }
            mods = get(b, "modules");
            if (!is_table(mods) || mods->u.t.n == 0) {
                return schema_fail(err, "builds", "a build needs a `modules` table");
            }
            ids = (mncf_module_id *)mn_arena_alloc(ar, mods->u.t.n * sizeof *ids);
            if (ids == NULL) {
                goto nomem;
            }
            for (k = 0; k < mods->u.t.n; k++) {
                const struct mn_entry *m = &mods->u.t.e[k];
                if ((c.plat->modules != NULL && !mn_platform_has_module(c.plat, m->key.p, m->key.n)) || m->key.n == 0) {
                    return schema_fail(err, "builds", "a module of a build is not a module of the platform");
                }
                if (!is_str(m->val) || !mn_platform_module_id(c.plat, m->val->u.s.p, m->val->u.s.n)) {
                    return schema_fail(err, "builds", "a module identity is not one of the platform");
                }
                ids[k].name = m->key.p;
                ids[k].id = m->val->u.s.p;
            }
            version = get(b, "version");
            if (version != NULL && !is_str(version)) {
                return schema_fail(err, "builds", "a build's `version` must be a string");
            }
            out[i].key = e->key.p;
            out[i].version = version != NULL ? version->u.s.p : e->key.p;
            out[i].modules = ids;
            out[i].module_count = mods->u.t.n;
        }
        info->build_count = builds->u.t.n;
    }

    choices = get(doc, "choices");
    if (choices != NULL && !is_table(choices)) {
        return schema_fail(err, "choices", "`choices` must be a table");
    }
    if (choices != NULL) {
        mncf_choice_list *lists = (mncf_choice_list *)mn_arena_zalloc(ar, (choices->u.t.n + 1) * sizeof *lists);
        if (lists == NULL) {
            goto nomem;
        }
        info->choices = lists;
        for (i = 0; i < choices->u.t.n; i++) {
            mncf_choice *items;
            size_t count;
            if (!choice_list(ar, choices->u.t.e[i].val, &items, &count)) {
                if (ar->failed) {
                    goto nomem;
                }
                return schema_fail(err, "choices", "a choice list must map whole numbers to names");
            }
            lists[i].name = choices->u.t.e[i].key.p;
            lists[i].items = items;
            lists[i].count = count;
        }
        info->choice_list_count = choices->u.t.n;
    }

    cheats = get(doc, "cheat");
    {
        int good = is_array(cheats) && cheats->u.a.n > 0;
        for (i = 0; good && i < cheats->u.a.n; i++) {
            good = is_table(cheats->u.a.items[i]);
        }
        if (!good) {
            return schema_fail(err, "cheat", "the file needs at least one [[cheat]]");
        }
    }
    f->cheat_count = cheats->u.a.n;
    f->cheats = (mn_cheat *)mn_arena_zalloc(ar, f->cheat_count * sizeof *f->cheats);
    if (f->cheats == NULL || !alloc_index(f)) {
        goto nomem;
    }
    for (i = 0; i < f->cheat_count; i++) {
        const mncf_toml *id = get(cheats->u.a.items[i], "id");
        size_t mask = f->cheat_index.cap - 1, at;
        if (!is_str(id) || !is_id(id->u.s.p, id->u.s.n)) {
            char message[64];
            snprintf(message, sizeof message, "cheat %lu has no valid `id`", (unsigned long)(i + 1));
            return schema_fail(err, "cheat.id", message);
        }
        if (mn_cheat_index(f, id->u.s.p, id->u.s.n) != (size_t)-1) {
            char code[300];
            snprintf(code, sizeof code, "duplicate-id:%s", id->u.s.p);
            return file_fail(err, MNCF_ERR_DUPLICATE_ID, code, "two cheats have the same id");
        }
        f->cheats[i].pub.index = i;
        f->cheats[i].pub.id = id->u.s.p;
        f->cheats[i].id_len = id->u.s.n;
        at = (size_t)mn_hash(id->u.s.p, id->u.s.n) & mask;
        while (f->cheat_index.slots[at] != 0) {
            at = (at + 1) & mask;
        }
        f->cheat_index.slots[at] = (uint32_t)(i + 1);
    }
    for (i = 0; i < f->cheat_count; i++) {
        if (!check_cheat(&c, &f->cheats[i], cheats->u.a.items[i])) {
            goto nomem;
        }
    }

    presets = get(doc, "preset");
    if (presets != NULL && !is_array(presets)) {
        return schema_fail(err, "preset", "`preset` must be an array of tables");
    }
    if (presets != NULL) {
        mncf_preset *out = (mncf_preset *)mn_arena_zalloc(ar, (presets->u.a.n + 1) * sizeof *out);
        if (out == NULL) {
            goto nomem;
        }
        info->presets = out;
        for (i = 0; i < presets->u.a.n; i++) {
            const mncf_toml *p = presets->u.a.items[i];
            int r = preset(&c, p, &out[info->preset_count], out, info->preset_count);
            if (r < 0) {
                goto nomem;
            }
            if (r == 0) {
                const mncf_toml *pid = is_table(p) ? get(p, "id") : NULL;
                char label[32];
                snprintf(label, sizeof label, "%lu", (unsigned long)(i + 1));
                if (!warn(&c, "preset:", is_str(pid) ? pid->u.s.p : label, NULL)) {
                    goto nomem;
                }
            } else {
                info->preset_count++;
            }
        }
    }

    i18n = get(doc, "i18n");
    if (i18n != NULL) {
        int good = is_table(i18n);
        for (i = 0; good && i < i18n->u.t.n; i++) {
            good = is_table(i18n->u.t.e[i].val);
        }
        if (good) {
            info->i18n = i18n;
        } else if (!warn(&c, "i18n", "", NULL)) {
            goto nomem;
        }
    }

    {
        mncf_cheat *pub = (mncf_cheat *)mn_arena_alloc(ar, f->cheat_count * sizeof *pub);
        if (pub == NULL) {
            goto nomem;
        }
        for (i = 0; i < f->cheat_count; i++) {
            pub[i] = f->cheats[i].pub;
        }
        info->cheats = pub;
        info->cheat_count = f->cheat_count;
    }
    info->warnings = c.warnings;
    info->warning_count = c.warning_count;
    if (ar->failed) {
        goto nomem;
    }
    return 1;
nomem:
    mn_error_nomem(err);
    return 0;
}
