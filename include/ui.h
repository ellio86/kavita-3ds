#pragma once

#include <3ds.h>
#include <citro2d.h>
#include <stdbool.h>

/* ------------------------------------------------------------------ */
/* Colours                                                              */
/* ------------------------------------------------------------------ */
#define COL_BG_TOP        C2D_Color32(0x1a, 0x1a, 0x2e, 0xff)
#define COL_BG_BOTTOM     C2D_Color32(0x16, 0x13, 0x2e, 0xff)
#define COL_ACCENT        C2D_Color32(0xe9, 0x4f, 0x37, 0xff)  /* red-orange */
#define COL_ACCENT2       C2D_Color32(0x39, 0x3e, 0x46, 0xff)  /* dark grey */
#define COL_WHITE         C2D_Color32(0xff, 0xff, 0xff, 0xff)
#define COL_GREY          C2D_Color32(0x88, 0x88, 0x99, 0xff)
#define COL_DARK          C2D_Color32(0x0f, 0x0f, 0x1a, 0xff)
#define COL_HIGHLIGHT     C2D_Color32(0xe9, 0x4f, 0x37, 0x55)
#define COL_ERROR         C2D_Color32(0xff, 0x44, 0x44, 0xff)
#define COL_SUCCESS       C2D_Color32(0x44, 0xff, 0x88, 0xff)
#define COL_PANEL         C2D_Color32(0x22, 0x22, 0x3a, 0xff)

/* ------------------------------------------------------------------ */
/* Text scale constants                                                 */
/* ------------------------------------------------------------------ */
#define FONT_LARGE  0.8f
#define FONT_MED    0.6f
#define FONT_SMALL  0.5f
#define FONT_TINY   0.42f

/* ------------------------------------------------------------------ */
/* Spinner state (updated each frame)                                   */
/* ------------------------------------------------------------------ */
typedef struct {
    float angle;   /* degrees, incremented each frame */
} UiSpinner;

void ui_spinner_tick(UiSpinner* s);

/* ------------------------------------------------------------------ */
/* Primitives                                                           */
/* ------------------------------------------------------------------ */

/* Filled rounded rectangle */
void ui_rect(C3D_RenderTarget* tgt, float x, float y, float w, float h, u32 color);

/* Text (single line). Returns pixel width. */
float ui_text(C3D_RenderTarget* tgt, const char* str, float x, float y,
              float scale, u32 color);

/* Text centred horizontally in [x, x+width] */
void ui_text_centered(C3D_RenderTarget* tgt, const char* str,
                       float x, float width, float y,
                       float scale, u32 color);

/* Text truncated with "..." if it exceeds max_width px */
void ui_text_truncated(C3D_RenderTarget* tgt, const char* str,
                        float x, float y, float max_width,
                        float scale, u32 color);

/* Button: filled rect + centred label. Returns true if tapped this frame. */
bool ui_button(C3D_RenderTarget* tgt,
               float x, float y, float w, float h,
               u32 bg_color, const char* label, float label_scale);

/* Accent button: centred text, or green face "A" + label when show_face_a (e.g. Connect). */
void ui_labeled_button(C3D_RenderTarget* tgt,
                       float x, float y, float w, float h,
                       u32 bg_color, bool show_face_a, const char* label);

/* Animated loading spinner drawn as a rotating arc */
void ui_spinner(C3D_RenderTarget* tgt, const UiSpinner* s,
                float cx, float cy, float r, u32 color);

/* Horizontal progress bar. progress in [0,1]. */
void ui_progress_bar(C3D_RenderTarget* tgt,
                      float x, float y, float w, float h,
                      float progress, u32 fg, u32 bg);

/* Simple vertical scrollable list of text items.
   selected and scroll_offset are in/out.
   Returns true if selection changed this frame. */
bool ui_list(C3D_RenderTarget* tgt,
             float x, float y, float w, float h,
             const char** items, int count,
             int* selected, int* scroll_offset,
             float item_height, float text_scale);

/* ------------------------------------------------------------------ */
/* Touch helpers                                                        */
/* ------------------------------------------------------------------ */

/* Returns true if the bottom-screen touch landed inside the rect this frame */
bool ui_touched(float x, float y, float w, float h);

/* Returns raw touch position (valid when hidKeysHeld & KEY_TOUCH) */
touchPosition ui_touch_pos(void);
