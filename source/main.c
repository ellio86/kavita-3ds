#include <3ds.h>
#include <citro2d.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "app.h"
#include "debug_log.h"

/* SOC buffer - must be page-aligned for socInit */
static u32 soc_buf[0x8000] __attribute__((aligned(0x1000)));

/* Shared text buffer - cleared each frame, used by all screens */
C2D_TextBuf g_text_buf;

/* Render targets */
C3D_RenderTarget* g_target_top;
C3D_RenderTarget* g_target_bottom;

int main(int argc, char* argv[]) {
    (void)argc;
    (void)argv;

    /* --- Core service init --- */
    gfxInitDefault();
    cfguInit();

    /* Enable wide-mode on top screen (400px wide) */
    gfxSetWide(false);

    /* Network services */
    acInit();
    Result soc_res = socInit(soc_buf, sizeof(soc_buf));
    if (R_FAILED(soc_res)) {
        /* On Citra socInit may fail — continue anyway as httpc may still work */
    }
    httpcInit(0);
    romfsInit();

    /* --- citro2d/citro3d init --- */
    C3D_Init(C3D_DEFAULT_CMDBUF_SIZE);
    /* Extra headroom: EPUB text + HUD + lists can push many quads per frame. */
    C2D_Init(C2D_DEFAULT_MAX_OBJECTS * 2);
    C2D_Prepare();

    /* Create render targets for both screens */
    g_target_top    = C2D_CreateScreenTarget(GFX_TOP, GFX_LEFT);
    g_target_bottom = C2D_CreateScreenTarget(GFX_BOTTOM, GFX_LEFT);

    /* Shared text buffer (4096 glyphs per frame) */
    g_text_buf = C2D_TextBufNew(4096);

    /* --- Debug logger (init before app so first log lines are captured) --- */
    dlog_init();

    /* --- App init (loads config, sets initial screen) --- */
    app_init();

    /* --- Main loop --- */
    while (aptMainLoop()) {
        hidScanInput();

        u32 keys_down = hidKeysDown();

        /* Global exit: SELECT + START */
        if ((keys_down & KEY_SELECT) && (keys_down & KEY_START)) {
            break;
        }

        /* Begin frame */
        C3D_FrameBegin(C3D_FRAME_SYNCDRAW);

        C2D_TargetClear(g_target_top,    C2D_Color32(0x1a, 0x1a, 0x2e, 0xff));
        C2D_TargetClear(g_target_bottom, C2D_Color32(0x16, 0x13, 0x2e, 0xff));
        C2D_TextBufClear(g_text_buf);

        /* Dispatch to active screen (handles input + rendering) */
        app_tick();

        /* Debug overlay: drawn on top if SELECT is held */
        dlog_overlay_tick();

        C3D_FrameEnd(0);
    }

    /* --- Cleanup --- */
    app_fini();
    dlog_fini();
    C2D_TextBufDelete(g_text_buf);
    C2D_Fini();
    C3D_Fini();
    httpcExit();
    socExit();
    acExit();
    romfsExit();
    cfguExit();
    gfxExit();

    return 0;
}
