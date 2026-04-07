#include "reader_page_cache.h"
#include "debug_log.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statvfs.h>

#define READ_CACHE_ROOT "sdmc:/3ds/kavita-3ds/pages"

/* FAT + headroom so writes do not fail at cluster boundaries */
#define SD_MARGIN_BYTES (256u * 1024u)
#define MAX_PAGE_FILE_BYTES (32u * 1024u * 1024u)

static void ensure_dirs(void) {
    mkdir("sdmc:/3ds", 0777);
    mkdir("sdmc:/3ds/kavita-3ds", 0777);
    mkdir(READ_CACHE_ROOT, 0777);
}

static void build_path(char* out, size_t out_sz, int chapter_id, int page0) {
    snprintf(out, out_sz, READ_CACHE_ROOT "/%08d_%05d.bin", chapter_id, page0);
}

static bool parse_name(const char* name, int* out_ch, int* out_pg) {
    return sscanf(name, "%08d_%05d.bin", out_ch, out_pg) == 2;
}

void reader_page_cache_clear(void) {
    DIR* d = opendir(READ_CACHE_ROOT);
    if (!d)
        return;
    struct dirent* ent;
    char path[288];
    while ((ent = readdir(d)) != NULL) {
        if (ent->d_name[0] == '.')
            continue;
        int ch, pg;
        if (!parse_name(ent->d_name, &ch, &pg))
            continue;
        snprintf(path, sizeof(path), READ_CACHE_ROOT "/%s", ent->d_name);
        if (remove(path) != 0)
            dlog("[page_cache] remove %s failed", path);
    }
    closedir(d);
}

bool reader_page_cache_exists(int chapter_id, int page0) {
    char path[160];
    build_path(path, sizeof(path), chapter_id, page0);
    struct stat st;
    return stat(path, &st) == 0 && S_ISREG(st.st_mode);
}

void reader_page_cache_delete_pages_before(int chapter_id, int reader_page0) {
    DIR* d = opendir(READ_CACHE_ROOT);
    if (!d)
        return;
    struct dirent* ent;
    char path[288];
    while ((ent = readdir(d)) != NULL) {
        if (ent->d_name[0] == '.')
            continue;
        int ch, pg;
        if (!parse_name(ent->d_name, &ch, &pg))
            continue;
        if (ch != chapter_id || pg >= reader_page0)
            continue;
        snprintf(path, sizeof(path), READ_CACHE_ROOT "/%s", ent->d_name);
        remove(path);
    }
    closedir(d);
}

static bool reader_page_cache_sd_free_bytes(u64* out_free) {
    if (!out_free)
        return false;
    struct statvfs sv;
    if (statvfs("sdmc:/", &sv) != 0)
        return false;
    *out_free = (u64)sv.f_bavail * (u64)sv.f_frsize;
    return true;
}

static bool list_best_evict(int chapter_id, int reader_page0, int lookahead_n,
                            int* out_pg) {
    /* Protected window: (reader_page0, reader_page0 + lookahead_n] — next N pages after
       the current spread start; pages <= reader_page0 are "behind" and evicted first. */
    DIR* d = opendir(READ_CACHE_ROOT);
    if (!d)
        return false;

    int min_behind = -1;
    int max_beyond  = -1;
    struct dirent* ent;

    while ((ent = readdir(d)) != NULL) {
        if (ent->d_name[0] == '.')
            continue;
        int ch, pg;
        if (!parse_name(ent->d_name, &ch, &pg))
            continue;
        if (ch != chapter_id)
            continue;
        if (pg <= reader_page0) {
            if (min_behind < 0 || pg < min_behind)
                min_behind = pg;
        } else if (lookahead_n > 0 && pg > reader_page0 + lookahead_n) {
            if (max_beyond < 0 || pg > max_beyond)
                max_beyond = pg;
        } else if (lookahead_n <= 0 && pg > reader_page0) {
            if (max_beyond < 0 || pg > max_beyond)
                max_beyond = pg;
        }
    }
    closedir(d);

    if (min_behind >= 0) {
        *out_pg = min_behind;
        return true;
    }
    if (max_beyond >= 0) {
        *out_pg = max_beyond;
        return true;
    }
    return false;
}

static bool reader_page_cache_evict_one(int chapter_id, int reader_page0, int lookahead_n) {
    int pg;
    if (!list_best_evict(chapter_id, reader_page0, lookahead_n, &pg))
        return false;
    char path[160];
    build_path(path, sizeof(path), chapter_id, pg);
    if (remove(path) != 0) {
        dlog("[page_cache] evict remove failed %s", path);
        return false;
    }
    return true;
}

static bool reader_page_cache_ensure_space(size_t need_bytes, int chapter_id,
                                           int reader_page0, int lookahead_n) {
    u64 free_b = 0;
    if (!reader_page_cache_sd_free_bytes(&free_b))
        return false;
    while ((u64)need_bytes + SD_MARGIN_BYTES > free_b) {
        if (!reader_page_cache_evict_one(chapter_id, reader_page0, lookahead_n)) {
            dlog("[page_cache] ensure_space: need %zu, free %llu, cannot evict",
                 need_bytes, (unsigned long long)free_b);
            return false;
        }
        if (!reader_page_cache_sd_free_bytes(&free_b))
            return false;
    }
    return true;
}

bool reader_page_cache_read(int chapter_id, int page0, u8** out_data, size_t* out_size) {
    if (!out_data || !out_size)
        return false;
    *out_data = NULL;
    *out_size = 0;

    char path[160];
    build_path(path, sizeof(path), chapter_id, page0);

    FILE* f = fopen(path, "rb");
    if (!f)
        return false;
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return false;
    }
    long len = ftell(f);
    if (len <= 0 || (size_t)len > MAX_PAGE_FILE_BYTES) {
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

bool reader_page_cache_write(int chapter_id, int page0,
                             const u8* data, size_t size,
                             int reader_page0, int lookahead_n) {
    if (!data || size == 0 || size > MAX_PAGE_FILE_BYTES)
        return false;

    if (!reader_page_cache_ensure_space(size, chapter_id, reader_page0, lookahead_n))
        return false;

    ensure_dirs();

    char path[160];
    char tmp[168];
    build_path(path, sizeof(path), chapter_id, page0);
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);

    FILE* f = fopen(tmp, "wb");
    if (!f) {
        dlog("[page_cache] fopen write %s failed", tmp);
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
        dlog("[page_cache] rename %s failed", path);
        return false;
    }
    return true;
}
