#include "debug_log.h"
#include "app.h"

#include <3ds.h>
#include <citro2d.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <sys/stat.h>
#include <sys/types.h>

/* sdmc: prefix required so paths hit the SD card after romfsInit() (newlib device) */
#define LOG_DIR  "sdmc:/3ds/kavita-3ds"
#define LOG_PATH "sdmc:/3ds/kavita-3ds/debug.log"
#define LOG_BUF_LINES  200

/* ------------------------------------------------------------------ */
/* State                                                                */
/* ------------------------------------------------------------------ */
static char  s_lines[LOG_BUF_LINES][LOG_LINE_LEN];
static int   s_head;
static int   s_count;
static FILE* s_file;
static LightLock s_lock;

/* Own text buffer — never exhausted by screen rendering */
static C2D_TextBuf s_tbuf;

/* ------------------------------------------------------------------ */
/* Init / Fini                                                          */
/* ------------------------------------------------------------------ */
void dlog_init(void) {
    LightLock_Init(&s_lock);
    s_head  = 0;
    s_count = 0;
    s_file  = NULL;

    /* Allocate dedicated text buffer for the overlay (2048 glyphs) */
    s_tbuf = C2D_TextBufNew(2048);

    /* Ensure the app directory exists before trying to open the log file */
    mkdir("sdmc:/3ds", 0777);      /* may already exist — that's fine */
    mkdir(LOG_DIR, 0777);

    s_file = fopen(LOG_PATH, "w");
    if (s_file) {
        fprintf(s_file, "=== kavita-3ds debug log ===\n");
        fflush(s_file);
        dlog("[dlog] log file: %s", LOG_PATH);
    } else {
        /* File logging unavailable, but ring buffer still works */
        dlog("[dlog] WARNING: could not open %s", LOG_PATH);
    }
    dlog("[dlog] logger ready");
}

void dlog_fini(void) {
    if (s_file) {
        fclose(s_file);
        s_file = NULL;
    }
    if (s_tbuf) {
        C2D_TextBufDelete(s_tbuf);
        s_tbuf = NULL;
    }
}

/* ------------------------------------------------------------------ */
/* Log a line                                                           */
/* ------------------------------------------------------------------ */
void dlog(const char* fmt, ...) {
    LightLock_Lock(&s_lock);

    char buf[LOG_LINE_LEN];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    buf[LOG_LINE_LEN - 1] = '\0';

    /* Ring buffer */
    memcpy(s_lines[s_head], buf, LOG_LINE_LEN);
    s_head = (s_head + 1) % LOG_BUF_LINES;
    if (s_count < LOG_BUF_LINES) s_count++;

    /* SD card file */
    if (s_file) {
        fprintf(s_file, "%s\n", buf);
        fflush(s_file);
    }

    LightLock_Unlock(&s_lock);
}

/* ------------------------------------------------------------------ */
/* Internal: draw one text line using the overlay's own text buffer    */
/* ------------------------------------------------------------------ */
static void draw_line(C3D_RenderTarget* tgt, const char* str,
                       float x, float y, float scale, u32 color) {
    if (!s_tbuf) return;
    C2D_Text t;
    C2D_SceneBegin(tgt);
    C2D_TextBufClear(s_tbuf);
    C2D_TextParse(&t, s_tbuf, str);
    C2D_TextOptimize(&t);
    C2D_DrawText(&t, C2D_WithColor, x, y, 0.9f, scale, scale, color);
}

/* ------------------------------------------------------------------ */
/* Overlay — call once per frame from main loop after app_tick()       */
/* ------------------------------------------------------------------ */
bool dlog_overlay_tick(void) {
    if (!(hidKeysHeld() & KEY_SELECT)) return false;

    LightLock_Lock(&s_lock);

    /* Semi-transparent black overlay on both screens */
    C2D_SceneBegin(g_target_top);
    C2D_DrawRectSolid(0, 0, 0.89f, 400, 240, C2D_Color32(0, 0, 0, 220));
    C2D_SceneBegin(g_target_bottom);
    C2D_DrawRectSolid(0, 0, 0.89f, 320, 240, C2D_Color32(0, 0, 0, 220));

    /* --- Top screen: log lines --- */
    draw_line(g_target_top,
              "-- DEBUG LOG (release SELECT) --",
              4, 1, 0.44f, C2D_Color32(0xff, 0xff, 0x44, 0xff));

    int lines_to_show = s_count < LOG_OVERLAY_LINES ? s_count : LOG_OVERLAY_LINES;
    /* Show the most recent lines at the bottom of the list */
    int start = (s_head - lines_to_show + LOG_BUF_LINES) % LOG_BUF_LINES;

    for (int i = 0; i < lines_to_show; i++) {
        int idx = (start + i) % LOG_BUF_LINES;
        float y = 14.0f + i * 11.0f;
        if (y > 237) break;

        u32 col = C2D_Color32(0xcc, 0xcc, 0xcc, 0xff);
        const char* line = s_lines[idx];
        if      (strstr(line, "[ERR]"))  col = C2D_Color32(0xff, 0x66, 0x66, 0xff);
        else if (strstr(line, "[OK]"))   col = C2D_Color32(0x66, 0xff, 0x88, 0xff);
        else if (strstr(line, "[HTTP]")) col = C2D_Color32(0x88, 0xcc, 0xff, 0xff);
        else if (strstr(line, "[API]"))  col = C2D_Color32(0xff, 0xcc, 0x66, 0xff);
        else if (strstr(line, "[dlog]")) col = C2D_Color32(0x88, 0x88, 0x88, 0xff);

        draw_line(g_target_top, line, 2, y, 0.40f, col);
    }

    /* --- Bottom screen: total count + hint --- */
    char hdr[128];
    snprintf(hdr, sizeof(hdr), "Lines: %d  |  File: %s",
             s_count, s_file ? "OK" : "FAIL");
    draw_line(g_target_bottom, hdr,
              2, 1, 0.42f, C2D_Color32(0xff, 0xff, 0x44, 0xff));

    /* Last 20 lines on bottom screen (smallest font, scroll from top) */
    int bshow = s_count < 20 ? s_count : 20;
    int bstart = (s_head - bshow + LOG_BUF_LINES) % LOG_BUF_LINES;
    for (int i = 0; i < bshow; i++) {
        int idx = (bstart + i) % LOG_BUF_LINES;
        float y = 14.0f + i * 11.0f;
        if (y > 237) break;
        draw_line(g_target_bottom, s_lines[idx],
                  2, y, 0.38f, C2D_Color32(0xcc, 0xcc, 0xcc, 0xff));
    }

    LightLock_Unlock(&s_lock);
    return true;
}
