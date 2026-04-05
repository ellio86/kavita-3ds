#pragma once

#include <3ds.h>
#include <citro2d.h>
#include <stdbool.h>

/* ------------------------------------------------------------------ */
/* Screen identifiers                                                   */
/* ------------------------------------------------------------------ */
typedef enum {
    SCREEN_SETUP,
    SCREEN_LIBRARIES,
    SCREEN_SETTINGS,
    SCREEN_SERIES,
    SCREEN_DETAIL,
    SCREEN_READER,
} AppScreen;

/* ------------------------------------------------------------------ */
/* Global application state                                             */
/* ------------------------------------------------------------------ */
typedef struct {
    AppScreen current_screen;

    /* Server connection */
    char base_url[256];    /* e.g. http://192.168.1.10:5000 */
    char username[64];
    char password[64];
    char token[1024];      /* JWT Bearer token (~600 bytes in practice) */
    char api_key[64];      /* Kavita apiKey used in image URL params */

    /* Navigation context */
    int  selected_library_id;
    char selected_library_name[128];

    int  selected_series_id;
    char selected_series_name[256];

    int  selected_volume_id;
    int  selected_chapter_id;
    int  reader_page;
    int  reader_total_pages;
    bool reader_epub;       /* EPUB: pages from downloaded archive, not Reader/image */

    /* Set to true to request app exit from any screen */
    bool exit_requested;

    /* Persisted via config.ini (see config_save / app_save_config) */
    bool cover_cache;
} AppState;

extern AppState g_app;

/* ------------------------------------------------------------------ */
/* Shared rendering resources (initialised in main.c)                  */
/* ------------------------------------------------------------------ */
extern C2D_TextBuf       g_text_buf;
extern C3D_RenderTarget* g_target_top;
extern C3D_RenderTarget* g_target_bottom;

/* ------------------------------------------------------------------ */
/* App lifecycle                                                        */
/* ------------------------------------------------------------------ */
void app_init(void);
void app_fini(void);
void app_tick(void);               /* called once per frame */
void app_transition(AppScreen next); /* change screen, calls fini/init */

/* Writes base_url, username, password_enc, cover_cache to SD config. */
void app_save_config(void);
