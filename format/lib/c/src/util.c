/* util.c: memory, buffers, errors, text and numbers. MIT License. */
#include "internal.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>

const char *mncf_version(void)
{
    return MNCF_VERSION;
}

/* ---- Memory ----------------------------------------------------------------------------------------- */

static void *default_alloc(void *ctx, void *ptr, size_t old_size, size_t new_size)
{
    (void)ctx;
    (void)old_size;
    if (new_size == 0) {
        free(ptr);
        return NULL;
    }
    return realloc(ptr, new_size);
}

mn_alloc mn_alloc_of(const mncf_allocator *a)
{
    mn_alloc out;
    if (a != NULL && a->fn != NULL) {
        out.fn = a->fn;
        out.ctx = a->ctx;
    } else {
        out.fn = default_alloc;
        out.ctx = NULL;
    }
    return out;
}

void *mn_malloc(const mn_alloc *a, size_t size)
{
    if (size == 0) {
        size = 1;
    }
    return a->fn(a->ctx, NULL, 0, size);
}

void *mn_calloc(const mn_alloc *a, size_t size)
{
    void *p = mn_malloc(a, size);
    if (p != NULL) {
        memset(p, 0, size);
    }
    return p;
}

void *mn_realloc(const mn_alloc *a, void *p, size_t old_size, size_t new_size)
{
    if (new_size == 0) {
        new_size = 1;
    }
    return a->fn(a->ctx, p, old_size, new_size);
}

void mn_free(const mn_alloc *a, void *p, size_t size)
{
    if (p != NULL) {
        a->fn(a->ctx, p, size, 0);
    }
}

void mn_free_const(const mn_alloc *a, const void *p, size_t size)
{
    void *q;
    memcpy(&q, &p, sizeof q);
    mn_free(a, q, size);
}

/* ---- Arena ------------------------------------------------------------------------------------------ */

#define MN_ALIGN 16u
#define MN_CHUNK_MIN (16u * 1024u)
#define MN_CHUNK_MAX (1024u * 1024u)

struct mn_chunk {
    mn_chunk *next;
    size_t cap;
    /* the payload follows, aligned */
};

static size_t chunk_header(void)
{
    return (sizeof(mn_chunk) + MN_ALIGN - 1) & ~(size_t)(MN_ALIGN - 1);
}

void mn_arena_init(mn_arena *ar, mn_alloc alloc)
{
    ar->alloc = alloc;
    ar->head = NULL;
    ar->used = 0;
    ar->cap = 0;
    ar->failed = 0;
    ar->grow_failed = 0;
}

void mn_arena_free(mn_arena *ar)
{
    mn_chunk *c = ar->head;
    while (c != NULL) {
        mn_chunk *next = c->next;
        mn_free(&ar->alloc, c, chunk_header() + c->cap);
        c = next;
    }
    ar->head = NULL;
    ar->used = 0;
    ar->cap = 0;
}

void mn_arena_reset(mn_arena *ar)
{
    mn_chunk *c = ar->head;
    if (c == NULL) {
        return;
    }
    while (c->next != NULL) {
        mn_chunk *next = c->next;
        c->next = next->next;
        mn_free(&ar->alloc, next, chunk_header() + next->cap);
    }
    ar->used = 0;
    ar->cap = c->cap;
    ar->failed = 0;
}

void *mn_arena_alloc(mn_arena *ar, size_t size)
{
    size_t need = (size + MN_ALIGN - 1) & ~(size_t)(MN_ALIGN - 1);
    if (need < size) {
        ar->failed = 1;
        return NULL;
    }
    if (ar->head == NULL || need > ar->cap - ar->used) {
        size_t grow = ar->head == NULL ? MN_CHUNK_MIN : ar->head->cap * 2;
        mn_chunk *c;
        if (grow > MN_CHUNK_MAX) {
            grow = MN_CHUNK_MAX;
        }
        if (grow < need) {
            grow = need;
        }
        if (grow > (size_t)-1 - chunk_header()) {
            ar->failed = 1;
            return NULL;
        }
        c = (mn_chunk *)mn_malloc(&ar->alloc, chunk_header() + grow);
        if (c == NULL) {
            ar->failed = 1;
            return NULL;
        }
        c->cap = grow;
        if (ar->head != NULL && need > MN_CHUNK_MAX && ar->cap - ar->used > MN_CHUNK_MIN) {
            /* A large block goes behind the current chunk, which keeps its free room. */
            c->next = ar->head->next;
            ar->head->next = c;
            return (char *)c + chunk_header();
        }
        c->next = ar->head;
        ar->head = c;
        ar->used = 0;
        ar->cap = grow;
    }
    {
        char *p = (char *)ar->head + chunk_header() + ar->used;
        ar->used += need;
        return p;
    }
}

void *mn_arena_zalloc(mn_arena *ar, size_t size)
{
    void *p = mn_arena_alloc(ar, size);
    if (p != NULL) {
        memset(p, 0, size);
    }
    return p;
}

void *mn_arena_dup(mn_arena *ar, const void *p, size_t size)
{
    void *q = mn_arena_alloc(ar, size == 0 ? 1 : size);
    if (q != NULL && size > 0) {
        memcpy(q, p, size);
    }
    return q;
}

char *mn_arena_strndup(mn_arena *ar, const char *s, size_t n)
{
    char *q = (char *)mn_arena_alloc(ar, n + 1);
    if (q != NULL) {
        if (n > 0) {
            memcpy(q, s, n);
        }
        q[n] = 0;
    }
    return q;
}

char *mn_arena_strdup(mn_arena *ar, const char *s)
{
    return s == NULL ? NULL : mn_arena_strndup(ar, s, strlen(s));
}

char *mn_arena_printf(mn_arena *ar, const char *fmt, ...)
{
    va_list ap;
    int n;
    char *q;
    va_start(ap, fmt);
    n = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    if (n < 0) {
        ar->failed = 1;
        return NULL;
    }
    q = (char *)mn_arena_alloc(ar, (size_t)n + 1);
    if (q == NULL) {
        return NULL;
    }
    va_start(ap, fmt);
    vsnprintf(q, (size_t)n + 1, fmt, ap);
    va_end(ap);
    return q;
}

void *mn_arena_grow(mn_arena *ar, void *items, size_t count, size_t *cap, size_t item_size)
{
    ar->grow_failed = 0;
    if (count >= *cap) {
        size_t next = *cap == 0 ? 4 : *cap * 2;
        void *grown;
        if (next < *cap || next > (size_t)-1 / item_size) {
            ar->failed = 1;
            ar->grow_failed = 1;
            return items;
        }
        grown = mn_arena_alloc(ar, next * item_size);
        if (grown == NULL) {
            ar->grow_failed = 1;
            return items;
        }
        if (count > 0) {
            memcpy(grown, items, count * item_size);
        }
        *cap = next;
        return grown;
    }
    return items;
}

/* ---- Buffers ---------------------------------------------------------------------------------------- */

void mn_buf_init(mn_buf *b, mn_alloc alloc)
{
    b->alloc = alloc;
    b->data = NULL;
    b->size = 0;
    b->cap = 0;
    b->failed = 0;
}

void mn_buf_free(mn_buf *b)
{
    mn_free(&b->alloc, b->data, b->cap);
    b->data = NULL;
    b->size = 0;
    b->cap = 0;
}

void mn_buf_add(mn_buf *b, const void *p, size_t n)
{
    if (b->failed) {
        return;
    }
    if (n + 1 > b->cap - b->size || b->data == NULL) {
        size_t next = b->cap == 0 ? 256 : b->cap;
        char *grown;
        while (next - b->size < n + 1) {
            if (next > (size_t)-1 / 2) {
                b->failed = 1;
                return;
            }
            next *= 2;
        }
        grown = (char *)mn_realloc(&b->alloc, b->data, b->cap, next);
        if (grown == NULL) {
            b->failed = 1;
            return;
        }
        b->data = grown;
        b->cap = next;
    }
    if (n > 0) {
        memcpy(b->data + b->size, p, n);
    }
    b->size += n;
    b->data[b->size] = 0;
}

void mn_buf_addc(mn_buf *b, char c)
{
    mn_buf_add(b, &c, 1);
}

void mn_buf_adds(mn_buf *b, const char *s)
{
    mn_buf_add(b, s, strlen(s));
}

void mn_buf_printf(mn_buf *b, const char *fmt, ...)
{
    char small[256];
    va_list ap;
    int n;
    va_start(ap, fmt);
    n = vsnprintf(small, sizeof small, fmt, ap);
    va_end(ap);
    if (n < 0) {
        b->failed = 1;
        return;
    }
    if ((size_t)n < sizeof small) {
        mn_buf_add(b, small, (size_t)n);
        return;
    }
    {
        char *big = (char *)mn_malloc(&b->alloc, (size_t)n + 1);
        if (big == NULL) {
            b->failed = 1;
            return;
        }
        va_start(ap, fmt);
        vsnprintf(big, (size_t)n + 1, fmt, ap);
        va_end(ap);
        mn_buf_add(b, big, (size_t)n);
        mn_free(&b->alloc, big, (size_t)n + 1);
    }
}

/* ---- Errors ----------------------------------------------------------------------------------------- */

size_t mn_utf8_cut(const char *s, size_t n, size_t max)
{
    if (n <= max) {
        return n;
    }
    n = max;
    while (n > 0 && ((unsigned char)s[n] & 0xC0) == 0x80) {
        n--;
    }
    return n;
}

void mn_error(mncf_error *err, mncf_status status, const char *code, const char *fmt, ...)
{
    va_list ap;
    size_t n;
    char message[1024];
    if (err == NULL) {
        return;
    }
    err->status = status;
    n = mn_utf8_cut(code, strlen(code), sizeof err->code - 1);
    memcpy(err->code, code, n);
    err->code[n] = 0;
    va_start(ap, fmt);
    vsnprintf(message, sizeof message, fmt, ap);
    va_end(ap);
    n = mn_utf8_cut(message, strlen(message), sizeof err->message - 1);
    memcpy(err->message, message, n);
    err->message[n] = 0;
}

void mn_error_nomem(mncf_error *err)
{
    mn_error(err, MNCF_ERR_MEMORY, "memory", "the allocator gave no memory");
}

void mn_error_clear(mncf_error *err)
{
    if (err != NULL) {
        err->status = MNCF_OK;
        err->code[0] = 0;
        err->message[0] = 0;
    }
}

/* ---- Text ------------------------------------------------------------------------------------------- */

int mn_utf8_valid(const uint8_t *p, size_t n, size_t *at)
{
    size_t i = 0;
    while (i < n) {
        uint8_t c = p[i];
        size_t len;
        uint32_t cp;
        size_t k;
        if (c < 0x80) {
            i++;
            continue;
        }
        if (c >= 0xC2 && c <= 0xDF) {
            len = 2;
            cp = c & 0x1F;
        } else if (c >= 0xE0 && c <= 0xEF) {
            len = 3;
            cp = c & 0x0F;
        } else if (c >= 0xF0 && c <= 0xF4) {
            len = 4;
            cp = c & 0x07;
        } else {
            *at = i;
            return 0;
        }
        if (n - i < len) {
            *at = i;
            return 0;
        }
        for (k = 1; k < len; k++) {
            if ((p[i + k] & 0xC0) != 0x80) {
                *at = i;
                return 0;
            }
            cp = (cp << 6) | (p[i + k] & 0x3F);
        }
        if ((len == 3 && (cp < 0x800 || (cp >= 0xD800 && cp <= 0xDFFF))) || (len == 4 && (cp < 0x10000 || cp > 0x10FFFF))) {
            *at = i;
            return 0;
        }
        i += len;
    }
    return 1;
}

size_t mn_utf8_decode(const uint8_t *p, size_t n, uint32_t *cp)
{
    uint8_t c = p[0];
    if (c >= 0xC0 && c < 0xE0 && n >= 2) {
        *cp = ((uint32_t)(c & 0x1F) << 6) | (p[1] & 0x3F);
        return 2;
    }
    if (c >= 0xE0 && c < 0xF0 && n >= 3) {
        *cp = ((uint32_t)(c & 0x0F) << 12) | ((uint32_t)(p[1] & 0x3F) << 6) | (p[2] & 0x3F);
        return 3;
    }
    if (c >= 0xF0 && n >= 4) {
        *cp = ((uint32_t)(c & 0x07) << 18) | ((uint32_t)(p[1] & 0x3F) << 12) | ((uint32_t)(p[2] & 0x3F) << 6) |
              (p[3] & 0x3F);
        return 4;
    }
    *cp = c;
    return 1;
}

int mn_py_space(uint32_t c)
{
    return (c >= 0x09 && c <= 0x0D) || (c >= 0x1C && c <= 0x20) || c == 0x85 || c == 0xA0 || c == 0x1680 ||
           (c >= 0x2000 && c <= 0x200A) || c == 0x2028 || c == 0x2029 || c == 0x202F || c == 0x205F ||
           c == 0x3000;
}

size_t mn_utf8_count(const char *s, size_t n)
{
    size_t count = 0;
    size_t i;
    for (i = 0; i < n; i++) {
        if (((unsigned char)s[i] & 0xC0) != 0x80) {
            count++;
        }
    }
    return count;
}

int mn_cmp_names(const void *a, const void *b)
{
    return strcmp(*(const char *const *)a, *(const char *const *)b);
}

int mn_is_hex(int c)
{
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

int mn_hex_value(int c)
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

int mn_streq(const char *a, size_t an, const char *b, size_t bn)
{
    return an == bn && (an == 0 || memcmp(a, b, an) == 0);
}

int mn_streqz(const char *a, size_t an, const char *z)
{
    return mn_streq(a, an, z, strlen(z));
}

uint64_t mn_hash(const void *p, size_t n)
{
    const unsigned char *s = (const unsigned char *)p;
    uint64_t h = 1469598103934665603ULL;
    size_t i;
    for (i = 0; i < n; i++) {
        h ^= s[i];
        h *= 1099511628211ULL;
    }
    return h ^ (h >> 29);
}

/* ---- Numbers ---------------------------------------------------------------------------------------- */

mn_num mn_num_none(void)
{
    mn_num n;
    memset(&n, 0, sizeof n);
    n.kind = MNCF_NUMBER_NONE;
    return n;
}

mn_num mn_num_signed(int negative, uint64_t magnitude)
{
    mn_num n = mn_num_none();
    n.kind = MNCF_NUMBER_INT;
    n.negative = negative && magnitude != 0;
    n.magnitude = magnitude;
    return n;
}

mn_num mn_num_i64(int64_t v)
{
    if (v < 0) {
        return mn_num_signed(1, (uint64_t)0 - (uint64_t)v);
    }
    return mn_num_signed(0, (uint64_t)v);
}

mn_num mn_num_u64(uint64_t v)
{
    return mn_num_signed(0, v);
}

mn_num mn_num_f64(double v)
{
    mn_num n = mn_num_none();
    n.kind = MNCF_NUMBER_FLOAT;
    n.real = v;
    return n;
}

mn_num mn_num_bool(int v)
{
    mn_num n = mn_num_none();
    n.kind = MNCF_NUMBER_BOOL;
    n.magnitude = v ? 1 : 0;
    return n;
}

mncf_number mncf_number_int(int64_t v)
{
    return mn_num_i64(v);
}

mncf_number mncf_number_uint(uint64_t v)
{
    return mn_num_u64(v);
}

mncf_number mncf_number_float(double v)
{
    return mn_num_f64(v);
}

mncf_number mncf_number_bool(int v)
{
    return mn_num_bool(v);
}

int mncf_number_to_int64(mncf_number n, int64_t *out)
{
    if ((n.kind != MNCF_NUMBER_INT && n.kind != MNCF_NUMBER_BOOL) || n.huge) {
        return 0;
    }
    if (n.negative) {
        if (n.magnitude > (uint64_t)1 << 63) {
            return 0;
        }
        *out = n.magnitude == (uint64_t)1 << 63 ? INT64_MIN : -(int64_t)n.magnitude;
        return 1;
    }
    if (n.magnitude > (uint64_t)INT64_MAX) {
        return 0;
    }
    *out = (int64_t)n.magnitude;
    return 1;
}

int mn_num_is_int(const mn_num *n)
{
    return n->kind == MNCF_NUMBER_INT;
}

double mn_u64_to_double(uint64_t v)
{
    int bits = 0;
    uint64_t x = v;
    int shift;
    uint64_t m, rest, half;
    if (v < ((uint64_t)1 << 53)) {
        return (double)v;
    }
    while (x != 0) {
        bits++;
        x >>= 1;
    }
    shift = bits - 53;
    m = v >> shift;
    rest = v & (((uint64_t)1 << shift) - 1);
    half = (uint64_t)1 << (shift - 1);
    if (rest > half || (rest == half && (m & 1))) {
        m++;
    }
    return ldexp((double)m, shift);
}

double mn_num_to_double(const mn_num *n)
{
    double d;
    switch (n->kind) {
    case MNCF_NUMBER_FLOAT:
        return n->real;
    case MNCF_NUMBER_BOOL:
        return (double)n->magnitude;
    case MNCF_NUMBER_INT:
        d = mn_u64_to_double(n->magnitude);
        if (n->huge) {
            d += 18446744073709551616.0 * n->huge;
        }
        return n->negative ? -d : d;
    default:
        return 0.0;
    }
}

uint64_t mn_num_low64(const mn_num *n)
{
    if (n->kind == MNCF_NUMBER_BOOL) {
        return n->magnitude;
    }
    return n->negative ? (uint64_t)0 - n->magnitude : n->magnitude;
}

/* Compares two whole numbers given by sign, magnitude and whether they are above 2^64 - 1. */
static int cmp_int(int an, uint64_t am, int ah, int bn, uint64_t bm, int bh)
{
    if (am == 0 && !ah) {
        an = 0;
    }
    if (bm == 0 && !bh) {
        bn = 0;
    }
    if (an != bn) {
        return an ? -1 : 1;
    }
    {
        int c;
        if (ah != bh) {
            c = ah < bh ? -1 : 1;
        } else {
            c = am < bm ? -1 : am > bm ? 1 : 0;
        }
        return an ? -c : c;
    }
}

/* Compares a whole number with a float, exactly. */
static int cmp_int_float(int neg, uint64_t mag, int huge, double d)
{
    double t, frac;
    int tneg;
    uint64_t tmag;
    int c;
    if (d != d) {
        return 2;
    }
    if (huge) {
        double v = mn_u64_to_double(mag) + 18446744073709551616.0 * huge;
        if (neg) {
            v = -v;
        }
        return v < d ? -1 : v > d ? 1 : 0;
    }
    if (d >= 18446744073709551616.0) {
        return -1;
    }
    if (d <= -18446744073709551616.0) {
        return 1;
    }
    t = d < 0 ? ceil(d) : floor(d);
    frac = d - t;
    tneg = t < 0;
    tmag = (uint64_t)(tneg ? -t : t);
    c = cmp_int(neg, mag, 0, tneg, tmag, 0);
    if (c != 0) {
        return c;
    }
    return frac > 0 ? -1 : frac < 0 ? 1 : 0;
}

int mn_num_cmp(const mn_num *a, const mn_num *b)
{
    int ak = a->kind == MNCF_NUMBER_BOOL ? MNCF_NUMBER_INT : a->kind;
    int bk = b->kind == MNCF_NUMBER_BOOL ? MNCF_NUMBER_INT : b->kind;
    if (ak == MNCF_NUMBER_NONE || bk == MNCF_NUMBER_NONE) {
        return 2;
    }
    if (ak == MNCF_NUMBER_INT && bk == MNCF_NUMBER_INT) {
        return cmp_int(a->negative, a->magnitude, a->huge, b->negative, b->magnitude, b->huge);
    }
    if (ak == MNCF_NUMBER_FLOAT && bk == MNCF_NUMBER_FLOAT) {
        if (a->real != a->real || b->real != b->real) {
            return 2;
        }
        return a->real < b->real ? -1 : a->real > b->real ? 1 : 0;
    }
    if (ak == MNCF_NUMBER_INT) {
        return cmp_int_float(a->negative, a->magnitude, a->huge, b->real);
    }
    {
        int c = cmp_int_float(b->negative, b->magnitude, b->huge, a->real);
        return c == 2 ? 2 : -c;
    }
}

int mn_num_eq(const mn_num *a, const mn_num *b)
{
    return mn_num_cmp(a, b) == 0;
}

/* ---- Types ------------------------------------------------------------------------------------------ */

static const char *const TYPE_NAMES[] = {"", "u8", "u16", "u32", "u64", "s8", "s16", "s32", "s64", "f32", "f64",
                                         "bool", "bytes"};

const char *mncf_type_name(mncf_type type)
{
    if ((int)type < 0 || (size_t)type >= sizeof TYPE_NAMES / sizeof TYPE_NAMES[0]) {
        return "";
    }
    return TYPE_NAMES[type];
}

mncf_type mn_type_parse(const char *s, size_t n)
{
    size_t i;
    for (i = 1; i < sizeof TYPE_NAMES / sizeof TYPE_NAMES[0]; i++) {
        if (mn_streqz(s, n, TYPE_NAMES[i])) {
            return (mncf_type)i;
        }
    }
    return MNCF_TYPE_NONE;
}

int mn_type_is_int(mncf_type t)
{
    return t >= MNCF_TYPE_U8 && t <= MNCF_TYPE_S64;
}

int mn_type_is_float(mncf_type t)
{
    return t == MNCF_TYPE_F32 || t == MNCF_TYPE_F64;
}

int mn_type_is_number(mncf_type t)
{
    return mn_type_is_int(t) || mn_type_is_float(t);
}

int mn_type_signed(mncf_type t)
{
    return t >= MNCF_TYPE_S8 && t <= MNCF_TYPE_S64;
}

size_t mn_type_size(mncf_type t)
{
    switch (t) {
    case MNCF_TYPE_U8:
    case MNCF_TYPE_S8:
    case MNCF_TYPE_BOOL:
        return 1;
    case MNCF_TYPE_U16:
    case MNCF_TYPE_S16:
        return 2;
    case MNCF_TYPE_U32:
    case MNCF_TYPE_S32:
    case MNCF_TYPE_F32:
        return 4;
    case MNCF_TYPE_U64:
    case MNCF_TYPE_S64:
    case MNCF_TYPE_F64:
        return 8;
    default:
        return 0;
    }
}

int mn_fits(mncf_type t, const mn_num *v)
{
    if (t == MNCF_TYPE_BOOL) {
        if (v->kind == MNCF_NUMBER_BOOL) {
            return 1;
        }
        return v->kind == MNCF_NUMBER_INT && !v->huge && !v->negative && v->magnitude <= 1;
    }
    if (mn_type_is_int(t)) {
        unsigned bits = (unsigned)(8 * mn_type_size(t));
        if (v->kind != MNCF_NUMBER_INT || v->huge) {
            return 0;
        }
        if (mn_type_signed(t)) {
            uint64_t half = (uint64_t)1 << (bits - 1);
            return v->negative ? v->magnitude <= half : v->magnitude <= half - 1;
        }
        if (v->negative) {
            return 0;
        }
        return bits == 64 || v->magnitude <= (((uint64_t)1 << bits) - 1);
    }
    if (mn_type_is_float(t)) {
        if (v->kind != MNCF_NUMBER_INT && v->kind != MNCF_NUMBER_FLOAT) {
            return 0;
        }
        if (t == MNCF_TYPE_F32 && v->kind == MNCF_NUMBER_FLOAT && isfinite(v->real)) {
            volatile float y = (float)v->real;
            return isfinite(y) ? 1 : 0;
        }
        return 1;
    }
    return 0;
}

static void put_uint(uint8_t *out, uint64_t v, size_t size, int big)
{
    size_t i;
    for (i = 0; i < size; i++) {
        uint8_t b = (uint8_t)(v >> (8 * i));
        out[big ? size - 1 - i : i] = b;
    }
}

void mn_encode_value(mncf_type t, const mn_num *v, int big_endian, uint8_t *out)
{
    if (t == MNCF_TYPE_BOOL) {
        out[0] = (uint8_t)((v->kind == MNCF_NUMBER_FLOAT ? v->real != 0 : v->magnitude != 0) ? 1 : 0);
        return;
    }
    if (mn_type_is_int(t)) {
        put_uint(out, mn_num_low64(v), mn_type_size(t), big_endian);
        return;
    }
    if (t == MNCF_TYPE_F32) {
        volatile float rounded = (float)mn_num_to_double(v);
        float f = rounded;
        uint32_t bits;
        memcpy(&bits, &f, 4);
        put_uint(out, bits, 4, big_endian);
        return;
    }
    if (t == MNCF_TYPE_F64) {
        double d = mn_num_to_double(v);
        uint64_t bits;
        memcpy(&bits, &d, 8);
        put_uint(out, bits, 8, big_endian);
    }
}

mn_num mn_decode_value(mncf_type t, const uint8_t *raw, int big_endian)
{
    size_t size = mn_type_size(t);
    uint64_t v = 0;
    size_t i;
    for (i = 0; i < size; i++) {
        v |= (uint64_t)raw[big_endian ? size - 1 - i : i] << (8 * i);
    }
    if (t == MNCF_TYPE_F32) {
        uint32_t bits = (uint32_t)v;
        float f;
        memcpy(&f, &bits, 4);
        return mn_num_f64((double)f);
    }
    if (t == MNCF_TYPE_F64) {
        double d;
        memcpy(&d, &v, 8);
        return mn_num_f64(d);
    }
    if (mn_type_signed(t) && size < 8) {
        uint64_t sign = (uint64_t)1 << (8 * size - 1);
        if (v & sign) {
            return mn_num_signed(1, (((uint64_t)1 << (8 * size)) - v));
        }
        return mn_num_u64(v);
    }
    if (mn_type_signed(t) && (v >> 63)) {
        return mn_num_signed(1, (uint64_t)0 - v);
    }
    return mn_num_u64(v);
}

/* ---- Names ------------------------------------------------------------------------------------------ */

const char *mncf_state_name(mncf_state state)
{
    switch (state) {
    case MNCF_STATE_ON:
        return "on";
    case MNCF_STATE_WAITING:
        return "waiting";
    case MNCF_STATE_REFUSED:
        return "refused";
    default:
        return "off";
    }
}

const char *mncf_op_name(mncf_op_kind kind)
{
    switch (kind) {
    case MNCF_OP_PATCH:
        return "patch";
    case MNCF_OP_WRITE:
        return "write";
    case MNCF_OP_OVERRIDE:
        return "override";
    case MNCF_OP_READ:
        return "read";
    case MNCF_OP_WHEN:
        return "when";
    case MNCF_OP_RAW:
        return "raw";
    default:
        return "";
    }
}

const char *mncf_compare_name(mncf_compare op)
{
    static const char *const names[] = {"", "==", "!=", "<", "<=", ">", ">=", "&", "!&", "between", "changed"};
    if ((int)op < 0 || (size_t)op >= sizeof names / sizeof names[0]) {
        return "";
    }
    return names[op];
}

const char *mncf_change_name(mncf_change_kind kind)
{
    switch (kind) {
    case MNCF_CHANGE_PATCH:
        return "patch";
    case MNCF_CHANGE_RESTORE:
        return "restore";
    case MNCF_CHANGE_HOOK:
        return "hook";
    case MNCF_CHANGE_UNHOOK:
        return "unhook";
    case MNCF_CHANGE_WRITE:
        return "write";
    case MNCF_CHANGE_RAW:
        return "raw";
    default:
        return "";
    }
}

const char *mncf_signature_name(mncf_signature_result result)
{
    switch (result) {
    case MNCF_SIGNATURE_VERIFIED:
        return "verified";
    case MNCF_SIGNATURE_UNKNOWN_KEY:
        return "unknown-key";
    case MNCF_SIGNATURE_INVALID:
        return "invalid";
    case MNCF_SIGNATURE_REVOKED:
        return "revoked";
    default:
        return "unsigned";
    }
}

const char *mncf_pack_status_name(mncf_pack_status status)
{
    switch (status) {
    case MNCF_PACK_HASH:
        return "hash";
    case MNCF_PACK_MISSING:
        return "missing";
    default:
        return "ok";
    }
}
