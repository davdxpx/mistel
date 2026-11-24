/* internal.h: what the source files of the library share. Not part of the API. MIT License. */
#ifndef MNCF_INTERNAL_H
#define MNCF_INTERNAL_H

#include "mncf.h"

#include <stdarg.h>
#include <string.h>

/* printf-style functions, so that GCC and Clang check their formats. */
#if defined(__GNUC__) || defined(__clang__)
#define MN_PRINTF(f, a) __attribute__((format(printf, f, a)))
#else
#define MN_PRINTF(f, a)
#endif

/* ---- Memory ----------------------------------------------------------------------------------------- */

typedef struct mn_alloc {
    mncf_alloc_fn fn;
    void *ctx;
} mn_alloc;

mn_alloc mn_alloc_of(const mncf_allocator *a);
void *mn_malloc(const mn_alloc *a, size_t size);
void *mn_calloc(const mn_alloc *a, size_t size);
void *mn_realloc(const mn_alloc *a, void *p, size_t old_size, size_t new_size);
void mn_free(const mn_alloc *a, void *p, size_t size);
void mn_free_const(const mn_alloc *a, const void *p, size_t size);   /* frees memory the caller holds as const */

/* An arena: memory that is freed all at once. Allocations are aligned for any scalar. */
typedef struct mn_chunk mn_chunk;
typedef struct mn_arena {
    mn_alloc alloc;
    mn_chunk *head;
    size_t used;          /* bytes used in head */
    size_t cap;           /* bytes of head's payload */
    int failed;           /* an allocation failed */
    int grow_failed;      /* the last mn_arena_grow failed */
} mn_arena;

void mn_arena_init(mn_arena *ar, mn_alloc alloc);
void mn_arena_free(mn_arena *ar);
void mn_arena_reset(mn_arena *ar);   /* frees everything but keeps the first chunk */
void *mn_arena_alloc(mn_arena *ar, size_t size);
void *mn_arena_zalloc(mn_arena *ar, size_t size);
void *mn_arena_dup(mn_arena *ar, const void *p, size_t size);
char *mn_arena_strndup(mn_arena *ar, const char *s, size_t n);
char *mn_arena_strdup(mn_arena *ar, const char *s);
char *mn_arena_printf(mn_arena *ar, const char *fmt, ...) MN_PRINTF(2, 3);

/* Makes room for one more item in an array in the arena: returns the array, moved when it grew. Where there
 * is no memory, it returns the array as it was and sets grow_failed. MN_GROW assigns the array back and is
 * NULL where it failed, so that items[count] can be written when it is not. */
void *mn_arena_grow(mn_arena *ar, void *items, size_t count, size_t *cap, size_t item_size);
#define MN_GROW(ar, items, count, cap)                                                                           \
    ((items) = mn_arena_grow((ar), (items), (count), &(cap), sizeof *(items)), (ar)->grow_failed ? NULL : (items))

/* A growable byte buffer on the allocator. */
typedef struct mn_buf {
    mn_alloc alloc;
    char *data;
    size_t size;
    size_t cap;
    int failed;
} mn_buf;

void mn_buf_init(mn_buf *b, mn_alloc alloc);
void mn_buf_free(mn_buf *b);
void mn_buf_add(mn_buf *b, const void *p, size_t n);
void mn_buf_addc(mn_buf *b, char c);
void mn_buf_adds(mn_buf *b, const char *s);
void mn_buf_printf(mn_buf *b, const char *fmt, ...) MN_PRINTF(2, 3);

/* ---- Errors ----------------------------------------------------------------------------------------- */

/* Sets err (which may be NULL) to the status, a code and a message. */
void mn_error(mncf_error *err, mncf_status status, const char *code, const char *fmt, ...) MN_PRINTF(4, 5);
void mn_error_nomem(mncf_error *err);
void mn_error_clear(mncf_error *err);
size_t mn_utf8_cut(const char *s, size_t n, size_t max);   /* a length <= max that ends on a character */

/* ---- Text ------------------------------------------------------------------------------------------- */

/* Whether the bytes are UTF-8 as Python's strict decoder takes it. *at gets the offset of the first bad
 * byte. */
int mn_utf8_valid(const uint8_t *p, size_t n, size_t *at);
/* Decodes the code point at p (valid UTF-8 assumed); returns its length. */
size_t mn_utf8_decode(const uint8_t *p, size_t n, uint32_t *cp);
/* Whether a code point is white space as Python's str.isspace() and \s see it. */
int mn_py_space(uint32_t cp);
size_t mn_utf8_count(const char *s, size_t n);     /* code points */

int mn_is_hex(int c);
int mn_cmp_names(const void *a, const void *b);   /* qsort of const char * by strcmp */
int mn_hex_value(int c);
int mn_streq(const char *a, size_t an, const char *b, size_t bn);
int mn_streqz(const char *a, size_t an, const char *z);
uint64_t mn_hash(const void *p, size_t n);

/* A string with its length; it also ends with a NUL byte. */
typedef struct mn_str {
    const char *p;
    size_t n;
} mn_str;

/* A set of strings to indexes, in an arena, for lookups in large tables. */
typedef struct mn_index {
    uint32_t *slots;      /* index + 1, 0 = empty */
    size_t cap;           /* a power of two, or 0 */
} mn_index;

/* ---- Numbers ---------------------------------------------------------------------------------------- */

typedef mncf_number mn_num;

mn_num mn_num_none(void);
mn_num mn_num_i64(int64_t v);
mn_num mn_num_u64(uint64_t v);
mn_num mn_num_f64(double v);
mn_num mn_num_bool(int v);
mn_num mn_num_signed(int negative, uint64_t magnitude);
int mn_num_is_int(const mn_num *n);         /* INT, not bool */
/* Compares two numbers by their exact values: -1, 0, 1, or 2 where one is NaN. Bools count as 0 and 1. */
int mn_num_cmp(const mn_num *a, const mn_num *b);
int mn_num_eq(const mn_num *a, const mn_num *b);     /* Python's == on numbers */
double mn_num_to_double(const mn_num *n);  /* as Python's float() of it */
uint64_t mn_num_low64(const mn_num *n);    /* two's complement low 64 bits of a whole number */
double mn_u64_to_double(uint64_t v);       /* correctly rounded */

/* The number types: sizes, signedness and ranges. */
int mn_type_is_int(mncf_type t);
int mn_type_is_float(mncf_type t);
int mn_type_is_number(mncf_type t);        /* int or float */
int mn_type_signed(mncf_type t);
size_t mn_type_size(mncf_type t);
mncf_type mn_type_parse(const char *s, size_t n);
/* Whether the number can be stored as type t (values.fits). */
int mn_fits(mncf_type t, const mn_num *v);
/* The bytes of the number as type t, in the byte order; out has room for 8. */
void mn_encode_value(mncf_type t, const mn_num *v, int big_endian, uint8_t *out);
/* The number that the bytes hold as type t. */
mn_num mn_decode_value(mncf_type t, const uint8_t *raw, int big_endian);

/* ---- TOML ------------------------------------------------------------------------------------------- */

enum { MN_ORIGIN_NONE = 0, MN_ORIGIN_IMPLICIT, MN_ORIGIN_HEADER, MN_ORIGIN_DOTTED, MN_ORIGIN_INLINE,
       MN_ORIGIN_TABLE_ARRAY };

struct mn_entry {
    mn_str key;
    struct mncf_toml *val;
};

struct mncf_toml {
    uint8_t type;         /* mncf_toml_type */
    uint8_t origin;       /* how a table or array came to be, while parsing */
    union {
        mn_str s;         /* string, date-time */
        int64_t i;
        double f;
        int b;
        struct {
            mncf_toml **items;
            size_t n, cap;
        } a;
        struct {
            struct mn_entry *e;
            size_t n, cap;
            mn_index index;
        } t;
    } u;
};

struct mncf_toml_doc {
    mn_arena arena;
    mncf_toml *root;
};

/* Parses into an arena that the caller owns. */
mncf_toml *mn_toml_parse(mn_arena *ar, const char *text, size_t n, mncf_error *err);
const mncf_toml *mn_toml_get(const mncf_toml *t, const char *key, size_t n);
const mncf_toml *mn_toml_getz(const mncf_toml *t, const char *key);
int mn_toml_is(const mncf_toml *v, mncf_toml_type type);
/* The checks of section 2 after parsing: no date-times, nested at most MNCF_MAX_DEPTH levels. */
int mn_toml_check_depth(const mncf_toml *v, int depth);   /* 0 ok, 1 too deep, 2 a date-time */

/* ---- Cryptography ----------------------------------------------------------------------------------- */

typedef struct mn_sha256 {
    uint32_t h[8];
    uint64_t total;
    uint8_t buf[64];
    size_t fill;
} mn_sha256;

void mn_sha256_init(mn_sha256 *s);
void mn_sha256_update(mn_sha256 *s, const void *p, size_t n);
void mn_sha256_final(mn_sha256 *s, uint8_t out[32]);

typedef struct mn_sha512 {
    uint64_t h[8];
    uint64_t total;
    uint8_t buf[128];
    size_t fill;
} mn_sha512;

void mn_sha512_init(mn_sha512 *s);
void mn_sha512_update(mn_sha512 *s, const void *p, size_t n);
void mn_sha512_final(mn_sha512 *s, uint8_t out[64]);

/* Base64, standard alphabet with padding. Decode returns the byte count, or -1. */
long mn_base64_decode(const char *s, size_t n, uint8_t *out, size_t cap);
size_t mn_base64_encode(const uint8_t *p, size_t n, char *out);

uint32_t mn_crc32(uint32_t crc, const void *p, size_t n);

/* ---- Signatures ------------------------------------------------------------------------------------- */

typedef struct mn_trust_key {
    char *name;
    uint8_t pub[32];
    char id[17];
} mn_trust_key;

struct mncf_trust {
    mn_alloc alloc;
    mn_trust_key *keys;
    size_t key_count, key_cap;
    char (*revoked)[17];
    size_t revoked_count, revoked_cap;
};

/* Checks the signature lines of a file's bytes into sig, with strings in the arena. 0 on no memory. */
int mn_signature_check(const uint8_t *data, size_t n, const mncf_trust *trust, mn_arena *ar,
                       mncf_signature *sig);
size_t mn_signature_split(const uint8_t *data, size_t n);   /* the size of the signed part */

/* ---- Platforms -------------------------------------------------------------------------------------- */

typedef struct mn_platform {
    const char *name;
    int game_id_kind;     /* 0 switch hex16, 1 n64 printable4, 2 pc store:id */
    const char *const *archs;
    size_t arch_count;
    int big_endian;
    size_t pointer_size;
    const char *const *modules;     /* NULL: any name */
    size_t module_count;
    int module_sha256;    /* 0: hex build IDs with prefix matching */
    const char *const *regions;
    size_t region_count;
    int addresses;
    const char *const *buttons;     /* NULL: upper-case names */
    size_t button_count;
} mn_platform;

const mn_platform *mn_platform_find(const char *name, size_t n);
int mn_platform_game_id(const mn_platform *p, const char *s, size_t n);
int mn_platform_module_id(const mn_platform *p, const char *s, size_t n);
int mn_platform_module_match(const mn_platform *p, const char *listed, size_t ln, const char *running,
                             size_t rn);
int mn_platform_button(const mn_platform *p, const char *s, size_t n);
int mn_platform_has_module(const mn_platform *p, const char *s, size_t n);

#endif /* MNCF_INTERNAL_H */
