#pragma once

#include <stdbool.h>
#include <stddef.h>

/* ------------------------------------------------------------------ */
/* Data types                                                           */
/* ------------------------------------------------------------------ */

typedef struct {
    int  id;
    char name[128];
    int  type;   /* 0=Book 1=Comic 2=Manga */
} KavitaLibrary;

typedef struct {
    int  id;
    char name[256];
    char local_title[256];
    int  pages_read;
    int  pages_total;
    int  library_id;
} KavitaSeries;

typedef struct {
    int  id;
    char range[64];   /* display label e.g. "Vol. 1" */
    int  number;
    int  first_chapter;      /* index into KavitaSeriesDetail.chapters[] */
    int  chapters_in_volume;
} KavitaVolume;

typedef struct {
    int  id;
    int  volume_id;
    char title[256];
    float number;     /* chapter number (0 for specials) */
    int  pages;
    int  pages_read;
} KavitaChapter;

typedef struct {
    KavitaVolume   volumes[64];
    int            volume_count;
    KavitaChapter  chapters[256];
    int            chapter_count;
} KavitaSeriesDetail;

/* ------------------------------------------------------------------ */
/* Authentication                                                       */
/* ------------------------------------------------------------------ */

/* POST /api/Account/login
   Fills token_out (JWT) and api_key_out on success.
   Returns true on HTTP 200, false otherwise. */
bool kavita_login(const char* base_url,
                  const char* username,
                  const char* password,
                  char* token_out, size_t token_sz,
                  char* api_key_out, size_t api_key_sz);

/* ------------------------------------------------------------------ */
/* Libraries                                                            */
/* ------------------------------------------------------------------ */

/* GET /api/Library/libraries
   Fills buf[0..max_count-1]. Returns count on success, -1 on error. */
int kavita_get_libraries(const char* base_url, const char* token,
                          KavitaLibrary* buf, int max_count);

/* ------------------------------------------------------------------ */
/* Series                                                               */
/* ------------------------------------------------------------------ */

/* POST /api/Series/all-v2  (paginated, page is 1-based in Kavita API)
   Returns count fetched, -1 on error, 0 when no more results. */
int kavita_get_series(const char* base_url, const char* token,
                       int library_id, int page, int page_size,
                       KavitaSeries* buf, int max_count,
                       int* out_total);

/* ------------------------------------------------------------------ */
/* Series detail (volumes + chapters)                                   */
/* ------------------------------------------------------------------ */

/* GET /api/Series/volumes?seriesId=
   Returns true on success. */
bool kavita_get_series_detail(const char* base_url, const char* token,
                               int series_id, KavitaSeriesDetail* out);

/* ------------------------------------------------------------------ */
/* Reading progress                                                     */
/* ------------------------------------------------------------------ */

/* POST /api/reader/progress (ProgressDto requires libraryId + seriesId + volumeId +
 * chapterId + pageNum per Kavita OpenAPI). */
bool kavita_save_progress(const char* base_url, const char* token,
                           int library_id, int series_id, int volume_id,
                           int chapter_id, int page_num);

/* ------------------------------------------------------------------ */
/* URL construction helpers (no network call)                           */
/* ------------------------------------------------------------------ */

void kavita_cover_url(const char* base_url, const char* api_key,
                       int series_id, char* buf, size_t sz);

void kavita_chapter_cover_url(const char* base_url, const char* api_key,
                               int chapter_id, char* buf, size_t sz);

void kavita_page_url(const char* base_url, const char* api_key,
                      int chapter_id, int page, char* buf, size_t sz);

/* GET /api/Reader/chapter-info?extractPdf=true — refreshes page count after PDF
 * unpack (volumes list can be stale or 0 for PDF). Fills *pages_out on success. */
bool kavita_get_chapter_info_pages(const char* base_url, const char* token,
                                    int chapter_id, int* pages_out);
