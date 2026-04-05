#include "cover_cache.h"
#include "debug_log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define CACHE_ROOT "sdmc:/3ds/kavita-3ds/covers"

#define MAX_COVER_BYTES (8u * 1024u * 1024u)

static uint32_t url_fingerprint(const char* base_url) {
    uint32_t h = 2166136261u;
    if (!base_url) return h;
    for (const unsigned char* p = (const unsigned char*)base_url; *p; p++) {
        h ^= *p;
        h *= 16777619u;
    }
    return h;
}

static void build_paths(char* dir, size_t dir_sz, char* file, size_t file_sz,
                        const char* base_url, char kind, int id) {
    uint32_t fp = url_fingerprint(base_url);
    snprintf(dir, dir_sz, CACHE_ROOT "/%08lx", (unsigned long)fp);
    snprintf(file, file_sz, CACHE_ROOT "/%08lx/%c%d.bin",
             (unsigned long)fp, kind, id);
}

static bool ensure_dir(const char* path) {
    struct stat st;
    if (stat(path, &st) == 0)
        return S_ISDIR(st.st_mode);
    if (mkdir(path, 0777) == 0)
        return true;
    dlog("[cache] mkdir %s failed", path);
    return false;
}

bool cover_cache_read(const char* base_url, char kind, int id,
                      u8** out_data, size_t* out_size) {
    if (!base_url || !base_url[0] || !out_data || !out_size)
        return false;
    *out_data = NULL;
    *out_size = 0;

    char subdir[96], path[128];
    build_paths(subdir, sizeof(subdir), path, sizeof(path), base_url, kind, id);

    FILE* f = fopen(path, "rb");
    if (!f)
        return false;

    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return false;
    }
    long len = ftell(f);
    if (len <= 0 || (size_t)len > MAX_COVER_BYTES) {
        fclose(f);
        return false;
    }
    if (fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        return false;
    }

    u8* buf = (u8*)malloc((size_t)len);
    if (!buf) {
        fclose(f);
        return false;
    }
    if (fread(buf, 1, (size_t)len, f) != (size_t)len) {
        free(buf);
        fclose(f);
        return false;
    }
    fclose(f);

    *out_data = buf;
    *out_size = (size_t)len;
    return true;
}

bool cover_cache_write(const char* base_url, char kind, int id,
                       const u8* data, size_t size) {
    if (!base_url || !base_url[0] || !data || size == 0 || size > MAX_COVER_BYTES)
        return false;

    mkdir("sdmc:/3ds", 0777);
    mkdir("sdmc:/3ds/kavita-3ds", 0777);
    mkdir(CACHE_ROOT, 0777);

    char subdir[96], path[128];
    build_paths(subdir, sizeof(subdir), path, sizeof(path), base_url, kind, id);

    if (!ensure_dir(subdir))
        return false;

    char tmp[144];
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);

    FILE* f = fopen(tmp, "wb");
    if (!f) {
        dlog("[cache] fopen write %s failed", tmp);
        return false;
    }
    if (fwrite(data, 1, size, f) != size) {
        fclose(f);
        remove(tmp);
        return false;
    }
    fclose(f);

    remove(path);
    if (rename(tmp, path) != 0) {
        remove(tmp);
        dlog("[cache] rename to %s failed", path);
        return false;
    }
    return true;
}
