#include "screen_detail.h"
#include "app.h"
#include "kavita_api.h"
#include "image_loader.h"
#include "http_client.h"
#include "ui.h"

#include <3ds.h>
#include <string.h>
#include <stdio.h>

/* ------------------------------------------------------------------ */
/* State                                                                */
/* ------------------------------------------------------------------ */
static KavitaSeriesDetail s_detail;
static bool               s_loading;
static bool               s_error;
static char               s_error_msg[128];
static UiSpinner          s_spinner;

/* Cover image */
static LoadedTexture      s_cover;
static bool               s_cover_loading;
static PreparedTexture    s_cover_prep;
static LightEvent         s_cover_event;
static volatile bool      s_cover_ready;

/* List navigation: volumes first when series has multiple volumes */
static bool               s_pick_volume;
static int                s_vol_idx;
static int                s_scroll;
static int                s_selected;

/* Background threads */
static Thread             s_detail_thread;
static volatile bool      s_detail_done;
static Thread             s_cover_thread;
static volatile bool      s_cover_thread_done;

static int detail_list_rows(void) {
    if (s_pick_volume)
        return s_detail.volume_count;
    if (s_detail.volume_count <= 1)
        return s_detail.chapter_count;
    return s_detail.volumes[s_vol_idx].chapters_in_volume;
}

static int detail_global_chapter_index(void) {
    if (s_detail.volume_count <= 1)
        return s_selected;
    return s_detail.volumes[s_vol_idx].first_chapter + s_selected;
}

/* ------------------------------------------------------------------ */
/* Background threads                                                   */
/* ------------------------------------------------------------------ */
static void detail_thread(void* arg) {
    (void)arg;
    kavita_get_series_detail(g_app.base_url, g_app.token,
                               g_app.selected_series_id, &s_detail);
    s_detail_done = true;
    threadExit(0);
}

static void cover_thread(void* arg) {
    (void)arg;
    char url[512];
    kavita_cover_url(g_app.base_url, g_app.api_key,
                      g_app.selected_series_id, url, sizeof(url));

    HttpResponse* resp = http_get(url, g_app.token);
    if (!resp || resp->status_code != 200) {
        http_response_free(resp);
        s_cover_thread_done = true;
        threadExit(0);
    }

    image_prepare_from_mem((const u8*)resp->data, resp->size, &s_cover_prep);
    http_response_free(resp);

    s_cover_ready = true;
    LightEvent_Signal(&s_cover_event);
    s_cover_thread_done = true;
    threadExit(0);
}

/* ------------------------------------------------------------------ */
/* Lifecycle                                                            */
/* ------------------------------------------------------------------ */
void screen_detail_init(void) {
    memset(&s_detail, 0, sizeof(s_detail));
    s_loading          = true;
    s_error            = false;
    s_pick_volume      = false;
    s_vol_idx          = 0;
    s_scroll           = 0;
    s_selected         = 0;
    s_detail_done      = false;
    s_cover_loading    = true;
    s_cover_ready      = false;
    s_cover_thread_done= false;
    s_cover.valid      = false;
    s_spinner.angle    = 0.0f;

    LightEvent_Init(&s_cover_event, RESET_ONESHOT);

    s_detail_thread = threadCreate(detail_thread, NULL,
                                    32 * 1024, 0x30, 1, false);
    s_cover_thread  = threadCreate(cover_thread, NULL,
                                    48 * 1024, 0x30, 1, false);
}

void screen_detail_fini(void) {
    if (s_detail_thread) {
        threadJoin(s_detail_thread, U64_MAX);
        threadFree(s_detail_thread);
        s_detail_thread = NULL;
    }
    if (s_cover_thread) {
        threadJoin(s_cover_thread, U64_MAX);
        threadFree(s_cover_thread);
        s_cover_thread = NULL;
    }
    if (s_cover_ready) image_prepared_free(&s_cover_prep);
    if (s_cover.valid)  image_texture_free(&s_cover);
}

/* ------------------------------------------------------------------ */
/* Tick                                                                 */
/* ------------------------------------------------------------------ */
void screen_detail_tick(void) {
    /* Check thread completion */
    if (s_loading && s_detail_done) {
        threadJoin(s_detail_thread, U64_MAX);
        threadFree(s_detail_thread);
        s_detail_thread = NULL;
        s_loading = false;
        if (s_detail.chapter_count == 0 && s_detail.volume_count == 0) {
            s_error = true;
            snprintf(s_error_msg, sizeof(s_error_msg), "No chapters found");
        } else {
            s_vol_idx     = 0;
            s_selected    = 0;
            s_scroll      = 0;
            s_pick_volume = (s_detail.volume_count > 1);
        }
    }

    /* Check cover ready */
    if (s_cover_loading && s_cover_ready &&
        LightEvent_TryWait(&s_cover_event) != 0) {
        image_upload_prepared(&s_cover_prep, &s_cover);
        s_cover_loading = false;
    }

    if (s_loading) ui_spinner_tick(&s_spinner);

    /* --- Input --- */
    if (!s_loading) {
        u32 kd = hidKeysDown();

        if (kd & KEY_B) {
            if (s_pick_volume || s_detail.volume_count <= 1) {
                app_transition(SCREEN_SERIES);
                return;
            }
            s_pick_volume = true;
            s_selected    = s_vol_idx;
            {
                int vis = 7;
                s_scroll = s_selected;
                if (s_detail.volume_count > vis) {
                    int max_sc = s_detail.volume_count - vis;
                    if (s_scroll > max_sc) s_scroll = max_sc;
                }
            }
            return;
        }

        if (kd & KEY_A) {
            if (s_pick_volume) {
                KavitaVolume* v = &s_detail.volumes[s_selected];
                if (v->chapters_in_volume <= 0)
                    return;
                s_vol_idx     = s_selected;
                s_pick_volume = false;
                s_selected    = 0;
                s_scroll      = 0;
                return;
            }
            int rows = detail_list_rows();
            if (rows <= 0)
                return;
            int gi     = detail_global_chapter_index();
            KavitaChapter* ch = &s_detail.chapters[gi];
            g_app.selected_chapter_id = ch->id;
            g_app.selected_volume_id  = ch->volume_id;
            g_app.reader_total_pages  = ch->pages;
            /* Resume from last read page */
            g_app.reader_page = (ch->pages_read > 0 && ch->pages_read < ch->pages)
                                 ? ch->pages_read : 0;
            app_transition(SCREEN_READER);
            return;
        }

        int rows = detail_list_rows();
        if (kd & KEY_DOWN) {
            if (rows > 0 && s_selected < rows - 1) s_selected++;
        }
        if (kd & KEY_UP) {
            if (s_selected > 0) s_selected--;
        }

        /* Scroll management */
        int visible = 7;
        if (s_selected < s_scroll) s_scroll = s_selected;
        if (rows > 0 && s_selected >= s_scroll + visible)
            s_scroll = s_selected - visible + 1;
    }

    /* --- Render: Top screen --- */
    C2D_SceneBegin(g_target_top);

    /* Header */
    C2D_DrawRectSolid(0, 0, 0.5f, 400, 24, COL_PANEL);
    ui_text_truncated(g_target_top, g_app.selected_series_name,
                       8, 4, 360, FONT_MED, COL_WHITE);

    if (s_loading) {
        ui_spinner(g_target_top, &s_spinner, 200, 130, 24, COL_ACCENT);
        ui_text_centered(g_target_top, "Loading...", 0, 400, 160, FONT_MED, COL_GREY);
    } else if (s_error) {
        ui_text_centered(g_target_top, s_error_msg, 0, 400, 120, FONT_MED, COL_ERROR);
    } else {
        /* Cover on left */
        int cover_x = 8, cover_y = 28, cover_w = 120, cover_h = 200;
        if (s_cover.valid) {
            float sx, sy, ox, oy;
            image_fit_dims(s_cover.src_width, s_cover.src_height,
                            cover_w, cover_h, &sx, &sy, &ox, &oy);
            C2D_DrawImageAt(s_cover.image,
                             cover_x + (int)ox, cover_y + (int)oy,
                             0.5f, NULL, sx, sy);
        } else {
            C2D_DrawRectSolid((float)cover_x, (float)cover_y, 0.5f,
                               (float)cover_w, (float)cover_h, COL_ACCENT2);
        }

        /* Volume / chapter context on right */
        char info[128];
        if (s_pick_volume) {
            snprintf(info, sizeof(info), "%d volumes", s_detail.volume_count);
        } else if (s_detail.volume_count > 1) {
            snprintf(info, sizeof(info), "Volume #%d  (%d ch)",
                     s_vol_idx + 1,
                     s_detail.volumes[s_vol_idx].chapters_in_volume);
        } else {
            snprintf(info, sizeof(info), "%d vol  %d chap",
                     s_detail.volume_count, s_detail.chapter_count);
        }
        ui_text(g_target_top, info, 140, 32, FONT_SMALL, COL_GREY);

        /* Volume or chapter list */
        int visible = 7;
        float list_x = 140, list_y = 50;
        float list_item_h = 26.0f;
        int list_rows = detail_list_rows();

        for (int i = 0; i < visible && (s_scroll + i) < list_rows; i++) {
            int idx = s_scroll + i;
            float iy = list_y + i * list_item_h;

            if (idx == s_selected) {
                C2D_DrawRectSolid(list_x, iy, 0.4f, 255, list_item_h - 2, COL_HIGHLIGHT);
            }

            if (s_pick_volume) {
                KavitaVolume* v = &s_detail.volumes[idx];
                char label[96];
                snprintf(label, sizeof(label), "Volume #%d  (%d)",
                         idx + 1, v->chapters_in_volume);
                ui_text_truncated(g_target_top, label,
                                   list_x + 4, iy + 2, 248, FONT_SMALL, COL_WHITE);
            } else {
                int gi;
                if (s_detail.volume_count <= 1)
                    gi = idx;
                else
                    gi = s_detail.volumes[s_vol_idx].first_chapter + idx;

                KavitaChapter* ch = &s_detail.chapters[gi];

                char label[64];
                float dnum = ch->number;
                if (dnum < -400.f || dnum > 1e6f || dnum != dnum)
                    dnum = (float)(gi + 1);
                if (ch->title[0] && dnum == 0.f) {
                    snprintf(label, sizeof(label), "%s", ch->title);
                } else if (ch->title[0]) {
                    snprintf(label, sizeof(label), "Ch.%.0f: %s", dnum, ch->title);
                } else {
                    snprintf(label, sizeof(label), "Chapter %.0f", dnum);
                }
                ui_text_truncated(g_target_top, label,
                                   list_x + 4, iy + 2, 248, FONT_SMALL, COL_WHITE);

                if (ch->pages > 0) {
                    float prog = (float)ch->pages_read / ch->pages;
                    ui_progress_bar(g_target_top,
                                     list_x + 4, iy + list_item_h - 5, 248, 3,
                                     prog, COL_ACCENT, COL_ACCENT2);
                }
            }
        }
    }

    /* --- Render: Bottom screen --- */
    C2D_SceneBegin(g_target_bottom);
    C2D_DrawRectSolid(0, 0, 0.5f, 320, 240, COL_BG_BOTTOM);

    if (!s_loading && !s_error && detail_list_rows() > 0) {
        if (s_pick_volume) {
            KavitaVolume* v = &s_detail.volumes[s_selected];
            char vol_title[48];
            snprintf(vol_title, sizeof(vol_title), "Volume #%d", s_selected + 1);
            ui_text_centered(g_target_bottom, vol_title, 0, 320, 15,
                              FONT_MED, COL_ACCENT);
            char sub[64];
            snprintf(sub, sizeof(sub), "%d chapters", v->chapters_in_volume);
            ui_text_centered(g_target_bottom, sub, 0, 320, 38,
                              FONT_SMALL, COL_GREY);
            ui_text_centered(g_target_bottom, "A: Open volume  B: Back",
                              0, 320, 160, FONT_MED, COL_WHITE);
            ui_text_centered(g_target_bottom,
                              "D-Pad toward D-Pad / away: move selection",
                              0, 320, 185, FONT_SMALL, COL_GREY);
        } else {
            int gi = detail_global_chapter_index();
            KavitaChapter* ch = &s_detail.chapters[gi];

            char title[256];
            float dnum = ch->number;
            if (dnum < -400.f || dnum > 1e6f || dnum != dnum)
                dnum = (float)(gi + 1);
            if (ch->title[0]) {
                strncpy(title, ch->title, sizeof(title) - 1);
                title[sizeof(title) - 1] = '\0';
            } else {
                snprintf(title, sizeof(title), "Chapter %.0f", dnum);
            }
            ui_text_centered(g_target_bottom, title, 0, 320, 15,
                              FONT_MED, COL_ACCENT);

            char pages_str[64];
            snprintf(pages_str, sizeof(pages_str),
                     "%d pages  (read: %d)", ch->pages, ch->pages_read);
            ui_text_centered(g_target_bottom, pages_str,
                              0, 320, 38, FONT_SMALL, COL_GREY);

            if (ch->pages > 0 && ch->pages_read > 0) {
                float prog = (float)ch->pages_read / ch->pages;
                ui_progress_bar(g_target_bottom, 20, 58, 280, 8,
                                 prog, COL_ACCENT, COL_ACCENT2);
            }

            if (s_detail.volume_count > 1) {
                ui_text_centered(g_target_bottom, "A: Read  B: Volume list",
                                  0, 320, 160, FONT_MED, COL_WHITE);
            } else {
                ui_text_centered(g_target_bottom, "A: Read  B: Back",
                                  0, 320, 160, FONT_MED, COL_WHITE);
            }
            ui_text_centered(g_target_bottom,
                              "D-Pad toward D-Pad / away: move selection",
                              0, 320, 185, FONT_SMALL, COL_GREY);
        }
    } else if (s_error) {
        ui_text_centered(g_target_bottom, "B: Back to series",
                          0, 320, 120, FONT_MED, COL_WHITE);
    }
}
