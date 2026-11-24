/* crypto.c: SHA-256 and SHA-512 (FIPS 180-4), base64, CRC-32 and Ed25519 verification (RFC 8032), written
 * for this library so that it needs nothing else. MIT License.
 *
 * Ed25519 verification checks [S]B = R + [k]A without the cofactor, refuses S >= L, and refuses encodings
 * of y >= p and points that are not on the curve, as the Python reference does. It handles public data
 * only and is not constant-time; it signs nothing.
 */
#include "internal.h"

/* ---- SHA-256 ---------------------------------------------------------------------------------------- */

static const uint32_t K256[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};

static uint32_t ror32(uint32_t x, int n)
{
    return (x >> n) | (x << (32 - n));
}

static void sha256_block(mn_sha256 *s, const uint8_t *p)
{
    uint32_t w[64];
    uint32_t a, b, c, d, e, f, g, h;
    int i;
    for (i = 0; i < 16; i++) {
        w[i] = (uint32_t)p[4 * i] << 24 | (uint32_t)p[4 * i + 1] << 16 | (uint32_t)p[4 * i + 2] << 8 | p[4 * i + 3];
    }
    for (i = 16; i < 64; i++) {
        uint32_t s0 = ror32(w[i - 15], 7) ^ ror32(w[i - 15], 18) ^ (w[i - 15] >> 3);
        uint32_t s1 = ror32(w[i - 2], 17) ^ ror32(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    a = s->h[0];
    b = s->h[1];
    c = s->h[2];
    d = s->h[3];
    e = s->h[4];
    f = s->h[5];
    g = s->h[6];
    h = s->h[7];
    for (i = 0; i < 64; i++) {
        uint32_t t1 = h + (ror32(e, 6) ^ ror32(e, 11) ^ ror32(e, 25)) + ((e & f) ^ (~e & g)) + K256[i] + w[i];
        uint32_t t2 = (ror32(a, 2) ^ ror32(a, 13) ^ ror32(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
        h = g;
        g = f;
        f = e;
        e = d + t1;
        d = c;
        c = b;
        b = a;
        a = t1 + t2;
    }
    s->h[0] += a;
    s->h[1] += b;
    s->h[2] += c;
    s->h[3] += d;
    s->h[4] += e;
    s->h[5] += f;
    s->h[6] += g;
    s->h[7] += h;
}

void mn_sha256_init(mn_sha256 *s)
{
    static const uint32_t iv[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                                   0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    memcpy(s->h, iv, sizeof iv);
    s->total = 0;
    s->fill = 0;
}

void mn_sha256_update(mn_sha256 *s, const void *data, size_t n)
{
    const uint8_t *p = (const uint8_t *)data;
    s->total += n;
    while (n > 0) {
        size_t take = 64 - s->fill;
        if (take > n) {
            take = n;
        }
        if (s->fill == 0 && n >= 64) {
            sha256_block(s, p);
            p += 64;
            n -= 64;
            continue;
        }
        memcpy(s->buf + s->fill, p, take);
        s->fill += take;
        p += take;
        n -= take;
        if (s->fill == 64) {
            sha256_block(s, s->buf);
            s->fill = 0;
        }
    }
}

void mn_sha256_final(mn_sha256 *s, uint8_t out[32])
{
    uint64_t bits = s->total * 8;
    uint8_t pad[72];
    size_t padn = (s->fill < 56 ? 56 : 120) - s->fill;
    int i;
    memset(pad, 0, sizeof pad);
    pad[0] = 0x80;
    for (i = 0; i < 8; i++) {
        pad[padn + (size_t)i] = (uint8_t)(bits >> (56 - 8 * i));
    }
    {
        uint64_t keep = s->total;
        mn_sha256_update(s, pad, padn + 8);
        s->total = keep;
    }
    for (i = 0; i < 8; i++) {
        out[4 * i] = (uint8_t)(s->h[i] >> 24);
        out[4 * i + 1] = (uint8_t)(s->h[i] >> 16);
        out[4 * i + 2] = (uint8_t)(s->h[i] >> 8);
        out[4 * i + 3] = (uint8_t)s->h[i];
    }
}

void mncf_sha256(const void *data, size_t size, uint8_t out[32])
{
    mn_sha256 s;
    mn_sha256_init(&s);
    mn_sha256_update(&s, data, size);
    mn_sha256_final(&s, out);
}

/* ---- SHA-512 ---------------------------------------------------------------------------------------- */

static const uint64_t K512[80] = {
    0x428a2f98d728ae22ULL, 0x7137449123ef65cdULL, 0xb5c0fbcfec4d3b2fULL, 0xe9b5dba58189dbbcULL,
    0x3956c25bf348b538ULL, 0x59f111f1b605d019ULL, 0x923f82a4af194f9bULL, 0xab1c5ed5da6d8118ULL,
    0xd807aa98a3030242ULL, 0x12835b0145706fbeULL, 0x243185be4ee4b28cULL, 0x550c7dc3d5ffb4e2ULL,
    0x72be5d74f27b896fULL, 0x80deb1fe3b1696b1ULL, 0x9bdc06a725c71235ULL, 0xc19bf174cf692694ULL,
    0xe49b69c19ef14ad2ULL, 0xefbe4786384f25e3ULL, 0x0fc19dc68b8cd5b5ULL, 0x240ca1cc77ac9c65ULL,
    0x2de92c6f592b0275ULL, 0x4a7484aa6ea6e483ULL, 0x5cb0a9dcbd41fbd4ULL, 0x76f988da831153b5ULL,
    0x983e5152ee66dfabULL, 0xa831c66d2db43210ULL, 0xb00327c898fb213fULL, 0xbf597fc7beef0ee4ULL,
    0xc6e00bf33da88fc2ULL, 0xd5a79147930aa725ULL, 0x06ca6351e003826fULL, 0x142929670a0e6e70ULL,
    0x27b70a8546d22ffcULL, 0x2e1b21385c26c926ULL, 0x4d2c6dfc5ac42aedULL, 0x53380d139d95b3dfULL,
    0x650a73548baf63deULL, 0x766a0abb3c77b2a8ULL, 0x81c2c92e47edaee6ULL, 0x92722c851482353bULL,
    0xa2bfe8a14cf10364ULL, 0xa81a664bbc423001ULL, 0xc24b8b70d0f89791ULL, 0xc76c51a30654be30ULL,
    0xd192e819d6ef5218ULL, 0xd69906245565a910ULL, 0xf40e35855771202aULL, 0x106aa07032bbd1b8ULL,
    0x19a4c116b8d2d0c8ULL, 0x1e376c085141ab53ULL, 0x2748774cdf8eeb99ULL, 0x34b0bcb5e19b48a8ULL,
    0x391c0cb3c5c95a63ULL, 0x4ed8aa4ae3418acbULL, 0x5b9cca4f7763e373ULL, 0x682e6ff3d6b2b8a3ULL,
    0x748f82ee5defb2fcULL, 0x78a5636f43172f60ULL, 0x84c87814a1f0ab72ULL, 0x8cc702081a6439ecULL,
    0x90befffa23631e28ULL, 0xa4506cebde82bde9ULL, 0xbef9a3f7b2c67915ULL, 0xc67178f2e372532bULL,
    0xca273eceea26619cULL, 0xd186b8c721c0c207ULL, 0xeada7dd6cde0eb1eULL, 0xf57d4f7fee6ed178ULL,
    0x06f067aa72176fbaULL, 0x0a637dc5a2c898a6ULL, 0x113f9804bef90daeULL, 0x1b710b35131c471bULL,
    0x28db77f523047d84ULL, 0x32caab7b40c72493ULL, 0x3c9ebe0a15c9bebcULL, 0x431d67c49c100d4cULL,
    0x4cc5d4becb3e42b6ULL, 0x597f299cfc657e2aULL, 0x5fcb6fab3ad6faecULL, 0x6c44198c4a475817ULL};

static uint64_t ror64(uint64_t x, int n)
{
    return (x >> n) | (x << (64 - n));
}

static void sha512_block(mn_sha512 *s, const uint8_t *p)
{
    uint64_t w[80];
    uint64_t a, b, c, d, e, f, g, h;
    int i, j;
    for (i = 0; i < 16; i++) {
        uint64_t v = 0;
        for (j = 0; j < 8; j++) {
            v = (v << 8) | p[8 * i + j];
        }
        w[i] = v;
    }
    for (i = 16; i < 80; i++) {
        uint64_t s0 = ror64(w[i - 15], 1) ^ ror64(w[i - 15], 8) ^ (w[i - 15] >> 7);
        uint64_t s1 = ror64(w[i - 2], 19) ^ ror64(w[i - 2], 61) ^ (w[i - 2] >> 6);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    a = s->h[0];
    b = s->h[1];
    c = s->h[2];
    d = s->h[3];
    e = s->h[4];
    f = s->h[5];
    g = s->h[6];
    h = s->h[7];
    for (i = 0; i < 80; i++) {
        uint64_t t1 = h + (ror64(e, 14) ^ ror64(e, 18) ^ ror64(e, 41)) + ((e & f) ^ (~e & g)) + K512[i] + w[i];
        uint64_t t2 = (ror64(a, 28) ^ ror64(a, 34) ^ ror64(a, 39)) + ((a & b) ^ (a & c) ^ (b & c));
        h = g;
        g = f;
        f = e;
        e = d + t1;
        d = c;
        c = b;
        b = a;
        a = t1 + t2;
    }
    s->h[0] += a;
    s->h[1] += b;
    s->h[2] += c;
    s->h[3] += d;
    s->h[4] += e;
    s->h[5] += f;
    s->h[6] += g;
    s->h[7] += h;
}

void mn_sha512_init(mn_sha512 *s)
{
    static const uint64_t iv[8] = {0x6a09e667f3bcc908ULL, 0xbb67ae8584caa73bULL, 0x3c6ef372fe94f82bULL,
                                   0xa54ff53a5f1d36f1ULL, 0x510e527fade682d1ULL, 0x9b05688c2b3e6c1fULL,
                                   0x1f83d9abfb41bd6bULL, 0x5be0cd19137e2179ULL};
    memcpy(s->h, iv, sizeof iv);
    s->total = 0;
    s->fill = 0;
}

void mn_sha512_update(mn_sha512 *s, const void *data, size_t n)
{
    const uint8_t *p = (const uint8_t *)data;
    s->total += n;
    while (n > 0) {
        size_t take = 128 - s->fill;
        if (take > n) {
            take = n;
        }
        if (s->fill == 0 && n >= 128) {
            sha512_block(s, p);
            p += 128;
            n -= 128;
            continue;
        }
        memcpy(s->buf + s->fill, p, take);
        s->fill += take;
        p += take;
        n -= take;
        if (s->fill == 128) {
            sha512_block(s, s->buf);
            s->fill = 0;
        }
    }
}

void mn_sha512_final(mn_sha512 *s, uint8_t out[64])
{
    uint64_t bits = s->total * 8;
    uint8_t pad[144];
    size_t padn = (s->fill < 112 ? 112 : 240) - s->fill;
    uint64_t keep = s->total;
    int i;
    memset(pad, 0, sizeof pad);
    pad[0] = 0x80;
    /* The length is 128 bits; the upper 64 hold the bits above 2^64, here 0 or the top 3 bits of total. */
    for (i = 0; i < 8; i++) {
        pad[padn + (size_t)i] = (uint8_t)(i == 7 ? (s->total >> 61) : 0);
    }
    for (i = 0; i < 8; i++) {
        pad[padn + 8 + (size_t)i] = (uint8_t)(bits >> (56 - 8 * i));
    }
    mn_sha512_update(s, pad, padn + 16);
    s->total = keep;
    for (i = 0; i < 8; i++) {
        int j;
        for (j = 0; j < 8; j++) {
            out[8 * i + j] = (uint8_t)(s->h[i] >> (56 - 8 * j));
        }
    }
}

void mncf_sha512(const void *data, size_t size, uint8_t out[64])
{
    mn_sha512 s;
    mn_sha512_init(&s);
    mn_sha512_update(&s, data, size);
    mn_sha512_final(&s, out);
}

/* ---- Base64 and CRC-32 ------------------------------------------------------------------------------ */

static const char B64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static int b64_value(int c)
{
    if (c >= 'A' && c <= 'Z') {
        return c - 'A';
    }
    if (c >= 'a' && c <= 'z') {
        return c - 'a' + 26;
    }
    if (c >= '0' && c <= '9') {
        return c - '0' + 52;
    }
    if (c == '+') {
        return 62;
    }
    if (c == '/') {
        return 63;
    }
    return -1;
}

size_t mn_base64_encode(const uint8_t *p, size_t n, char *out)
{
    size_t i, o = 0;
    for (i = 0; i + 2 < n; i += 3) {
        uint32_t v = (uint32_t)p[i] << 16 | (uint32_t)p[i + 1] << 8 | p[i + 2];
        out[o++] = B64[v >> 18];
        out[o++] = B64[(v >> 12) & 63];
        out[o++] = B64[(v >> 6) & 63];
        out[o++] = B64[v & 63];
    }
    if (n - i == 1) {
        uint32_t v = (uint32_t)p[i] << 16;
        out[o++] = B64[v >> 18];
        out[o++] = B64[(v >> 12) & 63];
        out[o++] = '=';
        out[o++] = '=';
    } else if (n - i == 2) {
        uint32_t v = (uint32_t)p[i] << 16 | (uint32_t)p[i + 1] << 8;
        out[o++] = B64[v >> 18];
        out[o++] = B64[(v >> 12) & 63];
        out[o++] = B64[(v >> 6) & 63];
        out[o++] = '=';
    }
    out[o] = 0;
    return o;
}

/* Decodes padded base64 of the standard alphabet, strictly: a length that is a multiple of 4, padding
 * only at the end. Bits after the last byte are ignored, as Python's decoder does. */
long mn_base64_decode(const char *s, size_t n, uint8_t *out, size_t cap)
{
    size_t i, o = 0;
    if (n % 4 != 0) {
        return -1;
    }
    for (i = 0; i < n; i += 4) {
        int v[4];
        int k, pad = 0;
        for (k = 0; k < 4; k++) {
            if (s[i + (size_t)k] == '=' && i + 4 == n && k >= 2) {
                v[k] = 0;
                pad++;
            } else {
                if (pad > 0) {
                    return -1;
                }
                v[k] = b64_value((unsigned char)s[i + (size_t)k]);
                if (v[k] < 0) {
                    return -1;
                }
            }
        }
        if (o + (size_t)(3 - pad) > cap) {
            return -1;
        }
        out[o++] = (uint8_t)(v[0] << 2 | v[1] >> 4);
        if (pad < 2) {
            out[o++] = (uint8_t)((v[1] & 15) << 4 | v[2] >> 2);
        }
        if (pad < 1) {
            out[o++] = (uint8_t)((v[2] & 3) << 6 | v[3]);
        }
    }
    return (long)o;
}

uint32_t mn_crc32(uint32_t crc, const void *data, size_t n)
{
    const uint8_t *p = (const uint8_t *)data;
    size_t i;
    crc = ~crc;
    for (i = 0; i < n; i++) {
        int k;
        crc ^= p[i];
        for (k = 0; k < 8; k++) {
            crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
        }
    }
    return ~crc;
}

/* ---- Ed25519: the field GF(2^255 - 19) -------------------------------------------------------------- */

/* A field element as 16 signed limbs of 16 bits. */
typedef int64_t fe[16];

static int64_t floor_div16(int64_t v)
{
    return v >= 0 ? v / 65536 : -((-v + 65535) / 65536);
}

static void fe_carry(fe o)
{
    int i;
    for (i = 0; i < 16; i++) {
        int64_t c = floor_div16(o[i]);
        o[i] -= c * 65536;
        if (i < 15) {
            o[i + 1] += c;
        } else {
            o[0] += 38 * c;          /* 2^256 = 38 (mod p) */
        }
    }
}

static void fe_set(fe o, const fe a)
{
    memcpy(o, a, sizeof(fe));
}

static void fe_small(fe o, int64_t v)
{
    memset(o, 0, sizeof(fe));
    o[0] = v;
}

static void fe_add(fe o, const fe a, const fe b)
{
    int i;
    for (i = 0; i < 16; i++) {
        o[i] = a[i] + b[i];
    }
}

static void fe_sub(fe o, const fe a, const fe b)
{
    int i;
    for (i = 0; i < 16; i++) {
        o[i] = a[i] - b[i];
    }
}

static void fe_mul(fe o, const fe a, const fe b)
{
    int64_t t[31];
    int i, j;
    memset(t, 0, sizeof t);
    for (i = 0; i < 16; i++) {
        for (j = 0; j < 16; j++) {
            t[i + j] += a[i] * b[j];
        }
    }
    for (i = 0; i < 15; i++) {
        t[i] += 38 * t[i + 16];
    }
    for (i = 0; i < 16; i++) {
        o[i] = t[i];
    }
    fe_carry(o);
    fe_carry(o);
}

/* The canonical 32 bytes of a field element: its value below p, little-endian. */
static void fe_pack(uint8_t out[32], const fe a)
{
    fe t, m;
    int i, round;
    fe_set(t, a);
    fe_carry(t);
    fe_carry(t);
    fe_carry(t);
    for (round = 0; round < 2; round++) {
        int borrow = 0;
        for (i = 0; i < 16; i++) {
            int64_t sub = i == 0 ? 0xffed : i == 15 ? 0x7fff : 0xffff;
            m[i] = t[i] - sub - borrow;
            borrow = m[i] < 0;
            if (borrow) {
                m[i] += 65536;
            }
        }
        if (!borrow) {
            fe_set(t, m);         /* t was at least p */
        }
    }
    for (i = 0; i < 16; i++) {
        out[2 * i] = (uint8_t)(t[i] & 0xff);
        out[2 * i + 1] = (uint8_t)(t[i] >> 8);
    }
}

static void fe_unpack(fe o, const uint8_t in[32])
{
    int i;
    for (i = 0; i < 16; i++) {
        o[i] = in[2 * i] + ((int64_t)in[2 * i + 1] << 8);
    }
    o[15] &= 0x7fff;
}

static int fe_equal(const fe a, const fe b)
{
    uint8_t x[32], y[32];
    fe_pack(x, a);
    fe_pack(y, b);
    return memcmp(x, y, 32) == 0;
}

static int fe_parity(const fe a)
{
    uint8_t x[32];
    fe_pack(x, a);
    return x[0] & 1;
}

static int fe_is_zero(const fe a)
{
    static const uint8_t zero[32] = {0};
    uint8_t x[32];
    fe_pack(x, a);
    return memcmp(x, zero, 32) == 0;
}

/* a to the power of a 256-bit exponent given little-endian. */
static void fe_pow(fe o, const fe a, const uint8_t e[32])
{
    fe r;
    int i;
    fe_small(r, 1);
    for (i = 255; i >= 0; i--) {
        fe_mul(r, r, r);
        if ((e[i / 8] >> (i % 8)) & 1) {
            fe_mul(r, r, a);
        }
    }
    fe_set(o, r);
}

/* The exponents p - 2 (inverse), (p - 5) / 8 (square roots) and (p - 1) / 4 (the square root of -1). */
static void exp_bytes(uint8_t e[32], uint8_t low, uint8_t high)
{
    memset(e, 0xff, 32);
    e[0] = low;
    e[31] = high;
}

static void fe_inv(fe o, const fe a)
{
    uint8_t e[32];
    exp_bytes(e, 0xeb, 0x7f);
    fe_pow(o, a, e);
}

typedef struct curve {
    fe d, d2, sqrt_m1;
    fe bx, by;
} curve;

/* ---- Ed25519: points -------------------------------------------------------------------------------- */

/* A point in extended coordinates: x = X/Z, y = Y/Z, xy = T/Z. */
typedef struct point {
    fe x, y, z, t;
} point;

static void point_add(const curve *cv, point *o, const point *p, const point *q)
{
    fe a, b, c, d, e, f, g, h, t;
    fe_sub(a, p->y, p->x);
    fe_sub(t, q->y, q->x);
    fe_mul(a, a, t);
    fe_add(b, p->y, p->x);
    fe_add(t, q->y, q->x);
    fe_mul(b, b, t);
    fe_mul(c, p->t, q->t);
    fe_mul(c, c, cv->d2);
    fe_mul(d, p->z, q->z);
    fe_add(d, d, d);
    fe_sub(e, b, a);
    fe_sub(f, d, c);
    fe_add(g, d, c);
    fe_add(h, b, a);
    fe_mul(o->x, e, f);
    fe_mul(o->y, g, h);
    fe_mul(o->z, f, g);
    fe_mul(o->t, e, h);
}

static void point_mul(const curve *cv, point *o, const point *p, const uint8_t s[32])
{
    point q;
    int i;
    fe_small(q.x, 0);
    fe_small(q.y, 1);
    fe_small(q.z, 1);
    fe_small(q.t, 0);
    for (i = 255; i >= 0; i--) {
        point_add(cv, &q, &q, &q);
        if ((s[i / 8] >> (i % 8)) & 1) {
            point_add(cv, &q, &q, p);
        }
    }
    *o = q;
}

static void point_pack(uint8_t out[32], const point *p)
{
    fe zi, x, y;
    fe_inv(zi, p->z);
    fe_mul(x, p->x, zi);
    fe_mul(y, p->y, zi);
    fe_pack(out, y);
    out[31] ^= (uint8_t)(fe_parity(x) << 7);
}

/* x from y and the sign bit, as the reference's _recover_x: 0 where there is none. */
static int recover_x(const curve *cv, fe x, const fe y, int sign)
{
    fe y2, u, v, x2, check, t;
    uint8_t e[32];
    fe one;
    fe_small(one, 1);
    fe_mul(y2, y, y);
    fe_sub(u, y2, one);
    fe_mul(v, cv->d, y2);
    fe_add(v, v, one);
    fe_inv(t, v);
    fe_mul(x2, u, t);
    if (fe_is_zero(x2)) {
        if (sign) {
            return 0;
        }
        fe_small(x, 0);
        return 1;
    }
    /* x = x2^((p + 3) / 8) = x2 * x2^((p - 5) / 8) */
    exp_bytes(e, 0xfd, 0x0f);
    fe_pow(t, x2, e);
    fe_mul(x, t, x2);
    fe_mul(check, x, x);
    if (!fe_equal(check, x2)) {
        fe_mul(x, x, cv->sqrt_m1);
    }
    fe_mul(check, x, x);
    if (!fe_equal(check, x2)) {
        return 0;
    }
    if (fe_parity(x) != sign) {
        fe zero;
        fe_small(zero, 0);
        fe_sub(x, zero, x);
    }
    return 1;
}

/* Decodes a point; 0 for y >= p or a point that is not on the curve. */
static int point_unpack(const curve *cv, point *o, const uint8_t in[32])
{
    static const uint8_t P[32] = {0xed, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
                                  0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
                                  0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x7f};
    int i;
    int sign = in[31] >> 7;
    uint8_t ybytes[32];
    memcpy(ybytes, in, 32);
    ybytes[31] &= 0x7f;
    for (i = 31; i >= 0; i--) {
        if (ybytes[i] != P[i]) {
            if (ybytes[i] > P[i]) {
                return 0;        /* y >= p */
            }
            break;
        }
    }
    if (i < 0) {
        return 0;                /* y == p */
    }
    fe_unpack(o->y, ybytes);
    if (!recover_x(cv, o->x, o->y, sign)) {
        return 0;
    }
    fe_small(o->z, 1);
    fe_mul(o->t, o->x, o->y);
    return 1;
}

static void curve_init(curve *cv)
{
    fe a, b, zero;
    uint8_t e[32];
    /* d = -121665 / 121666 */
    fe_small(a, 121666);
    fe_inv(b, a);
    fe_small(a, 121665);
    fe_mul(b, b, a);
    fe_small(zero, 0);
    fe_sub(cv->d, zero, b);
    fe_carry(cv->d);
    fe_add(cv->d2, cv->d, cv->d);
    fe_carry(cv->d2);
    /* sqrt(-1) = 2^((p - 1) / 4) */
    fe_small(a, 2);
    exp_bytes(e, 0xfb, 0x1f);
    fe_pow(cv->sqrt_m1, a, e);
    /* the base point: y = 4 / 5, x even */
    fe_small(a, 5);
    fe_inv(b, a);
    fe_small(a, 4);
    fe_mul(cv->by, a, b);
    recover_x(cv, cv->bx, cv->by, 0);
}

/* The group order L, little-endian. */
static const uint8_t GROUP_ORDER[32] = {0xed, 0xd3, 0xf5, 0x5c, 0x1a, 0x63, 0x12, 0x58, 0xd6, 0x9c, 0xf7,
                                  0xa2, 0xde, 0xf9, 0xde, 0x14, 0,    0,    0,    0,    0,    0,
                                  0,    0,    0,    0,    0,    0,    0,    0,    0,    0x10};

/* Compares two 32-byte little-endian numbers. */
static int cmp32(const uint8_t *a, const uint8_t *b)
{
    int i;
    for (i = 31; i >= 0; i--) {
        if (a[i] != b[i]) {
            return a[i] < b[i] ? -1 : 1;
        }
    }
    return 0;
}

/* A 64-byte little-endian number modulo L, bit by bit from the top. */
static void reduce_mod_l(uint8_t out[32], const uint8_t in[64])
{
    uint8_t r[33];
    int bit;
    memset(r, 0, sizeof r);
    for (bit = 511; bit >= 0; bit--) {
        int i, carry = (in[bit / 8] >> (bit % 8)) & 1;
        for (i = 0; i < 33; i++) {
            int v = (r[i] << 1) | carry;
            carry = v >> 8;
            r[i] = (uint8_t)v;
        }
        if (r[32] != 0 || cmp32(r, GROUP_ORDER) >= 0) {
            int borrow = 0;
            for (i = 0; i < 32; i++) {
                int v = r[i] - GROUP_ORDER[i] - borrow;
                borrow = v < 0;
                r[i] = (uint8_t)(v + (borrow ? 256 : 0));
            }
            r[32] = (uint8_t)(r[32] - borrow);
        }
    }
    memcpy(out, r, 32);
}

int mncf_ed25519_verify(const uint8_t public_key[32], const void *message, size_t size,
                        const uint8_t signature[64])
{
    curve cv;
    point a, r, sb, ka, rhs, base;
    uint8_t h[64], k[32], left[32], right[32];
    mn_sha512 hs;
    if (public_key == NULL || signature == NULL || (message == NULL && size > 0)) {
        return 0;
    }
    curve_init(&cv);
    if (!point_unpack(&cv, &a, public_key) || !point_unpack(&cv, &r, signature)) {
        return 0;
    }
    if (cmp32(signature + 32, GROUP_ORDER) >= 0) {
        return 0;
    }
    mn_sha512_init(&hs);
    mn_sha512_update(&hs, signature, 32);
    mn_sha512_update(&hs, public_key, 32);
    if (size > 0) {
        mn_sha512_update(&hs, message, size);
    }
    mn_sha512_final(&hs, h);
    reduce_mod_l(k, h);
    fe_set(base.x, cv.bx);
    fe_set(base.y, cv.by);
    fe_small(base.z, 1);
    fe_mul(base.t, cv.bx, cv.by);
    point_mul(&cv, &sb, &base, signature + 32);
    point_mul(&cv, &ka, &a, k);
    point_add(&cv, &rhs, &r, &ka);
    point_pack(left, &sb);
    point_pack(right, &rhs);
    return memcmp(left, right, 32) == 0;
}

void mncf_key_id(const uint8_t public_key[32], char out[17])
{
    static const char hex[] = "0123456789abcdef";
    uint8_t h[32];
    int i;
    mncf_sha256(public_key, 32, h);
    for (i = 0; i < 8; i++) {
        out[2 * i] = hex[h[i] >> 4];
        out[2 * i + 1] = hex[h[i] & 15];
    }
    out[16] = 0;
}
