#include "kavita_api.h"
#include "http_client.h"
#include "debug_log.h"
#include "cJSON.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <strings.h>

/* Kavita uses large negatives (e.g. -100000) in sort keys — not for display. */
static bool chapter_ord_sane(double x) {
    return x >= -500.0 && x <= 1.0e7;
}

static void chapter_sanitize_title(char* title, size_t title_sz) {
    if (!title || !title[0]) return;
    char* end = NULL;
    float v = strtof(title, &end);
    if (end && *end == '\0' && !chapter_ord_sane((double)v))
        title[0] = '\0';
    (void)title_sz;
}

/* Kavita often puts sort keys in range/title for specials; sanitizing would blank
 * them. File path is a reliable fallback when API title fields are empty. */
static void chapter_title_from_filepath(const char* path, char* out, size_t out_sz) {
    if (!path || !out || out_sz == 0) return;
    const char* base = path;
    for (const char* p = path; *p; p++) {
        if (*p == '/' || *p == '\\')
            base = p + 1;
    }
    if (!base[0]) return;

    size_t n = 0;
    while (base[n] && n + 1 < out_sz)
        n++;
    if (n == 0) return;

    memcpy(out, base, n);
    out[n] = '\0';

    static const char* const exts[] = {
        ".cbz", ".cbr", ".zip", ".rar", ".pdf", ".epub", ".7z"
    };
    for (size_t e = 0; e < sizeof(exts) / sizeof(exts[0]); e++) {
        size_t el = strlen(exts[e]);
        if (n > el && strcasecmp(out + n - el, exts[e]) == 0) {
            out[n - el] = '\0';
            break;
        }
    }
}

/* ------------------------------------------------------------------ */
/* API key (required for Image + Reader/image URL query params) */
/* ------------------------------------------------------------------ */

static void copy_api_key_from_auth_keys_array(cJSON* arr,
                                              char* out, size_t out_sz) {
    if (!out || out_sz == 0) return;
    out[0] = '\0';
    if (!cJSON_IsArray(arr)) return;

    cJSON* ak;
    cJSON_ArrayForEach(ak, arr) {
        cJSON* key = cJSON_GetObjectItemCaseSensitive(ak, "key");
        if (cJSON_IsString(key) && key->valuestring && key->valuestring[0]) {
            strncpy(out, key->valuestring, out_sz - 1);
            out[out_sz - 1] = '\0';
            return;
        }
    }
}

static bool opds_url_extract_api_key(const char* url, char* out, size_t out_sz) {
    if (!url || !out || out_sz == 0) return false;

    static const char* const markers[] = { "/api/Opds/", "/api/opds/" };
    const char* p = NULL;
    for (size_t i = 0; i < sizeof(markers) / sizeof(markers[0]); i++) {
        p = strstr(url, markers[i]);
        if (p) {
            p += strlen(markers[i]);
            break;
        }
    }
    if (!p || !*p) return false;

    size_t j = 0;
    while (*p && *p != '/' && *p != '?' && *p != '"' && j + 1 < out_sz)
        out[j++] = *p++;
    out[j] = '\0';
    return j > 0;
}

/* Fills out if login JSON omitted apiKey (common on current Kavita). */
static void kavita_fetch_api_key_remote(const char* base_url, const char* token,
                                        char* out, size_t out_sz) {
    if (!out || out_sz == 0 || !token || !token[0]) return;
    out[0] = '\0';

    char url[512];
    snprintf(url, sizeof(url), "%s/api/Account/auth-keys", base_url);
    HttpResponse* resp = http_get(url, token);
    if (resp && resp->status_code == 200 && resp->data) {
        cJSON* root = cJSON_Parse(resp->data);
        if (root) {
            copy_api_key_from_auth_keys_array(root, out, out_sz);
            cJSON_Delete(root);
        }
    }
    http_response_free(resp);

    if (out[0]) {
        dlog("[OK] api key from GET /api/Account/auth-keys");
        return;
    }

    snprintf(url, sizeof(url), "%s/api/Account/opds-url", base_url);
    resp = http_get(url, token);
    if (resp && resp->status_code == 200 && resp->data) {
        const char* urlstr = NULL;
        cJSON* j = cJSON_Parse(resp->data);
        if (cJSON_IsString(j))
            urlstr = j->valuestring;
        cJSON_Delete(j);
        if (!urlstr && resp->data[0] == 'h')
            urlstr = resp->data; /* plain URL body */
        if (urlstr && opds_url_extract_api_key(urlstr, out, out_sz))
            dlog("[OK] api key parsed from GET /api/Account/opds-url");
    }
    http_response_free(resp);
}

/* ------------------------------------------------------------------ */
/* Authentication                                                       */
/* ------------------------------------------------------------------ */
bool kavita_login(const char* base_url,
                  const char* username,
                  const char* password,
                  char* token_out, size_t token_sz,
                  char* api_key_out, size_t api_key_sz) {
    char url[512];
    snprintf(url, sizeof(url), "%s/api/Account/login", base_url);
    dlog("[API] login url='%s' user='%s'", url, username);

    /* Build JSON body */
    cJSON* body = cJSON_CreateObject();
    cJSON_AddStringToObject(body, "username", username);
    cJSON_AddStringToObject(body, "password", password);
    char* body_str = cJSON_PrintUnformatted(body);
    cJSON_Delete(body);
    if (!body_str) {
        dlog("[ERR] cJSON failed to serialise login body");
        return false;
    }

    HttpResponse* resp = http_post_json(url, NULL, body_str);
    free(body_str);

    if (!resp) {
        dlog("[ERR] login: http_post_json returned NULL (network error)");
        return false;
    }

    dlog("[API] login HTTP status: %d", resp->status_code);

    if (resp->status_code != 200) {
        dlog("[ERR] login failed: HTTP %d  body: %.200s",
             resp->status_code, resp->data ? resp->data : "(null)");
        http_response_free(resp);
        return false;
    }

    cJSON* root = cJSON_Parse(resp->data);
    http_response_free(resp);
    if (!root) {
        dlog("[ERR] login: failed to parse JSON response");
        return false;
    }

    if (api_key_out && api_key_sz)
        api_key_out[0] = '\0';

    bool ok = false;

    cJSON* token_item = cJSON_GetObjectItemCaseSensitive(root, "token");
    if (cJSON_IsString(token_item) && token_item->valuestring) {
        strncpy(token_out, token_item->valuestring, token_sz - 1);
        token_out[token_sz - 1] = '\0';
        ok = true;
        dlog("[OK] login: got token (len=%u)", (unsigned)strlen(token_out));
    } else {
        dlog("[ERR] login: 'token' field missing or not a string in response");
        /* Log all top-level keys to diagnose the response structure */
        cJSON* item = root->child;
        while (item) {
            dlog("[API]   response key: '%s'", item->string ? item->string : "?");
            item = item->next;
        }
    }

    /* apiKey: root field, or authKeys[] from UserDto (current Kavita) */
    if (api_key_out && api_key_sz) {
        cJSON* api_key_item = cJSON_GetObjectItemCaseSensitive(root, "apiKey");
        if (cJSON_IsString(api_key_item) && api_key_item->valuestring &&
            api_key_item->valuestring[0]) {
            strncpy(api_key_out, api_key_item->valuestring, api_key_sz - 1);
            api_key_out[api_key_sz - 1] = '\0';
            dlog("[OK] login: got apiKey");
        } else {
            cJSON* auth_keys = cJSON_GetObjectItemCaseSensitive(root, "authKeys");
            copy_api_key_from_auth_keys_array(auth_keys, api_key_out, api_key_sz);
            if (api_key_out[0])
                dlog("[OK] login: got api key from authKeys");
            else
                dlog("[API] login: no apiKey/authKeys key string in response");
        }
    }

    cJSON_Delete(root);

    if (ok && api_key_out && api_key_sz && !api_key_out[0])
        kavita_fetch_api_key_remote(base_url, token_out, api_key_out, api_key_sz);

    if (ok && api_key_out && api_key_sz && !api_key_out[0])
        dlog("[API] warning: no api key — covers and reader images may fail");

    return ok;
}

/* ------------------------------------------------------------------ */
/* Libraries                                                            */
/* ------------------------------------------------------------------ */
int kavita_get_libraries(const char* base_url, const char* token,
                          KavitaLibrary* buf, int max_count) {
    char url[512];
    snprintf(url, sizeof(url), "%s/api/Library/libraries", base_url);

    HttpResponse* resp = http_get(url, token);
    if (!resp || resp->status_code != 200) {
        http_response_free(resp);
        return -1;
    }

    cJSON* root = cJSON_Parse(resp->data);
    http_response_free(resp);
    if (!root || !cJSON_IsArray(root)) {
        cJSON_Delete(root);
        return -1;
    }

    int count = 0;
    cJSON* item;
    cJSON_ArrayForEach(item, root) {
        if (count >= max_count) break;
        KavitaLibrary* lib = &buf[count];
        memset(lib, 0, sizeof(*lib));

        cJSON* id   = cJSON_GetObjectItemCaseSensitive(item, "id");
        cJSON* name = cJSON_GetObjectItemCaseSensitive(item, "name");
        cJSON* type = cJSON_GetObjectItemCaseSensitive(item, "type");

        if (cJSON_IsNumber(id))   lib->id   = id->valueint;
        if (cJSON_IsString(name)) strncpy(lib->name, name->valuestring,
                                           sizeof(lib->name) - 1);
        if (cJSON_IsNumber(type)) lib->type = type->valueint;

        count++;
    }

    cJSON_Delete(root);
    return count;
}

/* ------------------------------------------------------------------ */
/* Series                                                               */
/* ------------------------------------------------------------------ */
int kavita_get_series(const char* base_url, const char* token,
                       int library_id, int page, int page_size,
                       KavitaSeries* buf, int max_count,
                       int* out_total) {
    char url[768];
    /* Pagination: query params pageNumber + pageSize (UserParams), same as the
     * official web UI (see SeriesService.getAllSeriesV2). Include context=1
     * (QueryContext.None) like the Angular client. Body must use FilterV2Dto
     * field names from the API docs (sortOptions, not sorting). */
    snprintf(url, sizeof(url),
             "%s/api/Series/all-v2?context=1&pageNumber=%d&pageSize=%d",
             base_url, page, page_size);

    /* Build filter body */
    char lib_id_str[32];
    snprintf(lib_id_str, sizeof(lib_id_str), "%d", library_id);

    cJSON* body       = cJSON_CreateObject();
    cJSON* statements = cJSON_AddArrayToObject(body, "statements");
    cJSON* stmt       = cJSON_CreateObject();
    cJSON_AddNumberToObject(stmt, "comparison", 0);  /* Equal */
    cJSON_AddNumberToObject(stmt, "field", 19);       /* LibraryId field */
    cJSON_AddStringToObject(stmt, "value", lib_id_str);
    cJSON_AddItemToArray(statements, stmt);

    cJSON_AddNumberToObject(body, "combination", 1);  /* AND */

    cJSON* sort_opts = cJSON_AddObjectToObject(body, "sortOptions");
    cJSON_AddNumberToObject(sort_opts, "sortField", 1);   /* SortField.Name */
    cJSON_AddBoolToObject(sort_opts, "isAscending", 1);

    char* body_str = cJSON_PrintUnformatted(body);
    cJSON_Delete(body);
    if (!body_str) return -1;

    HttpResponse* resp = http_post_json(url, token, body_str);
    free(body_str);

    if (!resp || resp->status_code != 200) {
        http_response_free(resp);
        return -1;
    }

    cJSON* root = cJSON_Parse(resp->data);
    http_response_free(resp);
    if (!root) return -1;

    /* Kavita returns a bare SeriesDto[]; older builds used { items, totalCount }. */
    cJSON* items = NULL;
    if (cJSON_IsArray(root)) {
        items = root;
        /* Bare array has no totalCount; never guess "+1" or clients think there
         * is an extra item and request a bogus next page. Use exact page length;
         * callers that need the full library should paginate until n < page_size. */
        if (out_total) {
            int n = cJSON_GetArraySize(items);
            *out_total = (page - 1) * page_size + n;
        }
    } else {
        if (out_total) {
            cJSON* total = cJSON_GetObjectItemCaseSensitive(root, "totalCount");
            *out_total = cJSON_IsNumber(total) ? total->valueint : 0;
        }
        items = cJSON_GetObjectItemCaseSensitive(root, "items");
    }

    if (!items || !cJSON_IsArray(items)) {
        cJSON_Delete(root);
        return -1;
    }

    int count = 0;
    cJSON* item;
    cJSON_ArrayForEach(item, items) {
        if (count >= max_count) break;
        KavitaSeries* s = &buf[count];
        memset(s, 0, sizeof(*s));

        cJSON* id         = cJSON_GetObjectItemCaseSensitive(item, "id");
        cJSON* name       = cJSON_GetObjectItemCaseSensitive(item, "name");
        cJSON* local_title= cJSON_GetObjectItemCaseSensitive(item, "localizedName");
        cJSON* lib_id     = cJSON_GetObjectItemCaseSensitive(item, "libraryId");
        cJSON* pages_read = cJSON_GetObjectItemCaseSensitive(item, "pagesRead");
        cJSON* pages      = cJSON_GetObjectItemCaseSensitive(item, "pages");

        if (cJSON_IsNumber(id))        s->id         = id->valueint;
        if (cJSON_IsString(name))      strncpy(s->name, name->valuestring,
                                                sizeof(s->name) - 1);
        if (cJSON_IsString(local_title)) strncpy(s->local_title,
                                                  local_title->valuestring,
                                                  sizeof(s->local_title) - 1);
        if (cJSON_IsNumber(lib_id))    s->library_id = lib_id->valueint;
        if (cJSON_IsNumber(pages_read))s->pages_read = pages_read->valueint;
        if (cJSON_IsNumber(pages))     s->pages_total= pages->valueint;

        count++;
    }

    cJSON_Delete(root);
    return count;
}

/* ------------------------------------------------------------------ */
/* Series detail                                                        */
/* ------------------------------------------------------------------ */
bool kavita_get_series_detail(const char* base_url, const char* token,
                               int series_id, KavitaSeriesDetail* out) {
    memset(out, 0, sizeof(*out));

    char url[512];
    snprintf(url, sizeof(url), "%s/api/Series/volumes?seriesId=%d",
             base_url, series_id);

    HttpResponse* resp = http_get(url, token);
    if (!resp || resp->status_code != 200) {
        http_response_free(resp);
        return false;
    }

    cJSON* root = cJSON_Parse(resp->data);
    http_response_free(resp);
    if (!root || !cJSON_IsArray(root)) {
        cJSON_Delete(root);
        return false;
    }

    cJSON* vol_item;
    cJSON_ArrayForEach(vol_item, root) {
        if (out->volume_count >= 64) break;

        KavitaVolume* v = &out->volumes[out->volume_count];
        memset(v, 0, sizeof(*v));

        cJSON* vid   = cJSON_GetObjectItemCaseSensitive(vol_item, "id");
        cJSON* vnum  = cJSON_GetObjectItemCaseSensitive(vol_item, "number");
        cJSON* vmin  = cJSON_GetObjectItemCaseSensitive(vol_item, "minNumber");
        cJSON* vname = cJSON_GetObjectItemCaseSensitive(vol_item, "name");

        if (cJSON_IsNumber(vid))  v->id     = vid->valueint;
        if (cJSON_IsNumber(vnum)) v->number = vnum->valueint;
        else if (cJSON_IsNumber(vmin))
            v->number = (int)vmin->valuedouble;
        if (cJSON_IsString(vname))
            strncpy(v->range, vname->valuestring, sizeof(v->range) - 1);
        else
            snprintf(v->range, sizeof(v->range), "Vol. %d", v->number);

        int ch_start = out->chapter_count;

        /* Parse chapters within this volume */
        cJSON* chapters = cJSON_GetObjectItemCaseSensitive(vol_item, "chapters");
        if (chapters && cJSON_IsArray(chapters)) {
        cJSON* ch;
        cJSON_ArrayForEach(ch, chapters) {
            if (out->chapter_count >= 256) break;
            KavitaChapter* c = &out->chapters[out->chapter_count];
            memset(c, 0, sizeof(*c));

            cJSON* cid       = cJSON_GetObjectItemCaseSensitive(ch, "id");
            cJSON* ctitle    = cJSON_GetObjectItemCaseSensitive(ch, "title");
            cJSON* ctitleNm  = cJSON_GetObjectItemCaseSensitive(ch, "titleName");
            cJSON* crange    = cJSON_GetObjectItemCaseSensitive(ch, "range");
            cJSON* cnum      = cJSON_GetObjectItemCaseSensitive(ch, "number");
            cJSON* cminnum   = cJSON_GetObjectItemCaseSensitive(ch, "minNumber");
            cJSON* csort     = cJSON_GetObjectItemCaseSensitive(ch, "sortOrder");
            cJSON* cpages    = cJSON_GetObjectItemCaseSensitive(ch, "pages");
            cJSON* cpread    = cJSON_GetObjectItemCaseSensitive(ch, "pagesRead");
            cJSON* cispec    = cJSON_GetObjectItemCaseSensitive(ch, "isSpecial");

            c->volume_id = v->id;
            c->is_special = cJSON_IsBool(cispec) && cJSON_IsTrue(cispec);
            c->is_epub    = false;
            if (cJSON_IsNumber(cid))    c->id         = cid->valueint;
            /* Prefer titleName, then title, then range (Kavita ChapterDto). */
            if (cJSON_IsString(ctitleNm) && ctitleNm->valuestring)
                strncpy(c->title, ctitleNm->valuestring, sizeof(c->title) - 1);
            else if (cJSON_IsString(ctitle) && ctitle->valuestring)
                strncpy(c->title, ctitle->valuestring, sizeof(c->title) - 1);
            else if (cJSON_IsString(crange) && crange->valuestring)
                strncpy(c->title, crange->valuestring, sizeof(c->title) - 1);
            /* Do not strip numeric "titles" for specials — Kavita uses sort keys
             * (e.g. large negatives) in range; sanitize would clear the only label. */
            if (!c->is_special)
                chapter_sanitize_title(c->title, sizeof(c->title));
            if (!c->title[0] && c->is_special) {
                cJSON* cfiles = cJSON_GetObjectItemCaseSensitive(ch, "files");
                if (cJSON_IsArray(cfiles) && cJSON_GetArraySize(cfiles) > 0) {
                    cJSON* f0 = cJSON_GetArrayItem(cfiles, 0);
                    if (f0) {
                        cJSON* fpath =
                            cJSON_GetObjectItemCaseSensitive(f0, "filePath");
                        if (cJSON_IsString(fpath) && fpath->valuestring)
                            chapter_title_from_filepath(fpath->valuestring,
                                                        c->title,
                                                        sizeof(c->title));
                    }
                }
            }

            {
                cJSON* cfiles = cJSON_GetObjectItemCaseSensitive(ch, "files");
                if (cJSON_IsArray(cfiles) && cJSON_GetArraySize(cfiles) > 0) {
                    cJSON* f0 = cJSON_GetArrayItem(cfiles, 0);
                    if (f0) {
                        cJSON* ext =
                            cJSON_GetObjectItemCaseSensitive(f0, "extension");
                        if (cJSON_IsString(ext) && ext->valuestring) {
                            const char* e = ext->valuestring;
                            if (e[0] == '.') e++;
                            if (strcasecmp(e, "epub") == 0)
                                c->is_epub = true;
                        }
                        if (!c->is_epub) {
                            cJSON* fpath =
                                cJSON_GetObjectItemCaseSensitive(f0, "filePath");
                            if (cJSON_IsString(fpath) && fpath->valuestring) {
                                const char* fp = fpath->valuestring;
                                size_t n = strlen(fp);
                                if (n > 5 && strcasecmp(fp + n - 5, ".epub") == 0)
                                    c->is_epub = true;
                            }
                        }
                    }
                }
            }

            /* Display number: prefer minNumber / issue number over sortOrder.
             * Kavita often sends sortOrder 0 when unset; taking sortOrder first
             * made every chapter show as 0 (see ChapterDto in Kavita openapi). */
            {
                float ord  = 0.f;
                bool  have = false;
                double d;

                if (cJSON_IsNumber(cminnum)) {
                    d = cminnum->valuedouble;
                    if (chapter_ord_sane(d)) {
                        ord  = (float)d;
                        have = true;
                    }
                }
                if (!have && cJSON_IsString(cnum) && cnum->valuestring &&
                    cnum->valuestring[0]) {
                    d = strtod(cnum->valuestring, NULL);
                    if (chapter_ord_sane(d)) {
                        ord  = (float)d;
                        have = true;
                    }
                }
                if (!have && cJSON_IsNumber(cnum)) {
                    d = cnum->valuedouble;
                    if (chapter_ord_sane(d)) {
                        ord  = (float)d;
                        have = true;
                    }
                }
                if (!have && cJSON_IsNumber(csort)) {
                    d = csort->valuedouble;
                    if (chapter_ord_sane(d))
                        ord = (float)d;
                }
                c->number = ord;
            }
            if (cJSON_IsNumber(cpages)) c->pages      = cpages->valueint;
            if (cJSON_IsNumber(cpread)) c->pages_read = cpread->valueint;

            out->chapter_count++;
        }
        }

        v->first_chapter      = ch_start;
        v->chapters_in_volume = out->chapter_count - ch_start;
        out->volume_count++;
    }

    cJSON_Delete(root);
    return true;
}

/* ------------------------------------------------------------------ */
/* Reading progress                                                     */
/* ------------------------------------------------------------------ */
bool kavita_save_progress(const char* base_url, const char* token,
                           int library_id, int series_id, int volume_id,
                           int chapter_id, int page_num) {
    char url[512];
    snprintf(url, sizeof(url), "%s/api/reader/progress", base_url);

    cJSON* body = cJSON_CreateObject();
    cJSON_AddNumberToObject(body, "libraryId", library_id);
    cJSON_AddNumberToObject(body, "volumeId",  volume_id);
    cJSON_AddNumberToObject(body, "chapterId", chapter_id);
    cJSON_AddNumberToObject(body, "pageNum",   page_num);
    cJSON_AddNumberToObject(body, "seriesId",  series_id);
    char* body_str = cJSON_PrintUnformatted(body);
    cJSON_Delete(body);
    if (!body_str) return false;

    HttpResponse* resp = http_post_json(url, token, body_str);
    free(body_str);

    bool ok = resp && (resp->status_code == 200 || resp->status_code == 204);
    http_response_free(resp);
    return ok;
}

/* ------------------------------------------------------------------ */
/* URL construction                                                     */
/* ------------------------------------------------------------------ */
void kavita_cover_url(const char* base_url, const char* api_key,
                       int series_id, char* buf, size_t sz) {
    snprintf(buf, sz, "%s/api/Image/series-cover?seriesId=%d&apiKey=%s",
             base_url, series_id, api_key ? api_key : "");
}

void kavita_volume_cover_url(const char* base_url, const char* api_key,
                               int volume_id, char* buf, size_t sz) {
    snprintf(buf, sz, "%s/api/Image/volume-cover?volumeId=%d&apiKey=%s",
             base_url, volume_id, api_key ? api_key : "");
}

void kavita_chapter_cover_url(const char* base_url, const char* api_key,
                               int chapter_id, char* buf, size_t sz) {
    snprintf(buf, sz, "%s/api/Image/chapter-cover?chapterId=%d&apiKey=%s",
             base_url, chapter_id, api_key ? api_key : "");
}

void kavita_chapter_download_url(const char* base_url, int chapter_id,
                                  char* buf, size_t sz) {
    snprintf(buf, sz, "%s/api/Download/chapter?chapterId=%d",
             base_url, chapter_id);
}

void kavita_page_url(const char* base_url, const char* api_key,
                      int chapter_id, int page, char* buf, size_t sz) {
    /* extractPdf=true: PDF chapters are rasterized server-side; without it the
     * endpoint returns raw PDF bytes, which stb_image cannot decode. Ignored
     * for CBZ/CBR/image archives (Kavita OpenAPI). */
    snprintf(buf, sz,
             "%s/api/Reader/image?chapterId=%d&page=%d&extractPdf=true&apiKey=%s",
             base_url, chapter_id, page, api_key ? api_key : "");
}

bool kavita_get_chapter_info_pages(const char* base_url, const char* token,
                                    int chapter_id, int* pages_out) {
    if (!pages_out) return false;

    char url[512];
    snprintf(url, sizeof(url),
             "%s/api/Reader/chapter-info?chapterId=%d&extractPdf=true",
             base_url, chapter_id);

    HttpResponse* resp = http_get(url, token);
    if (!resp || resp->status_code != 200 || !resp->data) {
        http_response_free(resp);
        return false;
    }

    cJSON* root = cJSON_Parse(resp->data);
    http_response_free(resp);
    if (!root) return false;

    cJSON* pages = cJSON_GetObjectItemCaseSensitive(root, "pages");
    bool ok = false;
    if (cJSON_IsNumber(pages) && pages->valueint > 0) {
        *pages_out = pages->valueint;
        ok = true;
    }
    cJSON_Delete(root);
    return ok;
}
