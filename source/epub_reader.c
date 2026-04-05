#include "epub_reader.h"
#include "http_client.h"
#include "kavita_api.h"
#include "zip_mem.h"
#include "debug_log.h"

#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <stdio.h>
#include <ctype.h>

typedef struct {
    char id[96];
    char href[260];
    char media[96];
} ManItem;

static ZipArchive* s_zip;
static u8*         s_outer_buf;
static size_t      s_outer_sz;
static u8*         s_inner_buf;
static size_t      s_inner_sz;
static ManItem*    s_man;
static int         s_man_n;
static char**      s_spine;
static int         s_spine_n;
static bool        s_open;

void epub_reader_close(void) {
    if (s_zip) {
        zip_close(s_zip);
        s_zip = NULL;
    }
    free(s_outer_buf);
    s_outer_buf = NULL;
    s_outer_sz = 0;
    free(s_inner_buf);
    s_inner_buf = NULL;
    s_inner_sz = 0;
    free(s_man);
    s_man = NULL;
    s_man_n = 0;
    if (s_spine) {
        for (int i = 0; i < s_spine_n; i++)
            free(s_spine[i]);
        free(s_spine);
        s_spine = NULL;
    }
    s_spine_n = 0;
    s_open = false;
}

bool epub_reader_is_open(void) {
    return s_open;
}

static void norm_slashes(char* p) {
    for (; *p; p++)
        if (*p == '\\') *p = '/';
}

static void dirname_inplace(char* path) {
    char* s = strrchr(path, '/');
    if (s) *s = '\0';
    else path[0] = '\0';
}

/* Resolve rel against directory base_dir (no trailing slash). */
static void path_resolve(const char* base_dir, const char* rel, char* out, size_t cap) {
    char stack[16][260];
    int depth = 0;

    char tmp[512];
    if (rel[0] == '/') {
        snprintf(tmp, sizeof(tmp), "%s", rel + 1);
    } else if (base_dir && base_dir[0]) {
        snprintf(tmp, sizeof(tmp), "%s/%s", base_dir, rel);
    } else {
        snprintf(tmp, sizeof(tmp), "%s", rel);
    }
    norm_slashes(tmp);

    char* save = NULL;
    char* tok = strtok_r(tmp, "/", &save);
    while (tok) {
        if (strcmp(tok, ".") == 0) {
        } else if (strcmp(tok, "..") == 0) {
            if (depth > 0) depth--;
        } else {
            if (depth < 16) {
                char* d = stack[depth++];
                strncpy(d, tok, sizeof(stack[0]) - 1);
                d[sizeof(stack[0]) - 1] = '\0';
            }
        }
        tok = strtok_r(NULL, "/", &save);
    }

    out[0] = '\0';
    for (int i = 0; i < depth; i++) {
        size_t lo = strlen(out);
        snprintf(out + lo, cap - lo, "%s%s", i ? "/" : "", stack[i]);
    }
}

static bool extract_attr(const char* block, const char* key, char* out, size_t cap) {
    char pat[72];
    snprintf(pat, sizeof(pat), "%s=\"", key);
    const char* p = strstr(block, pat);
    char quote = '"';
    if (!p) {
        snprintf(pat, sizeof(pat), "%s='", key);
        p = strstr(block, pat);
        quote = '\'';
    }
    if (!p) return false;
    p += strlen(pat);
    const char* e = strchr(p, quote);
    if (!e) return false;
    size_t n = (size_t)(e - p);
    if (n >= cap) n = cap - 1;
    memcpy(out, p, n);
    out[n] = '\0';
    return true;
}

static const char* find_full_path_container(const char* xml) {
    const char* k = strstr(xml, "full-path");
    if (!k) return NULL;
    const char* eq = strchr(k, '=');
    if (!eq) return NULL;
    eq++;
    while (*eq == ' ' || *eq == '\t') eq++;
    char q = *eq;
    if (q != '"' && q != '\'') return NULL;
    eq++;
    const char* end = strchr(eq, q);
    if (!end) return NULL;
    static char buf[512];
    size_t n = (size_t)(end - eq);
    if (n >= sizeof(buf)) return NULL;
    memcpy(buf, eq, n);
    buf[n] = '\0';
    return buf;
}

static const char* skip_xml_decl(const char* s) {
    if (strncmp(s, "\xef\xbb\xbf", 3) == 0) s += 3;
    if (strncmp(s, "<?xml", 5) == 0) {
        const char* e = strstr(s, "?>");
        if (e) return e + 2;
    }
    return s;
}

static ManItem* man_find(const char* id) {
    for (int i = 0; i < s_man_n; i++) {
        if (strcmp(s_man[i].id, id) == 0)
            return &s_man[i];
    }
    return NULL;
}

static bool is_page_candidate(const ManItem* m) {
    const char* h = m->href;
    size_t len = strlen(h);
    if (strstr(m->media, "html"))
        return true;
    if (strstr(m->media, "image/"))
        return true;
    static const char* exts[] = {
        ".xhtml", ".html", ".htm", ".jpg", ".jpeg", ".png", ".gif"
    };
    for (size_t i = 0; i < sizeof(exts) / sizeof(exts[0]); i++) {
        size_t el = strlen(exts[i]);
        if (len >= el && strcasecmp(h + len - el, exts[i]) == 0)
            return true;
    }
    return false;
}

/* Attribute name must not be a suffix of a longer name (e.g. src in data-src). */
static bool attr_name_boundary_ok(const char* doc_start, const char* name_pos) {
    if (name_pos <= doc_start)
        return true;
    unsigned char c = (unsigned char)name_pos[-1];
    if (isalnum(c) || c == '-' || c == '_' || c == ':')
        return false;
    return true;
}

static bool find_attr_quoted(const char* doc, const char* from, const char* name,
                              char* val, size_t val_cap,
                              const char** out_name_pos, const char** out_after) {
    size_t nlen = strlen(name);
    for (const char* p = from; *p; p++) {
        if (strncasecmp(p, name, nlen) != 0)
            continue;
        if (!attr_name_boundary_ok(doc, p))
            continue;
        const char* q = p + nlen;
        while (*q == ' ' || *q == '\t' || *q == '\r' || *q == '\n')
            q++;
        if (*q != '=')
            continue;
        q++;
        while (*q == ' ' || *q == '\t')
            q++;
        if (*q != '"' && *q != '\'')
            continue;
        char delim = *q++;
        const char* e = strchr(q, delim);
        if (!e)
            continue;
        size_t n = (size_t)(e - q);
        if (n >= val_cap)
            n = val_cap - 1;
        memcpy(val, q, n);
        val[n] = '\0';
        *out_name_pos = p;
        *out_after    = e + 1;
        return true;
    }
    return false;
}

static void strip_url_query(char* s) {
    char* q = strchr(s, '?');
    if (q)
        *q = '\0';
}

static int hex_digit(unsigned char c) {
    if (c >= '0' && c <= '9') return (int)(c - '0');
    if (c >= 'a' && c <= 'f') return (int)(c - 'a' + 10);
    if (c >= 'A' && c <= 'F') return (int)(c - 'A' + 10);
    return -1;
}

static void uri_decode_inplace(char* s) {
    char* r = s;
    char* w = s;
    while (*r) {
        if (*r == '%' && r[1] && r[2]) {
            int hi = hex_digit((unsigned char)r[1]);
            int lo = hex_digit((unsigned char)r[2]);
            if (hi >= 0 && lo >= 0) {
                *w++ = (char)((hi << 4) | lo);
                r += 3;
                continue;
            }
        }
        *w++ = *r++;
    }
    *w = '\0';
}

static bool is_remote_or_data_url(const char* s) {
    return strncmp(s, "http://", 7) == 0 || strncmp(s, "https://", 8) == 0 ||
           strncmp(s, "data:", 5) == 0;
}

static bool looks_like_local_image_path(const char* s) {
    if (!s || !s[0])
        return false;
    char tmp[512];
    strncpy(tmp, s, sizeof(tmp) - 1);
    tmp[sizeof(tmp) - 1] = '\0';
    strip_url_query(tmp);
    size_t len = strlen(tmp);
    static const char* exts[] = { ".jpg", ".jpeg", ".png", ".gif" };
    for (size_t i = 0; i < sizeof(exts) / sizeof(exts[0]); i++) {
        size_t el = strlen(exts[i]);
        if (len >= el && strcasecmp(tmp + len - el, exts[i]) == 0)
            return true;
    }
    return false;
}

/* Walk srcset / picture candidates until one looks like a local raster image. */
static bool first_url_from_srcset(const char* setval, char* out, size_t cap) {
    const char* p = setval;
    while (*p) {
        while (*p == ' ' || *p == '\t' || *p == ',')
            p++;
        if (!*p)
            break;
        const char* start = p;
        while (*p && *p != ' ' && *p != '\t' && *p != ',')
            p++;
        size_t n = (size_t)(p - start);
        if (n > 0 && n < cap) {
            memcpy(out, start, n);
            out[n] = '\0';
            uri_decode_inplace(out);
            strip_url_query(out);
            if (looks_like_local_image_path(out))
                return true;
        }
    }
    return false;
}

/* Inline CSS: url("a.jpg") or url(a.jpg) */
static bool try_style_url_scan(const char* xhtml, char* out, size_t cap) {
    const char* p = xhtml;
    while (*p) {
        if (strncasecmp(p, "url(", 4) != 0) {
            p++;
            continue;
        }
        const char* inner = p + 4;
        while (*inner == ' ' || *inner == '\t')
            inner++;
        const char* start;
        const char* e;
        if (*inner == '"' || *inner == '\'') {
            char q = *inner++;
            start = inner;
            e = strchr(start, q);
        } else {
            start = inner;
            e = strchr(start, ')');
        }
        if (!e || e <= start) {
            p += 4;
            continue;
        }
        size_t n = (size_t)(e - start);
        if (n == 0 || n >= cap) {
            p += 4;
            continue;
        }
        memcpy(out, start, n);
        out[n] = '\0';
        uri_decode_inplace(out);
        strip_url_query(out);
        if (!is_remote_or_data_url(out) && looks_like_local_image_path(out))
            return true;
        p += 4;
    }
    return false;
}

static bool try_srcset_scan(const char* xhtml, char* out, size_t cap) {
    static const char key[] = "srcset";
    const size_t klen = sizeof(key) - 1;
    for (const char* p = xhtml; *p; p++) {
        if (strncasecmp(p, key, klen) != 0)
            continue;
        if (!attr_name_boundary_ok(xhtml, p))
            continue;
        const char* q = p + klen;
        while (*q == ' ' || *q == '\t' || *q == '\r' || *q == '\n')
            q++;
        if (*q != '=')
            continue;
        q++;
        while (*q == ' ' || *q == '\t')
            q++;
        if (*q != '"' && *q != '\'')
            continue;
        char delim = *q++;
        const char* e = strchr(q, delim);
        if (!e)
            continue;
        size_t slen = (size_t)(e - q);
        if (slen >= 2048)
            continue;
        char buf[2048];
        memcpy(buf, q, slen);
        buf[slen] = '\0';
        if (first_url_from_srcset(buf, out, cap))
            return true;
    }
    return false;
}

static bool xhtml_first_image(const char* xhtml, const char* xhtml_zip_path,
                               char* img_zip_path, size_t cap) {
    static const char* const ATTRS[] = {
        "xlink:href",
        "data-src",
        "data-original",
        "src",
        "poster",
        "data",
        "href",
    };

    char base[512];
    strncpy(base, xhtml_zip_path, sizeof(base) - 1);
    base[sizeof(base) - 1] = '\0';
    dirname_inplace(base);

    const char* resume = xhtml;
    while (*resume) {
        size_t best_off = (size_t)-1;
        char   cand[512];
        const char* next_resume = NULL;

        for (size_t i = 0; i < sizeof(ATTRS) / sizeof(ATTRS[0]); i++) {
            const char *np, *af;
            char tmp[512];
            if (!find_attr_quoted(xhtml, resume, ATTRS[i], tmp, sizeof(tmp), &np, &af))
                continue;
            size_t off = (size_t)(np - xhtml);
            if (off < best_off) {
                best_off = off;
                memcpy(cand, tmp, sizeof(cand));
                next_resume = af;
            }
        }

        if (best_off == (size_t)-1 || !next_resume)
            break;

        uri_decode_inplace(cand);
        if (!is_remote_or_data_url(cand) && looks_like_local_image_path(cand)) {
            path_resolve(base, cand, img_zip_path, cap);
            if (img_zip_path[0])
                return true;
        }

        resume = next_resume;
    }

    char from_srcset[512];
    if (try_srcset_scan(xhtml, from_srcset, sizeof(from_srcset)) &&
        !is_remote_or_data_url(from_srcset)) {
        uri_decode_inplace(from_srcset);
        path_resolve(base, from_srcset, img_zip_path, cap);
        if (img_zip_path[0])
            return true;
    }

    char from_style[512];
    if (try_style_url_scan(xhtml, from_style, sizeof(from_style)) &&
        !is_remote_or_data_url(from_style)) {
        path_resolve(base, from_style, img_zip_path, cap);
        if (img_zip_path[0])
            return true;
    }

    return false;
}

static bool parse_opf(const char* opf_text, const char* opf_zip_path) {
    char opf_dir[512];
    strncpy(opf_dir, opf_zip_path, sizeof(opf_dir) - 1);
    opf_dir[sizeof(opf_dir) - 1] = '\0';
    dirname_inplace(opf_dir);

    s_man_n = 0;
    int man_cap = 64;
    s_man = (ManItem*)calloc((size_t)man_cap, sizeof(ManItem));
    if (!s_man) return false;

    const char* p = opf_text;
    while ((p = strstr(p, "<item")) != NULL) {
        const char* end = strchr(p, '>');
        if (!end) break;
        size_t bl = (size_t)(end - p + 1);
        if (bl > 4096) {
            p++;
            continue;
        }
        char block[4097];
        memcpy(block, p, bl);
        block[bl] = '\0';

        ManItem m;
        memset(&m, 0, sizeof(m));
        if (!extract_attr(block, "id", m.id, sizeof(m.id)))
            m.id[0] = '\0';
        if (!extract_attr(block, "href", m.href, sizeof(m.href))) {
            p = end + 1;
            continue;
        }
        extract_attr(block, "media-type", m.media, sizeof(m.media));

        if (s_man_n >= man_cap) {
            man_cap *= 2;
            ManItem* nm = (ManItem*)realloc(s_man, (size_t)man_cap * sizeof(ManItem));
            if (!nm) {
                free(s_man);
                s_man = NULL;
                return false;
            }
            s_man = nm;
        }
        s_man[s_man_n++] = m;
        p = end + 1;
    }

    int spine_cap = 256;
    s_spine = (char**)calloc((size_t)spine_cap, sizeof(char*));
    if (!s_spine) return false;
    s_spine_n = 0;

    p = opf_text;
    while ((p = strstr(p, "<itemref")) != NULL) {
        const char* end = strchr(p, '>');
        if (!end) break;
        size_t bl = (size_t)(end - p + 1);
        if (bl > 2048) {
            p++;
            continue;
        }
        char block[2049];
        memcpy(block, p, bl);
        block[bl] = '\0';

        char idref[96];
        if (!extract_attr(block, "idref", idref, sizeof(idref))) {
            p = end + 1;
            continue;
        }
        char linear[16];
        if (extract_attr(block, "linear", linear, sizeof(linear)) &&
            strcasecmp(linear, "no") == 0) {
            p = end + 1;
            continue;
        }
        ManItem* m = man_find(idref);
        if (!m || !is_page_candidate(m)) {
            p = end + 1;
            continue;
        }

        char full[512];
        path_resolve(opf_dir, m->href, full, sizeof(full));

        if (s_spine_n >= spine_cap) {
            spine_cap *= 2;
            char** ns = (char**)realloc(s_spine, (size_t)spine_cap * sizeof(char*));
            if (!ns) return false;
            s_spine = ns;
        }
        s_spine[s_spine_n] = strdup(full);
        if (!s_spine[s_spine_n]) return false;
        s_spine_n++;
        p = end + 1;
    }

    return s_spine_n > 0;
}

bool epub_reader_open_chapter(const char* base_url, const char* token, int chapter_id,
                               int* pages_inout) {
    epub_reader_close();

    char url[512];
    kavita_chapter_download_url(base_url, chapter_id, url, sizeof(url));

    HttpResponse* resp = http_get_binary(url, token);
    if (!resp || resp->status_code != 200 || !resp->data || resp->size == 0) {
        dlog("[epub] download fail status=%d", resp ? resp->status_code : -1);
        http_response_free(resp);
        return false;
    }

    s_outer_buf = (u8*)resp->data;
    s_outer_sz = resp->size;
    resp->data = NULL;
    resp->size = 0;
    http_response_free(resp);

    ZipBlob blob = { s_outer_buf, s_outer_sz };
    ZipArchive* outer = NULL;
    if (!zip_open(&blob, &outer)) {
        dlog("[epub] outer zip_open fail");
        epub_reader_close();
        return false;
    }

    if (zip_exists_ci(outer, "META-INF/container.xml")) {
        /* Chapter payload is the EPUB zip itself; reuse outer archive handle. */
        s_zip = outer;
        outer = NULL;
        s_inner_buf = NULL;
        s_inner_sz = 0;
    } else {
        u8* epub_bytes = NULL;
        size_t epub_len = 0;
        char epub_path[512];
        if (!zip_find_suffix_ci(outer, ".epub", epub_path, sizeof(epub_path))) {
            dlog("[epub] no epub in chapter zip");
            zip_close(outer);
            epub_reader_close();
            return false;
        }
        epub_bytes = zip_read_file(outer, epub_path, &epub_len);
        zip_close(outer);
        outer = NULL;
        free(s_outer_buf);
        s_outer_buf = NULL;
        s_outer_sz = 0;
        if (!epub_bytes) {
            dlog("[epub] extract inner epub fail");
            epub_reader_close();
            return false;
        }
        s_inner_buf = epub_bytes;
        s_inner_sz = epub_len;

        ZipBlob eb = { epub_bytes, epub_len };
        if (!zip_open(&eb, &s_zip)) {
            dlog("[epub] epub zip_open fail");
            epub_reader_close();
            return false;
        }
    }

    size_t ctn_sz = 0;
    u8* ctn = zip_read_file(s_zip, "META-INF/container.xml", &ctn_sz);
    if (!ctn)
        ctn = zip_read_file(s_zip, "meta-inf/container.xml", &ctn_sz);
    if (!ctn) {
        dlog("[epub] no container.xml");
        epub_reader_close();
        return false;
    }
    char* ctnz = (char*)malloc(ctn_sz + 1);
    if (!ctnz) {
        free(ctn);
        epub_reader_close();
        return false;
    }
    memcpy(ctnz, ctn, ctn_sz);
    ctnz[ctn_sz] = '\0';
    free(ctn);
    const char* xml = skip_xml_decl(ctnz);
    const char* opf_rel = find_full_path_container(xml);
    free(ctnz);
    if (!opf_rel) {
        dlog("[epub] container full-path missing");
        epub_reader_close();
        return false;
    }

    char opf_zip_path[512];
    norm_slashes(strncpy(opf_zip_path, opf_rel, sizeof(opf_zip_path) - 1));
    opf_zip_path[sizeof(opf_zip_path) - 1] = '\0';

    size_t opf_sz = 0;
    u8* opf_raw = zip_read_file(s_zip, opf_zip_path, &opf_sz);
    if (!opf_raw) {
        dlog("[epub] cannot read opf %s", opf_zip_path);
        epub_reader_close();
        return false;
    }
    char* opf = (char*)malloc(opf_sz + 1);
    if (!opf) {
        free(opf_raw);
        epub_reader_close();
        return false;
    }
    memcpy(opf, opf_raw, opf_sz);
    opf[opf_sz] = '\0';
    free(opf_raw);

    if (!parse_opf(opf, opf_zip_path)) {
        dlog("[epub] opf parse / empty spine");
        free(opf);
        epub_reader_close();
        return false;
    }
    free(opf);

    if (pages_inout && *pages_inout > 0 && *pages_inout != s_spine_n) {
        dlog("[epub] spine pages=%d kavita said=%d (using spine)", s_spine_n, *pages_inout);
    }
    if (pages_inout)
        *pages_inout = s_spine_n;

    s_open = true;
    return true;
}

#define EPUB_TEXT_MAX 32000

static const char* strstr_ci(const char* hay, const char* needle) {
    if (!needle || !needle[0])
        return hay;
    size_t nl = strlen(needle);
    for (const char* h = hay; *h; h++) {
        if (strncasecmp(h, needle, nl) == 0)
            return h;
    }
    return NULL;
}

/* Strip tags/script/style and decode a few entities → UTF-8 plain text. */
static char* xhtml_to_plaintext(const char* html) {
    char* out = (char*)malloc(EPUB_TEXT_MAX);
    if (!out)
        return NULL;

    const char* p = html;
    char* w = out;
    char* end = out + EPUB_TEXT_MAX - 4;

    while (*p && w < end) {
        if (*p == '<') {
            if (strncasecmp(p + 1, "script", 6) == 0) {
                const char* c = strstr_ci(p + 2, "</script>");
                if (!c) {
                    p++;
                    continue;
                }
                p = c + (int)strlen("</script>");
                if (w > out && w[-1] != ' ' && w[-1] != '\n')
                    *w++ = ' ';
                continue;
            }
            if (strncasecmp(p + 1, "style", 5) == 0) {
                const char* c = strstr_ci(p + 2, "</style>");
                if (!c) {
                    p++;
                    continue;
                }
                p = c + (int)strlen("</style>");
                if (w > out && w[-1] != ' ' && w[-1] != '\n')
                    *w++ = ' ';
                continue;
            }
            const char* gt = strchr(p, '>');
            if (!gt)
                break;
            p = gt + 1;
            if (w > out && w[-1] != ' ' && w[-1] != '\n')
                *w++ = ' ';
            continue;
        }

        if (*p == '&') {
            if (!strncmp(p + 1, "nbsp;", 5)) {
                *w++ = ' ';
                p += 6;
                continue;
            }
            if (!strncmp(p + 1, "amp;", 4)) {
                *w++ = '&';
                p += 5;
                continue;
            }
            if (!strncmp(p + 1, "lt;", 3)) {
                *w++ = '<';
                p += 4;
                continue;
            }
            if (!strncmp(p + 1, "gt;", 3)) {
                *w++ = '>';
                p += 4;
                continue;
            }
            if (!strncmp(p + 1, "quot;", 5)) {
                *w++ = '"';
                p += 6;
                continue;
            }
            if (!strncmp(p + 1, "apos;", 5)) {
                *w++ = '\'';
                p += 6;
                continue;
            }
            if (p[1] == '#') {
                const char* q = p + 2;
                unsigned cp = 0;
                if (*q == 'x' || *q == 'X') {
                    q++;
                    while (*q && hex_digit((unsigned char)*q) >= 0)
                        cp = cp * 16 + (unsigned)hex_digit((unsigned char)*q++);
                } else {
                    while (*q && isdigit((unsigned char)*q))
                        cp = cp * 10 + (unsigned)(*q++ - '0');
                }
                if (*q == ';') {
                    if (cp < 0x80) {
                        *w++ = (char)cp;
                    } else if (cp < 0x800) {
                        *w++ = (char)(0xC0 | (cp >> 6));
                        *w++ = (char)(0x80 | (cp & 0x3F));
                    } else if (cp < 0x10000) {
                        *w++ = (char)(0xE0 | (cp >> 12));
                        *w++ = (char)(0x80 | ((cp >> 6) & 0x3F));
                        *w++ = (char)(0x80 | (cp & 0x3F));
                    }
                    p = q + 1;
                    continue;
                }
            }
        }

        *w++ = *p++;
    }
    *w = '\0';

    char* r = out;
    char* wr = out;
    bool sp = false;
    while (*r) {
        if (isspace((unsigned char)*r)) {
            if (!sp) {
                *wr++ = ' ';
                sp = true;
            }
            r++;
        } else {
            *wr++ = *r++;
            sp = false;
        }
    }
    *wr = '\0';

    while (out[0] == ' ')
        memmove(out, out + 1, strlen(out));
    size_t n = strlen(out);
    while (n > 0 && out[n - 1] == ' ')
        out[--n] = '\0';

    return out;
}

static size_t plaintext_sig_chars(const char* s) {
    size_t n = 0;
    for (; *s; s++) {
        if (!isspace((unsigned char)*s))
            n++;
    }
    return n;
}

bool epub_reader_get_page_payload(int page_index, EpubPageKind* kind,
                                   u8** image_data, size_t* image_size,
                                   char** text_utf8) {
    *image_data = NULL;
    *image_size = 0;
    *text_utf8  = NULL;
    if (!kind)
        return false;
    if (!s_open || !s_zip || page_index < 0 || page_index >= s_spine_n)
        return false;

    const char* zip_path = s_spine[page_index];
    size_t zs = 0;
    u8* data = zip_read_file(s_zip, zip_path, &zs);
    if (!data) {
        dlog("[epub] missing spine file %s", zip_path);
        return false;
    }

    bool is_markup = false;
    if (zs >= 3 && data[0] == 0xef && data[1] == 0xbb && data[2] == 0xbf)
        is_markup = true;
    else if (zs > 0 && data[0] == '<')
        is_markup = true;

    if (!is_markup) {
        *kind = EPUB_PAGE_IMAGE;
        *image_data = data;
        *image_size = zs;
        return true;
    }

    char* xs = (char*)malloc(zs + 1);
    if (!xs) {
        free(data);
        return false;
    }
    memcpy(xs, data, zs);
    xs[zs] = '\0';
    free(data);

    char img_path[512];
    if (xhtml_first_image(xs, zip_path, img_path, sizeof(img_path))) {
        u8* img = zip_read_file(s_zip, img_path, image_size);
        free(xs);
        if (!img) {
            dlog("[epub] missing image %s", img_path);
            return false;
        }
        *kind = EPUB_PAGE_IMAGE;
        *image_data = img;
        return true;
    }

    char* plain = xhtml_to_plaintext(xs);
    free(xs);
    if (!plain)
        return false;
    if (plaintext_sig_chars(plain) < 4u) {
        free(plain);
        dlog("[epub] empty text page %s", zip_path);
        return false;
    }

    *kind = EPUB_PAGE_TEXT;
    *text_utf8 = plain;
    return true;
}
