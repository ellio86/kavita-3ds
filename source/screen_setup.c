#include "screen_setup.h"
#include "app.h"
#include "config.h"
#include "kavita_api.h"
#include "debug_log.h"
#include "ui.h"

#include <3ds.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

/* ------------------------------------------------------------------ */
/* Screen state                                                         */
/* ------------------------------------------------------------------ */
typedef enum {
    STATE_IDLE,
    STATE_CONNECTING,
    STATE_ERROR,
} SetupState;

static SetupState  s_state;
static char        s_status_msg[256];
static UiSpinner   s_spinner;

/* Background login thread */
static Thread      s_thread;
static volatile bool s_thread_done;
static volatile bool s_thread_ok;
static char        s_thread_token[1024];
static char        s_thread_api_key[64];

/* ------------------------------------------------------------------ */
/* Background thread: perform Kavita login                             */
/* ------------------------------------------------------------------ */
static void login_thread(void* arg) {
    (void)arg;
    bool ok = kavita_login(
        g_app.base_url,
        g_app.username,
        g_app.password,
        s_thread_token,  sizeof(s_thread_token),
        s_thread_api_key, sizeof(s_thread_api_key)
    );
    s_thread_ok   = ok;
    s_thread_done = true;
    threadExit(0);
}

/* ------------------------------------------------------------------ */
/* swkbd helper                                                         */
/* ------------------------------------------------------------------ */
static void prompt_text(const char* hint, const char* initial,
                         char* out, size_t out_sz,
                         SwkbdType type, bool password) {
    SwkbdState kbd;
    swkbdInit(&kbd, type, 2, (int)out_sz - 1);
    swkbdSetHintText(&kbd, hint);
    if (initial && initial[0]) swkbdSetInitialText(&kbd, initial);
    if (password) swkbdSetPasswordMode(&kbd, SWKBD_PASSWORD_HIDE_DELAY);
    swkbdSetButton(&kbd, SWKBD_BUTTON_RIGHT, "OK", true);
    swkbdSetButton(&kbd, SWKBD_BUTTON_LEFT,  "Cancel", false);

    char buf[256] = {0};
    SwkbdButton btn = swkbdInputText(&kbd, buf, sizeof(buf));
    if (btn == SWKBD_BUTTON_CONFIRM && buf[0]) {
        strncpy(out, buf, out_sz - 1);
        out[out_sz - 1] = '\0';
    }
}

static void setup_try_connect(void) {
    if (!g_app.base_url[0] || !g_app.username[0] || !g_app.password[0]) {
        snprintf(s_status_msg, sizeof(s_status_msg),
                 "Please fill in all fields");
        s_state = STATE_ERROR;
        return;
    }
    s_state       = STATE_CONNECTING;
    s_thread_done = false;
    s_thread_ok   = false;
    snprintf(s_status_msg, sizeof(s_status_msg), "Connecting...");
    dlog("[API] connecting to '%s' as '%s'", g_app.base_url, g_app.username);
    s_thread = threadCreate(login_thread, NULL,
                             32 * 1024, 0x30, 1, false);
}

/* ------------------------------------------------------------------ */
/* Lifecycle                                                            */
/* ------------------------------------------------------------------ */
void screen_setup_init(void) {
    s_state       = STATE_IDLE;
    s_thread_done = false;
    s_thread_ok   = false;
    s_spinner.angle = 0.0f;
    snprintf(s_status_msg, sizeof(s_status_msg),
             "Enter your Kavita server details");
}

void screen_setup_fini(void) {
    if (s_state == STATE_CONNECTING && s_thread) {
        /* Wait for thread to finish before destroying */
        threadJoin(s_thread, U64_MAX);
        threadFree(s_thread);
        s_thread = NULL;
    }
}

/* ------------------------------------------------------------------ */
/* Tick                                                                 */
/* ------------------------------------------------------------------ */
void screen_setup_tick(void) {
    /* --- Input handling --- */
    if (s_state == STATE_IDLE || s_state == STATE_ERROR) {
        /* Field tap: URL */
        if (ui_touched(10, 30, 300, 26)) {
            prompt_text("http://192.168.1.10:5000",
                         g_app.base_url,
                         g_app.base_url, sizeof(g_app.base_url),
                         SWKBD_TYPE_NORMAL, false);
        }
        /* Username */
        if (ui_touched(10, 75, 300, 26)) {
            prompt_text("admin",
                         g_app.username,
                         g_app.username, sizeof(g_app.username),
                         SWKBD_TYPE_NORMAL, false);
        }
        /* Password */
        if (ui_touched(10, 120, 300, 26)) {
            prompt_text("password",
                         g_app.password,
                         g_app.password, sizeof(g_app.password),
                         SWKBD_TYPE_NORMAL, true);
        }

        /* Connect: touch button or press A */
        if (ui_touched(80, 165, 160, 32) || (hidKeysDown() & KEY_A)) {
            setup_try_connect();
        }
    }

    /* Check if login thread finished */
    if (s_state == STATE_CONNECTING && s_thread_done) {
        threadJoin(s_thread, U64_MAX);
        threadFree(s_thread);
        s_thread = NULL;

        if (s_thread_ok) {
            strncpy(g_app.token,   s_thread_token,   sizeof(g_app.token)   - 1);
            strncpy(g_app.api_key, s_thread_api_key, sizeof(g_app.api_key) - 1);

            /* Persist credentials */
            Config cfg;
            strncpy(cfg.base_url, g_app.base_url, sizeof(cfg.base_url) - 1);
            strncpy(cfg.username, g_app.username, sizeof(cfg.username) - 1);
            strncpy(cfg.password, g_app.password, sizeof(cfg.password) - 1);
            config_save(&cfg);

            app_transition(SCREEN_LIBRARIES);
            return;
        } else {
            snprintf(s_status_msg, sizeof(s_status_msg),
                     "Login failed. Hold SELECT for details.");
            s_state = STATE_ERROR;
            dlog("[ERR] setup: login failed for user='%s' url='%s'",
                 g_app.username, g_app.base_url);
        }
    }

    if (s_state == STATE_CONNECTING) {
        ui_spinner_tick(&s_spinner);
    }

    /* --- Render --- */

    /* Top screen: title */
    C2D_SceneBegin(g_target_top);
    ui_text_centered(g_target_top, "Kavita", 0, 400, 20, FONT_LARGE, COL_ACCENT);
    ui_text_centered(g_target_top, "3DS Reader", 0, 400, 55, FONT_MED, COL_GREY);

    /* Status message */
    u32 status_color = (s_state == STATE_ERROR) ? COL_ERROR : COL_GREY;
    ui_text_centered(g_target_top, s_status_msg, 0, 400, 200, FONT_SMALL, status_color);

    if (s_state == STATE_CONNECTING) {
        ui_spinner(g_target_top, &s_spinner, 200, 130, 20, COL_ACCENT);
    }

    /* Bottom screen: input fields */
    C2D_SceneBegin(g_target_bottom);

    /* Server URL field */
    ui_rect(g_target_bottom, 10, 10, 300, 20, COL_PANEL);
    ui_text(g_target_bottom, "Server URL", 14, 12, FONT_TINY, COL_GREY);

    ui_rect(g_target_bottom, 10, 30, 300, 26, COL_ACCENT2);
    if (g_app.base_url[0]) {
        ui_text_truncated(g_target_bottom, g_app.base_url,
                           14, 34, 292, FONT_SMALL, COL_WHITE);
    } else {
        ui_text(g_target_bottom, "Tap to enter...", 14, 34, FONT_SMALL, COL_GREY);
    }

    /* Username field */
    ui_rect(g_target_bottom, 10, 58, 300, 20, COL_PANEL);
    ui_text(g_target_bottom, "Username", 14, 60, FONT_TINY, COL_GREY);

    ui_rect(g_target_bottom, 10, 75, 300, 26, COL_ACCENT2);
    if (g_app.username[0]) {
        ui_text(g_target_bottom, g_app.username, 14, 79, FONT_SMALL, COL_WHITE);
    } else {
        ui_text(g_target_bottom, "Tap to enter...", 14, 79, FONT_SMALL, COL_GREY);
    }

    /* Password field */
    ui_rect(g_target_bottom, 10, 103, 300, 20, COL_PANEL);
    ui_text(g_target_bottom, "Password", 14, 105, FONT_TINY, COL_GREY);

    ui_rect(g_target_bottom, 10, 120, 300, 26, COL_ACCENT2);
    if (g_app.password[0]) {
        ui_text(g_target_bottom, "••••••••", 14, 124, FONT_SMALL, COL_GREY);
    } else {
        ui_text(g_target_bottom, "Tap to enter...", 14, 124, FONT_SMALL, COL_GREY);
    }

    /* Connect button (green A + "Connect" when idle) */
    bool connecting = (s_state == STATE_CONNECTING);
    u32 btn_color = connecting ? COL_GREY : COL_ACCENT;
    ui_labeled_button(g_target_bottom, 80, 165, 160, 32, btn_color,
                      !connecting, connecting ? "Connecting..." : "Connect");

    /* Help text */
    ui_text_centered(g_target_bottom, "SELECT+START to exit",
                      0, 320, 228, FONT_TINY, COL_GREY);
}
