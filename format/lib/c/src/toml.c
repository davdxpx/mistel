/* toml.c: a parser for TOML 1.0.0 documents. MIT License.
 *
 * It reads what Python's tomllib reads and refuses what it refuses, for the documents the format uses.
 * Tables keep their keys in the order in which they first appear. A new line inside a multi-line string
 * is a line feed, also where the document has a carriage return and a line feed. Integers are 64-bit
 * signed, as TOML 1.0 says; a literal outside that range is an error.
 */
#include "internal.h"

#include <errno.h>
#include <locale.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#define MAX_NESTING 128

typedef struct parser {
    mn_arena *ar;
    const char *src;
    size_t len;
    size_t pos;
    int nesting;
    mncf_error *err;
    int failed;
} parser;

/* ---- Tables ----------------------------------------------------------------------------------------- */

static mncf_toml *new_node(parser *p, int type)
{
    mncf_toml *v = (mncf_toml *)mn_arena_zalloc(p->ar, sizeof *v);
    if (v == NULL) {
        p->failed = 1;
        mn_error_nomem(p->err);
        return NULL;
    }
    v->type = (uint8_t)type;
    return v;
}

static size_t index_find(const mncf_toml *t, const char *key, size_t n, uint64_t h)
{
    size_t mask = t->u.t.index.cap - 1;
    size_t at = (size_t)h & mask;
    for (;;) {
        uint32_t slot = t->u.t.index.slots[at];
        if (slot == 0) {
            return (size_t)-1;
        }
        if (mn_streq(t->u.t.e[slot - 1].key.p, t->u.t.e[slot - 1].key.n, key, n)) {
            return slot - 1;
        }
        at = (at + 1) & mask;
    }
}

static size_t table_find(const mncf_toml *t, const char *key, size_t n)
{
    size_t i;
    if (t->u.t.index.cap != 0) {
        return index_find(t, key, n, mn_hash(key, n));
    }
    for (i = 0; i < t->u.t.n; i++) {
        if (mn_streq(t->u.t.e[i].key.p, t->u.t.e[i].key.n, key, n)) {
            return i;
        }
    }
    return (size_t)-1;
}

static int index_build(mn_arena *ar, mncf_toml *t, size_t cap)
{
    uint32_t *slots = (uint32_t *)mn_arena_zalloc(ar, cap * sizeof *slots);
    size_t i;
    if (slots == NULL) {
        return 0;
    }
    t->u.t.index.slots = slots;
    t->u.t.index.cap = cap;
    for (i = 0; i < t->u.t.n; i++) {
        size_t at = (size_t)mn_hash(t->u.t.e[i].key.p, t->u.t.e[i].key.n) & (cap - 1);
        while (slots[at] != 0) {
            at = (at + 1) & (cap - 1);
        }
        slots[at] = (uint32_t)(i + 1);
    }
    return 1;
}

/* Adds a key that is not in the table yet. */
static int table_add(mn_arena *ar, mncf_toml *t, const char *key, size_t n, mncf_toml *value)
{
    size_t count = t->u.t.n;
    struct mn_entry *e;
    if (count >= 0xFFFFFFF0u || MN_GROW(ar, t->u.t.e, t->u.t.n, t->u.t.cap) == NULL) {
        return 0;
    }
    e = &t->u.t.e[count];
    e->key.p = mn_arena_strndup(ar, key, n);
    if (e->key.p == NULL) {
        return 0;
    }
    e->key.n = n;
    e->val = value;
    count++;
    t->u.t.n = count;
    if (t->u.t.index.cap != 0 && count * 2 <= t->u.t.index.cap) {
        size_t at = (size_t)mn_hash(key, n) & (t->u.t.index.cap - 1);
        while (t->u.t.index.slots[at] != 0) {
            at = (at + 1) & (t->u.t.index.cap - 1);
        }
        t->u.t.index.slots[at] = (uint32_t)count;
    } else if (count > 8) {
        size_t want = 16;
        while (want < count * 4) {
            want *= 2;
        }
        if (!index_build(ar, t, want)) {
            return 0;
        }
    }
    return 1;
}

static int array_add(mn_arena *ar, mncf_toml *a, mncf_toml *value)
{
    if (MN_GROW(ar, a->u.a.items, a->u.a.n, a->u.a.cap) == NULL) {
        return 0;
    }
    a->u.a.items[a->u.a.n++] = value;
    return 1;
}

/* ---- Errors ----------------------------------------------------------------------------------------- */

static void toml_fail(parser *p, size_t at, const char *fmt, ...) MN_PRINTF(3, 4);

static void toml_fail(parser *p, size_t at, const char *fmt, ...)
{
    char reason[300];
    va_list ap;
    size_t line = 1, line_start = 0, i;
    if (p->failed) {
        return;
    }
    p->failed = 1;
    va_start(ap, fmt);
    vsnprintf(reason, sizeof reason, fmt, ap);
    va_end(ap);
    if (at > p->len) {
        at = p->len;
    }
    for (i = 0; i < at; i++) {
        if (p->src[i] == '\n') {
            line++;
            line_start = i + 1;
        }
    }
    mn_error(p->err, MNCF_ERR_TOML, "toml", "%s (at line %lu, column %lu)", reason, (unsigned long)line,
             (unsigned long)(mn_utf8_count(p->src + line_start, at - line_start) + 1));
}

static void toml_nomem(parser *p)
{
    if (!p->failed) {
        p->failed = 1;
        mn_error_nomem(p->err);
    }
}

static int at(const parser *p, size_t i)
{
    return i < p->len ? (unsigned char)p->src[i] : -1;
}

static int peek(const parser *p)
{
    return at(p, p->pos);
}

static int starts(const parser *p, const char *s)
{
    size_t n = strlen(s);
    return p->len - p->pos >= n && memcmp(p->src + p->pos, s, n) == 0;
}

/* Describes the character at pos, for an error message. */
static const char *found(const parser *p, char *buf, size_t size)
{
    int c = peek(p);
    if (c == -1) {
        return "the end of the file";
    }
    if (c == '\n' || c == '\r') {
        return "the end of the line";
    }
    if (c > ' ' && c < 0x7F) {
        snprintf(buf, size, "'%c'", c);
        return buf;
    }
    {
        uint32_t cp;
        mn_utf8_decode((const uint8_t *)p->src + p->pos, p->len - p->pos, &cp);
        snprintf(buf, size, "U+%04lX", (unsigned long)cp);
    }
    return buf;
}

static int is_digit(int c)
{
    return c >= '0' && c <= '9';
}

static int is_bare(int c)
{
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || is_digit(c) || c == '_' || c == '-';
}

static int is_number_char(int c)
{
    return is_bare(c) || c == '+' || c == '.';
}

/* ---- Spaces, comments and new lines ----------------------------------------------------------------- */

static void skip_spaces(parser *p)
{
    while (peek(p) == ' ' || peek(p) == '\t') {
        p->pos++;
    }
}

/* The length of the character at pos that may stand in a string or a comment, or 0 after an error. */
static size_t text_char(parser *p)
{
    unsigned char c = (unsigned char)p->src[p->pos];
    if ((c < ' ' && c != '\t') || c == 0x7F) {
        toml_fail(p, p->pos, "control character U+%04X is not allowed", c);
        return 0;
    }
    if (c < 0x80) {
        return 1;
    }
    {
        uint32_t cp;
        return mn_utf8_decode((const uint8_t *)p->src + p->pos, p->len - p->pos, &cp);
    }
}

/* Skips a line feed, or a carriage return and a line feed. -1 after an error. */
static int newline(parser *p)
{
    int c = peek(p);
    if (c == '\n') {
        p->pos++;
        return 1;
    }
    if (c != '\r') {
        return 0;
    }
    if (at(p, p->pos + 1) != '\n') {
        toml_fail(p, p->pos, "a carriage return must be followed by a line feed");
        return -1;
    }
    p->pos += 2;
    return 1;
}

static void comment(parser *p)
{
    p->pos++;
    while (p->pos < p->len) {
        char c = p->src[p->pos];
        size_t n;
        if (c == '\n' || (c == '\r' && at(p, p->pos + 1) == '\n')) {
            return;
        }
        n = text_char(p);
        if (n == 0) {
            return;
        }
        p->pos += n;
    }
}

static void skip_blank(parser *p)
{
    int nl;
    do {
        skip_spaces(p);
        if (peek(p) == '#') {
            comment(p);
            if (p->failed) {
                return;
            }
        }
        nl = newline(p);
    } while (nl == 1);
}

static void end_of_line(parser *p)
{
    char buf[16];
    skip_spaces(p);
    if (peek(p) == '#') {
        comment(p);
        if (p->failed) {
            return;
        }
    }
    if (p->pos < p->len) {
        int nl = newline(p);
        if (nl == 0) {
            toml_fail(p, p->pos, "expected the end of the line, found %s", found(p, buf, sizeof buf));
        }
    }
}

static int expect(parser *p, const char *token)
{
    char buf[16];
    if (!starts(p, token)) {
        toml_fail(p, p->pos, "expected '%s', found %s", token, found(p, buf, sizeof buf));
        return 0;
    }
    p->pos += strlen(token);
    return 1;
}

/* ---- Strings ---------------------------------------------------------------------------------------- */

static void add_utf8(mn_buf *b, uint32_t cp)
{
    char out[4];
    size_t n;
    if (cp < 0x80) {
        out[0] = (char)cp;
        n = 1;
    } else if (cp < 0x800) {
        out[0] = (char)(0xC0 | (cp >> 6));
        out[1] = (char)(0x80 | (cp & 0x3F));
        n = 2;
    } else if (cp < 0x10000) {
        out[0] = (char)(0xE0 | (cp >> 12));
        out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[2] = (char)(0x80 | (cp & 0x3F));
        n = 3;
    } else {
        out[0] = (char)(0xF0 | (cp >> 18));
        out[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
        out[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[3] = (char)(0x80 | (cp & 0x3F));
        n = 4;
    }
    mn_buf_add(b, out, n);
}

static void unicode_escape(parser *p, mn_buf *out, size_t start, int digits)
{
    uint32_t code = 0;
    int i;
    for (i = 0; i < digits; i++) {
        int d = mn_hex_value(peek(p));
        if (d < 0) {
            toml_fail(p, start, "a \\%c escape needs %d hex digits", p->src[start + 1], digits);
            return;
        }
        if (code > 0x0FFFFFFF) {
            code = 0x7FFFFFFF;
        } else {
            code = code * 16 + (uint32_t)d;
        }
        p->pos++;
    }
    if (code > 0x10FFFF || (code >= 0xD800 && code <= 0xDFFF)) {
        toml_fail(p, start, "escape %.*s is not a Unicode scalar value", (int)(p->pos - start), p->src + start);
        return;
    }
    add_utf8(out, code);
}

static void escape(parser *p, mn_buf *out)
{
    size_t start = p->pos;
    int c = at(p, p->pos + 1);
    p->pos += 2;
    switch (c) {
    case 'b':
        mn_buf_addc(out, '\b');
        return;
    case 't':
        mn_buf_addc(out, '\t');
        return;
    case 'n':
        mn_buf_addc(out, '\n');
        return;
    case 'f':
        mn_buf_addc(out, '\f');
        return;
    case 'r':
        mn_buf_addc(out, '\r');
        return;
    case '"':
        mn_buf_addc(out, '"');
        return;
    case '\\':
        mn_buf_addc(out, '\\');
        return;
    case 'u':
        unicode_escape(p, out, start, 4);
        return;
    case 'U':
        unicode_escape(p, out, start, 8);
        return;
    default:
        if (c > ' ' && c < 0x7F) {
            toml_fail(p, start, "invalid escape sequence \\%c", c);
        } else {
            toml_fail(p, start, "invalid escape sequence");
        }
    }
}

static int add_text_char(parser *p, mn_buf *out)
{
    size_t n = text_char(p);
    if (n == 0) {
        return 0;
    }
    mn_buf_add(out, p->src + p->pos, n);
    p->pos += n;
    return 1;
}

static int finish_string(parser *p, mn_buf *b, mn_str *out)
{
    if (b->failed) {
        mn_buf_free(b);
        toml_nomem(p);
        return 0;
    }
    out->p = mn_arena_strndup(p->ar, b->data != NULL ? b->data : "", b->size);
    out->n = b->size;
    mn_buf_free(b);
    if (out->p == NULL) {
        toml_nomem(p);
        return 0;
    }
    return 1;
}

static int basic_string(parser *p, mn_str *out)
{
    size_t start = p->pos++;
    mn_buf b;
    mn_buf_init(&b, p->ar->alloc);
    for (;;) {
        int c = peek(p);
        if (c == '"') {
            p->pos++;
            return finish_string(p, &b, out);
        }
        if (c == -1 || c == '\n' || c == '\r') {
            toml_fail(p, start, "unterminated string");
            break;
        }
        if (c == '\\') {
            escape(p, &b);
        } else {
            add_text_char(p, &b);
        }
        if (p->failed) {
            break;
        }
    }
    mn_buf_free(&b);
    return 0;
}

static int literal_string(parser *p, mn_str *out)
{
    size_t start = p->pos++;
    for (;;) {
        int c = peek(p);
        size_t n;
        if (c == '\'') {
            p->pos++;
            out->n = p->pos - 1 - (start + 1);
            out->p = mn_arena_strndup(p->ar, p->src + start + 1, out->n);
            if (out->p == NULL) {
                toml_nomem(p);
                return 0;
            }
            return 1;
        }
        if (c == -1 || c == '\n' || c == '\r') {
            toml_fail(p, start, "unterminated string");
            return 0;
        }
        n = text_char(p);
        if (n == 0) {
            return 0;
        }
        p->pos += n;
    }
}

/* A run of quotes in a multi-line string: three close it, and up to two more before them belong to it. */
static int closing_quotes(parser *p, char quote, mn_buf *out)
{
    int run = 0;
    int i;
    int closes;
    while (run < 5 && at(p, p->pos + (size_t)run) == quote) {
        run++;
    }
    closes = run >= 3;
    for (i = closes ? 3 : 0; i < run; i++) {
        mn_buf_addc(out, quote);
    }
    p->pos += (size_t)run;
    return closes;
}

/* Skips a backslash at the end of a line with all spaces and new lines after it. */
static int line_ending_backslash(parser *p)
{
    size_t q = p->pos + 1;
    while (at(p, q) == ' ' || at(p, q) == '\t') {
        q++;
    }
    if (at(p, q) != '\n' && at(p, q) != '\r') {
        return 0;
    }
    p->pos = q;
    for (;;) {
        if (peek(p) == ' ' || peek(p) == '\t') {
            p->pos++;
        } else {
            int nl = newline(p);
            if (nl != 1) {
                return 1;
            }
        }
    }
}

static int multi_line_string(parser *p, char quote, mn_str *out)
{
    size_t start = p->pos;
    mn_buf b;
    p->pos += 3;
    mn_buf_init(&b, p->ar->alloc);
    if (newline(p) < 0) {
        return 0;
    }
    for (;;) {
        int c = peek(p);
        int nl;
        if (c == -1) {
            toml_fail(p, start, "unterminated multi-line string");
            break;
        }
        if (c == quote) {
            if (closing_quotes(p, quote, &b)) {
                return finish_string(p, &b, out);
            }
            continue;
        }
        if (quote == '"' && c == '\\') {
            if (!line_ending_backslash(p)) {
                escape(p, &b);
            }
        } else {
            nl = newline(p);
            if (nl == 1) {
                mn_buf_addc(&b, '\n');
            } else if (nl == 0) {
                add_text_char(p, &b);
            }
        }
        if (p->failed) {
            break;
        }
    }
    mn_buf_free(&b);
    return 0;
}

/* ---- Keys ------------------------------------------------------------------------------------------- */

typedef struct key_parts {
    mn_str *parts;
    size_t n, cap;
} key_parts;

static int simple_key(parser *p, mn_str *out)
{
    int c = peek(p);
    char buf[16];
    size_t start;
    if (c == '"' || c == '\'') {
        if (starts(p, c == '"' ? "\"\"\"" : "'''")) {
            toml_fail(p, p->pos, "a key cannot be a multi-line string");
            return 0;
        }
        return c == '"' ? basic_string(p, out) : literal_string(p, out);
    }
    start = p->pos;
    while (is_bare(peek(p))) {
        p->pos++;
    }
    if (p->pos == start) {
        toml_fail(p, p->pos, "expected a key, found %s", found(p, buf, sizeof buf));
        return 0;
    }
    out->p = mn_arena_strndup(p->ar, p->src + start, p->pos - start);
    out->n = p->pos - start;
    if (out->p == NULL) {
        toml_nomem(p);
        return 0;
    }
    return 1;
}

static int read_key(parser *p, key_parts *key)
{
    key->parts = NULL;
    key->n = 0;
    key->cap = 0;
    for (;;) {
        mn_str *part;
        if (MN_GROW(p->ar, key->parts, key->n, key->cap) == NULL) {
            toml_nomem(p);
            return 0;
        }
        part = &key->parts[key->n++];
        if (!simple_key(p, part)) {
            return 0;
        }
        skip_spaces(p);
        if (peek(p) != '.') {
            return 1;
        }
        p->pos++;
        skip_spaces(p);
    }
}

/* Writes the first count parts of a key as TOML would, for an error message. */
static const char *key_name(parser *p, const key_parts *key, size_t count)
{
    mn_buf b;
    size_t i, j;
    const char *out;
    mn_buf_init(&b, p->ar->alloc);
    for (i = 0; i < count; i++) {
        const mn_str *part = &key->parts[i];
        int bare = part->n > 0;
        for (j = 0; j < part->n && bare; j++) {
            bare = is_bare((unsigned char)part->p[j]);
        }
        if (i > 0) {
            mn_buf_addc(&b, '.');
        }
        if (bare) {
            mn_buf_add(&b, part->p, part->n);
        } else {
            mn_buf_addc(&b, '"');
            for (j = 0; j < part->n; j++) {
                if (part->p[j] == '"' || part->p[j] == '\\') {
                    mn_buf_addc(&b, '\\');
                }
                mn_buf_addc(&b, part->p[j]);
            }
            mn_buf_addc(&b, '"');
        }
    }
    out = mn_arena_strndup(p->ar, b.data != NULL ? b.data : "", b.size);
    mn_buf_free(&b);
    return out != NULL ? out : "";
}

static const char *describe(const mncf_toml *v)
{
    switch (v->origin) {
    case MN_ORIGIN_IMPLICIT:
        return "a table";
    case MN_ORIGIN_HEADER:
        return "a table defined by a header";
    case MN_ORIGIN_DOTTED:
        return "a table defined by dotted keys";
    case MN_ORIGIN_INLINE:
        return "an inline table";
    case MN_ORIGIN_TABLE_ARRAY:
        return "an array of tables";
    default:
        return v->type == MNCF_TOML_ARRAY ? "an array" : "a value";
    }
}

static mncf_toml *new_table(parser *p, int origin)
{
    mncf_toml *t = new_node(p, MNCF_TOML_TABLE);
    if (t != NULL) {
        t->origin = (uint8_t)origin;
    }
    return t;
}

static mncf_toml *child_of(const mncf_toml *t, const mn_str *key)
{
    size_t i = table_find(t, key->p, key->n);
    return i == (size_t)-1 ? NULL : t->u.t.e[i].val;
}

static int put(parser *p, mncf_toml *t, const mn_str *key, mncf_toml *value)
{
    if (!table_add(p->ar, t, key->p, key->n, value)) {
        toml_nomem(p);
        return 0;
    }
    return 1;
}

/* ---- Values ----------------------------------------------------------------------------------------- */

static mncf_toml *value(parser *p);
static int key_value(parser *p, mncf_toml *table);

static mncf_toml *array(parser *p)
{
    mncf_toml *a;
    char buf[16];
    if (++p->nesting > MAX_NESTING) {
        toml_fail(p, p->pos, "arrays and inline tables nest deeper than %d levels", MAX_NESTING);
        return NULL;
    }
    p->pos++;
    a = new_node(p, MNCF_TOML_ARRAY);
    if (a == NULL) {
        return NULL;
    }
    skip_blank(p);
    while (!p->failed && peek(p) != ']') {
        mncf_toml *v = value(p);
        if (v == NULL) {
            return NULL;
        }
        if (!array_add(p->ar, a, v)) {
            toml_nomem(p);
            return NULL;
        }
        skip_blank(p);
        if (p->failed) {
            return NULL;
        }
        if (peek(p) == ',') {
            p->pos++;
            skip_blank(p);
        } else if (peek(p) == -1) {
            toml_fail(p, p->pos, "unterminated array");
            return NULL;
        } else if (peek(p) != ']') {
            toml_fail(p, p->pos, "expected ',' or ']', found %s", found(p, buf, sizeof buf));
            return NULL;
        }
    }
    if (p->failed) {
        return NULL;
    }
    p->pos++;
    p->nesting--;
    return a;
}

static mncf_toml *inline_table(parser *p)
{
    mncf_toml *t;
    char buf[16];
    if (++p->nesting > MAX_NESTING) {
        toml_fail(p, p->pos, "arrays and inline tables nest deeper than %d levels", MAX_NESTING);
        return NULL;
    }
    p->pos++;
    t = new_table(p, MN_ORIGIN_INLINE);
    if (t == NULL) {
        return NULL;
    }
    skip_spaces(p);
    if (peek(p) != '}') {
        for (;;) {
            if (!key_value(p, t)) {
                return NULL;
            }
            skip_spaces(p);
            if (peek(p) != ',') {
                break;
            }
            p->pos++;
            skip_spaces(p);
        }
        if (peek(p) != '}') {
            toml_fail(p, p->pos, "expected ',' or '}', found %s", found(p, buf, sizeof buf));
            return NULL;
        }
    }
    p->pos++;
    p->nesting--;
    return t;
}

/* Whether the token is digits with single underscores between them, from the given digit set. */
static int digits_ok(const char *s, size_t n, int base)
{
    size_t i;
    int prev_digit = 0;
    if (n == 0) {
        return 0;
    }
    for (i = 0; i < n; i++) {
        int c = (unsigned char)s[i];
        int d = mn_hex_value(c);
        if (c == '_') {
            if (!prev_digit || i + 1 == n) {
                return 0;
            }
            prev_digit = 0;
            continue;
        }
        if (d < 0 || d >= base) {
            return 0;
        }
        prev_digit = 1;
    }
    return 1;
}

/* The decimal integer part: [+-]?(0|[1-9](_?[0-9])*); returns its length or 0. */
static size_t decimal_part(const char *s, size_t n)
{
    size_t i = 0;
    size_t start;
    if (i < n && (s[i] == '+' || s[i] == '-')) {
        i++;
    }
    start = i;
    if (i >= n || !is_digit((unsigned char)s[i])) {
        return 0;
    }
    if (s[i] == '0') {
        return i + 1;
    }
    while (i < n && (is_digit((unsigned char)s[i]) || s[i] == '_')) {
        i++;
    }
    while (i > start && s[i - 1] == '_') {
        i--;
    }
    return digits_ok(s + start, i - start, 10) ? i : 0;
}

/* Parses digits (with underscores) of a base into an unsigned value; 0 when it does not fit. */
static int parse_uint(const char *s, size_t n, int base, uint64_t limit, uint64_t *out)
{
    uint64_t v = 0;
    size_t i;
    for (i = 0; i < n; i++) {
        int d;
        if (s[i] == '_') {
            continue;
        }
        d = mn_hex_value((unsigned char)s[i]);
        if (v > (limit - (uint64_t)d) / (uint64_t)base) {
            return 0;
        }
        v = v * (uint64_t)base + (uint64_t)d;
    }
    *out = v;
    return 1;
}

static double parse_double(parser *p, const char *s, size_t n, int *ok)
{
    char small[128];
    char *buf = small;
    size_t i, j = 0;
    char point = '.';
    struct lconv *lc = localeconv();
    double d;
    char *end;
    if (lc != NULL && lc->decimal_point != NULL && lc->decimal_point[0] != 0 && lc->decimal_point[1] == 0) {
        point = lc->decimal_point[0];
    }
    if (n + 1 > sizeof small) {
        buf = (char *)mn_arena_alloc(p->ar, n + 1);
        if (buf == NULL) {
            *ok = 0;
            return 0;
        }
    }
    for (i = 0; i < n; i++) {
        if (s[i] == '_') {
            continue;
        }
        buf[j++] = s[i] == '.' ? point : s[i];
    }
    buf[j] = 0;
    errno = 0;
    d = strtod(buf, &end);
    *ok = *end == 0;
    return d;
}

static mncf_toml *number(parser *p)
{
    size_t start = p->pos;
    const char *s;
    size_t n, dec;
    char buf[16];
    mncf_toml *v;
    while (is_number_char(peek(p))) {
        p->pos++;
    }
    s = p->src + start;
    n = p->pos - start;
    if (n == 0) {
        toml_fail(p, start, "expected a value, found %s", found(p, buf, sizeof buf));
        return NULL;
    }
    dec = decimal_part(s, n);
    if (dec == n) {
        int negative = s[0] == '-';
        size_t skip = (s[0] == '+' || s[0] == '-') ? 1 : 0;
        uint64_t mag;
        if (!parse_uint(s + skip, n - skip, 10, negative ? (uint64_t)1 << 63 : (uint64_t)INT64_MAX, &mag)) {
            toml_fail(p, start, "integer %.*s does not fit in 64 bits", (int)n, s);
            return NULL;
        }
        v = new_node(p, MNCF_TOML_INTEGER);
        if (v != NULL) {
            v->u.i = negative ? (mag == (uint64_t)1 << 63 ? INT64_MIN : -(int64_t)mag) : (int64_t)mag;
        }
        return v;
    }
    if (n > 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'o' || s[1] == 'b')) {
        int base = s[1] == 'x' ? 16 : s[1] == 'o' ? 8 : 2;
        uint64_t mag;
        if (digits_ok(s + 2, n - 2, base)) {
            if (!parse_uint(s + 2, n - 2, base, (uint64_t)INT64_MAX, &mag)) {
                toml_fail(p, start, "integer %.*s does not fit in 64 bits", (int)n, s);
                return NULL;
            }
            v = new_node(p, MNCF_TOML_INTEGER);
            if (v != NULL) {
                v->u.i = (int64_t)mag;
            }
            return v;
        }
    }
    if (dec > 0) {
        /* A float: the decimal part, then a fraction, an exponent, or both. */
        size_t i = dec;
        int has = 0;
        if (i < n && s[i] == '.') {
            size_t f = i + 1, e = f;
            while (e < n && (is_digit((unsigned char)s[e]) || s[e] == '_')) {
                e++;
            }
            if (!digits_ok(s + f, e - f, 10)) {
                goto invalid;
            }
            i = e;
            has = 1;
        }
        if (i < n && (s[i] == 'e' || s[i] == 'E')) {
            size_t f = i + 1, e;
            if (f < n && (s[f] == '+' || s[f] == '-')) {
                f++;
            }
            e = f;
            while (e < n && (is_digit((unsigned char)s[e]) || s[e] == '_')) {
                e++;
            }
            if (!digits_ok(s + f, e - f, 10)) {
                goto invalid;
            }
            i = e;
            has = 1;
        }
        if (has && i == n) {
            int ok;
            double d = parse_double(p, s, n, &ok);
            if (!ok) {
                goto invalid;
            }
            v = new_node(p, MNCF_TOML_FLOAT);
            if (v != NULL) {
                v->u.f = d;
            }
            return v;
        }
    }
    {
        size_t skip = (s[0] == '+' || s[0] == '-') ? 1 : 0;
        if (mn_streqz(s + skip, n - skip, "inf") || mn_streqz(s + skip, n - skip, "nan")) {
            v = new_node(p, MNCF_TOML_FLOAT);
            if (v != NULL) {
                if (s[skip] == 'n') {
                    v->u.f = s[0] == '-' ? -NAN : NAN;
                    if (s[0] == '-' && !signbit(v->u.f)) {
                        v->u.f = -v->u.f;
                    }
                } else {
                    v->u.f = s[0] == '-' ? -INFINITY : INFINITY;
                }
            }
            return v;
        }
    }
invalid:
    toml_fail(p, start, "invalid value %.*s", (int)n, s);
    return NULL;
}

static int digits_at(const parser *p, size_t from, int count)
{
    int i;
    for (i = 0; i < count; i++) {
        if (!is_digit(at(p, from + (size_t)i))) {
            return 0;
        }
    }
    return 1;
}

static int read_digits(parser *p, int count, int *out)
{
    int v = 0;
    int i;
    char buf[16];
    for (i = 0; i < count; i++) {
        int c = peek(p);
        if (!is_digit(c)) {
            toml_fail(p, p->pos, "expected a digit, found %s", found(p, buf, sizeof buf));
            return 0;
        }
        v = v * 10 + c - '0';
        p->pos++;
    }
    *out = v;
    return 1;
}

static int days_in_month(int year, int month)
{
    if (month == 2) {
        return (year % 4 == 0 && (year % 100 != 0 || year % 400 == 0)) ? 29 : 28;
    }
    return (month == 4 || month == 6 || month == 9 || month == 11) ? 30 : 31;
}

static mncf_toml *date_time(parser *p)
{
    size_t start = p->pos;
    int has_date = at(p, p->pos + 2) != ':';
    int has_time = !has_date;
    int year = 0, month = 0, day = 0, hour = 0, minute = 0, second = 0;
    mncf_toml *v;
    char buf[16];
    if (has_date) {
        int c;
        if (!read_digits(p, 4, &year) || !expect(p, "-") || !read_digits(p, 2, &month) || !expect(p, "-") ||
            !read_digits(p, 2, &day)) {
            return NULL;
        }
        if (month < 1 || month > 12) {
            toml_fail(p, start, "invalid month %d", month);
            return NULL;
        }
        if (day < 1 || day > days_in_month(year, month)) {
            toml_fail(p, start, "invalid day %d in month %d of %d", day, month, year);
            return NULL;
        }
        c = peek(p);
        has_time = c == 'T' || c == 't' || (c == ' ' && is_digit(at(p, p->pos + 1)));
        if (has_time) {
            p->pos++;
        }
    }
    if (has_time) {
        int sign;
        if (!read_digits(p, 2, &hour) || !expect(p, ":") || !read_digits(p, 2, &minute) || !expect(p, ":") ||
            !read_digits(p, 2, &second)) {
            return NULL;
        }
        if (peek(p) == '.') {
            size_t from;
            p->pos++;
            from = p->pos;
            while (is_digit(peek(p))) {
                p->pos++;
            }
            if (p->pos == from) {
                toml_fail(p, p->pos, "expected a digit, found %s", found(p, buf, sizeof buf));
                return NULL;
            }
        }
        if (hour > 23) {
            toml_fail(p, start, "invalid hour %d", hour);
            return NULL;
        }
        if (minute > 59) {
            toml_fail(p, start, "invalid minute %d", minute);
            return NULL;
        }
        if (second > 60) {
            toml_fail(p, start, "invalid second %d", second);
            return NULL;
        }
        sign = peek(p);
        if (has_date && (sign == 'Z' || sign == 'z')) {
            p->pos++;
        } else if (has_date && (sign == '+' || sign == '-')) {
            size_t offset_start = p->pos++;
            int oh, om;
            if (!read_digits(p, 2, &oh) || !expect(p, ":") || !read_digits(p, 2, &om)) {
                return NULL;
            }
            if (oh > 23 || om > 59) {
                toml_fail(p, offset_start, "invalid offset %.*s", (int)(p->pos - offset_start), p->src + offset_start);
                return NULL;
            }
        }
    }
    v = new_node(p, MNCF_TOML_DATETIME);
    if (v == NULL) {
        return NULL;
    }
    v->u.s.n = p->pos - start;
    v->u.s.p = mn_arena_strndup(p->ar, p->src + start, v->u.s.n);
    if (v->u.s.p == NULL) {
        toml_nomem(p);
        return NULL;
    }
    return v;
}

static mncf_toml *string_node(parser *p, int (*read)(parser *, mn_str *))
{
    mncf_toml *v = new_node(p, MNCF_TOML_STRING);
    if (v == NULL || !read(p, &v->u.s)) {
        return NULL;
    }
    return v;
}

static int read_multi_basic(parser *p, mn_str *out)
{
    return multi_line_string(p, '"', out);
}

static int read_multi_literal(parser *p, mn_str *out)
{
    return multi_line_string(p, '\'', out);
}

static mncf_toml *value(parser *p)
{
    mncf_toml *v;
    if (starts(p, "true")) {
        p->pos += 4;
        v = new_node(p, MNCF_TOML_BOOLEAN);
        if (v != NULL) {
            v->u.b = 1;
        }
        return v;
    }
    if (starts(p, "false")) {
        p->pos += 5;
        v = new_node(p, MNCF_TOML_BOOLEAN);
        if (v != NULL) {
            v->u.b = 0;
        }
        return v;
    }
    switch (peek(p)) {
    case '"':
        return string_node(p, starts(p, "\"\"\"") ? read_multi_basic : basic_string);
    case '\'':
        return string_node(p, starts(p, "'''") ? read_multi_literal : literal_string);
    case '[':
        return array(p);
    case '{':
        return inline_table(p);
    default:
        if ((digits_at(p, p->pos, 4) && at(p, p->pos + 4) == '-') ||
            (digits_at(p, p->pos, 2) && at(p, p->pos + 2) == ':')) {
            return date_time(p);
        }
        return number(p);
    }
}

/* Steps from a table to the child that a dotted key names with key part i. */
static mncf_toml *dotted_step(parser *p, mncf_toml *parent, const key_parts *key, size_t i, size_t start)
{
    mncf_toml *child = child_of(parent, &key->parts[i]);
    if (child == NULL) {
        child = new_table(p, MN_ORIGIN_DOTTED);
        if (child == NULL || !put(p, parent, &key->parts[i], child)) {
            return NULL;
        }
        return child;
    }
    if (child->origin == MN_ORIGIN_IMPLICIT) {
        child->origin = MN_ORIGIN_DOTTED;
    } else if (child->origin != MN_ORIGIN_DOTTED) {
        toml_fail(p, start, "cannot add keys to %s, it is %s", key_name(p, key, i + 1), describe(child));
        return NULL;
    }
    return child;
}

static int key_value(parser *p, mncf_toml *table)
{
    size_t start = p->pos;
    key_parts key;
    mncf_toml *target = table;
    mncf_toml *v;
    size_t i;
    const mn_str *last;
    if (!read_key(p, &key) || !expect(p, "=")) {
        return 0;
    }
    skip_spaces(p);
    for (i = 0; i + 1 < key.n; i++) {
        target = dotted_step(p, target, &key, i, start);
        if (target == NULL) {
            return 0;
        }
    }
    last = &key.parts[key.n - 1];
    if (table_find(target, last->p, last->n) != (size_t)-1) {
        toml_fail(p, start, "duplicate key %s", key_name(p, &key, key.n));
        return 0;
    }
    v = value(p);
    if (v == NULL) {
        return 0;
    }
    return put(p, target, last, v);
}

/* Steps from a table to the child that a table header names with key part i. */
static mncf_toml *header_step(parser *p, mncf_toml *parent, const key_parts *key, size_t i, size_t start)
{
    mncf_toml *child = child_of(parent, &key->parts[i]);
    if (child == NULL) {
        child = new_table(p, MN_ORIGIN_IMPLICIT);
        if (child == NULL || !put(p, parent, &key->parts[i], child)) {
            return NULL;
        }
        return child;
    }
    if (child->origin == MN_ORIGIN_TABLE_ARRAY) {
        return child->u.a.items[child->u.a.n - 1];
    }
    if (child->origin != MN_ORIGIN_IMPLICIT && child->origin != MN_ORIGIN_HEADER &&
        child->origin != MN_ORIGIN_DOTTED) {
        toml_fail(p, start, "cannot add a table to %s, it is %s", key_name(p, key, i + 1), describe(child));
        return NULL;
    }
    return child;
}

static mncf_toml *table_header(parser *p, mncf_toml *root)
{
    size_t start = p->pos;
    int is_array = starts(p, "[[");
    key_parts key;
    mncf_toml *parent = root;
    mncf_toml *existing;
    size_t i;
    const mn_str *last;
    p->pos += is_array ? 2 : 1;
    skip_spaces(p);
    if (!read_key(p, &key) || !expect(p, is_array ? "]]" : "]")) {
        return NULL;
    }
    for (i = 0; i + 1 < key.n; i++) {
        parent = header_step(p, parent, &key, i, start);
        if (parent == NULL) {
            return NULL;
        }
    }
    last = &key.parts[key.n - 1];
    existing = child_of(parent, last);
    if (is_array) {
        mncf_toml *tables;
        mncf_toml *table;
        if (existing == NULL) {
            tables = new_node(p, MNCF_TOML_ARRAY);
            if (tables == NULL) {
                return NULL;
            }
            tables->origin = MN_ORIGIN_TABLE_ARRAY;
            if (!put(p, parent, last, tables)) {
                return NULL;
            }
        } else if (existing->origin == MN_ORIGIN_TABLE_ARRAY) {
            tables = existing;
        } else {
            toml_fail(p, start, "cannot append a table to %s, it is %s", key_name(p, &key, key.n), describe(existing));
            return NULL;
        }
        table = new_table(p, MN_ORIGIN_HEADER);
        if (table == NULL) {
            return NULL;
        }
        if (!array_add(p->ar, tables, table)) {
            toml_nomem(p);
            return NULL;
        }
        return table;
    }
    if (existing == NULL) {
        mncf_toml *table = new_table(p, MN_ORIGIN_HEADER);
        if (table == NULL || !put(p, parent, last, table)) {
            return NULL;
        }
        return table;
    }
    if (existing->origin == MN_ORIGIN_IMPLICIT) {
        existing->origin = MN_ORIGIN_HEADER;
        return existing;
    }
    toml_fail(p, start, "cannot define table %s, it is already %s", key_name(p, &key, key.n), describe(existing));
    return NULL;
}

mncf_toml *mn_toml_parse(mn_arena *ar, const char *text, size_t n, mncf_error *err)
{
    parser p;
    mncf_toml *root;
    mncf_toml *table;
    size_t bad;
    p.ar = ar;
    p.src = text;
    p.len = n;
    p.pos = 0;
    p.nesting = 0;
    p.err = err;
    p.failed = 0;
    if (!mn_utf8_valid((const uint8_t *)text, n, &bad)) {
        p.pos = bad;
        toml_fail(&p, bad, "the text is not UTF-8");
        return NULL;
    }
    root = new_table(&p, MN_ORIGIN_HEADER);
    if (root == NULL) {
        return NULL;
    }
    table = root;
    while (!p.failed && p.pos < p.len) {
        int c;
        skip_spaces(&p);
        c = peek(&p);
        if (c == '[') {
            table = table_header(&p, root);
            if (table == NULL) {
                break;
            }
        } else if (c != '#' && c != '\n' && c != '\r' && c != -1) {
            if (!key_value(&p, table)) {
                break;
            }
        }
        end_of_line(&p);
    }
    if (p.failed || ar->failed) {
        if (!p.failed) {
            mn_error_nomem(err);
        }
        return NULL;
    }
    return root;
}

/* ---- Checks after parsing --------------------------------------------------------------------------- */

int mn_toml_check_depth(const mncf_toml *v, int depth)
{
    size_t i;
    if (depth > MNCF_MAX_DEPTH) {
        return 1;
    }
    if (v->type == MNCF_TOML_DATETIME) {
        return 2;
    }
    if (v->type == MNCF_TOML_TABLE) {
        for (i = 0; i < v->u.t.n; i++) {
            int r = mn_toml_check_depth(v->u.t.e[i].val, depth + 1);
            if (r != 0) {
                return r;
            }
        }
    } else if (v->type == MNCF_TOML_ARRAY) {
        for (i = 0; i < v->u.a.n; i++) {
            int r = mn_toml_check_depth(v->u.a.items[i], depth + 1);
            if (r != 0) {
                return r;
            }
        }
    }
    return 0;
}

/* ---- Lookups and the API ---------------------------------------------------------------------------- */

const mncf_toml *mn_toml_get(const mncf_toml *t, const char *key, size_t n)
{
    size_t i;
    if (t == NULL || t->type != MNCF_TOML_TABLE) {
        return NULL;
    }
    i = table_find(t, key, n);
    return i == (size_t)-1 ? NULL : t->u.t.e[i].val;
}

const mncf_toml *mn_toml_getz(const mncf_toml *t, const char *key)
{
    return mn_toml_get(t, key, strlen(key));
}

int mn_toml_is(const mncf_toml *v, mncf_toml_type type)
{
    return v != NULL && v->type == type;
}

mncf_toml_doc *mncf_toml_parse(const char *text, size_t size, const mncf_allocator *alloc, mncf_error *err)
{
    mn_alloc a = mn_alloc_of(alloc);
    mncf_toml_doc *doc = (mncf_toml_doc *)mn_malloc(&a, sizeof *doc);
    mn_error_clear(err);
    if (doc == NULL) {
        mn_error_nomem(err);
        return NULL;
    }
    mn_arena_init(&doc->arena, a);
    doc->root = mn_toml_parse(&doc->arena, text != NULL ? text : "", text != NULL ? size : 0, err);
    if (doc->root == NULL) {
        mn_arena_free(&doc->arena);
        mn_free(&a, doc, sizeof *doc);
        return NULL;
    }
    return doc;
}

void mncf_toml_free(mncf_toml_doc *doc)
{
    mn_alloc a;
    if (doc == NULL) {
        return;
    }
    a = doc->arena.alloc;
    mn_arena_free(&doc->arena);
    mn_free(&a, doc, sizeof *doc);
}

const mncf_toml *mncf_toml_root(const mncf_toml_doc *doc)
{
    return doc != NULL ? doc->root : NULL;
}

mncf_toml_type mncf_toml_type_of(const mncf_toml *v)
{
    return v != NULL ? (mncf_toml_type)v->type : (mncf_toml_type)0;
}

const char *mncf_toml_string(const mncf_toml *v, size_t *size)
{
    if (v == NULL || (v->type != MNCF_TOML_STRING && v->type != MNCF_TOML_DATETIME)) {
        if (size != NULL) {
            *size = 0;
        }
        return NULL;
    }
    if (size != NULL) {
        *size = v->u.s.n;
    }
    return v->u.s.p;
}

int64_t mncf_toml_integer(const mncf_toml *v)
{
    return v != NULL && v->type == MNCF_TOML_INTEGER ? v->u.i : 0;
}

double mncf_toml_float(const mncf_toml *v)
{
    return v != NULL && v->type == MNCF_TOML_FLOAT ? v->u.f : 0.0;
}

int mncf_toml_boolean(const mncf_toml *v)
{
    return v != NULL && v->type == MNCF_TOML_BOOLEAN ? v->u.b : 0;
}

size_t mncf_toml_count(const mncf_toml *v)
{
    if (v == NULL) {
        return 0;
    }
    if (v->type == MNCF_TOML_ARRAY) {
        return v->u.a.n;
    }
    if (v->type == MNCF_TOML_TABLE) {
        return v->u.t.n;
    }
    return 0;
}

const mncf_toml *mncf_toml_at(const mncf_toml *v, size_t i)
{
    if (v == NULL) {
        return NULL;
    }
    if (v->type == MNCF_TOML_ARRAY && i < v->u.a.n) {
        return v->u.a.items[i];
    }
    if (v->type == MNCF_TOML_TABLE && i < v->u.t.n) {
        return v->u.t.e[i].val;
    }
    return NULL;
}

const char *mncf_toml_key(const mncf_toml *v, size_t i, size_t *size)
{
    if (v == NULL || v->type != MNCF_TOML_TABLE || i >= v->u.t.n) {
        if (size != NULL) {
            *size = 0;
        }
        return NULL;
    }
    if (size != NULL) {
        *size = v->u.t.e[i].key.n;
    }
    return v->u.t.e[i].key.p;
}

const mncf_toml *mncf_toml_get(const mncf_toml *table, const char *key)
{
    return key != NULL ? mn_toml_getz(table, key) : NULL;
}

const mncf_toml *mncf_toml_getn(const mncf_toml *table, const char *key, size_t size)
{
    return key != NULL ? mn_toml_get(table, key, size) : NULL;
}
