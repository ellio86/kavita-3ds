#include "app.h"
#include "config.h"
#include "screen_setup.h"
#include "screen_libraries.h"
#include "screen_series.h"
#include "screen_detail.h"
#include "screen_reader.h"

#include <string.h>

/* ------------------------------------------------------------------ */
/* Global state                                                         */
/* ------------------------------------------------------------------ */
AppState g_app;

/* ------------------------------------------------------------------ */
/* Internal helpers                                                     */
/* ------------------------------------------------------------------ */
static void screen_init(AppScreen s) {
    switch (s) {
        case SCREEN_SETUP:      screen_setup_init();     break;
        case SCREEN_LIBRARIES:  screen_libraries_init(); break;
        case SCREEN_SERIES:     screen_series_init();    break;
        case SCREEN_DETAIL:     screen_detail_init();    break;
        case SCREEN_READER:     screen_reader_init();    break;
    }
}

static void screen_fini(AppScreen s) {
    switch (s) {
        case SCREEN_SETUP:      screen_setup_fini();     break;
        case SCREEN_LIBRARIES:  screen_libraries_fini(); break;
        case SCREEN_SERIES:     screen_series_fini();    break;
        case SCREEN_DETAIL:     screen_detail_fini();    break;
        case SCREEN_READER:     screen_reader_fini();    break;
    }
}

static void screen_tick(AppScreen s) {
    switch (s) {
        case SCREEN_SETUP:      screen_setup_tick();     break;
        case SCREEN_LIBRARIES:  screen_libraries_tick(); break;
        case SCREEN_SERIES:     screen_series_tick();    break;
        case SCREEN_DETAIL:     screen_detail_tick();    break;
        case SCREEN_READER:     screen_reader_tick();    break;
    }
}

/* ------------------------------------------------------------------ */
/* Lifecycle                                                            */
/* ------------------------------------------------------------------ */
void app_init(void) {
    memset(&g_app, 0, sizeof(g_app));

    /* Try to restore saved credentials */
    Config cfg;
    if (config_load(&cfg)) {
        strncpy(g_app.base_url,  cfg.base_url,  sizeof(g_app.base_url)  - 1);
        strncpy(g_app.username,  cfg.username,  sizeof(g_app.username)  - 1);
        strncpy(g_app.password,  cfg.password,  sizeof(g_app.password)  - 1);
        /* Start on setup screen regardless — user must connect (tap or A) to login */
    }

    g_app.current_screen = SCREEN_SETUP;
    screen_init(g_app.current_screen);
}

void app_fini(void) {
    screen_fini(g_app.current_screen);
}

void app_tick(void) {
    screen_tick(g_app.current_screen);
}

void app_transition(AppScreen next) {
    if (next == g_app.current_screen) return;
    screen_fini(g_app.current_screen);
    g_app.current_screen = next;
    screen_init(next);
}
