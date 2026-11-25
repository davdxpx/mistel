/* load.c: reads an .mncf file: its size, its encoding, its TOML, its schema and its signature lines
 * (spec, sections 2, 4 to 9, 12 and 17, Appendix D). MIT License. */
#include "model.h"

#include <stdio.h>

static const char *const DEFAULT_EXTENSIONS[] = {"raw/1", "arm64-imm/1"};
static const char *const DEFAULT_SYNTAXES[] = {"atmosphere"};

void mncf_runtime_init(mncf_runtime *rt, const char *platform)
{
    if (rt == NULL) {
        return;
    }
    memset(rt, 0, sizeof *rt);
    rt->platform = platform;
    rt->capabilities = MNCF_CAP_ALL;
    rt->extensions = DEFAULT_EXTENSIONS;
    rt->extension_count = 2;
    rt->raw_syntaxes = DEFAULT_SYNTAXES;
    rt->raw_syntax_count = 1;
}

/* The text of a file's bytes, checked against section 2: UTF-8 without a byte order mark, LF line ends.
 * Shared with packs, whose manifest follows the same rules. */
int mn_check_text(const uint8_t *data, size_t size, mncf_status status, const char *code, mncf_error *err)
{
    size_t at;
    if (size >= 3 && data[0] == 0xEF && data[1] == 0xBB && data[2] == 0xBF) {
        mn_error(err, status, code, "the file starts with a byte order mark");
        return 0;
    }
    if (!mn_utf8_valid(data, size, &at)) {
        mn_error(err, status, code, "the file is not UTF-8 (at byte %lu)", (unsigned long)at);
        return 0;
    }
    if (memchr(data, '\r', size) != NULL) {
        mn_error(err, status, code, "the file has CR characters; line ends are LF only");
        return 0;
    }
    return 1;
}

/* Parses TOML for the format: no date or time values, nested at most 128 levels. */
mncf_toml *mn_parse_document(mn_arena *ar, const uint8_t *data, size_t size, mncf_status status, const char *code,
                             mncf_error *err)
{
    mncf_toml *doc = mn_toml_parse(ar, (const char *)data, size, err);
    int r;
    if (doc == NULL) {
        if (err != NULL && err->status == MNCF_ERR_TOML && status != MNCF_ERR_TOML) {
            char message[sizeof err->message];
            memcpy(message, err->message, sizeof message);
            mn_error(err, status, code, "%s", message);
        }
        return NULL;
    }
    r = mn_toml_check_depth(doc, 0);
    if (r != 0) {
        mn_error(err, status, code, r == 1 ? "arrays and tables nest deeper than 128 levels"
                                           : "the format has no date or time values");
        return NULL;
    }
    return doc;
}

mncf_file *mncf_load(const void *data, size_t size, const mncf_trust *trust, const mncf_allocator *alloc,
                     mncf_error *err)
{
    mn_alloc a = mn_alloc_of(alloc);
    mncf_file *f;
    const mncf_toml *doc;
    const uint8_t *bytes = (const uint8_t *)data;
    mn_error_clear(err);
    if (bytes == NULL && size > 0) {
        mn_error(err, MNCF_ERR_ARGUMENT, "argument", "no bytes given");
        return NULL;
    }
    if (bytes == NULL) {
        bytes = (const uint8_t *)"";
    }
    if (size > MNCF_MAX_FILE) {
        mn_error(err, MNCF_ERR_SIZE, "size", "the file has more than %u bytes", (unsigned)MNCF_MAX_FILE);
        return NULL;
    }
    if (!mn_check_text(bytes, size, MNCF_ERR_ENCODING, "encoding", err)) {
        return NULL;
    }
    f = (mncf_file *)mn_calloc(&a, sizeof *f);
    if (f == NULL) {
        mn_error_nomem(err);
        return NULL;
    }
    f->alloc = a;
    mn_arena_init(&f->arena, a);
    doc = mn_parse_document(&f->arena, bytes, size, MNCF_ERR_TOML, "toml", err);
    if (doc == NULL) {
        mncf_free(f);
        return NULL;
    }
    if (!mn_build_file(f, doc, err)) {
        mncf_free(f);
        return NULL;
    }
    /* After the schema, so that a key of the file's own `signers` can check a line (section 12). */
    if (!mn_signature_check(bytes, size, trust, f->info.signers, f->info.signer_count, &f->arena,
                            &f->info.signature)) {
        mn_error_nomem(err);
        mncf_free(f);
        return NULL;
    }
    return f;
}

void mncf_free(mncf_file *f)
{
    mn_alloc a;
    if (f == NULL) {
        return;
    }
    a = f->alloc;
    mn_arena_free(&f->arena);
    mn_free(&a, f, sizeof *f);
}

const mncf_file_info *mncf_file_info_of(const mncf_file *f)
{
    return f != NULL ? &f->info : NULL;
}

const mncf_cheat *mncf_file_cheat(const mncf_file *f, const char *id)
{
    size_t i;
    if (f == NULL || id == NULL) {
        return NULL;
    }
    i = mn_cheat_index(f, id, strlen(id));
    return i == (size_t)-1 ? NULL : &f->info.cheats[i];
}
