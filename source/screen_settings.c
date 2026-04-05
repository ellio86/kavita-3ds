#include "screen_settings.h"
#include "app.h"
#include "ui.h"

#include <3ds.h>
#include <stdio.h>

void screen_settings_init(void) {
}

void screen_settings_fini(void) {
}

void screen_settings_tick(void) {
    u32 kd = hidKeysDown();

    if (kd & KEY_A)
        g_app.cover_cache = !g_app.cover_cache;

    if (kd & KEY_B) {
        app_save_config();
        app_transition(SCREEN_LIBRARIES);
        return;
    }

    C2D_SceneBegin(g_target_top);
    C2D_DrawRectSolid(0, 0, 0.5f, 400, 24, COL_PANEL);
    ui_text(g_target_top, "Settings", 8, 4, FONT_MED, COL_WHITE);

    ui_text(g_target_top, "Cover cache (SD)", 16, 48, FONT_MED, COL_WHITE);
    ui_text(g_target_top, g_app.cover_cache ? "On" : "Off",
            220, 48, FONT_MED, g_app.cover_cache ? COL_ACCENT : COL_GREY);

    ui_text(g_target_top,
            "Caches series / volume / chapter covers on the SD card",
            16, 88, FONT_SMALL, COL_GREY);
    ui_text(g_target_top,
            "so they load faster on repeat visits.",
            16, 106, FONT_SMALL, COL_GREY);
    ui_text(g_target_top,
            "Per-server folder under 3ds/kavita-3ds/covers/",
            16, 132, FONT_SMALL, COL_GREY);

    C2D_SceneBegin(g_target_bottom);
    C2D_DrawRectSolid(0, 0, 0.5f, 320, 240, COL_BG_BOTTOM);

    ui_text_centered(g_target_bottom, "A: Toggle cover cache",
                      0, 320, 88, FONT_MED, COL_WHITE);
    ui_text_centered(g_target_bottom, "B: Back (save)",
                      0, 320, 116, FONT_MED, COL_GREY);
    ui_text_centered(g_target_bottom, "SELECT+START to exit",
                      0, 320, 228, FONT_TINY, COL_GREY);
}
