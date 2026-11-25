/* engine.c: runs the cheats of a file pass by pass, as a runtime does (spec, sections 7.2, 7.3, 7.9, 11.3
 * and 11.5). MIT License.
 *
 * The engine reads guest memory only through the reader it is given and changes nothing itself: the caller
 * makes the changes of each pass, all code changes in one stop. It keeps the original bytes of every place
 * it changed and puts them back when no cheat that is on covers the place any more.
 */
#include "evaluate.h"

#include <math.h>
#include <stdlib.h>

/* ---- Maps of addresses ------------------------------------------------------------------------------ */

/* Address -> the original byte and the byte the engine knows is there now, for every code byte a cheat
 * covers. Open addressing with tombstones, on the allocator. */
typedef struct kept_slot {
    uint64_t address;
    uint8_t state;      /* 0 empty, 1 used, 2 deleted */
    uint8_t kept;
    uint8_t now;
} kept_slot;

typedef struct kept_map {
    kept_slot *slots;
    size_t cap, used, filled;
} kept_map;

static size_t addr_hash(uint64_t a)
{
    a ^= a >> 33;
    a *= 0xff51afd7ed558ccdULL;
    a ^= a >> 33;
    return (size_t)a;
}

static kept_slot *kept_find(const kept_map *m, uint64_t a)
{
    size_t i;
    if (m->cap == 0) {
        return NULL;
    }
    i = addr_hash(a) & (m->cap - 1);
    for (;;) {
        kept_slot *s = &m->slots[i];
        if (s->state == 0) {
            return NULL;
        }
        if (s->state == 1 && s->address == a) {
            return s;
        }
        i = (i + 1) & (m->cap - 1);
    }
}

static int kept_grow(kept_map *m, const mn_alloc *alloc)
{
    size_t cap = m->cap == 0 ? 64 : m->cap, i;
    kept_slot *slots;
    while (cap < (m->used + 1) * 4) {
        cap *= 2;
    }
    slots = (kept_slot *)mn_calloc(alloc, cap * sizeof *slots);
    if (slots == NULL) {
        return 0;
    }
    for (i = 0; i < m->cap; i++) {
        if (m->slots[i].state == 1) {
            size_t k = addr_hash(m->slots[i].address) & (cap - 1);
            while (slots[k].state != 0) {
                k = (k + 1) & (cap - 1);
            }
            slots[k] = m->slots[i];
        }
    }
    mn_free(alloc, m->slots, m->cap * sizeof *m->slots);
    m->slots = slots;
    m->cap = cap;
    m->filled = m->used;
    return 1;
}

static int kept_put(kept_map *m, const mn_alloc *alloc, uint64_t a, uint8_t kept, uint8_t now)
{
    size_t i;
    kept_slot *s = kept_find(m, a);
    if (s != NULL) {
        s->kept = kept;
        s->now = now;
        return 1;
    }
    if ((m->filled + 1) * 2 > m->cap && !kept_grow(m, alloc)) {
        return 0;
    }
    i = addr_hash(a) & (m->cap - 1);
    while (m->slots[i].state == 1) {
        i = (i + 1) & (m->cap - 1);
    }
    if (m->slots[i].state == 0) {
        m->filled++;
    }
    m->slots[i].state = 1;
    m->slots[i].address = a;
    m->slots[i].kept = kept;
    m->slots[i].now = now;
    m->used++;
    return 1;
}

static void kept_del(kept_map *m, uint64_t a)
{
    kept_slot *s = kept_find(m, a);
    if (s != NULL) {
        s->state = 2;
        m->used--;
    }
}

/* A map of addresses to a byte, in the pass's arena, without deletion. */
typedef struct byte_map {
    uint64_t *keys;
    uint8_t *vals;
    uint8_t *used;
    size_t cap, count;
} byte_map;

static int bm_put(mn_arena *ar, byte_map *m, uint64_t a, uint8_t v)
{
    size_t i;
    if ((m->count + 1) * 2 > m->cap) {
        size_t cap = m->cap == 0 ? 64 : m->cap * 2, k;
        uint64_t *keys = (uint64_t *)mn_arena_alloc(ar, cap * sizeof *keys);
        uint8_t *vals = (uint8_t *)mn_arena_alloc(ar, cap);
        uint8_t *used = (uint8_t *)mn_arena_zalloc(ar, cap);
        if (keys == NULL || vals == NULL || used == NULL) {
            return 0;
        }
        for (k = 0; k < m->cap; k++) {
            if (m->used[k]) {
                size_t j = addr_hash(m->keys[k]) & (cap - 1);
                while (used[j]) {
                    j = (j + 1) & (cap - 1);
                }
                used[j] = 1;
                keys[j] = m->keys[k];
                vals[j] = m->vals[k];
            }
        }
        m->keys = keys;
        m->vals = vals;
        m->used = used;
        m->cap = cap;
    }
    i = addr_hash(a) & (m->cap - 1);
    while (m->used[i]) {
        if (m->keys[i] == a) {
            m->vals[i] = v;
            return 1;
        }
        i = (i + 1) & (m->cap - 1);
    }
    m->used[i] = 1;
    m->keys[i] = a;
    m->vals[i] = v;
    m->count++;
    return 1;
}

static int bm_get(const byte_map *m, uint64_t a, uint8_t *v)
{
    size_t i;
    if (m->cap == 0) {
        return 0;
    }
    i = addr_hash(a) & (m->cap - 1);
    while (m->used[i]) {
        if (m->keys[i] == a) {
            if (v != NULL) {
                *v = m->vals[i];
            }
            return 1;
        }
        i = (i + 1) & (m->cap - 1);
    }
    return 0;
}

/* ---- What the engine keeps per cheat ---------------------------------------------------------------- */

typedef struct cover {
    int set;
    uint64_t address;
    uint8_t *data;              /* a copy, on the allocator */
    size_t len;
} cover;

typedef struct hook {
    int set;
    uint64_t address;
    int action;
    int has_value;
    mn_num value;
} hook;

typedef struct run {
    const char *refused;        /* the reason it is refused, or NULL */
    mncf_state state;
    const char *reason;
    uint64_t passes;            /* passes since it was switched on */
    int has_started;
    double started;             /* the time of its first pass, for timed */
    int pressed;                /* an action that was pressed since it was switched on */
    int held_before;            /* whether its conditions held at its last test */
    uint8_t *last;              /* per operation, 8 bytes read at the last test, for changed */
    unsigned char *has_last;
    int active;                 /* whether its patches and overrides are applied */
    cover *covers, *covers_next;
    hook *hooks, *hooks_next;
    size_t ops;                 /* operations of the cheat */
} run;

/* What one cheat does in a pass. */
typedef struct work {
    int present;
    int active;
    int keep;                   /* a pass between two tests of an interval: nothing changes */
    mn_place (*places)[2];      /* per operation */
    mncf_change *out;
    size_t out_count, out_cap;
    mncf_shown *shown;
    size_t shown_count, shown_cap;
    int acted;
    const uint8_t **checked;    /* per operation, the original bytes found by the expect check */
} work;

typedef struct pass_read_entry {
    uint64_t address;
    size_t size;
    int ok;
    uint8_t *data;
    int used;
} pass_read_entry;

struct mncf_engine {
    mn_alloc alloc;
    mn_arena keep;              /* reasons and the pattern cache, for the engine's life */
    mn_arena pass;              /* what one pass makes; reset at the next pass */
    mn_arena presses_arena;     /* presses between passes */
    mn_eval ev;
    const mncf_file *f;
    double rate;
    mn_value *values;
    size_t value_count, value_cap;
    mn_result **own;
    run *runs;
    size_t n;
    mn_on on;
    mn_combo held, held_before;
    mn_combo *presses;
    size_t press_count, press_cap;
    uint64_t number;
    kept_map kept;
    hook *hooked;               /* sorted by address */
    size_t hooked_count;
    /* the pass that runs */
    work *work;
    pass_read_entry *reads;
    size_t read_cap, read_count;
    mncf_read_fn read;
    void *read_ctx;
    int failed;
};

/* ---- Small helpers ---------------------------------------------------------------------------------- */

static void *ealloc(mncf_engine *e, size_t size)
{
    void *p = mn_calloc(&e->alloc, size);
    if (p == NULL) {
        e->failed = 1;
    }
    return p;
}

static void free_str(mncf_engine *e, const char *s)
{
    mn_free_const(&e->alloc, s, strlen(s) + 1);
}

static void combo_free(mncf_engine *e, mn_combo *c)
{
    size_t i;
    for (i = 0; i < c->count; i++) {
        free_str(e, c->names[i]);
    }
    if (c->cap > 0) {
        mn_free_const(&e->alloc, c->names, c->cap * sizeof *c->names);
    }
    c->names = NULL;
    c->count = 0;
    c->cap = 0;
}

/* Makes c a combination of the names, sorted, each once, on the allocator. */
static int combo_set(mncf_engine *e, mn_combo *c, const char *const *names, size_t count)
{
    const char **copy = (const char **)ealloc(e, (count + 1) * sizeof *copy);
    size_t i, k = 0;
    if (copy == NULL) {
        return 0;
    }
    for (i = 0; i < count; i++) {
        const char *s = names[i] != NULL ? names[i] : "";
        size_t n = strlen(s);
        char *d = (char *)mn_malloc(&e->alloc, n + 1);
        if (d == NULL) {
            size_t j;
            for (j = 0; j < i; j++) {
                free_str(e, copy[j]);
            }
            mn_free_const(&e->alloc, copy, (count + 1) * sizeof *copy);
            e->failed = 1;
            return 0;
        }
        memcpy(d, s, n + 1);
        copy[i] = d;
    }
    qsort(copy, count, sizeof *copy, mn_cmp_names);
    for (i = 0; i < count; i++) {
        if (k > 0 && strcmp(copy[k - 1], copy[i]) == 0) {
            free_str(e, copy[i]);
            continue;
        }
        copy[k++] = copy[i];
    }
    combo_free(e, c);
    c->names = copy;
    c->count = k;
    c->cap = count + 1;
    return 1;
}

static const mn_cheat *cheat_at(const mncf_engine *e, size_t i)
{
    return &e->f->cheats[i];
}

static void run_restart(run *r)
{
    r->passes = 0;
    r->has_started = 0;
    r->started = 0;
    r->pressed = 0;
    r->held_before = 0;
    if (r->has_last != NULL) {
        memset(r->has_last, 0, r->ops + 1);
    }
}

static void covers_clear(mncf_engine *e, cover *c, size_t n)
{
    size_t i;
    for (i = 0; i < n; i++) {
        if (c[i].data != NULL) {
            mn_free(&e->alloc, c[i].data, c[i].len);
        }
        memset(&c[i], 0, sizeof c[i]);
    }
}

/* ---- Switching -------------------------------------------------------------------------------------- */

/* Gives the cheats whose state changed with the cheats that are on now; before is the old flags. */
static void set_on_after(mncf_engine *e, const unsigned char *before, size_t done)
{
    size_t i;
    for (i = 0; i < e->n; i++) {
        run *r = &e->runs[i];
        if (r->refused != NULL) {
            continue;
        }
        if (before[i] && !e->on.is[i]) {
            r->state = MNCF_STATE_OFF;
            r->reason = i == done ? "done" : "";
        } else if (!before[i] && e->on.is[i]) {
            int waits;
            run_restart(r);
            waits = e->own[i]->uses == MNCF_USES_CORE && mn_waits_for_buttons(cheat_at(e, i), &e->held, 0);
            r->state = waits ? MNCF_STATE_WAITING : MNCF_STATE_ON;
            r->reason = "";
        }
    }
}

static unsigned char *flags_copy(mncf_engine *e)
{
    unsigned char *b = (unsigned char *)mn_malloc(&e->alloc, e->n + 1);
    if (b == NULL) {
        e->failed = 1;
        return NULL;
    }
    memcpy(b, e->on.is, e->n + 1);
    return b;
}

static int switch_cheat(mncf_engine *e, size_t i, int on, size_t done)
{
    unsigned char *before = flags_copy(e);
    int ok;
    if (before == NULL) {
        return 0;
    }
    ok = on ? mn_switch_on(&e->ev, &e->on, i) : mn_switch_off(&e->ev, &e->on, i);
    if (ok) {
        set_on_after(e, before, done);
    }
    mn_free(&e->alloc, before, e->n + 1);
    if (!ok) {
        e->failed = 1;
    }
    return ok;
}

/* Gives every cheat the result of its relations after a cheat's own result changed, and switches off
 * the cheats that are refused now. */
static int refresh(mncf_engine *e)
{
    const char **rel = (const char **)mn_malloc(&e->alloc, (e->n + 1) * sizeof *rel);
    unsigned char *before;
    size_t i;
    if (rel == NULL) {
        e->failed = 1;
        return 0;
    }
    if (!mn_relations(&e->ev, e->own, rel)) {
        mn_free(&e->alloc, (void *)rel, (e->n + 1) * sizeof *rel);
        e->failed = 1;
        return 0;
    }
    for (i = 0; i < e->n; i++) {
        run *r = &e->runs[i];
        if (rel[i] != NULL || e->own[i]->state == MNCF_STATE_REFUSED) {
            r->refused = rel[i] != NULL ? rel[i] : e->own[i]->reason;
            r->state = MNCF_STATE_REFUSED;
            r->reason = r->refused;
        } else if (r->refused != NULL) {
            r->refused = NULL;
            r->state = MNCF_STATE_OFF;
            r->reason = "";
        }
    }
    mn_free(&e->alloc, (void *)rel, (e->n + 1) * sizeof *rel);
    before = flags_copy(e);
    if (before == NULL) {
        return 0;
    }
    for (i = 0; i < e->n; i++) {
        if (e->on.is[i] && e->runs[i].refused != NULL && !mn_switch_off(&e->ev, &e->on, i)) {
            mn_free(&e->alloc, before, e->n + 1);
            e->failed = 1;
            return 0;
        }
    }
    set_on_after(e, before, (size_t)-1);
    mn_free(&e->alloc, before, e->n + 1);
    return 1;
}

static int replace_own(mncf_engine *e, size_t i, mn_result *r)
{
    if (r == NULL) {
        e->failed = 1;
        return 0;
    }
    mn_result_free(&e->ev, e->own[i]);
    e->own[i] = r;
    return 1;
}

/* ---- Creating and freeing --------------------------------------------------------------------------- */

void mncf_engine_free(mncf_engine *e)
{
    mn_alloc a;
    size_t i;
    if (e == NULL) {
        return;
    }
    a = e->alloc;
    for (i = 0; e->runs != NULL && i < e->n; i++) {
        run *r = &e->runs[i];
        if (r->covers != NULL) {
            covers_clear(e, r->covers, r->ops);
        }
        if (r->covers_next != NULL) {
            covers_clear(e, r->covers_next, r->ops);
        }
        mn_free(&a, r->covers, (r->ops + 1) * sizeof *r->covers);
        mn_free(&a, r->covers_next, (r->ops + 1) * sizeof *r->covers_next);
        mn_free(&a, r->hooks, (r->ops + 1) * sizeof *r->hooks);
        mn_free(&a, r->hooks_next, (r->ops + 1) * sizeof *r->hooks_next);
        mn_free(&a, r->last, (r->ops + 1) * 8);
        mn_free(&a, r->has_last, r->ops + 1);
    }
    for (i = 0; e->own != NULL && i < e->n; i++) {
        mn_result_free(&e->ev, e->own[i]);
    }
    for (i = 0; e->values != NULL && i < e->value_count; i++) {
        mn_free_const(&a, e->values[i].key, e->values[i].key_len + 1);
    }
    mn_free(&a, e->values, e->value_cap * sizeof *e->values);
    mn_free(&a, e->own, (e->n + 1) * sizeof *e->own);
    mn_free(&a, e->runs, (e->n + 1) * sizeof *e->runs);
    mn_on_free(&e->on, &a, e->n);
    combo_free(e, &e->held);
    combo_free(e, &e->held_before);
    mn_free(&a, e->kept.slots, e->kept.cap * sizeof *e->kept.slots);
    mn_free(&a, e->hooked, (e->hooked_count + 1) * sizeof *e->hooked);
    mn_arena_free(&e->keep);
    mn_arena_free(&e->pass);
    mn_arena_free(&e->presses_arena);
    mn_free(&a, e, sizeof *e);
}

static int value_put(mncf_engine *e, const char *key, mncf_number v)
{
    size_t i, n = strlen(key);
    char *copy;
    for (i = 0; i < e->value_count; i++) {
        if (mn_streq(e->values[i].key, e->values[i].key_len, key, n)) {
            e->values[i].value = v;
            return 1;
        }
    }
    if (e->value_count == e->value_cap) {
        size_t cap = e->value_cap == 0 ? 8 : e->value_cap * 2;
        mn_value *grown = (mn_value *)mn_realloc(&e->alloc, e->values, e->value_cap * sizeof *grown, cap * sizeof *grown);
        if (grown == NULL) {
            e->failed = 1;
            return 0;
        }
        e->values = grown;
        e->value_cap = cap;
    }
    copy = (char *)mn_malloc(&e->alloc, n + 1);
    if (copy == NULL) {
        e->failed = 1;
        return 0;
    }
    memcpy(copy, key, n + 1);
    e->values[e->value_count].key = copy;
    e->values[e->value_count].key_len = n;
    e->values[e->value_count].value = v;
    e->value_count++;
    e->ev.values = e->values;
    e->ev.value_count = e->value_count;
    return 1;
}

mncf_engine *mncf_engine_new(const mncf_file *f, const mncf_runtime *rt, const mncf_choices *ch, double rate,
                             mncf_error *err)
{
    mn_alloc a;
    mncf_engine *e;
    const char **rel = NULL;
    unsigned char *refused = NULL;
    size_t i;
    mn_error_clear(err);
    if (f == NULL || rt == NULL) {
        mn_error(err, MNCF_ERR_ARGUMENT, "argument", "no file or no runtime given");
        return NULL;
    }
    if (!(rate > 0)) {
        mn_error(err, MNCF_ERR_ARGUMENT, "argument", "rate is the number of passes per second, above 0");
        return NULL;
    }
    a = f->alloc;
    e = (mncf_engine *)mn_calloc(&a, sizeof *e);
    if (e == NULL) {
        mn_error_nomem(err);
        return NULL;
    }
    e->alloc = a;
    e->f = f;
    e->rate = rate;
    e->n = f->cheat_count;
    mn_arena_init(&e->keep, a);
    mn_arena_init(&e->pass, a);
    mn_arena_init(&e->presses_arena, a);
    mn_eval_init(&e->ev, f, rt, a, &e->keep);
    for (i = 0; ch != NULL && i < ch->value_count; i++) {
        if (!value_put(e, ch->values[i].key != NULL ? ch->values[i].key : "", ch->values[i].value)) {
            goto nomem;
        }
    }
    e->own = (mn_result **)ealloc(e, (e->n + 1) * sizeof *e->own);
    e->runs = (run *)ealloc(e, (e->n + 1) * sizeof *e->runs);
    if (e->own == NULL || e->runs == NULL || !mn_on_init(&e->on, &a, e->n)) {
        goto nomem;
    }
    for (i = 0; i < e->n; i++) {
        run *r = &e->runs[i];
        size_t ops = cheat_at(e, i)->op_count;
        r->ops = ops;
        r->covers = (cover *)ealloc(e, (ops + 1) * sizeof *r->covers);
        r->covers_next = (cover *)ealloc(e, (ops + 1) * sizeof *r->covers_next);
        r->hooks = (hook *)ealloc(e, (ops + 1) * sizeof *r->hooks);
        r->hooks_next = (hook *)ealloc(e, (ops + 1) * sizeof *r->hooks_next);
        r->last = (uint8_t *)ealloc(e, (ops + 1) * 8);
        r->has_last = (unsigned char *)ealloc(e, ops + 1);
        if (e->failed) {
            goto nomem;
        }
        e->own[i] = mn_own(&e->ev, i);
        if (e->own[i] == NULL) {
            goto nomem;
        }
    }
    rel = (const char **)ealloc(e, (e->n + 1) * sizeof *rel);
    refused = (unsigned char *)ealloc(e, e->n + 1);
    if (rel == NULL || refused == NULL || !mn_relations(&e->ev, e->own, rel)) {
        goto nomem;
    }
    for (i = 0; i < e->n; i++) {
        refused[i] = e->own[i]->state == MNCF_STATE_REFUSED || rel[i] != NULL;
    }
    if (!combo_set(e, &e->held, rt->held, rt->held_count) || !combo_set(e, &e->held_before, rt->held, rt->held_count) ||
        !mn_load_switch(&e->ev, ch, refused, &e->on)) {
        goto nomem;
    }
    for (i = 0; i < e->n; i++) {
        run *r = &e->runs[i];
        const mn_cheat *c = cheat_at(e, i);
        r->reason = "";
        if (refused[i]) {
            r->refused = rel[i] != NULL ? rel[i] : e->own[i]->reason;
            r->state = MNCF_STATE_REFUSED;
            r->reason = r->refused;
        } else if (!e->on.is[i]) {
            r->state = MNCF_STATE_OFF;
        } else if (e->own[i]->path_broken) {
            r->state = MNCF_STATE_WAITING;
            r->reason = "path-broken";
        } else if (e->own[i]->uses == MNCF_USES_CORE && mn_waits_for_buttons(c, &e->held, 0)) {
            r->state = MNCF_STATE_WAITING;
        } else {
            r->state = MNCF_STATE_ON;
        }
    }
    mn_free(&a, (void *)rel, (e->n + 1) * sizeof *rel);
    mn_free(&a, refused, e->n + 1);
    if (e->failed || e->ev.failed) {
        mncf_engine_free(e);
        mn_error_nomem(err);
        return NULL;
    }
    return e;
nomem:
    mn_free(&a, (void *)rel, (e->n + 1) * sizeof *rel);
    mn_free(&a, refused, e->n + 1);
    mncf_engine_free(e);
    mn_error_nomem(err);
    return NULL;
}

/* ---- What the player does between passes ------------------------------------------------------------ */

static size_t cheat_named(const mncf_engine *e, const char *id)
{
    return id != NULL ? mn_cheat_index(e->f, id, strlen(id)) : (size_t)-1;
}

mncf_status mncf_engine_switch(mncf_engine *e, const char *cheat_id, int on, int *is_on)
{
    size_t i;
    if (e == NULL) {
        return MNCF_ERR_ARGUMENT;
    }
    i = cheat_named(e, cheat_id);
    if (i == (size_t)-1) {
        return MNCF_ERR_ARGUMENT;
    }
    if (!cheat_at(e, i)->pub.master && e->runs[i].refused == NULL) {
        if (on && !e->on.is[i]) {
            if (!switch_cheat(e, i, 1, (size_t)-1)) {
                return MNCF_ERR_MEMORY;
            }
        } else if (!on && e->on.is[i]) {
            if (!switch_cheat(e, i, 0, (size_t)-1)) {
                return MNCF_ERR_MEMORY;
            }
        }
    }
    if (is_on != NULL) {
        *is_on = e->on.is[i];
    }
    return MNCF_OK;
}

mncf_status mncf_engine_set_value(mncf_engine *e, const char *key, mncf_number value)
{
    const char *dot;
    size_t i, k;
    const mn_cheat *c;
    if (e == NULL || key == NULL) {
        return MNCF_ERR_ARGUMENT;
    }
    dot = strchr(key, '.');
    i = mn_cheat_index(e->f, key, dot != NULL ? (size_t)(dot - key) : strlen(key));
    if (i == (size_t)-1) {
        return MNCF_ERR_ARGUMENT;
    }
    c = cheat_at(e, i);
    for (k = 0; k < c->pub.value_count; k++) {
        if (dot != NULL && strcmp(c->pub.values[k].id, dot + 1) == 0) {
            break;
        }
    }
    if (k == c->pub.value_count) {
        return MNCF_ERR_ARGUMENT;
    }
    if (!value_put(e, key, value) || !replace_own(e, i, mn_own(&e->ev, i)) || !refresh(e)) {
        return MNCF_ERR_MEMORY;
    }
    return MNCF_OK;
}

mncf_status mncf_engine_hold(mncf_engine *e, const char *const *buttons, size_t count)
{
    if (e == NULL || (buttons == NULL && count > 0)) {
        return MNCF_ERR_ARGUMENT;
    }
    return combo_set(e, &e->held, buttons, count) ? MNCF_OK : MNCF_ERR_MEMORY;
}

mncf_status mncf_engine_press(mncf_engine *e, const char *combination)
{
    mn_combo c;
    if (e == NULL || combination == NULL) {
        return MNCF_ERR_ARGUMENT;
    }
    if (!mn_combo_parse(&e->presses_arena, combination, strlen(combination), &c) ||
        MN_GROW(&e->presses_arena, e->presses, e->press_count, e->press_cap) == NULL) {
        return MNCF_ERR_MEMORY;
    }
    e->presses[e->press_count++] = c;
    return MNCF_OK;
}

mncf_state mncf_engine_state(const mncf_engine *e, size_t index, const char **reason)
{
    if (e == NULL || index >= e->n) {
        if (reason != NULL) {
            *reason = "";
        }
        return MNCF_STATE_OFF;
    }
    if (reason != NULL) {
        *reason = e->runs[index].reason;
    }
    return e->runs[index].state;
}

const char *mncf_engine_build(const mncf_engine *e)
{
    return e != NULL ? e->ev.build : NULL;
}

/* ---- Reading guest memory, each place once a pass --------------------------------------------------- */

static size_t read_slot(uint64_t a, size_t size, size_t cap)
{
    return (addr_hash(a) ^ (size * 0x9E3779B97F4A7C15ULL)) & (cap - 1);
}

/* The bytes at a place as this pass reads them, or NULL where they cannot be read. */
static const uint8_t *pass_read(mncf_engine *e, uint64_t address, size_t size)
{
    size_t i;
    pass_read_entry *r;
    if ((e->read_count + 1) * 2 > e->read_cap) {
        size_t cap = e->read_cap == 0 ? 64 : e->read_cap * 2, k;
        pass_read_entry *grown = (pass_read_entry *)mn_arena_zalloc(&e->pass, cap * sizeof *grown);
        if (grown == NULL) {
            e->failed = 1;
            return NULL;
        }
        for (k = 0; k < e->read_cap; k++) {
            if (e->reads[k].used) {
                size_t j = read_slot(e->reads[k].address, e->reads[k].size, cap);
                while (grown[j].used) {
                    j = (j + 1) & (cap - 1);
                }
                grown[j] = e->reads[k];
            }
        }
        e->reads = grown;
        e->read_cap = cap;
    }
    i = read_slot(address, size, e->read_cap);
    for (;;) {
        r = &e->reads[i];
        if (!r->used) {
            break;
        }
        if (r->address == address && r->size == size) {
            return r->ok ? r->data : NULL;
        }
        i = (i + 1) & (e->read_cap - 1);
    }
    r->used = 1;
    r->address = address;
    r->size = size;
    r->data = (uint8_t *)mn_arena_alloc(&e->pass, size == 0 ? 1 : size);
    e->read_count++;
    if (r->data == NULL) {
        e->failed = 1;
        r->ok = 0;
        return NULL;
    }
    if (e->read != NULL) {
        r->ok = e->read(e->read_ctx, address, r->data, size) ? 1 : 0;
    } else {
        r->ok = mn_runtime_read(&e->ev.rt, address, r->data, size);
    }
    return r->ok ? r->data : NULL;
}

static int pass_reader(void *ctx, uint64_t address, uint8_t *out, size_t size)
{
    const uint8_t *data = pass_read((mncf_engine *)ctx, address, size);
    if (data == NULL) {
        return 0;
    }
    if (size > 0) {
        memcpy(out, data, size);
    }
    return 1;
}

/* ---- A pass ----------------------------------------------------------------------------------------- */

static int combo_held(const mn_combo *combo, const mn_combo *held)
{
    return mn_combo_subset(combo, held);
}

/* Whether a combination of the file was pressed since the last pass (section 7.3). */
static int was_pressed(const mncf_engine *e, const mn_combo *combo)
{
    size_t i;
    if (combo_held(combo, &e->held) && !combo_held(combo, &e->held_before)) {
        return 1;
    }
    for (i = 0; i < e->press_count; i++) {
        if (mn_combo_subset(combo, &e->presses[i])) {
            return 1;
        }
    }
    return 0;
}

static mncf_change *add_change(mncf_engine *e, work *w)
{
    mncf_change *c;
    if (MN_GROW(&e->pass, w->out, w->out_count, w->out_cap) == NULL) {
        e->failed = 1;
        return NULL;
    }
    c = &w->out[w->out_count++];
    memset(c, 0, sizeof *c);
    return c;
}

static int holds(int op, const mn_num *a, const mn_num *v, const mn_num *low, const mn_num *high)
{
    int c;
    if (op == MNCF_COMPARE_BETWEEN) {
        int lo = mn_num_cmp(low, a), hi = mn_num_cmp(a, high);
        return (lo == -1 || lo == 0) && (hi == -1 || hi == 0);
    }
    if (op == MNCF_COMPARE_AND || op == MNCF_COMPARE_NAND) {
        int both_negative = a->negative && v->negative && v->kind != MNCF_NUMBER_BOOL;
        int any = (mn_num_low64(a) & mn_num_low64(v)) != 0 || both_negative;
        return op == MNCF_COMPARE_AND ? any : !any;
    }
    c = mn_num_cmp(a, v);
    switch (op) {
    case MNCF_COMPARE_EQ:
        return c == 0;
    case MNCF_COMPARE_NE:
        return c != 0;
    case MNCF_COMPARE_LT:
        return c == -1;
    case MNCF_COMPARE_LE:
        return c == -1 || c == 0;
    case MNCF_COMPARE_GT:
        return c == 1;
    default:
        return c == 1 || c == 0;
    }
}

/* Whether the cheat's conditions hold now, and what its changed conditions read: 1 held, 0 not, -1 a place
 * could not be read. Every condition is tested. */
static int test(mncf_engine *e, size_t ci, const mn_result *own, const work *w, uint8_t *last, unsigned char *has_last)
{
    const mn_cheat *c = cheat_at(e, ci);
    const run *r = &e->runs[ci];
    int big = e->f->platform->big_endian;
    size_t i;
    int any = 0, all = 1, count = 0;
    for (i = 0; i < own->plan_count; i++) {
        const mn_plan *p = &own->plans[i];
        size_t size = mn_type_size(p->type);
        const uint8_t *raw;
        int result;
        if (p->kind != MNCF_OP_WHEN) {
            continue;
        }
        raw = pass_read(e, w->places[p->index][0].address, size);
        if (raw == NULL) {
            return -1;
        }
        if (p->op->compare == MNCF_COMPARE_CHANGED) {
            memcpy(last + 8 * p->index, raw, size);
            has_last[p->index] = 1;
            result = r->has_last[p->index] && memcmp(r->last + 8 * p->index, raw, size) != 0;
        } else {
            mn_num a = mn_decode_value(p->type, raw, big);
            mn_num v = p->value;
            if (p->value_kind == VAL_PLACE) {
                size_t osize = mn_type_size(p->op->other_type);
                const uint8_t *other = pass_read(e, w->places[p->index][1].address, osize);
                if (other == NULL) {
                    return -1;
                }
                v = mn_decode_value(p->op->other_type, other, big);
            }
            if (v.kind == MNCF_NUMBER_BOOL) {
                v = mn_num_u64(v.magnitude);
            }
            result = holds(p->op->compare, &a, &v, &p->op->low, &p->op->high);
        }
        count++;
        any |= result;
        all &= result;
    }
    if (count == 0) {
        return 1;
    }
    return c->pub.match_any ? any : all;
}

static work *wait(run *r, work *w, const char *reason)
{
    r->state = MNCF_STATE_WAITING;
    r->reason = reason;
    w->active = 0;
    w->keep = 0;
    w->out_count = 0;
    w->shown_count = 0;
    w->acted = 0;
    return w;
}

/* What a cheat that is on does in this pass, and its state after it. */
static int do_work(mncf_engine *e, size_t ci, const unsigned char *pressed_press)
{
    const mn_cheat *c = cheat_at(e, ci);
    run *r = &e->runs[ci];
    const mn_result *own = e->own[ci];
    work *w = &e->work[ci];
    int pressing, result, acts;
    size_t i;
    uint8_t *last;
    unsigned char *has_last;
    memset(w, 0, sizeof *w);
    w->present = 1;
    r->passes++;
    if (own->uses == MNCF_USES_RAW) {
        mncf_change *ch = add_change(e, w);
        if (ch == NULL) {
            return 0;
        }
        r->state = MNCF_STATE_ON;
        r->reason = "";
        ch->kind = MNCF_CHANGE_RAW;
        ch->cheat = c->pub.id;
        ch->syntax = own->raw_entry->syntax;
        ch->build = own->raw_entry->build;
        ch->code = own->raw_entry->code;
        w->acted = 1;
        return 1;
    }
    if ((r->passes - 1) % (uint64_t)c->pub.interval != 0) {
        w->active = r->active;
        w->keep = 1;
        return 1;
    }
    pressing = c->pub.run == MNCF_RUN_ACTION && pressed_press[ci];
    if (mn_waits_for_buttons(c, &e->held, r->pressed || pressing)) {
        wait(r, w, "");
        return 1;
    }
    w->places = (mn_place(*)[2])mn_arena_zalloc(&e->pass, (own->plan_count + 1) * sizeof *w->places);
    w->checked = (const uint8_t **)mn_arena_zalloc(&e->pass, (own->plan_count + 1) * sizeof *w->checked);
    last = (uint8_t *)mn_arena_zalloc(&e->pass, (own->plan_count + 1) * 8);
    has_last = (unsigned char *)mn_arena_zalloc(&e->pass, own->plan_count + 1);
    if (w->places == NULL || w->checked == NULL || last == NULL || has_last == NULL) {
        e->failed = 1;
        return 0;
    }
    for (i = 0; i < own->plan_count; i++) {
        const mn_plan *p = &own->plans[i];
        size_t k;
        for (k = 0; k < p->target_count; k++) {
            if (!mn_follow(e->f->platform, &p->targets[k], pass_reader, e, &e->alloc, &w->places[p->index][k])) {
                wait(r, w, "path-broken");
                return !e->failed;
            }
        }
    }
    result = test(e, ci, own, w, last, has_last);
    if (result < 0) {
        wait(r, w, "path-broken");
        return !e->failed;
    }
    acts = result && (!c->pub.trigger_enter || !r->held_before) && (c->pub.run != MNCF_RUN_ACTION || pressing);
    if (acts) {
        int big = e->f->platform->big_endian;
        for (i = 0; i < own->plan_count; i++) {
            const mn_plan *p = &own->plans[i];
            uint64_t address = w->places[p->index][0].address;
            if (p->kind == MNCF_OP_WRITE) {
                mncf_change *ch;
                if (p->expect != NULL) {
                    const uint8_t *there = pass_read(e, address, p->expect_len);
                    if (there == NULL) {
                        wait(r, w, "path-broken");
                        return !e->failed;
                    }
                    if (memcmp(there, p->expect, p->expect_len) != 0) {
                        continue;
                    }
                }
                ch = add_change(e, w);
                if (ch == NULL) {
                    return 0;
                }
                ch->kind = MNCF_CHANGE_WRITE;
                ch->address = address;
                ch->bytes = (const uint8_t *)mn_arena_dup(&e->pass, p->data, p->data_len);
                ch->size = p->data_len;
                ch->cheat = c->pub.id;
                if (ch->bytes == NULL) {
                    e->failed = 1;
                    return 0;
                }
            } else if (p->kind == MNCF_OP_READ) {
                size_t size = mn_type_size(p->type);
                const uint8_t *raw = pass_read(e, address, size);
                mncf_shown *s;
                if (raw == NULL) {
                    wait(r, w, "path-broken");
                    return !e->failed;
                }
                if (MN_GROW(&e->pass, w->shown, w->shown_count, w->shown_cap) == NULL) {
                    e->failed = 1;
                    return 0;
                }
                s = &w->shown[w->shown_count++];
                s->cheat = c->pub.id;
                s->label = p->label;
                s->type = p->type;
                s->value = mn_decode_value(p->type, raw, big);
            }
        }
    }
    r->held_before = result;
    memcpy(r->last, last, own->plan_count * 8);
    memcpy(r->has_last, has_last, own->plan_count);
    if (acts && c->pub.run == MNCF_RUN_ACTION) {
        r->pressed = 1;
    }
    r->state = c->pub.run == MNCF_RUN_ACTION && !r->pressed ? MNCF_STATE_WAITING : MNCF_STATE_ON;
    r->reason = "";
    w->active = result && (c->pub.run != MNCF_RUN_ACTION || r->pressed);
    w->acted = acts;
    return 1;
}

static int is_code(const mn_plan *p)
{
    return p->kind == MNCF_OP_PATCH || p->kind == MNCF_OP_OVERRIDE;
}

/* Checks expect for every patch and stub that covers a place it did not cover in the pass before, and
 * refuses the cheats where it differs. */
static int check(mncf_engine *e)
{
    size_t ci;
    int any = 0;
    for (ci = 0; ci < e->n; ci++) {
        work *w = &e->work[ci];
        run *r = &e->runs[ci];
        const mn_result *own = e->own[ci];
        const char *refusal = NULL;
        size_t i;
        if (!w->present || !w->active || w->keep) {
            continue;
        }
        for (i = 0; i < own->plan_count && refusal == NULL; i++) {
            const mn_plan *p = &own->plans[i];
            uint64_t address;
            const cover *cv = &r->covers[p->index];
            size_t n = p->expect_len, k;
            const uint8_t *there = NULL;
            uint8_t *found;
            int all_kept = 1;
            if (!is_code(p) || p->data == NULL) {
                continue;
            }
            address = w->places[p->index][0].address;
            if (cv->set && cv->address == address && cv->len == p->data_len) {
                continue;
            }
            for (k = 0; k < n && all_kept; k++) {
                all_kept = kept_find(&e->kept, address + k) != NULL;
            }
            if (!all_kept) {
                there = pass_read(e, address, n);
                if (there == NULL) {
                    if (e->failed) {
                        return 0;
                    }
                    refusal = "outside";
                    break;
                }
            }
            found = (uint8_t *)mn_arena_alloc(&e->pass, n == 0 ? 1 : n);
            if (found == NULL) {
                e->failed = 1;
                return 0;
            }
            for (k = 0; k < n; k++) {
                const kept_slot *s = kept_find(&e->kept, address + k);
                found[k] = s != NULL ? s->kept : (there != NULL ? there[k] : 0);
            }
            if (memcmp(found, p->expect, n) != 0 &&
                (p->kind == MNCF_OP_OVERRIDE || p->data_len != n || memcmp(found, p->data, n) != 0)) {
                refusal = "expect-mismatch";
                break;
            }
            w->checked[p->index] = found;
        }
        if (refusal != NULL) {
            if (!replace_own(e, ci, mn_refused(&e->ev, refusal))) {
                return 0;
            }
            any = 1;
        }
    }
    if (any) {
        return refresh(e);
    }
    return 1;
}

typedef struct range {
    uint64_t address;
    size_t len;
} range;

typedef struct marked {
    uint64_t address;
    int patch;
} marked;

static int cmp_marked(const void *a, const void *b)
{
    uint64_t x = ((const marked *)a)->address, y = ((const marked *)b)->address;
    return x < y ? -1 : x > y;
}

typedef struct hook_in {
    uint64_t address;
    size_t order;
    int action;
    int has_value;
    mn_num value;
} hook_in;

static int cmp_hook_in(const void *a, const void *b)
{
    const hook_in *x = (const hook_in *)a, *y = (const hook_in *)b;
    if (x->address != y->address) {
        return x->address < y->address ? -1 : 1;
    }
    return x->order < y->order ? -1 : x->order > y->order;
}

static int same_hook(const hook *h, const hook_in *n)
{
    if (h->action != n->action || h->has_value != n->has_value) {
        return 0;
    }
    return !h->has_value || mn_num_eq(&h->value, &n->value);
}

typedef struct code_change {
    uint64_t address;
    int order;          /* 0 a byte change, 1 a hook */
    mncf_change change;
} code_change;

static int cmp_code(const void *a, const void *b)
{
    const code_change *x = (const code_change *)a, *y = (const code_change *)b;
    if (x->address != y->address) {
        return x->address < y->address ? -1 : 1;
    }
    return x->order - y->order;
}

/* The code changes of a pass (section 11.5). */
static int code(mncf_engine *e, mncf_change **out, size_t *out_count)
{
    byte_map want, new_kept, new_now;
    range *ranges = NULL;
    size_t range_count = 0, range_cap = 0;
    hook_in *hooks = NULL;
    size_t hook_count = 0, hook_cap = 0;
    marked *spots = NULL;
    size_t mark_count = 0, mark_cap = 0;
    code_change *changes = NULL;
    size_t change_count = 0, change_cap = 0;
    size_t ci, i, k;
    memset(&want, 0, sizeof want);
    memset(&new_kept, 0, sizeof new_kept);
    memset(&new_now, 0, sizeof new_now);
    for (ci = 0; ci < e->n; ci++) {
        work *w = e->work != NULL ? &e->work[ci] : NULL;
        run *r = &e->runs[ci];
        const mn_result *own = e->own[ci];
        if (w == NULL || !w->present || !w->active) {
            r->active = 0;
            covers_clear(e, r->covers, r->ops);
            memset(r->hooks, 0, (r->ops + 1) * sizeof *r->hooks);
            continue;
        }
        covers_clear(e, r->covers_next, r->ops);
        memset(r->hooks_next, 0, (r->ops + 1) * sizeof *r->hooks_next);
        for (i = 0; i < own->plan_count; i++) {
            const mn_plan *p = &own->plans[i];
            uint64_t address;
            const uint8_t *data;
            size_t len;
            cover *next = &r->covers_next[p->index];
            if (!is_code(p)) {
                continue;
            }
            if (w->keep) {
                if (p->data == NULL) {
                    const hook *h = &r->hooks[p->index];
                    if (h->set) {
                        if (MN_GROW(&e->pass, hooks, hook_count, hook_cap) == NULL) {
                            goto nomem;
                        }
                        hooks[hook_count].address = h->address;
                        hooks[hook_count].order = hook_count;
                        hooks[hook_count].action = h->action;
                        hooks[hook_count].has_value = h->has_value;
                        hooks[hook_count].value = h->value;
                        hook_count++;
                        r->hooks_next[p->index] = *h;
                    }
                    continue;
                }
                if (!r->covers[p->index].set) {
                    continue;
                }
                /* the cover moves to the next pass with its own bytes */
                *next = r->covers[p->index];
                memset(&r->covers[p->index], 0, sizeof r->covers[p->index]);
                address = next->address;
                data = next->data;
                len = next->len;
            } else {
                address = w->places[p->index][0].address;
                data = p->data;
                len = p->data_len;
                if (data != NULL) {
                    next->data = (uint8_t *)mn_malloc(&e->alloc, len == 0 ? 1 : len);
                    if (next->data == NULL) {
                        goto nomem;
                    }
                    memcpy(next->data, data, len);
                    next->set = 1;
                    next->address = address;
                    next->len = len;
                    data = next->data;
                }
            }
            if (data == NULL) {
                hook *h = &r->hooks_next[p->index];
                if (MN_GROW(&e->pass, hooks, hook_count, hook_cap) == NULL) {
                    goto nomem;
                }
                hooks[hook_count].address = address;
                hooks[hook_count].order = hook_count;
                hooks[hook_count].action = p->action;
                hooks[hook_count].has_value = p->has_value;
                hooks[hook_count].value = p->value;
                hook_count++;
                h->set = 1;
                h->address = address;
                h->action = p->action;
                h->has_value = p->has_value;
                h->value = p->value;
                continue;
            }
            {
                const uint8_t *found = w->keep ? NULL : w->checked[p->index];
                for (k = 0; k < len; k++) {
                    uint64_t a = address + k;
                    if (!bm_put(&e->pass, &want, a, data[k])) {
                        goto nomem;
                    }
                    if (kept_find(&e->kept, a) == NULL && !bm_get(&new_kept, a, NULL)) {
                        if (!bm_put(&e->pass, &new_kept, a, p->expect[k]) ||
                            !bm_put(&e->pass, &new_now, a, found != NULL ? found[k] : p->expect[k])) {
                            goto nomem;
                        }
                    }
                }
            }
            if (MN_GROW(&e->pass, ranges, range_count, range_cap) == NULL) {
                goto nomem;
            }
            ranges[range_count].address = address;
            ranges[range_count].len = len;
            range_count++;
        }
        r->active = 1;
        {
            cover *t = r->covers;
            hook *h = r->hooks;
            covers_clear(e, r->covers, r->ops);
            r->covers = r->covers_next;
            r->covers_next = t;
            r->hooks = r->hooks_next;
            r->hooks_next = h;
        }
    }
    for (i = 0; i < new_kept.cap; i++) {
        if (new_kept.used[i]) {
            uint8_t now = 0;
            bm_get(&new_now, new_kept.keys[i], &now);
            if (!kept_put(&e->kept, &e->alloc, new_kept.keys[i], new_kept.vals[i], now)) {
                goto nomem;
            }
        }
    }
    {
        byte_map is_marked;
        memset(&is_marked, 0, sizeof is_marked);
        for (i = 0; i < range_count; i++) {
            int differs = 0;
            for (k = 0; k < ranges[i].len && !differs; k++) {
                uint64_t a = ranges[i].address + k;
                uint8_t v = 0;
                const kept_slot *s = kept_find(&e->kept, a);
                bm_get(&want, a, &v);
                differs = s == NULL || v != s->now;
            }
            if (!differs) {
                continue;
            }
            for (k = 0; k < ranges[i].len; k++) {
                uint64_t a = ranges[i].address + k;
                if (!bm_get(&is_marked, a, NULL)) {
                    if (!bm_put(&e->pass, &is_marked, a, 1) || MN_GROW(&e->pass, spots, mark_count, mark_cap) == NULL) {
                        goto nomem;
                    }
                    spots[mark_count].address = a;
                    spots[mark_count].patch = 1;
                    mark_count++;
                }
            }
        }
        for (i = 0; i < e->kept.cap; i++) {
            const kept_slot *s = &e->kept.slots[i];
            if (s->state == 1 && !bm_get(&want, s->address, NULL)) {
                if (MN_GROW(&e->pass, spots, mark_count, mark_cap) == NULL) {
                    goto nomem;
                }
                spots[mark_count].address = s->address;
                spots[mark_count].patch = 0;
                mark_count++;
            }
        }
    }
    if (mark_count > 1) {
        qsort(spots, mark_count, sizeof *spots, cmp_marked);
    }
    {
        size_t start = 0;
        while (start < mark_count) {
            size_t end = start + 1;
            int differs = 0;
            uint8_t *bytes;
            while (end < mark_count && spots[end].patch == spots[start].patch &&
                   spots[end].address > spots[end - 1].address && spots[end].address - spots[end - 1].address == 1) {
                end++;
            }
            bytes = (uint8_t *)mn_arena_alloc(&e->pass, end - start);
            if (bytes == NULL) {
                goto nomem;
            }
            for (k = start; k < end; k++) {
                const kept_slot *s = kept_find(&e->kept, spots[k].address);
                uint8_t target = 0;
                if (spots[k].patch) {
                    bm_get(&want, spots[k].address, &target);
                } else if (s != NULL) {
                    target = s->kept;
                }
                bytes[k - start] = target;
                if (s == NULL || target != s->now) {
                    differs = 1;
                }
            }
            if (differs) {
                code_change *cc;
                if (MN_GROW(&e->pass, changes, change_count, change_cap) == NULL) {
                    goto nomem;
                }
                cc = &changes[change_count++];
                memset(cc, 0, sizeof *cc);
                cc->address = spots[start].address;
                cc->order = 0;
                cc->change.kind = spots[start].patch ? MNCF_CHANGE_PATCH : MNCF_CHANGE_RESTORE;
                cc->change.address = spots[start].address;
                cc->change.bytes = bytes;
                cc->change.size = end - start;
            }
            start = end;
        }
    }
    for (i = 0; i < mark_count; i++) {
        kept_slot *s = kept_find(&e->kept, spots[i].address);
        if (spots[i].patch) {
            if (s != NULL) {
                bm_get(&want, spots[i].address, &s->now);
            }
        } else {
            kept_del(&e->kept, spots[i].address);
        }
    }
    /* Hooks: the last cheat in file order wins at an address. */
    if (hook_count > 1) {
        qsort(hooks, hook_count, sizeof *hooks, cmp_hook_in);
    }
    k = 0;
    for (i = 0; i < hook_count; i++) {
        if (k > 0 && hooks[k - 1].address == hooks[i].address) {
            hooks[k - 1] = hooks[i];
        } else {
            hooks[k++] = hooks[i];
        }
    }
    hook_count = k;
    {
        size_t a = 0, b = 0;
        hook *next = (hook *)mn_calloc(&e->alloc, (hook_count + 1) * sizeof *next);
        if (next == NULL) {
            goto nomem;
        }
        while (a < e->hooked_count || b < hook_count) {
            code_change *cc;
            int take_old = b == hook_count || (a < e->hooked_count && e->hooked[a].address < hooks[b].address);
            int take_new = a == e->hooked_count || (b < hook_count && hooks[b].address < e->hooked[a].address);
            if (MN_GROW(&e->pass, changes, change_count, change_cap) == NULL) {
                mn_free(&e->alloc, next, (hook_count + 1) * sizeof *next);
                goto nomem;
            }
            if (take_old) {
                cc = &changes[change_count++];
                memset(cc, 0, sizeof *cc);
                cc->address = e->hooked[a].address;
                cc->order = 1;
                cc->change.kind = MNCF_CHANGE_UNHOOK;
                cc->change.address = e->hooked[a].address;
                a++;
                continue;
            }
            if (take_new || !same_hook(&e->hooked[a], &hooks[b])) {
                cc = &changes[change_count++];
                memset(cc, 0, sizeof *cc);
                cc->address = hooks[b].address;
                cc->order = 1;
                cc->change.kind = MNCF_CHANGE_HOOK;
                cc->change.address = hooks[b].address;
                cc->change.action = (mncf_action)hooks[b].action;
                cc->change.has_value = hooks[b].has_value;
                cc->change.value = hooks[b].value;
            }
            if (!take_new) {
                a++;
            }
            b++;
        }
        for (i = 0; i < hook_count; i++) {
            next[i].set = 1;
            next[i].address = hooks[i].address;
            next[i].action = hooks[i].action;
            next[i].has_value = hooks[i].has_value;
            next[i].value = hooks[i].value;
        }
        mn_free(&e->alloc, e->hooked, (e->hooked_count + 1) * sizeof *e->hooked);
        e->hooked = next;
        e->hooked_count = hook_count;
    }
    if (change_count > 1) {
        qsort(changes, change_count, sizeof *changes, cmp_code);
    }
    *out = (mncf_change *)mn_arena_alloc(&e->pass, (change_count + 1) * sizeof **out);
    if (*out == NULL) {
        goto nomem;
    }
    for (i = 0; i < change_count; i++) {
        (*out)[i] = changes[i].change;
    }
    *out_count = change_count;
    return 1;
nomem:
    e->failed = 1;
    return 0;
}

/* The pass's changes: the code changes, then each cheat's writes and raw entries, in file order. */
static int finish(mncf_engine *e, mncf_change *codes, size_t code_count, mncf_pass *out)
{
    size_t total = code_count, shown = 0, ci, i;
    mncf_change *changes;
    mncf_shown *values;
    for (ci = 0; e->work != NULL && ci < e->n; ci++) {
        if (e->work[ci].present) {
            total += e->work[ci].out_count;
            shown += e->work[ci].shown_count;
        }
    }
    changes = (mncf_change *)mn_arena_alloc(&e->pass, (total + 1) * sizeof *changes);
    values = (mncf_shown *)mn_arena_alloc(&e->pass, (shown + 1) * sizeof *values);
    if (changes == NULL || values == NULL) {
        e->failed = 1;
        return 0;
    }
    memcpy(changes, codes, code_count * sizeof *changes);
    total = code_count;
    shown = 0;
    for (ci = 0; e->work != NULL && ci < e->n; ci++) {
        const work *w = &e->work[ci];
        if (!w->present) {
            continue;
        }
        for (i = 0; i < w->out_count; i++) {
            changes[total++] = w->out[i];
        }
        for (i = 0; i < w->shown_count; i++) {
            values[shown++] = w->shown[i];
        }
    }
    out->number = e->number;
    out->changes = changes;
    out->change_count = total;
    out->shown = values;
    out->shown_count = shown;
    return 1;
}

static mncf_status failed(mncf_engine *e, mncf_pass *out)
{
    memset(out, 0, sizeof *out);
    e->failed = 0;
    return MNCF_ERR_MEMORY;
}

mncf_status mncf_engine_tick(mncf_engine *e, mncf_read_fn read, void *read_ctx, const double *now_p, mncf_pass *out)
{
    double now;
    unsigned char *pressed_toggle, *pressed_press;
    mncf_change *codes = NULL;
    size_t code_count = 0, ci;
    if (e == NULL || out == NULL) {
        return MNCF_ERR_ARGUMENT;
    }
    memset(out, 0, sizeof *out);
    mn_arena_reset(&e->pass);
    e->reads = NULL;
    e->read_cap = 0;
    e->read_count = 0;
    e->work = NULL;
    e->number++;
    now = now_p != NULL ? *now_p : (double)(e->number - 1) / e->rate;
    if (read != NULL) {
        e->read = read;
        e->read_ctx = read_ctx;
    } else {
        e->read = e->ev.rt.read;
        e->read_ctx = e->ev.rt.read_ctx;
    }
    pressed_toggle = (unsigned char *)mn_arena_zalloc(&e->pass, e->n + 1);
    pressed_press = (unsigned char *)mn_arena_zalloc(&e->pass, e->n + 1);
    e->work = (work *)mn_arena_zalloc(&e->pass, (e->n + 1) * sizeof *e->work);
    if (pressed_toggle == NULL || pressed_press == NULL || e->work == NULL) {
        return failed(e, out);
    }
    for (ci = 0; ci < e->n; ci++) {
        const mn_cheat *c = cheat_at(e, ci);
        if (c->has_toggle) {
            pressed_toggle[ci] = (unsigned char)was_pressed(e, &c->toggle);
        }
        if (c->has_press) {
            pressed_press[ci] = (unsigned char)was_pressed(e, &c->press);
        }
    }
    if (!combo_set(e, &e->held_before, e->held.names, e->held.count)) {
        return failed(e, out);
    }
    mn_arena_reset(&e->presses_arena);
    e->presses = NULL;
    e->press_count = 0;
    e->press_cap = 0;

    for (ci = 0; ci < e->n; ci++) {                       /* toggles, in file order */
        const mn_cheat *c = cheat_at(e, ci);
        if (c->has_toggle && !c->pub.master && e->runs[ci].refused == NULL && pressed_toggle[ci] &&
            !switch_cheat(e, ci, !e->on.is[ci], (size_t)-1)) {
            return failed(e, out);
        }
    }
    for (ci = 0; ci < e->n; ci++) {                       /* timed cheats whose time is over */
        const mn_cheat *c = cheat_at(e, ci);
        run *r = &e->runs[ci];
        if (c->pub.run == MNCF_RUN_TIMED && e->on.is[ci]) {
            if (!r->has_started) {
                r->has_started = 1;
                r->started = now;
            }
            if (now - r->started >= c->pub.duration && !switch_cheat(e, ci, 0, ci)) {
                return failed(e, out);
            }
        }
    }
    for (ci = 0; ci < e->n; ci++) {
        if (e->on.is[ci] && e->runs[ci].refused == NULL && !do_work(e, ci, pressed_press)) {
            return failed(e, out);
        }
    }
    if (!check(e)) {
        return failed(e, out);
    }
    for (ci = 0; ci < e->n; ci++) {                       /* what a refusal in this pass switched off */
        if (e->work[ci].present && (!e->on.is[ci] || e->runs[ci].refused != NULL)) {
            e->work[ci].present = 0;
        }
    }
    if (!code(e, &codes, &code_count) || !finish(e, codes, code_count, out)) {
        return failed(e, out);
    }
    for (ci = 0; ci < e->n; ci++) {                       /* once cheats that did their work */
        const mn_cheat *c = cheat_at(e, ci);
        if (c->pub.run == MNCF_RUN_ONCE && e->work[ci].present && e->work[ci].acted && e->on.is[ci] &&
            !switch_cheat(e, ci, 0, ci)) {
            return failed(e, out);
        }
    }
    if (e->failed || e->ev.failed) {
        return failed(e, out);
    }
    return MNCF_OK;
}

mncf_status mncf_engine_stop(mncf_engine *e, mncf_pass *out)
{
    unsigned char *before;
    mncf_change *codes = NULL;
    size_t code_count = 0, i;
    if (e == NULL || out == NULL) {
        return MNCF_ERR_ARGUMENT;
    }
    memset(out, 0, sizeof *out);
    mn_arena_reset(&e->pass);
    e->reads = NULL;
    e->read_cap = 0;
    e->read_count = 0;
    e->work = NULL;
    before = flags_copy(e);
    if (before == NULL) {
        return failed(e, out);
    }
    for (i = 0; i < e->on.count; i++) {
        e->on.is[e->on.items[i]] = 0;
    }
    e->on.count = 0;
    set_on_after(e, before, (size_t)-1);
    mn_free(&e->alloc, before, e->n + 1);
    if (!code(e, &codes, &code_count) || !finish(e, codes, code_count, out)) {
        return failed(e, out);
    }
    out->shown = NULL;
    out->shown_count = 0;
    return MNCF_OK;
}
