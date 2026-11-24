/* evaluate.h: what the evaluator and the pass engine share. Not part of the API. MIT License. */
#ifndef MNCF_EVALUATE_H
#define MNCF_EVALUATE_H

#include "encode.h"
#include "model.h"

#define MN_BUF_SIZE MN_STUB_MAX

/* A place an operation acts on: its address and, for a place without a pointer path in a module, that
 * module and the offset in it. */
typedef struct mn_place {
    uint64_t address;
    const mncf_module *module;     /* the module whose own bytes the place is in, or NULL */
    const char *module_name;
    uint64_t offset;
} mn_place;

/* A place before its pointer path is followed, and how many bytes the operation reads or writes. */
typedef struct mn_target {
    mn_place base;
    const uint64_t *path;
    size_t path_len;
    size_t size;
} mn_target;

/* One operation of a cheat, resolved as far as it can be without reading guest memory that changes. */
typedef struct mn_plan {
    int kind;                      /* MNCF_OP_* */
    size_t index;
    const mn_op *op;
    mn_target targets[2];
    size_t target_count;
    const uint8_t *data;           /* what a patch, a write or a stub writes; NULL for a hooked override */
    size_t data_len;
    const uint8_t *expect;
    size_t expect_len;
    mncf_type type;
    int action;
    int has_value;
    mn_num value;                  /* an override's value, a condition's number */
    int value_kind;                /* a condition's: VAL_NONE, VAL_NUMBER, VAL_RANGE, VAL_PLACE */
    const char *label;
    int path_expect;               /* a patch or stub behind a pointer path: expect is checked there */
    uint8_t buf[MN_BUF_SIZE];      /* room for a computed value or stub */
    uint8_t ebuf[8];               /* room for an encoded expect */
} mn_plan;

/* A cheat's own result before its relations to other cheats are looked at. */
typedef struct mn_result {
    mncf_state state;
    const char *reason;            /* interned, or "" */
    mncf_uses uses;
    mn_plan *plans;
    size_t plan_count;
    const mn_raw *raw_entry;
    int path_broken;
    mncf_op *ops;                  /* resolved operations, where asked for */
    size_t op_count;
    size_t plans_size, ops_size;   /* the bytes allocated for plans and ops */
} mn_result;

/* A value the player chose, by "cheat.value". */
typedef struct mn_value {
    const char *key;
    size_t key_len;
    mn_num value;
} mn_value;

typedef struct mn_cache_entry {
    const mncf_module *module;
    const mn_loc *loc;
    size_t count;                  /* 0, 1 or 2 (two or more) */
    uint64_t at;
} mn_cache_entry;

typedef struct mn_eval {
    const mncf_file *f;
    mncf_runtime rt;
    const char *build;             /* the build that runs, or NULL */
    const char *arch;
    size_t arch_len;
    mn_alloc alloc;
    mn_arena *keep;                /* reason strings and the pattern cache, kept as long as the evaluator */
    mn_cache_entry *cache;
    size_t cache_count, cache_cap;
    const char **interned;
    size_t interned_count, interned_cap;
    const mn_value *values;
    size_t value_count;
    int want_ops;                  /* make resolved operations, for mncf_evaluate */
    int failed;                    /* no memory */
} mn_eval;

int mn_eval_init(mn_eval *ev, const mncf_file *f, const mncf_runtime *rt, mn_alloc alloc, mn_arena *keep);
/* A cheat's own result for the values; NULL when there was no memory. */
mn_result *mn_own(mn_eval *ev, size_t cheat);
void mn_result_free(mn_eval *ev, mn_result *r);
mn_result *mn_refused(mn_eval *ev, const char *reason);
const char *mn_intern(mn_eval *ev, const char *prefix, const char *name);

/* The results after relations (section 7.8): reasons, by cheat, for cheats refused by a relation, else
 * NULL to keep the cheat's own result. out has a slot per cheat. 0 when there was no memory. */
int mn_relations(mn_eval *ev, mn_result *const *own, const char **out);

/* The list of cheats that are on, in the order they were switched on, with a flag per cheat. */
typedef struct mn_on {
    size_t *items;
    size_t count;
    unsigned char *is;             /* per cheat */
} mn_on;

int mn_on_init(mn_on *on, const mn_alloc *alloc, size_t cheats);
void mn_on_free(mn_on *on, const mn_alloc *alloc, size_t cheats);
int mn_switch_on(mn_eval *ev, mn_on *on, size_t x);
int mn_switch_off(mn_eval *ev, mn_on *on, size_t x);

/* Whether a cheat that is on waits for buttons: held buttons it needs, or the first press of an action. */
int mn_waits_for_buttons(const mn_cheat *c, const mn_combo *held, int pressed_once);

/* The place a target names now, or 0 where its pointer path does not resolve at the moment. */
typedef int (*mn_reader)(void *ctx, uint64_t address, uint8_t *out, size_t size);
int mn_follow(const mn_platform *plat, const mn_target *t, mn_reader read, void *ctx, const mn_alloc *alloc,
              mn_place *out);
/* Switches on the cheats that are on when the file is loaded (section 11.2). */
int mn_load_switch(mn_eval *ev, const mncf_choices *ch, const unsigned char *refused, mn_on *on);
/* Reads through the runtime: its reader, or the modules' bytes. */
int mn_runtime_read(const mncf_runtime *rt, uint64_t address, uint8_t *out, size_t size);

#endif /* MNCF_EVALUATE_H */
