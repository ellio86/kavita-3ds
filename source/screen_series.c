#include "screen_series.h"
#include "app.h"
#include "cover_cache.h"
#include "kavita_api.h"
#include "image_loader.h"
#include "http_client.h"
#include "ui.h"

#include <3ds.h>
#include <3ds/gpu/enums.h>
#include <citro3d.h>
#include <math.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* ------------------------------------------------------------------ */
/* Layout & library limits                                              */
/* ------------------------------------------------------------------ */
#define GRID_COLS         5
#define GRID_VISIBLE_ROWS 2   /* top screen fits two thumb rows under header */
#define THUMB_W           70
#define THUMB_H           92
#define THUMB_PAD_X       8
#define THUMB_PAD_Y       6
#define THUMB_OFFSET_X    5
#define THUMB_OFFSET_Y    28

/* Horizontal strip: full top screen (no title bar); HUD like reader */
#define ROW_THUMB_W       191
#define ROW_THUMB_H       228
#define ROW_PAD_X         10
#define ROW_MARGIN_H      8    /* total horizontal margin (split for centering) */
#define ROW_HUD_PAD_X     6.f
#define ROW_HUD_PAD_Y     4.f
#define ROW_HUD_ALPHA     180   /* same as reader page badge */
#define Z_ROW_BG          0.5f
#define Z_ROW_SELECTION   0.51f
#define Z_ROW_IMAGE       0.52f
#define Z_ROW_HUD_BG      0.55f
#define Z_ROW_HUD_FG      0.6f
#define ROW_PRELOAD_LEFT  2     /* indices before first visible */
#define ROW_PRELOAD_RIGHT 2     /* after last visible index */

/* Book / portrait spread: same rotation & logical sizes as reader */
#define BOOK_ROT_DEG      90.0f
#define SER_TOP_PHYS_W    400.0f
#define SER_TOP_PHYS_H    240.0f
#define SER_BOT_PHYS_W    320.0f
#define SER_BOT_PHYS_H    240.0f
#define SER_TOP_LOG_W     SER_TOP_PHYS_H
#define SER_TOP_LOG_H     SER_TOP_PHYS_W
#define SER_BOT_LOG_W     SER_BOT_PHYS_H
#define SER_BOT_LOG_H     SER_BOT_PHYS_W

#define SER_ZOOM_LEVEL_COUNT 7
static const float s_series_zoom_levels[SER_ZOOM_LEVEL_COUNT] =
    { 1.0f, 1.35f, 1.75f, 2.25f, 2.75f, 3.35f, 4.0f };

#define Z_BOOK_PAGE       0.5f
#define Z_BOOK_SELECTION  0.53f /* left (selected) cover frame — above image */
#define BOOK_SEL_BORDER   5.f
#define Z_BOOK_HUD_BG     0.55f
#define Z_BOOK_HUD_FG     0.6f
#define Z_BOOK_HINT_BG    0.55f

#define MAX_SERIES_LIST   1024   /* max series per library (RAM ~0.5 MiB) */
#define FETCH_CHUNK       100    /* items per API request while scanning */
#define COVER_POOL_SIZE   24     /* GPU textures — rest load on demand */

/* ------------------------------------------------------------------ */
/* Series list (full library, loaded in chunks on one screen visit)     */
/* ------------------------------------------------------------------ */
static KavitaSeries  s_series[MAX_SERIES_LIST];
static int           s_series_count;
static int           s_total_count;
static int           s_selected;
static int           s_scroll_row;
static int           s_scroll_first; /* row view: first visible series index */

typedef enum {
    SERIES_VIEW_GRID,
    SERIES_VIEW_ROW,
    SERIES_VIEW_BOOK,
} SeriesViewMode;

static SeriesViewMode s_view_mode;

/* Book spread: top = series[s_selected], bottom = next or (end) */
static int           s_book_zoom_i;
static float         s_book_pan_x[2];
static float         s_book_pan_y[2];
static bool          s_book_show_hint;
static int           s_book_hint_frames;

/* ------------------------------------------------------------------ */
/* Cover pool: series index -> pool slot; bounded GPU memory            */
/* ------------------------------------------------------------------ */
static LoadedTexture s_cover_pool[COVER_POOL_SIZE];
static bool          s_cover_valid_pool[COVER_POOL_SIZE];
static int           s_pool_owner[COVER_POOL_SIZE];
static short         s_series_to_pool[MAX_SERIES_LIST];

/* ------------------------------------------------------------------ */
/* State                                                                */
/* ------------------------------------------------------------------ */
typedef enum {
    STATE_LOADING_LIST,
    STATE_READY,
    STATE_ERROR,
} SeriesState;

static SeriesState   s_state;
static char          s_error_msg[128];
static UiSpinner     s_spinner;

/* ------------------------------------------------------------------ */
/* Background threads                                                   */
/* ------------------------------------------------------------------ */

static Thread        s_list_thread;
static volatile bool s_list_done;

static Thread        s_cover_thread;
static volatile bool s_cover_thread_done;
static volatile int  s_cover_fetch_idx;
static LightEvent    s_cover_ready_event;
static PreparedTexture s_pending_prep;
static int           s_pending_series_idx;
static volatile bool s_pending_ready;

/* ------------------------------------------------------------------ */
/* List fetch — concatenate all pages into s_series[]                   */
/* ------------------------------------------------------------------ */
static void list_thread(void* arg) {
    (void)arg;
    int total = 0;
    int api_page = 1;

    while (total < MAX_SERIES_LIST) {
        int space = MAX_SERIES_LIST - total;
        int ask   = FETCH_CHUNK < space ? FETCH_CHUNK : space;
        int n     = kavita_get_series(
            g_app.base_url, g_app.token,
            g_app.selected_library_id,
            api_page, ask,
            s_series + total, ask,
            NULL);

        if (n < 0) {
            if (total == 0)
                s_series_count = -1;
            else {
                s_series_count = total;
                s_total_count  = total;
            }
            break;
        }
        if (n == 0) {
            s_series_count = total;
            s_total_count  = total;
            break;
        }

        total += n;
        if (n < ask) {
            s_series_count = total;
            s_total_count  = total;
            break;
        }
        api_page++;
    }

    if (total >= MAX_SERIES_LIST) {
        s_series_count = total;
        s_total_count  = total;
    }

    s_list_done = true;
    threadExit(0);
}

static void cover_fetch_thread(void* arg) {
    int series_idx = (int)(intptr_t)arg;
    if (series_idx < 0 || series_idx >= s_series_count) {
        s_cover_thread_done = true;
        threadExit(0);
    }

    char url[512];
    kavita_cover_url(g_app.base_url, g_app.api_key,
                     s_series[series_idx].id, url, sizeof(url));

    PreparedTexture prep;
    memset(&prep, 0, sizeof(prep));
    bool ok = false;

    if (g_app.cover_cache) {
        u8* cached = NULL;
        size_t csz = 0;
        if (cover_cache_read(g_app.base_url, 's', s_series[series_idx].id,
                             &cached, &csz)) {
            ok = image_prepare_from_mem(cached, csz, &prep);
            free(cached);
        }
    }

    if (!ok) {
        HttpResponse* resp = http_get_binary(url, g_app.token);
        if (!resp || resp->status_code != 200) {
            http_response_free(resp);
            s_cover_thread_done = true;
            threadExit(0);
        }
        ok = image_prepare_from_mem((const u8*)resp->data, resp->size, &prep);
        if (ok && g_app.cover_cache)
            cover_cache_write(g_app.base_url, 's', s_series[series_idx].id,
                              (const u8*)resp->data, resp->size);
        http_response_free(resp);
    }

    if (!ok) {
        s_cover_thread_done = true;
        threadExit(0);
    }

    if (ok) {
        s_pending_prep        = prep;
        s_pending_series_idx  = series_idx;
        s_pending_ready       = true;
        LightEvent_Signal(&s_cover_ready_event);
    }

    s_cover_thread_done = true;
    threadExit(0);
}

/* ------------------------------------------------------------------ */
/* Cover pool helpers                                                   */
/* ------------------------------------------------------------------ */
static void cover_pool_clear_mappings(void) {
    for (int i = 0; i < MAX_SERIES_LIST; i++)
        s_series_to_pool[i] = -1;
    for (int i = 0; i < COVER_POOL_SIZE; i++)
        s_pool_owner[i] = -1;
}

static void cover_pool_free_textures(void) {
    for (int i = 0; i < COVER_POOL_SIZE; i++) {
        if (s_cover_valid_pool[i]) {
            image_texture_free(&s_cover_pool[i]);
            s_cover_valid_pool[i] = false;
        }
    }
}

static void cover_pool_reset(void) {
    cover_pool_free_textures();
    cover_pool_clear_mappings();
}

static bool cover_has_texture(int series_idx) {
    if (series_idx < 0 || series_idx >= MAX_SERIES_LIST)
        return false;
    int pi = s_series_to_pool[series_idx];
    return pi >= 0 && pi < COVER_POOL_SIZE && s_cover_valid_pool[pi];
}

static int row_slots_on_screen(void) {
    int avail = 400 - ROW_MARGIN_H;
    int n     = 1;
    for (int try = 16; try >= 1; try--) {
        int row_w = try * ROW_THUMB_W + (try - 1) * ROW_PAD_X;
        if (row_w <= avail) {
            n = try;
            break;
        }
    }
    return n;
}

static float series_book_zoom(void) {
    return s_series_zoom_levels[s_book_zoom_i];
}

static void series_book_reset_view(void) {
    s_book_zoom_i      = 0;
    s_book_pan_x[0]    = 0.f;
    s_book_pan_y[0]    = 0.f;
    s_book_pan_x[1]    = 0.f;
    s_book_pan_y[1]    = 0.f;
    s_book_show_hint   = false;
    s_book_hint_frames = 0;
}

static void series_book_apply_transform(float phys_w, float phys_h) {
    float lw = phys_h;
    float lh = phys_w;
    C2D_ViewTranslate(phys_w * 0.5f, phys_h * 0.5f);
    C2D_ViewRotateDegrees(BOOK_ROT_DEG);
    C2D_ViewTranslate(-lw * 0.5f, -lh * 0.5f);
}

static void series_clamp_pan_x(float log_w, float log_h, const LoadedTexture* tex,
                               float zoom, float* pan_x) {
    if (!tex || !tex->valid)
        return;
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
    if (*pan_x > lim)
        *pan_x = lim;
    if (*pan_x < -lim)
        *pan_x = -lim;
}

static void series_clamp_pan_y(float log_w, float log_h, const LoadedTexture* tex,
                               float zoom, float* pan_y) {
    if (!tex || !tex->valid)
        return;
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
    if (*pan_y > lim)
        *pan_y = lim;
    if (*pan_y < -lim)
        *pan_y = -lim;
}

static void series_book_draw_selection_frame(float log_w, float log_h) {
    const float z = Z_BOOK_SELECTION;
    const u32   c = COL_ERROR;
    C2D_DrawRectSolid(0, 0, z, log_w, BOOK_SEL_BORDER, c);
    C2D_DrawRectSolid(0, log_h - BOOK_SEL_BORDER, z, log_w, BOOK_SEL_BORDER, c);
    C2D_DrawRectSolid(0, 0, z, BOOK_SEL_BORDER, log_h, c);
    C2D_DrawRectSolid(log_w - BOOK_SEL_BORDER, 0, z, BOOK_SEL_BORDER, log_h, c);
}

static void series_draw_portrait_cover_zoom(float log_w, float log_h,
                                            const LoadedTexture* tex,
                                            float pan_x, float pan_y,
                                            float zoom) {
    if (!tex || !tex->valid)
        return;
    float s, sy, ox, oy;
    image_fit_dims(tex->src_width, tex->src_height,
                   (int)log_w, (int)log_h, &s, &sy, &ox, &oy);
    (void)sy;
    float z     = zoom;
    float scale = s * z;
    float draw_w = tex->src_width * scale;
    float draw_h = tex->src_height * scale;
    float cx     = (log_w - draw_w) * 0.5f + pan_x;
    float cy     = (log_h - draw_h) * 0.5f + pan_y;
    C2D_DrawImageAt(tex->image, cx, cy, Z_BOOK_PAGE, NULL, scale, scale);
}

static void series_text_centered_z(const char* str, float log_w, float y,
                                   float scale, u32 color, float z) {
    C2D_Text t;
    C2D_TextBufClear(g_text_buf);
    C2D_TextParse(&t, g_text_buf, str);
    C2D_TextOptimize(&t);
    float tw, th;
    C2D_TextGetDimensions(&t, scale, scale, &tw, &th);
    (void)th;
    float dx = (log_w - tw) * 0.5f;
    if (dx < 0)
        dx = 0;
    C2D_DrawText(&t, C2D_WithColor, dx, y, z, scale, scale, color);
}

static void series_spinner_draw(float cx, float cy, float r, u32 color) {
    int   segs = 12;
    float arc  = 120.0f * ((float)M_PI / 180.0f);
    float base = s_spinner.angle * ((float)M_PI / 180.0f);

    for (int i = 0; i < segs; i++) {
        float a0 = base + arc * ((float)i / segs);
        float a1 = base + arc * ((float)(i + 1) / segs);
        float x0 = cx + cosf(a0) * r;
        float y0 = cy + sinf(a0) * r;
        float x1 = cx + cosf(a1) * r;
        float y1 = cy + sinf(a1) * r;
        u32 alpha = (u32)(255 * (float)(i + 1) / segs);
        u32 c     = (color & 0x00ffffff) | (alpha << 24);
        C2D_DrawLine(x0, y0, c, x1, y1, c, 2.0f, Z_BOOK_PAGE);
    }
}

static const LoadedTexture* series_pool_tex(int series_idx) {
    if (series_idx < 0 || series_idx >= s_series_count)
        return NULL;
    int pi = s_series_to_pool[series_idx];
    if (pi < 0 || pi >= COVER_POOL_SIZE || !s_cover_valid_pool[pi])
        return NULL;
    return &s_cover_pool[pi];
}

static void visible_cell_indices(int* out, int* out_count) {
    *out_count = 0;
    if (s_view_mode == SERIES_VIEW_ROW) {
        int nvis = row_slots_on_screen();
        for (int j = 0; j < nvis; j++) {
            int idx = s_scroll_first + j;
            if (idx < s_series_count && *out_count < 16)
                out[(*out_count)++] = idx;
        }
        for (int k = 0; k < ROW_PRELOAD_RIGHT && *out_count < 16; k++) {
            int idx = s_scroll_first + nvis + k;
            if (idx < s_series_count)
                out[(*out_count)++] = idx;
        }
        for (int k = 1; k <= ROW_PRELOAD_LEFT && *out_count < 16; k++) {
            int idx = s_scroll_first - k;
            if (idx >= 0)
                out[(*out_count)++] = idx;
        }
    } else if (s_view_mode == SERIES_VIEW_BOOK) {
        for (int j = 0; j < 2; j++) {
            int idx = s_selected + j;
            if (idx < s_series_count && *out_count < 16)
                out[(*out_count)++] = idx;
        }
        for (int k = 1; k <= 3 && *out_count < 16; k++) {
            int idx = s_selected - k;
            if (idx >= 0)
                out[(*out_count)++] = idx;
        }
        for (int k = 2; k <= 5 && *out_count < 16; k++) {
            int idx = s_selected + k;
            if (idx < s_series_count)
                out[(*out_count)++] = idx;
        }
    } else {
        for (int vr = 0; vr < GRID_VISIBLE_ROWS; vr++) {
            for (int c = 0; c < GRID_COLS; c++) {
                int idx = (s_scroll_row + vr) * GRID_COLS + c;
                if (idx < s_series_count)
                    out[(*out_count)++] = idx;
            }
        }
    }
}

static int cover_find_free_pool_slot(void) {
    for (int i = 0; i < COVER_POOL_SIZE; i++) {
        if (s_pool_owner[i] < 0)
            return i;
    }
    return -1;
}

static void cover_evict_pool_slot(int pool_idx) {
    if (pool_idx < 0 || pool_idx >= COVER_POOL_SIZE)
        return;
    int owner = s_pool_owner[pool_idx];
    if (owner >= 0 && owner < MAX_SERIES_LIST)
        s_series_to_pool[owner] = -1;
    if (s_cover_valid_pool[pool_idx]) {
        image_texture_free(&s_cover_pool[pool_idx]);
        s_cover_valid_pool[pool_idx] = false;
    }
    s_pool_owner[pool_idx] = -1;
}

static int cover_choose_eviction_victim(void) {
    int vis[16], nv;
    visible_cell_indices(vis, &nv);

    int best     = 0;
    int best_dist = -1;
    for (int i = 0; i < COVER_POOL_SIZE; i++) {
        int o = s_pool_owner[i];
        if (o < 0)
            return i;

        bool on_vis = false;
        for (int k = 0; k < nv; k++) {
            if (vis[k] == o) {
                on_vis = true;
                break;
            }
        }
        if (on_vis)
            continue;

        int d;
        if (s_view_mode == SERIES_VIEW_ROW) {
            d = o > s_selected ? o - s_selected : s_selected - o;
        } else if (s_view_mode == SERIES_VIEW_BOOK) {
            d = o > s_selected ? o - s_selected : s_selected - o;
            if (s_selected + 1 < s_series_count) {
                int d1 = o > s_selected + 1 ? o - (s_selected + 1)
                                            : (s_selected + 1) - o;
                if (d1 < d)
                    d = d1;
            }
        } else {
            int row = o / GRID_COLS, sr = s_selected / GRID_COLS;
            int col = o % GRID_COLS, sc = s_selected % GRID_COLS;
            d = (row > sr ? row - sr : sr - row) +
                (col > sc ? col - sc : sc - col);
        }
        if (best_dist < 0 || d > best_dist) {
            best_dist = d;
            best      = i;
        }
    }
    return best;
}

static void cover_install_prepared(int series_idx, PreparedTexture* prep) {
    if (series_idx < 0 || series_idx >= s_series_count)
        return;

    int pool = s_series_to_pool[series_idx];
    if (pool < 0) {
        pool = cover_find_free_pool_slot();
        if (pool < 0) {
            pool = cover_choose_eviction_victim();
            cover_evict_pool_slot(pool);
        }
        s_pool_owner[pool]           = series_idx;
        s_series_to_pool[series_idx] = (short)pool;
    }

    if (s_cover_valid_pool[pool]) {
        image_texture_free(&s_cover_pool[pool]);
        s_cover_valid_pool[pool] = false;
    }
    if (image_upload_prepared(prep, &s_cover_pool[pool]))
        s_cover_valid_pool[pool] = true;
    else
        image_prepared_free(prep);
}

static bool cover_is_fetching(int series_idx) {
    return s_cover_thread && !s_cover_thread_done &&
           s_cover_fetch_idx == series_idx;
}

static int cover_pick_next_fetch_idx(void) {
    int vis[16], nv;
    visible_cell_indices(vis, &nv);
    for (int k = 0; k < nv; k++) {
        int i = vis[k];
        if (cover_has_texture(i) || cover_is_fetching(i))
            continue;
        return i;
    }
    for (int ring = 1; ring < 400; ring++) {
        int a = s_selected + ring;
        if (a < s_series_count && !cover_has_texture(a) && !cover_is_fetching(a))
            return a;
        int b = s_selected - ring;
        if (b >= 0 && !cover_has_texture(b) && !cover_is_fetching(b))
            return b;
    }
    return -1;
}

static void start_cover_fetch(int series_idx) {
    if (series_idx < 0 || series_idx >= s_series_count)
        return;
    s_cover_thread_done = false;
    s_cover_fetch_idx   = series_idx;
    s_cover_thread = threadCreate(cover_fetch_thread,
                                  (void*)(intptr_t)series_idx,
                                  48 * 1024, 0x30, 1, false);
}

static void kick_cover_worker(void) {
    if (s_cover_thread)
        return;
    int next = cover_pick_next_fetch_idx();
    if (next >= 0)
        start_cover_fetch(next);
}

/* ------------------------------------------------------------------ */
/* Scroll                                                               */
/* ------------------------------------------------------------------ */
static void series_sync_scroll(void) {
    if (s_series_count <= 0 || s_total_count <= 0) {
        s_scroll_row   = 0;
        s_scroll_first = 0;
        return;
    }
    int slot = s_selected;
    if (slot < 0 || slot >= s_series_count)
        return;

    if (s_view_mode == SERIES_VIEW_ROW) {
        int nvis = row_slots_on_screen();
        if (slot < s_scroll_first)
            s_scroll_first = slot;
        if (slot >= s_scroll_first + nvis)
            s_scroll_first = slot - nvis + 1;

        int max_first = s_series_count - nvis;
        if (max_first < 0)
            max_first = 0;
        if (s_scroll_first > max_first)
            s_scroll_first = max_first;
        if (s_scroll_first < 0)
            s_scroll_first = 0;
    } else {
        int row = slot / GRID_COLS;
        if (row < s_scroll_row)
            s_scroll_row = row;
        if (row >= s_scroll_row + GRID_VISIBLE_ROWS)
            s_scroll_row = row - GRID_VISIBLE_ROWS + 1;

        int rows_in_page = (s_series_count + GRID_COLS - 1) / GRID_COLS;
        int max_scroll   = rows_in_page > GRID_VISIBLE_ROWS
                               ? rows_in_page - GRID_VISIBLE_ROWS
                               : 0;
        if (s_scroll_row > max_scroll)
            s_scroll_row = max_scroll;
        if (s_scroll_row < 0)
            s_scroll_row = 0;
    }
}

/* ------------------------------------------------------------------ */
/* Lifecycle                                                            */
/* ------------------------------------------------------------------ */
void screen_series_init(void) {
    s_series_count = 0;
    s_total_count  = 0;
    s_selected     = 0;
    s_scroll_row   = 0;
    s_scroll_first = 0;
    s_view_mode    = SERIES_VIEW_GRID;
    series_book_reset_view();
    s_state        = STATE_LOADING_LIST;
    s_list_done    = false;
    s_cover_thread = NULL;
    s_cover_thread_done = false;
    s_pending_ready = false;
    s_spinner.angle = 0.0f;

    cover_pool_reset();
    LightEvent_Init(&s_cover_ready_event, RESET_ONESHOT);

    s_list_thread = threadCreate(list_thread, NULL,
                                 32 * 1024, 0x30, 1, false);
}

void screen_series_fini(void) {
    if (s_list_thread) {
        threadJoin(s_list_thread, U64_MAX);
        threadFree(s_list_thread);
        s_list_thread = NULL;
    }
    if (s_cover_thread) {
        threadJoin(s_cover_thread, U64_MAX);
        threadFree(s_cover_thread);
        s_cover_thread = NULL;
    }
    if (s_pending_ready) {
        image_prepared_free(&s_pending_prep);
        s_pending_ready = false;
    }
    cover_pool_reset();
}

/* ------------------------------------------------------------------ */
/* Tick                                                                 */
/* ------------------------------------------------------------------ */
void screen_series_tick(void) {
    switch (s_state) {
        case STATE_LOADING_LIST:
            ui_spinner_tick(&s_spinner);
            if (s_list_done) {
                threadJoin(s_list_thread, U64_MAX);
                threadFree(s_list_thread);
                s_list_thread = NULL;

                if (s_series_count < 0) {
                    s_state = STATE_ERROR;
                    snprintf(s_error_msg, sizeof(s_error_msg),
                             "Failed to load series");
                    s_series_count = 0;
                } else {
                    cover_pool_reset();
                    s_state = STATE_READY;
                    series_sync_scroll();
                    kick_cover_worker();
                }
            }
            break;

        case STATE_READY:
            if (s_pending_ready &&
                LightEvent_TryWait(&s_cover_ready_event) != 0) {
                int si = s_pending_series_idx;
                if (si >= 0 && si < s_series_count)
                    cover_install_prepared(si, &s_pending_prep);
                else
                    image_prepared_free(&s_pending_prep);
                s_pending_ready = false;
            }

            if (s_cover_thread && s_cover_thread_done) {
                threadJoin(s_cover_thread, U64_MAX);
                threadFree(s_cover_thread);
                s_cover_thread = NULL;
            }
            kick_cover_worker();

            if (s_view_mode == SERIES_VIEW_BOOK)
                ui_spinner_tick(&s_spinner);

            {
                u32 kd = hidKeysDown();

                if (kd & KEY_B) {
                    app_transition(SCREEN_LIBRARIES);
                    return;
                }

                if (kd & KEY_A) {
                    if (s_selected >= 0 && s_selected < s_series_count) {
                        g_app.selected_series_id = s_series[s_selected].id;
                        strncpy(g_app.selected_series_name,
                                s_series[s_selected].name,
                                sizeof(g_app.selected_series_name) - 1);
                        app_transition(SCREEN_DETAIL);
                        return;
                    }
                }

                if (kd & KEY_Y) {
                    if (s_view_mode == SERIES_VIEW_GRID)
                        s_view_mode = SERIES_VIEW_ROW;
                    else if (s_view_mode == SERIES_VIEW_ROW) {
                        s_view_mode = SERIES_VIEW_BOOK;
                        series_book_reset_view();
                    } else
                        s_view_mode = SERIES_VIEW_GRID;
                    series_sync_scroll();
                }

                if (s_view_mode == SERIES_VIEW_BOOK) {
                    float zz = series_book_zoom();
                    if (kd & KEY_X) {
                        s_book_show_hint  = true;
                        s_book_hint_frames = 180;
                    }
                    if (kd & KEY_DRIGHT && s_book_zoom_i < SER_ZOOM_LEVEL_COUNT - 1) {
                        s_book_zoom_i++;
                        s_book_pan_x[0] = s_book_pan_y[0] = 0.f;
                        s_book_pan_x[1] = s_book_pan_y[1] = 0.f;
                    }
                    if (kd & KEY_DLEFT && s_book_zoom_i > 0) {
                        s_book_zoom_i--;
                        s_book_pan_x[0] = s_book_pan_y[0] = 0.f;
                        s_book_pan_x[1] = s_book_pan_y[1] = 0.f;
                    }
                    {
                        int sel_before = s_selected;
                        if (kd & KEY_DUP && s_selected > 0)
                            s_selected--;
                        if (kd & KEY_DDOWN && s_selected + 1 < s_series_count)
                            s_selected++;
                        if (s_selected != sel_before) {
                            s_book_pan_x[0] = s_book_pan_y[0] = 0.f;
                            s_book_pan_x[1] = s_book_pan_y[1] = 0.f;
                        }
                    }

                    if (zz > 1.0005f) {
                        circlePosition cir;
                        hidCircleRead(&cir);
                        float dx = (float)cir.dx;
                        if (dx < -14.f || dx > 14.f) {
                            float adj = dx * 0.2f;
                            s_book_pan_y[0] += adj;
                            s_book_pan_y[1] += adj;
                        }
                        float dy = (float)cir.dy;
                        if (dy < -14.f || dy > 14.f) {
                            float adj = dy * 0.2f;
                            s_book_pan_x[0] += adj;
                            s_book_pan_x[1] += adj;
                        }
                    }
                } else if (s_view_mode == SERIES_VIEW_ROW) {
                    if (kd & KEY_RIGHT && s_selected + 1 < s_series_count)
                        s_selected++;
                    if (kd & KEY_LEFT && s_selected > 0)
                        s_selected--;
                } else {
                    if (kd & KEY_RIGHT) {
                        if (s_selected % GRID_COLS < GRID_COLS - 1 &&
                            s_selected + 1 < s_series_count) {
                            s_selected++;
                        }
                    }
                    if (kd & KEY_LEFT) {
                        if (s_selected % GRID_COLS > 0)
                            s_selected--;
                    }
                    if (kd & KEY_DOWN) {
                        int next = s_selected + GRID_COLS;
                        if (next < s_series_count)
                            s_selected = next;
                    }
                    if (kd & KEY_UP) {
                        int prev = s_selected - GRID_COLS;
                        if (prev >= 0)
                            s_selected = prev;
                    }
                }

                series_sync_scroll();

                if (hidKeysDown() & KEY_TOUCH) {
                    touchPosition tp;
                    hidTouchRead(&tp);
                    (void)tp;
                }
            }

            if (s_view_mode == SERIES_VIEW_BOOK) {
                if (s_book_show_hint && s_book_hint_frames > 0)
                    s_book_hint_frames--;
                else
                    s_book_show_hint = false;

                float zz = series_book_zoom();
                const LoadedTexture* t0 = series_pool_tex(s_selected);
                if (t0 && t0->valid) {
                    series_clamp_pan_x(SER_TOP_LOG_W, SER_TOP_LOG_H, t0, zz,
                                       &s_book_pan_x[0]);
                    series_clamp_pan_y(SER_TOP_LOG_W, SER_TOP_LOG_H, t0, zz,
                                       &s_book_pan_y[0]);
                }
                if (s_selected + 1 < s_series_count) {
                    const LoadedTexture* t1 = series_pool_tex(s_selected + 1);
                    if (t1 && t1->valid) {
                        series_clamp_pan_x(SER_BOT_LOG_W, SER_BOT_LOG_H, t1, zz,
                                           &s_book_pan_x[1]);
                        series_clamp_pan_y(SER_BOT_LOG_W, SER_BOT_LOG_H, t1, zz,
                                           &s_book_pan_y[1]);
                    }
                }
            }
            break;

        case STATE_ERROR:
            if (hidKeysDown() & KEY_B) {
                app_transition(SCREEN_LIBRARIES);
                return;
            }
            break;
    }

    /* --- Render: Top screen --- */
    C2D_SceneBegin(g_target_top);

    const bool row_immersive =
        (s_state == STATE_READY && s_view_mode == SERIES_VIEW_ROW);
    const bool book_top =
        (s_state == STATE_READY && s_view_mode == SERIES_VIEW_BOOK);

    if (row_immersive) {
        C2D_DrawRectSolid(0, 0, Z_ROW_BG, 400, 240, COL_BG_TOP);
    } else if (!book_top) {
        C2D_DrawRectSolid(0, 0, 0.5f, 400, 24, COL_PANEL);
        ui_text_truncated(g_target_top, g_app.selected_library_name,
                           8, 4, 320, FONT_MED, COL_WHITE);

        if (s_total_count > 0 && s_state != STATE_LOADING_LIST) {
            char pos[40];
            snprintf(pos, sizeof(pos), "%d / %d", s_selected + 1, s_total_count);
            C2D_Text t;
            C2D_TextParse(&t, g_text_buf, pos);
            C2D_TextOptimize(&t);
            float tw2, th2;
            C2D_TextGetDimensions(&t, FONT_SMALL, FONT_SMALL, &tw2, &th2);
            C2D_DrawText(&t, C2D_WithColor, 392 - tw2, 4, 0.5f,
                         FONT_SMALL, FONT_SMALL, COL_GREY);
        }
    }

    if (s_state == STATE_LOADING_LIST) {
        ui_spinner_tick(&s_spinner);
        ui_spinner(g_target_top, &s_spinner, 200, 130, 24, COL_ACCENT);
        ui_text_centered(g_target_top, "Loading series...", 0, 400, 160,
                         FONT_MED, COL_GREY);
    } else if (s_state == STATE_ERROR) {
        ui_text_centered(g_target_top, s_error_msg, 0, 400, 110,
                         FONT_MED, COL_ERROR);
    } else if (s_view_mode == SERIES_VIEW_BOOK) {
        C3D_SetScissor(GPU_SCISSOR_NORMAL, 0, 0,
                       (u32)SER_TOP_LOG_W, (u32)SER_TOP_LOG_H);
        {
            C3D_Mtx save;
            C2D_ViewSave(&save);
            series_book_apply_transform(SER_TOP_PHYS_W, SER_TOP_PHYS_H);

            C2D_DrawRectSolid(0, 0, Z_BOOK_PAGE, SER_TOP_LOG_W, SER_TOP_LOG_H,
                              COL_BG_TOP);

            const LoadedTexture* tex = series_pool_tex(s_selected);
            if (tex && tex->valid) {
                series_draw_portrait_cover_zoom(
                    SER_TOP_LOG_W, SER_TOP_LOG_H, tex,
                    s_book_pan_x[0], s_book_pan_y[0], series_book_zoom());
            } else {
                C2D_DrawRectSolid(0, 0, Z_BOOK_PAGE, SER_TOP_LOG_W,
                                   SER_TOP_LOG_H, COL_ACCENT2);
                if (s_cover_fetch_idx == s_selected && !s_cover_thread_done) {
                    series_spinner_draw(SER_TOP_LOG_W * 0.5f,
                                        SER_TOP_LOG_H * 0.5f, 28.f, COL_ACCENT);
                }
            }

            series_book_draw_selection_frame(SER_TOP_LOG_W, SER_TOP_LOG_H);

            if (s_total_count > 0) {
                char pg[40];
                snprintf(pg, sizeof(pg), "%d/%d",
                         s_selected + 1, s_total_count);
                C2D_Text t;
                C2D_TextBufClear(g_text_buf);
                C2D_TextParse(&t, g_text_buf, pg);
                C2D_TextOptimize(&t);
                float twb, thb;
                C2D_TextGetDimensions(&t, FONT_TINY, FONT_TINY, &twb, &thb);
                float bg_w = twb + ROW_HUD_PAD_X * 2.f;
                float bg_h = thb + ROW_HUD_PAD_Y * 2.f;
                float bg_x = SER_TOP_LOG_W - 4.f - bg_w;
                float bg_y = 4.f;
                C2D_DrawRectSolid(bg_x, bg_y, Z_BOOK_HUD_BG, bg_w, bg_h,
                                  C2D_Color32(0, 0, 0, ROW_HUD_ALPHA));
                C2D_DrawText(&t, C2D_WithColor,
                             bg_x + ROW_HUD_PAD_X, bg_y + ROW_HUD_PAD_Y,
                             Z_BOOK_HUD_FG, FONT_TINY, FONT_TINY, COL_WHITE);
                if (series_book_zoom() > 1.001f) {
                    char zb[16];
                    snprintf(zb, sizeof(zb), "%.2gx", series_book_zoom());
                    C2D_TextBufClear(g_text_buf);
                    C2D_TextParse(&t, g_text_buf, zb);
                    C2D_TextOptimize(&t);
                    C2D_DrawText(&t, C2D_WithColor,
                                 bg_x + ROW_HUD_PAD_X,
                                 bg_y + ROW_HUD_PAD_Y + thb + 2.f,
                                 Z_BOOK_HUD_FG, FONT_TINY, FONT_TINY, COL_GREY);
                }
            }

            C2D_ViewRestore(&save);
        }
        C3D_SetScissor(GPU_SCISSOR_NORMAL, 0, 0, 400, 240);
    } else if (s_view_mode == SERIES_VIEW_ROW) {
        int nvis = row_slots_on_screen();
        int row_cy = (240 - ROW_THUMB_H) / 2;
        if (row_cy < 0)
            row_cy = 0;

        float row_w =
            (float)nvis * ROW_THUMB_W + (float)(nvis - 1) * ROW_PAD_X;
        float base_cx = (400.0f - row_w) * 0.5f;
        float step    = (float)(ROW_THUMB_W + ROW_PAD_X);

        for (int j = 0; j < nvis; j++) {
            int slot = s_scroll_first + j;
            if (slot >= s_series_count)
                break;

            float cx = base_cx + j * step;
            float cy = (float)row_cy;

            if (slot == s_selected) {
                C2D_DrawRectSolid(cx - 2, cy - 2, Z_ROW_SELECTION,
                                   ROW_THUMB_W + 4, ROW_THUMB_H + 4,
                                   COL_ACCENT);
            }

            int pi = s_series_to_pool[slot];
            if (pi >= 0 && pi < COVER_POOL_SIZE && s_cover_valid_pool[pi]) {
                float sx, sy, ox, oy;
                image_fit_dims(s_cover_pool[pi].src_width,
                               s_cover_pool[pi].src_height,
                               ROW_THUMB_W, ROW_THUMB_H,
                               &sx, &sy, &ox, &oy);
                C2D_DrawImageAt(s_cover_pool[pi].image,
                                cx + ox, cy + oy, Z_ROW_IMAGE, NULL, sx, sy);
            } else {
                C2D_DrawRectSolid(cx, cy, Z_ROW_IMAGE, ROW_THUMB_W, ROW_THUMB_H,
                                   COL_ACCENT2);
                if (s_cover_fetch_idx == slot && !s_cover_thread_done) {
                    ui_spinner_tick(&s_spinner);
                    ui_spinner(g_target_top, &s_spinner,
                               cx + ROW_THUMB_W / 2, cy + ROW_THUMB_H / 2, 10,
                               COL_GREY);
                }
            }
        }

        if (s_total_count > 0) {
            char pos[40];
            snprintf(pos, sizeof(pos), "%d/%d", s_selected + 1, s_total_count);
            C2D_Text t;
            C2D_TextBufClear(g_text_buf);
            C2D_TextParse(&t, g_text_buf, pos);
            C2D_TextOptimize(&t);
            float twb, thb;
            C2D_TextGetDimensions(&t, FONT_TINY, FONT_TINY, &twb, &thb);
            float bg_w = twb + ROW_HUD_PAD_X * 2.f;
            float bg_h = thb + ROW_HUD_PAD_Y * 2.f;
            float bg_x = 400.f - 4.f - bg_w;
            float bg_y = 4.f;
            C2D_DrawRectSolid(bg_x, bg_y, Z_ROW_HUD_BG, bg_w, bg_h,
                              C2D_Color32(0, 0, 0, ROW_HUD_ALPHA));
            C2D_DrawText(&t, C2D_WithColor,
                         bg_x + ROW_HUD_PAD_X, bg_y + ROW_HUD_PAD_Y,
                         Z_ROW_HUD_FG,
                         FONT_TINY, FONT_TINY, COL_WHITE);
        }
    } else {
        for (int vr = 0; vr < GRID_VISIBLE_ROWS; vr++) {
            for (int col = 0; col < GRID_COLS; col++) {
                int slot = (s_scroll_row + vr) * GRID_COLS + col;
                if (slot >= s_series_count) break;

                float cx = THUMB_OFFSET_X + col * (THUMB_W + THUMB_PAD_X);
                float cy = THUMB_OFFSET_Y + vr * (THUMB_H + THUMB_PAD_Y + 14);

                if (slot == s_selected) {
                    C2D_DrawRectSolid(cx - 2, cy - 2, 0.4f,
                                       THUMB_W + 4, THUMB_H + 4,
                                       COL_ACCENT);
                }

                int pi = s_series_to_pool[slot];
                if (pi >= 0 && pi < COVER_POOL_SIZE && s_cover_valid_pool[pi]) {
                    float sx, sy, ox, oy;
                    image_fit_dims(s_cover_pool[pi].src_width,
                                   s_cover_pool[pi].src_height,
                                   THUMB_W, THUMB_H,
                                   &sx, &sy, &ox, &oy);
                    C2D_DrawImageAt(s_cover_pool[pi].image,
                                    cx + ox, cy + oy, 0.5f, NULL, sx, sy);
                } else {
                    C2D_DrawRectSolid(cx, cy, 0.5f, THUMB_W, THUMB_H,
                                       COL_ACCENT2);
                    if (s_cover_fetch_idx == slot && !s_cover_thread_done) {
                        ui_spinner_tick(&s_spinner);
                        ui_spinner(g_target_top, &s_spinner,
                                   cx + THUMB_W / 2, cy + THUMB_H / 2, 8,
                                   COL_GREY);
                    }
                }

                ui_text_truncated(g_target_top, s_series[slot].name,
                                   cx, cy + THUMB_H + 2, THUMB_W,
                                   FONT_TINY, COL_WHITE);
            }
        }
    }

    /* --- Render: Bottom screen --- */
    C2D_SceneBegin(g_target_bottom);

    if (s_state == STATE_READY && s_view_mode == SERIES_VIEW_BOOK) {
        C3D_SetScissor(GPU_SCISSOR_NORMAL, 0, 0,
                       (u32)SER_BOT_LOG_W, (u32)SER_BOT_LOG_H);
        {
            C3D_Mtx save;
            C2D_ViewSave(&save);
            series_book_apply_transform(SER_BOT_PHYS_W, SER_BOT_PHYS_H);

            C2D_DrawRectSolid(0, 0, Z_BOOK_PAGE, SER_BOT_LOG_W, SER_BOT_LOG_H,
                              COL_BG_TOP);

            if (s_selected + 1 < s_series_count) {
                int idx1 = s_selected + 1;
                const LoadedTexture* tex = series_pool_tex(idx1);
                if (tex && tex->valid) {
                    series_draw_portrait_cover_zoom(
                        SER_BOT_LOG_W, SER_BOT_LOG_H, tex,
                        s_book_pan_x[1], s_book_pan_y[1], series_book_zoom());
                } else {
                    C2D_DrawRectSolid(0, 0, Z_BOOK_PAGE, SER_BOT_LOG_W,
                                       SER_BOT_LOG_H, COL_ACCENT2);
                    if (s_cover_fetch_idx == idx1 && !s_cover_thread_done) {
                        series_spinner_draw(SER_BOT_LOG_W * 0.5f,
                                            SER_BOT_LOG_H * 0.5f, 24.f,
                                            COL_ACCENT);
                    }
                }

                if (s_total_count > 0) {
                    char pg2[40];
                    snprintf(pg2, sizeof(pg2), "%d/%d",
                             s_selected + 2, s_total_count);
                    C2D_Text t;
                    C2D_TextBufClear(g_text_buf);
                    C2D_TextParse(&t, g_text_buf, pg2);
                    C2D_TextOptimize(&t);
                    float twb, thb;
                    C2D_TextGetDimensions(&t, FONT_TINY, FONT_TINY, &twb, &thb);
                    float bg_w = twb + ROW_HUD_PAD_X * 2.f;
                    float bg_h = thb + ROW_HUD_PAD_Y * 2.f;
                    float bg_x = SER_BOT_LOG_W - 4.f - bg_w;
                    float bg_y = 4.f;
                    C2D_DrawRectSolid(bg_x, bg_y, Z_BOOK_HUD_BG, bg_w, bg_h,
                                      C2D_Color32(0, 0, 0, ROW_HUD_ALPHA));
                    C2D_DrawText(&t, C2D_WithColor,
                                 bg_x + ROW_HUD_PAD_X, bg_y + ROW_HUD_PAD_Y,
                                 Z_BOOK_HUD_FG, FONT_TINY, FONT_TINY,
                                 COL_WHITE);
                    if (series_book_zoom() > 1.001f) {
                        char zb[16];
                        snprintf(zb, sizeof(zb), "%.2gx", series_book_zoom());
                        C2D_TextBufClear(g_text_buf);
                        C2D_TextParse(&t, g_text_buf, zb);
                        C2D_TextOptimize(&t);
                        C2D_DrawText(&t, C2D_WithColor,
                                     bg_x + ROW_HUD_PAD_X,
                                     bg_y + ROW_HUD_PAD_Y + thb + 2.f,
                                     Z_BOOK_HUD_FG, FONT_TINY, FONT_TINY,
                                     COL_GREY);
                    }
                }
            } else {
                C2D_DrawRectSolid(0, 0, Z_BOOK_PAGE, SER_BOT_LOG_W,
                                   SER_BOT_LOG_H, COL_DARK);
                series_text_centered_z("(end)", SER_BOT_LOG_W,
                                        SER_BOT_LOG_H * 0.5f - 10.f,
                                        FONT_SMALL, COL_GREY, Z_BOOK_PAGE);
            }

            if (s_book_show_hint) {
                const float hint_top  = SER_BOT_LOG_H - 92.f;
                const float hint_h    = 86.f;
                const float line_y0   = SER_BOT_LOG_H - 84.f;
                const float line_step = 15.f;
                C2D_DrawRectSolid(8, hint_top, Z_BOOK_HINT_BG,
                                   SER_BOT_LOG_W - 16, hint_h,
                                   C2D_Color32(0, 0, 0, 200));
                series_text_centered_z("D-Pad L/R: Zoom",
                                        SER_BOT_LOG_W, line_y0, FONT_TINY,
                                        COL_WHITE, Z_BOOK_HUD_FG);
                series_text_centered_z("D-Pad Up/Down: Previous / Next",
                                        SER_BOT_LOG_W, line_y0 + line_step,
                                        FONT_TINY, COL_GREY, Z_BOOK_HUD_FG);
                series_text_centered_z("Circle Pad: Pan when zoomed",
                                        SER_BOT_LOG_W,
                                        line_y0 + 2.f * line_step, FONT_TINY,
                                        COL_GREY, Z_BOOK_HUD_FG);
                series_text_centered_z("A: Open  B: Back  Y: View  X: Help",
                                        SER_BOT_LOG_W,
                                        line_y0 + 3.f * line_step, FONT_TINY,
                                        COL_GREY, Z_BOOK_HUD_FG);
            }

            C2D_ViewRestore(&save);
        }
        C3D_SetScissor(GPU_SCISSOR_NORMAL, 0, 0, 320, 240);
    } else {
        C2D_DrawRectSolid(0, 0, 0.5f, 320, 240, COL_BG_BOTTOM);

        if (s_state == STATE_ERROR) {
            ui_text_centered(g_target_bottom, s_error_msg,
                              0, 320, 80, FONT_MED, COL_ERROR);
            ui_text_centered(g_target_bottom, "B: Back",
                              0, 320, 120, FONT_MED, COL_WHITE);
        } else if (s_state == STATE_LOADING_LIST) {
            ui_text_centered(g_target_bottom, "Loading series...",
                              0, 320, 110, FONT_MED, COL_GREY);
            ui_text_centered(g_target_bottom, "B: Back",
                              0, 320, 140, FONT_MED, COL_WHITE);
        } else if (s_series_count > 0 && s_total_count > 0) {
            if (s_selected >= 0 && s_selected < s_series_count) {
                ui_text_centered(g_target_bottom,
                                  s_series[s_selected].name,
                                  0, 320, 10, FONT_MED, COL_ACCENT);

                int pr = s_series[s_selected].pages_read;
                int pt = s_series[s_selected].pages_total;
                if (pt > 0) {
                    char prog_str[64];
                    snprintf(prog_str, sizeof(prog_str), "%d / %d pages", pr, pt);
                    ui_text_centered(g_target_bottom, prog_str,
                                      0, 320, 32, FONT_SMALL, COL_GREY);
                    ui_progress_bar(g_target_bottom, 20, 50, 280, 10,
                                     (float)pr / pt, COL_ACCENT, COL_ACCENT2);
                }
            }

            if (s_view_mode == SERIES_VIEW_ROW) {
                ui_text_centered(g_target_bottom,
                                  "Left/Right: Browse (scrolls)",
                                  0, 320, 150, FONT_SMALL, COL_GREY);
            } else {
                ui_text_centered(g_target_bottom, "D-Pad: Navigate (scrolls)",
                                  0, 320, 150, FONT_SMALL, COL_GREY);
            }
            ui_text_centered(g_target_bottom,
                              "A: Open  B: Back  Y: View",
                              0, 320, 168, FONT_SMALL, COL_GREY);
        } else if (s_state == STATE_READY) {
            ui_text_centered(g_target_bottom, "No series found",
                              0, 320, 110, FONT_MED, COL_GREY);
            ui_text_centered(g_target_bottom, "B: Back",
                              0, 320, 140, FONT_MED, COL_WHITE);
        }
    }
}
