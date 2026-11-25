/*
 * mncf.h: reads files of the Mistel Native Cheat Format, version 1 (mncf/1).
 *
 * The C library of format/: parse and check a file, check its signature lines against trusted keys, read
 * packs, identify the build that runs, resolve every location, give each cheat its state, reason and the
 * bytes a runtime writes, run the cheats pass by pass (spec, section 11.5), and take new versions of a file
 * without losing what the player set (section 17). C99, no dependencies, no global state. MIT License, as
 * all of format/.
 *
 *     mncf_error err;
 *     mncf_file *f = mncf_load(data, size, trust, NULL, &err);    // NULL: err.code says why
 *     mncf_runtime rt;
 *     mncf_runtime_init(&rt, "switch");
 *     rt.modules = modules; rt.module_count = 1;
 *     mncf_engine *eng = mncf_engine_new(f, &rt, NULL, 60, &err);
 *     mncf_pass pass;
 *     mncf_engine_tick(eng, read_guest, ctx, NULL, &pass);       // the changes of one pass
 *
 * Every object is made by a function of this library and freed by its *_free function. Pointers that an
 * object gives out stay valid until it is freed; the changes of a pass stay valid until the next pass.
 * Structures this header defines are read-only views: the library makes them, and later versions may add
 * fields at their end. Strings are UTF-8 and end with a NUL byte.
 */
#ifndef MNCF_H
#define MNCF_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MNCF_VERSION_MAJOR 1
#define MNCF_VERSION_MINOR 1
#define MNCF_VERSION_PATCH 0
#define MNCF_VERSION "1.1.0"
/* The version as one number, major * 10000 + minor * 100 + patch. */
#define MNCF_VERSION_NUMBER (MNCF_VERSION_MAJOR * 10000 + MNCF_VERSION_MINOR * 100 + MNCF_VERSION_PATCH)

#if defined(MNCF_SHARED) && defined(_WIN32)
#  ifdef MNCF_BUILDING
#    define MNCF_API __declspec(dllexport)
#  else
#    define MNCF_API __declspec(dllimport)
#  endif
#elif defined(MNCF_SHARED) && defined(__GNUC__)
#  define MNCF_API __attribute__((visibility("default")))
#else
#  define MNCF_API
#endif

/* The version of the library that runs, as MNCF_VERSION gives it. */
MNCF_API const char *mncf_version(void);

/* ---- Errors ----------------------------------------------------------------------------------------- */

/* What went wrong. The codes of spec Appendix D, and three of the library's own. */
typedef enum mncf_status {
    MNCF_OK = 0,
    MNCF_ERR_SIZE,           /* "size": a file or pack entry of more than 16 MiB, or a pack too large */
    MNCF_ERR_ENCODING,       /* "encoding": not UTF-8, a byte order mark, or a CR character */
    MNCF_ERR_TOML,           /* "toml": not TOML 1.0, a date or time value, or nested too deep */
    MNCF_ERR_FORMAT,         /* "format": `format` is not the first key, or not the version this reads */
    MNCF_ERR_PLATFORM,       /* "platform": game.platform is not in Appendix A */
    MNCF_ERR_SCHEMA,         /* "schema:<key>": a key outside the cheats is missing or wrong */
    MNCF_ERR_DUPLICATE_ID,   /* "duplicate-id:<id>": two cheats have that id */
    MNCF_ERR_ZIP,            /* "zip": a pack that is not a ZIP file this reader reads */
    MNCF_ERR_MANIFEST,       /* "manifest": no pack.toml, or not UTF-8 TOML with LF line ends */
    MNCF_ERR_PATH,           /* "path:<path>": a pack path that section 13 does not allow */
    MNCF_ERR_MEMORY,         /* "memory": the allocator gave no memory */
    MNCF_ERR_ARGUMENT,       /* "argument": a caller's mistake, such as an unknown cheat or value */
    MNCF_ERR_STATE           /* "state": a call that does not fit the object's state */
} mncf_status;

/* An error as a code and a message. `code` is the full code ("schema:game.id"); a code longer than the
 * buffer is cut at a character boundary. */
typedef struct mncf_error {
    mncf_status status;
    char code[256];
    char message[256];
} mncf_error;

/* ---- Memory ----------------------------------------------------------------------------------------- */

/* One function for all memory, as Lua's allocator: new_size 0 frees ptr and returns NULL; ptr NULL
 * allocates; otherwise it reallocates from old_size to new_size. NULL means no memory. */
typedef void *(*mncf_alloc_fn)(void *ctx, void *ptr, size_t old_size, size_t new_size);

typedef struct mncf_allocator {
    mncf_alloc_fn fn;
    void *ctx;
} mncf_allocator;
/* Every function that takes a `const mncf_allocator *` uses malloc, realloc and free when it is NULL. */

/* ---- Numbers ---------------------------------------------------------------------------------------- */

typedef enum mncf_number_kind {
    MNCF_NUMBER_NONE = 0,    /* not a number: a value a player cannot choose */
    MNCF_NUMBER_INT,
    MNCF_NUMBER_FLOAT,
    MNCF_NUMBER_BOOL
} mncf_number_kind;

/* A number of the format: a whole number from -(2^64 - 1) to 2^64 - 1, a float, or a boolean. Whole
 * numbers and floats compare by their exact values, as the format says (section 7.9). */
typedef struct mncf_number {
    mncf_number_kind kind;
    int negative;            /* INT: the value is -magnitude */
    uint64_t magnitude;      /* INT: the absolute value; BOOL: 0 or 1 */
    double real;             /* FLOAT */
    int huge;                /* INT: the bits above the lowest 64 of the magnitude, for the only whole
                              * numbers above 2^64 - 1: choice keys of 20 decimal digits */
} mncf_number;

MNCF_API mncf_number mncf_number_int(int64_t v);
MNCF_API mncf_number mncf_number_uint(uint64_t v);
MNCF_API mncf_number mncf_number_float(double v);
MNCF_API mncf_number mncf_number_bool(int v);
/* The number as a signed 64-bit value; 0 where it is no whole number that fits. */
MNCF_API int mncf_number_to_int64(mncf_number n, int64_t *out);

/* ---- TOML ------------------------------------------------------------------------------------------- */

/* A TOML 1.0 document, as the library's own parser reads it. Tables keep their keys in document order. */
typedef struct mncf_toml mncf_toml;
typedef struct mncf_toml_doc mncf_toml_doc;

typedef enum mncf_toml_type {
    MNCF_TOML_STRING = 1,
    MNCF_TOML_INTEGER,
    MNCF_TOML_FLOAT,
    MNCF_TOML_BOOLEAN,
    MNCF_TOML_DATETIME,      /* kept as its text; the format refuses them */
    MNCF_TOML_ARRAY,
    MNCF_TOML_TABLE
} mncf_toml_type;

/* Parses TOML text (UTF-8), as Python's tomllib does: a byte order mark is an error too. NULL with err on
 * failure. */
MNCF_API mncf_toml_doc *mncf_toml_parse(const char *text, size_t size, const mncf_allocator *alloc,
                                        mncf_error *err);
MNCF_API void mncf_toml_free(mncf_toml_doc *doc);
MNCF_API const mncf_toml *mncf_toml_root(const mncf_toml_doc *doc);

MNCF_API mncf_toml_type mncf_toml_type_of(const mncf_toml *v);
/* A string's bytes (they may hold NUL bytes) and length; a date-time's text. NULL for other types. */
MNCF_API const char *mncf_toml_string(const mncf_toml *v, size_t *size);
MNCF_API int64_t mncf_toml_integer(const mncf_toml *v);
MNCF_API double mncf_toml_float(const mncf_toml *v);
MNCF_API int mncf_toml_boolean(const mncf_toml *v);
/* The items of an array or the keys of a table. */
MNCF_API size_t mncf_toml_count(const mncf_toml *v);
MNCF_API const mncf_toml *mncf_toml_at(const mncf_toml *v, size_t i);       /* item, or value of key i */
MNCF_API const char *mncf_toml_key(const mncf_toml *v, size_t i, size_t *size);
MNCF_API const mncf_toml *mncf_toml_get(const mncf_toml *table, const char *key);
MNCF_API const mncf_toml *mncf_toml_getn(const mncf_toml *table, const char *key, size_t size);

/* ---- Trust ------------------------------------------------------------------------------------------ */

/* The keys a reader trusts, by name, and the key ids it no longer trusts (section 12). */
typedef struct mncf_trust mncf_trust;

MNCF_API mncf_trust *mncf_trust_new(const mncf_allocator *alloc);
MNCF_API mncf_status mncf_trust_add(mncf_trust *trust, const char *name, const uint8_t public_key[32]);
MNCF_API mncf_status mncf_trust_revoke(mncf_trust *trust, const char *key_id);
/* Reads a trust list file (`format = "mncf-keys/1"`). Codes: toml, format, schema:key, schema:revoked. */
MNCF_API mncf_trust *mncf_trust_read(const char *text, size_t size, const mncf_allocator *alloc,
                                     mncf_error *err);
MNCF_API void mncf_trust_free(mncf_trust *trust);

/* The key id of a public key: the first 16 hexadecimal digits of its SHA-256 hash, lower case, and NUL. */
MNCF_API void mncf_key_id(const uint8_t public_key[32], char out[17]);
/* Whether sig is a valid Ed25519 signature of message by the public key (RFC 8032, without cofactor). */
MNCF_API int mncf_ed25519_verify(const uint8_t public_key[32], const void *message, size_t size,
                                 const uint8_t signature[64]);
MNCF_API void mncf_sha256(const void *data, size_t size, uint8_t out[32]);
MNCF_API void mncf_sha512(const void *data, size_t size, uint8_t out[64]);

typedef enum mncf_signature_result {
    MNCF_SIGNATURE_UNSIGNED = 0,
    MNCF_SIGNATURE_VERIFIED,
    MNCF_SIGNATURE_UNKNOWN_KEY,
    MNCF_SIGNATURE_INVALID,
    MNCF_SIGNATURE_REVOKED
} mncf_signature_result;

/* "unsigned", "verified", "unknown-key", "invalid" or "revoked". */
MNCF_API const char *mncf_signature_name(mncf_signature_result result);

typedef struct mncf_signature {
    mncf_signature_result result;
    const char *const *signers;     /* names of the trusted keys that signed it, for "verified" */
    size_t signer_count;
    const char *const *key_ids;     /* key ids of every well-formed ed25519 line, in order */
    size_t key_id_count;
    size_t signed_size;             /* how many bytes at the start a signature covers */
    /* The lines that verified, with a trusted key or a key of the file's `signers` (sections 12 and 17.2):
     * each key id once, in the order of the lines, and the public key it verified with. Set for every
     * result, also for "unknown-key" and "invalid". */
    const char *const *verified_ids;
    const uint8_t (*verified_keys)[32];
    size_t verified_count;
} mncf_signature;

/* ---- A file ----------------------------------------------------------------------------------------- */

typedef struct mncf_file mncf_file;

/* Reads and checks an .mncf file. The bytes are not kept. trust may be NULL: no key is trusted. */
MNCF_API mncf_file *mncf_load(const void *data, size_t size, const mncf_trust *trust,
                              const mncf_allocator *alloc, mncf_error *err);
MNCF_API void mncf_free(mncf_file *file);

typedef enum mncf_type {
    MNCF_TYPE_NONE = 0,
    MNCF_TYPE_U8, MNCF_TYPE_U16, MNCF_TYPE_U32, MNCF_TYPE_U64,
    MNCF_TYPE_S8, MNCF_TYPE_S16, MNCF_TYPE_S32, MNCF_TYPE_S64,
    MNCF_TYPE_F32, MNCF_TYPE_F64,
    MNCF_TYPE_BOOL,
    MNCF_TYPE_BYTES
} mncf_type;

/* "u8" ... "f64", "bool", "bytes"; "" for MNCF_TYPE_NONE. */
MNCF_API const char *mncf_type_name(mncf_type type);

typedef enum mncf_run {
    MNCF_RUN_ON = 0, MNCF_RUN_ONCE, MNCF_RUN_ACTION, MNCF_RUN_TIMED, MNCF_RUN_MONITOR
} mncf_run;

typedef struct mncf_choice {
    mncf_number key;
    const char *name;
} mncf_choice;

typedef struct mncf_choice_list {
    const char *name;
    const mncf_choice *items;
    size_t count;
} mncf_choice_list;

/* A value the player sets (section 7.6). */
typedef struct mncf_value_spec {
    const char *id;
    mncf_type type;
    mncf_number default_value;
    mncf_number min, max, step;     /* kind MNCF_NUMBER_NONE where not given */
    const char *unit;               /* NULL where not given */
    int hex;                        /* display = "hex" */
    const mncf_choice *choices;     /* NULL where it has none */
    size_t choice_count;
    const mncf_choice *flags;       /* bit number -> name; NULL where it has none */
    size_t flag_count;
} mncf_value_spec;

/* One cheat as the file says it. Where `problem` is set, the cheat is refused whatever the runtime, and
 * the fields after the one that broke a rule keep their defaults. */
typedef struct mncf_cheat {
    size_t index;
    const char *id;
    const char *problem;            /* "unknown-key:<key>", "invalid:<key>", ..., or NULL */
    const char *name;
    const char *about;
    const char *group;
    const char *const *tags;
    size_t tag_count;
    int default_on;
    int master;
    mncf_run run;
    double duration;                /* for MNCF_RUN_TIMED */
    const char *hold;               /* "ZL+R", or NULL */
    const char *toggle;
    const char *press;
    const char *const *requires;
    size_t requires_count;
    const char *const *excludes;
    size_t excludes_count;
    const char *radio;
    int match_any;                  /* match = "any" */
    int trigger_enter;              /* trigger = "enter" */
    int64_t interval;
    const mncf_value_spec *values;
    size_t value_count;
} mncf_cheat;

typedef struct mncf_module_id {
    const char *name;
    const char *id;
} mncf_module_id;

typedef struct mncf_build {
    const char *key;
    const char *version;
    const mncf_module_id *modules;
    size_t module_count;
} mncf_build;

typedef struct mncf_preset_value {
    const char *key;                /* "cheat.value" */
    mncf_number value;
} mncf_preset_value;

typedef struct mncf_preset {
    const char *id;
    const char *name;
    const char *const *cheats;
    size_t cheat_count;
    const mncf_preset_value *values;
    size_t value_count;
} mncf_preset;

/* A name and the name it becomes: a cheat id or a pack path before and after. `to` is NULL where there is
 * none after. */
typedef struct mncf_name_pair {
    const char *from;
    const char *to;
} mncf_name_pair;

/* One change entry (`[[changes]]`, section 17.1). In a pack manifest, the lists name paths of the pack. */
typedef struct mncf_release {
    const char *version;
    const char *date;               /* "YYYY-MM-DD", or NULL */
    const char *text;               /* NULL where not given; mncf_release_text translates it */
    const char *const *added;       /* the lists in the order of the file */
    size_t added_count;
    const char *const *fixed;
    size_t fixed_count;
    const char *const *removed;
    size_t removed_count;
    const mncf_name_pair *renamed;  /* from the id in the version before to the id in this one */
    size_t renamed_count;
} mncf_release;

/* What a file says, outside its cheats' operations. */
typedef struct mncf_file_info {
    const char *name;
    const char *const *authors;
    size_t author_count;
    const char *about;              /* NULL where not given */
    const char *file_version;
    const char *language;           /* "en" where not given */
    const char *origin_format;      /* NULL where there is no origin */
    const char *origin_name;
    const char *origin_site;
    const char *const *required;    /* extensions */
    size_t required_count;
    const char *const *optional;
    size_t optional_count;
    const char *platform;
    const char *game_id;
    const char *game_name;
    const mncf_build *builds;
    size_t build_count;
    const mncf_choice_list *choices;
    size_t choice_list_count;
    const mncf_cheat *cheats;
    size_t cheat_count;
    const mncf_preset *presets;
    size_t preset_count;
    const char *const *warnings;    /* "unknown-key:<key>", "preset:<id>", "i18n", "signers", "id", "update",
                                     * "changes", "changes:<version or position>" */
    size_t warning_count;
    mncf_signature signature;
    const mncf_toml *document;      /* the whole document */
    const mncf_toml *i18n;          /* the i18n table, or NULL where there is none or it was ignored */
    /* Section 17. Keys that break its rules are left out, with a warning. */
    const char *id;                 /* the file's identity across its versions, or NULL */
    const uint8_t (*signers)[32];   /* the public keys of `signers` (section 12) */
    size_t signer_count;
    const char *update_url;         /* `update.url`, or NULL where the file has no [update] it can use */
    const char *update_host;        /* the host of update_url, or NULL */
    const mncf_release *changes;    /* the change entries, newest first */
    size_t change_count;
} mncf_file_info;

MNCF_API const mncf_file_info *mncf_file_info_of(const mncf_file *file);
/* The cheat with that id, or NULL. */
MNCF_API const mncf_cheat *mncf_file_cheat(const mncf_file *file, const char *id);

/* ---- The runtime ------------------------------------------------------------------------------------ */

/* Reads size bytes of guest memory at address into out. Returns 1, or 0 where there is no guest memory. */
typedef int (*mncf_read_fn)(void *ctx, uint64_t address, void *out, size_t size);

/* A code module that runs: its name and identity, where it is loaded, and its bytes as the module file
 * gave them (patterns and `expect` are checked against these, never against changed memory). */
typedef struct mncf_module {
    const char *name;               /* "main" */
    const char *id;                 /* its identity, such as the build ID; NULL where unknown */
    uint64_t base;
    const uint8_t *data;            /* NULL where the bytes are not at hand */
    size_t data_size;
    uint64_t size;                  /* how many bytes it spans */
    int has_size;                   /* 0: size unknown, then nothing is checked against it */
    /* [start, end) offsets of its sections. With none given, the whole module is code. */
    int has_code, has_rodata, has_data;
    uint64_t code_start, code_end, rodata_start, rodata_end, data_start, data_end;
} mncf_module;

typedef struct mncf_region {
    const char *name;               /* "heap" */
    uint64_t base;
    uint64_t size;
} mncf_region;

enum {
    MNCF_CAP_GUEST_READ = 1 << 0,
    MNCF_CAP_GUEST_WRITE = 1 << 1,
    MNCF_CAP_CODE_BYTES = 1 << 2,
    MNCF_CAP_CODE_HOOK = 1 << 3,
    MNCF_CAP_BUTTONS = 1 << 4,
    MNCF_CAP_DISPLAY = 1 << 5,
    MNCF_CAP_ALL = (1 << 6) - 1
};

/* A run of a module's code that the runtime changed itself, such as a rewriter does: its offset and
 * length in bytes. */
typedef struct mncf_rewritten {
    const char *module;
    uint64_t offset;
    uint64_t length;
} mncf_rewritten;

/* What the runtime that applies a file has and knows (section 11). mncf_runtime_init fills in the
 * defaults: every capability, the extensions raw/1 and arm64-imm/1, the raw syntax atmosphere. Arrays
 * are the caller's and must stay valid while an engine made from this runtime lives. */
typedef struct mncf_runtime {
    const char *platform;
    const char *game_id;            /* NULL: the game is not compared */
    const char *arch;               /* NULL: the platform's first architecture */
    const mncf_module *modules;
    size_t module_count;
    const mncf_region *regions;
    size_t region_count;
    mncf_read_fn read;              /* NULL: only the modules' bytes, at their base, can be read */
    void *read_ctx;
    unsigned capabilities;          /* MNCF_CAP_* */
    const char *const *extensions;
    size_t extension_count;
    const char *const *raw_syntaxes;
    size_t raw_syntax_count;
    const char *build;              /* a build key the runtime names itself, as a recompilation does */
    const char *const *held;        /* buttons held when the file is loaded */
    size_t held_count;
    /* The parts of the core the runtime does not have yet (section 11.1), out of "run.once", "run.action",
     * "run.timed", "run.monitor", "hold", "toggle", "press", "when", "when.many", "match.any",
     * "trigger.enter", "interval" and "override". A cheat that uses one is refused with
     * runtime:unsupported:<part>. */
    const char *const *unsupported;
    size_t unsupported_count;
    /* Runs of code the runtime rewrote for its own reasons (section 11.3). A patch or a stub over one is
     * refused with runtime:rewritten. */
    const mncf_rewritten *rewritten;
    size_t rewritten_count;
} mncf_runtime;

MNCF_API void mncf_runtime_init(mncf_runtime *rt, const char *platform);

/* A value the player chose: "cheat.value" and the number. */
typedef struct mncf_choice_value {
    const char *key;
    mncf_number value;
} mncf_choice_value;

/* What the player chose: cheats switched on or off against their default, and values. */
typedef struct mncf_choices {
    const char *const *on;
    size_t on_count;
    const char *const *off;
    size_t off_count;
    const mncf_choice_value *values;
    size_t value_count;
} mncf_choices;

/* ---- Evaluating ------------------------------------------------------------------------------------- */

typedef enum mncf_state {
    MNCF_STATE_OFF = 0, MNCF_STATE_ON, MNCF_STATE_WAITING, MNCF_STATE_REFUSED
} mncf_state;

/* "off", "on", "waiting", "refused". */
MNCF_API const char *mncf_state_name(mncf_state state);

typedef enum mncf_op_kind {
    MNCF_OP_PATCH = 1, MNCF_OP_WRITE, MNCF_OP_OVERRIDE, MNCF_OP_READ, MNCF_OP_WHEN, MNCF_OP_RAW
} mncf_op_kind;

/* "patch", "write", "override", "read", "when", "raw". */
MNCF_API const char *mncf_op_name(mncf_op_kind kind);

typedef enum mncf_action { MNCF_ACTION_NONE = 0, MNCF_ACTION_RETURN, MNCF_ACTION_SKIP } mncf_action;

typedef enum mncf_compare {
    MNCF_COMPARE_NONE = 0,
    MNCF_COMPARE_EQ, MNCF_COMPARE_NE, MNCF_COMPARE_LT, MNCF_COMPARE_LE, MNCF_COMPARE_GT, MNCF_COMPARE_GE,
    MNCF_COMPARE_AND, MNCF_COMPARE_NAND, MNCF_COMPARE_BETWEEN, MNCF_COMPARE_CHANGED
} mncf_compare;

/* "==", "!=", "<", "<=", ">", ">=", "&", "!&", "between", "changed". */
MNCF_API const char *mncf_compare_name(mncf_compare op);

typedef enum mncf_when_value {
    MNCF_WHEN_NONE = 0,             /* changed */
    MNCF_WHEN_NUMBER,               /* value */
    MNCF_WHEN_RANGE,                /* low and high */
    MNCF_WHEN_PLACE                 /* other_address and other_type */
} mncf_when_value;

/* An operation of a cheat, resolved for the runtime (section 11.2). */
typedef struct mncf_op {
    mncf_op_kind kind;
    int resolved;                   /* 0: a pointer path does not resolve now; only kind is set */
    uint64_t address;               /* the absolute guest address, after a pointer path */
    const char *module;             /* the module of a place without a pointer path, else NULL */
    uint64_t offset;                /* the offset in that module */
    mncf_type type;                 /* write, read and when */
    const uint8_t *expect;          /* the bytes compared, or NULL */
    size_t expect_size;
    const uint8_t *bytes;           /* what a patch or write writes, an override's stub; NULL: none */
    size_t size;
    mncf_action action;             /* override */
    int has_value;
    mncf_number value;              /* override: the value it returns; when: the number compared with */
    mncf_compare compare;           /* when */
    mncf_when_value value_kind;     /* when */
    mncf_number low, high;          /* when, between */
    uint64_t other_address;         /* when, another place */
    mncf_type other_type;
    const char *label;              /* read */
    const char *syntax;             /* raw */
    const char *build;
    const char *code;
} mncf_op;

typedef enum mncf_uses { MNCF_USES_NONE = 0, MNCF_USES_CORE, MNCF_USES_RAW } mncf_uses;

typedef struct mncf_cheat_result {
    const char *id;
    mncf_state state;
    const char *reason;             /* Appendix B, or "" */
    mncf_uses uses;
    const mncf_op *ops;
    size_t op_count;
} mncf_cheat_result;

typedef struct mncf_evaluation mncf_evaluation;

/* The build that runs and every cheat's state, reason and resolved operations. choices may be NULL. */
MNCF_API mncf_evaluation *mncf_evaluate(const mncf_file *file, const mncf_runtime *rt,
                                        const mncf_choices *choices, mncf_error *err);
MNCF_API void mncf_evaluation_free(mncf_evaluation *ev);
/* The key of the build that runs, or NULL. */
MNCF_API const char *mncf_evaluation_build(const mncf_evaluation *ev);
MNCF_API size_t mncf_evaluation_count(const mncf_evaluation *ev);
MNCF_API const mncf_cheat_result *mncf_evaluation_cheat(const mncf_evaluation *ev, size_t i);

/* ---- Passes ----------------------------------------------------------------------------------------- */

typedef enum mncf_change_kind {
    MNCF_CHANGE_PATCH = 1,          /* write bytes into code at address */
    MNCF_CHANGE_RESTORE,            /* put these original bytes back into code at address */
    MNCF_CHANGE_HOOK,               /* make the function at address do action, returning value */
    MNCF_CHANGE_UNHOOK,             /* let the function at address run as it was */
    MNCF_CHANGE_WRITE,              /* write bytes into guest memory at address */
    MNCF_CHANGE_RAW                 /* run this raw entry once */
} mncf_change_kind;

/* "patch", "restore", "hook", "unhook", "write", "raw". */
MNCF_API const char *mncf_change_name(mncf_change_kind kind);

typedef struct mncf_change {
    mncf_change_kind kind;
    uint64_t address;
    const uint8_t *bytes;
    size_t size;
    const char *cheat;              /* write and raw: the cheat it is for */
    mncf_action action;             /* hook */
    int has_value;                  /* hook, "return" */
    mncf_number value;
    const char *syntax;             /* raw */
    const char *build;
    const char *code;
} mncf_change;

/* A value a monitor read in a pass. */
typedef struct mncf_shown {
    const char *cheat;
    const char *label;              /* NULL where the read has none */
    mncf_type type;
    mncf_number value;
} mncf_shown;

/* What one pass did. Code changes come first, in address order, then writes and raw entries in file
 * order; all code changes of a pass belong in one stop of the game's threads. */
typedef struct mncf_pass {
    uint64_t number;                /* from 1 */
    const mncf_change *changes;
    size_t change_count;
    const mncf_shown *shown;        /* by cheat in file order, each in the order of its reads */
    size_t shown_count;
} mncf_pass;

typedef struct mncf_engine mncf_engine;

/* The cheats of a file in one runtime, pass by pass. rate is the number of passes per second, above 0;
 * timed cheats count passes with it where the caller gives no time. The file and the runtime's arrays
 * and module bytes must outlive the engine; the runtime structure itself is copied. */
MNCF_API mncf_engine *mncf_engine_new(const mncf_file *file, const mncf_runtime *rt,
                                      const mncf_choices *choices, double rate, mncf_error *err);
MNCF_API void mncf_engine_free(mncf_engine *eng);

/* What the player does between passes. A master cheat and a refused cheat are not switched. */
MNCF_API mncf_status mncf_engine_switch(mncf_engine *eng, const char *cheat_id, int on, int *is_on);
/* Sets a value: "cheat.value". A value that does not fit refuses the cheat with value-out-of-range. */
MNCF_API mncf_status mncf_engine_set_value(mncf_engine *eng, const char *key, mncf_number value);
/* The buttons held from now on. */
MNCF_API mncf_status mncf_engine_hold(mncf_engine *eng, const char *const *buttons, size_t count);
/* A press shorter than a pass, "ZL+R". */
MNCF_API mncf_status mncf_engine_press(mncf_engine *eng, const char *combination);

/* Runs one pass. read reads guest memory as it is at this pass (NULL: the runtime's reader); now is the
 * time in seconds, or NULL to count passes. The changes stay valid until the next call on the engine. */
MNCF_API mncf_status mncf_engine_tick(mncf_engine *eng, mncf_read_fn read, void *read_ctx, const double *now,
                                      mncf_pass *out);
/* Switches every cheat off; out gets the changes that put back every place the cheats changed. */
MNCF_API mncf_status mncf_engine_stop(mncf_engine *eng, mncf_pass *out);

/* The build that runs, or NULL. */
MNCF_API const char *mncf_engine_build(const mncf_engine *eng);
/* A cheat's state now, by its index in the file; *reason gets its reason ("" for none). */
MNCF_API mncf_state mncf_engine_state(const mncf_engine *eng, size_t index, const char **reason);

/* ---- Packs ------------------------------------------------------------------------------------------ */

typedef enum mncf_pack_status {
    MNCF_PACK_OK = 0, MNCF_PACK_HASH, MNCF_PACK_MISSING
} mncf_pack_status;

/* "ok", "hash", "missing". */
MNCF_API const char *mncf_pack_status_name(mncf_pack_status status);

typedef struct mncf_pack_file {
    const char *path;
    const char *sha256;             /* 64 lower-case hexadecimal digits, as the manifest says */
    mncf_pack_status status;
    const uint8_t *data;            /* the file's bytes where its status is ok, else NULL */
    size_t size;
} mncf_pack_file;

typedef struct mncf_pack_info {
    const char *name;
    const char *const *authors;
    size_t author_count;
    const char *about;
    const char *file_version;
    const char *const *warnings;
    size_t warning_count;
    mncf_signature signature;       /* of the manifest */
    const mncf_pack_file *files;
    size_t file_count;
    int usable;                     /* 0 where the manifest's signature is invalid or revoked */
    /* The keys of section 17 for the pack as a whole (section 17.6), as in mncf_file_info. The lists of
     * the change entries name paths of the pack. */
    const char *id;
    const uint8_t (*signers)[32];
    size_t signer_count;
    const char *update_url;
    const char *update_host;
    const mncf_release *changes;
    size_t change_count;
} mncf_pack_info;

typedef struct mncf_pack mncf_pack;

/* Reads a pack's bytes (section 13). Codes: zip, size, manifest, format, schema:<key>, path:<path>. */
MNCF_API mncf_pack *mncf_pack_read(const void *data, size_t size, const mncf_trust *trust,
                                   const mncf_allocator *alloc, mncf_error *err);
MNCF_API const mncf_pack_info *mncf_pack_info_of(const mncf_pack *pack);
MNCF_API void mncf_pack_free(mncf_pack *pack);

/* ---- Updates (section 17) --------------------------------------------------------------------------- */

/* The forms of section 17. A version is 1 to 8 whole numbers of 1 to 9 decimal digits, separated by dots. */
MNCF_API int mncf_version_valid(const char *text);
/* *result gets -1, 0 or 1 as version a is below, the same as or above version b: number by number from the
 * left, a missing number counting as 0. MNCF_ERR_ARGUMENT where one of them is not a version. */
MNCF_API mncf_status mncf_version_compare(const char *a, const char *b, int *result);
/* Whether the text is a file id: 1 to 128 characters, lower-case ASCII letters, digits, '.', '-' and '_',
 * in parts separated by '/', none of them empty, "." or "..". */
MNCF_API int mncf_file_id_valid(const char *text);
/* Whether the text is a day of the Gregorian calendar as "YYYY-MM-DD". */
MNCF_API int mncf_date_valid(const char *text);
/* The length of the host of an address that section 17.3 allows, or 0 where it does not allow it. host
 * (NULL where size is 0) gets the host, cut to size - 1 bytes, and a NUL byte; MNCF_MAX_URL + 1 bytes
 * always hold it. */
MNCF_API size_t mncf_update_url_host(const char *url, char *host, size_t size);

/* Whether a file is a new version of the one a runtime holds (section 17.2). */
typedef struct mncf_update mncf_update;

typedef struct mncf_update_info {
    const char *reason;             /* "" where the new file is taken, else a reason of Appendix E */
    const char *from_version;       /* the file_version of the file the runtime holds, or NULL */
    const char *to_version;         /* the file_version of the new file, or NULL where it has none or does
                                     * not load */
    const mncf_file *file;          /* the new file where it loads, also where it is not taken; else NULL */
    const mncf_pack *pack;          /* the same for mncf_pack_update_check */
} mncf_update_info;

/* Checks the bytes of a new file against the loaded file `installed`, in the order of section 17.2. Load
 * `installed` with the same trust list, so that both signatures are checked against the keys trusted now.
 * A new file that does not load gives "update:error:<code>"; NULL with err only on no memory or a wrong
 * argument. The new file is read with alloc (NULL: malloc) and belongs to the result. */
MNCF_API mncf_update *mncf_update_check(const mncf_file *installed, const void *data, size_t size,
                                        const mncf_trust *trust, const mncf_allocator *alloc, mncf_error *err);
/* The same for packs (section 17.6): the pack's errors for step 1, the manifest's signature, no game. */
MNCF_API mncf_update *mncf_pack_update_check(const mncf_pack *installed, const void *data, size_t size,
                                             const mncf_trust *trust, const mncf_allocator *alloc,
                                             mncf_error *err);
MNCF_API const mncf_update_info *mncf_update_info_of(const mncf_update *update);
/* The new file or pack, which is then the caller's, to free with mncf_free or mncf_pack_free; the result
 * gives NULL for it from then on. */
MNCF_API mncf_file *mncf_update_take_file(mncf_update *update);
MNCF_API mncf_pack *mncf_update_take_pack(mncf_update *update);
MNCF_API void mncf_update_free(mncf_update *update);

/* How many of the change entries, newest first as a file or pack info gives them, are above `version`:
 * those at the start of the array are the entries to show to a player who has that version (section
 * 17.4). version NULL, or not a version, counts as below every version: all of them. */
MNCF_API size_t mncf_releases_since(const mncf_release *changes, size_t count, const char *version);
/* The text of a change entry of a file in a language (a BCP 47 tag): i18n.<tag>.changes."<version>".text
 * where the file has it, else the entry's own text, NULL where it has none. file may be NULL, as for a
 * pack, and language NULL or "": the entry's own text. */
MNCF_API const char *mncf_release_text(const mncf_file *file, const mncf_release *release, const char *language);

/* Where the cheats of the version `version` of a file are in the new version `file` (section 17.5): out[i]
 * gets the id that names[i] has in `file`, or NULL where the cheat is gone. The ids follow the change
 * entries above `version` (NULL: all of them), oldest first. The strings are the file's. */
MNCF_API mncf_status mncf_ids(const mncf_file *file, const char *version, const char *const *names, size_t count,
                              const char **out, mncf_error *err);

/* The cheats to mark in a new version for a player who had `version` (section 17.4). */
typedef struct mncf_marks {
    const char *const *new_ids;     /* cheats to mark as new, ids of the file in file order */
    size_t new_count;
    const char *const *fixed_ids;   /* cheats to mark as fixed and not as new, in file order */
    size_t fixed_count;
} mncf_marks;

/* Made with the file's allocator; it uses the file's strings, so the file must outlive it. */
MNCF_API mncf_marks *mncf_mark(const mncf_file *file, const char *version, mncf_error *err);
MNCF_API void mncf_marks_free(mncf_marks *found);

/* A value that goes back to its default in the new version: its key there and why. */
typedef struct mncf_value_reset {
    const char *key;                /* "cheat.value", with the cheat's id in the new version */
    const char *why;                /* "type", "range" or "choice" */
} mncf_value_reset;

/* What the player set, moved to a new version (section 17.5). */
typedef struct mncf_carried {
    mncf_choices choices;           /* on, off and values with the ids of the new version */
    const char *preset;             /* the preset the player chose, or NULL where the new version has none */
    const mncf_name_pair *ids;      /* each cheat id seen, in the order seen: its id in the new version */
    size_t id_count;
    const char *const *dropped;     /* the cheat ids whose settings were dropped, in the order seen */
    size_t dropped_count;
    const char *const *gone;        /* "cheat.value" keys whose value the cheat no longer has */
    size_t gone_count;
    const mncf_value_reset *reset;  /* values back at their default, which the runtime names to the player */
    size_t reset_count;
} mncf_carried;

/* Moves what the player set for the version `version` of a file to the new version `file`: the cheats
 * switched on and off and the values, as mncf_choices gives them (where a key is given twice, the last
 * value counts), and the preset (NULL for none). `kept` names cheats for which the runtime keeps something
 * else, such as a crash answer, so that their new ids are in the result's `ids` too. A value keeps what
 * the player chose where it fits the value of the new version as the evaluator checks it. Made with the
 * file's allocator, with strings of its own. */
MNCF_API mncf_carried *mncf_carry(const mncf_file *file, const char *version, const mncf_choices *choices,
                                  const char *preset, const char *const *kept, size_t kept_count,
                                  mncf_error *err);
MNCF_API void mncf_carried_free(mncf_carried *carried);

/* The files of an old pack matched with those of a new one (section 17.6). */
typedef struct mncf_pairs {
    const mncf_name_pair *items;    /* each file of the old pack, in its order: its path, and the path of
                                     * the file of the new pack it is matched with, or NULL */
    size_t count;
} mncf_pairs;

/* A file is matched with the file of the new pack that has the same id and game, else by its path,
 * following the `renamed` of the new pack's change entries, where one of the two has no id. Made with the
 * old pack's allocator, with strings of its own. */
MNCF_API mncf_pairs *mncf_pack_pairs(const mncf_pack *old_pack, const mncf_pack *new_pack, mncf_error *err);
MNCF_API void mncf_pairs_free(mncf_pairs *pairs);

/* Limits (spec Appendix D, section 17). */
#define MNCF_MAX_FILE (16u * 1024u * 1024u)
#define MNCF_MAX_PACK_ENTRIES 4096u
#define MNCF_MAX_PACK_TOTAL (256u * 1024u * 1024u)
#define MNCF_MAX_DEPTH 128
#define MNCF_MAX_FILE_ID 128u
#define MNCF_MAX_URL 2048u

#ifdef __cplusplus
}
#endif

#endif /* MNCF_H */
