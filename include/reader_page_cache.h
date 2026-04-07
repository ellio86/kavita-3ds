#pragma once

#include <3ds.h>
#include <stdbool.h>
#include <stddef.h>

/* Temporary raw page image bytes under sdmc:/3ds/kavita-3ds/pages/
   Cleared when opening the reader, leaving the reader, or app exit. */

void reader_page_cache_clear(void);

/* Delete cached files for pages strictly before reader_page0 (0-based). */
void reader_page_cache_delete_pages_before(int chapter_id, int reader_page0);

bool reader_page_cache_exists(int chapter_id, int page0);

/* Read cached page bytes; caller frees *out_data with free(). */
bool reader_page_cache_read(int chapter_id, int page0, u8** out_data, size_t* out_size);

/* Write after download; may evict older files to make room. lookahead_n = setting
   "next N pages" — protects pages in (reader_page0, reader_page0 + lookahead_n]. */
bool reader_page_cache_write(int chapter_id, int page0,
                             const u8* data, size_t size,
                             int reader_page0, int lookahead_n);
