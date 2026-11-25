/* conformance.c: runs the conformance suite in format/tests against the C library.
 *
 *     mncf_conformance <suite folder>            every case, one line per failure, and a summary
 *     mncf_conformance <suite folder> --dump     each case's results as JSON, as conformance.py --dump
 *
 * It uses only the library's API. The results have the shape of the Python runner's, so that the JSON of
 * --dump can be compared with that of format/lib/python/tests/conformance.py byte for byte (the
 * conversion cases aside: this library has no converter yet). MIT License.
 */
#if !defined(_WIN32) && !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L    /* opendir and readdir under -std=c99 */
#endif

#include "mncf.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#include <windows.h>
#else
#include <dirent.h>
#endif

/* ---- Memory that lives until the end of a case ------------------------------------------------------ */

typedef struct block {
    struct block *next;
} block;

static block *blocks;

static void *xalloc(size_t n)
{
    block *b = (block *)calloc(1, sizeof(block) + n + 16);
    if (b == NULL) {
        fprintf(stderr, "conformance: out of memory\n");
        exit(2);
    }
    b->next = blocks;
    blocks = b;
    return (char *)b + sizeof(block) + (16 - sizeof(block) % 16) % 16;
}

static void xfree_all(void)
{
    while (blocks != NULL) {
        block *next = blocks->next;
        free(blocks);
        blocks = next;
    }
}

static char *xstrndup(const char *s, size_t n)
{
    char *d = (char *)xalloc(n + 1);
    memcpy(d, s, n);
    d[n] = 0;
    return d;
}

#if defined(__GNUC__) || defined(__clang__)
#define PRINTF_LIKE(f, a) __attribute__((format(printf, f, a)))
#else
#define PRINTF_LIKE(f, a)
#endif

static char *xprintf(const char *fmt, ...) PRINTF_LIKE(1, 2);

static char *xprintf(const char *fmt, ...)
{
    va_list ap;
    int n;
    char *s;
    va_start(ap, fmt);
    n = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    s = (char *)xalloc((size_t)n + 1);
    va_start(ap, fmt);
    vsnprintf(s, (size_t)n + 1, fmt, ap);
    va_end(ap);
    return s;
}

/* ---- JSON values, as Python's results ----------------------------------------------------------------- */

enum { J_NULL, J_BOOL, J_INT, J_FLOAT, J_STR, J_LIST, J_DICT };

typedef struct jv {
    int type;
    int b;
    mncf_number num;            /* J_INT, J_FLOAT */
    const char *s;
    size_t n;
    struct jv **items;          /* list items, dict values */
    const char **keys;          /* dict keys */
    size_t *key_lens;
    size_t count, cap;
} jv;

static jv *j_new(int type)
{
    jv *v = (jv *)xalloc(sizeof *v);
    v->type = type;
    return v;
}

static jv *j_str(const char *s)
{
    jv *v = j_new(J_STR);
    v->s = s != NULL ? s : "";
    v->n = strlen(v->s);
    return v;
}

static jv *j_strn(const char *s, size_t n)
{
    jv *v = j_new(J_STR);
    v->s = s;
    v->n = n;
    return v;
}

static jv *j_bool(int b)
{
    jv *v = j_new(J_BOOL);
    v->b = b != 0;
    return v;
}

static jv *j_uint(uint64_t x)
{
    jv *v = j_new(J_INT);
    v->num = mncf_number_uint(x);
    return v;
}

static jv *j_number(mncf_number n)
{
    jv *v;
    if (n.kind == MNCF_NUMBER_BOOL) {
        return j_bool(n.magnitude != 0);
    }
    v = j_new(n.kind == MNCF_NUMBER_FLOAT ? J_FLOAT : J_INT);
    v->num = n;
    return v;
}

static void j_grow(jv *v)
{
    if (v->count == v->cap) {
        size_t cap = v->cap == 0 ? 8 : v->cap * 2;
        jv **items = (jv **)xalloc(cap * sizeof *items);
        const char **keys = (const char **)xalloc(cap * sizeof *keys);
        size_t *lens = (size_t *)xalloc(cap * sizeof *lens);
        if (v->count > 0) {
            memcpy(items, v->items, v->count * sizeof *items);
            if (v->keys != NULL) {
                memcpy(keys, v->keys, v->count * sizeof *keys);
                memcpy(lens, v->key_lens, v->count * sizeof *lens);
            }
        }
        v->items = items;
        v->keys = keys;
        v->key_lens = lens;
        v->cap = cap;
    }
}

static void j_push(jv *list, jv *item)
{
    j_grow(list);
    list->items[list->count++] = item;
}

static void j_setn(jv *dict, const char *key, size_t n, jv *value)
{
    size_t i;
    for (i = 0; i < dict->count; i++) {
        if (dict->key_lens[i] == n && memcmp(dict->keys[i], key, n) == 0) {
            dict->items[i] = value;
            return;
        }
    }
    j_grow(dict);
    dict->keys[dict->count] = key;
    dict->key_lens[dict->count] = n;
    dict->items[dict->count++] = value;
}

static void j_set(jv *dict, const char *key, jv *value)
{
    j_setn(dict, key, strlen(key), value);
}

static jv *j_get(const jv *dict, const char *key)
{
    size_t i, n = strlen(key);
    if (dict == NULL || dict->type != J_DICT) {
        return NULL;
    }
    for (i = 0; i < dict->count; i++) {
        if (dict->key_lens[i] == n && memcmp(dict->keys[i], key, n) == 0) {
            return dict->items[i];
        }
    }
    return NULL;
}

/* A TOML value of a case as a JSON value. */
static jv *j_from_toml(const mncf_toml *t)
{
    size_t i, n;
    jv *v;
    switch (mncf_toml_type_of(t)) {
    case MNCF_TOML_STRING:
    case MNCF_TOML_DATETIME: {
        const char *s = mncf_toml_string(t, &n);
        return j_strn(s, n);
    }
    case MNCF_TOML_INTEGER:
        return j_number(mncf_number_int(mncf_toml_integer(t)));
    case MNCF_TOML_FLOAT:
        return j_number(mncf_number_float(mncf_toml_float(t)));
    case MNCF_TOML_BOOLEAN:
        return j_bool(mncf_toml_boolean(t));
    case MNCF_TOML_ARRAY:
        v = j_new(J_LIST);
        for (i = 0; i < mncf_toml_count(t); i++) {
            j_push(v, j_from_toml(mncf_toml_at(t, i)));
        }
        return v;
    case MNCF_TOML_TABLE:
        v = j_new(J_DICT);
        for (i = 0; i < mncf_toml_count(t); i++) {
            const char *k = mncf_toml_key(t, i, &n);
            j_setn(v, k, n, j_from_toml(mncf_toml_at(t, i)));
        }
        return v;
    default:
        return j_new(J_NULL);
    }
}

/* ---- Python's == and the runner's comparisons -------------------------------------------------------- */

static int num_eq(const jv *a, const jv *b)
{
    mncf_number x = a->type == J_BOOL ? mncf_number_uint((uint64_t)a->b) : a->num;
    mncf_number y = b->type == J_BOOL ? mncf_number_uint((uint64_t)b->b) : b->num;
    if (x.kind == MNCF_NUMBER_INT && y.kind == MNCF_NUMBER_INT) {
        return x.negative == y.negative && x.magnitude == y.magnitude;
    }
    if (x.kind == MNCF_NUMBER_FLOAT && y.kind == MNCF_NUMBER_FLOAT) {
        return x.real == y.real;
    }
    {
        const mncf_number *i = x.kind == MNCF_NUMBER_INT ? &x : &y;
        const mncf_number *f = x.kind == MNCF_NUMBER_INT ? &y : &x;
        double d = f->real;
        if (d != d || d != floor(d) || fabs(d) >= 18446744073709551616.0) {
            return 0;
        }
        return (d < 0) == (i->negative != 0) && (uint64_t)fabs(d) == i->magnitude;
    }
}

static int is_num(const jv *v)
{
    return v != NULL && (v->type == J_INT || v->type == J_FLOAT || v->type == J_BOOL);
}

/* Python's == for the values of a TOML document and of the results. */
static int py_eq(const jv *a, const jv *b)
{
    size_t i;
    if (a == NULL || b == NULL) {
        return a == b;
    }
    if (is_num(a) && is_num(b)) {
        return num_eq(a, b);
    }
    if (a->type != b->type) {
        return 0;
    }
    switch (a->type) {
    case J_NULL:
        return 1;
    case J_STR:
        return a->n == b->n && memcmp(a->s, b->s, a->n) == 0;
    case J_LIST:
        if (a->count != b->count) {
            return 0;
        }
        for (i = 0; i < a->count; i++) {
            if (!py_eq(a->items[i], b->items[i])) {
                return 0;
            }
        }
        return 1;
    case J_DICT:
        if (a->count != b->count) {
            return 0;
        }
        for (i = 0; i < a->count; i++) {
            jv *other = NULL;
            size_t k;
            for (k = 0; k < b->count; k++) {
                if (b->key_lens[k] == a->key_lens[i] && memcmp(b->keys[k], a->keys[i], a->key_lens[i]) == 0) {
                    other = b->items[k];
                }
            }
            if (other == NULL || !py_eq(a->items[i], other)) {
                return 0;
            }
        }
        return 1;
    default:
        return 0;
    }
}

static int upper_eq(const jv *a, const jv *b)
{
    size_t i;
    if (a->n != b->n) {
        return 0;
    }
    for (i = 0; i < a->n; i++) {
        int x = (unsigned char)a->s[i], y = (unsigned char)b->s[i];
        if (x >= 'a' && x <= 'z') {
            x -= 32;
        }
        if (y >= 'a' && y <= 'z') {
            y -= 32;
        }
        if (x != y) {
            return 0;
        }
    }
    return 1;
}

/* The runner's _same: bytes in any case, booleans only equal to booleans. */
static int same(const jv *expected, const jv *actual, const char *key)
{
    size_t i;
    if (actual == NULL) {
        return 0;
    }
    if (key != NULL && (strcmp(key, "bytes") == 0 || strcmp(key, "expect") == 0) && expected->type == J_STR &&
        actual->type == J_STR) {
        return upper_eq(expected, actual);
    }
    if (expected->type == J_DICT) {
        if (actual->type != J_DICT || actual->count != expected->count) {
            return 0;
        }
        for (i = 0; i < expected->count; i++) {
            char *k = xstrndup(expected->keys[i], expected->key_lens[i]);
            if (!same(expected->items[i], j_get(actual, k), k)) {
                return 0;
            }
        }
        return 1;
    }
    if (expected->type == J_LIST) {
        if (actual->type != J_LIST || actual->count != expected->count) {
            return 0;
        }
        for (i = 0; i < expected->count; i++) {
            if (!same(expected->items[i], actual->items[i], NULL)) {
                return 0;
            }
        }
        return 1;
    }
    if (expected->type == J_BOOL || actual->type == J_BOOL) {
        return expected->type == J_BOOL && actual->type == J_BOOL && expected->b == actual->b;
    }
    return py_eq(expected, actual);
}

/* ---- Writing JSON as json.dumps(indent=1, sort_keys=True) ------------------------------------------- */

typedef struct outbuf {
    char *data;
    size_t size, cap;
} outbuf;

static void put(outbuf *o, const char *s, size_t n)
{
    if (o->size + n + 1 > o->cap) {
        size_t cap = o->cap == 0 ? 65536 : o->cap;
        while (cap < o->size + n + 1) {
            cap *= 2;
        }
        o->data = (char *)realloc(o->data, cap);
        if (o->data == NULL) {
            fprintf(stderr, "conformance: out of memory\n");
            exit(2);
        }
        o->cap = cap;
    }
    memcpy(o->data + o->size, s, n);
    o->size += n;
    o->data[o->size] = 0;
}

static void puts_(outbuf *o, const char *s)
{
    put(o, s, strlen(s));
}

static void indent(outbuf *o, int level)
{
    int i;
    for (i = 0; i < level; i++) {
        put(o, " ", 1);
    }
}

/* A string as json.dumps writes it with ensure_ascii: escapes for '"', '\', control characters and
 * everything outside space to '~', astral characters as surrogate pairs. */
static void json_string(outbuf *o, const char *s, size_t n)
{
    size_t i = 0;
    put(o, "\"", 1);
    while (i < n) {
        unsigned char c = (unsigned char)s[i];
        uint32_t cp;
        size_t len = 1;
        char buf[16];
        if (c < 0x80) {
            cp = c;
        } else if (c < 0xE0 && i + 1 < n) {
            cp = ((uint32_t)(c & 0x1F) << 6) | ((unsigned char)s[i + 1] & 0x3F);
            len = 2;
        } else if (c < 0xF0 && i + 2 < n) {
            cp = ((uint32_t)(c & 0x0F) << 12) | ((uint32_t)((unsigned char)s[i + 1] & 0x3F) << 6) |
                 ((unsigned char)s[i + 2] & 0x3F);
            len = 3;
        } else if (i + 3 < n) {
            cp = ((uint32_t)(c & 0x07) << 18) | ((uint32_t)((unsigned char)s[i + 1] & 0x3F) << 12) |
                 ((uint32_t)((unsigned char)s[i + 2] & 0x3F) << 6) | ((unsigned char)s[i + 3] & 0x3F);
            len = 4;
        } else {
            cp = c;
        }
        i += len;
        switch (cp) {
        case '"':
            puts_(o, "\\\"");
            continue;
        case '\\':
            puts_(o, "\\\\");
            continue;
        case '\n':
            puts_(o, "\\n");
            continue;
        case '\r':
            puts_(o, "\\r");
            continue;
        case '\t':
            puts_(o, "\\t");
            continue;
        case '\b':
            puts_(o, "\\b");
            continue;
        case '\f':
            puts_(o, "\\f");
            continue;
        default:
            break;
        }
        if (cp >= 0x20 && cp <= 0x7E) {
            char ch = (char)cp;
            put(o, &ch, 1);
        } else if (cp < 0x10000) {
            snprintf(buf, sizeof buf, "\\u%04x", (unsigned)cp);
            puts_(o, buf);
        } else {
            uint32_t v = cp - 0x10000;
            snprintf(buf, sizeof buf, "\\u%04x\\u%04x", (unsigned)(0xD800 + (v >> 10)), (unsigned)(0xDC00 + (v & 0x3FF)));
            puts_(o, buf);
        }
    }
    put(o, "\"", 1);
}

/* The shortest text that reads back as the same double, laid out as Python's repr() of a float. */
static void float_repr(outbuf *o, double d)
{
    char buf[64], digits[32];
    int precision, exponent = 0, nd = 0, point;
    const char *p;
    if (d != d) {
        puts_(o, "NaN");
        return;
    }
    if (isinf(d)) {
        puts_(o, d > 0 ? "Infinity" : "-Infinity");
        return;
    }
    if (d == 0) {
        puts_(o, signbit(d) ? "-0.0" : "0.0");
        return;
    }
    for (precision = 1; precision <= 17; precision++) {
        snprintf(buf, sizeof buf, "%.*e", precision - 1, d);
        if (strtod(buf, NULL) == d) {
            break;
        }
    }
    /* buf is [-]d[.ddd]e[+-]xx: take the digits and the exponent. */
    p = buf;
    if (*p == '-') {
        put(o, "-", 1);
        p++;
    }
    while (*p != 'e' && *p != 0) {
        if (*p >= '0' && *p <= '9') {
            digits[nd++] = *p;
        }
        p++;
    }
    if (*p == 'e') {
        exponent = atoi(p + 1);
    }
    while (nd > 1 && digits[nd - 1] == '0') {
        nd--;
    }
    digits[nd] = 0;
    point = exponent + 1;          /* the value is 0.<digits> times ten to the power point */
    if (point <= -4 || point > 16) {
        put(o, digits, 1);
        if (nd > 1) {
            put(o, ".", 1);
            put(o, digits + 1, (size_t)nd - 1);
        }
        snprintf(buf, sizeof buf, "e%c%02d", point - 1 < 0 ? '-' : '+', abs(point - 1));
        puts_(o, buf);
    } else if (point <= 0) {
        int i;
        puts_(o, "0.");
        for (i = 0; i < -point; i++) {
            put(o, "0", 1);
        }
        put(o, digits, (size_t)nd);
    } else if (point >= nd) {
        int i;
        put(o, digits, (size_t)nd);
        for (i = nd; i < point; i++) {
            put(o, "0", 1);
        }
        puts_(o, ".0");
    } else {
        put(o, digits, (size_t)point);
        put(o, ".", 1);
        put(o, digits + point, (size_t)(nd - point));
    }
}

static int cmp_keys(const char *a, size_t an, const char *b, size_t bn)
{
    size_t n = an < bn ? an : bn;
    int c = n > 0 ? memcmp(a, b, n) : 0;
    if (c != 0) {
        return c;
    }
    return an < bn ? -1 : an > bn;
}

static void json_write(outbuf *o, const jv *v, int level)
{
    size_t i;
    char buf[64];
    switch (v->type) {
    case J_NULL:
        puts_(o, "null");
        return;
    case J_BOOL:
        puts_(o, v->b ? "true" : "false");
        return;
    case J_INT:
        snprintf(buf, sizeof buf, "%s%llu", v->num.negative ? "-" : "", (unsigned long long)v->num.magnitude);
        puts_(o, buf);
        return;
    case J_FLOAT:
        float_repr(o, v->num.real);
        return;
    case J_STR:
        json_string(o, v->s, v->n);
        return;
    case J_LIST:
        if (v->count == 0) {
            puts_(o, "[]");
            return;
        }
        put(o, "[", 1);
        for (i = 0; i < v->count; i++) {
            puts_(o, i == 0 ? "\n" : ",\n");
            indent(o, level + 1);
            json_write(o, v->items[i], level + 1);
        }
        put(o, "\n", 1);
        indent(o, level);
        put(o, "]", 1);
        return;
    default: {
        size_t *order;
        if (v->count == 0) {
            puts_(o, "{}");
            return;
        }
        order = (size_t *)xalloc(v->count * sizeof *order);
        for (i = 0; i < v->count; i++) {
            size_t k = i;
            order[i] = i;
            while (k > 0 && cmp_keys(v->keys[order[k - 1]], v->key_lens[order[k - 1]], v->keys[order[k]],
                                     v->key_lens[order[k]]) > 0) {
                size_t t = order[k];
                order[k] = order[k - 1];
                order[k - 1] = t;
                k--;
            }
        }
        put(o, "{", 1);
        for (i = 0; i < v->count; i++) {
            puts_(o, i == 0 ? "\n" : ",\n");
            indent(o, level + 1);
            json_string(o, v->keys[order[i]], v->key_lens[order[i]]);
            puts_(o, ": ");
            json_write(o, v->items[order[i]], level + 1);
        }
        put(o, "\n", 1);
        indent(o, level);
        put(o, "}", 1);
    }
    }
}

/* ---- Files ------------------------------------------------------------------------------------------ */

static char *read_file(const char *path, size_t *size)
{
    FILE *f = fopen(path, "rb");
    char *data;
    long n;
    if (f == NULL) {
        return NULL;
    }
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fseek(f, 0, SEEK_SET);
    data = (char *)xalloc((size_t)n + 1);
    if (n > 0 && fread(data, 1, (size_t)n, f) != (size_t)n) {
        fclose(f);
        return NULL;
    }
    fclose(f);
    data[n] = 0;
    *size = (size_t)n;
    return data;
}

/* A text file as Python's read_text reads it: each CR LF, and each CR alone, becomes a line feed. */
static char *read_text(const char *path, size_t *size)
{
    size_t n, i, k = 0;
    char *data = read_file(path, &n);
    if (data == NULL) {
        return NULL;
    }
    for (i = 0; i < n; i++) {
        if (data[i] == '\r') {
            data[k++] = '\n';
            if (i + 1 < n && data[i + 1] == '\n') {
                i++;
            }
        } else {
            data[k++] = data[i];
        }
    }
    data[k] = 0;
    *size = k;
    return data;
}

static int cmp_strings(const void *a, const void *b)
{
    return strcmp(*(const char *const *)a, *(const char *const *)b);
}

/* The .toml files of a folder, sorted by name. */
static char **list_cases(const char *folder, size_t *count)
{
    char **names = NULL;
    size_t n = 0, cap = 0;
#ifdef _WIN32
    WIN32_FIND_DATAA fd;
    char *pattern = xprintf("%s\\*.toml", folder);
    HANDLE h = FindFirstFileA(pattern, &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            size_t len = strlen(fd.cFileName);
            if (len > 5 && strcmp(fd.cFileName + len - 5, ".toml") == 0) {
                if (n == cap) {
                    char **grown;
                    cap = cap == 0 ? 64 : cap * 2;
                    grown = (char **)xalloc(cap * sizeof *grown);
                    if (n > 0) {
                        memcpy(grown, names, n * sizeof *grown);
                    }
                    names = grown;
                }
                names[n++] = xstrndup(fd.cFileName, len - 5);
            }
        } while (FindNextFileA(h, &fd));
        FindClose(h);
    }
#else
    DIR *d = opendir(folder);
    struct dirent *e;
    if (d != NULL) {
        while ((e = readdir(d)) != NULL) {
            size_t len = strlen(e->d_name);
            if (len > 5 && strcmp(e->d_name + len - 5, ".toml") == 0) {
                if (n == cap) {
                    char **grown;
                    cap = cap == 0 ? 64 : cap * 2;
                    grown = (char **)xalloc(cap * sizeof *grown);
                    if (n > 0) {
                        memcpy(grown, names, n * sizeof *grown);
                    }
                    names = grown;
                }
                names[n++] = xstrndup(e->d_name, len - 5);
            }
        }
        closedir(d);
    }
#endif
    if (n > 0) {
        qsort(names, n, sizeof *names, cmp_strings);
    }
    *count = n;
    return names;
}

/* ---- Bytes of a case -------------------------------------------------------------------------------- */

static int hexval(int c)
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

/* bytes.fromhex: pairs of hexadecimal digits, with white space between them. */
static uint8_t *from_hex(const char *s, size_t n, size_t *len)
{
    uint8_t *out = (uint8_t *)xalloc(n / 2 + 1);
    size_t i = 0, k = 0;
    while (i < n) {
        if (s[i] == ' ' || s[i] == '\t' || s[i] == '\n' || s[i] == '\r' || s[i] == '\v' || s[i] == '\f') {
            i++;
            continue;
        }
        if (i + 1 >= n || hexval((unsigned char)s[i]) < 0 || hexval((unsigned char)s[i + 1]) < 0) {
            fprintf(stderr, "conformance: bad hex in a case\n");
            exit(2);
        }
        out[k++] = (uint8_t)(hexval((unsigned char)s[i]) << 4 | hexval((unsigned char)s[i + 1]));
        i += 2;
    }
    *len = k;
    return out;
}

static const char *tstr(const mncf_toml *v, size_t *n)
{
    return mncf_toml_type_of(v) == MNCF_TOML_STRING ? mncf_toml_string(v, n) : NULL;
}

static const char *tstrz(const mncf_toml *v)
{
    size_t n;
    const char *s = tstr(v, &n);
    return s != NULL ? xstrndup(s, n) : NULL;
}

static uint64_t tuint(const mncf_toml *v, uint64_t fallback)
{
    return mncf_toml_type_of(v) == MNCF_TOML_INTEGER ? (uint64_t)mncf_toml_integer(v) : fallback;
}

static const uint8_t *case_bytes(const mncf_toml *c, const char *folder, size_t *size)
{
    const mncf_toml *v;
    size_t n;
    const char *s;
    if ((v = mncf_toml_get(c, "file")) != NULL) {
        char *path = xprintf("%s/%s", folder, tstrz(v));
        const uint8_t *data = (const uint8_t *)read_file(path, size);
        if (data == NULL) {
            fprintf(stderr, "conformance: cannot read %s\n", path);
            exit(2);
        }
        return data;
    }
    if ((v = mncf_toml_get(c, "hex")) != NULL) {
        s = tstr(v, &n);
        return from_hex(s, n, size);
    }
    s = tstr(mncf_toml_get(c, "text"), &n);
    *size = n;
    return (const uint8_t *)(s != NULL ? s : "");
}

static long b64(const char *s, size_t n, uint8_t *out, size_t cap)
{
    static const char A[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    uint32_t acc = 0;
    int bits = 0;
    size_t i, k = 0;
    for (i = 0; i < n; i++) {
        const char *p = strchr(A, s[i]);
        if (s[i] == '=' || p == NULL) {
            continue;
        }
        acc = acc << 6 | (uint32_t)(p - A);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            if (k < cap) {
                out[k] = (uint8_t)(acc >> bits);
            }
            k++;
        }
    }
    return (long)k;
}

static mncf_trust *trust_list(const mncf_toml *c)
{
    mncf_trust *t = mncf_trust_new(NULL);
    const mncf_toml *trust = mncf_toml_get(c, "trust");
    const mncf_toml *keys = mncf_toml_get(trust, "key"), *revoked = mncf_toml_get(trust, "revoked");
    size_t i;
    for (i = 0; i < mncf_toml_count(keys); i++) {
        const mncf_toml *k = mncf_toml_at(keys, i);
        size_t n;
        const char *pub = tstr(mncf_toml_get(k, "public"), &n);
        uint8_t raw[64];
        if (pub != NULL && b64(pub, n, raw, sizeof raw) == 32) {
            mncf_trust_add(t, tstrz(mncf_toml_get(k, "name")), raw);
        }
    }
    for (i = 0; i < mncf_toml_count(revoked); i++) {
        mncf_trust_revoke(t, tstrz(mncf_toml_at(revoked, i)));
    }
    return t;
}

/* ---- Guest memory of a case ------------------------------------------------------------------------- */

typedef struct chunk {
    uint64_t start;
    uint8_t *data;
    size_t size;
} chunk;

typedef struct memory {
    chunk *chunks;
    size_t count, cap;
} memory;

static void mem_add(memory *m, uint64_t start, const uint8_t *data, size_t size)
{
    chunk *c;
    if (m->count == m->cap) {
        size_t cap = m->cap == 0 ? 16 : m->cap * 2;
        chunk *grown = (chunk *)xalloc(cap * sizeof *grown);
        if (m->count > 0) {
            memcpy(grown, m->chunks, m->count * sizeof *grown);
        }
        m->chunks = grown;
        m->cap = cap;
    }
    c = &m->chunks[m->count++];
    c->start = start;
    c->size = size;
    c->data = (uint8_t *)xalloc(size + 1);
    if (data != NULL && size > 0) {
        memcpy(c->data, data, size);
    }
}

static chunk *mem_find(const memory *m, uint64_t at, size_t size)
{
    size_t i;
    for (i = 0; i < m->count; i++) {
        chunk *c = &m->chunks[i];
        if (c->start <= at && at - c->start <= c->size && size <= c->size - (size_t)(at - c->start)) {
            return c;
        }
    }
    return NULL;
}

/* Writes bytes into the piece they lie in, or makes a piece of their own. */
static void mem_put(memory *m, uint64_t at, const uint8_t *raw, size_t size)
{
    chunk *c = mem_find(m, at, size);
    if (c != NULL) {
        if (size > 0) {
            memcpy(c->data + (at - c->start), raw, size);
        }
        return;
    }
    mem_add(m, at, raw, size);
}

static int mem_read(void *ctx, uint64_t address, void *outp, size_t size)
{
    chunk *c = mem_find((const memory *)ctx, address, size);
    if (c == NULL) {
        return 0;
    }
    if (size > 0) {
        memcpy(outp, c->data + (address - c->start), size);
    }
    return 1;
}

static void put_pieces(memory *m, const mncf_toml *pieces)
{
    size_t i;
    for (i = 0; i < mncf_toml_count(pieces); i++) {
        const mncf_toml *p = mncf_toml_at(pieces, i);
        uint64_t at = tuint(mncf_toml_get(p, "at"), 0);
        const mncf_toml *hex = mncf_toml_get(p, "hex");
        if (hex != NULL) {
            size_t n, len;
            const char *s = tstr(hex, &n);
            const uint8_t *raw = from_hex(s, n, &len);
            mem_put(m, at, raw, len);
        } else {
            size_t size = (size_t)tuint(mncf_toml_get(p, "size"), 0);
            uint8_t *zero = (uint8_t *)xalloc(size + 1);
            mem_put(m, at, zero, size);
        }
    }
}

static const char **string_list(const mncf_toml *v, size_t *count)
{
    size_t i, n = mncf_toml_count(v);
    const char **out = (const char **)xalloc((n + 1) * sizeof *out);
    for (i = 0; i < n; i++) {
        out[i] = tstrz(mncf_toml_at(v, i));
    }
    *count = n;
    return out;
}

static unsigned capability_bits(const mncf_toml *v)
{
    static const char *const NAMES[] = {"guest-read", "guest-write", "code-bytes", "code-hook", "buttons", "display"};
    unsigned bits = 0;
    size_t i, k;
    for (i = 0; i < mncf_toml_count(v); i++) {
        const char *s = tstrz(mncf_toml_at(v, i));
        for (k = 0; k < 6; k++) {
            if (s != NULL && strcmp(s, NAMES[k]) == 0) {
                bits |= 1u << k;
            }
        }
    }
    return bits;
}

/* The runtime of a case's [env] table, with its guest memory in m. */
static void runtime(const mncf_toml *env, memory *m, mncf_runtime *rt)
{
    const mncf_toml *modules = mncf_toml_get(env, "modules"), *regions = mncf_toml_get(env, "regions");
    const mncf_toml *v;
    size_t i, k;
    mncf_module *mods = (mncf_module *)xalloc((mncf_toml_count(modules) + 1) * sizeof *mods);
    mncf_region *regs = (mncf_region *)xalloc((mncf_toml_count(regions) + 1) * sizeof *regs);
    const char *platform = tstrz(mncf_toml_get(env, "platform"));
    mncf_runtime_init(rt, platform != NULL ? platform : "switch");
    for (i = 0; i < mncf_toml_count(modules); i++) {
        const mncf_toml *mt = mncf_toml_at(modules, i);
        mncf_module *md = &mods[i];
        size_t n, size = (size_t)tuint(mncf_toml_get(mt, "size"), 0);
        uint8_t *data = (uint8_t *)xalloc(size + 1);
        const mncf_toml *bytes = mncf_toml_get(mt, "bytes");
        const char *key = mncf_toml_key(modules, i, &n);
        md->name = xstrndup(key, n);
        md->id = tstrz(mncf_toml_get(mt, "id"));
        md->base = tuint(mncf_toml_get(mt, "base"), 0);
        for (k = 0; k < mncf_toml_count(bytes); k++) {
            const mncf_toml *piece = mncf_toml_at(bytes, k);
            size_t hn, len;
            const char *s = tstr(mncf_toml_get(piece, "hex"), &hn);
            const uint8_t *raw = from_hex(s, hn, &len);
            uint64_t at = tuint(mncf_toml_get(piece, "at"), 0);
            if (at + len <= size) {
                memcpy(data + at, raw, len);
            }
        }
        md->data = data;
        md->data_size = size;
        md->size = size;
        md->has_size = 1;
        if ((v = mncf_toml_get(mt, "code")) != NULL) {
            md->has_code = 1;
            md->code_start = tuint(mncf_toml_at(v, 0), 0);
            md->code_end = tuint(mncf_toml_at(v, 1), 0);
        }
        if ((v = mncf_toml_get(mt, "rodata")) != NULL) {
            md->has_rodata = 1;
            md->rodata_start = tuint(mncf_toml_at(v, 0), 0);
            md->rodata_end = tuint(mncf_toml_at(v, 1), 0);
        }
        if ((v = mncf_toml_get(mt, "data")) != NULL) {
            md->has_data = 1;
            md->data_start = tuint(mncf_toml_at(v, 0), 0);
            md->data_end = tuint(mncf_toml_at(v, 1), 0);
        }
        mem_add(m, md->base, data, size);
    }
    for (i = 0; i < mncf_toml_count(regions); i++) {
        const mncf_toml *r = mncf_toml_at(regions, i);
        size_t n;
        const char *key = mncf_toml_key(regions, i, &n);
        regs[i].name = xstrndup(key, n);
        regs[i].base = tuint(mncf_toml_get(r, "base"), 0);
        regs[i].size = tuint(mncf_toml_get(r, "size"), 0);
        mem_add(m, regs[i].base, NULL, (size_t)regs[i].size);
    }
    put_pieces(m, mncf_toml_get(env, "memory"));
    rt->game_id = tstrz(mncf_toml_get(env, "game"));
    rt->arch = tstrz(mncf_toml_get(env, "arch"));
    rt->modules = mods;
    rt->module_count = mncf_toml_count(modules);
    rt->regions = regs;
    rt->region_count = mncf_toml_count(regions);
    rt->read = mem_read;
    rt->read_ctx = m;
    if ((v = mncf_toml_get(env, "capabilities")) != NULL) {
        rt->capabilities = capability_bits(v);
    }
    if ((v = mncf_toml_get(env, "extensions")) != NULL) {
        rt->extensions = string_list(v, &rt->extension_count);
    }
    if ((v = mncf_toml_get(env, "raw")) != NULL) {
        rt->raw_syntaxes = string_list(v, &rt->raw_syntax_count);
    }
    rt->build = tstrz(mncf_toml_get(env, "build"));
    if ((v = mncf_toml_get(env, "held")) != NULL) {
        rt->held = string_list(v, &rt->held_count);
    }
    if ((v = mncf_toml_get(env, "unsupported")) != NULL) {
        rt->unsupported = string_list(v, &rt->unsupported_count);
    }
    if ((v = mncf_toml_get(env, "rewritten")) != NULL) {
        size_t total = 0, n = 0;
        mncf_rewritten *runs;
        for (i = 0; i < mncf_toml_count(v); i++) {
            total += mncf_toml_count(mncf_toml_at(v, i));
        }
        runs = (mncf_rewritten *)xalloc((total + 1) * sizeof *runs);
        for (i = 0; i < mncf_toml_count(v); i++) {
            size_t kn;
            const char *key = mncf_toml_key(v, i, &kn);
            const mncf_toml *list = mncf_toml_at(v, i);
            for (k = 0; k < mncf_toml_count(list); k++) {
                const mncf_toml *pair = mncf_toml_at(list, k);
                runs[n].module = xstrndup(key, kn);
                runs[n].offset = tuint(mncf_toml_at(pair, 0), 0);
                runs[n].length = tuint(mncf_toml_at(pair, 1), 0);
                n++;
            }
        }
        rt->rewritten = runs;
        rt->rewritten_count = n;
    }
}

/* A value of a case for a player value: whole numbers, floats and booleans as they are, anything else
 * as no number. */
static mncf_number toml_number(const mncf_toml *v)
{
    mncf_number n;
    switch (mncf_toml_type_of(v)) {
    case MNCF_TOML_INTEGER:
        return mncf_number_int(mncf_toml_integer(v));
    case MNCF_TOML_FLOAT:
        return mncf_number_float(mncf_toml_float(v));
    case MNCF_TOML_BOOLEAN:
        return mncf_number_bool(mncf_toml_boolean(v));
    default:
        memset(&n, 0, sizeof n);
        n.kind = MNCF_NUMBER_NONE;
        return n;
    }
}

static void choices_of(const mncf_toml *env, mncf_choices *ch)
{
    const mncf_toml *values = mncf_toml_get(env, "values");
    size_t i, n = mncf_toml_count(values);
    mncf_choice_value *vals = (mncf_choice_value *)xalloc((n + 1) * sizeof *vals);
    memset(ch, 0, sizeof *ch);
    ch->on = string_list(mncf_toml_get(env, "on"), &ch->on_count);
    ch->off = string_list(mncf_toml_get(env, "off"), &ch->off_count);
    for (i = 0; i < n; i++) {
        size_t kn;
        const char *key = mncf_toml_key(values, i, &kn);
        vals[i].key = xstrndup(key, kn);
        vals[i].value = toml_number(mncf_toml_at(values, i));
    }
    ch->values = vals;
    ch->value_count = n;
}

/* ---- Results ---------------------------------------------------------------------------------------- */

static const char *hex_bytes(const uint8_t *p, size_t n)
{
    static const char H[] = "0123456789ABCDEF";
    char *s = (char *)xalloc(n * 3 + 1);
    size_t i, k = 0;
    for (i = 0; i < n; i++) {
        if (i > 0) {
            s[k++] = ' ';
        }
        s[k++] = H[p[i] >> 4];
        s[k++] = H[p[i] & 15];
    }
    s[k] = 0;
    return s;
}

static jv *op_dict(const mncf_op *op)
{
    jv *d = j_new(J_DICT);
    j_set(d, "kind", j_str(mncf_op_name(op->kind)));
    if (!op->resolved) {
        return d;
    }
    if (op->kind == MNCF_OP_RAW) {
        j_set(d, "syntax", j_str(op->syntax));
        j_set(d, "build", j_str(op->build));
        j_set(d, "code", j_str(op->code));
        return d;
    }
    j_set(d, "address", j_uint(op->address));
    if (op->module != NULL) {
        j_set(d, "module", j_str(op->module));
        j_set(d, "offset", j_uint(op->offset));
    }
    if (op->type != MNCF_TYPE_NONE) {
        j_set(d, "type", j_str(mncf_type_name(op->type)));
    }
    if (op->expect != NULL) {
        j_set(d, "expect", j_str(hex_bytes(op->expect, op->expect_size)));
    }
    if (op->bytes != NULL) {
        j_set(d, "bytes", j_str(hex_bytes(op->bytes, op->size)));
    }
    if (op->kind == MNCF_OP_OVERRIDE) {
        j_set(d, "action", j_str(op->action == MNCF_ACTION_RETURN ? "return" : "skip"));
        if (op->has_value) {
            j_set(d, "value", j_number(op->value));
        }
    }
    if (op->kind == MNCF_OP_WHEN) {
        j_set(d, "op", j_str(mncf_compare_name(op->compare)));
        if (op->value_kind == MNCF_WHEN_NUMBER) {
            j_set(d, "value", j_number(op->value));
        } else if (op->value_kind == MNCF_WHEN_RANGE) {
            jv *l = j_new(J_LIST);
            j_push(l, j_number(op->low));
            j_push(l, j_number(op->high));
            j_set(d, "value", l);
        } else if (op->value_kind == MNCF_WHEN_PLACE) {
            jv *p = j_new(J_DICT);
            j_set(p, "address", j_uint(op->other_address));
            j_set(p, "type", j_str(mncf_type_name(op->other_type)));
            j_set(d, "value", p);
        }
    }
    if (op->label != NULL) {
        j_set(d, "label", j_str(op->label));
    }
    return d;
}

static jv *signature_signers(const mncf_signature *s)
{
    jv *l = j_new(J_LIST);
    size_t i;
    for (i = 0; i < s->signer_count; i++) {
        j_push(l, j_str(xprintf("%s", s->signers[i])));
    }
    return l;
}

static jv *error_result(const mncf_error *err)
{
    jv *d = j_new(J_DICT);
    j_set(d, "error", j_str(xprintf("%s", err->code)));
    return d;
}

static int cmp_warnings(const void *a, const void *b)
{
    return strcmp(*(const char *const *)a, *(const char *const *)b);
}

static jv *file_results(const mncf_toml *c, const char *folder)
{
    size_t size, i, k;
    const uint8_t *data = case_bytes(c, folder, &size);
    mncf_trust *trust = trust_list(c);
    mncf_error err;
    mncf_file *f = mncf_load(data, size, trust, NULL, &err);
    const mncf_toml *env = mncf_toml_get(c, "env");
    const mncf_file_info *info;
    memory m;
    mncf_runtime rt;
    mncf_choices ch;
    mncf_evaluation *ev;
    jv *out, *cheats, *warnings;
    const char **sorted;
    mncf_trust_free(trust);
    if (f == NULL) {
        return error_result(&err);
    }
    memset(&m, 0, sizeof m);
    runtime(env, &m, &rt);
    choices_of(env, &ch);
    ev = mncf_evaluate(f, &rt, &ch, &err);
    if (ev == NULL) {
        fprintf(stderr, "conformance: evaluate failed: %s\n", err.message);
        exit(2);
    }
    info = mncf_file_info_of(f);
    out = j_new(J_DICT);
    j_set(out, "signature", j_str(mncf_signature_name(info->signature.result)));
    j_set(out, "signers", signature_signers(&info->signature));
    j_set(out, "build", j_str(mncf_evaluation_build(ev) != NULL ? xprintf("%s", mncf_evaluation_build(ev)) : ""));
    sorted = (const char **)xalloc((info->warning_count + 1) * sizeof *sorted);
    for (i = 0; i < info->warning_count; i++) {
        sorted[i] = xprintf("%s", info->warnings[i]);
    }
    qsort(sorted, info->warning_count, sizeof *sorted, cmp_warnings);
    warnings = j_new(J_LIST);
    for (i = 0; i < info->warning_count; i++) {
        j_push(warnings, j_str(sorted[i]));
    }
    j_set(out, "warnings", warnings);
    cheats = j_new(J_DICT);
    for (i = 0; i < mncf_evaluation_count(ev); i++) {
        const mncf_cheat_result *r = mncf_evaluation_cheat(ev, i);
        jv *d = j_new(J_DICT), *ops = j_new(J_LIST);
        j_set(d, "state", j_str(mncf_state_name(r->state)));
        j_set(d, "reason", j_str(xprintf("%s", r->reason)));
        j_set(d, "uses", j_str(r->uses == MNCF_USES_CORE ? "core" : r->uses == MNCF_USES_RAW ? "raw" : ""));
        for (k = 0; k < r->op_count; k++) {
            jv *od = op_dict(&r->ops[k]);
            /* copy strings out of the evaluation, which is freed below */
            size_t j;
            for (j = 0; j < od->count; j++) {
                if (od->items[j]->type == J_STR) {
                    od->items[j]->s = xstrndup(od->items[j]->s, od->items[j]->n);
                }
            }
            j_push(ops, od);
        }
        j_set(d, "ops", ops);
        j_set(cheats, xprintf("%s", r->id), d);
    }
    j_set(out, "cheat", cheats);
    mncf_evaluation_free(ev);
    mncf_free(f);
    return out;
}

static jv *states(const mncf_engine *eng, const mncf_file *f)
{
    const mncf_file_info *info = mncf_file_info_of(f);
    jv *d = j_new(J_DICT);
    size_t i;
    for (i = 0; i < info->cheat_count; i++) {
        const char *reason;
        mncf_state st = mncf_engine_state(eng, i, &reason);
        jv *s = j_new(J_DICT);
        j_set(s, "state", j_str(mncf_state_name(st)));
        j_set(s, "reason", j_str(xprintf("%s", reason)));
        j_set(d, xprintf("%s", info->cheats[i].id), s);
    }
    return d;
}

static jv *change_dict(const mncf_change *ch)
{
    jv *d = j_new(J_DICT);
    j_set(d, "kind", j_str(mncf_change_name(ch->kind)));
    if (ch->kind != MNCF_CHANGE_RAW) {
        j_set(d, "address", j_uint(ch->address));
    }
    if (ch->bytes != NULL) {
        j_set(d, "bytes", j_str(hex_bytes(ch->bytes, ch->size)));
    }
    if (ch->cheat != NULL) {
        j_set(d, "cheat", j_str(xprintf("%s", ch->cheat)));
    }
    if (ch->kind == MNCF_CHANGE_HOOK) {
        j_set(d, "action", j_str(ch->action == MNCF_ACTION_RETURN ? "return" : "skip"));
        if (ch->has_value) {
            j_set(d, "value", j_number(ch->value));
        }
    }
    if (ch->kind == MNCF_CHANGE_RAW) {
        j_set(d, "syntax", j_str(xprintf("%s", ch->syntax)));
        j_set(d, "build", j_str(xprintf("%s", ch->build)));
        j_set(d, "code", j_str(xprintf("%s", ch->code)));
    }
    return d;
}

static const char *const STEP_KEYS[] = {"about", "memory", "values", "off", "on", "held", "press",
                                        "now", "pass", "stop", "expect"};
static const char *const STEP_EXPECT_KEYS[] = {"changes", "shown", "cheat"};

static jv *pass_results(const mncf_toml *c, const char *folder)
{
    size_t size, i, k;
    const uint8_t *data = case_bytes(c, folder, &size);
    mncf_trust *trust = trust_list(c);
    mncf_error err;
    mncf_file *f = mncf_load(data, size, trust, NULL, &err);
    const mncf_toml *env = mncf_toml_get(c, "env"), *steps = mncf_toml_get(c, "step"), *rate_v;
    memory m;
    mncf_runtime rt;
    mncf_choices ch;
    mncf_engine *eng;
    double rate = 60;
    jv *out, *list;
    mncf_trust_free(trust);
    if (f == NULL) {
        return error_result(&err);
    }
    memset(&m, 0, sizeof m);
    runtime(env, &m, &rt);
    choices_of(env, &ch);
    rate_v = mncf_toml_get(c, "rate");
    if (mncf_toml_type_of(rate_v) == MNCF_TOML_INTEGER) {
        rate = (double)mncf_toml_integer(rate_v);
    } else if (mncf_toml_type_of(rate_v) == MNCF_TOML_FLOAT) {
        rate = mncf_toml_float(rate_v);
    }
    eng = mncf_engine_new(f, &rt, &ch, rate, &err);
    if (eng == NULL) {
        fprintf(stderr, "conformance: the engine could not start: %s\n", err.message);
        exit(2);
    }
    out = j_new(J_DICT);
    j_set(out, "load", states(eng, f));
    list = j_new(J_LIST);
    for (i = 0; i < mncf_toml_count(steps); i++) {
        const mncf_toml *step = mncf_toml_at(steps, i);
        const mncf_toml *v;
        jv *got = j_new(J_DICT);
        int do_pass = 1, stop = 0;
        put_pieces(&m, mncf_toml_get(step, "memory"));
        v = mncf_toml_get(step, "values");
        for (k = 0; k < mncf_toml_count(v); k++) {
            size_t kn;
            const char *key = mncf_toml_key(v, k, &kn);
            mncf_engine_set_value(eng, xstrndup(key, kn), toml_number(mncf_toml_at(v, k)));
        }
        v = mncf_toml_get(step, "off");
        for (k = 0; k < mncf_toml_count(v); k++) {
            mncf_engine_switch(eng, tstrz(mncf_toml_at(v, k)), 0, NULL);
        }
        v = mncf_toml_get(step, "on");
        for (k = 0; k < mncf_toml_count(v); k++) {
            mncf_engine_switch(eng, tstrz(mncf_toml_at(v, k)), 1, NULL);
        }
        if ((v = mncf_toml_get(step, "held")) != NULL) {
            size_t n;
            const char **names = string_list(v, &n);
            mncf_engine_hold(eng, names, n);
        }
        v = mncf_toml_get(step, "press");
        for (k = 0; k < mncf_toml_count(v); k++) {
            mncf_engine_press(eng, tstrz(mncf_toml_at(v, k)));
        }
        if ((v = mncf_toml_get(step, "stop")) != NULL && mncf_toml_type_of(v) == MNCF_TOML_BOOLEAN) {
            stop = mncf_toml_boolean(v);
        }
        if ((v = mncf_toml_get(step, "pass")) != NULL && mncf_toml_type_of(v) == MNCF_TOML_BOOLEAN) {
            do_pass = mncf_toml_boolean(v);
        }
        if (stop || do_pass) {
            mncf_pass pass;
            jv *changes = j_new(J_LIST), *shown = j_new(J_DICT);
            mncf_status st;
            if (stop) {
                st = mncf_engine_stop(eng, &pass);
            } else {
                double now = 0;
                const mncf_toml *nv = mncf_toml_get(step, "now");
                int has_now = 0;
                if (mncf_toml_type_of(nv) == MNCF_TOML_INTEGER) {
                    now = (double)mncf_toml_integer(nv);
                    has_now = 1;
                } else if (mncf_toml_type_of(nv) == MNCF_TOML_FLOAT) {
                    now = mncf_toml_float(nv);
                    has_now = 1;
                }
                st = mncf_engine_tick(eng, mem_read, &m, has_now ? &now : NULL, &pass);
            }
            if (st != MNCF_OK) {
                fprintf(stderr, "conformance: a pass failed\n");
                exit(2);
            }
            for (k = 0; k < pass.change_count; k++) {
                j_push(changes, change_dict(&pass.changes[k]));
            }
            for (k = 0; k < pass.shown_count; k++) {
                const mncf_shown *s = &pass.shown[k];
                jv *values = j_get(shown, s->cheat), *d = j_new(J_DICT);
                if (values == NULL) {
                    values = j_new(J_LIST);
                    j_set(shown, xprintf("%s", s->cheat), values);
                }
                j_set(d, "type", j_str(mncf_type_name(s->type)));
                j_set(d, "value", j_number(s->value));
                if (s->label != NULL) {
                    j_set(d, "label", j_str(xprintf("%s", s->label)));
                }
                j_push(values, d);
            }
            j_set(got, "changes", changes);
            j_set(got, "shown", shown);
            for (k = 0; k < pass.change_count; k++) {
                const mncf_change *chg = &pass.changes[k];
                if (chg->kind == MNCF_CHANGE_PATCH || chg->kind == MNCF_CHANGE_RESTORE || chg->kind == MNCF_CHANGE_WRITE) {
                    mem_put(&m, chg->address, chg->bytes, chg->size);
                }
            }
        }
        j_set(got, "cheat", states(eng, f));
        j_push(list, got);
    }
    j_set(out, "steps", list);
    mncf_engine_free(eng);
    mncf_free(f);
    return out;
}

/* A stored ZIP file of the entries, as Python's zipfile writes one for the suite. */
static uint8_t *make_zip(const char **names, const uint8_t **datas, const size_t *sizes, size_t count, size_t *len)
{
    size_t total = 22, i, at = 0, cd_start;
    uint8_t *z;
    uint32_t *offsets = (uint32_t *)xalloc((count + 1) * sizeof *offsets);
    uint32_t *crcs = (uint32_t *)xalloc((count + 1) * sizeof *crcs);
    for (i = 0; i < count; i++) {
        total += 30 + 46 + 2 * strlen(names[i]) + sizes[i];
    }
    z = (uint8_t *)xalloc(total);
#define W16(v)                                                                                                   \
    do {                                                                                                         \
        z[at++] = (uint8_t)((uint32_t)(v)&0xFFu);                                                                \
        z[at++] = (uint8_t)(((uint32_t)(v) >> 8) & 0xFFu);                                                       \
    } while (0)
#define W32(v)                                                                                                   \
    do {                                                                                                         \
        W16((uint32_t)(v)&0xFFFF);                                                                               \
        W16((uint32_t)(v) >> 16);                                                                                \
    } while (0)
    for (i = 0; i < count; i++) {
        size_t n = strlen(names[i]), k;
        uint32_t crc = 0xFFFFFFFFu;
        int utf8 = 0;
        for (k = 0; k < n; k++) {
            utf8 |= (unsigned char)names[i][k] >= 0x80;
        }
        for (k = 0; k < sizes[i]; k++) {
            int b;
            crc ^= datas[i][k];
            for (b = 0; b < 8; b++) {
                crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
            }
        }
        crcs[i] = ~crc;
        offsets[i] = (uint32_t)at;
        W32(0x04034b50u);
        W16(20);
        W16(utf8 ? 0x800 : 0);
        W16(0);
        W16(0);
        W16(0x21);
        W32(crcs[i]);
        W32(sizes[i]);
        W32(sizes[i]);
        W16(n);
        W16(0);
        memcpy(z + at, names[i], n);
        at += n;
        if (sizes[i] > 0) {
            memcpy(z + at, datas[i], sizes[i]);
        }
        at += sizes[i];
    }
    cd_start = at;
    for (i = 0; i < count; i++) {
        size_t n = strlen(names[i]), k;
        int utf8 = 0;
        for (k = 0; k < n; k++) {
            utf8 |= (unsigned char)names[i][k] >= 0x80;
        }
        W32(0x02014b50u);
        W16(20);
        W16(20);
        W16(utf8 ? 0x800 : 0);
        W16(0);
        W16(0);
        W16(0x21);
        W32(crcs[i]);
        W32(sizes[i]);
        W32(sizes[i]);
        W16(n);
        W16(0);
        W16(0);
        W16(0);
        W16(0);
        W32(0);
        W32(offsets[i]);
        memcpy(z + at, names[i], n);
        at += n;
    }
    {
        size_t cd_size = at - cd_start;
        W32(0x06054b50u);
        W16(0);
        W16(0);
        W16(count);
        W16(count);
        W32(cd_size);
        W32(cd_start);
        W16(0);
    }
#undef W16
#undef W32
    *len = at;
    return z;
}

/* The bytes of a case's ZIP table: text, exact hex, or entries made from files of the folder. */
static const uint8_t *zip_bytes(const mncf_toml *z, const char *folder, size_t *size)
{
    const mncf_toml *text = mncf_toml_get(z, "text"), *hex = mncf_toml_get(z, "hex");
    size_t i, n;
    const char **names;
    const uint8_t **datas;
    size_t *sizes;
    if (text != NULL) {
        return (const uint8_t *)tstr(text, size);
    }
    if (hex != NULL) {
        const char *s = tstr(hex, &n);
        return from_hex(s != NULL ? s : "", s != NULL ? n : 0, size);       /* the exact bytes of a pack */
    }
    n = mncf_toml_count(z);
    names = (const char **)xalloc((n + 1) * sizeof *names);
    datas = (const uint8_t **)xalloc((n + 1) * sizeof *datas);
    sizes = (size_t *)xalloc((n + 1) * sizeof *sizes);
    for (i = 0; i < n; i++) {
        size_t kn;
        const char *key = mncf_toml_key(z, i, &kn);
        char *path = xprintf("%s/%s", folder, tstrz(mncf_toml_at(z, i)));
        names[i] = xstrndup(key, kn);
        datas[i] = (const uint8_t *)read_file(path, &sizes[i]);
        if (datas[i] == NULL) {
            fprintf(stderr, "conformance: cannot read %s\n", path);
            exit(2);
        }
    }
    return make_zip(names, datas, sizes, n, size);
}

static jv *pack_results(const mncf_toml *c, const char *folder)
{
    const uint8_t *data;
    size_t size, i;
    mncf_trust *trust = trust_list(c);
    mncf_error err;
    mncf_pack *pack;
    const mncf_pack_info *info;
    jv *out, *files;
    data = zip_bytes(mncf_toml_get(c, "zip"), folder, &size);
    pack = mncf_pack_read(data, size, trust, NULL, &err);
    mncf_trust_free(trust);
    if (pack == NULL) {
        return error_result(&err);
    }
    info = mncf_pack_info_of(pack);
    out = j_new(J_DICT);
    j_set(out, "signature", j_str(mncf_signature_name(info->signature.result)));
    {
        jv *l = j_new(J_LIST);
        for (i = 0; i < info->signature.signer_count; i++) {
            j_push(l, j_str(xprintf("%s", info->signature.signers[i])));
        }
        j_set(out, "signers", l);
    }
    files = j_new(J_DICT);
    for (i = 0; i < info->file_count; i++) {
        j_set(files, xprintf("%s", info->files[i].path), j_str(mncf_pack_status_name(info->files[i].status)));
    }
    j_set(out, "files", files);
    mncf_pack_free(pack);
    return out;
}

/* ---- Update cases ----------------------------------------------------------------------------------- */

/* A copy of a string of the library, which lives until the end of the run; "" for NULL. */
static jv *j_copy(const char *s)
{
    return j_str(s != NULL ? xprintf("%s", s) : "");
}

/* The bytes of one side of an update case, "installed" or "new": <side>_text, <side>_hex for exact
 * bytes, or a file of the folder. */
static const uint8_t *side_bytes(const mncf_toml *c, const char *side, const char *folder, size_t *size)
{
    const mncf_toml *v;
    size_t n;
    const char *s;
    char *path;
    const uint8_t *data;
    if ((v = mncf_toml_get(c, xprintf("%s_text", side))) != NULL) {
        s = tstr(v, &n);
        *size = s != NULL ? n : 0;
        return (const uint8_t *)(s != NULL ? s : "");
    }
    if ((v = mncf_toml_get(c, xprintf("%s_hex", side))) != NULL) {
        s = tstr(v, &n);
        return from_hex(s != NULL ? s : "", s != NULL ? n : 0, size);
    }
    path = xprintf("%s/%s", folder, tstrz(mncf_toml_get(c, side)));
    data = (const uint8_t *)read_file(path, size);
    if (data == NULL) {
        fprintf(stderr, "conformance: cannot read %s\n", path);
        exit(2);
    }
    return data;
}

/* What the reader says of the forms of section 17: addresses, versions, ids and dates. */
static jv *syntax_results(const mncf_toml *c)
{
    static const char *const KINDS[] = {"url", "version", "compare", "id", "date"};
    static char host[MNCF_MAX_URL + 1];
    jv *out = j_new(J_DICT);
    size_t k, i;
    for (k = 0; k < 5; k++) {
        const mncf_toml *items = mncf_toml_get(c, KINDS[k]);
        jv *list = j_new(J_LIST);
        for (i = 0; i < mncf_toml_count(items); i++) {
            const mncf_toml *item = mncf_toml_at(items, i);
            const char *text = tstrz(mncf_toml_get(item, "text"));
            if (k == 0) {
                mncf_update_url_host(text, host, sizeof host);
                j_push(list, j_copy(host));
            } else if (k == 1) {
                j_push(list, j_bool(mncf_version_valid(text)));
            } else if (k == 2) {
                int r = 0;
                const char *a = tstrz(mncf_toml_get(item, "a")), *b = tstrz(mncf_toml_get(item, "b"));
                if (mncf_version_compare(a, b, &r) != MNCF_OK) {
                    fprintf(stderr, "conformance: a compare case without two versions\n");
                    exit(2);
                }
                j_push(list, j_number(mncf_number_int(r)));
            } else if (k == 3) {
                j_push(list, j_bool(mncf_file_id_valid(text)));
            } else {
                j_push(list, j_bool(mncf_date_valid(text)));
            }
        }
        j_set(out, KINDS[k], list);
    }
    return out;
}

static jv *string_items(const char *const *items, size_t count)
{
    jv *l = j_new(J_LIST);
    size_t i;
    for (i = 0; i < count; i++) {
        j_push(l, j_copy(items[i]));
    }
    return l;
}

/* What the player set, moved to the new version, in the shape of the Python runner's results. */
static jv *carried_dict(const mncf_carried *cr)
{
    jv *d = j_new(J_DICT), *values = j_new(J_DICT), *ids = j_new(J_DICT), *reset = j_new(J_DICT);
    size_t i;
    j_set(d, "on", string_items(cr->choices.on, cr->choices.on_count));
    j_set(d, "off", string_items(cr->choices.off, cr->choices.off_count));
    for (i = 0; i < cr->choices.value_count; i++) {
        j_set(values, xprintf("%s", cr->choices.values[i].key), j_number(cr->choices.values[i].value));
    }
    j_set(d, "values", values);
    j_set(d, "preset", j_copy(cr->preset));
    for (i = 0; i < cr->id_count; i++) {
        j_set(ids, xprintf("%s", cr->ids[i].from), j_copy(cr->ids[i].to));
    }
    j_set(d, "ids", ids);
    j_set(d, "dropped", string_items(cr->dropped, cr->dropped_count));
    j_set(d, "gone", string_items(cr->gone, cr->gone_count));
    for (i = 0; i < cr->reset_count; i++) {
        j_set(reset, xprintf("%s", cr->reset[i].key), j_copy(cr->reset[i].why));
    }
    j_set(d, "reset", reset);
    return d;
}

/* The entries to show, the cheats to mark, the pairs of a pack and what the player set, moved over, for
 * a new side that loads. */
static void update_details(jv *out, const mncf_toml *c, const mncf_update_info *info, const mncf_pack *old_pack,
                           const char *old_version)
{
    const mncf_toml *settings = mncf_toml_get(c, "settings");
    const char *language = tstrz(mncf_toml_get(c, "language"));
    const mncf_release *changes;
    size_t count, since, i;
    mncf_error err;
    jv *versions = j_get(out, "since"), *texts = j_get(out, "texts");
    if (info->file != NULL) {
        changes = mncf_file_info_of(info->file)->changes;
        count = mncf_file_info_of(info->file)->change_count;
    } else {
        changes = mncf_pack_info_of(info->pack)->changes;
        count = mncf_pack_info_of(info->pack)->change_count;
    }
    since = mncf_releases_since(changes, count, old_version);
    for (i = 0; i < since; i++) {
        j_push(versions, j_copy(changes[i].version));
        j_push(texts, j_copy(mncf_release_text(info->file, &changes[i], language)));
    }
    if (info->pack != NULL) {
        mncf_pairs *pairs = mncf_pack_pairs(old_pack, info->pack, &err);
        jv *d = j_new(J_DICT);
        if (pairs == NULL) {
            fprintf(stderr, "conformance: the pairs failed: %s\n", err.message);
            exit(2);
        }
        for (i = 0; i < pairs->count; i++) {
            j_set(d, xprintf("%s", pairs->items[i].from), j_copy(pairs->items[i].to));
        }
        j_set(out, "pairs", d);
        mncf_pairs_free(pairs);
        return;
    }
    {
        mncf_marks *marks = mncf_mark(info->file, old_version, &err);
        if (marks == NULL) {
            fprintf(stderr, "conformance: the marks failed: %s\n", err.message);
            exit(2);
        }
        j_set(out, "new", string_items(marks->new_ids, marks->new_count));
        j_set(out, "fixed", string_items(marks->fixed_ids, marks->fixed_count));
        mncf_marks_free(marks);
    }
    if (settings != NULL) {
        mncf_choices ch;
        mncf_carried *cr;
        const char *preset = tstrz(mncf_toml_get(settings, "preset"));
        const char **kept;
        size_t kept_count;
        choices_of(settings, &ch);
        kept = string_list(mncf_toml_get(settings, "kept"), &kept_count);
        cr = mncf_carry(info->file, old_version, &ch, preset != NULL && preset[0] != 0 ? preset : NULL, kept,
                        kept_count, &err);
        if (cr == NULL) {
            fprintf(stderr, "conformance: the carry failed: %s\n", err.message);
            exit(2);
        }
        j_set(out, "settings", carried_dict(cr));
        mncf_carried_free(cr);
    }
}

/* Whether the new side of an update case is a new version of the installed side (spec, section 17), the
 * change entries to show and the cheats to mark, and with [settings], what the player set, moved to the
 * new version. */
static jv *update_results(const mncf_toml *c, const char *folder)
{
    static const char *const SYNTAX[] = {"url", "version", "compare", "id", "date"};
    const mncf_toml *installed_zip = mncf_toml_get(c, "installed_zip");
    mncf_trust *trust;
    mncf_error err;
    mncf_file *old_file = NULL;
    mncf_pack *old_pack = NULL;
    mncf_update *u;
    const mncf_update_info *info;
    const char *old_version;
    const uint8_t *data;
    size_t size, i;
    jv *out;
    for (i = 0; i < 5; i++) {
        if (mncf_toml_get(c, SYNTAX[i]) != NULL) {
            return syntax_results(c);
        }
    }
    trust = trust_list(c);
    if (installed_zip != NULL) {
        data = zip_bytes(installed_zip, folder, &size);
        old_pack = mncf_pack_read(data, size, trust, NULL, &err);
        if (old_pack == NULL) {
            fprintf(stderr, "conformance: the installed pack does not read: %s\n", err.code);
            exit(2);
        }
        old_version = mncf_pack_info_of(old_pack)->file_version;
        data = zip_bytes(mncf_toml_get(c, "new_zip"), folder, &size);
        u = mncf_pack_update_check(old_pack, data, size, trust, NULL, &err);
    } else {
        data = side_bytes(c, "installed", folder, &size);
        old_file = mncf_load(data, size, trust, NULL, &err);
        if (old_file == NULL) {
            fprintf(stderr, "conformance: the installed file does not load: %s\n", err.code);
            exit(2);
        }
        old_version = mncf_file_info_of(old_file)->file_version;
        data = side_bytes(c, "new", folder, &size);
        u = mncf_update_check(old_file, data, size, trust, NULL, &err);
    }
    mncf_trust_free(trust);
    if (u == NULL) {
        fprintf(stderr, "conformance: the update check failed: %s\n", err.message);
        exit(2);
    }
    info = mncf_update_info_of(u);
    out = j_new(J_DICT);
    j_set(out, "reason", j_copy(info->reason));
    j_set(out, "from", j_copy(old_version));
    j_set(out, "to", j_copy(info->to_version));
    j_set(out, "since", j_new(J_LIST));
    j_set(out, "texts", j_new(J_LIST));
    j_set(out, "new", j_new(J_LIST));
    j_set(out, "fixed", j_new(J_LIST));
    if (info->file != NULL || info->pack != NULL) {
        update_details(out, c, info, old_pack, old_version);
    }
    mncf_update_free(u);
    mncf_free(old_file);
    mncf_pack_free(old_pack);
    return out;
}

/* ---- Comparing, as the Python runner does ----------------------------------------------------------- */

typedef struct problems {
    const char *name;
    int count;
} problems;

static void problem(problems *p, const char *fmt, ...) PRINTF_LIKE(2, 3);

static void problem(problems *p, const char *fmt, ...)
{
    va_list ap;
    printf("%s: ", p->name);
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    printf("\n");
    p->count++;
}

static jv *sorted_strings(const jv *list)
{
    jv *out = j_new(J_LIST);
    size_t i, k;
    if (list == NULL || list->type != J_LIST) {
        return out;
    }
    for (i = 0; i < list->count; i++) {
        j_push(out, list->items[i]);
    }
    for (i = 1; i < out->count; i++) {
        for (k = i; k > 0; k--) {
            jv *a = out->items[k - 1], *b = out->items[k];
            if (a->type != J_STR || b->type != J_STR || cmp_keys(a->s, a->n, b->s, b->n) <= 0) {
                break;
            }
            out->items[k - 1] = b;
            out->items[k] = a;
        }
    }
    return out;
}

static int same_keys(const jv *a, const jv *b)
{
    size_t i;
    if (a->count != b->count) {
        return 0;
    }
    for (i = 0; i < a->count; i++) {
        if (j_get(b, xstrndup(a->keys[i], a->key_lens[i])) == NULL) {
            return 0;
        }
    }
    return 1;
}

static void compare_items(problems *p, const char *where, const jv *want, const jv *have)
{
    size_t i, k;
    if (want == NULL) {
        want = j_new(J_LIST);
    }
    if (want->count != have->count) {
        problem(p, "%s: expected %lu, got %lu", where, (unsigned long)want->count, (unsigned long)have->count);
        return;
    }
    for (i = 0; i < want->count; i++) {
        const jv *w = want->items[i], *g = have->items[i];
        for (k = 0; w->type == J_DICT && k < w->count; k++) {
            char *key = xstrndup(w->keys[k], w->key_lens[k]);
            if (!same(w->items[k], j_get(g, key), key)) {
                problem(p, "%s[%lu].%s: differs", where, (unsigned long)i, key);
            }
        }
    }
}

static void compare_states(problems *p, const char *where, const jv *want, const jv *have)
{
    size_t i;
    static const char *const KEYS[] = {"state", "reason"};
    for (i = 0; want != NULL && i < want->count; i++) {
        char *cid = xstrndup(want->keys[i], want->key_lens[i]);
        const jv *w = want->items[i], *h = j_get(have, cid);
        size_t k;
        if (h == NULL) {
            problem(p, "%s%s: no such cheat", where, cid);
            continue;
        }
        for (k = 0; k < 2; k++) {
            const jv *wv = j_get(w, KEYS[k]);
            if (wv != NULL && !py_eq(wv, j_get(h, KEYS[k]))) {
                problem(p, "%s%s.%s: differs", where, cid, KEYS[k]);
            }
        }
    }
}

static int key_known(const char *key, size_t n, const char *const *names, size_t count)
{
    size_t i;
    for (i = 0; i < count; i++) {
        if (strlen(names[i]) == n && memcmp(names[i], key, n) == 0) {
            return 1;
        }
    }
    return 0;
}

static void compare_passes(problems *p, const jv *c, const jv *got)
{
    const jv *steps = j_get(c, "step"), *gsteps;
    size_t n;
    if (j_get(got, "error") != NULL) {
        problem(p, "error: the file does not load (%s)", j_get(got, "error")->s);
        return;
    }
    compare_states(p, "load: ", j_get(j_get(c, "expect"), "cheat"), j_get(got, "load"));
    gsteps = j_get(got, "steps");
    for (n = 0; steps != NULL && n < steps->count && n < gsteps->count; n++) {
        const jv *step = steps->items[n], *g = gsteps->items[n];
        const jv *want = j_get(step, "expect");
        char *where;
        size_t k;
        jv *unknown = j_new(J_LIST), *unknown_want = j_new(J_LIST);
        for (k = 0; k < step->count; k++) {
            if (!key_known(step->keys[k], step->key_lens[k], STEP_KEYS, sizeof STEP_KEYS / sizeof STEP_KEYS[0])) {
                j_push(unknown, j_strn(step->keys[k], step->key_lens[k]));
            }
        }
        for (k = 0; want != NULL && k < want->count; k++) {
            if (!key_known(want->keys[k], want->key_lens[k], STEP_EXPECT_KEYS, 3)) {
                j_push(unknown_want, j_strn(want->keys[k], want->key_lens[k]));
            }
        }
        unknown = sorted_strings(unknown);
        unknown_want = sorted_strings(unknown_want);
        for (k = 0; k < unknown->count; k++) {
            problem(p, "step %lu: unknown key %.*s", (unsigned long)n + 1, (int)unknown->items[k]->n, unknown->items[k]->s);
        }
        for (k = 0; k < unknown_want->count; k++) {
            problem(p, "step %lu: unknown key %.*s", (unsigned long)n + 1, (int)unknown_want->items[k]->n,
                    unknown_want->items[k]->s);
        }
        if (j_get(g, "changes") != NULL) {
            const jv *shown_want = j_get(want, "shown"), *gs = j_get(g, "shown");
            where = xprintf("step %lu: changes", (unsigned long)n + 1);
            compare_items(p, where, j_get(want, "changes"), j_get(g, "changes"));
            if (shown_want == NULL) {
                shown_want = j_new(J_DICT);
            }
            if (!same_keys(shown_want, gs)) {
                problem(p, "step %lu: shown: the cheats differ", (unsigned long)n + 1);
            }
            for (k = 0; k < shown_want->count; k++) {
                char *cid = xstrndup(shown_want->keys[k], shown_want->key_lens[k]);
                if (j_get(gs, cid) != NULL) {
                    compare_items(p, xprintf("step %lu: shown.%s", (unsigned long)n + 1, cid), shown_want->items[k],
                                  j_get(gs, cid));
                }
            }
        } else if (j_get(want, "changes") != NULL || j_get(want, "shown") != NULL) {
            problem(p, "step %lu: expects changes, but has no pass", (unsigned long)n + 1);
        }
        compare_states(p, xprintf("step %lu: ", (unsigned long)n + 1), j_get(want, "cheat"), j_get(g, "cheat"));
    }
}

static void compare(problems *p, const jv *expect, const jv *got)
{
    static const char *const KEYS[] = {"signature", "signers", "build", "warnings"};
    size_t i;
    const jv *we = j_get(expect, "error"), *ge = j_get(got, "error");
    if (we != NULL || ge != NULL) {
        if (we == NULL || ge == NULL || !py_eq(we, ge)) {
            problem(p, "error: expected %s, got %s", we != NULL ? we->s : "None", ge != NULL ? ge->s : "None");
        }
        return;
    }
    for (i = 0; i < 4; i++) {
        const jv *g = j_get(got, KEYS[i]), *w = j_get(expect, KEYS[i]);
        if (g == NULL) {
            continue;
        }
        if (w == NULL) {
            w = i == 0 ? j_str("unsigned") : i == 2 ? j_str("") : j_new(J_LIST);
        }
        if (i == 3) {
            w = sorted_strings(w);
        }
        if (!py_eq(w, g)) {
            problem(p, "%s: differs", KEYS[i]);
        }
    }
    if (j_get(got, "files") != NULL) {
        const jv *w = j_get(expect, "files");
        if (w == NULL) {
            w = j_new(J_DICT);
        }
        if (!py_eq(w, j_get(got, "files"))) {
            problem(p, "files: differ");
        }
    }
    if (j_get(got, "cheat") != NULL) {
        const jv *want = j_get(expect, "cheat"), *have = j_get(got, "cheat");
        if (want == NULL) {
            want = j_new(J_DICT);
        }
        if (!same_keys(want, have)) {
            problem(p, "cheats: the listed cheats differ");
        }
        for (i = 0; i < want->count; i++) {
            char *cid = xstrndup(want->keys[i], want->key_lens[i]);
            const jv *w = want->items[i], *g = j_get(have, cid), *wops;
            static const char *const CKEYS[] = {"state", "reason", "uses"};
            size_t k;
            if (g == NULL) {
                continue;
            }
            for (k = 0; k < 3; k++) {
                const jv *wv = j_get(w, CKEYS[k]);
                if (wv != NULL && !py_eq(wv, j_get(g, CKEYS[k]))) {
                    problem(p, "%s.%s: expected %.*s, got %.*s", cid, CKEYS[k], (int)wv->n, wv->s ? wv->s : "",
                            (int)j_get(g, CKEYS[k])->n, j_get(g, CKEYS[k])->s);
                }
            }
            wops = j_get(w, "ops");
            if (wops != NULL) {
                const jv *gops = j_get(g, "ops");
                if (wops->count != gops->count) {
                    problem(p, "%s.ops: expected %lu operations, got %lu", cid, (unsigned long)wops->count,
                            (unsigned long)gops->count);
                    continue;
                }
                for (k = 0; k < wops->count; k++) {
                    const jv *wo = wops->items[k], *go = gops->items[k];
                    size_t j;
                    for (j = 0; j < wo->count; j++) {
                        char *key = xstrndup(wo->keys[j], wo->key_lens[j]);
                        if (!same(wo->items[j], j_get(go, key), key)) {
                            problem(p, "%s.ops[%lu].%s: differs", cid, (unsigned long)k, key);
                        }
                    }
                }
            }
        }
    }
}

/* The differences between an update case's [expect] table and what the reader said. Only the keys the
 * case gives are compared, and reason; under settings, every key, with its default where it is missing. */
static void compare_updates(problems *p, const jv *c, const jv *got)
{
    static const char *const KINDS[][2] = {
        {"url", "host"}, {"version", "valid"}, {"compare", "result"}, {"id", "valid"}, {"date", "valid"}};
    static const char *const SETTINGS[] = {"on", "off", "values", "preset", "ids", "dropped", "gone", "reset"};
    const jv *want, *ws, *gs;
    size_t i, k;
    if (j_get(got, "url") != NULL) {
        for (k = 0; k < 5; k++) {
            const jv *items = j_get(c, KINDS[k][0]), *values = j_get(got, KINDS[k][0]);
            for (i = 0; items != NULL && i < items->count && i < values->count; i++) {
                const jv *item = items->items[i], *text = j_get(item, "text");
                if (text == NULL) {
                    text = j_get(item, "a");
                }
                if (!same(j_get(item, KINDS[k][1]), values->items[i], NULL)) {
                    problem(p, "%s %.*s: differs", KINDS[k][0], text != NULL ? (int)text->n : 0,
                            text != NULL ? text->s : "");
                }
            }
        }
        return;
    }
    want = j_get(c, "expect");
    if (want == NULL) {
        want = j_new(J_DICT);
    }
    for (i = 0; i < want->count; i++) {
        char *key = xstrndup(want->keys[i], want->key_lens[i]);
        if (strcmp(key, "settings") != 0 && !same(want->items[i], j_get(got, key), NULL)) {
            problem(p, "%s: differs", key);
        }
    }
    if (j_get(want, "reason") == NULL && !same(j_str(""), j_get(got, "reason"), NULL)) {
        problem(p, "reason: expected '', got %s", j_get(got, "reason")->s);
    }
    ws = j_get(want, "settings");
    if (ws == NULL) {
        return;
    }
    gs = j_get(got, "settings");
    if (gs == NULL) {
        gs = j_new(J_DICT);
    }
    for (k = 0; k < sizeof SETTINGS / sizeof SETTINGS[0]; k++) {
        const jv *w = j_get(ws, SETTINGS[k]);
        if (w == NULL) {
            w = k == 3 ? j_str("") : (k == 2 || k == 4 || k == 7) ? j_new(J_DICT) : j_new(J_LIST);
        }
        if (!same(w, j_get(gs, SETTINGS[k]), NULL)) {
            problem(p, "settings.%s: differs", SETTINGS[k]);
        }
    }
}

/* ---- The suite -------------------------------------------------------------------------------------- */

int main(int argc, char **argv)
{
    static const char *const KINDS[] = {"cases", "packs", "convert", "passes", "updates"};
    const char *suite = NULL;
    int dump = 0, i;
    size_t total = 0, failed = 0, skipped = 0, k;
    jv *all = j_new(J_DICT);
    outbuf o;
    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--dump") == 0) {
            dump = 1;
        } else {
            suite = argv[i];
        }
    }
    if (suite == NULL) {
        fprintf(stderr, "usage: mncf_conformance <format/tests folder> [--dump]\n");
        return 2;
    }
#ifdef _WIN32
    _setmode(_fileno(stdout), _O_BINARY);
#endif
    memset(&o, 0, sizeof o);
    /* The results stay until the dump is written, in the same blocks as each case's own memory. */
    for (k = 0; k < sizeof KINDS / sizeof KINDS[0]; k++) {
        char *folder = xprintf("%s/%s", suite, KINDS[k]);
        size_t count, c;
        char **names = list_cases(folder, &count);
        for (c = 0; c < count; c++) {
            char *path = xprintf("%s/%s.toml", folder, names[c]);
            char *name = xprintf("%s/%s", KINDS[k], names[c]);
            size_t size;
            char *text = read_text(path, &size);
            mncf_error err;
            mncf_toml_doc *doc;
            const mncf_toml *ct;
            jv *got, *cj;
            problems p;
            if (strcmp(KINDS[k], "convert") == 0) {
                skipped++;
                continue;
            }
            if (text == NULL) {
                fprintf(stderr, "conformance: cannot read %s\n", path);
                return 2;
            }
            doc = mncf_toml_parse(text, size, NULL, &err);
            if (doc == NULL) {
                fprintf(stderr, "conformance: %s is not TOML: %s\n", path, err.message);
                return 2;
            }
            ct = mncf_toml_root(doc);
            total++;
            if (strcmp(KINDS[k], "cases") == 0) {
                got = file_results(ct, folder);
            } else if (strcmp(KINDS[k], "packs") == 0) {
                got = pack_results(ct, folder);
            } else if (strcmp(KINDS[k], "updates") == 0) {
                got = update_results(ct, folder);
            } else {
                got = pass_results(ct, folder);
            }
            cj = j_from_toml(ct);
            p.name = name;
            p.count = 0;
            if (strcmp(KINDS[k], "passes") == 0) {
                compare_passes(&p, cj, got);
            } else if (strcmp(KINDS[k], "updates") == 0) {
                compare_updates(&p, cj, got);
            } else {
                const jv *expect = j_get(cj, "expect");
                compare(&p, expect != NULL ? expect : j_new(J_DICT), got);
            }
            if (p.count > 0) {
                failed++;
            }
            j_set(all, name, got);
            mncf_toml_free(doc);
        }
    }
    if (dump) {
        json_write(&o, all, 0);
        put(&o, "\n", 1);
        fwrite(o.data, 1, o.size, stdout);
    }
    printf("conformance: %lu of %lu cases passed", (unsigned long)(total - failed), (unsigned long)total);
    if (skipped > 0) {
        printf(" (%lu conversion cases left out: the C library has no converter yet)", (unsigned long)skipped);
    }
    printf("\n");
    free(o.data);
    xfree_all();
    return failed == 0 ? 0 : 1;
}
