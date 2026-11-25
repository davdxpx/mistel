/* unit.c: unit tests of the C library: hashes, Ed25519, base64, CRC-32, inflate and ZIP, the TOML
 * parser, numbers, machine code, trust lists, signatures, packs, the API's errors, updates, and every
 * allocation failing in turn. The conformance runner tests the rest against the suite. MIT License.
 *
 *     mncf_unit [<format/tests folder>]
 */
#include "encode.h"
#include "model.h"
#include "zip.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>

static int checks, failures;

#define CHECK(cond)                                                                                              \
    do {                                                                                                         \
        checks++;                                                                                                \
        if (!(cond)) {                                                                                           \
            failures++;                                                                                          \
            printf("%s:%d: check failed: %s\n", __FILE__, __LINE__, #cond);                                      \
        }                                                                                                        \
    } while (0)

static size_t unhex(const char *s, uint8_t *out, size_t cap)
{
    size_t n = 0;
    while (s[0] != 0 && s[1] != 0 && n < cap) {
        out[n++] = (uint8_t)(mn_hex_value((unsigned char)s[0]) << 4 | mn_hex_value((unsigned char)s[1]));
        s += 2;
    }
    return n;
}

static int hex_is(const uint8_t *p, size_t n, const char *hex)
{
    static const char H[] = "0123456789abcdef";
    size_t i;
    if (strlen(hex) != 2 * n) {
        return 0;
    }
    for (i = 0; i < n; i++) {
        if (hex[2 * i] != H[p[i] >> 4] || hex[2 * i + 1] != H[p[i] & 15]) {
            return 0;
        }
    }
    return 1;
}

/* ---- Hashes, base64, CRC-32 ------------------------------------------------------------------------- */

static void test_sha256(void)
{
    uint8_t h[32];
    mn_sha256 s;
    size_t i;
    static char million[1000];
    mncf_sha256("", 0, h);
    CHECK(hex_is(h, 32, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"));
    mncf_sha256("abc", 3, h);
    CHECK(hex_is(h, 32, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"));
    mncf_sha256("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq", 56, h);
    CHECK(hex_is(h, 32, "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1"));
    memset(million, 'a', sizeof million);
    mn_sha256_init(&s);
    for (i = 0; i < 1000; i++) {
        mn_sha256_update(&s, million, i % 2 == 0 ? 1000 : 0);
        mn_sha256_update(&s, million, i % 2 == 0 ? 0 : 1000);
    }
    mn_sha256_final(&s, h);
    CHECK(hex_is(h, 32, "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0"));
}

static void test_sha512(void)
{
    uint8_t h[64];
    mn_sha512 s;
    const char *msg = "abcdefghbcdefghicdefghijdefghijkefghijklfghijklmghijklmnhijklmnoijklmnopjklmnopqklmnopqrlmnopqrsmnopqrstnopqrstu";
    size_t i;
    mncf_sha512("", 0, h);
    CHECK(hex_is(h, 64, "cf83e1357eefb8bdf1542850d66d8007d620e4050b5715dc83f4a921d36ce9ce47d0d13c5d85f2b0ff8318d2877eec2f63b931bd47417a81a538327af927da3e"));
    mncf_sha512("abc", 3, h);
    CHECK(hex_is(h, 64, "ddaf35a193617abacc417349ae20413112e6fa4e89a97ea20a9eeee64b55d39a2192992a274fc1a836ba3c23a3feebbd454d4423643ce80e2a9ac94fa54ca49f"));
    mn_sha512_init(&s);
    for (i = 0; i < strlen(msg); i++) {
        mn_sha512_update(&s, msg + i, 1);
    }
    mn_sha512_final(&s, h);
    CHECK(hex_is(h, 64, "8e959b75dae313da8cf4f72814fc143f8f7779c6eb9f7fa17299aeadb6889018501d289e4900f7e4331b99dec4b5433ac7d329eeb6dd26545e96e55b874be909"));
}

static void test_base64_crc(void)
{
    uint8_t raw[64];
    char text[96];
    CHECK(mn_base64_encode((const uint8_t *)"foob", 4, text) == 8 && strcmp(text, "Zm9vYg==") == 0);
    CHECK(mn_base64_encode((const uint8_t *)"fooba", 5, text) == 8 && strcmp(text, "Zm9vYmE=") == 0);
    CHECK(mn_base64_encode((const uint8_t *)"foobar", 6, text) == 8 && strcmp(text, "Zm9vYmFy") == 0);
    CHECK(mn_base64_decode("Zm9vYmFy", 8, raw, sizeof raw) == 6 && memcmp(raw, "foobar", 6) == 0);
    CHECK(mn_base64_decode("Zm9vYg==", 8, raw, sizeof raw) == 4 && memcmp(raw, "foob", 4) == 0);
    CHECK(mn_base64_decode("Zm9vYg=", 7, raw, sizeof raw) == -1);
    CHECK(mn_base64_decode("Zm=vYg==", 8, raw, sizeof raw) == -1);
    CHECK(mn_base64_decode("Zm9v*g==", 8, raw, sizeof raw) == -1);
    CHECK(mn_crc32(0, "123456789", 9) == 0xCBF43926u);
    CHECK(mn_crc32(mn_crc32(0, "1234", 4), "56789", 5) == 0xCBF43926u);
}

/* ---- Ed25519 ---------------------------------------------------------------------------------------- */

static const char *const RFC8032[][3] = {
    {"d75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a", "",
     "e5564300c360ac729086e2cc806e828a84877f1eb8e5d974d873e065224901555fb8821590a33bacc61e39701cf9b46bd25bf5f0595bbe24655141438e7a100b"},
    {"3d4017c3e843895a92b70aa74d1b7ebc9c982ccf2ec4968cc0cd55f12af4660c", "72",
     "92a009a9f0d4cab8720e820b5f642540a2b27b5416503f8fb3762223ebdb69da085ac1e43e15996e458f3613d0f11d8c387b2eaeb4302aeeb00d291612bb0c00"},
    {"fc51cd8e6218a1a38da47ed00230f0580816ed13ba3303ac5deb911548908025", "af82",
     "6291d657deec24024827e69c3abe01a30ce548a284743a445e3680d7db5ac3ac18ff9b538d16f290ae67f760984dc6594a7c15e9716ed28dc027beceea1ec40a"},
    {"ec172b93ad5e563bf4932c70e1245034c35467ef2efd4d64ebf819683467e2bf",
     "ddaf35a193617abacc417349ae20413112e6fa4e89a97ea20a9eeee64b55d39a2192992a274fc1a836ba3c23a3feebbd454d4423643ce80e2a9ac94fa54ca49f",
     "dc2a4459e7369633a52b1bf277839a00201009a3efbf3ecb69bea2186c26b58909351fc9ac90b3ecfdfbc7c66431e0303dca179c138ac17ad9bef1177331a704"},
};

static void test_ed25519(void)
{
    uint8_t pub[32], msg[64], sig[64], bad[64], key[32];
    size_t i, n;
    char kid[17];
    for (i = 0; i < 4; i++) {
        unhex(RFC8032[i][0], pub, 32);
        n = unhex(RFC8032[i][1], msg, sizeof msg);
        unhex(RFC8032[i][2], sig, 64);
        CHECK(mncf_ed25519_verify(pub, msg, n, sig));
    }
    unhex(RFC8032[1][0], pub, 32);
    n = unhex(RFC8032[1][1], msg, sizeof msg);
    unhex(RFC8032[1][2], sig, 64);
    msg[n] = 0;
    CHECK(!mncf_ed25519_verify(pub, msg, n + 1, sig));       /* a longer message */
    memcpy(bad, sig, 64);
    bad[63] ^= 1;
    CHECK(!mncf_ed25519_verify(pub, msg, n, bad));           /* a changed S */
    memcpy(bad, sig, 64);
    bad[5] ^= 0x10;
    CHECK(!mncf_ed25519_verify(pub, msg, n, bad));           /* a changed R */
    /* S + L: [S + L]B = [S]B, but S must be below L */
    unhex(RFC8032[0][0], pub, 32);
    unhex(RFC8032[0][2], sig, 64);
    {
        static const uint8_t L[32] = {0xed, 0xd3, 0xf5, 0x5c, 0x1a, 0x63, 0x12, 0x58, 0xd6, 0x9c, 0xf7,
                                      0xa2, 0xde, 0xf9, 0xde, 0x14, 0,    0,    0,    0,    0,    0,
                                      0,    0,    0,    0,    0,    0,    0,    0,    0,    0x10};
        int carry = 0;
        memcpy(bad, sig, 64);
        for (i = 0; i < 32; i++) {
            int v = bad[32 + i] + L[i] + carry;
            bad[32 + i] = (uint8_t)v;
            carry = v >> 8;
        }
        CHECK(!mncf_ed25519_verify(pub, NULL, 0, bad));
    }
    /* y = p + 1 encodes y = 1, a point on the curve, but not canonically */
    memset(key, 0xff, 32);
    key[0] = 0xee;
    key[31] = 0x7f;
    CHECK(!mncf_ed25519_verify(key, NULL, 0, sig));
    memcpy(bad, key, 32);
    memcpy(bad + 32, sig + 32, 32);
    CHECK(!mncf_ed25519_verify(pub, NULL, 0, bad));
    /* y = 2 is not on the curve; x = 0 with the sign bit set is no encoding */
    memset(key, 0, 32);
    key[0] = 2;
    CHECK(!mncf_ed25519_verify(key, NULL, 0, sig));
    memset(key, 0, 32);
    key[0] = 1;
    key[31] = 0x80;
    CHECK(!mncf_ed25519_verify(key, NULL, 0, sig));
    CHECK(mncf_ed25519_verify(pub, NULL, 0, sig));
    mncf_key_id(pub, kid);
    CHECK(strcmp(kid, "21fe31dfa154a261") == 0);
}

/* ---- Inflate and ZIP -------------------------------------------------------------------------------- */

static const char DEFLATED_PACK[] =
    "504b03041400000008009405785d0a4524607300000088000000090000007061636b2e746f6d6c15ca310ec2300c40d1dda7"
    "a8b2034d489c78e00eec550727b15504692be8fd45b3fce1e9ebf66d7c0c8fc1b4b5e865e7f2be59032b37e9f83400d3a4af"
    "8fcc33ec7c2c1df9da5f03bf855dc02e91ee59c90ac53c4a450a78566ccc4949903072e4e47c2dc98dd54b4867ad86488ed0"
    "574d06fe504b03041400000008009405785d34e392e398000000d600000006000000612e6d6e6366458e4d0ac2400c46f739"
    "4598b5e85471e942c5a527285d0c3553073a3fb453db22dedda416cc22f0f2bd84d8d87993f184ca87daee0a05c17812be4d"
    "c6a7961440d9f0a882d49a6c5997b01f5dae9f0adc4348177aadf3e5ca5d76cafa492657d5aaf81868fe1fbfff101057713b"
    "762e13eb88cb3b6feca8713188cc79521b8cd6f624999e0a8d1f36f39c966bc361af185fa61d848ff005504b010214001400"
    "000008009405785d0a45246073000000880000000900000000000000000000008001000000007061636b2e746f6d6c504b01"
    "0214001400000008009405785d34e392e398000000d600000006000000000000000000000080019a000000612e6d6e636650"
    "4b050600000000020002006b000000560100000000";

/* 400 lines of text, deflated with dynamic Huffman codes by zlib at level 9. */
static const char DYNAMIC[] =
    "edd54d3643411485d1be51d410d42d04b3f1f3109ebc08118cde8a1158a7bddbb54eabbe75f7bcde4cedf4ba7d3c4ded6dbf"
    "be7b69b7bbe5b0690fcb577bdebf6edfdbf239edfe9ee79b9fef76bf3c9eccc74d0f36156c46b0390b36e7c1e622d8ac82cd"
    "65b0b94afe340a2129a12729f4a4859ec4d0931a7a92434f7ae849103d29a292222aba0d4911951451491195145149119514"
    "51491195143192224652c488b8488a1849112329622445e019cf78c6339ef18c673ce319cf78c6339ef18c673ce319cf78c6"
    "339ef18c673ce319cf78c6339ef18c673ce319cf78c6339ef18c673ce319cf78c6339ef18c673ce319cf78c6339ef18c673c"
    "e319cf78c6339ef18c673ce319cf78c6339ef18c673ce319cfff1ffd02";

static const char FILE_TEXT[] = "format = \"mncf/1\"\nname = \"Example\"\n\n[game]\nplatform = \"switch\"\n"
                                "id = \"0100000000ABC000\"\n\n[[cheat]]\nid = \"money\"\nname = \"Money\"\n\n"
                                "  [[cheat.write]]\n  at = { region = \"heap\", offset = 0x10 }\n  type = \"u32\"\n"
                                "  value = 5\n";
static const char FILE_SIGNATURE[] =
    "# mncf-signature: ed25519 21fe31dfa154a261 "
    "TxX03pZH0h9+vg9B1Dks1hU9In9Nt9W79P0QPchoHij77i8jbmIbbZA7gXUAlMxB1xGh+b4LY9PjND4ynTBuDw==\n";
static const char FILE_SHA256[] = "793bf91e97b0ed6956ed6e17b8f9e6967a7a824dc820d4e580d41f5792964df8";

/* An entry of a test ZIP file. */
typedef struct test_entry {
    const char *name;
    const uint8_t *data;
    size_t size;
    uint32_t method;
    uint32_t flags;
    uint32_t extract;
    uint64_t claimed_size;            /* 0: the real size */
} test_entry;

static void w16(uint8_t *z, size_t *at, uint32_t v)
{
    z[(*at)++] = (uint8_t)(v & 0xFF);
    z[(*at)++] = (uint8_t)((v >> 8) & 0xFF);
}

static void w32(uint8_t *z, size_t *at, uint32_t v)
{
    w16(z, at, v & 0xFFFF);
    w16(z, at, v >> 16);
}

/* A ZIP file of stored or given entries, with junk bytes before it and a comment after it. */
static uint8_t *make_zip(const test_entry *e, size_t count, size_t junk, size_t *len)
{
    size_t total = junk + 22 + 8, i, at = junk, cd;
    uint8_t *z;
    uint32_t offsets[16];
    for (i = 0; i < count; i++) {
        total += 30 + 46 + 2 * strlen(e[i].name) + e[i].size;
    }
    z = (uint8_t *)calloc(1, total);
    memset(z, 'J', junk);
    for (i = 0; i < count; i++) {
        size_t n = strlen(e[i].name);
        offsets[i] = (uint32_t)(at - junk);
        w32(z, &at, 0x04034b50u);
        w16(z, &at, 20);
        w16(z, &at, e[i].flags);
        w16(z, &at, e[i].method);
        w32(z, &at, 0);
        w32(z, &at, 0);
        w32(z, &at, (uint32_t)e[i].size);
        w32(z, &at, (uint32_t)e[i].size);
        w16(z, &at, (uint32_t)n);
        w16(z, &at, 0);
        memcpy(z + at, e[i].name, n);
        at += n;
        if (e[i].size > 0) {
            memcpy(z + at, e[i].data, e[i].size);
        }
        at += e[i].size;
    }
    cd = at;
    for (i = 0; i < count; i++) {
        size_t n = strlen(e[i].name);
        w32(z, &at, 0x02014b50u);
        w16(z, &at, 20);
        w16(z, &at, e[i].extract != 0 ? e[i].extract : 20);
        w16(z, &at, e[i].flags);
        w16(z, &at, e[i].method);
        w32(z, &at, 0);
        w32(z, &at, e[i].method == 0 ? mn_crc32(0, e[i].data, e[i].size) : 0);
        w32(z, &at, (uint32_t)e[i].size);
        w32(z, &at, (uint32_t)(e[i].claimed_size != 0 ? e[i].claimed_size : e[i].size));
        w16(z, &at, (uint32_t)n);
        w16(z, &at, 0);
        w16(z, &at, 0);
        w16(z, &at, 0);
        w16(z, &at, 0);
        w32(z, &at, 0);
        w32(z, &at, offsets[i]);
        memcpy(z + at, e[i].name, n);
        at += n;
    }
    w32(z, &at, 0x06054b50u);
    w16(z, &at, 0);
    w16(z, &at, 0);
    w16(z, &at, (uint32_t)count);
    w16(z, &at, (uint32_t)count);
    w32(z, &at, (uint32_t)(at - 12 - cd));
    w32(z, &at, (uint32_t)(cd - junk));
    w16(z, &at, 8);
    memcpy(z + at, "comment!", 8);
    at += 8;
    *len = at;
    return z;
}

static void test_inflate(void)
{
    mn_arena ar;
    mn_zip z;
    mncf_error err;
    mn_alloc a = mn_alloc_of(NULL);
    uint8_t stream[600];
    size_t n = unhex(DYNAMIC, stream, sizeof stream);
    test_entry e[1];
    uint8_t *zip, *out = NULL;
    size_t len, out_len = 0, i;
    char *want = (char *)malloc(30000), *w = want;
    for (i = 0; i < 400; i++) {
        w += sprintf(w, "line %d: the quick brown fox jumps over the lazy dog\n", (int)(i % 37));
    }
    e[0].name = "t";
    e[0].data = stream;
    e[0].size = n;
    e[0].method = 8;
    e[0].flags = 0;
    e[0].extract = 0;
    e[0].claimed_size = (uint64_t)(w - want);
    zip = make_zip(e, 1, 0, &len);
    mn_arena_init(&ar, a);
    CHECK(mn_zip_open(&z, &ar, zip, len, &err) && z.count == 1);
    /* the CRC-32 in the directory is 0, so a full read fails, and a read short of the end does not check */
    CHECK(!mn_zip_read(&z, &z.entries[0], 1u << 20, &a, &out, &out_len, &err) && err.status == MNCF_ERR_ZIP);
    CHECK(mn_zip_read(&z, &z.entries[0], 1000, &a, &out, &out_len, &err) && out_len == 1000 &&
          memcmp(out, want, 1000) == 0);
    mn_free(&a, out, out_len);
    z.entries[0].crc = mn_crc32(0, want, (size_t)(w - want));
    CHECK(mn_zip_read(&z, &z.entries[0], 1u << 20, &a, &out, &out_len, &err) && out_len == (size_t)(w - want) &&
          memcmp(out, want, out_len) == 0);
    mn_free(&a, out, out_len);
    /* a broken stream: block type 3 (the entry's data starts after the 30 bytes of the header and its name) */
    zip[31] = 0x07;
    z.entries[0].crc = 0;
    CHECK(!mn_zip_read(&z, &z.entries[0], 1u << 20, &a, &out, &out_len, &err) && err.status == MNCF_ERR_ZIP);
    mn_arena_free(&ar);
    free(zip);
    free(want);
}

static void test_zip(void)
{
    mn_arena ar;
    mn_zip z;
    mncf_error err;
    mn_alloc a = mn_alloc_of(NULL);
    uint8_t raw[600], *zip, *out = NULL;
    size_t n = unhex(DEFLATED_PACK, raw, sizeof raw), len, out_len = 0;
    test_entry e[3];
    mn_arena_init(&ar, a);
    /* Python's zipfile wrote this one, deflated */
    CHECK(mn_zip_open(&z, &ar, raw, n, &err) && z.count == 2);
    CHECK(z.count == 2 && mn_streqz(z.entries[1].name.p, z.entries[1].name.n, "a.mncf"));
    CHECK(z.count == 2 && mn_zip_read(&z, &z.entries[1], 1u << 20, &a, &out, &out_len, &err) &&
          out_len == strlen(FILE_TEXT) && memcmp(out, FILE_TEXT, out_len) == 0);
    mn_free(&a, out, out_len);
    mn_arena_free(&ar);
    /* data before the archive and a comment after it */
    memset(e, 0, sizeof e);
    e[0].name = "a";
    e[0].data = (const uint8_t *)"hello";
    e[0].size = 5;
    zip = make_zip(e, 1, 100, &len);
    mn_arena_init(&ar, a);
    CHECK(mn_zip_open(&z, &ar, zip, len, &err) && z.count == 1 && z.entries[0].header_offset == 100);
    CHECK(mn_zip_read(&z, &z.entries[0], 100, &a, &out, &out_len, &err) && out_len == 5);
    mn_free(&a, out, out_len);
    mn_arena_free(&ar);
    free(zip);
    /* not a ZIP file, or one cut off */
    mn_arena_init(&ar, a);
    CHECK(!mn_zip_open(&z, &ar, (const uint8_t *)"PK\x05\x06", 4, &err) && err.status == MNCF_ERR_ZIP);
    CHECK(!mn_zip_open(&z, &ar, (const uint8_t *)"", 0, &err) && err.status == MNCF_ERR_ZIP);
    mn_arena_free(&ar);
    /* a version this reader does not read, encryption, another method */
    e[0].extract = 64;
    zip = make_zip(e, 1, 0, &len);
    mn_arena_init(&ar, a);
    CHECK(!mn_zip_open(&z, &ar, zip, len, &err) && err.status == MNCF_ERR_ZIP);
    mn_arena_free(&ar);
    free(zip);
    e[0].extract = 0;
    e[0].flags = 1;
    zip = make_zip(e, 1, 0, &len);
    mn_arena_init(&ar, a);
    CHECK(mn_zip_open(&z, &ar, zip, len, &err) && !mn_zip_readable(&z.entries[0]));
    CHECK(!mn_zip_read(&z, &z.entries[0], 100, &a, &out, &out_len, &err));
    mn_arena_free(&ar);
    free(zip);
    e[0].flags = 0;
    e[0].method = 12;
    zip = make_zip(e, 1, 0, &len);
    mn_arena_init(&ar, a);
    CHECK(mn_zip_open(&z, &ar, zip, len, &err) && !mn_zip_readable(&z.entries[0]));
    mn_arena_free(&ar);
    free(zip);
    /* a bad CRC-32, and a name in the local header that differs */
    e[0].method = 0;
    zip = make_zip(e, 1, 0, &len);
    zip[30 + 1] ^= 1;               /* the data: "hfllo" */
    mn_arena_init(&ar, a);
    CHECK(mn_zip_open(&z, &ar, zip, len, &err) && !mn_zip_read(&z, &z.entries[0], 100, &a, &out, &out_len, &err));
    mn_arena_free(&ar);
    zip[30 + 1] ^= 1;
    zip[30] = 'b';
    mn_arena_init(&ar, a);
    CHECK(mn_zip_open(&z, &ar, zip, len, &err) && !mn_zip_read(&z, &z.entries[0], 100, &a, &out, &out_len, &err));
    mn_arena_free(&ar);
    free(zip);
    /* a UTF-8 name that is not UTF-8 */
    e[0].name = "\xff";
    e[0].flags = 0x800;
    zip = make_zip(e, 1, 0, &len);
    mn_arena_init(&ar, a);
    CHECK(!mn_zip_open(&z, &ar, zip, len, &err) && err.status == MNCF_ERR_ZIP);
    mn_arena_free(&ar);
    free(zip);
    /* a code page 437 name */
    e[0].flags = 0;
    e[0].name = "\x81";
    zip = make_zip(e, 1, 0, &len);
    mn_arena_init(&ar, a);
    CHECK(mn_zip_open(&z, &ar, zip, len, &err) && mn_streqz(z.entries[0].name.p, z.entries[0].name.n, "\xc3\xbc"));
    mn_arena_free(&ar);
    free(zip);
}

/* ---- Packs ------------------------------------------------------------------------------------------ */

static void test_packs(void)
{
    mncf_error err;
    mncf_pack *p;
    const mncf_pack_info *info;
    uint8_t raw[600], *zip;
    size_t n = unhex(DEFLATED_PACK, raw, sizeof raw), len;
    char manifest[400];
    test_entry e[3];
    p = mncf_pack_read(raw, n, NULL, NULL, &err);
    CHECK(p != NULL);
    if (p != NULL) {
        info = mncf_pack_info_of(p);
        CHECK(strcmp(info->name, "P") == 0 && info->file_count == 1 && info->files[0].status == MNCF_PACK_OK &&
              info->files[0].size == strlen(FILE_TEXT) && info->usable);
        mncf_pack_free(p);
    }
    snprintf(manifest, sizeof manifest, "format = \"mncf-pack/1\"\nname = \"P\"\n\n[[file]]\npath = \"a.mncf\"\nsha256 = \"%s\"\n",
             FILE_SHA256);
    memset(e, 0, sizeof e);
    e[0].name = "pack.toml";
    e[0].data = (const uint8_t *)manifest;
    e[0].size = strlen(manifest);
    e[1].name = "a.mncf";
    e[1].data = (const uint8_t *)FILE_TEXT;
    e[1].size = strlen(FILE_TEXT);
    e[2].name = "dir/";
    zip = make_zip(e, 3, 0, &len);
    p = mncf_pack_read(zip, len, NULL, NULL, &err);
    CHECK(p != NULL && mncf_pack_info_of(p)->files[0].status == MNCF_PACK_OK);
    mncf_pack_free(p);
    /* the bytes a reader reads from one pack are limited */
    p = mn_pack_read(zip, len, NULL, NULL, e[0].size + e[1].size - 1, &err);
    CHECK(p == NULL && err.status == MNCF_ERR_SIZE && strcmp(err.code, "size") == 0);
    p = mn_pack_read(zip, len, NULL, NULL, e[0].size + e[1].size, &err);
    CHECK(p != NULL);
    mncf_pack_free(p);
    free(zip);
    /* an entry that says it has more than 16 MiB */
    e[1].claimed_size = (uint64_t)MNCF_MAX_FILE + 1;
    zip = make_zip(e, 2, 0, &len);
    p = mncf_pack_read(zip, len, NULL, NULL, &err);
    CHECK(p == NULL && err.status == MNCF_ERR_SIZE);
    free(zip);
    e[1].claimed_size = 0;
    /* two entries of the same name */
    e[1].name = "pack.toml";
    zip = make_zip(e, 2, 0, &len);
    p = mncf_pack_read(zip, len, NULL, NULL, &err);
    CHECK(p == NULL && strcmp(err.code, "zip") == 0);
    free(zip);
    /* a manifest with CR line ends */
    e[0].data = (const uint8_t *)"format = \"mncf-pack/1\"\r\nname = \"P\"\r\n";
    e[0].size = strlen((const char *)e[0].data);
    zip = make_zip(e, 1, 0, &len);
    p = mncf_pack_read(zip, len, NULL, NULL, &err);
    CHECK(p == NULL && strcmp(err.code, "manifest") == 0);
    free(zip);
    CHECK(mncf_pack_read(NULL, 4, NULL, NULL, &err) == NULL && err.status == MNCF_ERR_ARGUMENT);
}

/* ---- TOML ------------------------------------------------------------------------------------------- */

static int toml_ok(const char *text)
{
    mncf_error err;
    mncf_toml_doc *d = mncf_toml_parse(text, strlen(text), NULL, &err);
    mncf_toml_free(d);
    return d != NULL;
}

static void test_toml(void)
{
    mncf_error err;
    mncf_toml_doc *d;
    const mncf_toml *r, *v;
    size_t n;
    const char *s;
    const char *doc = "a.b.c = 1\n\"quoted key\" = 'lit'\n[t]\nx = { y = [1, 2.5, \"s\"], z = true }\n"
                      "[[arr]]\nk = 0x1F\n[[arr]]\nk = 0o17\nm = \"\"\"\nline\\\n   joined \\u00e9\\U0001F600\"\"\"\n"
                      "big = 9_223_372_036_854_775_807\nneg = -9223372036854775808\nb = 0b101\n"
                      "f = [inf, -inf, nan, 1e3, -0.0]\nd = 1979-05-27T07:32:00Z\n";
    d = mncf_toml_parse(doc, strlen(doc), NULL, &err);
    CHECK(d != NULL);
    if (d == NULL) {
        printf("  %s\n", err.message);
        return;
    }
    r = mncf_toml_root(d);
    CHECK(mncf_toml_count(r) == 4);
    v = mncf_toml_get(mncf_toml_get(mncf_toml_get(r, "a"), "b"), "c");
    CHECK(mncf_toml_type_of(v) == MNCF_TOML_INTEGER && mncf_toml_integer(v) == 1);
    s = mncf_toml_string(mncf_toml_get(r, "quoted key"), &n);
    CHECK(s != NULL && n == 3 && memcmp(s, "lit", 3) == 0);
    v = mncf_toml_get(mncf_toml_get(mncf_toml_get(r, "t"), "x"), "y");
    CHECK(mncf_toml_count(v) == 3 && mncf_toml_float(mncf_toml_at(v, 1)) == 2.5);
    v = mncf_toml_get(r, "arr");
    CHECK(mncf_toml_count(v) == 2 && mncf_toml_integer(mncf_toml_get(mncf_toml_at(v, 0), "k")) == 31 &&
          mncf_toml_integer(mncf_toml_get(mncf_toml_at(v, 1), "k")) == 15);
    v = mncf_toml_at(v, 1);
    s = mncf_toml_string(mncf_toml_get(v, "m"), &n);
    CHECK(s != NULL && n == 4 + 6 + 2 + 4 + 1 && memcmp(s, "linejoined \xc3\xa9\xf0\x9f\x98\x80", n) == 0);
    CHECK(mncf_toml_integer(mncf_toml_get(v, "big")) == INT64_MAX);
    CHECK(mncf_toml_integer(mncf_toml_get(v, "neg")) == INT64_MIN);
    CHECK(mncf_toml_integer(mncf_toml_get(v, "b")) == 5);
    {
        const mncf_toml *f = mncf_toml_get(v, "f");
        CHECK(isinf(mncf_toml_float(mncf_toml_at(f, 0))) && mncf_toml_float(mncf_toml_at(f, 0)) > 0);
        CHECK(isinf(mncf_toml_float(mncf_toml_at(f, 1))) && mncf_toml_float(mncf_toml_at(f, 1)) < 0);
        CHECK(isnan(mncf_toml_float(mncf_toml_at(f, 2))));
        CHECK(mncf_toml_float(mncf_toml_at(f, 3)) == 1000.0);
        CHECK(signbit(mncf_toml_float(mncf_toml_at(f, 4))));
    }
    CHECK(mncf_toml_type_of(mncf_toml_get(v, "d")) == MNCF_TOML_DATETIME);
    mncf_toml_free(d);

    CHECK(toml_ok(""));
    CHECK(toml_ok("# only a comment"));
    CHECK(toml_ok("a = 1\r\nb = 2\r\n"));
    CHECK(toml_ok("[a.b]\n[a]\nc = 1"));
    CHECK(toml_ok("[a]\nb.c = 1\n[a.b.d]"));
    CHECK(toml_ok("a = [\n 1, # one\n 2,\n]"));
    CHECK(toml_ok("t = 07:32:00\nd = 1979-05-27"));
    CHECK(!toml_ok("a = 1\na = 2"));
    CHECK(!toml_ok("[a]\n[a]"));
    CHECK(!toml_ok("a = {b = 1}\na.c = 2"));
    CHECK(!toml_ok("[a]\nb.c = 1\n[a.b]"));
    CHECK(!toml_ok("a = [1]\n[[a]]"));
    CHECK(!toml_ok("a = 9223372036854775808"));
    CHECK(!toml_ok("a = 0x8000000000000000"));
    CHECK(!toml_ok("a = 01"));
    CHECK(!toml_ok("a = 1__0"));
    CHECK(!toml_ok("a = 1."));
    CHECK(!toml_ok("a = \"\\q\""));
    CHECK(!toml_ok("a = \"\\uD800\""));
    CHECK(!toml_ok("a = \"tab\x01\""));
    CHECK(!toml_ok("a = 1\rb = 2"));
    CHECK(!toml_ok("a = {b = 1,}"));
    CHECK(!toml_ok("a = 1979-02-30"));
    CHECK(!toml_ok("a = 1 b = 2"));
    CHECK(!toml_ok("a = \"\xff\""));
    CHECK(!toml_ok("\xef\xbb\xbf" "a = 1"));
    {
        /* arrays nest at most 128 levels in the parser */
        char deep[400];
        int levels;
        for (levels = 128; levels <= 129; levels++) {
            int i, at = 0;
            at += sprintf(deep, "a = ");
            for (i = 0; i < levels; i++) {
                deep[at++] = '[';
            }
            for (i = 0; i < levels; i++) {
                deep[at++] = ']';
            }
            deep[at] = 0;
            CHECK(toml_ok(deep) == (levels == 128));
        }
    }
    {
        /* a table with many keys, looked up through its index */
        char *big = (char *)malloc(200000), *w = big;
        int i;
        for (i = 0; i < 10000; i++) {
            w += sprintf(w, "k%d = %d\n", i, i);
        }
        d = mncf_toml_parse(big, (size_t)(w - big), NULL, &err);
        CHECK(d != NULL && mncf_toml_count(mncf_toml_root(d)) == 10000 &&
              mncf_toml_integer(mncf_toml_get(mncf_toml_root(d), "k7777")) == 7777 &&
              mncf_toml_get(mncf_toml_root(d), "k10000") == NULL);
        mncf_toml_free(d);
        w += sprintf(w, "k5 = 1\n");
        d = mncf_toml_parse(big, (size_t)(w - big), NULL, &err);
        CHECK(d == NULL && err.status == MNCF_ERR_TOML);
        free(big);
    }
}

/* ---- Numbers and machine code ----------------------------------------------------------------------- */

static void test_numbers(void)
{
    mn_num a, b;
    uint8_t out[8];
    a = mn_num_u64(UINT64_MAX);
    b = mn_num_f64(18446744073709551616.0);
    CHECK(mn_num_cmp(&a, &b) == -1);           /* 2^64 - 1 < 2^64, though both round to the same double */
    a = mn_num_i64(9007199254740993LL);
    b = mn_num_f64(9007199254740992.0);
    CHECK(mn_num_cmp(&a, &b) == 1);
    a = mn_num_i64(3);
    b = mn_num_f64(3.5);
    CHECK(mn_num_cmp(&a, &b) == -1);
    b = mn_num_f64(NAN);
    CHECK(mn_num_cmp(&a, &b) == 2);
    a = mn_num_i64(0);
    b = mn_num_f64(-0.0);
    CHECK(mn_num_cmp(&a, &b) == 0);
    a = mn_num_bool(1);
    b = mn_num_i64(1);
    CHECK(mn_num_eq(&a, &b));
    a = mn_num_i64(-1);
    CHECK(!mn_fits(MNCF_TYPE_U8, &a) && mn_fits(MNCF_TYPE_S8, &a));
    a = mn_num_i64(-129);
    CHECK(!mn_fits(MNCF_TYPE_S8, &a));
    a = mn_num_f64(1e39);
    CHECK(!mn_fits(MNCF_TYPE_F32, &a) && mn_fits(MNCF_TYPE_F64, &a));
    a = mn_num_f64(INFINITY);
    CHECK(mn_fits(MNCF_TYPE_F32, &a));
    a = mn_num_f64(1.5);
    CHECK(!mn_fits(MNCF_TYPE_U32, &a));
    CHECK(mn_u64_to_double(9007199254740993ULL) == 9007199254740992.0);
    CHECK(mn_u64_to_double(9007199254740995ULL) == 9007199254740996.0);
    CHECK(mn_u64_to_double(UINT64_MAX) == 18446744073709551616.0);
    a = mn_num_i64(-2);
    mn_encode_value(MNCF_TYPE_S16, &a, 0, out);
    CHECK(out[0] == 0xFE && out[1] == 0xFF);
    mn_encode_value(MNCF_TYPE_S16, &a, 1, out);
    CHECK(out[0] == 0xFF && out[1] == 0xFE);
    a = mn_num_f64(1.0);
    mn_encode_value(MNCF_TYPE_F32, &a, 0, out);
    CHECK(out[0] == 0 && out[1] == 0 && out[2] == 0x80 && out[3] == 0x3F);
    out[0] = 0xFE;
    out[1] = 0xFF;
    a = mn_decode_value(MNCF_TYPE_S16, out, 0);
    CHECK(a.kind == MNCF_NUMBER_INT && a.negative && a.magnitude == 2);
    a = mn_decode_value(MNCF_TYPE_U16, out, 0);
    CHECK(!a.negative && a.magnitude == 0xFFFE);
}

static void test_encode(void)
{
    uint8_t out[MN_STUB_MAX];
    mn_num v;
    CHECK(mn_stub("arm64", 5, MNCF_ACTION_SKIP, 0, out) == 4 && hex_is(out, 4, "c0035fd6"));
    CHECK(mn_stub("arm64", 5, MNCF_ACTION_RETURN, 1, out) == 8 && hex_is(out, 8, "200080d2c0035fd6"));
    CHECK(mn_stub("arm64", 5, MNCF_ACTION_RETURN, (uint64_t)-1, out) == 8 && hex_is(out, 8, "00008092c0035fd6"));
    CHECK(mn_stub("arm64", 5, MNCF_ACTION_RETURN, 0x0001000000020003ULL, out) == 16);
    CHECK(mn_stub("arm64", 5, MNCF_ACTION_RETURN, 0x1234567890ABCDEFULL, out) == 20);
    CHECK(mn_stub("x86-64", 6, MNCF_ACTION_SKIP, 0, out) == 1 && out[0] == 0xC3);
    CHECK(mn_stub("x86-64", 6, MNCF_ACTION_RETURN, 7, out) == 6 && hex_is(out, 6, "b807000000c3"));
    CHECK(mn_stub("x86-64", 6, MNCF_ACTION_RETURN, (uint64_t)-2, out) == 8 && hex_is(out, 8, "48c7c0feffffffc3"));
    CHECK(mn_stub("x86-64", 6, MNCF_ACTION_RETURN, 0x100000000ULL, out) == 11);
    CHECK(mn_stub("mips", 4, MNCF_ACTION_SKIP, 0, out) == 0);
    v = mn_num_i64(5);
    CHECK(mn_arm64_imm(MN_INSN_MOVZ, 32, 8, &v, out) == 4 && hex_is(out, 4, "a8008052"));
    v = mn_num_i64(0x12345);
    CHECK(mn_arm64_imm(MN_INSN_MOVZ, 32, 8, &v, out) == 0);
    CHECK(mn_arm64_imm(MN_INSN_MOVZ_MOVK, 64, 0, &v, out) == 8 && hex_is(out, 8, "a06884d22000a0f2"));
    v = mn_num_f64(1.0);
    CHECK(mn_arm64_imm(MN_INSN_FMOV, 32, 0, &v, out) == 4 && hex_is(out, 4, "00102e1e"));
    CHECK(mn_arm64_imm(MN_INSN_MOVZ, 32, 0, &v, out) == 0);      /* movz takes whole numbers only */
    v = mn_num_f64(0.1);
    CHECK(mn_arm64_imm(MN_INSN_FMOV, 32, 0, &v, out) == 0);
    v = mn_num_i64(2);
    CHECK(mn_arm64_imm(MN_INSN_FMOV, 64, 1, &v, out) == 4 && hex_is(out, 4, "0110601e"));
}

/* ---- Trust lists and signatures --------------------------------------------------------------------- */

static void test_trust(void)
{
    mncf_error err;
    mncf_trust *t;
    const char *good = "format = \"mncf-keys/1\"\n[[key]]\nname = \"One\"\n"
                       "public = \"11qYAYKxCrfVS/7TyWQHOg7hcvPapiMlrwIaaPcHURo=\"\nid = \"21fe31dfa154a261\"\n"
                       "[[revoked]]\nid = \"39f713d0a644253f\"\n";
    t = mncf_trust_read(good, strlen(good), NULL, &err);
    CHECK(t != NULL && t->key_count == 1 && t->revoked_count == 1);
    mncf_trust_free(t);
    t = mncf_trust_read("format = \"mncf-keys/1\"\n[[key]]\nname = \"One\"\npublic = \"11qYAYKxCrfVS/7TyWQHOg7hcvPapiMlrwIaaPcHURp=\"\n",
                        strlen("format = \"mncf-keys/1\"\n[[key]]\nname = \"One\"\npublic = \"11qYAYKxCrfVS/7TyWQHOg7hcvPapiMlrwIaaPcHURp=\"\n"),
                        NULL, &err);
    CHECK(t == NULL && strcmp(err.code, "schema:key") == 0);
    t = mncf_trust_read("format = \"mncf-keys/1\"\n[[revoked]]\nid = \"39F713D0A644253F\"\n",
                        strlen("format = \"mncf-keys/1\"\n[[revoked]]\nid = \"39F713D0A644253F\"\n"), NULL, &err);
    CHECK(t == NULL && strcmp(err.code, "schema:revoked") == 0);
    t = mncf_trust_read("name = 1\nformat = \"mncf-keys/1\"\n", 32, NULL, &err);
    CHECK(t == NULL && strcmp(err.code, "format") == 0);
    t = mncf_trust_read("format = \"mncf-keys/1\"\nwhen = 1979-05-27\n", 41, NULL, &err);
    CHECK(t == NULL && strcmp(err.code, "toml") == 0);
}

static void test_signatures(void)
{
    char text[1024];
    mncf_error err;
    mncf_trust *t = mncf_trust_new(NULL);
    uint8_t pub[32];
    mncf_file *f;
    unhex(RFC8032[0][0], pub, 32);
    mncf_trust_add(t, "Test key one", pub);
    snprintf(text, sizeof text, "%s%s", FILE_TEXT, FILE_SIGNATURE);
    f = mncf_load(text, strlen(text), t, NULL, &err);
    CHECK(f != NULL && mncf_file_info_of(f)->signature.result == MNCF_SIGNATURE_VERIFIED &&
          mncf_file_info_of(f)->signature.signer_count == 1 &&
          strcmp(mncf_file_info_of(f)->signature.signers[0], "Test key one") == 0 &&
          mncf_file_info_of(f)->signature.signed_size == strlen(FILE_TEXT));
    mncf_free(f);
    f = mncf_load(text, strlen(text), NULL, NULL, &err);
    CHECK(f != NULL && mncf_file_info_of(f)->signature.result == MNCF_SIGNATURE_UNKNOWN_KEY);
    mncf_free(f);
    text[30] = 'X';                     /* the name changes after signing */
    f = mncf_load(text, strlen(text), t, NULL, &err);
    CHECK(f != NULL && mncf_file_info_of(f)->signature.result == MNCF_SIGNATURE_INVALID);
    mncf_free(f);
    snprintf(text, sizeof text, "%s%s", FILE_TEXT, FILE_SIGNATURE);
    mncf_trust_revoke(t, "21FE31DFA154A261");
    f = mncf_load(text, strlen(text), t, NULL, &err);
    CHECK(f != NULL && mncf_file_info_of(f)->signature.result == MNCF_SIGNATURE_REVOKED);
    mncf_free(f);
    /* a key after the signature line is not covered by it: the file is invalid (the key goes into the
     * table open there, cheat.write, where it is unknown) */
    snprintf(text, sizeof text, "%s%sname = 1\n", FILE_TEXT, FILE_SIGNATURE);
    f = mncf_load(text, strlen(text), NULL, NULL, &err);
    CHECK(f != NULL && mncf_file_info_of(f)->signature.result == MNCF_SIGNATURE_INVALID &&
          strcmp(mncf_file_info_of(f)->cheats[0].problem, "unknown-key:write.name") == 0);
    mncf_free(f);
    snprintf(text, sizeof text, "%s%s# a comment\n", FILE_TEXT, FILE_SIGNATURE);
    f = mncf_load(text, strlen(text), NULL, NULL, &err);
    CHECK(f != NULL && mncf_file_info_of(f)->signature.result == MNCF_SIGNATURE_INVALID);
    mncf_free(f);
    mncf_trust_free(t);
}

/* ---- The API ---------------------------------------------------------------------------------------- */

static const char ENGINE_TEXT[] =
    "format = \"mncf/1\"\nname = \"T\"\n\n[game]\nplatform = \"switch\"\nid = \"0100000000ABC000\"\n\n"
    "[builds.\"1.0.2\"]\nmodules = { main = \"A1B2C3D4E5F60718\" }\n\n"
    "[[cheat]]\nid = \"money\"\nname = \"Money\"\ndefault = true\n"
    "  [cheat.values.count]\n  type = \"u32\"\n  max = 1000\n  default = 100\n"
    "  [[cheat.write]]\n  at = { region = \"heap\", offset = 0x10 }\n  type = \"u32\"\n  value = \"$count\"\n\n"
    "[[cheat]]\nid = \"speed\"\nname = \"Speed\"\nhold = \"ZL\"\n"
    "  [[cheat.patch]]\n  at = { module = \"main\", find = \"68 12 00 B9 ?? ?? ?? 94\" }\n"
    "  expect = \"68 12 00 B9\"\n  bytes = \"69 12 00 B9\"\n";

typedef struct world {
    uint8_t image[0x1000];
    uint8_t heap[0x100];
    mncf_module module;
    mncf_region region;
    mncf_runtime rt;
} world;

static int world_read(void *ctx, uint64_t address, void *out, size_t size)
{
    world *w = (world *)ctx;
    if (address >= 0x8000000 && address - 0x8000000 + size <= sizeof w->image) {
        memcpy(out, w->image + (address - 0x8000000), size);
        return 1;
    }
    if (address >= 0x10000000 && address - 0x10000000 + size <= sizeof w->heap) {
        memcpy(out, w->heap + (address - 0x10000000), size);
        return 1;
    }
    return 0;
}

static void world_init(world *w)
{
    static const uint8_t code[] = {0x68, 0x12, 0x00, 0xB9, 0x10, 0x00, 0x00, 0x94};
    memset(w, 0, sizeof *w);
    memcpy(w->image + 0x100, code, sizeof code);
    w->module.name = "main";
    w->module.id = "A1B2C3D4E5F60718";
    w->module.base = 0x8000000;
    w->module.data = w->image;
    w->module.data_size = sizeof w->image;
    w->module.size = sizeof w->image;
    w->module.has_size = 1;
    w->region.name = "heap";
    w->region.base = 0x10000000;
    w->region.size = sizeof w->heap;
    mncf_runtime_init(&w->rt, "switch");
    w->rt.modules = &w->module;
    w->rt.module_count = 1;
    w->rt.regions = &w->region;
    w->rt.region_count = 1;
    w->rt.read = world_read;
    w->rt.read_ctx = w;
}

static void test_api(void)
{
    mncf_error err;
    world w;
    mncf_file *f;
    mncf_evaluation *ev;
    mncf_engine *eng;
    mncf_pass pass;
    const char *reason;
    const char *held[] = {"ZL"};
    int is_on = -1;
    CHECK(strcmp(mncf_version(), MNCF_VERSION) == 0 && MNCF_VERSION_NUMBER == 10100);
    CHECK(mncf_load(NULL, 1, NULL, NULL, &err) == NULL && err.status == MNCF_ERR_ARGUMENT);
    CHECK(mncf_load("", 0, NULL, NULL, &err) == NULL && strcmp(err.code, "format") == 0);
    CHECK(mncf_load("format = \"mncf/2\"", 17, NULL, NULL, NULL) == NULL);
    world_init(&w);
    f = mncf_load(ENGINE_TEXT, strlen(ENGINE_TEXT), NULL, NULL, &err);
    CHECK(f != NULL);
    if (f == NULL) {
        printf("  %s: %s\n", err.code, err.message);
        return;
    }
    CHECK(mncf_file_cheat(f, "speed") != NULL && mncf_file_cheat(f, "slow") == NULL);
    CHECK(mncf_file_info_of(f)->cheats[0].value_count == 1 && mncf_file_info_of(f)->build_count == 1);
    ev = mncf_evaluate(f, &w.rt, NULL, &err);
    CHECK(ev != NULL && strcmp(mncf_evaluation_build(ev), "1.0.2") == 0 && mncf_evaluation_count(ev) == 2);
    CHECK(ev != NULL && mncf_evaluation_cheat(ev, 0)->state == MNCF_STATE_ON &&
          mncf_evaluation_cheat(ev, 1)->state == MNCF_STATE_OFF && mncf_evaluation_cheat(ev, 2) == NULL);
    CHECK(ev != NULL && mncf_evaluation_cheat(ev, 1)->ops[0].address == 0x8000100);
    mncf_evaluation_free(ev);
    CHECK(mncf_engine_new(f, &w.rt, NULL, 0, &err) == NULL && err.status == MNCF_ERR_ARGUMENT);
    eng = mncf_engine_new(f, &w.rt, NULL, 60, &err);
    CHECK(eng != NULL);
    if (eng == NULL) {
        mncf_free(f);
        return;
    }
    CHECK(mncf_engine_switch(eng, "nothing", 1, NULL) == MNCF_ERR_ARGUMENT);
    CHECK(mncf_engine_set_value(eng, "money.nothing", mncf_number_int(1)) == MNCF_ERR_ARGUMENT);
    CHECK(mncf_engine_set_value(eng, "money", mncf_number_int(1)) == MNCF_ERR_ARGUMENT);
    CHECK(mncf_engine_tick(eng, NULL, NULL, NULL, &pass) == MNCF_OK && pass.number == 1 && pass.change_count == 1 &&
          pass.changes[0].kind == MNCF_CHANGE_WRITE && pass.changes[0].address == 0x10000010 &&
          pass.changes[0].size == 4 && pass.changes[0].bytes[0] == 100);
    CHECK(mncf_engine_set_value(eng, "money.count", mncf_number_int(5000)) == MNCF_OK);
    CHECK(mncf_engine_state(eng, 0, &reason) == MNCF_STATE_REFUSED && strcmp(reason, "value-out-of-range") == 0);
    CHECK(mncf_engine_set_value(eng, "money.count", mncf_number_int(7)) == MNCF_OK);
    CHECK(mncf_engine_state(eng, 0, &reason) == MNCF_STATE_OFF);
    CHECK(mncf_engine_switch(eng, "money", 1, &is_on) == MNCF_OK && is_on == 1);
    CHECK(mncf_engine_switch(eng, "speed", 1, &is_on) == MNCF_OK && is_on == 1);
    CHECK(mncf_engine_state(eng, 1, &reason) == MNCF_STATE_WAITING);
    CHECK(mncf_engine_hold(eng, held, 1) == MNCF_OK);
    CHECK(mncf_engine_tick(eng, NULL, NULL, NULL, &pass) == MNCF_OK && pass.change_count == 2 &&
          pass.changes[0].kind == MNCF_CHANGE_PATCH && pass.changes[0].address == 0x8000100 &&
          pass.changes[1].kind == MNCF_CHANGE_WRITE && pass.changes[1].bytes[0] == 7);
    w.image[0x100] = 0x69;              /* the runtime made the patch */
    CHECK(mncf_engine_hold(eng, NULL, 0) == MNCF_OK);
    CHECK(mncf_engine_tick(eng, NULL, NULL, NULL, &pass) == MNCF_OK && pass.change_count == 2 &&
          pass.changes[0].kind == MNCF_CHANGE_RESTORE && pass.changes[0].bytes[0] == 0x68);
    CHECK(mncf_engine_stop(eng, &pass) == MNCF_OK && pass.change_count == 0);
    CHECK(mncf_engine_state(eng, 0, &reason) == MNCF_STATE_OFF);
    CHECK(mncf_engine_tick(NULL, NULL, NULL, NULL, &pass) == MNCF_ERR_ARGUMENT);
    CHECK(mncf_engine_state(eng, 99, &reason) == MNCF_STATE_OFF && strcmp(reason, "") == 0);
    CHECK(strcmp(mncf_engine_build(eng), "1.0.2") == 0);
    mncf_engine_free(eng);
    mncf_free(f);
    CHECK(strcmp(mncf_type_name(MNCF_TYPE_F32), "f32") == 0 && strcmp(mncf_signature_name(MNCF_SIGNATURE_REVOKED), "revoked") == 0);
    CHECK(strcmp(mncf_compare_name(MNCF_COMPARE_NAND), "!&") == 0 && strcmp(mncf_change_name(MNCF_CHANGE_UNHOOK), "unhook") == 0);
}

/* A module without sections is code only: a pattern in its data finds nothing (section 6.3). */
static void test_sections(void)
{
    static const char text[] =
        "format = \"mncf/1\"\nname = \"T\"\n\n[game]\nplatform = \"switch\"\nid = \"0100000000ABC000\"\n\n"
        "[[cheat]]\nid = \"in-code\"\nname = \"c\"\n"
        "  [[cheat.patch]]\n  at = { module = \"main\", find = \"68 12 00 B9 ?? ?? ?? 94\", section = \"code\" }\n"
        "  expect = \"68 12 00 B9\"\n  bytes = \"69 12 00 B9\"\n\n"
        "[[cheat]]\nid = \"in-data\"\nname = \"d\"\n"
        "  [[cheat.patch]]\n  at = { module = \"main\", find = \"68 12 00 B9 ?? ?? ?? 94\", section = \"data\" }\n"
        "  expect = \"68 12 00 B9\"\n  bytes = \"69 12 00 B9\"\n\n"
        "[[cheat]]\nid = \"overlap\"\nname = \"o\"\n"
        "  [[cheat.patch]]\n  at = { module = \"main\", find = \"AA ?? AA AA\" }\n"
        "  expect = \"AA\"\n  bytes = \"AB\"\n";
    mncf_error err;
    world w;
    mncf_file *f;
    mncf_evaluation *ev;
    world_init(&w);
    memset(w.image + 0x200, 0xAA, 5);       /* five bytes AA: the pattern matches at two places */
    f = mncf_load(text, strlen(text), NULL, NULL, &err);
    CHECK(f != NULL);
    if (f == NULL) {
        return;
    }
    ev = mncf_evaluate(f, &w.rt, NULL, &err);
    CHECK(ev != NULL && strcmp(mncf_evaluation_cheat(ev, 0)->reason, "") == 0 &&
          mncf_evaluation_cheat(ev, 0)->ops[0].address == 0x8000100);
    CHECK(ev != NULL && strcmp(mncf_evaluation_cheat(ev, 1)->reason, "pattern-not-found") == 0);
    CHECK(ev != NULL && strcmp(mncf_evaluation_cheat(ev, 2)->reason, "pattern-ambiguous") == 0);
    mncf_evaluation_free(ev);
    mncf_free(f);
}

/* Parts of the core a runtime does not have yet, and code it rewrote (sections 11.1 and 11.3). */
static void test_runtime_gaps(void)
{
    static const char text[] =
        "format = \"mncf/1\"\nname = \"T\"\n\n[game]\nplatform = \"switch\"\nid = \"0100000000ABC000\"\n\n"
        "[[cheat]]\nid = \"timed-toggle\"\nname = \"t\"\nrun = \"timed\"\nduration = 5\ntoggle = \"L\"\n"
        "  [[cheat.write]]\n  at = { region = \"heap\", offset = 0x0 }\n  type = \"u8\"\n  value = 1\n\n"
        "[[cheat]]\nid = \"interval-alone\"\nname = \"i\"\ninterval = 2\n"
        "  [[cheat.write]]\n  at = { region = \"heap\", offset = 0x0 }\n  type = \"u8\"\n  value = 1\n\n"
        "[[cheat]]\nid = \"interval-when\"\nname = \"w\"\ninterval = 2\n"
        "  [[cheat.when]]\n  at = { region = \"heap\", offset = 0x10 }\n  type = \"u8\"\n  op = \"changed\"\n"
        "  [[cheat.write]]\n  at = { region = \"heap\", offset = 0x0 }\n  type = \"u8\"\n  value = 1\n\n"
        "[[cheat]]\nid = \"over-rewritten\"\nname = \"r\"\n"
        "  [[cheat.patch]]\n  at = { module = \"main\", offset = 0x100 }\n  expect = \"68 12 00 B9\"\n"
        "  bytes = \"69 12 00 B9\"\n\n"
        "[[cheat]]\nid = \"elsewhere\"\nname = \"e\"\n"
        "  [[cheat.patch]]\n  at = { module = \"main\", offset = 0x200 }\n  expect = \"00 00 00 00\"\n"
        "  bytes = \"01 00 00 00\"\n\n"
        "[[cheat]]\nid = \"stub-over-rewritten\"\nname = \"s\"\n"
        "  [[cheat.override]]\n  function = { module = \"main\", offset = 0x100 }\n  action = \"skip\"\n"
        "  expect = \"68 12 00 B9\"\n";
    static const char *const unsupported[] = {"toggle", "interval"};
    mncf_rewritten runs[3];
    mncf_error err;
    world w;
    mncf_file *f;
    mncf_evaluation *ev;
    world_init(&w);
    runs[0].module = "main";
    runs[0].offset = 0x102;
    runs[0].length = 4;
    runs[1].module = "main";             /* runs near 2^64 overlap nothing below them */
    runs[1].offset = UINT64_MAX - 0x10;
    runs[1].length = 0x100;
    runs[2].module = "sdk";
    runs[2].offset = 0x200;
    runs[2].length = 4;
    w.rt.unsupported = unsupported;
    w.rt.unsupported_count = 2;
    w.rt.rewritten = runs;
    w.rt.rewritten_count = 3;
    f = mncf_load(text, strlen(text), NULL, NULL, &err);
    CHECK(f != NULL);
    if (f == NULL) {
        printf("  %s: %s\n", err.code, err.message);
        return;
    }
    ev = mncf_evaluate(f, &w.rt, NULL, &err);
    CHECK(ev != NULL);
    if (ev != NULL) {
        CHECK(strcmp(mncf_evaluation_cheat(ev, 0)->reason, "runtime:unsupported:toggle") == 0);
        CHECK(strcmp(mncf_evaluation_cheat(ev, 1)->reason, "") == 0);   /* interval counts with conditions */
        CHECK(strcmp(mncf_evaluation_cheat(ev, 2)->reason, "runtime:unsupported:interval") == 0);
        CHECK(strcmp(mncf_evaluation_cheat(ev, 3)->reason, "runtime:rewritten") == 0);
        CHECK(mncf_evaluation_cheat(ev, 4)->state == MNCF_STATE_OFF && strcmp(mncf_evaluation_cheat(ev, 4)->reason, "") == 0);
        CHECK(strcmp(mncf_evaluation_cheat(ev, 5)->reason, "runtime:rewritten") == 0);
    }
    mncf_evaluation_free(ev);
    mncf_free(f);
}

/* ---- Updates (section 17) --------------------------------------------------------------------------- */

static const char UPDATE_OLD[] =
    "format = \"mncf/1\"\nname = \"U\"\nid = \"example.org/u\"\nfile_version = \"1\"\n\n"
    "[game]\nplatform = \"switch\"\nid = \"0100000000ABC000\"\n\n"
    "[[cheat]]\nid = \"money\"\nname = \"Money\"\n"
    "  [cheat.values.count]\n  type = \"u32\"\n  max = 1000\n  default = 100\n"
    "  [[cheat.write]]\n  at = { region = \"heap\", offset = 0x10 }\n  type = \"u32\"\n  value = \"$count\"\n\n"
    "[[cheat]]\nid = \"speed\"\nname = \"Speed\"\n"
    "  [[cheat.write]]\n  at = { region = \"heap\", offset = 0x20 }\n  type = \"u8\"\n  value = 1\n\n"
    "[[preset]]\nid = \"rich\"\nname = \"Rich\"\ncheats = [\"money\"]\n";

static const char UPDATE_NEW[] =
    "format = \"mncf/1\"\nname = \"U\"\nid = \"example.org/u\"\nfile_version = \"2.1\"\n"
    "signers = [\"11qYAYKxCrfVS/7TyWQHOg7hcvPapiMlrwIaaPcHURo=\"]\n\n"
    "[update]\nurl = \"https://example.org:8443/u.mncf?v=2\"\n\n"
    "[[changes]]\nversion = \"2\"\ntext = \"Coins.\"\nrenamed = { money = \"coins\" }\nremoved = [\"speed\"]\n\n"
    "[[changes]]\nversion = \"2.1\"\ndate = \"2026-12-02\"\nadded = [\"jump\"]\nfixed = [\"coins\"]\n\n"
    "[game]\nplatform = \"switch\"\nid = \"0100000000abc000\"\n\n"
    "[[cheat]]\nid = \"coins\"\nname = \"Coins\"\n"
    "  [cheat.values.count]\n  type = \"u32\"\n  max = 500\n  default = 100\n"
    "  [cheat.values.rate]\n  type = \"f32\"\n  default = 1.0\n"
    "  [[cheat.write]]\n  at = { region = \"heap\", offset = 0x10 }\n  type = \"u32\"\n  value = \"$count\"\n\n"
    "[[cheat]]\nid = \"jump\"\nname = \"Jump\"\n"
    "  [[cheat.write]]\n  at = { region = \"heap\", offset = 0x30 }\n  type = \"u8\"\n  value = 1\n\n"
    "[[preset]]\nid = \"rich\"\nname = \"Rich\"\ncheats = [\"coins\"]\n\n"
    "[i18n.de.changes.\"2\"]\ntext = \"Muenzen.\"\n";

/* A pack of two files: a manifest that starts with head, then the two files with their hashes. Free it
 * with free(). */
static uint8_t *two_file_pack(const char *head, const char *name_a, const char *text_a, const char *name_b,
                              const char *text_b, size_t *len)
{
    static const char H[] = "0123456789abcdef";
    char manifest[1024], hex[2][65];
    uint8_t hash[32];
    test_entry e[3];
    size_t i, k;
    for (k = 0; k < 2; k++) {
        const char *text = k == 0 ? text_a : text_b;
        mncf_sha256(text, strlen(text), hash);
        for (i = 0; i < 32; i++) {
            hex[k][2 * i] = H[hash[i] >> 4];
            hex[k][2 * i + 1] = H[hash[i] & 15];
        }
        hex[k][64] = 0;
    }
    snprintf(manifest, sizeof manifest, "%s\n[[file]]\npath = \"%s\"\nsha256 = \"%s\"\n\n[[file]]\npath = \"%s\"\nsha256 = \"%s\"\n",
             head, name_a, hex[0], name_b, hex[1]);
    memset(e, 0, sizeof e);
    e[0].name = "pack.toml";
    e[0].data = (const uint8_t *)manifest;
    e[0].size = strlen(manifest);
    e[1].name = name_a;
    e[1].data = (const uint8_t *)text_a;
    e[1].size = strlen(text_a);
    e[2].name = name_b;
    e[2].data = (const uint8_t *)text_b;
    e[2].size = strlen(text_b);
    return make_zip(e, 3, 0, len);
}

static const char PACK_OLD_HEAD[] = "format = \"mncf-pack/1\"\nname = \"P\"\nid = \"example.org/p\"\nfile_version = \"1\"\n";
static const char PACK_NEW_HEAD[] =
    "format = \"mncf-pack/1\"\nname = \"P\"\nid = \"example.org/p\"\nfile_version = \"2\"\n"
    "[[changes]]\nversion = \"2\"\nrenamed = { \"b.mncf\" = \"c.mncf\" }\nadded = [\"dir/x.mncf\"]\n";

static void test_update_forms(void)
{
    char host[8], url[MNCF_MAX_URL + 2];
    int r = 7;
    CHECK(mncf_version_valid("1.0.0.0.0.0.0.0") && !mncf_version_valid(NULL) && !mncf_version_valid("1.") &&
          !mncf_version_valid(".1") && !mncf_version_valid("+1"));
    CHECK(mncf_version_compare("1.0.0.0.0.0.0.0", "1", &r) == MNCF_OK && r == 0);
    CHECK(mncf_version_compare("999999999", "99999999.9", &r) == MNCF_OK && r == 1);
    CHECK(mncf_version_compare("0.1", "0.01", &r) == MNCF_OK && r == 0);
    r = 7;
    CHECK(mncf_version_compare("1", "v1", &r) == MNCF_ERR_ARGUMENT && r == 7);
    CHECK(mncf_version_compare(NULL, "1", NULL) == MNCF_ERR_ARGUMENT);
    memset(url, 'a', MNCF_MAX_FILE_ID);
    url[MNCF_MAX_FILE_ID] = 0;
    CHECK(mncf_file_id_valid(url));
    url[MNCF_MAX_FILE_ID] = 'a';
    url[MNCF_MAX_FILE_ID + 1] = 0;
    CHECK(!mncf_file_id_valid(url) && !mncf_file_id_valid(NULL) && !mncf_file_id_valid("a/./b") &&
          !mncf_file_id_valid("a/b/") && mncf_file_id_valid("..a/b.."));
    CHECK(mncf_date_valid("0000-02-29") && mncf_date_valid("2026-12-31") && !mncf_date_valid("2026-12-32") &&
          !mncf_date_valid("2026-12-3a") && !mncf_date_valid(NULL));
    /* the host, cut to the room given */
    CHECK(mncf_update_url_host("https://example.org/x", host, sizeof host) == 11 && strcmp(host, "example") == 0);
    CHECK(mncf_update_url_host("https://example.org", NULL, 0) == 11);
    CHECK(mncf_update_url_host("https://a.example:65535/?q?r=%41", host, sizeof host) == 9 &&
          strcmp(host, "a.examp") == 0);
    CHECK(mncf_update_url_host("https://a.example:/x", host, sizeof host) == 0 && host[0] == 0);
    CHECK(mncf_update_url_host("https://a.example:123456/x", host, sizeof host) == 0);
    CHECK(mncf_update_url_host("https://example.org/x%4", host, sizeof host) == 0);
    CHECK(mncf_update_url_host("https://example.org/x%", host, sizeof host) == 0);
    CHECK(mncf_update_url_host("https://1.2.example/x", host, sizeof host) == 11);
    CHECK(mncf_update_url_host("https://example.123/x", host, sizeof host) == 0);
    CHECK(mncf_update_url_host("https://a-.example/x", host, sizeof host) == 0);
    CHECK(mncf_update_url_host("https://example", host, sizeof host) == 0 && mncf_update_url_host(NULL, host, 1) == 0);
    memset(url, 'a', sizeof url - 1);
    url[sizeof url - 1] = 0;
    memcpy(url, "https://example.org/", 20);
    url[MNCF_MAX_URL] = 0;
    CHECK(mncf_update_url_host(url, NULL, 0) == 11);
    url[MNCF_MAX_URL] = 'a';
    CHECK(mncf_update_url_host(url, NULL, 0) == 0);
}

static void test_update_keys(void)
{
    mncf_error err;
    mncf_file *f = mncf_load(UPDATE_NEW, strlen(UPDATE_NEW), NULL, NULL, &err);
    const mncf_file_info *info;
    const mncf_release *r;
    uint8_t one[32];
    CHECK(f != NULL);
    if (f == NULL) {
        printf("  %s: %s\n", err.code, err.message);
        return;
    }
    unhex(RFC8032[0][0], one, 32);
    info = mncf_file_info_of(f);
    CHECK(info->warning_count == 0 && strcmp(info->id, "example.org/u") == 0);
    CHECK(strcmp(info->update_url, "https://example.org:8443/u.mncf?v=2") == 0 &&
          strcmp(info->update_host, "example.org") == 0);
    CHECK(info->signer_count == 1 && memcmp(info->signers[0], one, 32) == 0 &&
          info->signature.result == MNCF_SIGNATURE_UNSIGNED);
    CHECK(info->change_count == 2);
    if (info->change_count != 2) {
        mncf_free(f);
        return;
    }
    r = &info->changes[0];              /* newest first */
    CHECK(strcmp(r->version, "2.1") == 0 && strcmp(r->date, "2026-12-02") == 0 && r->text == NULL &&
          r->added_count == 1 && strcmp(r->added[0], "jump") == 0 && r->fixed_count == 1 &&
          strcmp(r->fixed[0], "coins") == 0 && r->removed_count == 0 && r->renamed_count == 0);
    r = &info->changes[1];
    CHECK(strcmp(r->version, "2") == 0 && r->date == NULL && strcmp(r->text, "Coins.") == 0 &&
          r->removed_count == 1 && strcmp(r->removed[0], "speed") == 0 && r->renamed_count == 1 &&
          strcmp(r->renamed[0].from, "money") == 0 && strcmp(r->renamed[0].to, "coins") == 0);
    /* the entries to show and their texts */
    CHECK(mncf_releases_since(info->changes, 2, "1") == 2 && mncf_releases_since(info->changes, 2, "2") == 1 &&
          mncf_releases_since(info->changes, 2, "2.1.0") == 0 && mncf_releases_since(info->changes, 2, NULL) == 2 &&
          mncf_releases_since(info->changes, 2, "v1") == 2 && mncf_releases_since(NULL, 2, NULL) == 0);
    CHECK(strcmp(mncf_release_text(f, r, "de"), "Muenzen.") == 0 && strcmp(mncf_release_text(f, r, "fr"), "Coins.") == 0 &&
          strcmp(mncf_release_text(f, r, ""), "Coins.") == 0 && strcmp(mncf_release_text(NULL, r, "de"), "Coins.") == 0 &&
          mncf_release_text(f, &info->changes[0], "de") == NULL && mncf_release_text(f, NULL, "de") == NULL);
    mncf_free(f);
}

static void test_update_check(void)
{
    mncf_error err;
    mncf_file *old = mncf_load(UPDATE_OLD, strlen(UPDATE_OLD), NULL, NULL, &err), *fresh;
    mncf_update *u;
    const mncf_update_info *info;
    CHECK(old != NULL);
    if (old == NULL) {
        return;
    }
    CHECK(mncf_update_check(NULL, UPDATE_NEW, 3, NULL, NULL, &err) == NULL && err.status == MNCF_ERR_ARGUMENT);
    CHECK(mncf_update_check(old, NULL, 3, NULL, NULL, &err) == NULL && err.status == MNCF_ERR_ARGUMENT);
    u = mncf_update_check(old, "nope", 4, NULL, NULL, &err);
    info = mncf_update_info_of(u);
    CHECK(u != NULL && strcmp(info->reason, "update:error:toml") == 0 && info->file == NULL &&
          info->to_version == NULL && strcmp(info->from_version, "1") == 0 && mncf_update_take_file(u) == NULL);
    mncf_update_free(u);
    u = mncf_update_check(old, UPDATE_OLD, strlen(UPDATE_OLD), NULL, NULL, &err);
    CHECK(u != NULL && strcmp(mncf_update_info_of(u)->reason, "update:version") == 0);
    mncf_update_free(u);
    u = mncf_update_check(old, UPDATE_NEW, strlen(UPDATE_NEW), NULL, NULL, &err);
    info = mncf_update_info_of(u);
    CHECK(u != NULL && strcmp(info->reason, "") == 0 && strcmp(info->to_version, "2.1") == 0 && info->file != NULL &&
          info->pack == NULL);
    fresh = mncf_update_take_file(u);
    CHECK(fresh != NULL && info->file == NULL && mncf_update_take_file(u) == NULL && mncf_update_take_pack(u) == NULL);
    mncf_update_free(u);
    CHECK(mncf_update_info_of(NULL) == NULL && mncf_update_take_file(NULL) == NULL);
    mncf_update_free(NULL);
    /* the old version is no new version of the new one */
    u = mncf_update_check(fresh, UPDATE_OLD, strlen(UPDATE_OLD), NULL, NULL, &err);
    CHECK(u != NULL && strcmp(mncf_update_info_of(u)->reason, "update:version") == 0);
    mncf_update_free(u);
    mncf_free(fresh);
    mncf_free(old);
}

static void test_update_settings(void)
{
    mncf_error err;
    mncf_file *f = mncf_load(UPDATE_NEW, strlen(UPDATE_NEW), NULL, NULL, &err);
    const char *names[] = {"money", "speed", "nothing", NULL};
    const char *out[4];
    const char *on[] = {"money", "money"}, *off[] = {"speed"}, *kept[] = {"speed", "ghost"};
    mncf_choice_value values[5];
    mncf_choices ch;
    mncf_marks *m;
    mncf_carried *c;
    if (f == NULL) {
        CHECK(f != NULL);
        return;
    }
    CHECK(mncf_ids(f, "1", names, 4, out, &err) == MNCF_OK && out[0] == mncf_file_cheat(f, "coins")->id &&
          out[1] == NULL && out[2] == NULL && out[3] == NULL);
    CHECK(mncf_ids(f, "2", names, 1, out, &err) == MNCF_OK && out[0] == NULL);
    CHECK(mncf_ids(f, "1", NULL, 1, out, &err) == MNCF_ERR_ARGUMENT && err.status == MNCF_ERR_ARGUMENT);
    CHECK(mncf_ids(f, "1", names, 0, NULL, &err) == MNCF_OK);
    m = mncf_mark(f, "1", &err);
    CHECK(m != NULL && m->new_count == 1 && strcmp(m->new_ids[0], "jump") == 0 && m->fixed_count == 1 &&
          strcmp(m->fixed_ids[0], "coins") == 0);
    mncf_marks_free(m);
    m = mncf_mark(f, "2.1", &err);
    CHECK(m != NULL && m->new_count == 0 && m->fixed_count == 0);
    mncf_marks_free(m);
    CHECK(mncf_mark(NULL, "1", &err) == NULL && err.status == MNCF_ERR_ARGUMENT);
    /* a key given twice: the last value counts; a whole number fits a float value; a key without a value
     * id is gone */
    values[0].key = "money.count";
    values[0].value = mncf_number_int(300);
    values[1].key = "money.count";
    values[1].value = mncf_number_int(900);
    values[2].key = "money.rate";
    values[2].value = mncf_number_int(2);
    values[3].key = "speed.x";
    values[3].value = mncf_number_int(1);
    values[4].key = "money";
    values[4].value = mncf_number_int(5);
    memset(&ch, 0, sizeof ch);
    ch.on = on;
    ch.on_count = 2;
    ch.off = off;
    ch.off_count = 1;
    ch.values = values;
    ch.value_count = 5;
    c = mncf_carry(f, "1", &ch, "rich", kept, 2, &err);
    CHECK(c != NULL);
    if (c != NULL) {
        CHECK(c->choices.on_count == 1 && strcmp(c->choices.on[0], "coins") == 0 && c->choices.off_count == 0);
        CHECK(c->id_count == 3 && strcmp(c->ids[0].from, "money") == 0 && strcmp(c->ids[0].to, "coins") == 0 &&
              strcmp(c->ids[1].from, "speed") == 0 && c->ids[1].to == NULL && strcmp(c->ids[2].from, "ghost") == 0 &&
              c->ids[2].to == NULL);
        CHECK(c->dropped_count == 2 && strcmp(c->dropped[0], "speed") == 0 && strcmp(c->dropped[1], "ghost") == 0);
        CHECK(c->reset_count == 1 && strcmp(c->reset[0].key, "coins.count") == 0 && strcmp(c->reset[0].why, "range") == 0);
        CHECK(c->choices.value_count == 1 && strcmp(c->choices.values[0].key, "coins.rate") == 0 &&
              c->choices.values[0].value.kind == MNCF_NUMBER_INT && c->choices.values[0].value.magnitude == 2);
        CHECK(c->gone_count == 1 && strcmp(c->gone[0], "money") == 0 && strcmp(c->preset, "rich") == 0);
    }
    mncf_carried_free(c);
    c = mncf_carry(f, NULL, NULL, "slow", NULL, 0, &err);
    CHECK(c != NULL && c->id_count == 0 && c->choices.on_count == 0 && c->preset == NULL);
    mncf_carried_free(c);
    ch.on = NULL;
    CHECK(mncf_carry(f, NULL, &ch, NULL, NULL, 0, &err) == NULL && err.status == MNCF_ERR_ARGUMENT);
    mncf_carried_free(NULL);
    mncf_marks_free(NULL);
    mncf_free(f);
}

static void test_update_packs(void)
{
    mncf_error err;
    size_t old_len, new_len;
    uint8_t *old_zip = two_file_pack(PACK_OLD_HEAD, "a.mncf", UPDATE_OLD, "b.mncf", FILE_TEXT, &old_len);
    uint8_t *new_zip = two_file_pack(PACK_NEW_HEAD, "a2.mncf", UPDATE_NEW, "c.mncf", FILE_TEXT, &new_len);
    mncf_pack *old = mncf_pack_read(old_zip, old_len, NULL, NULL, &err), *fresh;
    mncf_update *u;
    mncf_pairs *pairs;
    const mncf_pack_info *info;
    CHECK(old != NULL);
    if (old == NULL) {
        free(old_zip);
        free(new_zip);
        return;
    }
    u = mncf_pack_update_check(old, new_zip, new_len, NULL, NULL, &err);
    CHECK(u != NULL && strcmp(mncf_update_info_of(u)->reason, "") == 0 && mncf_update_info_of(u)->file == NULL &&
          strcmp(mncf_update_info_of(u)->to_version, "2") == 0);
    fresh = mncf_update_take_pack(u);
    mncf_update_free(u);
    CHECK(fresh != NULL);
    if (fresh != NULL) {
        info = mncf_pack_info_of(fresh);
        CHECK(strcmp(info->id, "example.org/p") == 0 && info->change_count == 1 && info->update_url == NULL &&
              info->changes[0].renamed_count == 1 && strcmp(info->changes[0].renamed[0].to, "c.mncf") == 0 &&
              info->changes[0].added_count == 1 && strcmp(info->changes[0].added[0], "dir/x.mncf") == 0);
        /* by id and game, whatever the case of the game ID, and by path where neither file has an id */
        pairs = mncf_pack_pairs(old, fresh, &err);
        CHECK(pairs != NULL && pairs->count == 2 && strcmp(pairs->items[0].from, "a.mncf") == 0 &&
              strcmp(pairs->items[0].to, "a2.mncf") == 0 && strcmp(pairs->items[1].from, "b.mncf") == 0 &&
              strcmp(pairs->items[1].to, "c.mncf") == 0);
        mncf_pairs_free(pairs);
        /* the other way, the entries of the old pack have nothing to follow */
        pairs = mncf_pack_pairs(fresh, old, &err);
        CHECK(pairs != NULL && pairs->count == 2 && strcmp(pairs->items[0].to, "a.mncf") == 0 &&
              pairs->items[1].to == NULL);
        mncf_pairs_free(pairs);
        mncf_pack_free(fresh);
    }
    CHECK(mncf_pack_pairs(old, NULL, &err) == NULL && err.status == MNCF_ERR_ARGUMENT);
    u = mncf_pack_update_check(old, old_zip, old_len, NULL, NULL, &err);
    CHECK(u != NULL && strcmp(mncf_update_info_of(u)->reason, "update:version") == 0);
    mncf_update_free(u);
    u = mncf_pack_update_check(old, "PK", 2, NULL, NULL, &err);
    CHECK(u != NULL && strcmp(mncf_update_info_of(u)->reason, "update:error:zip") == 0 &&
          mncf_update_info_of(u)->pack == NULL);
    mncf_update_free(u);
    mncf_pack_free(old);
    free(old_zip);
    free(new_zip);
}

/* ---- Every allocation failing in turn --------------------------------------------------------------- */

/* An allocator that fails at one allocation and checks that every free gives the size it was given. */
typedef struct counting {
    long fail_at;                       /* the allocation that fails, from 1; 0 for none */
    long count;
    long live;
    size_t bytes;
    int size_mismatch;
} counting;

typedef struct header {
    size_t size;
    size_t pad;
} header;

static void *counting_alloc(void *ctx, void *ptr, size_t old_size, size_t new_size)
{
    counting *c = (counting *)ctx;
    header *h = NULL;
    if (ptr != NULL) {
        h = (header *)ptr - 1;
        if (h->size != old_size) {
            c->size_mismatch = 1;
        }
    }
    if (new_size == 0) {
        if (ptr != NULL) {
            c->live--;
            c->bytes -= h->size;
            free(h);
        }
        return NULL;
    }
    c->count++;
    if (c->fail_at != 0 && c->count == c->fail_at) {
        return NULL;
    }
    if (ptr != NULL) {
        size_t old = h->size;
        header *g = (header *)realloc(h, sizeof(header) + new_size);
        if (g == NULL) {
            return NULL;
        }
        g->size = new_size;
        c->bytes = c->bytes - old + new_size;
        return g + 1;
    }
    h = (header *)malloc(sizeof(header) + new_size);
    if (h == NULL) {
        return NULL;
    }
    h->size = new_size;
    c->live++;
    c->bytes += new_size;
    return h + 1;
}

/* Checks a new version of a file and of a pack with the allocator, marks, moves what the player set and
 * pairs the files; 1 when every step worked. */
static int run_updates(const mncf_allocator *a)
{
    mncf_error err;
    mncf_file *old = mncf_load(UPDATE_OLD, strlen(UPDATE_OLD), NULL, a, &err), *fresh;
    mncf_pack *old_pack, *new_pack;
    mncf_update *u;
    mncf_marks *m;
    mncf_carried *c;
    mncf_pairs *pairs;
    const char *names[] = {"money", "speed"};
    const char *out[2];
    mncf_choice_value values[1];
    mncf_choices ch;
    uint8_t *old_zip, *new_zip;
    size_t old_len, new_len;
    int ok = 1;
    if (old == NULL) {
        return 0;
    }
    u = mncf_update_check(old, UPDATE_NEW, strlen(UPDATE_NEW), NULL, a, &err);
    fresh = mncf_update_take_file(u);
    ok &= fresh != NULL;
    mncf_update_free(u);
    if (fresh != NULL) {
        m = mncf_mark(fresh, "1", &err);
        ok &= m != NULL;
        mncf_marks_free(m);
        ok &= mncf_ids(fresh, "1", names, 2, out, &err) == MNCF_OK;
        values[0].key = "money.count";
        values[0].value = mncf_number_int(900);
        memset(&ch, 0, sizeof ch);
        ch.on = names;
        ch.on_count = 2;
        ch.values = values;
        ch.value_count = 1;
        c = mncf_carry(fresh, "1", &ch, "rich", names, 2, &err);
        ok &= c != NULL;
        mncf_carried_free(c);
        mncf_free(fresh);
    }
    mncf_free(old);
    old_zip = two_file_pack(PACK_OLD_HEAD, "a.mncf", UPDATE_OLD, "b.mncf", FILE_TEXT, &old_len);
    new_zip = two_file_pack(PACK_NEW_HEAD, "a2.mncf", UPDATE_NEW, "c.mncf", FILE_TEXT, &new_len);
    old_pack = mncf_pack_read(old_zip, old_len, NULL, a, &err);
    ok &= old_pack != NULL;
    if (old_pack != NULL) {
        u = mncf_pack_update_check(old_pack, new_zip, new_len, NULL, a, &err);
        new_pack = mncf_update_take_pack(u);
        ok &= new_pack != NULL;
        mncf_update_free(u);
        if (new_pack != NULL) {
            pairs = mncf_pack_pairs(old_pack, new_pack, &err);
            ok &= pairs != NULL;
            mncf_pairs_free(pairs);
            mncf_pack_free(new_pack);
        }
        mncf_pack_free(old_pack);
    }
    free(old_zip);
    free(new_zip);
    return ok;
}

/* Runs a file, an evaluation, an engine, a pack and updates with the allocator; 1 when every step
 * worked. */
static int run_all(const mncf_allocator *a)
{
    mncf_error err;
    world w;
    mncf_file *f;
    mncf_evaluation *ev;
    mncf_engine *eng;
    mncf_pass pass;
    mncf_trust *t;
    mncf_pack *p;
    uint8_t raw[600];
    size_t n = unhex(DEFLATED_PACK, raw, sizeof raw);
    const char *held[] = {"ZL"};
    int ok = 1;
    int verbose = getenv("MNCF_UNIT_VERBOSE") != NULL;
#define STEP(name)                                                                                               \
    do {                                                                                                         \
        if (verbose) {                                                                                           \
            printf("    %s\n", name);                                                                            \
        }                                                                                                        \
    } while (0)
    world_init(&w);
    STEP("trust");
    t = mncf_trust_new(a);
    if (t == NULL) {
        return 0;
    }
    STEP("load");
    f = mncf_load(ENGINE_TEXT, strlen(ENGINE_TEXT), t, a, &err);
    if (f == NULL) {
        mncf_trust_free(t);
        return 0;
    }
    STEP("evaluate");
    ev = mncf_evaluate(f, &w.rt, NULL, &err);
    ok &= ev != NULL;
    mncf_evaluation_free(ev);
    STEP("engine");
    eng = mncf_engine_new(f, &w.rt, NULL, 60, &err);
    if (eng != NULL) {
        STEP("switch");
        ok &= mncf_engine_switch(eng, "speed", 1, NULL) == MNCF_OK;
        STEP("hold");
        ok &= mncf_engine_hold(eng, held, 1) == MNCF_OK;
        STEP("tick");
        ok &= mncf_engine_tick(eng, NULL, NULL, NULL, &pass) == MNCF_OK;
        STEP("set value");
        ok &= mncf_engine_set_value(eng, "money.count", mncf_number_int(3)) == MNCF_OK;
        STEP("press");
        ok &= mncf_engine_press(eng, "ZL+R") == MNCF_OK;
        STEP("tick");
        ok &= mncf_engine_tick(eng, NULL, NULL, NULL, &pass) == MNCF_OK;
        STEP("stop");
        ok &= mncf_engine_stop(eng, &pass) == MNCF_OK;
        STEP("free");
        mncf_engine_free(eng);
    } else {
        ok = 0;
    }
    STEP("pack");
    p = mncf_pack_read(raw, n, t, a, &err);
    ok &= p != NULL;
    mncf_pack_free(p);
    STEP("updates");
    ok &= run_updates(a);
    mncf_free(f);
    mncf_trust_free(t);
#undef STEP
    return ok;
}

static void test_allocation_failures(void)
{
    counting c;
    mncf_allocator a;
    long total, k;
    int leaks = 0, mismatches = 0;
    memset(&c, 0, sizeof c);
    a.fn = counting_alloc;
    a.ctx = &c;
    CHECK(run_all(&a));
    CHECK(c.live == 0 && c.bytes == 0 && !c.size_mismatch);
    total = c.count;
    CHECK(total > 10);
    for (k = 1; k <= total; k++) {
        memset(&c, 0, sizeof c);
        c.fail_at = k;
        if (getenv("MNCF_UNIT_VERBOSE") != NULL) {
            printf("  allocation %ld fails\n", k);
        }
        run_all(&a);
        if (c.live != 0 || c.size_mismatch) {
            printf("  when allocation %ld fails: %ld blocks left, sizes %s\n", k, c.live,
                   c.size_mismatch ? "differ" : "fit");
        }
        leaks += c.live != 0;
        mismatches += c.size_mismatch;
    }
    CHECK(leaks == 0);
    CHECK(mismatches == 0);
    printf("unit: %ld allocations failed in turn\n", total);
}

/* ---- The suite's own files through the API ---------------------------------------------------------- */

/* A file of the suite, read with malloc; NULL where it cannot be read. */
static char *suite_file(const char *suite, const char *name, size_t *size)
{
    char path[1024];
    FILE *fp;
    long n;
    char *data;
    snprintf(path, sizeof path, "%s/%s", suite, name);
    fp = fopen(path, "rb");
    if (fp == NULL) {
        return NULL;
    }
    fseek(fp, 0, SEEK_END);
    n = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    data = (char *)malloc((size_t)n + 1);
    if (data != NULL && fread(data, 1, (size_t)n, fp) != (size_t)n) {
        free(data);
        data = NULL;
    }
    fclose(fp);
    *size = (size_t)n;
    return data;
}

/* The lines that verified, with a trusted key or a key of the file's signers (sections 12 and 17.2). */
static void test_verified_lines(const char *suite)
{
    static const char *const NAMES[] = {"updates/files/v5-both.mncf", "updates/files/v5-one.mncf",
                                        "updates/files/v5-one-bare.mncf", "updates/files/v5-one-changed.mncf"};
    char *data[4];
    size_t size[4], i;
    mncf_error err;
    mncf_file *f[4];
    mncf_trust *t = mncf_trust_new(NULL);
    uint8_t one[32], two[32];
    mncf_update *u;
    unhex(RFC8032[0][0], one, 32);
    unhex(RFC8032[1][0], two, 32);
    for (i = 0; i < 4; i++) {
        data[i] = suite_file(suite, NAMES[i], &size[i]);
        CHECK(data[i] != NULL);
        f[i] = data[i] != NULL ? mncf_load(data[i], size[i], NULL, NULL, &err) : NULL;
        CHECK(f[i] != NULL);
    }
    if (f[0] != NULL && f[1] != NULL && f[2] != NULL && f[3] != NULL) {
        const mncf_signature *s = &mncf_file_info_of(f[0])->signature;
        CHECK(s->result == MNCF_SIGNATURE_UNKNOWN_KEY && s->verified_count == 2 &&
              strcmp(s->verified_ids[0], "21fe31dfa154a261") == 0 && memcmp(s->verified_keys[0], one, 32) == 0 &&
              strcmp(s->verified_ids[1], "39f713d0a644253f") == 0 && memcmp(s->verified_keys[1], two, 32) == 0);
        s = &mncf_file_info_of(f[2])->signature;
        CHECK(s->result == MNCF_SIGNATURE_UNKNOWN_KEY && s->verified_count == 0 && s->key_id_count == 1);
        s = &mncf_file_info_of(f[3])->signature;
        CHECK(s->result == MNCF_SIGNATURE_INVALID && s->verified_count == 0 && s->key_id_count == 1);
        /* a line whose key is trusted verifies with that key, and also counts for step 6 */
        mncf_trust_add(t, "Test key one", one);
        mncf_free(f[1]);
        f[1] = mncf_load(data[1], size[1], t, NULL, &err);
        s = f[1] != NULL ? &mncf_file_info_of(f[1])->signature : NULL;
        CHECK(s != NULL && s->result == MNCF_SIGNATURE_VERIFIED && s->signer_count == 1 && s->verified_count == 1 &&
              memcmp(s->verified_keys[0], one, 32) == 0);
        u = mncf_update_check(f[2], data[0], size[0], NULL, NULL, &err);
        CHECK(u != NULL && strcmp(mncf_update_info_of(u)->reason, "update:version") == 0);
        mncf_update_free(u);
    }
    for (i = 0; i < 4; i++) {
        mncf_free(f[i]);
        free(data[i]);
    }
    mncf_trust_free(t);
}

static void test_suite_files(const char *suite)
{
    char path[1024];
    FILE *fp;
    long n;
    char *data;
    mncf_error err;
    mncf_file *f;
    snprintf(path, sizeof path, "%s/cases/signed-one.mncf", suite);
    fp = fopen(path, "rb");
    CHECK(fp != NULL);
    if (fp == NULL) {
        return;
    }
    fseek(fp, 0, SEEK_END);
    n = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    data = (char *)malloc((size_t)n);
    CHECK(fread(data, 1, (size_t)n, fp) == (size_t)n);
    fclose(fp);
    f = mncf_load(data, (size_t)n, NULL, NULL, &err);
    CHECK(f != NULL && mncf_file_info_of(f)->signature.result == MNCF_SIGNATURE_UNKNOWN_KEY &&
          mncf_file_info_of(f)->signature.key_id_count == 1 &&
          strcmp(mncf_file_info_of(f)->signature.key_ids[0], "21fe31dfa154a261") == 0);
    mncf_free(f);
    free(data);
    test_verified_lines(suite);
}

/* Runs a test and says how it went. */
static void run(void (*test)(void), const char *name)
{
    int before = failures;
    test();
    printf("%s %s\n", failures == before ? "ok  " : "FAIL", name);
}

#define RUN(test) run(test, #test)

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    RUN(test_sha256);
    RUN(test_sha512);
    RUN(test_base64_crc);
    RUN(test_ed25519);
    RUN(test_inflate);
    RUN(test_zip);
    RUN(test_packs);
    RUN(test_toml);
    RUN(test_numbers);
    RUN(test_encode);
    RUN(test_trust);
    RUN(test_signatures);
    RUN(test_api);
    RUN(test_sections);
    RUN(test_runtime_gaps);
    RUN(test_update_forms);
    RUN(test_update_keys);
    RUN(test_update_check);
    RUN(test_update_settings);
    RUN(test_update_packs);
    RUN(test_allocation_failures);
    if (argc > 1) {
        test_suite_files(argv[1]);
    }
    printf("unit: %d checks, %d failed\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
