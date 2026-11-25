/* model.h: a file after it was read and checked, as the evaluator and the engine use it. MIT License. */
#ifndef MNCF_MODEL_H
#define MNCF_MODEL_H

#include "internal.h"

enum { LOC_MODULE = 1, LOC_MODULE_BUILDS, LOC_PATTERN, LOC_REGION, LOC_REGION_BUILDS, LOC_ADDRESS };
enum { SEC_NONE = 0, SEC_CODE, SEC_RODATA, SEC_DATA };
enum { VAL_NONE = 0, VAL_NUMBER, VAL_REF, VAL_BYTES, VAL_RANGE, VAL_PLACE };

typedef struct mn_offset {
    mn_str build;
    uint64_t offset;
} mn_offset;

/* A location (section 6.2). */
typedef struct mn_loc {
    int kind;
    mn_str name;               /* the module or region */
    uint64_t address;          /* LOC_ADDRESS */
    uint64_t offset;           /* LOC_MODULE, LOC_REGION */
    int64_t find_offset;       /* LOC_PATTERN */
    mn_offset *offsets;        /* LOC_MODULE_BUILDS, LOC_REGION_BUILDS */
    size_t offset_count;
    int16_t *find;             /* byte values, -1 for ?? */
    size_t find_len;
    int section;
    uint64_t *path;            /* each step's low 64 bits, two's complement */
    size_t path_len;
} mn_loc;

/* One operation: a patch, write, override, read or condition. */
typedef struct mn_op {
    int kind;                  /* MNCF_OP_* */
    size_t index;              /* its position among the cheat's operations */
    mn_loc at;                 /* the place, or the function of an override */
    mncf_type type;
    /* patch: expect and bytes, or encode; write of bytes: bytes and maybe expect; override: expect */
    uint8_t *expect;
    size_t expect_len;
    int has_expect;
    uint8_t *bytes;
    size_t bytes_len;
    int encode;                /* arm64-imm/1 */
    int insn;                  /* 0 movz, 1 movz-movk, 2 fmov */
    int width;
    int rd;
    /* the value: a number, a player value (ref), a range or another place */
    int value_kind;
    mn_num value;
    size_t ref;
    mn_num low, high;
    mn_loc other_at;
    mncf_type other_type;
    int has_num_expect;        /* a write of a number type with expect */
    mn_num num_expect;
    int action;                /* MNCF_ACTION_* */
    const char *label;
    int compare;               /* MNCF_COMPARE_* */
} mn_op;

typedef struct mn_raw {
    const char *syntax;
    const char *build;
    const char *code;
} mn_raw;

/* A button combination: its names, sorted, each once. */
typedef struct mn_combo {
    const char **names;
    size_t count;
    size_t cap;                /* for a combination the engine owns: the size of names, else 0 */
} mn_combo;

typedef struct mn_cheat {
    mncf_cheat pub;
    size_t id_len;
    mn_op *ops;
    size_t op_count, op_cap;
    mn_raw *raw;
    size_t raw_count, raw_cap;
    size_t *requires;          /* indexes of cheats */
    size_t *excludes;
    mn_combo hold, toggle, press;
    int has_hold, has_toggle, has_press;
} mn_cheat;

struct mncf_file {
    mn_alloc alloc;
    mn_arena arena;
    mncf_file_info info;
    const mn_platform *platform;
    mn_cheat *cheats;
    size_t cheat_count;
    mn_index cheat_index;
};

struct mncf_pack {
    mn_alloc alloc;
    mn_arena arena;
    mncf_pack_info info;
    const mncf_toml *doc;         /* the manifest */
    uint8_t **datas;              /* the bytes of each file whose status is ok, on the allocator */
    size_t *sizes;
    size_t count;
};

/* Warnings as they are found, in an arena. */
typedef struct mn_warnings {
    const char **items;
    size_t count, cap;
} mn_warnings;

/* Adds a warning made as printf makes it. 0 on no memory. */
int mn_warn(mn_arena *ar, mn_warnings *w, const char *fmt, ...) MN_PRINTF(3, 4);

/* Builds the model of a parsed document. 0 with err set when the file cannot be used. */
int mn_build_file(mncf_file *f, const mncf_toml *doc, mncf_error *err);
/* The index of the cheat with that id, or (size_t)-1. */
size_t mn_cheat_index(const mncf_file *f, const char *id, size_t n);
int mn_combo_parse(mn_arena *ar, const char *text, size_t n, mn_combo *out);
int mn_combo_subset(const mn_combo *a, const mn_combo *b);       /* every name of a is in b */
int mn_combo_equal(const mn_combo *a, const mn_combo *b);
int mn_value_in_spec(const mncf_value_spec *s, const mn_num *n);
int mn_choices_have(const mncf_value_spec *s, const mn_num *n);
/* Whether a value the player chose fits a value of the file, as the evaluator and section 17.5 check it:
 * MN_FIT_OK, or why not. */
enum { MN_FIT_OK = 0, MN_FIT_TYPE, MN_FIT_RANGE, MN_FIT_CHOICE };
int mn_value_fit(const mncf_value_spec *s, const mn_num *v);
int mn_cheat_id_valid(const char *s, size_t n);                 /* ^[a-z][a-z0-9-]*\Z */
int mn_pack_path_valid(const char *s, size_t n);                /* section 13, not pack.toml */

/* ---- Updates (update.c) ----------------------------------------------------------------------------- */

int mn_version_valid(const char *s, size_t n);
/* -1, 0 or 1 for two versions, which must be valid. */
int mn_version_cmp(const char *a, size_t an, const char *b, size_t bn);
int mn_file_id_valid(const char *s, size_t n);
int mn_date_valid(const char *s, size_t n);
/* The length of the host of an address that section 17.3 allows, and in *start its offset; 0 where it does
 * not allow it. */
size_t mn_url_host(const char *s, size_t n, size_t *start);

/* The keys of section 17.1 of a file or a pack manifest. */
typedef struct mn_update_keys {
    const char *id;
    const char *update_url;
    const char *update_host;
    const mncf_release *changes;   /* newest first */
    size_t change_count;
} mn_update_keys;

/* Reads `signers` (section 12): the keys, or none with the warning signers. 0 on no memory. */
int mn_read_signers(mn_arena *ar, const mncf_toml *doc, mn_warnings *w, const uint8_t (**keys)[32], size_t *count);
/* Reads the keys of section 17.1; what breaks a rule becomes a warning. item checks one name in the lists
 * of a change entry: a cheat id, or a pack's path. 0 on no memory. */
int mn_read_update_keys(mn_arena *ar, const mncf_toml *doc, int (*item)(const char *s, size_t n), mn_warnings *w,
                        mn_update_keys *out);

/* Section 2 for a file's or a manifest's bytes, and the TOML of the format, with the given error code. */
int mn_check_text(const uint8_t *data, size_t size, mncf_status status, const char *code, mncf_error *err);
mncf_toml *mn_parse_document(mn_arena *ar, const uint8_t *data, size_t size, mncf_status status, const char *code,
                             mncf_error *err);

/* Reads a pack with another limit for all the bytes it reads, so that tests can reach it. */
mncf_pack *mn_pack_read(const void *data, size_t size, const mncf_trust *trust, const mncf_allocator *alloc,
                        uint64_t max_total, mncf_error *err);

#endif /* MNCF_MODEL_H */
