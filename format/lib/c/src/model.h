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

/* Builds the model of a parsed document. 0 with err set when the file cannot be used. */
int mn_build_file(mncf_file *f, const mncf_toml *doc, mncf_error *err);
/* The index of the cheat with that id, or (size_t)-1. */
size_t mn_cheat_index(const mncf_file *f, const char *id, size_t n);
int mn_combo_parse(mn_arena *ar, const char *text, size_t n, mn_combo *out);
int mn_combo_subset(const mn_combo *a, const mn_combo *b);       /* every name of a is in b */
int mn_combo_equal(const mn_combo *a, const mn_combo *b);
int mn_value_in_spec(const mncf_value_spec *s, const mn_num *n);
int mn_choices_have(const mncf_value_spec *s, const mn_num *n);

/* Section 2 for a file's or a manifest's bytes, and the TOML of the format, with the given error code. */
int mn_check_text(const uint8_t *data, size_t size, mncf_status status, const char *code, mncf_error *err);
mncf_toml *mn_parse_document(mn_arena *ar, const uint8_t *data, size_t size, mncf_status status, const char *code,
                             mncf_error *err);

/* Reads a pack with another limit for all the bytes it reads, so that tests can reach it. */
mncf_pack *mn_pack_read(const void *data, size_t size, const mncf_trust *trust, const mncf_allocator *alloc,
                        uint64_t max_total, mncf_error *err);

#endif /* MNCF_MODEL_H */
