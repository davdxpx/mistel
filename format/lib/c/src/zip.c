/* zip.c: ZIP archives in memory, read through their central directory as the reference reader's ZIP
 * module reads them, and an inflater for deflated entries (RFC 1951). MIT License.
 *
 * The end record is searched for in the last 64 KiB, data before the archive is allowed, and names are
 * UTF-8 where the entry says so and code page 437 otherwise. Stored and deflated entries that are not
 * encrypted can be read; their CRC-32 is checked when the read reaches the entry's end.
 */
#include "zip.h"

#include <stdio.h>

#define END_SIZE 22
#define END64_SIZE 56
#define LOCATOR_SIZE 20
#define CENTRAL_SIZE 46
#define LOCAL_SIZE 30
#define MAX_COMMENT 0xFFFF
#define MAX_EXTRACT_VERSION 63

/* Code page 437 from 0x80 to 0xFF; the bytes below are ASCII. */
static const uint16_t CP437_HIGH[128] = {
    0x00C7, 0x00FC, 0x00E9, 0x00E2, 0x00E4, 0x00E0, 0x00E5, 0x00E7, 0x00EA, 0x00EB, 0x00E8, 0x00EF, 0x00EE,
    0x00EC, 0x00C4, 0x00C5, 0x00C9, 0x00E6, 0x00C6, 0x00F4, 0x00F6, 0x00F2, 0x00FB, 0x00F9, 0x00FF, 0x00D6,
    0x00DC, 0x00A2, 0x00A3, 0x00A5, 0x20A7, 0x0192, 0x00E1, 0x00ED, 0x00F3, 0x00FA, 0x00F1, 0x00D1, 0x00AA,
    0x00BA, 0x00BF, 0x2310, 0x00AC, 0x00BD, 0x00BC, 0x00A1, 0x00AB, 0x00BB, 0x2591, 0x2592, 0x2593, 0x2502,
    0x2524, 0x2561, 0x2562, 0x2556, 0x2555, 0x2563, 0x2551, 0x2557, 0x255D, 0x255C, 0x255B, 0x2510, 0x2514,
    0x2534, 0x252C, 0x251C, 0x2500, 0x253C, 0x255E, 0x255F, 0x255A, 0x2554, 0x2569, 0x2566, 0x2560, 0x2550,
    0x256C, 0x2567, 0x2568, 0x2564, 0x2565, 0x2559, 0x2558, 0x2552, 0x2553, 0x256B, 0x256A, 0x2518, 0x250C,
    0x2588, 0x2584, 0x258C, 0x2590, 0x2580, 0x03B1, 0x00DF, 0x0393, 0x03C0, 0x03A3, 0x03C3, 0x00B5, 0x03C4,
    0x03A6, 0x0398, 0x03A9, 0x03B4, 0x221E, 0x03C6, 0x03B5, 0x2229, 0x2261, 0x00B1, 0x2265, 0x2264, 0x2320,
    0x2321, 0x00F7, 0x2248, 0x00B0, 0x2219, 0x00B7, 0x221A, 0x207F, 0x00B2, 0x25A0, 0x00A0};

static uint32_t u16(const uint8_t *b)
{
    return (uint32_t)b[0] | (uint32_t)b[1] << 8;
}

static uint32_t u32(const uint8_t *b)
{
    return u16(b) | u16(b + 2) << 16;
}

static uint64_t u64(const uint8_t *b)
{
    return (uint64_t)u32(b) | (uint64_t)u32(b + 4) << 32;
}

static int sig(const uint8_t *b, int third, int fourth)
{
    return b[0] == 'P' && b[1] == 'K' && b[2] == third && b[3] == fourth;
}

static int zip_fail(mncf_error *err, const char *message)
{
    mn_error(err, MNCF_ERR_ZIP, "zip", "%s", message);
    return 0;
}

/* The name of an entry as text: UTF-8 where the flag says so, else code page 437. 0 where the flag says
 * UTF-8 and it is not. */
static int entry_name(mn_arena *ar, const uint8_t *raw, size_t n, uint32_t flags, mn_str *out)
{
    size_t at, i, len = 0;
    char *s;
    if (flags & 0x800) {
        if (!mn_utf8_valid(raw, n, &at)) {
            return 0;
        }
        out->p = mn_arena_strndup(ar, (const char *)raw, n);
        out->n = n;
        return out->p != NULL ? 1 : -1;
    }
    for (i = 0; i < n; i++) {
        uint32_t c = raw[i] < 0x80 ? raw[i] : CP437_HIGH[raw[i] - 0x80];
        len += c < 0x80 ? 1 : c < 0x800 ? 2 : 3;
    }
    s = (char *)mn_arena_alloc(ar, len + 1);
    if (s == NULL) {
        return -1;
    }
    len = 0;
    for (i = 0; i < n; i++) {
        uint32_t c = raw[i] < 0x80 ? raw[i] : CP437_HIGH[raw[i] - 0x80];
        if (c < 0x80) {
            s[len++] = (char)c;
        } else if (c < 0x800) {
            s[len++] = (char)(0xC0 | (c >> 6));
            s[len++] = (char)(0x80 | (c & 0x3F));
        } else {
            s[len++] = (char)(0xE0 | (c >> 12));
            s[len++] = (char)(0x80 | ((c >> 6) & 0x3F));
            s[len++] = (char)(0x80 | (c & 0x3F));
        }
    }
    s[len] = 0;
    out->p = s;
    out->n = len;
    return 1;
}

/* The end record: the central directory's size and offset, the record's position, and whether ZIP64. */
static int end_record(const uint8_t *data, size_t size, uint64_t *size_cd, uint64_t *offset_cd, uint64_t *location,
                      int *zip64, mncf_error *err)
{
    size_t start = (size_t)-1;
    if (size >= END_SIZE && sig(data + size - END_SIZE, 5, 6) && data[size - 2] == 0 && data[size - 1] == 0) {
        start = size - END_SIZE;
    } else if (size >= 4) {
        size_t from = size > (size_t)MAX_COMMENT + END_SIZE ? size - MAX_COMMENT - END_SIZE : 0;
        size_t i = size - 4;
        for (;;) {
            if (sig(data + i, 5, 6)) {
                start = i;
                break;
            }
            if (i == from) {
                break;
            }
            i--;
        }
        if (start != (size_t)-1 && size - start < END_SIZE) {
            start = (size_t)-1;
        }
    }
    if (start == (size_t)-1) {
        return zip_fail(err, "not a ZIP file");
    }
    *size_cd = u32(data + start + 12);
    *offset_cd = u32(data + start + 16);
    *location = start;
    *zip64 = 0;
    /* ZIP64: a locator just before the end record, and the ZIP64 end record before it. */
    if (start >= LOCATOR_SIZE && sig(data + start - LOCATOR_SIZE, 6, 7)) {
        const uint8_t *loc = data + start - LOCATOR_SIZE;
        if (u32(loc + 4) != 0 || u32(loc + 16) > 1) {
            return zip_fail(err, "ZIP files that span several disks are not read");
        }
        if (start >= LOCATOR_SIZE + END64_SIZE && sig(data + start - LOCATOR_SIZE - END64_SIZE, 6, 6)) {
            const uint8_t *end = data + start - LOCATOR_SIZE - END64_SIZE;
            *size_cd = u64(end + 40);
            *offset_cd = u64(end + 48);
            *zip64 = 1;
        }
    }
    return 1;
}

static int decode_extra(mn_zip_entry *e, const uint8_t *extra, size_t n, uint32_t name_crc, mncf_error *err)
{
    size_t at = 0;
    while (n - at >= 4) {
        uint32_t type = u16(extra + at);
        uint32_t len = u16(extra + at + 2);
        const uint8_t *field = extra + at + 4;
        size_t f = 0;
        if (len + 4 > n - at) {
            return zip_fail(err, "a corrupt extra field");
        }
        if (type == 0x0001) {
            if (e->size == 0xFFFFFFFFu) {
                if (len - f < 8) {
                    return zip_fail(err, "a corrupt ZIP64 extra field");
                }
                e->size = u64(field + f);
                f += 8;
            }
            if (e->compressed_size == 0xFFFFFFFFu) {
                if (len - f < 8) {
                    return zip_fail(err, "a corrupt ZIP64 extra field");
                }
                e->compressed_size = u64(field + f);
                f += 8;
            }
            if (e->header_offset == 0xFFFFFFFFu) {
                if (len - f < 8) {
                    return zip_fail(err, "a corrupt ZIP64 extra field");
                }
                e->header_offset = u64(field + f);
                f += 8;
            }
        } else if (type == 0x7075) {
            size_t bad;
            if (len < 5) {
                return zip_fail(err, "a corrupt unicode path extra field");
            }
            if (field[0] == 1 && u32(field + 1) == name_crc && !mn_utf8_valid(field + 5, len - 5, &bad)) {
                return zip_fail(err, "a unicode path extra field that is not UTF-8");
            }
        }
        at += 4 + len;
    }
    return 1;
}

static int cmp_offset(const void *a, const void *b)
{
    const mn_zip_entry *x = *(const mn_zip_entry *const *)a, *y = *(const mn_zip_entry *const *)b;
    return x->header_offset < y->header_offset ? -1 : x->header_offset > y->header_offset;
}

int mn_zip_open(mn_zip *z, mn_arena *ar, const uint8_t *data, size_t size, mncf_error *err)
{
    uint64_t size_cd, offset_cd, location, total = 0, start_dir, concat;
    int zip64;
    const uint8_t *cd;
    size_t cd_len, pos = 0, i;
    mn_zip_entry **sorted;
    uint64_t end_offset;
    memset(z, 0, sizeof *z);
    z->data = data;
    z->size = size;
    if (!end_record(data, size, &size_cd, &offset_cd, &location, &zip64, err)) {
        return 0;
    }
    if (location < size_cd || location - size_cd < offset_cd) {
        return zip_fail(err, "a bad offset for the central directory");
    }
    concat = location - size_cd - offset_cd;
    if (zip64) {
        if (concat < END64_SIZE + LOCATOR_SIZE) {
            return zip_fail(err, "a bad offset for the central directory");
        }
        concat -= END64_SIZE + LOCATOR_SIZE;
    }
    start_dir = offset_cd + concat;
    cd = start_dir < size ? data + start_dir : data + size;
    cd_len = start_dir < size ? (size_t)(size - start_dir) : 0;
    if (size_cd < cd_len) {
        cd_len = (size_t)size_cd;
    }
    while (total < size_cd) {
        mn_zip_entry *e;
        uint32_t extract, flags, name_len, extra_len, comment_len;
        const uint8_t *rec;
        size_t take;
        int r;
        if (cd_len - pos < CENTRAL_SIZE) {
            return zip_fail(err, "a truncated central directory");
        }
        rec = cd + pos;
        if (!sig(rec, 1, 2)) {
            return zip_fail(err, "a bad magic number in the central directory");
        }
        if (MN_GROW(ar, z->entries, z->count, z->cap) == NULL) {
            mn_error_nomem(err);
            return 0;
        }
        e = &z->entries[z->count];
        memset(e, 0, sizeof *e);
        extract = rec[6];
        flags = u16(rec + 8);
        e->flags = flags;
        e->method = u16(rec + 10);
        e->crc = u32(rec + 16);
        e->compressed_size = u32(rec + 20);
        e->size = u32(rec + 24);
        name_len = u16(rec + 28);
        extra_len = u16(rec + 30);
        comment_len = u16(rec + 32);
        e->header_offset = u32(rec + 42);
        pos += CENTRAL_SIZE;
        take = name_len < cd_len - pos ? name_len : cd_len - pos;
        e->raw_name = cd + pos;
        e->raw_name_len = take;
        r = entry_name(ar, cd + pos, take, flags, &e->name);
        if (r < 0) {
            mn_error_nomem(err);
            return 0;
        }
        if (r == 0) {
            return zip_fail(err, "an entry name is not UTF-8");
        }
        pos += take;
        {
            size_t extra_take = extra_len < cd_len - pos ? extra_len : cd_len - pos;
            const uint8_t *extra = cd + pos;
            pos += extra_take;
            pos += comment_len < cd_len - pos ? comment_len : cd_len - pos;
            if (extract > MAX_EXTRACT_VERSION) {
                return zip_fail(err, "a ZIP version that is not read");
            }
            if (!decode_extra(e, extra, extra_take, mn_crc32(0, e->raw_name, e->raw_name_len), err)) {
                return 0;
            }
        }
        e->header_offset += concat;
        z->count++;
        total += (uint64_t)CENTRAL_SIZE + name_len + extra_len + comment_len;
    }
    sorted = (mn_zip_entry **)mn_arena_alloc(ar, (z->count + 1) * sizeof *sorted);
    if (sorted == NULL) {
        mn_error_nomem(err);
        return 0;
    }
    for (i = 0; i < z->count; i++) {
        sorted[i] = &z->entries[i];
    }
    qsort(sorted, z->count, sizeof *sorted, cmp_offset);
    end_offset = start_dir;
    for (i = z->count; i > 0; i--) {
        sorted[i - 1]->end_offset = end_offset;
        end_offset = sorted[i - 1]->header_offset;
    }
    return 1;
}

int mn_zip_readable(const mn_zip_entry *e)
{
    return (e->method == 0 || e->method == 8) && (e->flags & 1) == 0;
}

/* ---- Inflate ---------------------------------------------------------------------------------------- */

typedef struct inflater {
    const uint8_t *in;
    size_t in_len, in_pos;
    uint32_t bits;
    int bit_count;
    uint8_t *out;
    size_t out_len, out_cap, limit;
    mn_alloc *alloc;
    int truncated;            /* the input ended */
} inflater;

enum { INF_DONE = 0, INF_LIMIT, INF_TRUNCATED, INF_BAD, INF_MEMORY };

static int need_bits(inflater *s, int n)
{
    while (s->bit_count < n) {
        if (s->in_pos >= s->in_len) {
            s->truncated = 1;
            return 0;
        }
        s->bits |= (uint32_t)s->in[s->in_pos++] << s->bit_count;
        s->bit_count += 8;
    }
    return 1;
}

static int get_bits(inflater *s, int n, uint32_t *v)
{
    if (n == 0) {
        *v = 0;
        return 1;
    }
    if (!need_bits(s, n)) {
        return 0;
    }
    *v = s->bits & ((1u << n) - 1);
    s->bits >>= n;
    s->bit_count -= n;
    return 1;
}

/* Appends a byte; 0 when the limit is reached or there is no memory. */
static int put_byte(inflater *s, uint8_t b, int *status)
{
    if (s->out_len >= s->limit) {
        *status = INF_LIMIT;
        return 0;
    }
    if (s->out_len == s->out_cap) {
        size_t cap = s->out_cap == 0 ? 4096 : s->out_cap * 2;
        uint8_t *grown;
        if (cap > s->limit && s->limit > s->out_cap) {
            cap = s->limit;
        }
        grown = (uint8_t *)mn_realloc(s->alloc, s->out, s->out_cap, cap);
        if (grown == NULL) {
            *status = INF_MEMORY;
            return 0;
        }
        s->out = grown;
        s->out_cap = cap;
    }
    s->out[s->out_len++] = b;
    return 1;
}

/* A canonical Huffman code: how many codes of each length, and the symbols in code order. */
typedef struct huffman {
    uint16_t count[16];
    uint16_t symbol[320];
} huffman;

/* Builds a code from code lengths. 0 where the lengths are too many for the length of a code; an
 * incomplete code is allowed, as zlib allows one for a single distance. */
static int build(huffman *h, const uint8_t *lengths, int n)
{
    uint16_t offs[16];
    int len, sym, left = 1;
    memset(h->count, 0, sizeof h->count);
    for (sym = 0; sym < n; sym++) {
        h->count[lengths[sym]]++;
    }
    if (h->count[0] == n) {
        return 1;
    }
    for (len = 1; len < 16; len++) {
        left <<= 1;
        left -= h->count[len];
        if (left < 0) {
            return 0;
        }
    }
    offs[1] = 0;
    for (len = 1; len < 15; len++) {
        offs[len + 1] = (uint16_t)(offs[len] + h->count[len]);
    }
    for (sym = 0; sym < n; sym++) {
        if (lengths[sym] != 0) {
            h->symbol[offs[lengths[sym]]++] = (uint16_t)sym;
        }
    }
    return left == 0 ? 1 : 2;
}

/* Decodes one symbol; -1 at the end of the input, -2 for a code that is not in the table. */
static int decode(inflater *s, const huffman *h)
{
    int code = 0, first = 0, index = 0, len;
    for (len = 1; len < 16; len++) {
        uint32_t bit;
        int count;
        if (!get_bits(s, 1, &bit)) {
            return -1;
        }
        code |= (int)bit;
        count = h->count[len];
        if (code - count < first) {
            return h->symbol[index + (code - first)];
        }
        index += count;
        first += count;
        first <<= 1;
        code <<= 1;
    }
    return -2;
}

static const uint16_t LEN_BASE[29] = {3,  4,  5,  6,  7,  8,  9,  10, 11,  13,  15,  17,  19,  23, 27,
                                      31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
static const uint8_t LEN_EXTRA[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
static const uint16_t DIST_BASE[30] = {1,   2,   3,   4,   5,   7,    9,    13,   17,   25,   33,   49,   65,    97,    129,
                                       193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
static const uint8_t DIST_EXTRA[30] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};

static int codes(inflater *s, const huffman *lit, const huffman *dist)
{
    int status = INF_DONE;
    for (;;) {
        int sym = decode(s, lit);
        if (sym == -1) {
            return INF_TRUNCATED;
        }
        if (sym < 0) {
            return INF_BAD;
        }
        if (sym < 256) {
            if (!put_byte(s, (uint8_t)sym, &status)) {
                return status;
            }
        } else if (sym == 256) {
            return INF_DONE;
        } else {
            uint32_t extra;
            size_t len, d, k;
            sym -= 257;
            if (sym >= 29) {
                return INF_BAD;
            }
            if (!get_bits(s, LEN_EXTRA[sym], &extra)) {
                return INF_TRUNCATED;
            }
            len = LEN_BASE[sym] + extra;
            sym = decode(s, dist);
            if (sym == -1) {
                return INF_TRUNCATED;
            }
            if (sym < 0 || sym >= 30) {
                return INF_BAD;
            }
            if (!get_bits(s, DIST_EXTRA[sym], &extra)) {
                return INF_TRUNCATED;
            }
            d = DIST_BASE[sym] + extra;
            if (d > s->out_len) {
                return INF_BAD;
            }
            for (k = 0; k < len; k++) {
                if (!put_byte(s, s->out[s->out_len - d], &status)) {
                    return status;
                }
            }
        }
    }
}

static int stored(inflater *s)
{
    size_t len, nlen, k;
    int status = INF_DONE;
    s->bits = 0;
    s->bit_count = 0;
    if (s->in_len - s->in_pos < 4) {
        s->in_pos = s->in_len;
        return INF_TRUNCATED;
    }
    len = u16(s->in + s->in_pos);
    nlen = u16(s->in + s->in_pos + 2);
    s->in_pos += 4;
    if (len != (~nlen & 0xFFFF)) {
        return INF_BAD;
    }
    for (k = 0; k < len; k++) {
        if (s->in_pos >= s->in_len) {
            return INF_TRUNCATED;
        }
        if (!put_byte(s, s->in[s->in_pos++], &status)) {
            return status;
        }
    }
    return INF_DONE;
}

static int fixed(inflater *s)
{
    uint8_t lengths[320];
    huffman lit, dist;
    int i;
    for (i = 0; i < 144; i++) {
        lengths[i] = 8;
    }
    for (; i < 256; i++) {
        lengths[i] = 9;
    }
    for (; i < 280; i++) {
        lengths[i] = 7;
    }
    for (; i < 288; i++) {
        lengths[i] = 8;
    }
    build(&lit, lengths, 288);
    for (i = 0; i < 30; i++) {
        lengths[i] = 5;
    }
    build(&dist, lengths, 30);
    return codes(s, &lit, &dist);
}

static int dynamic(inflater *s)
{
    static const uint8_t ORDER[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};
    uint8_t lengths[320];
    uint32_t nlen, ndist, ncode, v;
    huffman lencode, lit, dist;
    int index, r;
    if (!get_bits(s, 5, &nlen) || !get_bits(s, 5, &ndist) || !get_bits(s, 4, &ncode)) {
        return INF_TRUNCATED;
    }
    nlen += 257;
    ndist += 1;
    ncode += 4;
    if (nlen > 286 || ndist > 30) {
        return INF_BAD;
    }
    memset(lengths, 0, sizeof lengths);
    for (index = 0; index < (int)ncode; index++) {
        if (!get_bits(s, 3, &v)) {
            return INF_TRUNCATED;
        }
        lengths[ORDER[index]] = (uint8_t)v;
    }
    if (build(&lencode, lengths, 19) != 1) {
        return INF_BAD;
    }
    index = 0;
    while (index < (int)(nlen + ndist)) {
        int sym = decode(s, &lencode);
        int len = 0, repeat;
        if (sym == -1) {
            return INF_TRUNCATED;
        }
        if (sym < 0) {
            return INF_BAD;
        }
        if (sym < 16) {
            lengths[index++] = (uint8_t)sym;
            continue;
        }
        if (sym == 16) {
            if (index == 0) {
                return INF_BAD;
            }
            len = lengths[index - 1];
            if (!get_bits(s, 2, &v)) {
                return INF_TRUNCATED;
            }
            repeat = 3 + (int)v;
        } else if (sym == 17) {
            if (!get_bits(s, 3, &v)) {
                return INF_TRUNCATED;
            }
            repeat = 3 + (int)v;
        } else {
            if (!get_bits(s, 7, &v)) {
                return INF_TRUNCATED;
            }
            repeat = 11 + (int)v;
        }
        if (index + repeat > (int)(nlen + ndist)) {
            return INF_BAD;
        }
        while (repeat-- > 0) {
            lengths[index++] = (uint8_t)len;
        }
    }
    if (lengths[256] == 0) {
        return INF_BAD;
    }
    /* As zlib: an incomplete code is allowed only where it is one code of length 1. */
    r = build(&lit, lengths, (int)nlen);
    if (r == 0 || (r == 2 && !(lit.count[1] == 1 && nlen - lit.count[0] == 1))) {
        return INF_BAD;
    }
    r = build(&dist, lengths + nlen, (int)ndist);
    if (r == 0 || (r == 2 && !(dist.count[1] == 1 && ndist - dist.count[0] == 1))) {
        return INF_BAD;
    }
    return codes(s, &lit, &dist);
}

/* Inflates raw deflate data into at most limit bytes. */
static int inflate_all(const uint8_t *in, size_t in_len, size_t limit, mn_alloc *alloc, uint8_t **out, size_t *out_len)
{
    inflater s;
    int status = INF_DONE;
    uint32_t last = 0, type;
    memset(&s, 0, sizeof s);
    s.in = in;
    s.in_len = in_len;
    s.limit = limit;
    s.alloc = alloc;
    while (!last) {
        if (!get_bits(&s, 1, &last) || !get_bits(&s, 2, &type)) {
            status = INF_TRUNCATED;
            break;
        }
        if (type == 0) {
            status = stored(&s);
        } else if (type == 1) {
            status = fixed(&s);
        } else if (type == 2) {
            status = dynamic(&s);
        } else {
            status = INF_BAD;
        }
        if (status != INF_DONE) {
            break;
        }
    }
    *out = s.out;
    *out_len = s.out_len;
    return status;
}

/* ---- Reading an entry ------------------------------------------------------------------------------- */

int mn_zip_read(const mn_zip *z, const mn_zip_entry *e, size_t limit, mn_alloc *alloc, uint8_t **out, size_t *out_len,
                mncf_error *err)
{
    uint64_t h = e->header_offset, start, size, cap;
    uint32_t local_flags, name_len, extra_len;
    mn_str local_name;
    mn_arena tmp;
    int r;
    *out = NULL;
    *out_len = 0;
    if (!mn_zip_readable(e)) {
        return zip_fail(err, "an entry that is neither stored nor deflated, or is encrypted");
    }
    if (z->size < LOCAL_SIZE || h > z->size - LOCAL_SIZE) {
        return zip_fail(err, "a truncated file header");
    }
    if (!sig(z->data + h, 3, 4)) {
        return zip_fail(err, "a bad magic number for a file header");
    }
    local_flags = u16(z->data + h + 6);
    name_len = u16(z->data + h + 26);
    extra_len = u16(z->data + h + 28);
    if (e->flags & 0x20) {
        return zip_fail(err, "compressed patched data (flag bit 5)");
    }
    if (e->flags & 0x40) {
        return zip_fail(err, "strong encryption (flag bit 6)");
    }
    {
        uint64_t name_at = h + LOCAL_SIZE;
        size_t take = name_at >= z->size ? 0 : (size_t)(z->size - name_at < name_len ? z->size - name_at : name_len);
        mn_arena_init(&tmp, *alloc);
        r = entry_name(&tmp, z->data + name_at, take, local_flags, &local_name);
        if (r <= 0 || !mn_streq(local_name.p, local_name.n, e->name.p, e->name.n)) {
            mn_arena_free(&tmp);
            if (r < 0) {
                mn_error_nomem(err);
                return 0;
            }
            return zip_fail(err, "the file names in the directory and the file header differ");
        }
        mn_arena_free(&tmp);
        start = name_at + take + extra_len;
    }
    /* Sizes of 2^63 and more come only from a ZIP64 field; they are as good as endless. */
    if ((e->compressed_size >> 63) != 0 || start + e->compressed_size > e->end_offset) {
        if (e->end_offset != e->header_offset) {
            return zip_fail(err, "overlapped entries (possibly a ZIP bomb)");
        }
    }
    if ((e->compressed_size >> 63) != 0 || start > z->size || e->compressed_size > z->size - start) {
        return zip_fail(err, "the entry's data is cut off");
    }
    size = (e->size >> 63) != 0 ? ~(uint64_t)0 : e->size;
    cap = size < limit ? size : limit;
    if (e->method == 0) {
        size_t n = (size_t)(e->compressed_size < cap ? e->compressed_size : cap);
        *out = (uint8_t *)mn_malloc(alloc, n == 0 ? 1 : n);
        if (*out == NULL) {
            mn_error_nomem(err);
            return 0;
        }
        if (n > 0) {
            memcpy(*out, z->data + start, n);
        }
        *out_len = n;
    } else {
        int status = inflate_all(z->data + start, (size_t)e->compressed_size, (size_t)cap, alloc, out, out_len);
        if (status == INF_MEMORY) {
            mn_free(alloc, *out, *out_len);
            *out = NULL;
            mn_error_nomem(err);
            return 0;
        }
        if (status == INF_BAD) {
            mn_free(alloc, *out, *out_len);
            *out = NULL;
            *out_len = 0;
            return zip_fail(err, "the entry's deflate data is broken");
        }
        if (*out == NULL) {
            *out = (uint8_t *)mn_malloc(alloc, 1);
            if (*out == NULL) {
                mn_error_nomem(err);
                return 0;
            }
        }
    }
    if (cap == size && mn_crc32(0, *out, *out_len) != e->crc) {
        mn_free(alloc, *out, *out_len);
        *out = NULL;
        *out_len = 0;
        return zip_fail(err, "a bad CRC-32");
    }
    return 1;
}
