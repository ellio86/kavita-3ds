#pragma once

#include <stddef.h>
#include <stdbool.h>
#include <3ds/types.h>

typedef struct {
    const u8* data;
    size_t    size;
} ZipBlob;

typedef struct ZipArchive ZipArchive;

bool zip_open(const ZipBlob* blob, ZipArchive** out);
void zip_close(ZipArchive* z);

/* Heap-allocates decompressed file bytes; caller must free() result. */
u8* zip_read_file(ZipArchive* z, const char* path, size_t* out_len);

bool zip_exists_ci(ZipArchive* z, const char* path);
/* First entry whose name ends with suffix (case-insensitive). */
bool zip_find_suffix_ci(ZipArchive* z, const char* suffix, char* out, size_t out_cap);
