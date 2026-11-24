/* zip.h: ZIP archives in memory, for packs. Not part of the API. MIT License. */
#ifndef MNCF_ZIP_H
#define MNCF_ZIP_H

#include "internal.h"

#include <stdlib.h>

typedef struct mn_zip_entry {
    mn_str name;                   /* its raw name as text */
    const uint8_t *raw_name;
    size_t raw_name_len;
    uint32_t flags;
    uint32_t method;
    uint32_t crc;
    uint64_t compressed_size;
    uint64_t size;
    uint64_t header_offset;
    uint64_t end_offset;           /* where the next entry's local header starts, or the directory */
} mn_zip_entry;

typedef struct mn_zip {
    const uint8_t *data;
    size_t size;
    mn_zip_entry *entries;
    size_t count, cap;
} mn_zip;

/* Reads the central directory. 0 with a "zip" error, or "memory". */
int mn_zip_open(mn_zip *z, mn_arena *ar, const uint8_t *data, size_t size, mncf_error *err);
/* Whether an entry is stored or deflated and not encrypted: the only entries this reader reads. */
int mn_zip_readable(const mn_zip_entry *e);
/* Reads at most limit bytes of an entry, whatever its header claims, on the allocator. The CRC-32 is
 * checked when the read reaches the entry's end. */
int mn_zip_read(const mn_zip *z, const mn_zip_entry *e, size_t limit, mn_alloc *alloc, uint8_t **out, size_t *out_len,
                mncf_error *err);

#endif /* MNCF_ZIP_H */
