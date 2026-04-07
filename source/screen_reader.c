#include "screen_reader.h"
#include "app.h"
#include "kavita_api.h"
#include "epub_reader.h"
#include "image_loader.h"
#include "http_client.h"
#include "reader_page_cache.h"
#include "ui.h"
#include "debug_log.h"

#include <3ds.h>
#include <3ds/applets/swkbd.h>
#include <3ds/gpu/enums.h>
#include <citro2d.h>
#include <citro3d.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* ------------------------------------------------------------------ */
/* Book mode: portrait on each LCD (rotate +90°), two-page spread     */
/* D-pad Up/Down = prev/next spread (device held with pad at bottom)  */
/* ------------------------------------------------------------------ */
#define BOOK_ROT_DEG 90.0f

#define TOP_PHYS_W 400.0f
#define TOP_PHYS_H 240.0f
#define BOT_PHYS_W 320.0f
#define BOT_PHYS_H 240.0f

/* Logical portrait size after rotation (fits physical buffer) */
#define TOP_LOG_W TOP_PHYS_H
#define TOP_LOG_H TOP_PHYS_W
#define BOT_LOG_W BOT_PHYS_H
#define BOT_LOG_H BOT_PHYS_W

/* EPUB body text scale (fixed; do not multiply reader_zoom). */
#define READER_EPUB_TEXT_TOP       FONT_MED
#define READER_EPUB_TEXT_BOT       FONT_SMALL
#define READER_EPUB_LINE_GAP_SCALE 0.55f /* leading between baselines, × scale */

#define ZOOM_LEVEL_COUNT 7
static const float s_zoom_levels[ZOOM_LEVEL_COUNT] =
    { 1.0f, 1.35f, 1.75f, 2.25f, 2.75f, 3.35f, 4.0f };

static int   s_zoom_i;
static float s_pan_x[2];
static float s_pan_y[2];

/* Two spreads: current (reader_page P) and next (P+2). No backward cache —
 * go_next discards old current; go_prev discards old next preload.
 * tex[0] = left (top), tex[1] = right (bottom). */
typedef struct {
    LoadedTexture tex[2];
    bool          valid[2];
    char*         text[2];   /* EPUB text-only pages (heap); NULL if raster */
} SpreadPair;

enum {
    SI_CURR = 0,
    SI_NEXT = 1,
    SI_COUNT = 2,
};

static SpreadPair     s_sp[SI_COUNT];

static Thread         s_fetch_thread;
static volatile bool  s_fetch_done;
static volatile bool  s_fetch_failed;
static LightEvent     s_page_ready_event;
static PreparedTexture s_pending_prep;
static volatile int   s_pending_spread_idx; /* SI_CURR or SI_NEXT */
static volatile int   s_pending_side;      /* 0=left, 1=right */
static volatile int   s_pending_src_page;  /* 0-based page index for this fetch (for logs / errors) */
static volatile bool  s_pending_ready;
static volatile bool  s_pending_is_text;
static char*          s_pending_text_heap;

static Thread         s_progress_thread;
static volatile bool  s_progress_thread_running;

typedef enum {
    READER_LOADING,
    READER_SHOW,
    READER_ERROR,
} ReaderState;

static ReaderState    s_state;
static char           s_error_msg[128];
static UiSpinner      s_spinner;

static bool           s_show_hint;
static int            s_hint_frames;
static int            s_pages_since_save;

typedef struct {
    char base_url[256];
    char token[1024];
    int  library_id;
    int  series_id;
    int  volume_id;
    int  chapter_id;
    int  page_num;
} ProgressArgs;
static ProgressArgs s_progress_args;

typedef struct {
    int  spread_idx;
    int  side;
    int  page;
    bool allow_http;
    int  cache_reader_page;
} FetchArgs;
static FetchArgs s_fetch_args;

typedef struct {
    int page;
} PrefetchArgs;
static PrefetchArgs   s_prefetch_args;
static Thread         s_prefetch_thread;
static volatile bool  s_prefetch_done;

static void wait_prefetch(void) {
    if (s_prefetch_thread) {
        threadJoin(s_prefetch_thread, U64_MAX);
        threadFree(s_prefetch_thread);
        s_prefetch_thread = NULL;
    }
    s_prefetch_done = false;
}

static void prefetch_thread(void* arg) {
    PrefetchArgs* pa = (PrefetchArgs*)arg;
    int p = pa->page;
    char url_log[512];
    url_log[0] = '\0';

    kavita_page_url(g_app.base_url, g_app.api_key,
                    g_app.selected_chapter_id, p,
                    url_log, sizeof(url_log));

    HttpResponse* resp = http_get(url_log, g_app.token);
    if (!resp || resp->status_code != 200) {
        if (resp)
            http_response_free(resp);
        s_prefetch_done = true;
        threadExit(0);
    }

    int lookahead = g_app.reader_cache_pages > 0 ? g_app.reader_cache_pages : 10;
    int rp         = g_app.reader_page;

    reader_page_cache_write(g_app.selected_chapter_id, p,
                            (const u8*)resp->data, resp->size, rp, lookahead);
    http_response_free(resp);
    s_prefetch_done = true;
    threadExit(0);
}

static void try_start_prefetch(void) {
    if (s_prefetch_thread)
        return;
    if (s_fetch_thread)
        return;
    if (!g_app.reader_page_cache || g_app.reader_epub)
        return;

    int P = g_app.reader_page;
    int T = g_app.reader_total_pages;
    int N = g_app.reader_cache_pages > 0 ? g_app.reader_cache_pages : 10;
    int ch = g_app.selected_chapter_id;

    for (int p = P + 1; p <= P + N && p < T; p++) {
        if (reader_page_cache_exists(ch, p))
            continue;
        s_prefetch_args.page = p;
        s_prefetch_done      = false;
        s_prefetch_thread =
            threadCreate(prefetch_thread, &s_prefetch_args, 64 * 1024, 0x30, 1, false);
        return;
    }
}

/* EPUB text: one spine item flows left screen (top) then right (bottom), then D-pad
 * advances to the next slice of the same chapter. Line index is rebuilt from text[0]. */
static int*         s_epub_line_off = NULL;
static int*         s_epub_line_len = NULL;
static int          s_epub_line_n   = 0;
static const char*  s_epub_lines_doc = NULL;
static int          s_epub_seg_line0 = 0; /* first line index on this spread */
static bool         s_epub_open_last_seg = false; /* go_prev spine: land on last spread */

static void epub_lines_free(void);
static bool reader_epub_dual_column(void);
static void reader_epub_maybe_rebuild_lines(void);
static bool epub_more_forward(void);
static int  epub_lines_on_spread_from(int line0);
static int  epub_prev_spread_line0(int seg0);
static int  epub_last_spread_line0(void);

/* ------------------------------------------------------------------ */
/* Progress save                                                        */
/* ------------------------------------------------------------------ */
static void wait_progress_thread(void) {
    if (!s_progress_thread)
        return;
    threadJoin(s_progress_thread, U64_MAX);
    threadFree(s_progress_thread);
    s_progress_thread = NULL;
    s_progress_thread_running = false;
}

static void progress_thread(void* arg) {
    ProgressArgs* a = (ProgressArgs*)arg;
    bool ok = kavita_save_progress(a->base_url, a->token,
                                    a->library_id, a->series_id, a->volume_id,
                                    a->chapter_id, a->page_num);
    if (!ok)
        dlog("[reader] save progress FAILED chap=%d page=%d lib=%d",
             a->chapter_id, a->page_num, a->library_id);
    s_progress_thread_running = false;
    threadExit(0);
}

static void fire_save_progress(int page) {
    wait_progress_thread();

    strncpy(s_progress_args.base_url, g_app.base_url,
            sizeof(s_progress_args.base_url) - 1);
    s_progress_args.base_url[sizeof(s_progress_args.base_url) - 1] = '\0';
    strncpy(s_progress_args.token, g_app.token,
            sizeof(s_progress_args.token) - 1);
    s_progress_args.token[sizeof(s_progress_args.token) - 1] = '\0';
    s_progress_args.library_id = g_app.selected_library_id;
    s_progress_args.series_id  = g_app.selected_series_id;
    s_progress_args.volume_id  = g_app.selected_volume_id;
    s_progress_args.chapter_id = g_app.selected_chapter_id;
    s_progress_args.page_num   = page;

    s_progress_thread = threadCreate(progress_thread, &s_progress_args,
                                      16 * 1024, 0x3f, 1, false);
    if (!s_progress_thread) {
        dlog("[reader] progress threadCreate failed");
        s_progress_thread_running = false;
        return;
    }
    s_progress_thread_running = true;
}

/* ------------------------------------------------------------------ */
/* Spread navigation                                                    */
/* ------------------------------------------------------------------ */
static bool can_advance_spread(void) {
    if (g_app.reader_epub && reader_epub_dual_column()) {
        if (epub_more_forward())
            return true;
        return g_app.reader_page + 1 < g_app.reader_total_pages;
    }
    return g_app.reader_page + 2 < g_app.reader_total_pages;
}

static bool can_retreat_spread(void) {
    if (g_app.reader_epub && reader_epub_dual_column()) {
        reader_epub_maybe_rebuild_lines();
        if (s_epub_seg_line0 > 0)
            return true;
        return g_app.reader_page > 0;
    }
    return g_app.reader_page >= 2;
}

static SpreadPair* spread_at(int idx) {
    if ((unsigned)idx < (unsigned)SI_COUNT) return &s_sp[idx];
    return &s_sp[SI_CURR];
}

static bool spread_side_ready(const SpreadPair* s, int i) {
    if (g_app.reader_epub && i == 1 && s->text[0] && s->text[0][0] && !s->valid[0]
        && !s->valid[1] && !s->text[1])
        return true; /* EPUB: right LCD is continuation of left spine item */
    return s->valid[i] || (s->text[i] && s->text[i][0]);
}

static void spread_free(SpreadPair* s) {
    for (int i = 0; i < 2; i++) {
        if (s->text[i]) {
            if (s->text[i] == s_epub_lines_doc)
                epub_lines_free();
            free(s->text[i]);
            s->text[i] = NULL;
        }
        if (s->valid[i]) {
            image_texture_free(&s->tex[i]);
            s->valid[i] = false;
        }
    }
}

/* image_loader sets image.tex / image.subtex to point inside this struct. Assigning
 * LoadedTexture by value copies those pointers verbatim, so they still reference the
 * *old* SpreadPair slot. The next preload then uploads into that slot and the
 * on-screen “current” page mutates without navigation. */
static void loaded_texture_relink(LoadedTexture* lt) {
    if (!lt || !lt->valid) return;
    lt->image.tex    = &lt->tex;
    lt->image.subtex = &lt->subtex;
}

/* Frees dst, moves GPU textures from src into dst, clears src ownership. */
static void spread_move_into(SpreadPair* dst, SpreadPair* src) {
    spread_free(dst);
    for (int i = 0; i < 2; i++) {
        dst->tex[i]   = src->tex[i];
        dst->valid[i] = src->valid[i];
        dst->text[i]  = src->text[i];
        src->valid[i] = false;
        src->text[i]  = NULL;
        loaded_texture_relink(&dst->tex[i]);
    }
}

static bool curr_spread_complete(void) {
    int P = g_app.reader_page;
    int T = g_app.reader_total_pages;
    if (P >= T) return false;
    if (reader_epub_dual_column())
        return spread_side_ready(&s_sp[SI_CURR], 0);
    if (!spread_side_ready(&s_sp[SI_CURR], 0)) return false;
    if (P + 1 < T && !spread_side_ready(&s_sp[SI_CURR], 1)) return false;
    return true;
}

/* Preload for go_next: PDF = next pair; EPUB dual = next spine when current is exhausted. */
static bool next_spread_ready(void) {
    int P = g_app.reader_page;
    int T = g_app.reader_total_pages;
    if (g_app.reader_epub && reader_epub_dual_column()) {
        reader_epub_maybe_rebuild_lines();
        if (epub_more_forward())
            return true;
        if (P + 1 >= T)
            return false;
        return spread_side_ready(&s_sp[SI_NEXT], 0);
    }
    if (P + 2 >= T) return false;
    if (!spread_side_ready(&s_sp[SI_NEXT], 0)) return false;
    if (P + 3 < T && !spread_side_ready(&s_sp[SI_NEXT], 1)) return false;
    return true;
}

/* Drop a decoded page that the fetch thread delivered but we have not uploaded yet.
 * Must run before spread_free / spread_move_into so a late pending upload cannot
 * overwrite the wrong slot after reader_page changes (e.g. old left page clobbers
 * the new spread's top screen). */
static void discard_pending_page(void) {
    if (!s_pending_ready) return;
    if (LightEvent_TryWait(&s_page_ready_event) == 0)
        LightEvent_Clear(&s_page_ready_event);
    if (s_pending_is_text && s_pending_text_heap) {
        free(s_pending_text_heap);
        s_pending_text_heap = NULL;
    }
    s_pending_is_text = false;
    image_prepared_free(&s_pending_prep);
    s_pending_ready = false;
}

static int spread_side_page_1based(int spread_idx, int side) {
    int P = g_app.reader_page;
    int page0 = P + spread_idx * 2; /* SI_CURR=0 → P, SI_NEXT=1 → P+2 */
    return page0 + side + 1;
}

static void start_fetch(int page, int spread_idx, int side);

/* ------------------------------------------------------------------ */
/* Page fetch thread                                                    */
/* ------------------------------------------------------------------ */
static void fetch_page_thread(void* arg) {
    FetchArgs* a = (FetchArgs*)arg;

    PreparedTexture prep;
    bool ok = false;
    size_t body_sz = 0;
    char url_log[512];
    url_log[0] = '\0';

    if (g_app.reader_epub) {
        if (!epub_reader_is_open()) {
            int pages = g_app.reader_total_pages;
            if (!epub_reader_open_chapter(g_app.base_url, g_app.token,
                                          g_app.selected_chapter_id, &pages)) {
                dlog("[reader][ERR] epub open fail chap=%d", g_app.selected_chapter_id);
                s_fetch_failed = true;
                s_fetch_done   = true;
                threadExit(0);
            }
            g_app.reader_total_pages = pages;
        }
        EpubPageKind ek;
        u8* img = NULL;
        size_t iz = 0;
        char* txt = NULL;
        if (!epub_reader_get_page_payload(a->page, &ek, &img, &iz, &txt)) {
            dlog("[reader][ERR] epub page fail chap=%d page=%d",
                 g_app.selected_chapter_id, a->page + 1);
            s_fetch_failed = true;
            s_fetch_done   = true;
            threadExit(0);
        }
        if (ek == EPUB_PAGE_IMAGE) {
            body_sz = iz;
            ok = image_prepare_from_mem(img, iz, &prep);
            free(img);
            s_pending_is_text = false;
        } else {
            body_sz = txt ? strlen(txt) : 0;
            ok = (txt != NULL);
            memset(&prep, 0, sizeof(prep));
            s_pending_is_text = true;
            s_pending_text_heap = txt;
        }
    } else {
        kavita_page_url(g_app.base_url, g_app.api_key,
                         g_app.selected_chapter_id, a->page,
                         url_log, sizeof(url_log));

        int lookahead = g_app.reader_cache_pages > 0 ? g_app.reader_cache_pages : 10;
        int cache_rp    = a->cache_reader_page;

        ok = false;
        if (g_app.reader_page_cache) {
            u8* cached = NULL;
            size_t csz = 0;
            if (reader_page_cache_read(g_app.selected_chapter_id, a->page,
                                       &cached, &csz)) {
                body_sz = csz;
                ok = image_prepare_from_mem(cached, csz, &prep);
                free(cached);
                s_pending_is_text = false;
            } else if (!a->allow_http) {
                s_fetch_done = true;
                s_fetch_failed = false;
                threadExit(0);
            }
        }

        if (!ok) {
            HttpResponse* resp = http_get(url_log, g_app.token);
            if (!resp || resp->status_code != 200) {
                dlog("[reader][ERR] HTTP fail chap=%d page=%d (1-based) status=%d",
                     g_app.selected_chapter_id, a->page + 1,
                     resp ? resp->status_code : -1);
                http_response_free(resp);
                s_fetch_failed = true;
                s_fetch_done   = true;
                threadExit(0);
            }

            body_sz = resp->size;
            ok = image_prepare_from_mem((const u8*)resp->data, resp->size, &prep);
            if (ok && g_app.reader_page_cache) {
                reader_page_cache_write(g_app.selected_chapter_id, a->page,
                                        (const u8*)resp->data, resp->size,
                                        cache_rp, lookahead);
            }
            http_response_free(resp);
            s_pending_is_text = false;
        }
    }

    if (ok) {
        s_pending_prep        = prep;
        s_pending_spread_idx  = a->spread_idx;
        s_pending_side        = a->side;
        s_pending_src_page    = a->page;
        s_pending_ready       = true;
        LightEvent_Signal(&s_page_ready_event);
    } else {
        const char* ut = url_log[0] ? url_log : "epub";
        size_t ul = strlen(ut);
        if (ul > 48) ut += ul - 48;
        dlog("[reader][ERR] prepare(thread) fail chap=%d page=%d (1-based) body=%zu url_tail=%s",
             g_app.selected_chapter_id, a->page + 1, body_sz, ut);
        s_fetch_failed = true;
    }

    s_fetch_done = true;
    threadExit(0);
}

static void start_fetch(int page, int spread_idx, int side) {
    if (page < 0 || page >= g_app.reader_total_pages) return;
    if (s_fetch_thread) return;

    bool allow_http = true;
    if (g_app.reader_page_cache && !g_app.reader_epub && spread_idx == SI_NEXT)
        allow_http = false;

    s_fetch_args.page              = page;
    s_fetch_args.spread_idx        = spread_idx;
    s_fetch_args.side              = side;
    s_fetch_args.allow_http        = allow_http;
    s_fetch_args.cache_reader_page = g_app.reader_page;
    s_fetch_done                   = false;
    s_fetch_failed                 = false;

    s_fetch_thread = threadCreate(fetch_page_thread, &s_fetch_args,
                                   64 * 1024, 0x30, 1, false);
}

/* Finish current spread, then preload the following spread only. */
static void try_start_fetch(void) {
    if (s_fetch_thread) return;

    int P = g_app.reader_page;
    int T = g_app.reader_total_pages;

    bool cur_epub_dual =
        g_app.reader_epub && s_sp[SI_CURR].text[0] && s_sp[SI_CURR].text[0][0]
        && !s_sp[SI_CURR].valid[0] && !s_sp[SI_CURR].valid[1] && !s_sp[SI_CURR].text[1];

    if (P < T && !spread_side_ready(&s_sp[SI_CURR], 0)) {
        start_fetch(P, SI_CURR, 0);
        return;
    }

    if (g_app.reader_epub && cur_epub_dual) {
        reader_epub_maybe_rebuild_lines();
        if (!epub_more_forward() && P + 1 < T && !spread_side_ready(&s_sp[SI_NEXT], 0))
            start_fetch(P + 1, SI_NEXT, 0);
        return;
    }

    if (P + 1 < T && !spread_side_ready(&s_sp[SI_CURR], 1)) {
        start_fetch(P + 1, SI_CURR, 1);
        return;
    }
    if (P + 2 < T && !spread_side_ready(&s_sp[SI_NEXT], 0)) {
        start_fetch(P + 2, SI_NEXT, 0);
        return;
    }
    if (P + 2 < T && P + 3 < T && !spread_side_ready(&s_sp[SI_NEXT], 1)) {
        start_fetch(P + 3, SI_NEXT, 1);
        return;
    }
}

static void wait_fetch(void) {
    if (s_fetch_thread) {
        threadJoin(s_fetch_thread, U64_MAX);
        threadFree(s_fetch_thread);
        s_fetch_thread = NULL;
    }
    s_fetch_done = false;
}

static void go_next_spread(void) {
    if (reader_epub_dual_column()) {
        if (!curr_spread_complete()) return;
        reader_epub_maybe_rebuild_lines();
        int n = epub_lines_on_spread_from(s_epub_seg_line0);
        if (n <= 0)
            return;
        if (s_epub_seg_line0 + n < s_epub_line_n) {
            s_epub_seg_line0 += n;
            s_pan_y[0] = s_pan_y[1] = 0.f;
            s_pages_since_save++;
            if (s_pages_since_save >= 3) {
                fire_save_progress(g_app.reader_page);
                s_pages_since_save = 0;
            }
            return;
        }
        if (g_app.reader_page + 1 >= g_app.reader_total_pages)
            return;

        wait_prefetch();
        wait_fetch();
        discard_pending_page();

        if (next_spread_ready()) {
            spread_free(&s_sp[SI_CURR]);
            spread_move_into(&s_sp[SI_CURR], &s_sp[SI_NEXT]);
        } else {
            spread_free(&s_sp[SI_CURR]);
            spread_free(&s_sp[SI_NEXT]);
        }

        g_app.reader_page += 1;
        s_epub_seg_line0 = 0;
        epub_lines_free();

        s_state = curr_spread_complete() ? READER_SHOW : READER_LOADING;
        try_start_fetch();

        s_pages_since_save++;
        if (s_pages_since_save >= 3) {
            fire_save_progress(g_app.reader_page);
            s_pages_since_save = 0;
        }
        return;
    }

    if (!can_advance_spread() || !curr_spread_complete()) return;

    wait_prefetch();
    wait_fetch();
    discard_pending_page();

    if (next_spread_ready()) {
        spread_free(&s_sp[SI_CURR]);
        spread_move_into(&s_sp[SI_CURR], &s_sp[SI_NEXT]);
    } else {
        /* Preload not ready: drop current + partial preload, fetch fresh pair. */
        spread_free(&s_sp[SI_CURR]);
        spread_free(&s_sp[SI_NEXT]);
    }

    g_app.reader_page += 2;
    if (g_app.reader_page_cache && !g_app.reader_epub) {
        reader_page_cache_delete_pages_before(g_app.selected_chapter_id,
                                              g_app.reader_page);
    }
    /* Keep zoom + pan across spreads; clamp_pan_* fixes bounds per page. */

    s_state = curr_spread_complete() ? READER_SHOW : READER_LOADING;
    try_start_fetch();

    s_pages_since_save++;
    if (s_pages_since_save >= 3) {
        fire_save_progress(g_app.reader_page);
        s_pages_since_save = 0;
    }
}

static void go_prev_spread(void) {
    if (!can_retreat_spread()) return;

    if (reader_epub_dual_column()) {
        reader_epub_maybe_rebuild_lines();
        if (s_epub_seg_line0 > 0) {
            s_epub_seg_line0 = epub_prev_spread_line0(s_epub_seg_line0);
            s_pan_y[0] = s_pan_y[1] = 0.f;
            return;
        }

        wait_prefetch();
        wait_fetch();
        discard_pending_page();
        spread_free(&s_sp[SI_NEXT]);
        spread_free(&s_sp[SI_CURR]);

        g_app.reader_page -= 1;
        s_epub_seg_line0     = 0;
        s_epub_open_last_seg = true;
        epub_lines_free();

        s_state = READER_LOADING;
        try_start_fetch();
        return;
    }

    wait_prefetch();
    wait_fetch();
    discard_pending_page();
    spread_free(&s_sp[SI_NEXT]);
    spread_move_into(&s_sp[SI_NEXT], &s_sp[SI_CURR]);

    g_app.reader_page -= 2;
    /* Keep zoom + pan across spreads; clamp_pan_* fixes bounds per page. */

    s_state = curr_spread_complete() ? READER_SHOW : READER_LOADING;
    try_start_fetch();
}

/* Jump so the spread includes 1-based page `page_1` (EPUB: spine index; PDF: even-aligned). */
static void reader_jump_to_page_1based(int page_1) {
    int T = g_app.reader_total_pages;
    if (T <= 0) return;
    if (page_1 < 1) page_1 = 1;
    if (page_1 > T) page_1 = T;
    int idx0   = page_1 - 1;
    int new_rp;
    int max_rp;
    if (g_app.reader_epub) {
        new_rp = idx0;
        max_rp = T - 1;
    } else {
        new_rp = (idx0 / 2) * 2;
        max_rp = ((T - 1) / 2) * 2;
    }
    if (new_rp > max_rp) new_rp = max_rp;

    if (new_rp == g_app.reader_page && curr_spread_complete()
        && s_state != READER_ERROR) {
        s_zoom_i = 0;
        s_pan_x[0] = s_pan_x[1] = s_pan_y[0] = s_pan_y[1] = 0.f;
        fire_save_progress(g_app.reader_page);
        return;
    }

    wait_prefetch();
    wait_fetch();
    discard_pending_page();
    spread_free(&s_sp[SI_CURR]);
    spread_free(&s_sp[SI_NEXT]);
    g_app.reader_page = new_rp;
    s_epub_seg_line0     = 0;
    s_epub_open_last_seg = false;
    epub_lines_free();
    s_zoom_i = 0;
    s_pan_x[0] = s_pan_x[1] = s_pan_y[0] = s_pan_y[1] = 0.f;
    s_pages_since_save = 0;
    s_state = READER_LOADING;
    s_fetch_failed = false;
    try_start_fetch();
    fire_save_progress(g_app.reader_page);
}

static bool prompt_goto_page_1based(int* out_page_1) {
    SwkbdState kbd;
    swkbdInit(&kbd, SWKBD_TYPE_NUMPAD, 2, 15);
    char hint[48];
    snprintf(hint, sizeof(hint), "Page (1-%d)", g_app.reader_total_pages);
    swkbdSetHintText(&kbd, hint);
    char init[16];
    snprintf(init, sizeof(init), "%d", g_app.reader_page + 1);
    swkbdSetInitialText(&kbd, init);
    swkbdSetButton(&kbd, SWKBD_BUTTON_RIGHT, "OK", true);
    swkbdSetButton(&kbd, SWKBD_BUTTON_LEFT,  "Cancel", false);

    char buf[16] = {0};
    SwkbdButton btn = swkbdInputText(&kbd, buf, sizeof(buf));
    if (btn != SWKBD_BUTTON_CONFIRM || !buf[0])
        return false;
    int v = atoi(buf);
    if (v < 1)
        return false;
    *out_page_1 = v;
    return true;
}

/* ------------------------------------------------------------------ */
/* Portrait drawing (single C2D_SceneBegin; ui_* would reset matrix)   */
/* ------------------------------------------------------------------ */
static void reader_apply_book_transform(float phys_w, float phys_h) {
    float lw = phys_h;
    float lh = phys_w;
    C2D_ViewTranslate(phys_w * 0.5f, phys_h * 0.5f);
    C2D_ViewRotateDegrees(BOOK_ROT_DEG);
    C2D_ViewTranslate(-lw * 0.5f, -lh * 0.5f);
}

static void reader_spinner_draw(const UiSpinner* s,
                                float cx, float cy, float r, u32 color) {
    int segs = 12;
    float arc = 120.0f * ((float)M_PI / 180.0f);
    float base = s->angle * ((float)M_PI / 180.0f);

    for (int i = 0; i < segs; i++) {
        float a0 = base + arc * ((float)i     / segs);
        float a1 = base + arc * ((float)(i+1) / segs);
        float x0 = cx + cosf(a0) * r;
        float y0 = cy + sinf(a0) * r;
        float x1 = cx + cosf(a1) * r;
        float y1 = cy + sinf(a1) * r;
        u32 alpha = (u32)(255 * (float)(i + 1) / segs);
        u32 c = (color & 0x00ffffff) | (alpha << 24);
        C2D_DrawLine(x0, y0, c, x1, y1, c, 2.0f, 0.5f);
    }
}

/* Citro2D sorts by depth: higher z draws in front (see ui_button: bg 0.5, label 0.6). */
#define Z_READER_PAGE          0.5f
#define Z_READER_HINT_BG       0.55f
#define Z_READER_PAGE_BADGE_BG 0.6f
#define Z_READER_HUD_FG        0.65f

static void reader_text_draw_z(const char* str, float x, float y,
                               float scale, u32 color, float z) {
    C2D_Text t;
    C2D_TextParse(&t, g_text_buf, str);
    C2D_TextOptimize(&t);
    C2D_DrawText(&t, C2D_WithColor, x, y, z, scale, scale, color);
}

static void reader_text_centered_z(const char* str, float log_w, float y,
                                    float scale, u32 color, float z) {
    C2D_Text t;
    C2D_TextParse(&t, g_text_buf, str);
    C2D_TextOptimize(&t);
    float tw, th;
    C2D_TextGetDimensions(&t, scale, scale, &tw, &th);
    float dx = (log_w - tw) * 0.5f;
    if (dx < 0) dx = 0;
    C2D_DrawText(&t, C2D_WithColor, dx, y, z, scale, scale, color);
}

static void reader_text_centered(const char* str, float log_w, float y,
                                  float scale, u32 color) {
    reader_text_centered_z(str, log_w, y, scale, color, Z_READER_PAGE);
}

static float reader_zoom(void) {
    return s_zoom_levels[s_zoom_i];
}

static void clamp_pan_x(float log_w, float log_h, const LoadedTexture* tex,
                        float zoom, float* pan_x) {
    if (!tex || !tex->valid) return;
    float s, sy, ox, oy;
    image_fit_dims(tex->src_width, tex->src_height,
                   (int)log_w, (int)log_h, &s, &sy, &ox, &oy);
    (void)ox;
    (void)sy;
    float draw_w = tex->src_width * s * zoom;
    if (draw_w <= log_w) {
        *pan_x = 0.f;
        return;
    }
    float lim = (draw_w - log_w) * 0.5f;
    if (*pan_x > lim)  *pan_x = lim;
    if (*pan_x < -lim) *pan_x = -lim;
}

static void clamp_pan_y(float log_w, float log_h, const LoadedTexture* tex,
                        float zoom, float* pan_y) {
    if (!tex || !tex->valid) return;
    float s, sy, ox, oy;
    image_fit_dims(tex->src_width, tex->src_height,
                   (int)log_w, (int)log_h, &s, &sy, &ox, &oy);
    (void)ox;
    (void)sy;
    float draw_h = tex->src_height * s * zoom;
    if (draw_h <= log_h) {
        *pan_y = 0.f;
        return;
    }
    float lim = (draw_h - log_h) * 0.5f;
    if (*pan_y > lim)  *pan_y = lim;
    if (*pan_y < -lim) *pan_y = -lim;
}

static void draw_portrait_page_zoom(float log_w, float log_h,
                                    const LoadedTexture* tex,
                                    float pan_x, float pan_y,
                                    float zoom) {
    if (!tex || !tex->valid) return;
    float s, sy, ox, oy;
    image_fit_dims(tex->src_width, tex->src_height,
                   (int)log_w, (int)log_h, &s, &sy, &ox, &oy);
    (void)sy;
    float z = zoom;
    float scale = s * z;
    float draw_w = tex->src_width * scale;
    float draw_h = tex->src_height * scale;
    float cx = (log_w - draw_w) * 0.5f + pan_x;
    float cy = (log_h - draw_h) * 0.5f + pan_y;
    C2D_DrawImageAt(tex->image, cx, cy, 0.5f, NULL, scale, scale);
}

static float reader_epub_line_width(const char* line, float scale) {
    if (!line || !line[0])
        return 0.f;
    C2D_TextBufClear(g_text_buf);
    C2D_Text t;
    C2D_TextParse(&t, g_text_buf, line);
    C2D_TextOptimize(&t);
    float w, h;
    C2D_TextGetDimensions(&t, scale, scale, &w, &h);
    (void)h;
    return w;
}

static float reader_epub_line_step(float scale) {
    C2D_TextBufClear(g_text_buf);
    C2D_Text t;
    C2D_TextParse(&t, g_text_buf, "Mg");
    C2D_TextOptimize(&t);
    float w, h;
    C2D_TextGetDimensions(&t, scale, scale, &w, &h);
    (void)w;
    return h + READER_EPUB_LINE_GAP_SCALE * scale;
}

static void reader_epub_flush_line(float margin, float* cy, float line_h, float log_h,
                                   float pan_y, float scale, const char* line, bool draw) {
    if (!line[0])
        return;
    float sy = *cy - pan_y;
    if (draw && sy + line_h > 0.f && sy < log_h) {
        C2D_TextBufClear(g_text_buf);
        C2D_Text t;
        C2D_TextParse(&t, g_text_buf, line);
        C2D_TextOptimize(&t);
        C2D_DrawText(&t, C2D_WithColor, margin, sy, Z_READER_PAGE, scale, scale,
                     COL_WHITE);
    }
    *cy += line_h;
}

/* One C2D_DrawText per wrapped line (per-word drawing can exhaust C2D object cap). */
static float reader_epub_text_layout(float log_w, float log_h, const char* text,
                                     float scale, float pan_y, bool draw) {
    float margin = 8.f;
    float max_w  = log_w - margin * 2.f;
    if (max_w < 32.f)
        max_w = 32.f;
    float line_h = reader_epub_line_step(scale);
    float cy     = 8.f;
    char  line[640];
    line[0] = '\0';

    const char* r = text;
    int         guard = 0;
    while (*r && guard++ < 12000) {
        while (*r == ' ')
            r++;
        if (!*r)
            break;
        const char* wstart = r;
        while (*r && *r != ' ')
            r++;
        size_t wl = (size_t)(r - wstart);
        char   word[192];
        if (wl >= sizeof(word))
            wl = sizeof(word) - 1;
        memcpy(word, wstart, wl);
        word[wl] = '\0';

        char trial[640];
        if (line[0]) {
            snprintf(trial, sizeof(trial), "%s %s", line, word);
        } else {
            snprintf(trial, sizeof(trial), "%s", word);
        }

        float tw = reader_epub_line_width(trial, scale);
        if (tw > max_w && line[0]) {
            reader_epub_flush_line(margin, &cy, line_h, log_h, pan_y, scale, line, draw);
            line[0] = '\0';
            snprintf(trial, sizeof(trial), "%s", word);
        }
        snprintf(line, sizeof(line), "%s", trial);
    }
    reader_epub_flush_line(margin, &cy, line_h, log_h, pan_y, scale, line, draw);
    return (cy - 8.f) + line_h;
}

static void epub_lines_free(void) {
    free(s_epub_line_off);
    free(s_epub_line_len);
    s_epub_line_off  = NULL;
    s_epub_line_len  = NULL;
    s_epub_line_n    = 0;
    s_epub_lines_doc = NULL;
}

/* Left = EPUB text spine item; right = continuation (no separate spine fetch). */
static bool reader_epub_dual_column(void) {
    return g_app.reader_epub && s_sp[SI_CURR].text[0] && s_sp[SI_CURR].text[0][0]
           && !s_sp[SI_CURR].valid[0] && !s_sp[SI_CURR].valid[1] && !s_sp[SI_CURR].text[1];
}

static float reader_epub_dual_col_w(void) {
    float w = TOP_LOG_W < BOT_LOG_W ? TOP_LOG_W : BOT_LOG_W;
    return w - 16.f;
}

static int s_epub_line_build_cap;

static bool epub_line_table_push(int off, int len) {
    if (len <= 0)
        return true;
    if (s_epub_line_n >= s_epub_line_build_cap) {
        int ncap = s_epub_line_build_cap * 2;
        int* no  = (int*)realloc(s_epub_line_off, (size_t)ncap * sizeof(int));
        int* nl  = (int*)realloc(s_epub_line_len, (size_t)ncap * sizeof(int));
        if (!no || !nl) {
            free(no);
            free(nl);
            return false;
        }
        s_epub_line_off = no;
        s_epub_line_len = nl;
        s_epub_line_build_cap = ncap;
    }
    s_epub_line_off[s_epub_line_n] = off;
    s_epub_line_len[s_epub_line_n] = len;
    s_epub_line_n++;
    return true;
}

/* Build UTF-8 line table for `doc` at column width col_w (same both screens). */
static void epub_rebuild_wrapped_lines(const char* doc, float col_w, float scale) {
    epub_lines_free();
    if (!doc || !doc[0]) {
        s_epub_seg_line0 = 0;
        return;
    }

    float max_w = col_w;
    if (max_w < 32.f)
        max_w = 32.f;

    s_epub_line_build_cap = 256;
    s_epub_line_off       = (int*)malloc((size_t)s_epub_line_build_cap * sizeof(int));
    s_epub_line_len       = (int*)malloc((size_t)s_epub_line_build_cap * sizeof(int));
    if (!s_epub_line_off || !s_epub_line_len) {
        epub_lines_free();
        return;
    }

    const char* line_doc0 = NULL;
    char        line[640];
    line[0] = '\0';
    const char* r = doc;
    int         guard = 0;

    while (*r && guard++ < 200000) {
        if (*r == '\n') {
            if (line[0] && line_doc0) {
                if (!epub_line_table_push((int)(line_doc0 - doc), (int)(r - line_doc0)))
                    break;
                line[0] = '\0';
            }
            r++;
            continue;
        }
        while (*r == ' ')
            r++;
        if (!*r)
            break;
        const char* wstart = r;
        while (*r && *r != ' ' && *r != '\n')
            r++;
        size_t wl = (size_t)(r - wstart);
        char   word[192];
        if (wl >= sizeof(word))
            wl = sizeof(word) - 1;
        memcpy(word, wstart, wl);
        word[wl] = '\0';

        char trial[640];
        if (line[0]) {
            snprintf(trial, sizeof(trial), "%s %s", line, word);
        } else {
            snprintf(trial, sizeof(trial), "%s", word);
        }

        float tw = reader_epub_line_width(trial, scale);
        if (tw > max_w && line[0] && line_doc0) {
            if (!epub_line_table_push((int)(line_doc0 - doc), (int)(wstart - line_doc0)))
                break;
            line[0] = '\0';
            snprintf(trial, sizeof(trial), "%s", word);
        }
        if (!line[0])
            line_doc0 = wstart;
        snprintf(line, sizeof(line), "%s", trial);
    }
    if (line[0] && line_doc0) {
        const char* dend = doc + strlen(doc);
        epub_line_table_push((int)(line_doc0 - doc), (int)(dend - line_doc0));
    }

    s_epub_lines_doc = doc;
    if (s_epub_seg_line0 >= s_epub_line_n)
        s_epub_seg_line0 = 0;
}

static int epub_lines_on_spread_from(int line0) {
    float step = reader_epub_line_step(READER_EPUB_TEXT_TOP);
    float y    = 8.f;
    int   i    = line0;
    int   used = 0;
    while (i < s_epub_line_n && y + step <= TOP_LOG_H - 8.f) {
        y += step;
        i++;
        used++;
    }
    y = 8.f;
    while (i < s_epub_line_n && y + step <= BOT_LOG_H - 8.f) {
        y += step;
        i++;
        used++;
    }
    return used;
}

static int epub_prev_spread_line0(int seg0) {
    if (seg0 <= 0)
        return 0;
    int L = 0;
    for (;;) {
        int n = epub_lines_on_spread_from(L);
        if (n <= 0)
            return 0;
        if (L + n >= seg0)
            return L;
        L += n;
    }
}

static int epub_last_spread_line0(void) {
    if (s_epub_line_n <= 0)
        return 0;
    int L = 0;
    for (;;) {
        int n = epub_lines_on_spread_from(L);
        if (n <= 0)
            return L;
        if (L + n >= s_epub_line_n)
            return L;
        L += n;
    }
}

static bool epub_more_forward(void) {
    if (!reader_epub_dual_column() || s_epub_line_n <= 0)
        return false;
    int n = epub_lines_on_spread_from(s_epub_seg_line0);
    return s_epub_seg_line0 + n < s_epub_line_n;
}

static void reader_epub_maybe_rebuild_lines(void) {
    if (!reader_epub_dual_column())
        return;
    const char* doc = s_sp[SI_CURR].text[0];
    if (doc == s_epub_lines_doc && s_epub_line_n > 0)
        return;
    epub_rebuild_wrapped_lines(doc, reader_epub_dual_col_w(), READER_EPUB_TEXT_TOP);
}

static void reader_epub_draw_line_at(float margin, float y, float scale, int idx) {
    if (idx < 0 || idx >= s_epub_line_n)
        return;
    const char* doc = s_sp[SI_CURR].text[0];
    int         off = s_epub_line_off[idx];
    int         len = s_epub_line_len[idx];
    char        buf[512];
    if (len >= (int)sizeof(buf))
        len = (int)sizeof(buf) - 1;
    memcpy(buf, doc + off, (size_t)len);
    buf[len] = '\0';
    C2D_TextBufClear(g_text_buf);
    C2D_Text t;
    C2D_TextParse(&t, g_text_buf, buf);
    C2D_TextOptimize(&t);
    C2D_DrawText(&t, C2D_WithColor, margin, y, Z_READER_PAGE, scale, scale, COL_WHITE);
}

static void reader_epub_draw_dual_top(void) {
    reader_epub_maybe_rebuild_lines();
    float margin = 8.f;
    float scale  = READER_EPUB_TEXT_TOP;
    float step   = reader_epub_line_step(scale);
    float y      = margin;
    int   i      = s_epub_seg_line0;
    while (i < s_epub_line_n && y + step <= TOP_LOG_H - 8.f) {
        reader_epub_draw_line_at(margin, y, scale, i);
        y += step;
        i++;
    }
}

static void reader_epub_draw_dual_bottom(void) {
    reader_epub_maybe_rebuild_lines();
    float margin = 8.f;
    float scale  = READER_EPUB_TEXT_TOP;
    float step   = reader_epub_line_step(scale);
    float y      = margin;
    int   i      = s_epub_seg_line0;
    while (i < s_epub_line_n && y + step <= TOP_LOG_H - 8.f) {
        i++;
        y += step;
    }
    y = margin;
    while (i < s_epub_line_n && y + step <= BOT_LOG_H - 8.f) {
        reader_epub_draw_line_at(margin, y, scale, i);
        y += step;
        i++;
    }
}

static void clamp_text_pan(float log_h, float content_h, float* pan_y) {
    float view = log_h - 16.f;
    float max_pan = (content_h > view) ? (content_h - view) : 0.f;
    if (*pan_y < 0.f)
        *pan_y = 0.f;
    if (*pan_y > max_pan)
        *pan_y = max_pan;
}

/* ------------------------------------------------------------------ */
/* Lifecycle                                                            */
/* ------------------------------------------------------------------ */
void screen_reader_init(void) {
    for (int i = 0; i < SI_COUNT; i++)
        spread_free(&s_sp[i]);

    reader_page_cache_clear();

    /* PDF: refresh page count / server cache. EPUB uses list count; spine fixed at open. */
    if (!g_app.reader_epub) {
        int info_pages = 0;
        if (kavita_get_chapter_info_pages(g_app.base_url, g_app.token,
                                           g_app.selected_chapter_id, &info_pages)) {
            g_app.reader_total_pages = info_pages;
            if (g_app.reader_page >= g_app.reader_total_pages)
                g_app.reader_page = 0;
        }
    } else if (g_app.reader_page >= g_app.reader_total_pages && g_app.reader_total_pages > 0) {
        g_app.reader_page = 0;
    }

    s_state          = READER_LOADING;
    s_fetch_done     = false;
    s_fetch_failed   = false;
    s_fetch_thread   = NULL;
    s_pending_ready  = false;
    s_show_hint      = false;
    s_hint_frames    = 0;
    s_pages_since_save = 0;
    s_spinner.angle  = 0.0f;
    s_progress_thread          = NULL;
    s_progress_thread_running  = false;

    s_prefetch_thread = NULL;
    s_prefetch_done   = false;

    LightEvent_Init(&s_page_ready_event, RESET_ONESHOT);

    s_pending_is_text   = false;
    s_pending_text_heap = NULL;

    s_epub_seg_line0     = 0;
    s_epub_open_last_seg = false;

    s_zoom_i     = 0;
    s_pan_x[0]   = 0.f;
    s_pan_x[1]   = 0.f;
    s_pan_y[0]   = 0.f;
    s_pan_y[1]   = 0.f;

    try_start_fetch();
}

void screen_reader_fini(void) {
    if (spread_side_ready(&s_sp[SI_CURR], 0) || spread_side_ready(&s_sp[SI_CURR], 1)) {
        fire_save_progress(g_app.reader_page);
        wait_progress_thread();
    }

    wait_prefetch();
    wait_fetch();
    epub_reader_close();

    for (int i = 0; i < SI_COUNT; i++)
        spread_free(&s_sp[i]);

    if (s_pending_ready)
        discard_pending_page();
    if (s_pending_text_heap) {
        free(s_pending_text_heap);
        s_pending_text_heap = NULL;
    }
    s_pending_is_text = false;
    image_prepared_free(&s_pending_prep);
    s_pending_ready = false;

    reader_page_cache_clear();
}

/* ------------------------------------------------------------------ */
/* Tick                                                                 */
/* ------------------------------------------------------------------ */
void screen_reader_tick(void) {
    if (s_pending_ready && LightEvent_TryWait(&s_page_ready_event) != 0) {
        int si   = (int)s_pending_spread_idx;
        int sd   = (int)s_pending_side;
        int src_pg = (int)s_pending_src_page;
        int pg_1   = src_pg + 1;
        int reader_p = g_app.reader_page;
        SpreadPair* sp = spread_at(si);
        int had_tex_before_free = sp->valid[sd] ? 1 : 0;

        if (s_pending_is_text) {
            if (sp->text[sd]) {
                if (sp->text[sd] == s_epub_lines_doc)
                    epub_lines_free();
                free(sp->text[sd]);
                sp->text[sd] = NULL;
            }
            if (sp->valid[sd]) {
                image_texture_free(&sp->tex[sd]);
                sp->valid[sd] = false;
            }
            sp->text[sd]        = s_pending_text_heap;
            s_pending_text_heap = NULL;
            s_pending_is_text   = false;
            if (g_app.reader_epub && si == SI_CURR && sd == 0)
                s_epub_seg_line0 = 0;
            image_prepared_free(&s_pending_prep);
            wait_fetch();
            try_start_fetch();
            if (s_state == READER_LOADING && curr_spread_complete())
                s_state = READER_SHOW;
        } else {
            if (sp->text[sd]) {
                free(sp->text[sd]);
                sp->text[sd] = NULL;
            }
            if (sp->valid[sd]) image_texture_free(&sp->tex[sd]);
            if (image_upload_prepared(&s_pending_prep, &sp->tex[sd])) {
                sp->valid[sd] = true;
                /* Join the HTTP thread before try_start_fetch; it still holds s_fetch_thread. */
                wait_fetch();
                try_start_fetch();
                if (s_state == READER_LOADING && curr_spread_complete())
                    s_state = READER_SHOW;
            } else {
                dlog("[reader][ERR] upload(main) FAIL kavita_page=%d si=%d sd=%d "
                     "reader_P=%d reader_state=%d had_tex_before_replace=%d prep_valid=%d "
                     "prep_tex=%dx%d prep_src=%dx%d spread_logical_pg=%d",
                     pg_1, si, sd, reader_p, (int)s_state, had_tex_before_free,
                     s_pending_prep.valid ? 1 : 0,
                     s_pending_prep.tex_w, s_pending_prep.tex_h,
                     s_pending_prep.src_w, s_pending_prep.src_h,
                     spread_side_page_1based(si, sd));
                image_prepared_free(&s_pending_prep);
                wait_fetch();
                if (si == SI_CURR) {
                    s_state = READER_ERROR;
                    snprintf(s_error_msg, sizeof(s_error_msg),
                             "Failed to decode page %d", pg_1);
                    dlog("[reader][ERR] READER_ERROR set (curr slot) user_msg: %s", s_error_msg);
                } else {
                    dlog("[reader][ERR] preload upload fail (no full-screen error) si=%d", si);
                }
                /* Preload decode errors: no auto-retry here (avoid a tight fail loop). */
            }
        }
        s_pending_ready = false;
    }

    if (s_fetch_thread && s_fetch_done) {
        threadJoin(s_fetch_thread, U64_MAX);
        threadFree(s_fetch_thread);
        s_fetch_thread = NULL;

        if (s_fetch_failed) {
            dlog("[reader][ERR] join thread_fail kavita_page=%d si=%d sd=%d reader_P=%d "
                 "state=%d curr_r=%d/%d",
                 s_fetch_args.page + 1, s_fetch_args.spread_idx, s_fetch_args.side,
                 g_app.reader_page, (int)s_state,
                 spread_side_ready(&s_sp[SI_CURR], 0) ? 1 : 0,
                 spread_side_ready(&s_sp[SI_CURR], 1) ? 1 : 0);
            if (s_state == READER_LOADING && !curr_spread_complete()) {
                s_state = READER_ERROR;
                snprintf(s_error_msg, sizeof(s_error_msg),
                         "Failed to load page %d", s_fetch_args.page + 1);
            }
            s_fetch_failed = false;
        } else if (s_state == READER_LOADING && curr_spread_complete()) {
            s_state = READER_SHOW;
        }
    }

    if (s_prefetch_thread && s_prefetch_done) {
        threadJoin(s_prefetch_thread, U64_MAX);
        threadFree(s_prefetch_thread);
        s_prefetch_thread = NULL;
        s_prefetch_done = false;
    }

    try_start_prefetch();
    try_start_fetch();

    if (curr_spread_complete() && s_state == READER_LOADING) {
        s_state = READER_SHOW;
    }

    reader_epub_maybe_rebuild_lines();
    if (s_epub_open_last_seg && reader_epub_dual_column() && s_epub_line_n > 0) {
        s_epub_seg_line0     = epub_last_spread_line0();
        s_epub_open_last_seg = false;
    }

    if (s_show_hint && s_hint_frames > 0) s_hint_frames--;
    else s_show_hint = false;

    u32 kd = hidKeysDown();

    if (kd & KEY_A) {
        s_show_hint = true;
        s_hint_frames = 180;
    }

    if (g_app.reader_total_pages > 0 && (kd & KEY_START) && !(kd & KEY_SELECT)) {
        int p1;
        if (prompt_goto_page_1based(&p1))
            reader_jump_to_page_1based(p1);
    }

    if (s_state == READER_SHOW || s_state == READER_ERROR || s_state == READER_LOADING) {
        float z = reader_zoom();

        if (s_state == READER_SHOW || s_state == READER_LOADING) {
            /* Book mode: “up/down” along the page is physical D-Right / D-Left (D-Pad at bottom). */
            if (kd & KEY_DRIGHT && s_zoom_i < ZOOM_LEVEL_COUNT - 1) {
                s_zoom_i++;
                s_pan_x[0] = 0.f;
                s_pan_x[1] = 0.f;
                s_pan_y[0] = 0.f;
                s_pan_y[1] = 0.f;
            }
            if (kd & KEY_DLEFT && s_zoom_i > 0) {
                s_zoom_i--;
                s_pan_x[0] = 0.f;
                s_pan_x[1] = 0.f;
                s_pan_y[0] = 0.f;
                s_pan_y[1] = 0.f;
            }

            if (kd & KEY_DUP)
                go_prev_spread();
            if (kd & KEY_DDOWN && can_advance_spread()
                && curr_spread_complete()) {
                go_next_spread();
            }

            bool epub_text_scroll =
                g_app.reader_epub && !reader_epub_dual_column()
                && ((s_sp[SI_CURR].text[0] && s_sp[SI_CURR].text[0][0])
                    || (s_sp[SI_CURR].text[1] && s_sp[SI_CURR].text[1][0]));
            if (z > 1.0005f || epub_text_scroll) {
                circlePosition cir;
                hidCircleRead(&cir);
                /* Book mode (+90°): dx -> vertical pan, dy -> horizontal pan */
                float dx = (float)cir.dx;
                if (dx < -14.f || dx > 14.f) {
                    float adj = dx * 0.2f;
                    s_pan_y[0] += adj;
                    s_pan_y[1] += adj;
                }
                if (z > 1.0005f) {
                    float dy = (float)cir.dy;
                    if (dy < -14.f || dy > 14.f) {
                        float adj = dy * 0.2f;
                        s_pan_x[0] += adj;
                        s_pan_x[1] += adj;
                    }
                }
            }
        }

        if (kd & KEY_B) {
            app_transition(SCREEN_DETAIL);
            return;
        }
    }

    /* Never stay in SHOW while a required page is still missing (avoids one LCD
     * showing stale content because nothing redraws the loading UI). */
    if (s_state == READER_SHOW && !curr_spread_complete())
        s_state = READER_LOADING;

    if (s_state == READER_LOADING) ui_spinner_tick(&s_spinner);

    if (s_state != READER_ERROR) {
        float zz = reader_zoom();
        if (s_sp[SI_CURR].valid[0]) {
            clamp_pan_x(TOP_LOG_W, TOP_LOG_H, &s_sp[SI_CURR].tex[0], zz, &s_pan_x[0]);
            clamp_pan_y(TOP_LOG_W, TOP_LOG_H, &s_sp[SI_CURR].tex[0], zz, &s_pan_y[0]);
        } else if (g_app.reader_epub && s_sp[SI_CURR].text[0] && !reader_epub_dual_column()) {
            float ch = reader_epub_text_layout(TOP_LOG_W, TOP_LOG_H, s_sp[SI_CURR].text[0],
                                               READER_EPUB_TEXT_TOP, s_pan_y[0], false);
            clamp_text_pan(TOP_LOG_H, ch, &s_pan_y[0]);
            s_pan_x[0] = 0.f;
        }
        if (s_sp[SI_CURR].valid[1]) {
            clamp_pan_x(BOT_LOG_W, BOT_LOG_H, &s_sp[SI_CURR].tex[1], zz, &s_pan_x[1]);
            clamp_pan_y(BOT_LOG_W, BOT_LOG_H, &s_sp[SI_CURR].tex[1], zz, &s_pan_y[1]);
        } else if (g_app.reader_epub && s_sp[SI_CURR].text[1] && !reader_epub_dual_column()) {
            float ch = reader_epub_text_layout(BOT_LOG_W, BOT_LOG_H, s_sp[SI_CURR].text[1],
                                               READER_EPUB_TEXT_BOT, s_pan_y[1], false);
            clamp_text_pan(BOT_LOG_H, ch, &s_pan_y[1]);
            s_pan_x[1] = 0.f;
        }
    }

    /* --- Top screen: left page --- */
    C2D_SceneBegin(g_target_top);
    /* Scissor is (left, top, right, bottom) in *framebuffer* pixels. GSP stores
     * the top LCD as 240×400 and the bottom as 240×320 — not the physical
     * “landscape” 400×240 / 320×240 sizes. Wrong axes clip most of the page. */
    C3D_SetScissor(GPU_SCISSOR_NORMAL, 0, 0, (u32)TOP_LOG_W, (u32)TOP_LOG_H);
    {
        C3D_Mtx save;
        C2D_ViewSave(&save);
        reader_apply_book_transform(TOP_PHYS_W, TOP_PHYS_H);

        if (s_state == READER_ERROR) {
            reader_text_centered(s_error_msg, TOP_LOG_W, TOP_LOG_H * 0.5f - 20.f,
                                 FONT_MED, COL_ERROR);
        } else if (g_app.reader_page < g_app.reader_total_pages
                   && !spread_side_ready(&s_sp[SI_CURR], 0)) {
            reader_spinner_draw(&s_spinner, TOP_LOG_W * 0.5f, TOP_LOG_H * 0.5f,
                                28.0f, COL_ACCENT);
            char loading_str[64];
            snprintf(loading_str, sizeof(loading_str),
                     "Loading %d...", g_app.reader_page + 1);
            reader_text_centered(loading_str, TOP_LOG_W, TOP_LOG_H * 0.5f + 48.f,
                                 FONT_MED, COL_GREY);
        } else if (spread_side_ready(&s_sp[SI_CURR], 0)) {
            if (s_sp[SI_CURR].valid[0]) {
                draw_portrait_page_zoom(TOP_LOG_W, TOP_LOG_H, &s_sp[SI_CURR].tex[0],
                                        s_pan_x[0], s_pan_y[0], reader_zoom());
            } else if (reader_epub_dual_column()) {
                reader_epub_draw_dual_top();
            } else {
                reader_epub_text_layout(TOP_LOG_W, TOP_LOG_H, s_sp[SI_CURR].text[0],
                                        READER_EPUB_TEXT_TOP, s_pan_y[0], true);
            }
        }

        char pg[40];
        snprintf(pg, sizeof(pg), "%d/%d",
                 g_app.reader_page + 1, g_app.reader_total_pages);
        C2D_DrawRectSolid(TOP_LOG_W - 44, 4, Z_READER_PAGE_BADGE_BG, 40, 16,
                          C2D_Color32(0, 0, 0, 180));
        reader_text_draw_z(pg, TOP_LOG_W - 42, 5, FONT_TINY, COL_WHITE,
                           Z_READER_HUD_FG);
        if (reader_zoom() > 1.001f) {
            char zb[16];
            snprintf(zb, sizeof(zb), "%.2gx", reader_zoom());
            reader_text_draw_z(zb, TOP_LOG_W - 42, 22, FONT_TINY, COL_GREY,
                               Z_READER_HUD_FG);
        }

        C2D_ViewRestore(&save);
    }

    /* --- Bottom screen: right page --- */
    C2D_SceneBegin(g_target_bottom);
    C3D_SetScissor(GPU_SCISSOR_NORMAL, 0, 0, (u32)BOT_LOG_W, (u32)BOT_LOG_H);
    {
        C3D_Mtx save;
        C2D_ViewSave(&save);
        reader_apply_book_transform(BOT_PHYS_W, BOT_PHYS_H);

        if (reader_epub_dual_column()) {
            if (s_state == READER_ERROR) {
                reader_text_centered(s_error_msg, BOT_LOG_W, BOT_LOG_H * 0.5f,
                                     FONT_SMALL, COL_ERROR);
            } else if (spread_side_ready(&s_sp[SI_CURR], 0)) {
                reader_epub_draw_dual_bottom();
            } else {
                reader_spinner_draw(&s_spinner, BOT_LOG_W * 0.5f, BOT_LOG_H * 0.5f,
                                    24.0f, COL_ACCENT);
                char loading_str[64];
                snprintf(loading_str, sizeof(loading_str),
                         "Loading %d...", g_app.reader_page + 1);
                reader_text_centered(loading_str, BOT_LOG_W,
                                     BOT_LOG_H * 0.5f + 40.f, FONT_SMALL, COL_GREY);
            }
        } else if (g_app.reader_page + 1 < g_app.reader_total_pages) {
            if (s_state == READER_ERROR) {
                reader_text_centered(s_error_msg, BOT_LOG_W, BOT_LOG_H * 0.5f,
                                     FONT_SMALL, COL_ERROR);
            } else if (spread_side_ready(&s_sp[SI_CURR], 1)) {
                if (s_sp[SI_CURR].valid[1]) {
                    draw_portrait_page_zoom(BOT_LOG_W, BOT_LOG_H, &s_sp[SI_CURR].tex[1],
                                            s_pan_x[1], s_pan_y[1], reader_zoom());
                } else {
                    reader_epub_text_layout(BOT_LOG_W, BOT_LOG_H, s_sp[SI_CURR].text[1],
                                            READER_EPUB_TEXT_BOT, s_pan_y[1], true);
                }
            } else if (spread_side_ready(&s_sp[SI_CURR], 0)) {
                /* Right page still fetching: always redraw (not gated on LOADING). */
                reader_spinner_draw(&s_spinner, BOT_LOG_W * 0.5f, BOT_LOG_H * 0.5f,
                                    24.0f, COL_ACCENT);
                char loading_str[64];
                snprintf(loading_str, sizeof(loading_str),
                         "Loading %d...", g_app.reader_page + 2);
                reader_text_centered(loading_str, BOT_LOG_W,
                                     BOT_LOG_H * 0.5f + 40.f, FONT_SMALL, COL_GREY);
            } else {
                /* Left page still loading too (e.g. user advanced before preload). */
                reader_spinner_draw(&s_spinner, BOT_LOG_W * 0.5f, BOT_LOG_H * 0.5f,
                                    24.0f, COL_ACCENT);
                char loading_str[64];
                snprintf(loading_str, sizeof(loading_str),
                         "Loading %d...", g_app.reader_page + 2);
                reader_text_centered(loading_str, BOT_LOG_W,
                                     BOT_LOG_H * 0.5f + 40.f, FONT_SMALL, COL_GREY);
            }
        } else {
            C2D_DrawRectSolid(0, 0, 0.4f, BOT_LOG_W, BOT_LOG_H, COL_DARK);
            reader_text_centered("(end)", BOT_LOG_W, BOT_LOG_H * 0.5f,
                                 FONT_SMALL, COL_GREY);
        }

        char pg2[40];
        snprintf(pg2, sizeof(pg2), "%d/%d",
                 reader_epub_dual_column() ? (g_app.reader_page + 1)
                                           : (g_app.reader_page + 2),
                 g_app.reader_total_pages);
        if (reader_epub_dual_column() || g_app.reader_page + 1 < g_app.reader_total_pages) {
            C2D_DrawRectSolid(BOT_LOG_W - 44, 4, Z_READER_PAGE_BADGE_BG, 40, 16,
                              C2D_Color32(0, 0, 0, 180));
            reader_text_draw_z(pg2, BOT_LOG_W - 42, 5, FONT_TINY, COL_WHITE,
                               Z_READER_HUD_FG);
            if (reader_zoom() > 1.001f) {
                char zb[16];
                snprintf(zb, sizeof(zb), "%.2gx", reader_zoom());
                reader_text_draw_z(zb, BOT_LOG_W - 42, 22, FONT_TINY, COL_GREY,
                                   Z_READER_HUD_FG);
            }
        }

        if (s_show_hint) {
            /* y is text baseline; tight top padding so the first line sits near the panel top. */
            const float hint_top = BOT_LOG_H - 92.f;
            const float hint_h   = 86.f;
            const float line_y0  = BOT_LOG_H - 84.f;
            const float line_step = 15.f;
            C2D_DrawRectSolid(8, hint_top, Z_READER_HINT_BG, BOT_LOG_W - 16,
                              hint_h, C2D_Color32(0, 0, 0, 200));
            reader_text_centered_z("D-Pad L/R: Zoom (images; not EPUB text size)",
                                    BOT_LOG_W, line_y0, FONT_TINY, COL_WHITE,
                                    Z_READER_HUD_FG);
            reader_text_centered_z("Circle Pad: Pan when zoomed / scroll EPUB text",
                                    BOT_LOG_W, line_y0 + line_step, FONT_TINY,
                                    COL_GREY, Z_READER_HUD_FG);
            reader_text_centered_z("Start: Go To page", BOT_LOG_W,
                                    line_y0 + 2.f * line_step, FONT_TINY,
                                    COL_GREY, Z_READER_HUD_FG);
            reader_text_centered_z("B: Back to Chapter List", BOT_LOG_W,
                                    line_y0 + 3.f * line_step, FONT_TINY,
                                    COL_GREY, Z_READER_HUD_FG);
            reader_text_centered_z("D-Pad Up/Down: Previous / Next",
                                    BOT_LOG_W, line_y0 + 4.f * line_step,
                                    FONT_TINY, COL_GREY, Z_READER_HUD_FG);
        }

        C2D_ViewRestore(&save);
    }
}
