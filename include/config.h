#pragma once

#include <stdbool.h>

/* ------------------------------------------------------------------ */
/* Persisted configuration (stored on SD card)                          */
/* Path: sdmc:/3ds/kavita-3ds/config.ini                                */
/* Password is AES-256-GCM encrypted (device-bound key); see cred_crypto */
/* ------------------------------------------------------------------ */
typedef struct {
    char base_url[256];
    char username[64];
    char password[64];    /* in-memory only; on disk as password_enc= */
    bool cover_cache;       /* cache series/volume/chapter covers on SD */
    bool reader_page_cache;
    int  reader_cache_pages;
} Config;

bool config_load(Config* out);
bool config_save(const Config* cfg);
