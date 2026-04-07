#include "screen_settings.h"
#include "app.h"
#include "ui.h"

#include <3ds.h>
#include <stdio.h>

enum {
    SET_ROW_COVER = 0,
    SET_ROW_READER_CACHE = 1,
    SET_ROW_CACHE_PAGES = 2,
    SET_ROW_COUNT = 3,
};

static int s_sel;

void screen_settings_init(void) {
    s_sel = 0;
}

void screen_settings_fini(void) {
}

void screen_settings_tick(void) {
    u32 kd = hidKeysDown();

    if (kd & KEY_DUP && s_sel > 0)
        s_sel--;
    if (kd & KEY_DDOWN && s_sel < SET_ROW_COUNT - 1)
        s_sel++;

    if (kd & KEY_A) {
        if (s_sel == SET_ROW_COVER)
            g_app.cover_cache = !g_app.cover_cache;
        else if (s_sel == SET_ROW_READER_CACHE)
            g_app.reader_page_cache = !g_app.reader_page_cache;
    }

    if (s_sel == SET_ROW_CACHE_PAGES) {
        int n = g_app.reader_cache_pages > 0 ? g_app.reader_cache_pages : 10;
        if (kd & KEY_L) {
            if (n > 1) n--;
            g_app.reader_cache_pages = n;
        }
        if (kd & KEY_R) {
            if (n < 50) n++;
            g_app.reader_cache_pages = n;
        }
    }

    if (kd & KEY_B) {
        app_save_config();
        app_transition(SCREEN_LIBRARIES);
        return;
    }

    C2D_SceneBegin(g_target_top);
    C2D_DrawRectSolid(0, 0, 0.5f, 400, 24, COL_PANEL);
    ui_text(g_target_top, "Settings", 8, 4, FONT_MED, COL_WHITE);

    ui_text(g_target_top, "Cover cache (SD)", 16, 40, FONT_MED, COL_WHITE);
    ui_text(g_target_top, g_app.cover_cache ? "On" : "Off",
            220, 40, FONT_MED, g_app.cover_cache ? COL_ACCENT : COL_GREY);
    if (s_sel == SET_ROW_COVER)
        ui_text(g_target_top, ">", 4, 40, FONT_MED, COL_ACCENT);

    ui_text(g_target_top, "Reader page cache (SD)", 16, 68, FONT_MED, COL_WHITE);
    ui_text(g_target_top, g_app.reader_page_cache ? "On" : "Off",
            220, 68, FONT_MED, g_app.reader_page_cache ? COL_ACCENT : COL_GREY);
    if (s_sel == SET_ROW_READER_CACHE)
        ui_text(g_target_top, ">", 4, 68, FONT_MED, COL_ACCENT);

    {
        int n = g_app.reader_cache_pages > 0 ? g_app.reader_cache_pages : 10;
        char buf[48];
        snprintf(buf, sizeof(buf), "%d", n);
        ui_text(g_target_top, "Pages to prefetch ahead", 16, 96, FONT_MED, COL_WHITE);
        ui_text(g_target_top, buf, 220, 96, FONT_MED, COL_WHITE);
        if (s_sel == SET_ROW_CACHE_PAGES)
            ui_text(g_target_top, ">", 4, 96, FONT_MED, COL_ACCENT);
    }

    ui_text(g_target_top,
            "Caches series / volume / chapter covers on the SD card",
            16, 128, FONT_SMALL, COL_GREY);
    ui_text(g_target_top,
            "so they load faster on repeat visits.",
            16, 146, FONT_SMALL, COL_GREY);
    ui_text(g_target_top,
            "Per-server folder under 3ds/kavita-3ds/covers/",
            16, 164, FONT_SMALL, COL_GREY);

    ui_text(g_target_top,
            "Reader cache: next N image pages after the current",
            16, 188, FONT_SMALL, COL_GREY);
    ui_text(g_target_top,
            "page are saved under 3ds/kavita-3ds/pages/ (cleared",
            16, 206, FONT_SMALL, COL_GREY);
    ui_text(g_target_top,
            "when you leave the reader or exit the app).",
            16, 224, FONT_SMALL, COL_GREY);

    C2D_SceneBegin(g_target_bottom);
    C2D_DrawRectSolid(0, 0, 0.5f, 320, 240, COL_BG_BOTTOM);

    ui_text_centered(g_target_bottom, "Up/Down: Select row",
                      0, 320, 72, FONT_MED, COL_WHITE);
    ui_text_centered(g_target_bottom, "A: Toggle (cover / reader cache)",
                      0, 320, 96, FONT_MED, COL_GREY);
    ui_text_centered(g_target_bottom, "L/R: Pages ahead (1-50)",
                      0, 320, 120, FONT_MED, COL_GREY);
    ui_text_centered(g_target_bottom, "B: Back (save)",
                      0, 320, 148, FONT_MED, COL_GREY);
    ui_text_centered(g_target_bottom, "SELECT+START to exit",
                      0, 320, 228, FONT_TINY, COL_GREY);
}
