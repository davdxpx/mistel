/* evaluate.c: identifies the build and gives every cheat its state and reason (spec, sections 5, 6, 7.8,
 * 11 and Appendix B). MIT License. */
#include "evaluate.h"

#include <stdlib.h>

static const char *const CAP_NAMES[] = {"guest-read", "guest-write", "code-bytes", "code-hook", "buttons", "display"};

/* ---- Strings that live as long as the evaluator ----------------------------------------------------- */

const char *mn_intern(mn_eval *ev, const char *prefix, const char *name)
{
    size_t pn = strlen(prefix), nn = strlen(name), i;
    char *s;
    for (i = 0; i < ev->interned_count; i++) {
        const char *x = ev->interned[i];
        if (strncmp(x, prefix, pn) == 0 && strcmp(x + pn, name) == 0) {
            return x;
        }
    }
    s = (char *)mn_arena_alloc(ev->keep, pn + nn + 1);
    if (s == NULL || MN_GROW(ev->keep, ev->interned, ev->interned_count, ev->interned_cap) == NULL) {
        ev->failed = 1;
        return "";
    }
    memcpy(s, prefix, pn);
    memcpy(s + pn, name, nn + 1);
    ev->interned[ev->interned_count++] = s;
    return s;
}

/* ---- The runtime ------------------------------------------------------------------------------------ */

static int module_size(const mncf_module *m, uint64_t *size)
{
    if (m->has_size) {
        *size = m->size;
        return 1;
    }
    if (m->data != NULL) {
        *size = m->data_size;
        return 1;
    }
    return 0;
}

int mn_runtime_read(const mncf_runtime *rt, uint64_t address, uint8_t *out, size_t size)
{
    size_t i;
    if (rt->read != NULL) {
        return rt->read(rt->read_ctx, address, out, size) ? 1 : 0;
    }
    for (i = 0; i < rt->module_count; i++) {
        const mncf_module *m = &rt->modules[i];
        if (m->data != NULL && address >= m->base && address - m->base <= m->data_size &&
            size <= m->data_size - (size_t)(address - m->base)) {
            if (size > 0) {
                memcpy(out, m->data + (address - m->base), size);
            }
            return 1;
        }
    }
    return 0;
}

static int rt_reader(void *ctx, uint64_t address, uint8_t *out, size_t size)
{
    return mn_runtime_read((const mncf_runtime *)ctx, address, out, size);
}

/* Whether the runtime can read size bytes there. */
static int readable(mn_eval *ev, uint64_t address, size_t size)
{
    uint8_t small[256];
    uint8_t *buf = small;
    int ok;
    if (size > sizeof small) {
        buf = (uint8_t *)mn_malloc(&ev->alloc, size);
        if (buf == NULL) {
            ev->failed = 1;
            return 0;
        }
    }
    ok = mn_runtime_read(&ev->rt, address, buf, size);
    if (buf != small) {
        mn_free(&ev->alloc, buf, size);
    }
    return ok;
}

static const mncf_module *find_module(const mn_eval *ev, const char *name, size_t n)
{
    size_t i;
    for (i = 0; i < ev->rt.module_count; i++) {
        const char *m = ev->rt.modules[i].name;
        if (m != NULL && mn_streqz(name, n, m)) {
            return &ev->rt.modules[i];
        }
    }
    return NULL;
}

static const mncf_region *find_region(const mn_eval *ev, const char *name, size_t n)
{
    size_t i;
    for (i = 0; i < ev->rt.region_count; i++) {
        const char *r = ev->rt.regions[i].name;
        if (r != NULL && mn_streqz(name, n, r)) {
            return &ev->rt.regions[i];
        }
    }
    return NULL;
}

static int has_name(const char *const *list, size_t count, const char *name)
{
    size_t i;
    for (i = 0; i < count; i++) {
        if (list[i] != NULL && strcmp(list[i], name) == 0) {
            return 1;
        }
    }
    return 0;
}

static int has_extension(const mn_eval *ev, const char *name)
{
    return has_name(ev->rt.extensions, ev->rt.extension_count, name);
}

static int has_syntax(const mn_eval *ev, const char *name)
{
    return has_name(ev->rt.raw_syntaxes, ev->rt.raw_syntax_count, name);
}

/* The key of the build that runs, or NULL (section 5). */
static const char *identify(const mn_eval *ev)
{
    const mncf_file_info *info = &ev->f->info;
    size_t i, k;
    if (ev->rt.build != NULL) {
        for (i = 0; i < info->build_count; i++) {
            if (strcmp(info->builds[i].key, ev->rt.build) == 0) {
                return info->builds[i].key;
            }
        }
        return NULL;
    }
    for (i = 0; i < info->build_count; i++) {
        const mncf_build *b = &info->builds[i];
        int all = 1;
        for (k = 0; k < b->module_count && all; k++) {
            const mncf_module *m = find_module(ev, b->modules[k].name, strlen(b->modules[k].name));
            all = m != NULL && m->id != NULL &&
                  mn_platform_module_match(ev->f->platform, b->modules[k].id, strlen(b->modules[k].id), m->id,
                                           strlen(m->id));
        }
        if (all) {
            return b->key;
        }
    }
    return NULL;
}

int mn_eval_init(mn_eval *ev, const mncf_file *f, const mncf_runtime *rt, mn_alloc alloc, mn_arena *keep)
{
    memset(ev, 0, sizeof *ev);
    ev->f = f;
    ev->rt = *rt;
    ev->alloc = alloc;
    ev->keep = keep;
    ev->build = identify(ev);
    if (rt->arch != NULL) {
        ev->arch = rt->arch;
    } else {
        ev->arch = f->platform->archs[0];
    }
    ev->arch_len = strlen(ev->arch);
    return 1;
}

/* ---- Results ---------------------------------------------------------------------------------------- */

static mn_result *new_result(mn_eval *ev)
{
    mn_result *r = (mn_result *)mn_calloc(&ev->alloc, sizeof *r);
    if (r == NULL) {
        ev->failed = 1;
        return NULL;
    }
    r->reason = "";
    return r;
}

void mn_result_free(mn_eval *ev, mn_result *r)
{
    if (r == NULL) {
        return;
    }
    mn_free(&ev->alloc, r->plans, r->plans_size);
    mn_free(&ev->alloc, r->ops, r->ops_size);
    mn_free(&ev->alloc, r, sizeof *r);
}

mn_result *mn_refused(mn_eval *ev, const char *reason)
{
    mn_result *r = new_result(ev);
    if (r != NULL) {
        r->state = MNCF_STATE_REFUSED;
        r->reason = reason;
    }
    return r;
}

static int lower_eq(const char *a, const char *b)
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

static const char *file_reason(mn_eval *ev)
{
    const mncf_file_info *info = &ev->f->info;
    size_t i;
    if (ev->rt.platform == NULL || strcmp(ev->rt.platform, info->platform) != 0 ||
        (ev->rt.game_id != NULL && !lower_eq(ev->rt.game_id, info->game_id))) {
        return "game-mismatch";
    }
    if (info->signature.result == MNCF_SIGNATURE_INVALID || info->signature.result == MNCF_SIGNATURE_REVOKED) {
        return "signature";
    }
    for (i = 0; i < info->required_count; i++) {
        if (!has_extension(ev, info->required[i])) {
            return mn_intern(ev, "extension-missing:", info->required[i]);
        }
    }
    return NULL;
}

/* ---- Values the player chose ------------------------------------------------------------------------ */

/* The value the player chose, or the default; 0 where a chosen value is not one the player can choose. */
static int player_value(const mn_eval *ev, const mn_cheat *c, size_t ref, mn_num *out)
{
    const mncf_value_spec *s = &c->pub.values[ref];
    size_t vn = strlen(s->id);
    const mn_value *found = NULL;
    size_t i;
    for (i = 0; i < ev->value_count; i++) {
        const mn_value *v = &ev->values[i];
        if (v->key_len == c->id_len + 1 + vn && memcmp(v->key, c->pub.id, c->id_len) == 0 &&
            v->key[c->id_len] == '.' && memcmp(v->key + c->id_len + 1, s->id, vn) == 0) {
            found = v;
        }
    }
    if (found == NULL) {
        *out = s->default_value;
        return 1;
    }
    if (mn_value_fit(s, &found->value) != MN_FIT_OK) {
        return 0;
    }
    *out = found->value;
    return 1;
}

/* ---- Patterns --------------------------------------------------------------------------------------- */

/* A pattern's longest run of fixed bytes, where the search looks first. */
typedef struct anchor {
    size_t at;
    size_t len;
    uint8_t bytes[64];
} anchor;

static void find_anchor(const int16_t *items, size_t n, anchor *a)
{
    size_t i = 0;
    a->at = 0;
    a->len = 0;
    while (i < n) {
        size_t j = i;
        while (j < n && items[j] >= 0) {
            j++;
        }
        if (j - i > a->len) {
            a->at = i;
            a->len = j - i;
        }
        i = j + 1;
    }
    if (a->len > sizeof a->bytes) {
        a->len = sizeof a->bytes;
    }
    for (i = 0; i < a->len; i++) {
        a->bytes[i] = (uint8_t)items[a->at + i];
    }
}

/* The matches in [start, end) of the module's bytes, in order, until there are two. A pattern starts with
 * a fixed byte, so the anchor is never empty. */
static void search_range(const mncf_module *m, uint64_t start, uint64_t end, const int16_t *items, size_t n,
                         const anchor *a, uint64_t *found, size_t *count)
{
    const uint8_t *data = m->data;
    uint64_t h;
    if (end > m->data_size) {
        end = m->data_size;
    }
    if (start >= end || end - start < n) {
        return;
    }
    /* h is where the anchor is looked for: the match starts a->at before it */
    h = start + a->at;
    while (h - a->at + n <= end && *count < 2) {
        uint64_t last = end - n + a->at;          /* the last place the anchor can start */
        const uint8_t *hit = (const uint8_t *)memchr(data + h, a->bytes[0], (size_t)(last + 1 - h));
        uint64_t s;
        size_t k;
        int same = 0;
        if (hit == NULL) {
            return;
        }
        h = (uint64_t)(hit - data);
        s = h - a->at;
        if (memcmp(data + h, a->bytes, a->len) == 0) {
            for (k = 0; k < n; k++) {
                if (items[k] >= 0 && data[s + k] != (uint8_t)items[k]) {
                    break;
                }
            }
            if (k == n) {
                size_t j;
                for (j = 0; j < *count; j++) {
                    same |= found[j] == s;
                }
                if (!same) {
                    found[(*count)++] = s;
                }
            }
        }
        h++;
    }
}

/* Every module offset where the pattern matches, up to two (section 6.3). Without sections, the whole
 * module is code. */
static size_t find_all(const mncf_module *m, const mn_loc *loc, uint64_t *first)
{
    uint64_t found[2];
    size_t count = 0;
    uint64_t size;
    anchor a;
    if (m->data == NULL) {
        return 0;
    }
    find_anchor(loc->find, loc->find_len, &a);
    if (!m->has_code && !m->has_rodata && !m->has_data) {
        if ((loc->section == SEC_NONE || loc->section == SEC_CODE) && module_size(m, &size)) {
            search_range(m, 0, size, loc->find, loc->find_len, &a, found, &count);
        }
    } else if (loc->section != SEC_NONE) {
        if (loc->section == SEC_CODE && m->has_code) {
            search_range(m, m->code_start, m->code_end, loc->find, loc->find_len, &a, found, &count);
        } else if (loc->section == SEC_RODATA && m->has_rodata) {
            search_range(m, m->rodata_start, m->rodata_end, loc->find, loc->find_len, &a, found, &count);
        } else if (loc->section == SEC_DATA && m->has_data) {
            search_range(m, m->data_start, m->data_end, loc->find, loc->find_len, &a, found, &count);
        }
    } else {
        if (m->has_code) {
            search_range(m, m->code_start, m->code_end, loc->find, loc->find_len, &a, found, &count);
        }
        if (m->has_rodata && count < 2) {
            search_range(m, m->rodata_start, m->rodata_end, loc->find, loc->find_len, &a, found, &count);
        }
    }
    if (count > 0) {
        *first = found[0];
    }
    return count;
}

static size_t cache_slot(const mncf_module *m, const mn_loc *loc, size_t cap)
{
    uintptr_t a = (uintptr_t)m, b = (uintptr_t)loc;
    uint64_t h = (uint64_t)a * 0x9E3779B97F4A7C15ULL ^ (uint64_t)b * 0xC2B2AE3D27D4EB4FULL;
    return (size_t)(h ^ (h >> 29)) & (cap - 1);
}

static const char *search(mn_eval *ev, const mncf_module *m, const mn_loc *loc, uint64_t *at)
{
    size_t slot;
    mn_cache_entry *e;
    if (ev->cache_cap == 0 || (ev->cache_count + 1) * 2 > ev->cache_cap) {
        size_t cap = ev->cache_cap == 0 ? 64 : ev->cache_cap * 2, i;
        mn_cache_entry *grown = (mn_cache_entry *)mn_arena_zalloc(ev->keep, cap * sizeof *grown);
        if (grown == NULL) {
            ev->failed = 1;
            return "";
        }
        for (i = 0; i < ev->cache_cap; i++) {
            if (ev->cache[i].loc != NULL) {
                size_t s = cache_slot(ev->cache[i].module, ev->cache[i].loc, cap);
                while (grown[s].loc != NULL) {
                    s = (s + 1) & (cap - 1);
                }
                grown[s] = ev->cache[i];
            }
        }
        ev->cache = grown;
        ev->cache_cap = cap;
    }
    slot = cache_slot(m, loc, ev->cache_cap);
    for (;;) {
        e = &ev->cache[slot];
        if (e->loc == NULL) {
            e->module = m;
            e->loc = loc;
            e->count = find_all(m, loc, &e->at);
            ev->cache_count++;
            break;
        }
        if (e->loc == loc && e->module == m) {
            break;
        }
        slot = (slot + 1) & (ev->cache_cap - 1);
    }
    if (e->count == 0) {
        return "pattern-not-found";
    }
    if (e->count > 1) {
        return "pattern-ambiguous";
    }
    *at = e->at;
    return NULL;
}

/* ---- Places ----------------------------------------------------------------------------------------- */

static const mn_offset *offset_for(const mn_eval *ev, const mn_loc *loc)
{
    size_t i;
    if (ev->build == NULL) {
        return NULL;
    }
    for (i = 0; i < loc->offset_count; i++) {
        if (mn_streqz(loc->offsets[i].build.p, loc->offsets[i].build.n, ev->build)) {
            return &loc->offsets[i];
        }
    }
    return NULL;
}

/* The place of a location's base, checked, and the pointer path that follows it. */
static const char *target(mn_eval *ev, const mn_loc *loc, size_t size, mn_target *t)
{
    const mn_platform *plat = ev->f->platform;
    size_t need = loc->path_len > 0 ? plat->pointer_size : size;
    memset(t, 0, sizeof *t);
    if (loc->kind == LOC_ADDRESS) {
        t->base.address = loc->address;
        if (!readable(ev, loc->address, need)) {
            return "outside";
        }
    } else if (loc->kind == LOC_MODULE || loc->kind == LOC_MODULE_BUILDS || loc->kind == LOC_PATTERN) {
        const mncf_module *m = find_module(ev, loc->name.p, loc->name.n);
        uint64_t off, msize;
        if (m == NULL) {
            return "module-unknown";
        }
        if (loc->kind == LOC_MODULE) {
            off = loc->offset;
        } else if (loc->kind == LOC_MODULE_BUILDS) {
            const mn_offset *o = offset_for(ev, loc);
            if (o == NULL) {
                return "build-unknown";
            }
            off = o->offset;
        } else {
            uint64_t found;
            const char *reason = search(ev, m, loc, &found);
            if (reason != NULL) {
                return reason;
            }
            if (loc->find_offset < 0 && (uint64_t)0 - (uint64_t)loc->find_offset > found) {
                return "outside";
            }
            off = found + (uint64_t)loc->find_offset;
            if (loc->find_offset > 0 && off < found) {
                return "outside";
            }
        }
        if (module_size(m, &msize) && (off > msize || need > msize - off)) {
            return "outside";
        }
        t->base.address = m->base + off;
        t->base.module = m;
        t->base.module_name = loc->name.p;
        t->base.offset = off;
    } else {
        const mncf_region *r = find_region(ev, loc->name.p, loc->name.n);
        uint64_t off;
        if (r == NULL) {
            return "region-unknown";
        }
        if (loc->kind == LOC_REGION) {
            off = loc->offset;
        } else {
            const mn_offset *o = offset_for(ev, loc);
            if (o == NULL) {
                return "build-unknown";
            }
            off = o->offset;
        }
        if (off > r->size || need > r->size - off) {
            return "outside";
        }
        t->base.address = r->base + off;
    }
    t->path = loc->path;
    t->path_len = loc->path_len;
    t->size = size;
    return NULL;
}

static int read_scratch(mn_reader read, void *ctx, uint64_t address, size_t size, const mn_alloc *alloc)
{
    uint8_t small[256];
    uint8_t *buf = small;
    int ok;
    if (size > sizeof small) {
        buf = (uint8_t *)mn_malloc(alloc, size);
        if (buf == NULL) {
            return 0;
        }
    }
    ok = read(ctx, address, buf, size);
    if (buf != small) {
        mn_free(alloc, buf, size);
    }
    return ok;
}

int mn_follow(const mn_platform *plat, const mn_target *t, mn_reader read, void *ctx, const mn_alloc *alloc,
              mn_place *out)
{
    uint64_t address, mask;
    size_t i, k;
    if (t->path_len == 0) {
        *out = t->base;
        return 1;
    }
    address = t->base.address;
    mask = plat->pointer_size >= 8 ? ~(uint64_t)0 : (((uint64_t)1 << (8 * plat->pointer_size)) - 1);
    for (i = 0; i < t->path_len; i++) {
        uint8_t raw[8];
        uint64_t pointer = 0;
        if (!read(ctx, address, raw, plat->pointer_size)) {
            return 0;
        }
        for (k = 0; k < plat->pointer_size; k++) {
            size_t at = plat->big_endian ? plat->pointer_size - 1 - k : k;
            pointer |= (uint64_t)raw[at] << (8 * k);
        }
        if (pointer == 0) {
            return 0;
        }
        address = (pointer + t->path[i]) & mask;
    }
    if (!read_scratch(read, ctx, address, t->size, alloc)) {
        return 0;
    }
    memset(out, 0, sizeof *out);
    out->address = address;
    return 1;
}

/* Compares the bytes at a place with expect (section 7.4). */
static const char *check_expect(mn_eval *ev, const mn_place *place, const uint8_t *expect, size_t n,
                                const uint8_t *data)
{
    uint8_t small[256];
    uint8_t *buf = NULL;
    const uint8_t *actual = small;
    size_t got = n;
    const char *reason = NULL;
    if (place->module != NULL && place->module->data != NULL) {
        const mncf_module *m = place->module;
        size_t have = place->offset >= m->data_size ? 0 : (size_t)(m->data_size - place->offset);
        got = have < n ? have : n;
        actual = m->data + (got > 0 ? place->offset : 0);
    } else {
        uint8_t *into = small;
        if (n > sizeof small) {
            into = buf = (uint8_t *)mn_malloc(&ev->alloc, n);
            if (buf == NULL) {
                ev->failed = 1;
                return "";
            }
        }
        if (!mn_runtime_read(&ev->rt, place->address, into, n)) {
            reason = "outside";
        }
        actual = into;
    }
    if (reason == NULL && (got != n || memcmp(actual, expect, n) != 0) &&
        (data == NULL || got != n || memcmp(actual, data, n) != 0)) {
        reason = "expect-mismatch";
    }
    if (buf != NULL) {
        mn_free(&ev->alloc, buf, n);
    }
    return reason;
}

/* Whether a < b + len, exactly. */
static int below_sum(uint64_t a, uint64_t b, uint64_t len)
{
    return len > UINT64_MAX - b || a < b + len;
}

/* Refuses a patch or a stub over code the runtime rewrote for its own reasons (section 11.3). */
static const char *check_rewritten(const mn_eval *ev, const mn_place *place, size_t length)
{
    size_t i;
    if (place->module == NULL) {
        return NULL;
    }
    for (i = 0; i < ev->rt.rewritten_count; i++) {
        const mncf_rewritten *r = &ev->rt.rewritten[i];
        if (r->module != NULL && strcmp(r->module, place->module_name) == 0 &&
            below_sum(r->offset, place->offset, length) && below_sum(place->offset, r->offset, r->length)) {
            return "runtime:rewritten";
        }
    }
    return NULL;
}

/* The first part of the core in the spec's order that the cheat uses and the runtime does not have yet
 * (section 11.1), as a reason, or NULL. */
static const char *unsupported_part(mn_eval *ev, const mn_cheat *c)
{
    static const char *const FEATURES[] = {"run.once", "run.action", "run.timed", "run.monitor", "hold",
                                           "toggle", "press", "when", "when.many", "match.any",
                                           "trigger.enter", "interval", "override"};
    size_t conditions = 0, overrides = 0, i;
    int used[13];
    if (ev->rt.unsupported_count == 0) {
        return NULL;
    }
    for (i = 0; i < c->op_count; i++) {
        conditions += c->ops[i].kind == MNCF_OP_WHEN;
        overrides += c->ops[i].kind == MNCF_OP_OVERRIDE;
    }
    used[0] = c->pub.run == MNCF_RUN_ONCE;
    used[1] = c->pub.run == MNCF_RUN_ACTION;
    used[2] = c->pub.run == MNCF_RUN_TIMED;
    used[3] = c->pub.run == MNCF_RUN_MONITOR;
    used[4] = c->pub.hold != NULL;
    used[5] = c->pub.toggle != NULL;
    used[6] = c->pub.press != NULL;
    used[7] = conditions > 0;
    used[8] = conditions > 1;
    used[9] = c->pub.match_any && conditions > 0;
    used[10] = c->pub.trigger_enter && conditions > 0;
    used[11] = c->pub.interval != 1 && conditions > 0;
    used[12] = overrides > 0;
    for (i = 0; i < 13; i++) {
        if (used[i] && has_name(ev->rt.unsupported, ev->rt.unsupported_count, FEATURES[i])) {
            return mn_intern(ev, "runtime:unsupported:", FEATURES[i]);
        }
    }
    return NULL;
}

/* ---- Plans ------------------------------------------------------------------------------------------ */

static int by_bytes(const mn_eval *ev, const mn_op *op)
{
    return (ev->rt.capabilities & MNCF_CAP_CODE_BYTES) && op->has_expect && mn_arch_has_stub(ev->arch, ev->arch_len);
}

static const char *plan_op(mn_eval *ev, const mn_cheat *c, const mn_op *op, mn_plan *p)
{
    const char *reason;
    const int big = ev->f->platform->big_endian;
    memset(p, 0, sizeof *p);
    p->kind = op->kind;
    p->index = op->index;
    p->op = op;
    p->target_count = 1;
    switch (op->kind) {
    case MNCF_OP_PATCH:
        reason = target(ev, &op->at, op->expect_len, &p->targets[0]);
        if (reason != NULL) {
            return reason;
        }
        if (op->encode) {
            mn_num v = op->value;
            if (op->value_kind == VAL_REF && !player_value(ev, c, op->ref, &v)) {
                return "value-out-of-range";
            }
            p->data_len = mn_arm64_imm(op->insn, op->width, op->rd, &v, p->buf);
            if (p->data_len == 0) {
                return "value-out-of-range";
            }
            p->data = p->buf;
        } else {
            p->data = op->bytes;
            p->data_len = op->bytes_len;
        }
        p->expect = op->expect;
        p->expect_len = op->expect_len;
        if (op->at.path_len == 0) {
            reason = check_rewritten(ev, &p->targets[0].base, op->expect_len);
            if (reason == NULL) {
                reason = check_expect(ev, &p->targets[0].base, op->expect, op->expect_len, p->data);
            }
            if (reason != NULL) {
                return reason;
            }
        } else {
            p->path_expect = 1;
        }
        return NULL;
    case MNCF_OP_WRITE: {
        size_t size = op->type == MNCF_TYPE_BYTES ? op->bytes_len : mn_type_size(op->type);
        reason = target(ev, &op->at, size, &p->targets[0]);
        if (reason != NULL) {
            return reason;
        }
        p->type = op->type;
        if (op->type == MNCF_TYPE_BYTES) {
            p->data = op->bytes;
            p->data_len = op->bytes_len;
            if (op->has_expect) {
                p->expect = op->expect;
                p->expect_len = op->expect_len;
            }
            return NULL;
        }
        {
            mn_num v = op->value;
            if (op->value_kind == VAL_REF) {
                if (!player_value(ev, c, op->ref, &v)) {
                    return "value-out-of-range";
                }
                if (v.kind == MNCF_NUMBER_BOOL) {
                    v = mn_num_u64(v.magnitude);
                }
                if (!mn_fits(op->type, &v)) {
                    return "value-out-of-range";
                }
            }
            mn_encode_value(op->type, &v, big, p->buf);
            p->data = p->buf;
            p->data_len = size;
            if (op->has_num_expect) {
                mn_encode_value(op->type, &op->num_expect, big, p->ebuf);
                p->expect = p->ebuf;
                p->expect_len = size;
            }
        }
        return NULL;
    }
    case MNCF_OP_OVERRIDE: {
        mn_num v = op->value;
        int patches;
        size_t stub_len = 0;
        if (op->value_kind == VAL_REF) {
            if (!player_value(ev, c, op->ref, &v)) {
                return "value-out-of-range";
            }
            if (v.kind == MNCF_NUMBER_BOOL) {
                v = mn_num_u64(v.magnitude);
            }
        }
        patches = by_bytes(ev, op);
        if (patches) {
            stub_len = mn_stub(ev->arch, ev->arch_len, op->action, mn_num_low64(&v), p->buf);
        }
        reason = target(ev, &op->at, op->has_expect ? op->expect_len : 4, &p->targets[0]);
        if (reason != NULL) {
            return reason;
        }
        if (patches) {
            if (stub_len > op->expect_len) {
                return op->value_kind == VAL_REF ? "value-out-of-range" : "invalid:override.expect";
            }
            if (op->at.path_len == 0) {
                reason = check_rewritten(ev, &p->targets[0].base, stub_len);
                if (reason == NULL) {
                    reason = check_expect(ev, &p->targets[0].base, op->expect, op->expect_len, NULL);
                }
                if (reason != NULL) {
                    return reason;
                }
            }
            p->data = p->buf;
            p->data_len = stub_len;
            p->path_expect = op->at.path_len > 0;
        }
        if (op->has_expect) {
            p->expect = op->expect;
            p->expect_len = op->expect_len;
        }
        p->action = op->action;
        if (op->action == MNCF_ACTION_RETURN) {
            p->has_value = 1;
            p->value = v;
        }
        return NULL;
    }
    case MNCF_OP_READ:
        reason = target(ev, &op->at, mn_type_size(op->type), &p->targets[0]);
        if (reason != NULL) {
            return reason;
        }
        p->type = op->type;
        p->label = op->label;
        return NULL;
    default:
        reason = target(ev, &op->at, mn_type_size(op->type), &p->targets[0]);
        if (reason != NULL) {
            return reason;
        }
        p->type = op->type;
        p->value_kind = op->value_kind;
        if (op->value_kind == VAL_REF) {
            if (!player_value(ev, c, op->ref, &p->value)) {
                return "value-out-of-range";
            }
            p->value_kind = VAL_NUMBER;
            p->has_value = 1;
        } else if (op->value_kind == VAL_PLACE) {
            reason = target(ev, &op->other_at, mn_type_size(op->other_type), &p->targets[1]);
            if (reason != NULL) {
                return reason;
            }
            p->target_count = 2;
        } else if (op->value_kind == VAL_NUMBER) {
            p->value = op->value;
            p->has_value = 1;
        }
        return NULL;
    }
}

static void resolved(const mn_plan *p, const mn_place *places, mncf_op *out)
{
    const mn_op *op = p->op;
    memset(out, 0, sizeof *out);
    out->kind = (mncf_op_kind)p->kind;
    out->resolved = 1;
    out->address = places[0].address;
    if (places[0].module != NULL) {
        out->module = places[0].module_name;
        out->offset = places[0].offset;
    }
    out->type = p->kind == MNCF_OP_PATCH || p->kind == MNCF_OP_OVERRIDE ? MNCF_TYPE_NONE : p->type;
    out->expect = p->expect;
    out->expect_size = p->expect_len;
    out->bytes = p->data;
    out->size = p->data_len;
    out->label = p->label;
    if (p->kind == MNCF_OP_OVERRIDE) {
        out->action = (mncf_action)p->action;
        out->has_value = p->has_value;
        out->value = p->value;
    } else if (p->kind == MNCF_OP_WHEN) {
        out->compare = (mncf_compare)op->compare;
        out->value_kind = p->value_kind == VAL_NUMBER  ? MNCF_WHEN_NUMBER
                          : p->value_kind == VAL_RANGE ? MNCF_WHEN_RANGE
                          : p->value_kind == VAL_PLACE ? MNCF_WHEN_PLACE
                                                       : MNCF_WHEN_NONE;
        if (p->value_kind == VAL_NUMBER) {
            out->has_value = 1;
            out->value = p->value;
        } else if (p->value_kind == VAL_RANGE) {
            out->low = op->low;
            out->high = op->high;
        } else if (p->value_kind == VAL_PLACE) {
            out->other_address = places[1].address;
            out->other_type = op->other_type;
        }
    }
}

/* ---- One cheat -------------------------------------------------------------------------------------- */

static mn_result *raw_result(mn_eval *ev, const mn_raw *entry)
{
    mn_result *r = new_result(ev);
    if (r == NULL) {
        return NULL;
    }
    r->state = MNCF_STATE_OFF;
    r->uses = MNCF_USES_RAW;
    r->raw_entry = entry;
    if (ev->want_ops) {
        r->ops = (mncf_op *)mn_calloc(&ev->alloc, sizeof *r->ops);
        r->ops_size = sizeof *r->ops;
        if (r->ops == NULL) {
            ev->failed = 1;
            mn_result_free(ev, r);
            return NULL;
        }
        r->op_count = 1;
        r->ops[0].kind = MNCF_OP_RAW;
        r->ops[0].resolved = 1;
        r->ops[0].syntax = entry->syntax;
        r->ops[0].build = entry->build;
        r->ops[0].code = entry->code;
    }
    return r;
}

static mn_result *core(mn_eval *ev, const mn_cheat *c)
{
    mn_result *r = new_result(ev);
    size_t i;
    int waiting = 0;
    if (r == NULL) {
        return NULL;
    }
    r->plans_size = (c->op_count + 1) * sizeof *r->plans;
    r->plans = (mn_plan *)mn_calloc(&ev->alloc, r->plans_size);
    r->plan_count = c->op_count;
    if (ev->want_ops) {
        r->ops_size = (c->op_count + 1) * sizeof *r->ops;
        r->ops = (mncf_op *)mn_calloc(&ev->alloc, r->ops_size);
        r->op_count = c->op_count;
    }
    if (r->plans == NULL || (ev->want_ops && r->ops == NULL)) {
        ev->failed = 1;
        mn_result_free(ev, r);
        return NULL;
    }
    for (i = 0; i < c->op_count; i++) {
        mn_plan *p = &r->plans[i];
        const char *reason = plan_op(ev, c, &c->ops[i], p);
        mn_place places[2];
        size_t k;
        int ok = 1;
        if (reason == NULL) {
            for (k = 0; k < p->target_count && ok; k++) {
                ok = mn_follow(ev->f->platform, &p->targets[k], rt_reader, &ev->rt, &ev->alloc, &places[k]);
            }
            if (!ok) {
                waiting = 1;
                if (ev->want_ops) {
                    r->ops[i].kind = (mncf_op_kind)p->kind;
                }
                continue;
            }
            if (p->path_expect) {
                reason = check_expect(ev, &places[0], p->expect, p->expect_len,
                                      p->kind == MNCF_OP_PATCH ? p->data : NULL);
            }
        }
        if (reason != NULL) {
            mn_result_free(ev, r);
            return ev->failed ? NULL : mn_refused(ev, reason);
        }
        if (ev->want_ops) {
            resolved(p, places, &r->ops[i]);
        }
    }
    r->state = MNCF_STATE_OFF;
    r->uses = MNCF_USES_CORE;
    r->path_broken = waiting;
    return r;
}

mn_result *mn_own(mn_eval *ev, size_t index)
{
    const mn_cheat *c = &ev->f->cheats[index];
    const char *reason = file_reason(ev);
    unsigned caps = 0, missing;
    int needs_imm = 0, missing_ext, runs_raw = 0, has_core = 0;
    int button_caps = c->pub.hold != NULL || c->pub.toggle != NULL || c->pub.press != NULL;
    const mn_raw *entry = NULL;
    size_t i;
    if (reason != NULL) {
        return mn_refused(ev, reason);
    }
    if (c->pub.problem != NULL) {
        return mn_refused(ev, c->pub.problem);
    }
    reason = unsupported_part(ev, c);
    if (reason != NULL) {
        return mn_refused(ev, reason);
    }
    for (i = 0; i < c->op_count; i++) {
        const mn_op *op = &c->ops[i];
        if ((op->at.path_len > 0) || (op->kind == MNCF_OP_WHEN && op->value_kind == VAL_PLACE &&
                                      op->other_at.path_len > 0) ||
            op->kind == MNCF_OP_WHEN || op->kind == MNCF_OP_READ) {
            caps |= MNCF_CAP_GUEST_READ;
        }
        if (op->kind == MNCF_OP_WRITE) {
            caps |= MNCF_CAP_GUEST_WRITE;
        }
        if (op->kind == MNCF_OP_PATCH) {
            caps |= MNCF_CAP_CODE_BYTES;
            needs_imm |= op->encode;
        }
        if (op->kind == MNCF_OP_READ) {
            caps |= MNCF_CAP_DISPLAY;
        }
        if (op->kind == MNCF_OP_OVERRIDE && !by_bytes(ev, op)) {
            caps |= MNCF_CAP_CODE_HOOK;
        }
        if (op->kind != MNCF_OP_WHEN) {
            has_core = 1;
        }
    }
    if (button_caps) {
        caps |= MNCF_CAP_BUTTONS;
    }
    missing = caps & ~ev->rt.capabilities;
    missing_ext = needs_imm && !has_extension(ev, "arm64-imm/1");
    if (has_extension(ev, "raw/1")) {
        for (i = 0; i < c->raw_count && !runs_raw; i++) {
            runs_raw = has_syntax(ev, c->raw[i].syntax);
        }
    }
    for (i = 0; runs_raw && i < c->raw_count && entry == NULL; i++) {
        if (ev->build != NULL && strcmp(c->raw[i].build, ev->build) == 0 && has_syntax(ev, c->raw[i].syntax)) {
            entry = &c->raw[i];
        }
    }
    {
        int buttons_missing = button_caps && !(ev->rt.capabilities & MNCF_CAP_BUTTONS);
        if (has_core) {
            if (missing == 0 && !missing_ext) {
                return core(ev, c);
            }
            if (entry != NULL && !buttons_missing) {
                return raw_result(ev, entry);
            }
            if (missing != 0) {
                int bit = 0;
                while (!(missing & (1u << bit))) {
                    bit++;
                }
                return mn_refused(ev, mn_intern(ev, "capability-missing:", CAP_NAMES[bit]));
            }
            return mn_refused(ev, "extension-missing:arm64-imm/1");
        }
        if (!runs_raw) {
            return mn_refused(ev, "extension-missing:raw/1");
        }
        if (buttons_missing) {
            return mn_refused(ev, "capability-missing:buttons");
        }
        if (entry == NULL) {
            return mn_refused(ev, "build-unknown");
        }
        return raw_result(ev, entry);
    }
}

/* ---- Relations -------------------------------------------------------------------------------------- */

typedef struct marks {
    unsigned *stamp;
    unsigned epoch;
    size_t *stack;
    size_t *list;
    size_t count;
} marks;

static int marks_init(marks *m, const mn_alloc *a, size_t n)
{
    m->stamp = (unsigned *)mn_calloc(a, (n + 1) * sizeof *m->stamp);
    m->stack = (size_t *)mn_malloc(a, (n + 1) * sizeof *m->stack);
    m->list = (size_t *)mn_malloc(a, (n + 1) * sizeof *m->list);
    m->epoch = 0;
    m->count = 0;
    return m->stamp != NULL && m->stack != NULL && m->list != NULL;
}

static void marks_free(marks *m, const mn_alloc *a, size_t n)
{
    mn_free(a, m->stamp, (n + 1) * sizeof *m->stamp);
    mn_free(a, m->stack, (n + 1) * sizeof *m->stack);
    mn_free(a, m->list, (n + 1) * sizeof *m->list);
}

static void next_epoch(marks *m, size_t n)
{
    m->epoch++;
    if (m->epoch == 0) {
        memset(m->stamp, 0, (n + 1) * sizeof *m->stamp);
        m->epoch = 1;
    }
    m->count = 0;
}

static int cmp_size(const void *a, const void *b)
{
    size_t x = *(const size_t *)a, y = *(const size_t *)b;
    return x < y ? -1 : x > y;
}

/* Marks x and every cheat it requires, directly or not, without starting a new epoch. */
static void mark_closure(const mncf_file *f, marks *m, size_t x)
{
    size_t top = 0;
    m->stack[top++] = x;
    while (top > 0) {
        size_t y = m->stack[--top];
        const mn_cheat *c = &f->cheats[y];
        size_t i;
        if (m->stamp[y] == m->epoch) {
            continue;
        }
        m->stamp[y] = m->epoch;
        m->list[m->count++] = y;
        for (i = 0; i < c->pub.requires_count; i++) {
            if (m->stamp[c->requires[i]] != m->epoch) {
                m->stack[top++] = c->requires[i];
            }
        }
    }
}

/* The cheat and every cheat it requires, in file order, in m->list. */
static void closure(const mncf_file *f, marks *m, size_t x)
{
    next_epoch(m, f->cheat_count);
    mark_closure(f, m, x);
    qsort(m->list, m->count, sizeof *m->list, cmp_size);
}

static int same_radio(const mn_cheat *a, const mn_cheat *b)
{
    return a->pub.radio != NULL && b->pub.radio != NULL && strcmp(a->pub.radio, b->pub.radio) == 0;
}

/* A cheat of the group that can never be on together with the rest, or (size_t)-1. */
static size_t conflict_in(const mncf_file *f, const marks *m, size_t own)
{
    size_t i, j;
    for (i = 0; i < m->count; i++) {
        const mn_cheat *x = &f->cheats[m->list[i]];
        for (j = 0; j < x->pub.excludes_count; j++) {
            size_t y = x->excludes[j];
            if (m->stamp[y] == m->epoch) {
                return y != own ? y : m->list[i];
            }
        }
    }
    for (i = 0; i < m->count; i++) {
        const mn_cheat *x = &f->cheats[m->list[i]];
        if (x->pub.radio == NULL) {
            continue;
        }
        for (j = i + 1; j < m->count; j++) {
            if (same_radio(x, &f->cheats[m->list[j]])) {
                return m->list[j] != own ? m->list[j] : m->list[i];
            }
        }
    }
    return (size_t)-1;
}

int mn_relations(mn_eval *ev, mn_result *const *own, const char **out)
{
    const mncf_file *f = ev->f;
    size_t n = f->cheat_count, i, k;
    unsigned char *refused = (unsigned char *)mn_calloc(&ev->alloc, n + 1);
    marks m;
    int changed = 1;
    if (refused == NULL || !marks_init(&m, &ev->alloc, n)) {
        if (refused != NULL) {
            marks_free(&m, &ev->alloc, n);
        }
        mn_free(&ev->alloc, refused, n + 1);
        ev->failed = 1;
        return 0;
    }
    for (i = 0; i < n; i++) {
        out[i] = NULL;
        refused[i] = own[i]->state == MNCF_STATE_REFUSED;
    }
    while (changed) {
        changed = 0;
        for (i = 0; i < n; i++) {
            const mn_cheat *c = &f->cheats[i];
            if (refused[i]) {
                continue;
            }
            for (k = 0; k < c->pub.requires_count; k++) {
                if (refused[c->requires[k]]) {
                    out[i] = mn_intern(ev, "requires:", c->pub.requires[k]);
                    refused[i] = 1;
                    changed = 1;
                    break;
                }
            }
        }
    }
    for (i = 0; i < n; i++) {
        size_t conflict;
        if (refused[i]) {
            continue;
        }
        closure(f, &m, i);
        conflict = conflict_in(f, &m, i);
        if (conflict != (size_t)-1) {
            out[i] = mn_intern(ev, "excludes:", f->cheats[conflict].pub.id);
        }
    }
    marks_free(&m, &ev->alloc, n);
    mn_free(&ev->alloc, refused, n + 1);
    return !ev->failed;
}

/* ---- Switching -------------------------------------------------------------------------------------- */

int mn_on_init(mn_on *on, const mn_alloc *alloc, size_t cheats)
{
    on->items = (size_t *)mn_malloc(alloc, (cheats + 1) * sizeof *on->items);
    on->is = (unsigned char *)mn_calloc(alloc, cheats + 1);
    on->count = 0;
    return on->items != NULL && on->is != NULL;
}

void mn_on_free(mn_on *on, const mn_alloc *alloc, size_t cheats)
{
    mn_free(alloc, on->items, (cheats + 1) * sizeof *on->items);
    mn_free(alloc, on->is, cheats + 1);
}

static int conflicts(const mncf_file *f, size_t a, size_t b)
{
    const mn_cheat *ca = &f->cheats[a], *cb = &f->cheats[b];
    size_t i;
    if (a == b) {
        return 0;
    }
    for (i = 0; i < ca->pub.excludes_count; i++) {
        if (ca->excludes[i] == b) {
            return 1;
        }
    }
    for (i = 0; i < cb->pub.excludes_count; i++) {
        if (cb->excludes[i] == a) {
            return 1;
        }
    }
    return same_radio(ca, cb);
}

static void keep_if(mn_on *on, const unsigned char *keep)
{
    size_t i, k = 0;
    for (i = 0; i < on->count; i++) {
        size_t y = on->items[i];
        if (keep[i]) {
            on->items[k++] = y;
        } else {
            on->is[y] = 0;
        }
    }
    on->count = k;
}

static int drop_unmet(mn_eval *ev, mn_on *on)
{
    const mncf_file *f = ev->f;
    unsigned char *keep = (unsigned char *)mn_malloc(&ev->alloc, f->cheat_count + 1);
    if (keep == NULL) {
        ev->failed = 1;
        return 0;
    }
    for (;;) {
        size_t i, k;
        int dropped = 0;
        for (i = 0; i < on->count; i++) {
            const mn_cheat *c = &f->cheats[on->items[i]];
            keep[i] = 1;
            for (k = 0; k < c->pub.requires_count; k++) {
                if (!on->is[c->requires[k]]) {
                    keep[i] = 0;
                    dropped = 1;
                    break;
                }
            }
        }
        if (!dropped) {
            break;
        }
        keep_if(on, keep);
    }
    mn_free(&ev->alloc, keep, f->cheat_count + 1);
    return 1;
}

static int drop_masters(mn_eval *ev, mn_on *on)
{
    const mncf_file *f = ev->f;
    marks m;
    unsigned char *keep;
    size_t i;
    if (!marks_init(&m, &ev->alloc, f->cheat_count)) {
        marks_free(&m, &ev->alloc, f->cheat_count);
        ev->failed = 1;
        return 0;
    }
    keep = (unsigned char *)mn_malloc(&ev->alloc, f->cheat_count + 1);
    if (keep == NULL) {
        marks_free(&m, &ev->alloc, f->cheat_count);
        ev->failed = 1;
        return 0;
    }
    next_epoch(&m, f->cheat_count);
    for (i = 0; i < on->count; i++) {
        if (!f->cheats[on->items[i]].pub.master) {
            mark_closure(f, &m, on->items[i]);
        }
    }
    for (i = 0; i < on->count; i++) {
        keep[i] = m.stamp[on->items[i]] == m.epoch;
    }
    keep_if(on, keep);
    mn_free(&ev->alloc, keep, f->cheat_count + 1);
    marks_free(&m, &ev->alloc, f->cheat_count);
    return 1;
}

int mn_switch_on(mn_eval *ev, mn_on *on, size_t x)
{
    const mncf_file *f = ev->f;
    marks m;
    unsigned char *keep;
    size_t i, k;
    if (!marks_init(&m, &ev->alloc, f->cheat_count)) {
        marks_free(&m, &ev->alloc, f->cheat_count);
        ev->failed = 1;
        return 0;
    }
    keep = (unsigned char *)mn_malloc(&ev->alloc, f->cheat_count + 1);
    if (keep == NULL) {
        marks_free(&m, &ev->alloc, f->cheat_count);
        ev->failed = 1;
        return 0;
    }
    closure(f, &m, x);
    for (i = 0; i < on->count; i++) {
        keep[i] = 1;
        for (k = 0; k < m.count && keep[i]; k++) {
            if (conflicts(f, on->items[i], m.list[k])) {
                keep[i] = 0;
            }
        }
    }
    keep_if(on, keep);
    mn_free(&ev->alloc, keep, f->cheat_count + 1);
    if (!drop_unmet(ev, on)) {
        marks_free(&m, &ev->alloc, f->cheat_count);
        return 0;
    }
    for (k = 0; k < m.count; k++) {
        size_t g = m.list[k];
        if (!on->is[g]) {
            on->items[on->count++] = g;
            on->is[g] = 1;
        }
    }
    marks_free(&m, &ev->alloc, f->cheat_count);
    return drop_masters(ev, on);
}

int mn_switch_off(mn_eval *ev, mn_on *on, size_t x)
{
    size_t i, k = 0;
    for (i = 0; i < on->count; i++) {
        if (on->items[i] != x) {
            on->items[k++] = on->items[i];
        }
    }
    on->count = k;
    on->is[x] = 0;
    return drop_unmet(ev, on) && drop_masters(ev, on);
}

int mn_waits_for_buttons(const mn_cheat *c, const mn_combo *held, int pressed_once)
{
    if (c->pub.hold != NULL && !mn_combo_subset(&c->hold, held)) {
        return 1;
    }
    return c->pub.run == MNCF_RUN_ACTION && !pressed_once;
}

/* ---- mncf_evaluate ---------------------------------------------------------------------------------- */

struct mncf_evaluation {
    mn_alloc alloc;
    mn_arena arena;
    mn_eval ev;
    const char *build;
    size_t count;
    mn_result **own;
    mncf_cheat_result *results;
};

void mncf_evaluation_free(mncf_evaluation *e)
{
    size_t i;
    mn_alloc a;
    if (e == NULL) {
        return;
    }
    a = e->alloc;
    if (e->own != NULL) {
        for (i = 0; i < e->count; i++) {
            mn_result_free(&e->ev, e->own[i]);
        }
    }
    mn_free(&a, e->own, (e->count + 1) * sizeof *e->own);
    mn_arena_free(&e->arena);
    mn_free(&a, e, sizeof *e);
}

/* The values of a caller's choices, in the arena. */
static mn_value *copy_values(mn_arena *ar, const mncf_choices *ch, size_t *count)
{
    mn_value *out;
    size_t i;
    *count = 0;
    if (ch == NULL || ch->value_count == 0) {
        return NULL;
    }
    out = (mn_value *)mn_arena_alloc(ar, ch->value_count * sizeof *out);
    if (out == NULL) {
        return NULL;
    }
    for (i = 0; i < ch->value_count; i++) {
        const char *key = ch->values[i].key != NULL ? ch->values[i].key : "";
        out[i].key = mn_arena_strdup(ar, key);
        out[i].key_len = strlen(key);
        out[i].value = ch->values[i].value;
        if (out[i].key == NULL) {
            return NULL;
        }
    }
    *count = ch->value_count;
    return out;
}

int mn_load_switch(mn_eval *ev, const mncf_choices *ch, const unsigned char *refused, mn_on *on)
{
    const mncf_file *f = ev->f;
    size_t i;
    for (i = 0; i < f->cheat_count; i++) {
        const mn_cheat *c = &f->cheats[i];
        int wanted = 0;
        if (c->pub.master) {
            continue;
        }
        if (c->pub.default_on && !(ch != NULL && has_name(ch->off, ch->off_count, c->pub.id))) {
            wanted = 1;
        }
        if (ch != NULL && has_name(ch->on, ch->on_count, c->pub.id)) {
            wanted = 1;
        }
        if (wanted && !refused[i] && !mn_switch_on(ev, on, i)) {
            return 0;
        }
    }
    return 1;
}

mncf_evaluation *mncf_evaluate(const mncf_file *f, const mncf_runtime *rt, const mncf_choices *ch, mncf_error *err)
{
    mn_alloc a;
    mncf_evaluation *e;
    mn_eval *ev;
    const char **rel = NULL;
    unsigned char *refused = NULL;
    mn_on on;
    size_t i, n;
    mn_combo held;
    mn_error_clear(err);
    if (f == NULL || rt == NULL) {
        mn_error(err, MNCF_ERR_ARGUMENT, "argument", "no file or no runtime given");
        return NULL;
    }
    a = f->alloc;
    e = (mncf_evaluation *)mn_calloc(&a, sizeof *e);
    if (e == NULL) {
        mn_error_nomem(err);
        return NULL;
    }
    e->alloc = a;
    mn_arena_init(&e->arena, a);
    ev = &e->ev;
    mn_eval_init(ev, f, rt, a, &e->arena);
    ev->want_ops = 1;
    ev->values = copy_values(&e->arena, ch, &ev->value_count);
    n = f->cheat_count;
    e->count = n;
    e->build = ev->build;
    e->own = (mn_result **)mn_calloc(&a, (n + 1) * sizeof *e->own);
    e->results = (mncf_cheat_result *)mn_arena_zalloc(&e->arena, (n + 1) * sizeof *e->results);
    rel = (const char **)mn_arena_alloc(&e->arena, (n + 1) * sizeof *rel);
    refused = (unsigned char *)mn_arena_zalloc(&e->arena, n + 1);
    memset(&on, 0, sizeof on);
    if (e->own == NULL || e->results == NULL || rel == NULL || refused == NULL || !mn_on_init(&on, &a, n)) {
        goto nomem;
    }
    for (i = 0; i < n; i++) {
        e->own[i] = mn_own(ev, i);
        if (e->own[i] == NULL) {
            goto nomem;
        }
    }
    if (!mn_relations(ev, e->own, rel)) {
        goto nomem;
    }
    for (i = 0; i < n; i++) {
        refused[i] = e->own[i]->state == MNCF_STATE_REFUSED || rel[i] != NULL;
    }
    if (!mn_load_switch(ev, ch, refused, &on)) {
        goto nomem;
    }
    held.names = NULL;
    held.count = 0;
    {
        const char **names = (const char **)mn_arena_alloc(&e->arena, (rt->held_count + 1) * sizeof *names);
        if (names == NULL) {
            goto nomem;
        }
        for (i = 0; i < rt->held_count; i++) {
            names[i] = rt->held[i];
        }
        held.names = names;
        held.count = rt->held_count;
        qsort(names, held.count, sizeof *names, mn_cmp_names);
    }
    for (i = 0; i < n; i++) {
        const mn_cheat *c = &f->cheats[i];
        mncf_cheat_result *r = &e->results[i];
        const mn_result *own = e->own[i];
        r->id = c->pub.id;
        r->reason = "";
        if (refused[i]) {
            r->state = MNCF_STATE_REFUSED;
            r->reason = rel[i] != NULL ? rel[i] : own->reason;
            continue;
        }
        r->uses = own->uses;
        r->ops = own->ops;
        r->op_count = own->op_count;
        if (!on.is[i]) {
            r->state = MNCF_STATE_OFF;
        } else if (own->path_broken) {
            r->state = MNCF_STATE_WAITING;
            r->reason = "path-broken";
        } else if (own->uses == MNCF_USES_CORE && mn_waits_for_buttons(c, &held, 0)) {
            r->state = MNCF_STATE_WAITING;
        } else {
            r->state = MNCF_STATE_ON;
        }
    }
    mn_on_free(&on, &a, n);
    if (ev->failed || e->arena.failed) {
        goto nomem;
    }
    return e;
nomem:
    mn_on_free(&on, &a, n);
    mncf_evaluation_free(e);
    mn_error_nomem(err);
    return NULL;
}

const char *mncf_evaluation_build(const mncf_evaluation *e)
{
    return e != NULL ? e->build : NULL;
}

size_t mncf_evaluation_count(const mncf_evaluation *e)
{
    return e != NULL ? e->count : 0;
}

const mncf_cheat_result *mncf_evaluation_cheat(const mncf_evaluation *e, size_t i)
{
    return e != NULL && i < e->count ? &e->results[i] : NULL;
}
