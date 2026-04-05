#include "ui.h"

#include <3ds.h>
#include <citro2d.h>
#include <math.h>
#include <string.h>
#include <stdio.h>

#include "app.h"   /* for g_text_buf */

#define PI 3.14159265358979f

/* ------------------------------------------------------------------ */
/* Spinner                                                              */
/* ------------------------------------------------------------------ */
void ui_spinner_tick(UiSpinner* s) {
    s->angle += 6.0f;
    if (s->angle >= 360.0f) s->angle -= 360.0f;
}

/* ------------------------------------------------------------------ */
/* Primitives                                                           */
/* ------------------------------------------------------------------ */
void ui_rect(C3D_RenderTarget* tgt, float x, float y, float w, float h, u32 color) {
    C2D_SceneBegin(tgt);
    C2D_DrawRectSolid(x, y, 0.5f, w, h, color);
}

float ui_text(C3D_RenderTarget* tgt, const char* str,
              float x, float y, float scale, u32 color) {
    C2D_Text t;
    C2D_TextParse(&t, g_text_buf, str);
    C2D_TextOptimize(&t);
    C2D_SceneBegin(tgt);
    C2D_DrawText(&t, C2D_WithColor, x, y, 0.5f, scale, scale, color);

    float tw, th;
    C2D_TextGetDimensions(&t, scale, scale, &tw, &th);
    return tw;
}

void ui_text_centered(C3D_RenderTarget* tgt, const char* str,
                       float x, float width, float y,
                       float scale, u32 color) {
    C2D_Text t;
    C2D_TextParse(&t, g_text_buf, str);
    C2D_TextOptimize(&t);

    float tw, th;
    C2D_TextGetDimensions(&t, scale, scale, &tw, &th);

    float draw_x = x + (width - tw) * 0.5f;
    if (draw_x < x) draw_x = x;

    C2D_SceneBegin(tgt);
    C2D_DrawText(&t, C2D_WithColor, draw_x, y, 0.5f, scale, scale, color);
}

void ui_text_truncated(C3D_RenderTarget* tgt, const char* str,
                        float x, float y, float max_width,
                        float scale, u32 color) {
    char buf[256];
    strncpy(buf, str, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';

    C2D_Text t;
    C2D_TextParse(&t, g_text_buf, buf);
    C2D_TextOptimize(&t);

    float tw, th;
    C2D_TextGetDimensions(&t, scale, scale, &tw, &th);

    if (tw > max_width && strlen(buf) > 3) {
        /* Trim until it fits */
        size_t len = strlen(buf);
        while (len > 3 && tw > max_width) {
            len--;
            buf[len - 2] = '.';
            buf[len - 1] = '.';
            buf[len    ] = '.';
            buf[len + 1] = '\0';
            C2D_TextBufClear(g_text_buf);
            C2D_TextParse(&t, g_text_buf, buf);
            C2D_TextOptimize(&t);
            C2D_TextGetDimensions(&t, scale, scale, &tw, &th);
        }
    }

    C2D_SceneBegin(tgt);
    C2D_DrawText(&t, C2D_WithColor, x, y, 0.5f, scale, scale, color);
}

/* ------------------------------------------------------------------ */
/* Button                                                               */
/* ------------------------------------------------------------------ */
bool ui_button(C3D_RenderTarget* tgt,
               float x, float y, float w, float h,
               u32 bg_color, const char* label, float label_scale) {
    C2D_SceneBegin(tgt);
    C2D_DrawRectSolid(x, y, 0.5f, w, h, bg_color);

    /* Label */
    C2D_Text t;
    C2D_TextParse(&t, g_text_buf, label);
    C2D_TextOptimize(&t);
    float tw, th;
    C2D_TextGetDimensions(&t, label_scale, label_scale, &tw, &th);
    float lx = x + (w - tw) * 0.5f;
    float ly = y + (h - th) * 0.5f;
    C2D_DrawText(&t, C2D_WithColor, lx, ly, 0.6f,
                 label_scale, label_scale, COL_WHITE);

    /* Touch detection (bottom screen only — y < 240) */
    if (tgt != g_target_bottom) return false;
    if (!(hidKeysDown() & KEY_TOUCH)) return false;
    touchPosition tp;
    hidTouchRead(&tp);
    return (tp.px >= x && tp.px <= x + w &&
            tp.py >= y && tp.py <= y + h);
}

void ui_labeled_button(C3D_RenderTarget* tgt,
                       float x, float y, float w, float h,
                       u32 bg_color, bool show_face_a, const char* label) {
    C2D_SceneBegin(tgt);
    C2D_DrawRectSolid(x, y, 0.5f, w, h, bg_color);

    if (!show_face_a) {
        C2D_Text t;
        C2D_TextBufClear(g_text_buf);
        C2D_TextParse(&t, g_text_buf, label);
        C2D_TextOptimize(&t);
        float tw, th;
        C2D_TextGetDimensions(&t, FONT_MED, FONT_MED, &tw, &th);
        float lx = x + (w - tw) * 0.5f;
        float ly = y + (h - th) * 0.5f;
        C2D_DrawText(&t, C2D_WithColor, lx, ly, 0.6f,
                     FONT_MED, FONT_MED, COL_WHITE);
        return;
    }

    const float icon_r = 11.0f;
    const float gap    = 8.0f;

    C2D_Text t;
    C2D_TextBufClear(g_text_buf);
    C2D_TextParse(&t, g_text_buf, label);
    C2D_TextOptimize(&t);
    float tw, th;
    C2D_TextGetDimensions(&t, FONT_MED, FONT_MED, &tw, &th);

    float total    = icon_r * 2 + gap + tw;
    float start_x  = x + (w - total) * 0.5f;
    float cy       = y + h * 0.5f;
    float icon_cx  = start_x + icon_r;

    u32 green = C2D_Color32(0x2e, 0xc2, 0x4a, 0xff);
    float d   = icon_r * 2;
    C2D_DrawEllipse(icon_cx - icon_r, cy - icon_r, 0.6f, d, d,
                    green, green, green, green);

    C2D_TextBufClear(g_text_buf);
    C2D_Text ta;
    C2D_TextParse(&ta, g_text_buf, "A");
    C2D_TextOptimize(&ta);
    /* citro2d scale is ~0.35–0.8, not pixels (icon_r * k was wrongly ~8) */
    const float ascale = 0.38f;
    float aw, ah;
    C2D_TextGetDimensions(&ta, ascale, ascale, &aw, &ah);
    C2D_DrawText(&ta, C2D_WithColor, icon_cx - aw * 0.5f, cy - ah * 0.5f, 0.65f,
                 ascale, ascale, COL_WHITE);

    C2D_TextBufClear(g_text_buf);
    C2D_TextParse(&t, g_text_buf, label);
    C2D_TextOptimize(&t);
    C2D_TextGetDimensions(&t, FONT_MED, FONT_MED, &tw, &th);
    float ly     = y + (h - th) * 0.5f;
    float text_x = start_x + icon_r * 2 + gap;
    C2D_DrawText(&t, C2D_WithColor, text_x, ly, 0.6f,
                 FONT_MED, FONT_MED, COL_WHITE);
}

/* ------------------------------------------------------------------ */
/* Spinner                                                              */
/* ------------------------------------------------------------------ */
void ui_spinner(C3D_RenderTarget* tgt, const UiSpinner* s,
                float cx, float cy, float r, u32 color) {
    C2D_SceneBegin(tgt);
    /* Draw a short arc (~120°) rotated by s->angle */
    int segs = 12;
    float arc = 120.0f * (PI / 180.0f);
    float base = s->angle * (PI / 180.0f);

    for (int i = 0; i < segs; i++) {
        float a0 = base + arc * ((float)i     / segs);
        float a1 = base + arc * ((float)(i+1) / segs);
        float x0 = cx + cosf(a0) * r;
        float y0 = cy + sinf(a0) * r;
        float x1 = cx + cosf(a1) * r;
        float y1 = cy + sinf(a1) * r;
        /* Fade alpha along the arc */
        u32 alpha = (u32)(255 * (float)(i + 1) / segs);
        u32 c = (color & 0x00ffffff) | (alpha << 24);
        C2D_DrawLine(x0, y0, c, x1, y1, c, 2.0f, 0.5f);
    }
}

/* ------------------------------------------------------------------ */
/* Progress bar                                                         */
/* ------------------------------------------------------------------ */
void ui_progress_bar(C3D_RenderTarget* tgt,
                      float x, float y, float w, float h,
                      float progress, u32 fg, u32 bg) {
    C2D_SceneBegin(tgt);
    C2D_DrawRectSolid(x, y, 0.5f, w, h, bg);
    if (progress > 0.0f) {
        float fw = w * (progress > 1.0f ? 1.0f : progress);
        C2D_DrawRectSolid(x, y, 0.6f, fw, h, fg);
    }
}

/* ------------------------------------------------------------------ */
/* Scrollable list                                                      */
/* ------------------------------------------------------------------ */
bool ui_list(C3D_RenderTarget* tgt,
             float x, float y, float w, float h,
             const char** items, int count,
             int* selected, int* scroll_offset,
             float item_height, float text_scale) {
    if (!items || count <= 0 || !selected || !scroll_offset) return false;

    bool changed = false;
    int visible = (int)(h / item_height);

    /* D-pad navigation */
    u32 kd = hidKeysDown();
    if (kd & KEY_DOWN) {
        if (*selected < count - 1) { (*selected)++; changed = true; }
    }
    if (kd & KEY_UP) {
        if (*selected > 0) { (*selected)--; changed = true; }
    }

    /* Clamp scroll */
    if (*selected < *scroll_offset) *scroll_offset = *selected;
    if (*selected >= *scroll_offset + visible) *scroll_offset = *selected - visible + 1;
    if (*scroll_offset < 0) *scroll_offset = 0;

    C2D_SceneBegin(tgt);

    for (int i = 0; i < visible && (*scroll_offset + i) < count; i++) {
        int idx = *scroll_offset + i;
        float iy = y + i * item_height;

        if (idx == *selected) {
            C2D_DrawRectSolid(x, iy, 0.5f, w, item_height - 1, COL_HIGHLIGHT);
        }

        C2D_Text t;
        C2D_TextParse(&t, g_text_buf, items[idx]);
        C2D_TextOptimize(&t);
        C2D_DrawText(&t, C2D_WithColor, x + 4, iy + 2, 0.6f,
                     text_scale, text_scale, COL_WHITE);
    }

    /* Touch selection */
    if (tgt == g_target_bottom && (hidKeysDown() & KEY_TOUCH)) {
        touchPosition tp;
        hidTouchRead(&tp);
        if (tp.px >= x && tp.px <= x + w &&
            tp.py >= y && tp.py <= y + h) {
            int tapped = *scroll_offset + (int)((tp.py - y) / item_height);
            if (tapped >= 0 && tapped < count && tapped != *selected) {
                *selected = tapped;
                changed = true;
            }
        }
    }

    return changed;
}

/* ------------------------------------------------------------------ */
/* Touch helpers                                                        */
/* ------------------------------------------------------------------ */
bool ui_touched(float x, float y, float w, float h) {
    if (!(hidKeysDown() & KEY_TOUCH)) return false;
    touchPosition tp;
    hidTouchRead(&tp);
    return (tp.px >= x && tp.px <= x + w &&
            tp.py >= y && tp.py <= y + h);
}

touchPosition ui_touch_pos(void) {
    touchPosition tp;
    hidTouchRead(&tp);
    return tp;
}
