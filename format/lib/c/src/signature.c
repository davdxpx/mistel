/* signature.c: signature lines and trust lists (spec, section 12). MIT License. */
#include "internal.h"

#include <stdio.h>

static const char PREFIX[] = "# mncf-signature: ";
#define PREFIX_LEN (sizeof PREFIX - 1)

/* ---- Trust lists ------------------------------------------------------------------------------------ */

mncf_trust *mncf_trust_new(const mncf_allocator *alloc)
{
    mn_alloc a = mn_alloc_of(alloc);
    mncf_trust *t = (mncf_trust *)mn_calloc(&a, sizeof *t);
    if (t != NULL) {
        t->alloc = a;
    }
    return t;
}

void mncf_trust_free(mncf_trust *t)
{
    size_t i;
    mn_alloc a;
    if (t == NULL) {
        return;
    }
    a = t->alloc;
    for (i = 0; i < t->key_count; i++) {
        mn_free(&a, t->keys[i].name, strlen(t->keys[i].name) + 1);
    }
    mn_free(&a, t->keys, t->key_cap * sizeof *t->keys);
    mn_free(&a, t->revoked, t->revoked_cap * sizeof *t->revoked);
    mn_free(&a, t, sizeof *t);
}

mncf_status mncf_trust_add(mncf_trust *t, const char *name, const uint8_t public_key[32])
{
    mn_trust_key *k;
    size_t n;
    if (t == NULL || name == NULL || public_key == NULL) {
        return MNCF_ERR_ARGUMENT;
    }
    if (t->key_count == t->key_cap) {
        size_t next = t->key_cap == 0 ? 4 : t->key_cap * 2;
        mn_trust_key *grown = (mn_trust_key *)mn_realloc(&t->alloc, t->keys, t->key_cap * sizeof *grown,
                                                         next * sizeof *grown);
        if (grown == NULL) {
            return MNCF_ERR_MEMORY;
        }
        t->keys = grown;
        t->key_cap = next;
    }
    k = &t->keys[t->key_count];
    n = strlen(name);
    k->name = (char *)mn_malloc(&t->alloc, n + 1);
    if (k->name == NULL) {
        return MNCF_ERR_MEMORY;
    }
    memcpy(k->name, name, n + 1);
    memcpy(k->pub, public_key, 32);
    mncf_key_id(public_key, k->id);
    t->key_count++;
    return MNCF_OK;
}

mncf_status mncf_trust_revoke(mncf_trust *t, const char *key_id)
{
    size_t i, n;
    if (t == NULL || key_id == NULL) {
        return MNCF_ERR_ARGUMENT;
    }
    n = strlen(key_id);
    if (n > 16) {
        /* Ids longer than 16 characters name no key; they cannot match a line. */
        return MNCF_OK;
    }
    if (t->revoked_count == t->revoked_cap) {
        size_t next = t->revoked_cap == 0 ? 4 : t->revoked_cap * 2;
        char(*grown)[17] = (char(*)[17])mn_realloc(&t->alloc, t->revoked, t->revoked_cap * sizeof *grown,
                                                   next * sizeof *grown);
        if (grown == NULL) {
            return MNCF_ERR_MEMORY;
        }
        t->revoked = grown;
        t->revoked_cap = next;
    }
    for (i = 0; i < n; i++) {
        char c = key_id[i];
        t->revoked[t->revoked_count][i] = (char)(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c);
    }
    t->revoked[t->revoked_count][n] = 0;
    t->revoked_count++;
    return MNCF_OK;
}

static int is_revoked(const mncf_trust *t, const char *id)
{
    size_t i;
    if (t == NULL) {
        return 0;
    }
    for (i = 0; i < t->revoked_count; i++) {
        if (strcmp(t->revoked[i], id) == 0) {
            return 1;
        }
    }
    return 0;
}

static int lower_hex16(const char *s, size_t n)
{
    size_t i;
    if (n != 16) {
        return 0;
    }
    for (i = 0; i < n; i++) {
        if (!((s[i] >= '0' && s[i] <= '9') || (s[i] >= 'a' && s[i] <= 'f'))) {
            return 0;
        }
    }
    return 1;
}

/* Whether the text is the base64 of exactly 32 bytes, written as base64 writes them; out gets them. */
static int public_key_of(const char *s, size_t n, uint8_t out[32])
{
    char again[48];
    long got;
    if (n != 44) {
        return 0;
    }
    got = mn_base64_decode(s, n, out, 32);
    if (got != 32) {
        return 0;
    }
    mn_base64_encode(out, 32, again);
    return memcmp(again, s, 44) == 0;
}

mncf_trust *mncf_trust_read(const char *text, size_t size, const mncf_allocator *alloc, mncf_error *err)
{
    mn_arena ar;
    const mncf_toml *doc, *keys, *revoked;
    mncf_trust *trust = NULL;
    size_t i;
    int r;
    mn_error_clear(err);
    mn_arena_init(&ar, mn_alloc_of(alloc));
    doc = mn_toml_parse(&ar, text != NULL ? text : "", text != NULL ? size : 0, err);
    if (doc == NULL) {
        goto done;
    }
    r = mn_toml_check_depth(doc, 0);
    if (r != 0) {
        mn_error(err, MNCF_ERR_TOML, "toml", r == 1 ? "arrays and tables nest deeper than 128 levels"
                                                    : "the format has no date or time values");
        goto done;
    }
    if (doc->u.t.n == 0 || !mn_streqz(doc->u.t.e[0].key.p, doc->u.t.e[0].key.n, "format") ||
        !mn_toml_is(doc->u.t.e[0].val, MNCF_TOML_STRING) ||
        !mn_streqz(doc->u.t.e[0].val->u.s.p, doc->u.t.e[0].val->u.s.n, "mncf-keys/1")) {
        mn_error(err, MNCF_ERR_FORMAT, "format", "`format = \"mncf-keys/1\"` must be the first key of a trust list");
        goto done;
    }
    trust = mncf_trust_new(alloc);
    if (trust == NULL) {
        mn_error_nomem(err);
        goto done;
    }
    keys = mn_toml_getz(doc, "key");
    revoked = mn_toml_getz(doc, "revoked");
    if (keys != NULL) {
        int good = mn_toml_is(keys, MNCF_TOML_ARRAY);
        for (i = 0; good && i < keys->u.a.n; i++) {
            good = mn_toml_is(keys->u.a.items[i], MNCF_TOML_TABLE);
        }
        if (!good) {
            mn_error(err, MNCF_ERR_SCHEMA, "schema:key", "`key` must be an array of tables");
            goto fail;
        }
        for (i = 0; i < keys->u.a.n; i++) {
            const mncf_toml *k = keys->u.a.items[i];
            const mncf_toml *name = mn_toml_getz(k, "name");
            const mncf_toml *pub = mn_toml_getz(k, "public");
            const mncf_toml *id = mn_toml_getz(k, "id");
            uint8_t raw[32];
            char kid[17];
            if (!mn_toml_is(name, MNCF_TOML_STRING) || name->u.s.n == 0 || !mn_toml_is(pub, MNCF_TOML_STRING) ||
                !public_key_of(pub->u.s.p, pub->u.s.n, raw)) {
                mn_error(err, MNCF_ERR_SCHEMA, "schema:key",
                         "a key needs a `name` and a `public` key of 32 bytes in base64");
                goto fail;
            }
            mncf_key_id(raw, kid);
            if (id != NULL && !(mn_toml_is(id, MNCF_TOML_STRING) && mn_streqz(id->u.s.p, id->u.s.n, kid))) {
                mn_error(err, MNCF_ERR_SCHEMA, "schema:key", "the key id of %s is %s, not the one given",
                         name->u.s.p, kid);
                goto fail;
            }
            if (mncf_trust_add(trust, name->u.s.p, raw) != MNCF_OK) {
                mn_error_nomem(err);
                goto fail;
            }
        }
    }
    if (revoked != NULL) {
        int good = mn_toml_is(revoked, MNCF_TOML_ARRAY);
        for (i = 0; good && i < revoked->u.a.n; i++) {
            good = mn_toml_is(revoked->u.a.items[i], MNCF_TOML_TABLE);
        }
        if (!good) {
            mn_error(err, MNCF_ERR_SCHEMA, "schema:revoked", "`revoked` must be an array of tables");
            goto fail;
        }
        for (i = 0; i < revoked->u.a.n; i++) {
            const mncf_toml *id = mn_toml_getz(revoked->u.a.items[i], "id");
            if (!mn_toml_is(id, MNCF_TOML_STRING) || !lower_hex16(id->u.s.p, id->u.s.n)) {
                mn_error(err, MNCF_ERR_SCHEMA, "schema:revoked",
                         "a revoked key needs an `id` of 16 lower-case hexadecimal digits");
                goto fail;
            }
            if (mncf_trust_revoke(trust, id->u.s.p) != MNCF_OK) {
                mn_error_nomem(err);
                goto fail;
            }
        }
    }
    goto done;
fail:
    mncf_trust_free(trust);
    trust = NULL;
done:
    mn_arena_free(&ar);
    return trust;
}

/* ---- Signature lines -------------------------------------------------------------------------------- */

static int starts_with_prefix(const uint8_t *p, size_t n)
{
    return n >= PREFIX_LEN && memcmp(p, PREFIX, PREFIX_LEN) == 0;
}

size_t mn_signature_split(const uint8_t *data, size_t n)
{
    size_t i;
    if (starts_with_prefix(data, n)) {
        return 0;
    }
    for (i = 0; i < n; i++) {
        if (data[i] == '\n' && starts_with_prefix(data + i + 1, n - i - 1)) {
            return i + 1;
        }
    }
    return n;
}

/* The result of one line: a well-formed ed25519 line gives its key id and signature. */
enum { LINE_INVALID, LINE_OTHER, LINE_ED25519 };

static int parse_line(const uint8_t *p, size_t n, char kid[17], uint8_t sig[64], int *canonical)
{
    size_t i;
    const uint8_t *rest = p + PREFIX_LEN;
    size_t m = n - PREFIX_LEN;
    *canonical = 0;
    if (m == 8 + 16 + 1 + 88 && memcmp(rest, "ed25519 ", 8) == 0 && rest[24] == ' ') {
        int ok = 1;
        for (i = 0; i < 16 && ok; i++) {
            uint8_t c = rest[8 + i];
            ok = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
        }
        for (i = 0; i < 86 && ok; i++) {
            uint8_t c = rest[25 + i];
            ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '+' ||
                 c == '/';
        }
        if (ok && rest[111] == '=' && rest[112] == '=') {
            char again[96];
            memcpy(kid, rest + 8, 16);
            kid[16] = 0;
            if (mn_base64_decode((const char *)rest + 25, 88, sig, 64) == 64) {
                mn_base64_encode(sig, 64, again);
                *canonical = memcmp(again, rest + 25, 88) == 0;
            }
            return LINE_ED25519;
        }
    }
    /* Another algorithm: lower-case letters, digits and '-', then a space. */
    for (i = 0; i < m; i++) {
        uint8_t c = rest[i];
        if (c == ' ') {
            break;
        }
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-')) {
            return LINE_INVALID;
        }
    }
    if (i == 0 || i == m) {
        return LINE_INVALID;
    }
    if (i == 7 && memcmp(rest, "ed25519", 7) == 0) {
        return LINE_INVALID;
    }
    return LINE_OTHER;
}

static int push_str(mn_arena *ar, const char ***items, size_t *count, size_t *cap, const char *s)
{
    const char **list = *items;
    char *copy = mn_arena_strdup(ar, s);
    if (copy == NULL || MN_GROW(ar, list, *count, *cap) == NULL) {
        return 0;
    }
    list[(*count)++] = copy;
    *items = list;
    return 1;
}

int mn_signature_check(const uint8_t *data, size_t n, const mncf_trust *trust, mn_arena *ar,
                       mncf_signature *sig)
{
    size_t start = mn_signature_split(data, n);
    size_t pos;
    int seen[5] = {0, 0, 0, 0, 0};
    const char **signers = NULL, **kids = NULL;
    size_t signer_count = 0, signer_cap = 0, kid_count = 0, kid_cap = 0;
    memset(sig, 0, sizeof *sig);
    sig->signed_size = start;
    sig->result = MNCF_SIGNATURE_UNSIGNED;
    if (start == n) {
        return 1;
    }
    /* Only signature lines may follow the first one; an empty last line is the file's last line end. */
    for (pos = start; pos < n; pos++) {
        size_t line_end = pos;
        while (line_end < n && data[line_end] != '\n') {
            line_end++;
        }
        if (!starts_with_prefix(data + pos, line_end - pos)) {
            sig->result = MNCF_SIGNATURE_INVALID;
            return 1;
        }
        pos = line_end;
    }
    for (pos = start; pos < n; pos++) {
        size_t line_end = pos;
        char kid[17];
        uint8_t raw[64];
        int canonical;
        int kind;
        while (line_end < n && data[line_end] != '\n') {
            line_end++;
        }
        kind = parse_line(data + pos, line_end - pos, kid, raw, &canonical);
        if (kind == LINE_OTHER) {
            seen[MNCF_SIGNATURE_UNKNOWN_KEY] = 1;
        } else if (kind == LINE_INVALID) {
            seen[MNCF_SIGNATURE_INVALID] = 1;
        } else {
            size_t i;
            int any = 0, good = 0;
            if (!push_str(ar, &kids, &kid_count, &kid_cap, kid)) {
                return 0;
            }
            if (!canonical) {
                seen[MNCF_SIGNATURE_INVALID] = 1;
            } else if (is_revoked(trust, kid)) {
                seen[MNCF_SIGNATURE_REVOKED] = 1;
            } else {
                for (i = 0; trust != NULL && i < trust->key_count; i++) {
                    if (strcmp(trust->keys[i].id, kid) != 0) {
                        continue;
                    }
                    any = 1;
                    if (mncf_ed25519_verify(trust->keys[i].pub, data, start, raw)) {
                        good = 1;
                        if (!push_str(ar, &signers, &signer_count, &signer_cap, trust->keys[i].name)) {
                            return 0;
                        }
                    }
                }
                if (!any) {
                    seen[MNCF_SIGNATURE_UNKNOWN_KEY] = 1;
                } else {
                    seen[good ? MNCF_SIGNATURE_VERIFIED : MNCF_SIGNATURE_INVALID] = 1;
                }
            }
        }
        pos = line_end;
    }
    sig->key_ids = kids;
    sig->key_id_count = kid_count;
    if (seen[MNCF_SIGNATURE_INVALID]) {
        sig->result = MNCF_SIGNATURE_INVALID;
    } else if (seen[MNCF_SIGNATURE_REVOKED]) {
        sig->result = MNCF_SIGNATURE_REVOKED;
    } else if (seen[MNCF_SIGNATURE_VERIFIED]) {
        sig->result = MNCF_SIGNATURE_VERIFIED;
        sig->signers = signers;
        sig->signer_count = signer_count;
    } else if (seen[MNCF_SIGNATURE_UNKNOWN_KEY]) {
        sig->result = MNCF_SIGNATURE_UNKNOWN_KEY;
    }
    return 1;
}
