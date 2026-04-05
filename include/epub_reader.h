#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <3ds/types.h>

void epub_reader_close(void);

/* Downloads chapter archive, opens EPUB, builds spine. *pages_inout: Kavita page count;
 * if spine differs, updates to parsed count. Fetch-thread only. */
bool epub_reader_open_chapter(const char* base_url, const char* token, int chapter_id,
                               int* pages_inout);

bool epub_reader_is_open(void);

typedef enum {
    EPUB_PAGE_IMAGE,
    EPUB_PAGE_TEXT,
} EpubPageKind;

/* Resolves one spine page: raster image bytes or plain text (UTF-8).
 * On success: *kind IMAGE => *image_data malloc'd, *text_utf8 NULL.
 * TEXT => *text_utf8 malloc'd, *image_data NULL. Caller frees the non-NULL pointer. */
bool epub_reader_get_page_payload(int page_index, EpubPageKind* kind,
                                   u8** image_data, size_t* image_size,
                                   char** text_utf8);
