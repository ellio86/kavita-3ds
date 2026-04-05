#include "zip_mem.h"
#include "debug_log.h"

#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <zlib.h>

#define ZIP_LF_SIG 0x04034b50u
#define ZIP_CD_SIG 0x02014b50u
#define ZIP_EOCD_SIG 0x06054b50u

struct ZipArchive {
    const u8* base;
    size_t    len;
    size_t    cd_offset;
    u32       cd_size;
    u16       cd_entries;
};

static u32 rd_u16(const u8* p) {
    return (u32)p[0] | ((u32)p[1] << 8);
}

static u32 rd_u32(const u8* p) {
    return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24);
}

static bool find_eocd(const u8* z, size_t len, size_t* eocd_off) {
    if (len < 22) return false;
    size_t scan = len - 22;
    if (scan > 65535 + 22)
        scan = len - (65535 + 22);
    for (size_t i = len - 22; i >= scan; i--) {
        if (rd_u32(z + i) == ZIP_EOCD_SIG) {
            *eocd_off = i;
            return true;
        }
        if (i == 0) break;
    }
    return false;
}

bool zip_open(const ZipBlob* blob, ZipArchive** out) {
    if (!blob || !blob->data || blob->size < 22 || !out) return false;
    size_t eocd;
    if (!find_eocd(blob->data, blob->size, &eocd)) {
        dlog("[zip] EOCD not found");
        return false;
    }
    const u8* e = blob->data + eocd;
    /* EOCD: total CD entries @10, CD size @12, CD offset @16 */
    u16 entries_total = (u16)rd_u16(e + 10);
    u32 cd_size = rd_u32(e + 12);
    u32 cd_off = rd_u32(e + 16);

    if ((size_t)cd_off + cd_size > blob->size) {
        dlog("[zip] bad CD range off=%u size=%u", (unsigned)cd_off, (unsigned)cd_size);
        return false;
    }

    ZipArchive* z = (ZipArchive*)malloc(sizeof(ZipArchive));
    if (!z) return false;
    z->base = blob->data;
    z->len = blob->size;
    z->cd_offset = cd_off;
    z->cd_size = cd_size;
    z->cd_entries = entries_total;
    *out = z;
    return true;
}

void zip_close(ZipArchive* z) {
    free(z);
}

static int path_cmp(const char* a, const char* b) {
    return strcasecmp(a, b);
}

static const u8* cd_next_record(const ZipArchive* z, const u8* p, const u8* cd_end,
                                 char* name_buf, size_t name_cap,
                                 u16* method, u32* comp_sz, u32* uncomp_sz,
                                 u32* local_off);

typedef void (*ZipWalkCb)(const char* name, void* user);

static void zip_walk(ZipArchive* z, ZipWalkCb cb, void* user) {
    const u8* cd = z->base + z->cd_offset;
    const u8* cd_end = cd + z->cd_size;
    const u8* p = cd;
    char name[512];

    while (p < cd_end) {
        u16 method;
        u32 comp_sz, uncomp_sz, local_off;
        const u8* next = cd_next_record(z, p, cd_end, name, sizeof(name),
                                         &method, &comp_sz, &uncomp_sz, &local_off);
        if (!next) break;
        size_t n = strlen(name);
        if (n && name[n - 1] != '/')
            cb(name, user);
        p = next;
    }
}

struct exists_user {
    const char* path;
    bool        found;
};

static void exists_cb(const char* name, void* u) {
    struct exists_user* eu = (struct exists_user*)u;
    if (path_cmp(name, eu->path) == 0)
        eu->found = true;
}

bool zip_exists_ci(ZipArchive* z, const char* path) {
    struct exists_user eu = { path, false };
    zip_walk(z, exists_cb, &eu);
    return eu.found;
}

struct suf_user {
    const char* suffix;
    char*       out;
    size_t      cap;
    bool        found;
};

static void suf_cb(const char* name, void* u) {
    struct suf_user* su = (struct suf_user*)u;
    if (su->found) return;
    size_t ln = strlen(name);
    size_t ls = strlen(su->suffix);
    if (ls > ln) return;
    if (strcasecmp(name + ln - ls, su->suffix) != 0) return;
    strncpy(su->out, name, su->cap - 1);
    su->out[su->cap - 1] = '\0';
    su->found = true;
}

bool zip_find_suffix_ci(ZipArchive* z, const char* suffix, char* out, size_t out_cap) {
    if (!out || out_cap == 0) return false;
    out[0] = '\0';
    struct suf_user su = { suffix, out, out_cap, false };
    zip_walk(z, suf_cb, &su);
    return su.found;
}

static const u8* cd_next_record(const ZipArchive* z, const u8* p, const u8* cd_end,
                                 char* name_buf, size_t name_cap,
                                 u16* method, u32* comp_sz, u32* uncomp_sz,
                                 u32* local_off) {
    if ((size_t)(p - z->base) + 46 > z->len || p >= cd_end)
        return NULL;
    if (rd_u32(p) != ZIP_CD_SIG)
        return NULL;

    u16 nm = (u16)rd_u16(p + 28);
    u16 el = (u16)rd_u16(p + 30);
    u16 cl = (u16)rd_u16(p + 32);
    *method = (u16)rd_u16(p + 10);
    *comp_sz = rd_u32(p + 20);
    *uncomp_sz = rd_u32(p + 24);
    *local_off = rd_u32(p + 42);

    size_t rec_len = 46u + nm + el + cl;
    if ((size_t)(p - z->base) + rec_len > z->len)
        return NULL;

    if (nm + 1 > name_cap)
        nm = (u16)(name_cap > 0 ? name_cap - 1 : 0);
    memcpy(name_buf, p + 46, nm);
    name_buf[nm] = '\0';

    return p + rec_len;
}

static u8* inflate_deflate(const u8* src, size_t src_len, size_t dst_len) {
    u8* dst = (u8*)malloc(dst_len);
    if (!dst) return NULL;

    z_stream strm;
    memset(&strm, 0, sizeof(strm));
    strm.next_in = (Bytef*)src;
    strm.avail_in = (uInt)src_len;
    strm.next_out = dst;
    strm.avail_out = (uInt)dst_len;

    if (inflateInit2(&strm, -MAX_WBITS) != Z_OK) {
        free(dst);
        return NULL;
    }
    int r = inflate(&strm, Z_FINISH);
    inflateEnd(&strm);
    if (r != Z_STREAM_END || strm.total_out != dst_len) {
        dlog("[zip] inflate r=%d total_out=%u expect %zu", r,
             (unsigned)strm.total_out, dst_len);
        free(dst);
        return NULL;
    }
    return dst;
}

u8* zip_read_file(ZipArchive* z, const char* path, size_t* out_len) {
    if (!z || !path || !out_len) return NULL;
    *out_len = 0;

    const u8* cd = z->base + z->cd_offset;
    const u8* cd_end = cd + z->cd_size;
    const u8* p = cd;
    char name[512];

    u16 method = 0;
    u32 comp_sz = 0, uncomp_sz = 0, local_off = 0;
    bool found = false;

    while (p < cd_end) {
        const u8* next = cd_next_record(z, p, cd_end, name, sizeof(name),
                                         &method, &comp_sz, &uncomp_sz, &local_off);
        if (!next) break;
        if (path_cmp(name, path) == 0) {
            found = true;
            break;
        }
        p = next;
    }
    if (!found) {
        dlog("[zip] not found: %s", path);
        return NULL;
    }

    if ((size_t)local_off + 30 > z->len) return NULL;
    const u8* lh = z->base + local_off;
    if (rd_u32(lh) != ZIP_LF_SIG) {
        dlog("[zip] bad local sig");
        return NULL;
    }
    u16 nm = (u16)rd_u16(lh + 26);
    u16 el = (u16)rd_u16(lh + 28);
    size_t data_off = local_off + 30u + nm + el;
    if (data_off + comp_sz > z->len) {
        dlog("[zip] data OOB");
        return NULL;
    }
    const u8* comp_data = z->base + data_off;

    if (method == 0) {
        if (comp_sz != uncomp_sz) return NULL;
        u8* copy = (u8*)malloc(comp_sz);
        if (!copy) return NULL;
        memcpy(copy, comp_data, comp_sz);
        *out_len = comp_sz;
        return copy;
    }
    if (method == 8) {
        u8* dec = inflate_deflate(comp_data, comp_sz, uncomp_sz);
        if (dec) *out_len = uncomp_sz;
        return dec;
    }
    dlog("[zip] unsupported method %u", (unsigned)method);
    return NULL;
}
