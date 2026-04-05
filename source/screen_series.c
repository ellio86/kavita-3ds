#include "screen_series.h"
#include "app.h"
#include "kavita_api.h"
#include "image_loader.h"
#include "http_client.h"
#include "ui.h"

#include <3ds.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

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

    HttpResponse* resp = http_get(url, g_app.token);
    if (!resp || resp->status_code != 200) {
        http_response_free(resp);
        s_cover_thread_done = true;
        threadExit(0);
    }

    PreparedTexture prep;
    bool ok = image_prepare_from_mem((const u8*)resp->data, resp->size, &prep);
    http_response_free(resp);

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

static void visible_cell_indices(int* out, int* out_count) {
    *out_count = 0;
    for (int vr = 0; vr < GRID_VISIBLE_ROWS; vr++) {
        for (int c = 0; c < GRID_COLS; c++) {
            int idx = (s_scroll_row + vr) * GRID_COLS + c;
            if (idx < s_series_count)
                out[(*out_count)++] = idx;
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

        int row = o / GRID_COLS, sr = s_selected / GRID_COLS;
        int col = o % GRID_COLS, sc = s_selected % GRID_COLS;
        int d   = (row > sr ? row - sr : sr - row) + (col > sc ? col - sc : sc - col);
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
        s_scroll_row = 0;
        return;
    }
    int slot = s_selected;
    if (slot < 0 || slot >= s_series_count)
        return;
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

/* ------------------------------------------------------------------ */
/* Lifecycle                                                            */
/* ------------------------------------------------------------------ */
void screen_series_init(void) {
    s_series_count = 0;
    s_total_count  = 0;
    s_selected     = 0;
    s_scroll_row   = 0;
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

                if (kd & KEY_RIGHT) {
                    if (s_selected % GRID_COLS < GRID_COLS - 1 &&
                        s_selected + 1 < s_series_count) {
                        s_selected++;
                    }
                }
                if (kd & KEY_LEFT) {
                    if (s_selected % GRID_COLS > 0) s_selected--;
                }
                if (kd & KEY_DOWN) {
                    int next = s_selected + GRID_COLS;
                    if (next < s_series_count) s_selected = next;
                }
                if (kd & KEY_UP) {
                    int prev = s_selected - GRID_COLS;
                    if (prev >= 0) s_selected = prev;
                }

                series_sync_scroll();

                if (hidKeysDown() & KEY_TOUCH) {
                    touchPosition tp;
                    hidTouchRead(&tp);
                    (void)tp;
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

    if (s_state == STATE_LOADING_LIST) {
        ui_spinner_tick(&s_spinner);
        ui_spinner(g_target_top, &s_spinner, 200, 130, 24, COL_ACCENT);
        ui_text_centered(g_target_top, "Loading series...", 0, 400, 160,
                         FONT_MED, COL_GREY);
    } else if (s_state == STATE_ERROR) {
        ui_text_centered(g_target_top, s_error_msg, 0, 400, 110,
                         FONT_MED, COL_ERROR);
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

        ui_text_centered(g_target_bottom, "D-Pad: Navigate (scrolls)",
                          0, 320, 150, FONT_SMALL, COL_GREY);
        ui_text_centered(g_target_bottom, "A: Open  B: Back",
                          0, 320, 168, FONT_SMALL, COL_GREY);
    } else if (s_state == STATE_READY) {
        ui_text_centered(g_target_bottom, "No series found",
                          0, 320, 110, FONT_MED, COL_GREY);
        ui_text_centered(g_target_bottom, "B: Back",
                          0, 320, 140, FONT_MED, COL_WHITE);
    }
}
