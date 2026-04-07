/*
 * http_client.c — raw-socket HTTP/1.1 + mbedTLS HTTPS client
 *
 * TCP layer: BSD sockets via the 3DS SOC service (socInit'd in main.c).
 * TLS layer: mbedTLS, cert verification disabled (self-signed / LAN servers).
 *
 * Supports http:// and https://.  Handles redirects and chunked encoding.
 */

#include "http_client.h"
#include "debug_log.h"

#include <3ds.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <unistd.h>
#include <errno.h>

#include <mbedtls/ssl.h>
#include <mbedtls/entropy.h>
#include <mbedtls/ctr_drbg.h>
#include <mbedtls/error.h>
#include <mbedtls/net_sockets.h>

#define MAX_REDIRECTS    5
#define RECV_CHUNK       4096
#define LOG_BODY_PREVIEW 256

static bool s_initialised = false;

void http_client_init(void)  { s_initialised = true;  }
void http_client_fini(void)  { s_initialised = false; }

/* ------------------------------------------------------------------ */
/* Connection abstraction (plain or TLS)                               */
/* ------------------------------------------------------------------ */

typedef struct {
    bool is_tls;
    int  sock;                          /* plain HTTP socket */
    /* TLS */
    mbedtls_ssl_context      ssl;
    mbedtls_ssl_config       conf;
    mbedtls_ctr_drbg_context ctr_drbg;
    mbedtls_entropy_context  entropy;
} Conn;

/* mbedTLS bio callbacks that forward to our raw socket */
static int tls_send(void* ctx, const unsigned char* buf, size_t len) {
    int sock = *(int*)ctx;
    int n = send(sock, (const char*)buf, (int)len, 0);
    if (n < 0) return MBEDTLS_ERR_NET_SEND_FAILED;
    return n;
}
static int tls_recv(void* ctx, unsigned char* buf, size_t len) {
    int sock = *(int*)ctx;
    int n = recv(sock, (char*)buf, (int)len, 0);
    if (n < 0) return MBEDTLS_ERR_NET_RECV_FAILED;
    if (n == 0) return MBEDTLS_ERR_NET_CONN_RESET;
    return n;
}

/* inet_addr only parses dotted IPv4; hostnames need DNS (SOCU:gethostbyname). */
static bool resolve_ipv4(const char* host, struct in_addr* out) {
    if (inet_aton(host, out))
        return true;
    struct hostent* he = gethostbyname(host);
    if (!he || he->h_addrtype != AF_INET || !he->h_addr_list[0]) {
        dlog("[ERR] DNS: could not resolve '%s'", host);
        return false;
    }
    memcpy(out, he->h_addr_list[0], sizeof(struct in_addr));
    return true;
}

static bool conn_open(Conn* c, const char* host, int port, bool tls) {
    c->is_tls = tls;
    c->sock   = -1;

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port   = htons((u16)port);
    if (!resolve_ipv4(host, &addr.sin_addr))
        return false;

    /* --- TCP connect (shared by both modes) --- */
    c->sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (c->sock < 0) {
        dlog("[ERR] socket() errno=%d", errno);
        return false;
    }

    if (connect(c->sock, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        dlog("[ERR] connect() errno=%d", errno);
        closesocket(c->sock);
        c->sock = -1;
        return false;
    }

    if (!tls) return true;

    /* --- TLS handshake via mbedTLS --- */
    mbedtls_ssl_init(&c->ssl);
    mbedtls_ssl_config_init(&c->conf);
    mbedtls_ctr_drbg_init(&c->ctr_drbg);
    mbedtls_entropy_init(&c->entropy);

    int ret = mbedtls_ctr_drbg_seed(&c->ctr_drbg, mbedtls_entropy_func,
                                     &c->entropy, NULL, 0);
    if (ret != 0) {
        dlog("[ERR] mbedtls_ctr_drbg_seed: -0x%04X", (unsigned)-ret);
        goto tls_fail;
    }

    ret = mbedtls_ssl_config_defaults(&c->conf,
                                       MBEDTLS_SSL_IS_CLIENT,
                                       MBEDTLS_SSL_TRANSPORT_STREAM,
                                       MBEDTLS_SSL_PRESET_DEFAULT);
    if (ret != 0) {
        dlog("[ERR] mbedtls_ssl_config_defaults: -0x%04X", (unsigned)-ret);
        goto tls_fail;
    }

    /* Skip certificate verification — self-signed / LAN servers */
    mbedtls_ssl_conf_authmode(&c->conf, MBEDTLS_SSL_VERIFY_NONE);
    mbedtls_ssl_conf_rng(&c->conf, mbedtls_ctr_drbg_random, &c->ctr_drbg);

    ret = mbedtls_ssl_setup(&c->ssl, &c->conf);
    if (ret != 0) {
        dlog("[ERR] mbedtls_ssl_setup: -0x%04X", (unsigned)-ret);
        goto tls_fail;
    }

    mbedtls_ssl_set_hostname(&c->ssl, host);
    mbedtls_ssl_set_bio(&c->ssl, &c->sock, tls_send, tls_recv, NULL);

    dlog("[TLS] starting handshake with %s:%d", host, port);
    while ((ret = mbedtls_ssl_handshake(&c->ssl)) != 0) {
        if (ret != MBEDTLS_ERR_SSL_WANT_READ &&
            ret != MBEDTLS_ERR_SSL_WANT_WRITE) {
            dlog("[ERR] TLS handshake: -0x%04X", (unsigned)-ret);
            goto tls_fail;
        }
    }
    dlog("[TLS] handshake OK");
    return true;

tls_fail:
    mbedtls_ssl_free(&c->ssl);
    mbedtls_ssl_config_free(&c->conf);
    mbedtls_ctr_drbg_free(&c->ctr_drbg);
    mbedtls_entropy_free(&c->entropy);
    closesocket(c->sock);
    c->sock = -1;
    return false;
}

static bool conn_send_all(Conn* c, const char* buf, size_t len) {
    size_t sent = 0;
    while (sent < len) {
        int n;
        if (c->is_tls) {
            n = mbedtls_ssl_write(&c->ssl,
                                   (const unsigned char*)(buf + sent),
                                   len - sent);
            if (n == MBEDTLS_ERR_SSL_WANT_WRITE) continue;
        } else {
            n = send(c->sock, buf + sent, (int)(len - sent), 0);
        }
        if (n <= 0) {
            dlog("[ERR] conn_send_all: n=%d errno=%d", n, errno);
            return false;
        }
        sent += (size_t)n;
    }
    return true;
}

static int conn_recv(Conn* c, char* buf, size_t len) {
    if (c->is_tls) {
        int n;
        do {
            n = mbedtls_ssl_read(&c->ssl, (unsigned char*)buf, len);
        } while (n == MBEDTLS_ERR_SSL_WANT_READ);
        if (n == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY) return 0;
        return n;
    }
    return recv(c->sock, buf, (int)len, 0);
}

static void conn_close(Conn* c) {
    if (c->is_tls) {
        mbedtls_ssl_close_notify(&c->ssl);
        mbedtls_ssl_free(&c->ssl);
        mbedtls_ssl_config_free(&c->conf);
        mbedtls_ctr_drbg_free(&c->ctr_drbg);
        mbedtls_entropy_free(&c->entropy);
    }
    if (c->sock >= 0) {
        closesocket(c->sock);
        c->sock = -1;
    }
}

/* ------------------------------------------------------------------ */
/* Helpers                                                              */
/* ------------------------------------------------------------------ */

/* Case-insensitive substring search (strcasestr is a GNU extension). */
static const char* istrstr(const char* hay, const char* needle) {
    size_t nlen = strlen(needle);
    size_t hlen = strlen(hay);
    if (nlen > hlen) return NULL;
    for (size_t i = 0; i <= hlen - nlen; i++) {
        size_t j;
        for (j = 0; j < nlen; j++) {
            char hc = hay[i + j], nc = needle[j];
            if (hc >= 'A' && hc <= 'Z') hc += 32;
            if (nc >= 'A' && nc <= 'Z') nc += 32;
            if (hc != nc) break;
        }
        if (j == nlen) return hay + i;
    }
    return NULL;
}

/* Parse "http[s]://host[:port]/path" into components. */
static bool parse_url(const char* url,
                       bool* tls_out,
                       char* host, size_t host_sz,
                       int*  port,
                       char* path, size_t path_sz) {
    const char* p;
    if (strncmp(url, "https://", 8) == 0) {
        *tls_out = true;
        *port    = 443;
        p        = url + 8;
    } else if (strncmp(url, "http://", 7) == 0) {
        *tls_out = false;
        *port    = 80;
        p        = url + 7;
    } else {
        dlog("[ERR] unsupported scheme: %s", url);
        return false;
    }

    const char* slash = strchr(p, '/');
    const char* colon = strchr(p, ':');
    if (colon && slash && colon > slash) colon = NULL;

    if (colon) {
        size_t hlen = (size_t)(colon - p);
        if (hlen >= host_sz) return false;
        memcpy(host, p, hlen);
        host[hlen] = '\0';
        *port = atoi(colon + 1);
    } else {
        size_t hlen = slash ? (size_t)(slash - p) : strlen(p);
        if (hlen >= host_sz) return false;
        memcpy(host, p, hlen);
        host[hlen] = '\0';
    }

    strncpy(path, slash ? slash : "/", path_sz - 1);
    path[path_sz - 1] = '\0';
    return true;
}

/* Decode chunked transfer encoding in-place; returns decoded length. */
static size_t decode_chunked(char* buf, size_t len) {
    char* src = buf, *dst = buf, *end = buf + len;
    while (src < end) {
        char* nl = memchr(src, '\n', (size_t)(end - src));
        if (!nl) break;
        unsigned long csz = strtoul(src, NULL, 16);
        src = nl + 1;
        if (csz == 0) break;
        if (src + csz > end) csz = (unsigned long)(end - src);
        memmove(dst, src, csz);
        dst += csz;
        src += csz;
        if (src < end && *src == '\r') src++;
        if (src < end && *src == '\n') src++;
    }
    *dst = '\0';
    return (size_t)(dst - buf);
}

/* ------------------------------------------------------------------ */
/* Core request                                                         */
/* ------------------------------------------------------------------ */

static HttpResponse* do_request(const char* url,
                                 const char* token,
                                 const char* method,
                                 const char* json_body,
                                 const char* accept) {
    char current_url[1024];
    strncpy(current_url, url, sizeof(current_url) - 1);
    current_url[sizeof(current_url) - 1] = '\0';

    dlog("[HTTP] %s %s", method, current_url);
    if (json_body)
        dlog("[HTTP] body: %.200s", json_body);

    int redirects = 0;

    while (redirects < MAX_REDIRECTS) {
        bool tls  = false;
        char host[256] = {0};
        int  port      = 80;
        char path[1024] = {0};

        if (!parse_url(current_url, &tls, host, sizeof(host), &port, path, sizeof(path)))
            return NULL;

        dlog("[HTTP] %s %s:%d%s", tls ? "TLS" : "plain", host, port, path);

        Conn conn;
        if (!conn_open(&conn, host, port, tls))
            return NULL;

        /* ---- send request ---- */
        char line[1024];
        int  n;

#define SEND(s, l) do { if (!conn_send_all(&conn, (s), (l))) { conn_close(&conn); return NULL; } } while(0)
#define SENDL(s)   SEND((s), strlen(s))

        n = snprintf(line, sizeof(line), "%s %s HTTP/1.1\r\n", method, path);
        SEND(line, (size_t)n);

        n = (port == 80 || (tls && port == 443))
              ? snprintf(line, sizeof(line), "Host: %s\r\n", host)
              : snprintf(line, sizeof(line), "Host: %s:%d\r\n", host, port);
        SEND(line, (size_t)n);

        SENDL("User-Agent: kavita-3ds/1.0\r\n");
        if (accept && accept[0]) {
            n = snprintf(line, sizeof(line), "Accept: %s\r\n", accept);
            SEND(line, (size_t)n);
        } else {
            SENDL("Accept: application/json\r\n");
        }
        SENDL("Accept-Encoding: identity\r\n");
        SENDL("Connection: close\r\n");

        if (token && token[0]) {
            SENDL("Authorization: Bearer ");
            SENDL(token);
            SENDL("\r\n");
        }

        size_t body_len = json_body ? strlen(json_body) : 0;
        if (json_body && body_len > 0) {
            n = snprintf(line, sizeof(line),
                         "Content-Type: application/json\r\n"
                         "Content-Length: %u\r\n",
                         (unsigned)body_len);
            SEND(line, (size_t)n);
            dlog("[HTTP] Content-Length: %u", (unsigned)body_len);
        }

        SENDL("\r\n");

        if (json_body && body_len > 0)
            SEND(json_body, body_len);

#undef SEND
#undef SENDL

        /* ---- read response ---- */
        size_t capacity = RECV_CHUNK * 2;
        char*  buf      = (char*)malloc(capacity + 1);
        if (!buf) {
            dlog("[ERR] malloc failed");
            conn_close(&conn);
            return NULL;
        }

        size_t received = 0;
        int    r;
        while ((r = conn_recv(&conn, buf + received, RECV_CHUNK)) > 0) {
            received += (size_t)r;
            if (received + RECV_CHUNK > capacity) {
                capacity += RECV_CHUNK * 4;
                char* tmp = (char*)realloc(buf, capacity + 1);
                if (!tmp) {
                    dlog("[ERR] realloc failed");
                    free(buf);
                    conn_close(&conn);
                    return NULL;
                }
                buf = tmp;
            }
        }
        buf[received] = '\0';
        conn_close(&conn);

        dlog("[HTTP] received %u bytes", (unsigned)received);

        if (received == 0) {
            dlog("[ERR] server sent 0 bytes");
            free(buf);
            return NULL;
        }

        /* ---- parse status ---- */
        int status_code = 0;
        if (sscanf(buf, "HTTP/%*s %d", &status_code) != 1) {
            dlog("[ERR] bad status line: %.80s", buf);
            free(buf);
            return NULL;
        }
        dlog("[HTTP] status: %d", status_code);

        /* ---- find body ---- */
        char* hdr_end = strstr(buf, "\r\n\r\n");
        if (!hdr_end) {
            dlog("[ERR] no CRLFCRLF");
            free(buf);
            return NULL;
        }
        char*  body_start = hdr_end + 4;
        size_t body_bytes = received - (size_t)(body_start - buf);

        /* ---- redirect? ---- */
        if (status_code == 301 || status_code == 302 || status_code == 303 ||
            status_code == 307 || status_code == 308) {
            const char* loc = istrstr(buf, "\r\nLocation:");
            if (loc) {
                loc += 11;
                while (*loc == ' ' || *loc == '\t') loc++;
                const char* loc_end = strstr(loc, "\r\n");
                if (loc_end) {
                    size_t llen = (size_t)(loc_end - loc);
                    if (llen < sizeof(current_url)) {
                        memcpy(current_url, loc, llen);
                        current_url[llen] = '\0';
                        dlog("[HTTP] redirect -> %s", current_url);
                        free(buf);
                        redirects++;
                        continue;
                    }
                }
            }
            dlog("[ERR] redirect without Location");
            free(buf);
            return NULL;
        }

        /* ---- chunked decode ---- */
        if (istrstr(buf, "\r\nTransfer-Encoding: chunked") ||
            istrstr(buf, "\r\ntransfer-encoding: chunked"))
            body_bytes = decode_chunked(body_start, body_bytes);

        if (body_bytes > 0)
            dlog("[HTTP] body(%u bytes): %.*s",
                 (unsigned)body_bytes, LOG_BODY_PREVIEW, body_start);
        else
            dlog("[HTTP] empty body");

        memmove(buf, body_start, body_bytes);
        buf[body_bytes] = '\0';

        HttpResponse* resp = (HttpResponse*)malloc(sizeof(HttpResponse));
        if (!resp) { free(buf); return NULL; }
        resp->data        = buf;
        resp->size        = body_bytes;
        resp->status_code = status_code;
        return resp;
    }

    dlog("[ERR] too many redirects");
    return NULL;
}

/* ------------------------------------------------------------------ */
/* Public API                                                           */
/* ------------------------------------------------------------------ */

HttpResponse* http_get(const char* url, const char* token) {
    return do_request(url, token, "GET", NULL, "application/json");
}

HttpResponse* http_get_binary(const char* url, const char* token) {
    return do_request(url, token, "GET", NULL, "*/*");
}

HttpResponse* http_post_json(const char* url, const char* token,
                              const char* json_body) {
    return do_request(url, token, "POST", json_body, "application/json");
}

void http_response_free(HttpResponse* r) {
    if (!r) return;
    free(r->data);
    free(r);
}
