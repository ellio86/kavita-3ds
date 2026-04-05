#pragma once

#include <stdarg.h>
#include <stdbool.h>

/* ------------------------------------------------------------------ */
/* Debug logging                                                        */
/*                                                                      */
/* Always writes to sdmc:/3ds/kavita-3ds/debug.log on the SD card.     */
/* Hold SELECT in-app to view the last LOG_OVERLAY_LINES on screen.    */
/* ------------------------------------------------------------------ */

#define LOG_OVERLAY_LINES  20
#define LOG_LINE_LEN       200

void dlog_init(void);
void dlog_fini(void);

/* printf-style logging */
void dlog(const char* fmt, ...) __attribute__((format(printf, 1, 2)));

/* Draw the log overlay on both screens if SELECT is held.
   Call once per frame from app_tick(), AFTER all screen rendering.
   Returns true if the overlay was drawn (SELECT held). */
bool dlog_overlay_tick(void);
