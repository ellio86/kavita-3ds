#pragma once

#include <stddef.h>
#include <stdbool.h>

/* ------------------------------------------------------------------ */
/* HTTP response                                                        */
/* ------------------------------------------------------------------ */
typedef struct {
    char*  data;       /* heap-allocated response body (always NUL-terminated) */
    size_t size;       /* byte count (excluding NUL terminator) */
    int    status_code;
} HttpResponse;

/* ------------------------------------------------------------------ */
/* API                                                                  */
/* ------------------------------------------------------------------ */

/* Must be called once after socInit/httpcInit. Safe to call multiple times. */
void http_client_init(void);
void http_client_fini(void);

/* Perform a GET request.
   token: if non-NULL, adds Authorization: Bearer header.
   Returns NULL on error. Caller must call http_response_free(). */
HttpResponse* http_get(const char* url, const char* token);

/* GET for binary bodies (Accept: any); used for chapter archive download. */
HttpResponse* http_get_binary(const char* url, const char* token);

/* Perform a POST request with JSON body.
   token: if non-NULL, adds Authorization: Bearer header.
   Returns NULL on error. Caller must call http_response_free(). */
HttpResponse* http_post_json(const char* url, const char* token,
                              const char* json_body);

/* Free a response returned by http_get / http_post_json. */
void http_response_free(HttpResponse* r);
