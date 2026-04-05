#include "config.h"
#include "cred_crypto.h"
#include "debug_log.h"

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>

#define CONFIG_DIR  "sdmc:/3ds/kavita-3ds"
#define CONFIG_PATH "sdmc:/3ds/kavita-3ds/config.ini"

bool config_load(Config* out) {
    if (!out) return false;
    memset(out, 0, sizeof(*out));

    FILE* f = fopen(CONFIG_PATH, "r");
    if (!f) return false;

    char password_enc[256];
    char legacy_pw[sizeof(out->password)];

    memset(password_enc, 0, sizeof(password_enc));
    memset(legacy_pw, 0, sizeof(legacy_pw));

    char line[512];
    while (fgets(line, sizeof(line), f)) {
        /* Strip trailing newline */
        size_t len = strlen(line);
        while (len > 0 && (line[len-1] == '\n' || line[len-1] == '\r')) {
            line[--len] = '\0';
        }

        char* eq = strchr(line, '=');
        if (!eq) continue;
        *eq = '\0';
        const char* key = line;
        const char* val = eq + 1;

        if (strcmp(key, "base_url") == 0) {
            strncpy(out->base_url, val, sizeof(out->base_url) - 1);
        } else if (strcmp(key, "username") == 0) {
            strncpy(out->username, val, sizeof(out->username) - 1);
        } else if (strcmp(key, "password_enc") == 0) {
            strncpy(password_enc, val, sizeof(password_enc) - 1);
        } else if (strcmp(key, "password") == 0) {
            /* Legacy plaintext — migrated to password_enc on next save */
            strncpy(legacy_pw, val, sizeof(legacy_pw) - 1);
        }
    }

    fclose(f);

    if (password_enc[0]) {
        if (!cred_decrypt_password(password_enc, out->password, sizeof(out->password))) {
            out->password[0] = '\0';
            dlog("[cfg] password decrypt failed (wrong console or corrupt file)");
        }
    } else {
        memcpy(out->password, legacy_pw, sizeof(out->password));
    }

    return out->base_url[0] != '\0';
}

bool config_save(const Config* cfg) {
    if (!cfg) return false;

    /* Ensure directory exists */
    mkdir(CONFIG_DIR, 0777);

    FILE* f = fopen(CONFIG_PATH, "w");
    if (!f) return false;

    fprintf(f, "base_url=%s\n", cfg->base_url);
    fprintf(f, "username=%s\n", cfg->username);
    if (cfg->password[0]) {
        char enc[256];
        if (cred_encrypt_password(cfg->password, enc, sizeof(enc)))
            fprintf(f, "password_enc=%s\n", enc);
        else
            dlog("[cfg] password encrypt failed; password not written to disk");
    }

    fclose(f);
    return true;
}
