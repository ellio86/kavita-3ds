#include "screen_libraries.h"
#include "app.h"
#include "kavita_api.h"
#include "ui.h"

#include <3ds.h>
#include <string.h>
#include <stdio.h>

#define MAX_LIBRARIES 32

/* ------------------------------------------------------------------ */
/* Screen state                                                         */
/* ------------------------------------------------------------------ */
static KavitaLibrary s_libs[MAX_LIBRARIES];
static int           s_lib_count;
static int           s_selected;
static int           s_scroll;

static bool          s_loading;
static bool          s_error;
static char          s_error_msg[128];
static UiSpinner     s_spinner;

/* Background fetch thread */
static Thread        s_thread;
static volatile bool s_thread_done;

/* ------------------------------------------------------------------ */
/* Background thread                                                    */
/* ------------------------------------------------------------------ */
static void fetch_thread(void* arg) {
    (void)arg;
    s_lib_count = kavita_get_libraries(
        g_app.base_url, g_app.token, s_libs, MAX_LIBRARIES);
    s_thread_done = true;
    threadExit(0);
}

/* ------------------------------------------------------------------ */
/* Lifecycle                                                            */
/* ------------------------------------------------------------------ */
void screen_libraries_init(void) {
    s_lib_count   = 0;
    s_selected    = 0;
    s_scroll      = 0;
    s_loading     = true;
    s_error       = false;
    s_thread_done = false;
    s_spinner.angle = 0.0f;

    s_thread = threadCreate(fetch_thread, NULL, 32 * 1024, 0x30, 1, false);
}

void screen_libraries_fini(void) {
    if (s_thread) {
        threadJoin(s_thread, U64_MAX);
        threadFree(s_thread);
        s_thread = NULL;
    }
}

/* ------------------------------------------------------------------ */
/* Tick                                                                 */
/* ------------------------------------------------------------------ */
void screen_libraries_tick(void) {
    /* Check thread completion */
    if (s_loading && s_thread_done) {
        threadJoin(s_thread, U64_MAX);
        threadFree(s_thread);
        s_thread  = NULL;
        s_loading = false;

        if (s_lib_count < 0) {
            s_error = true;
            snprintf(s_error_msg, sizeof(s_error_msg),
                     "Failed to load libraries");
            s_lib_count = 0;
        }
    }

    if (s_loading) ui_spinner_tick(&s_spinner);

    /* --- Input --- */
    if (!s_loading && !s_error) {
        u32 kd = hidKeysDown();

        if ((kd & KEY_A) && s_lib_count > 0) {
            g_app.selected_library_id = s_libs[s_selected].id;
            strncpy(g_app.selected_library_name, s_libs[s_selected].name,
                    sizeof(g_app.selected_library_name) - 1);
            app_transition(SCREEN_SERIES);
            return;
        }

        if (kd & KEY_X) {
            app_transition(SCREEN_SETTINGS);
            return;
        }

        /* Logout: go back to setup */
        if (kd & KEY_SELECT) {
            memset(g_app.token,   0, sizeof(g_app.token));
            memset(g_app.api_key, 0, sizeof(g_app.api_key));
            app_transition(SCREEN_SETUP);
            return;
        }
    }

    /* Retry */
    if (s_error && (hidKeysDown() & KEY_A)) {
        screen_libraries_fini();
        screen_libraries_init();
        return;
    }

    /* --- Render: Top screen (library list) --- */
    C2D_SceneBegin(g_target_top);

    /* Header bar */
    C2D_DrawRectSolid(0, 0, 0.5f, 400, 24, COL_PANEL);
    ui_text(g_target_top, "Libraries", 8, 4, FONT_MED, COL_WHITE);

    if (!s_error && s_lib_count > 0) {
        char count_str[32];
        snprintf(count_str, sizeof(count_str), "%d", s_lib_count);
        C2D_Text ct;
        C2D_TextParse(&ct, g_text_buf, count_str);
        C2D_TextOptimize(&ct);
        float tw, th;
        C2D_TextGetDimensions(&ct, FONT_SMALL, FONT_SMALL, &tw, &th);
        C2D_DrawText(&ct, C2D_WithColor, 392 - tw, 4, 0.5f,
                     FONT_SMALL, FONT_SMALL, COL_GREY);
    }

    if (s_loading) {
        ui_spinner(g_target_top, &s_spinner, 200, 130, 24, COL_ACCENT);
        ui_text_centered(g_target_top, "Loading...", 0, 400, 160, FONT_MED, COL_GREY);
    } else if (s_error) {
        ui_text_centered(g_target_top, s_error_msg, 0, 400, 110, FONT_MED, COL_ERROR);
    } else {
        /* Type badge colours: raw RGBA u32 (r | g<<8 | b<<16 | a<<24) */
        static const u32 type_colors[3] = {
            0xffff8844u, /* Book   - blue   C2D_Color32(0x44,0x88,0xff,0xff) */
            0xff2288ffu, /* Comic  - orange C2D_Color32(0xff,0x88,0x22,0xff) */
            0xff8844ffu, /* Manga  - pink   C2D_Color32(0xff,0x44,0x88,0xff) */
        };
        static const char* type_labels[3] = { "B", "C", "M" };

        int visible = 8;
        for (int i = 0; i < visible && (s_scroll + i) < s_lib_count; i++) {
            int idx = s_scroll + i;
            float iy = 28 + i * 26.0f;

            if (idx == s_selected)
                C2D_DrawRectSolid(0, iy, 0.5f, 400, 24, COL_HIGHLIGHT);

            /* Type badge */
            int t = s_libs[idx].type;
            if (t < 0 || t > 2) t = 0;
            C2D_DrawRectSolid(4, iy + 2, 0.6f, 20, 20, type_colors[t]);
            ui_text_centered(g_target_top, type_labels[t], 4, 20, iy + 4,
                             FONT_TINY, COL_WHITE);

            ui_text(g_target_top, s_libs[idx].name, 30, iy + 4, FONT_MED, COL_WHITE);
        }
    }

    /* --- Render: Bottom screen (nav hints) --- */
    C2D_SceneBegin(g_target_bottom);
    C2D_DrawRectSolid(0, 0, 0.5f, 320, 240, COL_BG_BOTTOM);

    if (s_error) {
        ui_text_centered(g_target_bottom, "Press A to retry",
                          0, 320, 110, FONT_MED, COL_WHITE);
    } else if (!s_loading) {
        ui_text_centered(g_target_bottom, "D-Pad: Navigate",
                          0, 320, 80, FONT_SMALL, COL_GREY);
        ui_text_centered(g_target_bottom, "A: Open Library",
                          0, 320, 100, FONT_SMALL, COL_GREY);
        ui_text_centered(g_target_bottom, "X: Settings",
                          0, 320, 120, FONT_SMALL, COL_GREY);
        ui_text_centered(g_target_bottom, "SELECT: Logout",
                          0, 320, 140, FONT_SMALL, COL_GREY);

        if (s_lib_count > 0) {
            char sel_str[256];
            snprintf(sel_str, sizeof(sel_str), "%s", s_libs[s_selected].name);
            ui_text_centered(g_target_bottom, sel_str,
                              0, 320, 172, FONT_MED, COL_ACCENT);
        }
    }

    ui_text_centered(g_target_bottom, "SELECT+START to exit",
                      0, 320, 228, FONT_TINY, COL_GREY);

    /* D-pad nav (also updates scroll) */
    if (!s_loading && !s_error) {
        u32 kd = hidKeysDown();
        if (kd & KEY_DOWN) {
            if (s_selected < s_lib_count - 1) {
                s_selected++;
                if (s_selected >= s_scroll + 8) s_scroll++;
            }
        }
        if (kd & KEY_UP) {
            if (s_selected > 0) {
                s_selected--;
                if (s_selected < s_scroll) s_scroll--;
            }
        }
    }
}
