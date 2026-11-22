// SoulCore - HTTP for programs: WinINet, WinHTTP and URLDownloadToFile.
//
// A request is performed in one go when the program sends it: the host (the iPad app, through
// SoulLaunchOptions.http and URLSession) writes the response body to a temporary file, which the
// program then reads. Without a host callback a small built-in HTTP/1.1 client handles http://.
#include "hle.h"
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <poll.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#define CSTR(n) ((const char *)G2H(hle_arg(st, (n))))
#define WSTR(n) ((const uint16_t *)G2H(hle_arg(st, (n))))
#define FAIL(err) do { set_last_error(st, (err)); RET(0); return; } while (0)

enum { E_TIMEOUT = 12002, E_INVALID_URL = 12005, E_BAD_SCHEME = 12006, E_NAME = 12007, E_HANDLE = 12017,
       E_INCORRECT_HANDLE_TYPE = 12018, E_CANNOT_CONNECT = 12029, E_RESET = 12031, E_HEADER_NOT_FOUND = 12150,
       E_SECURE = 12157, E_INSUFFICIENT = 122 };

static char *gstr(uint64_t p, int wide) {
    if (!p) return NULL;
    return wide ? w2u_dup((const uint16_t *)G2H(p)) : strdup((const char *)G2H(p));
}
static char *gstr_n(uint64_t p, long n, int wide) { // n < 0: NUL-terminated
    if (!p) return NULL;
    if (n < 0 || n == (long)0xFFFFFFFF) return gstr(p, wide);
    if (wide) { char *s = malloc((size_t)n * 3 + 1); size_t k = w2u((const uint16_t *)G2H(p), n, s, (size_t)n * 3 + 1); s[k] = 0; return s; }
    char *s = malloc((size_t)n + 1);
    memcpy(s, G2H(p), (size_t)n);
    s[n] = 0;
    return s;
}

// ---- URLs ------------------------------------------------------------------------------------------------

typedef struct Url { char scheme[16], host[256], user[128], pass[128], path[2048]; int port; } Url;
static int parse_url(const char *u, Url *o) {
    memset(o, 0, sizeof *o);
    const char *p = strstr(u, "://");
    if (!p || p - u >= (long)sizeof o->scheme) return -1;
    snprintf(o->scheme, sizeof o->scheme, "%.*s", (int)(p - u), u);
    for (char *c = o->scheme; *c; c++) *c = (char)tolower((unsigned char)*c);
    p += 3;
    const char *end = p + strcspn(p, "/?#");
    const char *at = memchr(p, '@', (size_t)(end - p));
    if (at) {
        const char *colon = memchr(p, ':', (size_t)(at - p));
        if (colon) { snprintf(o->user, sizeof o->user, "%.*s", (int)(colon - p), p); snprintf(o->pass, sizeof o->pass, "%.*s", (int)(at - colon - 1), colon + 1); }
        else snprintf(o->user, sizeof o->user, "%.*s", (int)(at - p), p);
        p = at + 1;
    }
    const char *hs = p, *he = end;
    if (*hs == '[') { const char *rb = memchr(hs, ']', (size_t)(end - hs)); if (rb) { hs++; he = rb; p = rb + 1; } }
    else { const char *colon = memchr(p, ':', (size_t)(end - p)); he = colon ? colon : end; p = he; }
    snprintf(o->host, sizeof o->host, "%.*s", (int)(he - hs), hs);
    o->port = !strcmp(o->scheme, "https") ? 443 : !strcmp(o->scheme, "ftp") ? 21 : 80;
    if (*p == ':') o->port = atoi(p + 1);
    snprintf(o->path, sizeof o->path, "%s", *end ? end : "/");
    if (o->path[0] != '/') { char t[2048]; snprintf(t, sizeof t, "/%s", o->path); snprintf(o->path, sizeof o->path, "%s", t); }
    return o->host[0] ? 0 : -1;
}

// ---- the built-in HTTP/1.1 client (plain http only) -------------------------------------------------------

static int read_line(int fd, char *buf, size_t cap, uint8_t *pend, size_t *plen) {
    size_t n = 0;
    for (;;) {
        if (*plen == 0) {
            struct pollfd pf = { fd, POLLIN, 0 };
            if (poll(&pf, 1, 30000) <= 0) return -1;
            ssize_t r = recv(fd, pend, 8192, 0);
            if (r <= 0) return n ? (int)n : -1;
            *plen = (size_t)r;
        }
        uint8_t c = pend[0];
        memmove(pend, pend + 1, --*plen);
        if (c == '\n') { if (n && buf[n - 1] == '\r') n--; buf[n] = 0; return (int)n; }
        if (n + 1 < cap) buf[n++] = (char)c;
    }
}
static ssize_t read_some(int fd, uint8_t *buf, size_t cap, uint8_t *pend, size_t *plen) {
    if (*plen) { size_t k = *plen < cap ? *plen : cap; memcpy(buf, pend, k); memmove(pend, pend + k, *plen - k); *plen -= k; return (ssize_t)k; }
    struct pollfd pf = { fd, POLLIN, 0 };
    if (poll(&pf, 1, 30000) <= 0) return -1;
    return recv(fd, buf, cap, 0);
}
static int builtin_http(const SoulHttpRequest *rq, SoulHttpResponse *rs) {
    char url[2048];
    snprintf(url, sizeof url, "%s", rq->url);
    for (int hop = 0; hop < 6; hop++) {
        Url u;
        if (parse_url(url, &u) != 0) { snprintf(rs->error, sizeof rs->error, "geçersiz adres"); return -1; }
        if (strcmp(u.scheme, "http") != 0) { snprintf(rs->error, sizeof rs->error, "%s bu yapıda desteklenmiyor (yalnızca http)", u.scheme); return -2; }
        struct addrinfo hints, *res = NULL;
        memset(&hints, 0, sizeof hints);
        hints.ai_socktype = SOCK_STREAM;
        char port[16];
        snprintf(port, sizeof port, "%d", u.port);
        if (getaddrinfo(u.host, port, &hints, &res) != 0 || !res) { snprintf(rs->error, sizeof rs->error, "ad çözülemedi: %s", u.host); return -3; }
        int fd = -1;
        for (struct addrinfo *a = res; a && fd < 0; a = a->ai_next) {
            fd = socket(a->ai_family, a->ai_socktype, a->ai_protocol);
            if (fd >= 0 && connect(fd, a->ai_addr, a->ai_addrlen) != 0) { close(fd); fd = -1; }
        }
        freeaddrinfo(res);
        if (fd < 0) { snprintf(rs->error, sizeof rs->error, "bağlanılamadı: %s", u.host); return -4; }
#ifdef SO_NOSIGPIPE
        int one = 1;
        setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
#endif
        char head[8192];
        int hl = snprintf(head, sizeof head, "%s %s HTTP/1.1\r\nHost: %s", rq->method, u.path, u.host);
        if (u.port != 80) hl += snprintf(head + hl, sizeof head - (size_t)hl, ":%d", u.port);
        hl += snprintf(head + hl, sizeof head - (size_t)hl, "\r\nConnection: close\r\n");
        if (!rq->headers || !strcasestr(rq->headers, "User-Agent:")) hl += snprintf(head + hl, sizeof head - (size_t)hl, "User-Agent: Mozilla/5.0 (Windows NT 10.0; Win64; x64) Soul\r\n");
        if (rq->headers) hl += snprintf(head + hl, sizeof head - (size_t)hl, "%s", rq->headers);
        if (hl > 0 && head[hl - 1] != '\n') hl += snprintf(head + hl, sizeof head - (size_t)hl, "\r\n");
        if (rq->body_len || !strcasecmp(rq->method, "POST") || !strcasecmp(rq->method, "PUT"))
            hl += snprintf(head + hl, sizeof head - (size_t)hl, "Content-Length: %zu\r\n", rq->body_len);
        hl += snprintf(head + hl, sizeof head - (size_t)hl, "\r\n");
        int flags = 0;
#ifdef MSG_NOSIGNAL
        flags = MSG_NOSIGNAL;
#endif
        if (send(fd, head, (size_t)hl, flags) != hl || (rq->body_len && send(fd, rq->body, rq->body_len, flags) != (ssize_t)rq->body_len)) {
            close(fd); snprintf(rs->error, sizeof rs->error, "istek gönderilemedi"); return -5;
        }
        uint8_t *pend = malloc(8192);
        size_t plen = 0;
        char line[8192];
        if (read_line(fd, line, sizeof line, pend, &plen) < 0 || strncmp(line, "HTTP/", 5) != 0) {
            free(pend); close(fd); snprintf(rs->error, sizeof rs->error, "yanıt yok"); return -6;
        }
        rs->status = atoi(strchr(line, ' ') ? strchr(line, ' ') + 1 : "0");
        rs->headers[0] = 0;
        size_t ho = 0;
        long clen = -1;
        int chunked = 0;
        char location[2048] = "";
        while (read_line(fd, line, sizeof line, pend, &plen) > 0) {
            ho += (size_t)snprintf(rs->headers + ho, sizeof rs->headers - ho, "%s\r\n", line);
            if (ho >= sizeof rs->headers) ho = sizeof rs->headers - 1;
            if (!strncasecmp(line, "Content-Length:", 15)) clen = atol(line + 15);
            else if (!strncasecmp(line, "Transfer-Encoding:", 18) && strcasestr(line, "chunked")) chunked = 1;
            else if (!strncasecmp(line, "Location:", 9)) { const char *v = line + 9; while (*v == ' ') v++; snprintf(location, sizeof location, "%s", v); }
        }
        int redirect = (rs->status == 301 || rs->status == 302 || rs->status == 303 || rs->status == 307 || rs->status == 308) && location[0];
        FILE *out = redirect ? NULL : fopen(rq->body_path, "wb");
        uint8_t buf[16384];
        if (!redirect && strcasecmp(rq->method, "HEAD") != 0 && rs->status != 204 && rs->status != 304) {
            if (chunked) {
                for (;;) {
                    if (read_line(fd, line, sizeof line, pend, &plen) < 0) break;
                    long n = strtol(line, NULL, 16);
                    if (n <= 0) break;
                    while (n > 0) {
                        ssize_t r = read_some(fd, buf, (size_t)(n < (long)sizeof buf ? n : (long)sizeof buf), pend, &plen);
                        if (r <= 0) { n = 0; break; }
                        if (out) fwrite(buf, 1, (size_t)r, out);
                        n -= r;
                    }
                    read_line(fd, line, sizeof line, pend, &plen); // CRLF after the chunk
                }
            } else {
                long left = clen;
                while (left != 0) {
                    size_t want = left < 0 || left > (long)sizeof buf ? sizeof buf : (size_t)left;
                    ssize_t r = read_some(fd, buf, want, pend, &plen);
                    if (r <= 0) break;
                    if (out) fwrite(buf, 1, (size_t)r, out);
                    if (left > 0) left -= r;
                }
            }
        }
        if (out) fclose(out);
        free(pend);
        close(fd);
        snprintf(rs->final_url, sizeof rs->final_url, "%s", url);
        if (!redirect) return 0;
        if (location[0] == '/') { char t[2048]; snprintf(t, sizeof t, "%s://%s:%d%s", u.scheme, u.host, u.port, location); snprintf(url, sizeof url, "%s", t); }
        else snprintf(url, sizeof url, "%s", location);
    }
    snprintf(rs->error, sizeof rs->error, "çok fazla yönlendirme");
    return -7;
}

// Performs a request without the global lock. Returns 0 or a WinINet error code.
static uint32_t perform(const char *method, const char *url, const char *headers, const void *body, size_t body_len,
                        const char *body_path, SoulHttpResponse *rs) {
    memset(rs, 0, sizeof *rs);
    SoulHttpRequest rq = { method, url, headers ? headers : "", body, body_len, body_path, 60000 };
    SoulThread *t = soul_cur_thread();
    SoulHttpFn host = g_proc->opts.http;
    void *ctx = g_proc->opts.ctx;
    int verbose = g_proc->verbose;
    gil_unlock(t);
    int r = host ? host(ctx, &rq, rs) : builtin_http(&rq, rs);
    gil_lock(t);
    if (verbose) soul_log("[ağ] %s %s -> %d %s\n", method, url, rs->status, rs->error);
    if (r == 0 && rs->status > 0) return 0;
    if (r == -2) return E_SECURE;
    if (r == -3) return E_NAME;
    if (r == -1) return E_INVALID_URL;
    if (strstr(rs->error, "zaman") || strstr(rs->error, "timed out")) return E_TIMEOUT;
    return E_CANNOT_CONNECT;
}

// ---- handles ----------------------------------------------------------------------------------------------

enum { K_SESSION = 1, K_CONNECT, K_REQUEST };
typedef struct Inet {
    int used, kind;
    uint64_t parent;
    char agent[256];
    // connection
    char host[256], user[128], pass[128];
    int port, secure;
    // request
    char method[16], path[2048], *req_headers;
    uint8_t *body;
    size_t body_len, body_cap, total_len;
    int sent, status;
    char *resp_headers, final_url[2048], body_path[1100];
    int fd;
    uint64_t pos, size;
    uint32_t timeouts_ms;
} Inet;
#define INET_MAX 512
#define INET_BASE 0xCC0000u
static Inet g_inet[INET_MAX];

static Inet *inet_get(uint64_t h) {
    if (h < INET_BASE || h >= INET_BASE + INET_MAX * 4u || (h - INET_BASE) % 4) return NULL;
    Inet *o = &g_inet[(h - INET_BASE) / 4];
    return o->used ? o : NULL;
}
static uint64_t inet_new(int kind, Inet **out) {
    for (int i = 1; i < INET_MAX; i++) if (!g_inet[i].used) {
        memset(&g_inet[i], 0, sizeof g_inet[i]);
        g_inet[i].used = 1; g_inet[i].kind = kind; g_inet[i].fd = -1;
        *out = &g_inet[i];
        return INET_BASE + (uint64_t)i * 4;
    }
    *out = NULL;
    return 0;
}
static void inet_close(uint64_t h) {
    Inet *o = inet_get(h);
    if (!o) return;
    if (o->fd >= 0) close(o->fd);
    if (o->body_path[0]) unlink(o->body_path);
    free(o->req_headers); free(o->body); free(o->resp_headers);
    o->used = 0;
    for (int i = 0; i < INET_MAX; i++) if (g_inet[i].used && g_inet[i].parent == h) inet_close(INET_BASE + (uint64_t)i * 4);
}
static void temp_body_path(char *out, size_t cap) {
    static unsigned n;
    snprintf(out, cap, "%s/users/soul/Temp/soul-http-%d-%u.tmp", g_proc->drive_c, (int)getpid(), ++n);
}
static void add_headers(Inet *r, const char *h) {
    if (!h || !*h) return;
    size_t old = r->req_headers ? strlen(r->req_headers) : 0, n = strlen(h);
    r->req_headers = realloc(r->req_headers, old + n + 3);
    memcpy(r->req_headers + old, h, n + 1);
    if (n < 2 || strcmp(h + n - 2, "\r\n") != 0) strcat(r->req_headers, "\r\n");
}
// Sends the request and opens the body file. 0 or a WinINet error.
static uint32_t send_request(Inet *r) {
    Inet *c = inet_get(r->parent);
    if (!c) return E_HANDLE;
    char url[3000];
    const char *scheme = r->secure ? "https" : "http";
    int defport = r->secure ? 443 : 80;
    char auth[300] = "";
    if (c->user[0]) snprintf(auth, sizeof auth, "%s%s%s@", c->user, c->pass[0] ? ":" : "", c->pass);
    if (c->port && c->port != defport) snprintf(url, sizeof url, "%s://%s%s:%d%s", scheme, auth, c->host, c->port, r->path);
    else snprintf(url, sizeof url, "%s://%s%s%s", scheme, auth, c->host, r->path);
    if (!r->body_path[0]) temp_body_path(r->body_path, sizeof r->body_path);
    SoulHttpResponse *rs = malloc(sizeof *rs);
    uint32_t e = perform(r->method, url, r->req_headers, r->body, r->body_len, r->body_path, rs);
    if (!e) {
        r->status = rs->status;
        free(r->resp_headers);
        r->resp_headers = strdup(rs->headers);
        snprintf(r->final_url, sizeof r->final_url, "%s", rs->final_url[0] ? rs->final_url : url);
        if (r->fd >= 0) close(r->fd);
        r->fd = open(r->body_path, O_RDONLY);
        struct stat sb;
        r->size = r->fd >= 0 && fstat(r->fd, &sb) == 0 ? (uint64_t)sb.st_size : 0;
        r->pos = 0;
        r->sent = 1;
    }
    free(rs);
    return e;
}

// ---- header queries ------------------------------------------------------------------------------------------

static const char *query_name(uint32_t level) {
    switch (level & 0xFFFF) {
    case 1: return "Content-Type"; case 5: return "Content-Length"; case 9: return "Date"; case 10: return "Expires";
    case 11: return "Last-Modified"; case 29: return "Content-Encoding"; case 33: return "Location"; case 37: return "Server";
    case 42: return "Accept-Ranges"; case 43: return "Set-Cookie"; case 47: return "Content-Disposition"; case 54: return "ETag";
    case 70: return "Transfer-Encoding"; case 35: return "Connection"; case 3: return "Content-Transfer-Encoding";
    default: return NULL;
    }
}
// Fills out with the value; returns 1 found, 0 not found.
static int header_value(const char *all, const char *name, int index, char *out, size_t cap) {
    if (!all) return 0;
    size_t nl = strlen(name);
    int k = 0;
    for (const char *p = all; *p;) {
        const char *e = strstr(p, "\r\n");
        size_t l = e ? (size_t)(e - p) : strlen(p);
        if (l > nl && !strncasecmp(p, name, nl) && p[nl] == ':') {
            if (k++ == index) {
                const char *v = p + nl + 1;
                while (*v == ' ') v++;
                snprintf(out, cap, "%.*s", (int)(l - (size_t)(v - p)), v);
                return 1;
            }
        }
        p = e ? e + 2 : p + l;
    }
    return 0;
}
static int status_text(int code, char *out, size_t cap) {
    const char *t = code == 200 ? "OK" : code == 201 ? "Created" : code == 204 ? "No Content" : code == 206 ? "Partial Content"
                  : code == 301 ? "Moved Permanently" : code == 302 ? "Found" : code == 304 ? "Not Modified" : code == 400 ? "Bad Request"
                  : code == 401 ? "Unauthorized" : code == 403 ? "Forbidden" : code == 404 ? "Not Found" : code == 500 ? "Internal Server Error" : "";
    snprintf(out, cap, "%s", t);
    return 1;
}
// HttpQueryInfo / WinHttpQueryHeaders. buf/lenp: bytes. Returns 1 or 0 with the error set.
static int query_info(X86State *st, Inet *r, uint32_t level, const char *custom, uint64_t buf, uint64_t lenp, uint64_t indexp, int wide) {
    if (!r || r->kind != K_REQUEST) { set_last_error(st, E_INCORRECT_HANDLE_TYPE); return 0; }
    uint32_t code = level & 0xFFFF;
    int number = (level & 0x20000000) != 0, req = (level & 0x80000000) != 0;
    int index = indexp ? (int)mem_rd(indexp, 4) : 0;
    const char *src = req ? r->req_headers : r->resp_headers;
    char val[16384];
    int found = 0;
    if (code == 19) { snprintf(val, sizeof val, "%d", r->status); found = r->sent; }                       // STATUS_CODE
    else if (code == 20) found = status_text(r->status, val, sizeof val);                                   // STATUS_TEXT
    else if (code == 18) { snprintf(val, sizeof val, "HTTP/1.1"); found = 1; }                             // VERSION
    else if (code == 22 || code == 21) {                                                                    // RAW_HEADERS(_CRLF)
        char st_text[64];
        status_text(r->status, st_text, sizeof st_text);
        snprintf(val, sizeof val, "HTTP/1.1 %d %s\r\n%s\r\n", r->status, st_text, src ? src : "");
        found = 1;
        if (code == 21) for (char *c = val; *c; c++) if (*c == '\r') { memmove(c, c + 1, strlen(c)); } // NUL-separated below
    } else if (code == 65535 && custom) found = header_value(src, custom, index, val, sizeof val);        // CUSTOM
    else { const char *n = query_name(code); if (n) found = header_value(src, n, index, val, sizeof val); }
    if (!found) { set_last_error(st, E_HEADER_NOT_FOUND); return 0; }
    uint32_t cap = lenp ? (uint32_t)mem_rd(lenp, 4) : 0;
    if (number) {
        if (cap < 4) { mem_wr(lenp, 4, 4); set_last_error(st, E_INSUFFICIENT); return 0; }
        mem_wr(buf, 4, (uint32_t)strtoul(val, NULL, 10));
        mem_wr(lenp, 4, 4);
    } else {
        size_t units = wide ? u2w(val, -1, NULL, 0) : strlen(val);
        size_t bytes = (units + 1) * (wide ? 2 : 1);
        if (!buf || cap < bytes) { mem_wr(lenp, 4, (uint32_t)bytes); set_last_error(st, E_INSUFFICIENT); return 0; }
        if (wide) u2w(val, -1, (uint16_t *)G2H(buf), units + 1); else memcpy(G2H(buf), val, units + 1);
        if (code == 21) { for (size_t i = 0; i < units; i++) { uint32_t c = wide ? ((uint16_t *)G2H(buf))[i] : ((uint8_t *)G2H(buf))[i]; if (c == '\n') { if (wide) ((uint16_t *)G2H(buf))[i] = 0; else ((uint8_t *)G2H(buf))[i] = 0; } } }
        mem_wr(lenp, 4, (uint32_t)(units * (wide ? 2 : 1)));
    }
    if (indexp) mem_wr(indexp, 4, (uint32_t)index + 1);
    return 1;
}
static int read_body(Inet *r, void *buf, uint32_t n, uint32_t *got) {
    *got = 0;
    if (!r || r->kind != K_REQUEST || !r->sent) return 0;
    if (r->fd < 0 || !n) return 1;
    ssize_t k = read(r->fd, buf, n);
    if (k < 0) return 0;
    *got = (uint32_t)k;
    r->pos += (uint64_t)k;
    return 1;
}

// ---- WinINet --------------------------------------------------------------------------------------------------

static void internet_open(X86State *st, int wide) {
    Inet *o;
    uint64_t h = inet_new(K_SESSION, &o);
    if (!h) FAIL(ERROR_NOT_ENOUGH_MEMORY);
    char *a = gstr(ARG(0), wide);
    snprintf(o->agent, sizeof o->agent, "%s", a ? a : "");
    free(a);
    RET(h);
}
HLE(in_InternetOpenA) { internet_open(st, 0); }
HLE(in_InternetOpenW) { internet_open(st, 1); }
static void internet_connect(X86State *st, int wide) { // (session, server, port, user, pass, service, flags, ctx)
    if (!inet_get(ARG(0))) FAIL(E_HANDLE);
    if ((uint32_t)hle_arg(st, 5) == 1) FAIL(E_BAD_SCHEME); // FTP
    Inet *o;
    uint64_t h = inet_new(K_CONNECT, &o);
    if (!h) FAIL(ERROR_NOT_ENOUGH_MEMORY);
    o->parent = ARG(0);
    char *s = gstr(ARG(1), wide), *u = gstr(ARG(3), wide), *p = gstr(hle_arg(st, 4), wide);
    snprintf(o->host, sizeof o->host, "%s", s ? s : "");
    snprintf(o->user, sizeof o->user, "%s", u ? u : "");
    snprintf(o->pass, sizeof o->pass, "%s", p ? p : "");
    free(s); free(u); free(p);
    o->port = (int)(ARG(2) & 0xFFFF);
    RET(h);
}
HLE(in_InternetConnectA) { internet_connect(st, 0); }
HLE(in_InternetConnectW) { internet_connect(st, 1); }
static uint64_t open_request(uint64_t conn, const char *verb, const char *object, uint32_t flags, int winhttp) {
    Inet *c = inet_get(conn);
    if (!c || c->kind != K_CONNECT) return 0;
    Inet *o;
    uint64_t h = inet_new(K_REQUEST, &o);
    if (!h) return 0;
    o->parent = conn;
    snprintf(o->method, sizeof o->method, "%s", verb && *verb ? verb : "GET");
    snprintf(o->path, sizeof o->path, "%s%s", object && object[0] == '/' ? "" : "/", object ? object : "");
    o->secure = (flags & 0x00800000) != 0 || c->port == 443; // INTERNET_FLAG_SECURE / WINHTTP_FLAG_SECURE
    (void)winhttp;
    return h;
}
static void http_open_request(X86State *st, int wide) { // (connect, verb, object, version, referrer, accept, flags, ctx)
    char *v = gstr(ARG(1), wide), *ob = gstr(ARG(2), wide);
    uint64_t h = open_request(ARG(0), v, ob, (uint32_t)hle_arg(st, 6), 0);
    free(v); free(ob);
    if (!h) FAIL(E_HANDLE);
    RET(h);
}
HLE(in_HttpOpenRequestA) { http_open_request(st, 0); }
HLE(in_HttpOpenRequestW) { http_open_request(st, 1); }
static void http_add_headers(X86State *st, int wide) { // (req, headers, len, modifiers)
    Inet *r = inet_get(ARG(0));
    if (!r) FAIL(E_HANDLE);
    char *h = gstr_n(ARG(1), (long)(int32_t)ARG(2), wide);
    add_headers(r, h);
    free(h);
    RET(1);
}
HLE(in_HttpAddRequestHeadersA) { http_add_headers(st, 0); }
HLE(in_HttpAddRequestHeadersW) { http_add_headers(st, 1); }
static void http_send(X86State *st, int wide) { // (req, headers, len, optional, optlen)
    Inet *r = inet_get(ARG(0));
    if (!r || r->kind != K_REQUEST) FAIL(E_HANDLE);
    char *h = gstr_n(ARG(1), (long)(int32_t)ARG(2), wide);
    add_headers(r, h);
    free(h);
    uint32_t n = (uint32_t)hle_arg(st, 4);
    if (ARG(3) && n) { free(r->body); r->body = malloc(n); memcpy(r->body, PTR(3), n); r->body_len = n; }
    uint32_t e = send_request(r);
    if (e) FAIL(e);
    RET(1);
}
HLE(in_HttpSendRequestA) { http_send(st, 0); }
HLE(in_HttpSendRequestW) { http_send(st, 1); }
HLE(in_HttpSendRequestEx) { // (req, buffersIn, buffersOut, flags, ctx): the body comes with InternetWriteFile
    Inet *r = inet_get(ARG(0));
    if (!r) FAIL(E_HANDLE);
    r->body_len = 0;
    RET(1);
}
HLE(in_InternetWriteFile) { // (h, buf, n, &written)
    Inet *r = inet_get(ARG(0));
    if (!r) FAIL(E_HANDLE);
    uint32_t n = (uint32_t)ARG(2);
    if (r->body_len + n > r->body_cap) { r->body_cap = (r->body_len + n) * 2 + 1024; r->body = realloc(r->body, r->body_cap); }
    memcpy(r->body + r->body_len, PTR(1), n);
    r->body_len += n;
    if (ARG(3)) mem_wr(ARG(3), 4, n);
    RET(1);
}
HLE(in_HttpEndRequest) { Inet *r = inet_get(ARG(0)); if (!r) FAIL(E_HANDLE); uint32_t e = send_request(r); if (e) FAIL(e); RET(1); }
static void http_query(X86State *st, int wide) { // (req, level, buffer, &len, &index)
    Inet *r = inet_get(ARG(0));
    uint32_t level = (uint32_t)ARG(1);
    char *custom = (level & 0xFFFF) == 65535 ? gstr(ARG(2), wide) : NULL;
    int ok = query_info(st, r, level, custom, ARG(2), ARG(3), hle_arg(st, 4), wide);
    free(custom);
    RET(ok);
}
HLE(in_HttpQueryInfoA) { http_query(st, 0); }
HLE(in_HttpQueryInfoW) { http_query(st, 1); }
HLE(in_InternetReadFile) { // (h, buf, n, &read)
    Inet *r = inet_get(ARG(0));
    uint32_t got;
    if (!read_body(r, PTR(1), (uint32_t)ARG(2), &got)) FAIL(E_HANDLE);
    if (ARG(3)) mem_wr(ARG(3), 4, got);
    RET(1);
}
HLE(in_InternetReadFileEx) { // (h, INTERNET_BUFFERS*, flags, ctx): dwBufferLength at +PSZ*? (struct: size, next, header, hlen, htotal, buffer, len, total, lo, hi)
    Inet *r = inet_get(ARG(0));
    const char *L = "4PP44P4444";
    uint64_t ib = ARG(1);
    uint32_t got;
    if (!read_body(r, G2H(gs_get(ib, L, 5)), (uint32_t)gs_get(ib, L, 6), &got)) FAIL(E_HANDLE);
    gs_set(ib, L, 6, got);
    RET(1);
}
HLE(in_InternetQueryDataAvailable) {
    Inet *r = inet_get(ARG(0));
    if (!r || r->kind != K_REQUEST || !r->sent) FAIL(E_HANDLE);
    uint64_t left = r->size > r->pos ? r->size - r->pos : 0;
    if (ARG(1)) mem_wr(ARG(1), 4, left > 0x7FFFFFFF ? 0x7FFFFFFF : left);
    RET(1);
}
HLE(in_InternetSetFilePointer) { // (h, distance, &high, method, ctx)
    Inet *r = inet_get(ARG(0));
    if (!r || r->fd < 0) { RET((uint64_t)-1); return; }
    int64_t d = (int32_t)ARG(1);
    off_t at = lseek(r->fd, (off_t)d, ARG(3) == 1 ? SEEK_CUR : ARG(3) == 2 ? SEEK_END : SEEK_SET);
    r->pos = (uint64_t)at;
    RET((uint32_t)at);
}
static void internet_open_url(X86State *st, int wide) { // (session, url, headers, len, flags, ctx)
    if (!inet_get(ARG(0))) FAIL(E_HANDLE);
    char *u = gstr(ARG(1), wide);
    Url url;
    if (!u || parse_url(u, &url) != 0) { free(u); FAIL(E_INVALID_URL); }
    if (strcmp(url.scheme, "http") && strcmp(url.scheme, "https")) { free(u); FAIL(E_BAD_SCHEME); }
    Inet *c;
    uint64_t ch = inet_new(K_CONNECT, &c);
    if (!ch) { free(u); FAIL(ERROR_NOT_ENOUGH_MEMORY); }
    c->parent = ARG(0);
    snprintf(c->host, sizeof c->host, "%s", url.host);
    snprintf(c->user, sizeof c->user, "%s", url.user);
    snprintf(c->pass, sizeof c->pass, "%s", url.pass);
    c->port = url.port;
    uint64_t rh = open_request(ch, "GET", url.path, !strcmp(url.scheme, "https") ? 0x00800000 : 0, 0);
    Inet *r = inet_get(rh);
    if (!r) { inet_close(ch); free(u); FAIL(E_HANDLE); }
    r->secure = !strcmp(url.scheme, "https");
    char *h = gstr_n(ARG(2), (long)(int32_t)ARG(3), wide);
    add_headers(r, h);
    free(h);
    free(u);
    uint32_t e = send_request(r);
    if (e) { inet_close(ch); FAIL(e); }
    r->parent = ch;
    RET(rh);
}
HLE(in_InternetOpenUrlA) { internet_open_url(st, 0); }
HLE(in_InternetOpenUrlW) { internet_open_url(st, 1); }
HLE(in_InternetCloseHandle) { if (!inet_get(ARG(0))) FAIL(E_HANDLE); inet_close(ARG(0)); RET(1); }
HLE(in_True) { RET(1); }
HLE(in_Zero) { RET(0); }
HLE(in_InternetGetConnectedState) { if (ARG(0)) mem_wr(ARG(0), 4, 2 | 0x10); RET(1); } // LAN, configured
static void internet_query_option(X86State *st, int wide) { // (h, option, buffer, &len)
    Inet *r = inet_get(ARG(0));
    uint32_t opt = (uint32_t)ARG(1);
    uint64_t buf = ARG(2), lenp = ARG(3);
    uint32_t cap = lenp ? (uint32_t)mem_rd(lenp, 4) : 0;
    if (opt == 34 && r) { // INTERNET_OPTION_URL
        size_t units = wide ? u2w(r->final_url, -1, NULL, 0) : strlen(r->final_url);
        size_t bytes = (units + 1) * (wide ? 2 : 1);
        if (!buf || cap < bytes) { mem_wr(lenp, 4, (uint32_t)bytes); FAIL(E_INSUFFICIENT); }
        if (wide) u2w(r->final_url, -1, (uint16_t *)G2H(buf), units + 1); else memcpy(G2H(buf), r->final_url, units + 1);
        mem_wr(lenp, 4, (uint32_t)bytes);
        RET(1);
        return;
    }
    if (cap >= 4 && buf) { mem_wr(buf, 4, opt == 31 ? 0 : opt == 2 || opt == 5 || opt == 6 ? 60000 : 0); mem_wr(lenp, 4, 4); RET(1); return; }
    FAIL(E_INSUFFICIENT);
}
HLE(in_InternetQueryOptionA) { internet_query_option(st, 0); }
HLE(in_InternetQueryOptionW) { internet_query_option(st, 1); }

// URL_COMPONENTS: size, scheme, schemeLen, nScheme, host, hostLen, port(WORD), user, userLen, pass, passLen, path, pathLen, extra, extraLen
#define L_URLC "4P44P42P4P4P4P4"
static void crack_part(uint64_t uc, int pfield, int lfield, const char *full, const char *part, size_t plen, int wide) {
    uint64_t ptr = gs_get(uc, L_URLC, pfield);
    uint32_t len = (uint32_t)gs_get(uc, L_URLC, lfield);
    if (!ptr && !len) return;
    if (!ptr) { // point into the URL itself
        gs_set(uc, L_URLC, pfield, 0);
        (void)full;
        gs_set(uc, L_URLC, lfield, 0);
        return;
    }
    char tmp[2048];
    snprintf(tmp, sizeof tmp, "%.*s", (int)plen, part);
    if (wide) { size_t n = u2w(tmp, -1, (uint16_t *)G2H(ptr), len); gs_set(uc, L_URLC, lfield, n); }
    else { size_t n = strlen(tmp); if (n >= len) n = len ? len - 1 : 0; memcpy(G2H(ptr), tmp, n); ((char *)G2H(ptr))[n] = 0; gs_set(uc, L_URLC, lfield, n); }
}
static void crack_url(X86State *st, int wide, uint64_t url_arg, long ulen, uint64_t uc) {
    char *u = gstr_n(url_arg, ulen ? ulen : -1, wide);
    Url p;
    if (!u || parse_url(u, &p) != 0) { free(u); FAIL(E_INVALID_URL); }
    // pointer-into-the-string mode: compute offsets in units of the original string
    const char *path_q = strchr(p.path, '?');
    size_t path_len = path_q ? (size_t)(path_q - p.path) : strlen(p.path);
    struct { int pf, lf; const char *s; size_t n; } parts[] = {
        { 1, 2, p.scheme, strlen(p.scheme) }, { 4, 5, p.host, strlen(p.host) }, { 7, 8, p.user, strlen(p.user) },
        { 9, 10, p.pass, strlen(p.pass) }, { 11, 12, p.path, path_len }, { 13, 14, path_q ? path_q : "", path_q ? strlen(path_q) : 0 } };
    for (int i = 0; i < 6; i++) {
        uint64_t ptr = gs_get(uc, L_URLC, parts[i].pf);
        uint32_t len = (uint32_t)gs_get(uc, L_URLC, parts[i].lf);
        if (!ptr && len) { // point into the caller's string
            const char *at = parts[i].n ? strstr(u, parts[i].s) : NULL;
            if (at) {
                size_t off = (size_t)(at - u);
                if (wide) off = u2w(u, (long)off, NULL, 0);
                gs_set(uc, L_URLC, parts[i].pf, url_arg + off * (wide ? 2 : 1));
                gs_set(uc, L_URLC, parts[i].lf, wide ? u2w(parts[i].s, (long)parts[i].n, NULL, 0) : parts[i].n);
            } else { gs_set(uc, L_URLC, parts[i].pf, 0); gs_set(uc, L_URLC, parts[i].lf, 0); }
        } else crack_part(uc, parts[i].pf, parts[i].lf, u, parts[i].s, parts[i].n, wide);
    }
    gs_set(uc, L_URLC, 3, !strcmp(p.scheme, "https") ? 4 : !strcmp(p.scheme, "http") ? 3 : !strcmp(p.scheme, "ftp") ? 1 : !strcmp(p.scheme, "file") ? 9 : 0);
    gs_set(uc, L_URLC, 6, (uint64_t)p.port);
    free(u);
    RET(1);
}
HLE(in_InternetCrackUrlA) { crack_url(st, 0, ARG(0), (long)(int32_t)ARG(1), hle_arg(st, 3)); }
HLE(in_InternetCrackUrlW) { crack_url(st, 1, ARG(0), (long)(int32_t)ARG(1), hle_arg(st, 3)); }
HLE(in_InternetSetStatusCallback) { RET(0); }
HLE(in_InternetErrorDlg) { RET(0); }
HLE(in_InternetGetLastResponseInfo) { if (ARG(0)) mem_wr(ARG(0), 4, 0); if (ARG(2)) mem_wr(ARG(2), 4, 0); RET(1); }

// ---- WinHTTP -------------------------------------------------------------------------------------------------

HLE(wh_WinHttpOpen) { internet_open(st, 1); }
HLE(wh_WinHttpConnect) { // (session, server, port, reserved)
    if (!inet_get(ARG(0))) FAIL(E_HANDLE);
    Inet *o;
    uint64_t h = inet_new(K_CONNECT, &o);
    if (!h) FAIL(ERROR_NOT_ENOUGH_MEMORY);
    o->parent = ARG(0);
    char *s = gstr(ARG(1), 1);
    snprintf(o->host, sizeof o->host, "%s", s ? s : "");
    free(s);
    o->port = (int)(ARG(2) & 0xFFFF);
    if (!o->port) o->port = 80;
    RET(h);
}
HLE(wh_WinHttpOpenRequest) { // (connect, verb, object, version, referrer, accept, flags)
    char *v = gstr(ARG(1), 1), *ob = gstr(ARG(2), 1);
    uint64_t h = open_request(ARG(0), v, ob, (uint32_t)hle_arg(st, 6), 1);
    free(v); free(ob);
    if (!h) FAIL(E_HANDLE);
    RET(h);
}
HLE(wh_WinHttpAddRequestHeaders) { http_add_headers(st, 1); }
HLE(wh_WinHttpSendRequest) { // (req, headers, len, optional, optlen, total, ctx): sent at ReceiveResponse
    Inet *r = inet_get(ARG(0));
    if (!r || r->kind != K_REQUEST) FAIL(E_HANDLE);
    char *h = gstr_n(ARG(1), (long)(int32_t)ARG(2), 1);
    add_headers(r, h);
    free(h);
    uint32_t n = (uint32_t)hle_arg(st, 4);
    free(r->body);
    r->body = NULL;
    r->body_len = r->body_cap = 0;
    if (ARG(3) && n) { r->body = malloc(n); memcpy(r->body, PTR(3), n); r->body_len = r->body_cap = n; }
    r->total_len = (uint32_t)hle_arg(st, 5);
    RET(1);
}
HLE(wh_WinHttpWriteData) { in_InternetWriteFile(st); }
HLE(wh_WinHttpReceiveResponse) { Inet *r = inet_get(ARG(0)); if (!r) FAIL(E_HANDLE); uint32_t e = send_request(r); if (e) FAIL(e); RET(1); }
HLE(wh_WinHttpQueryHeaders) { // (req, level, name, buffer, &len, &index)
    Inet *r = inet_get(ARG(0));
    char *custom = ARG(2) ? gstr(ARG(2), 1) : NULL;
    int ok = query_info(st, r, (uint32_t)ARG(1), custom, ARG(3), hle_arg(st, 4), hle_arg(st, 5), 1);
    free(custom);
    RET(ok);
}
HLE(wh_WinHttpQueryDataAvailable) { in_InternetQueryDataAvailable(st); }
HLE(wh_WinHttpReadData) { in_InternetReadFile(st); }
HLE(wh_WinHttpCloseHandle) { in_InternetCloseHandle(st); }
HLE(wh_WinHttpCrackUrl) { crack_url(st, 1, ARG(0), (long)(int32_t)ARG(1), hle_arg(st, 3)); }
HLE(wh_WinHttpGetIEProxyConfigForCurrentUser) { // { BOOL autoDetect; LPWSTR autoConfigUrl, proxy, bypass }
    const char *L = "4PPP";
    if (ARG(0)) { memset(PTR(0), 0, gs_size(L)); }
    RET(1);
}
HLE(wh_WinHttpGetProxyForUrl) { set_last_error(st, 12180); RET(0); } // ERROR_WINHTTP_AUTODETECTION_FAILED
HLE(wh_WinHttpQueryOption) { internet_query_option(st, 1); }

// ---- urlmon ---------------------------------------------------------------------------------------------------

#define INET_E_DOWNLOAD_FAILURE 0x800C0008u
static uint32_t download(X86State *st, const char *url, const char *win_dest) {
    (void)st;
    char host[2048];
    path_w2h(win_dest, host, sizeof host);
    SoulHttpResponse *rs = malloc(sizeof *rs);
    uint32_t e = perform("GET", url, NULL, NULL, 0, host, rs);
    int status = rs->status;
    free(rs);
    if (e || status >= 400) { unlink(host); return INET_E_DOWNLOAD_FAILURE; }
    return 0;
}
static void url_download(X86State *st, int wide) { // (caller, url, file, reserved, callback)
    char *u = gstr(ARG(1), wide), *f = gstr(ARG(2), wide);
    uint32_t r = u && f ? download(st, u, f) : 0x80070057u;
    free(u); free(f);
    RET(r);
}
HLE(um_URLDownloadToFileA) { url_download(st, 0); }
HLE(um_URLDownloadToFileW) { url_download(st, 1); }
static void url_download_cache(X86State *st, int wide) { // (caller, url, buffer, cch, reserved, callback)
    char *u = gstr(ARG(1), wide);
    if (!u) { RET(0x80070057u); return; }
    const char *slash = strrchr(u, '/');
    char name[260], win[600];
    snprintf(name, sizeof name, "%s", slash && slash[1] ? slash + 1 : "download.bin");
    char *q = strchr(name, '?');
    if (q) *q = 0;
    snprintf(win, sizeof win, "C:\\users\\soul\\AppData\\Local\\Microsoft\\Windows\\INetCache\\%s", name);
    char hostdir[2048];
    path_w2h("C:\\users\\soul\\AppData\\Local\\Microsoft\\Windows\\INetCache", hostdir, sizeof hostdir);
    char cmd[2100];
    snprintf(cmd, sizeof cmd, "%s", hostdir);
    for (char *c = cmd + 1; *c; c++) if (*c == '/') { *c = 0; mkdir(cmd, 0755); *c = '/'; }
    mkdir(cmd, 0755);
    uint32_t r = download(st, u, win);
    free(u);
    if (!r) {
        uint32_t cap = (uint32_t)ARG(3);
        if (wide) u2w(win, -1, PTR(2), cap); else snprintf(PTR(2), cap, "%s", win);
    }
    RET(r);
}
HLE(um_URLDownloadToCacheFileA) { url_download_cache(st, 0); }
HLE(um_URLDownloadToCacheFileW) { url_download_cache(st, 1); }

const HleEntry hle_inet_table[] = {
    { "InternetOpenA", in_InternetOpenA, NULL }, { "InternetOpenW", in_InternetOpenW, NULL },
    { "InternetConnectA", in_InternetConnectA, NULL }, { "InternetConnectW", in_InternetConnectW, NULL },
    { "HttpOpenRequestA", in_HttpOpenRequestA, NULL }, { "HttpOpenRequestW", in_HttpOpenRequestW, NULL },
    { "HttpAddRequestHeadersA", in_HttpAddRequestHeadersA, NULL }, { "HttpAddRequestHeadersW", in_HttpAddRequestHeadersW, NULL },
    { "HttpSendRequestA", in_HttpSendRequestA, NULL }, { "HttpSendRequestW", in_HttpSendRequestW, NULL },
    { "HttpSendRequestExA", in_HttpSendRequestEx, NULL }, { "HttpSendRequestExW", in_HttpSendRequestEx, NULL },
    { "HttpEndRequestA", in_HttpEndRequest, NULL }, { "HttpEndRequestW", in_HttpEndRequest, NULL },
    { "InternetWriteFile", in_InternetWriteFile, NULL }, { "HttpQueryInfoA", in_HttpQueryInfoA, NULL }, { "HttpQueryInfoW", in_HttpQueryInfoW, NULL },
    { "InternetReadFile", in_InternetReadFile, NULL }, { "InternetReadFileExA", in_InternetReadFileEx, NULL },
    { "InternetReadFileExW", in_InternetReadFileEx, NULL }, { "InternetQueryDataAvailable", in_InternetQueryDataAvailable, NULL },
    { "InternetSetFilePointer", in_InternetSetFilePointer, NULL },
    { "InternetOpenUrlA", in_InternetOpenUrlA, NULL }, { "InternetOpenUrlW", in_InternetOpenUrlW, NULL },
    { "InternetCloseHandle", in_InternetCloseHandle, NULL }, { "InternetGetConnectedState", in_InternetGetConnectedState, NULL },
    { "InternetGetConnectedStateExA", in_InternetGetConnectedState, NULL }, { "InternetGetConnectedStateExW", in_InternetGetConnectedState, NULL },
    { "InternetCheckConnectionA", in_True, NULL }, { "InternetCheckConnectionW", in_True, NULL }, { "InternetAttemptConnect", in_Zero, NULL },
    { "InternetSetOptionA", in_True, NULL }, { "InternetSetOptionW", in_True, NULL }, { "InternetSetOptionExA", in_True, NULL },
    { "InternetSetOptionExW", in_True, NULL }, { "InternetQueryOptionA", in_InternetQueryOptionA, NULL },
    { "InternetQueryOptionW", in_InternetQueryOptionW, NULL }, { "InternetCrackUrlA", in_InternetCrackUrlA, NULL },
    { "InternetCrackUrlW", in_InternetCrackUrlW, NULL }, { "InternetSetStatusCallback", in_InternetSetStatusCallback, NULL },
    { "InternetSetStatusCallbackA", in_InternetSetStatusCallback, NULL }, { "InternetSetStatusCallbackW", in_InternetSetStatusCallback, NULL },
    { "InternetErrorDlg", in_InternetErrorDlg, NULL }, { "InternetGetLastResponseInfoA", in_InternetGetLastResponseInfo, NULL },
    { "InternetGetLastResponseInfoW", in_InternetGetLastResponseInfo, NULL },
    { "WinHttpOpen", wh_WinHttpOpen, NULL }, { "WinHttpConnect", wh_WinHttpConnect, NULL }, { "WinHttpOpenRequest", wh_WinHttpOpenRequest, NULL },
    { "WinHttpAddRequestHeaders", wh_WinHttpAddRequestHeaders, NULL }, { "WinHttpSendRequest", wh_WinHttpSendRequest, NULL },
    { "WinHttpWriteData", wh_WinHttpWriteData, NULL }, { "WinHttpReceiveResponse", wh_WinHttpReceiveResponse, NULL },
    { "WinHttpQueryHeaders", wh_WinHttpQueryHeaders, NULL }, { "WinHttpQueryDataAvailable", wh_WinHttpQueryDataAvailable, NULL },
    { "WinHttpReadData", wh_WinHttpReadData, NULL }, { "WinHttpCloseHandle", wh_WinHttpCloseHandle, NULL },
    { "WinHttpSetOption", in_True, NULL }, { "WinHttpSetTimeouts", in_True, NULL }, { "WinHttpSetCredentials", in_True, NULL },
    { "WinHttpQueryOption", wh_WinHttpQueryOption, NULL }, { "WinHttpCrackUrl", wh_WinHttpCrackUrl, NULL },
    { "WinHttpSetStatusCallback", in_Zero, NULL }, { "WinHttpCheckPlatform", in_True, NULL },
    { "WinHttpGetIEProxyConfigForCurrentUser", wh_WinHttpGetIEProxyConfigForCurrentUser, NULL },
    { "WinHttpGetProxyForUrl", wh_WinHttpGetProxyForUrl, NULL }, { "WinHttpSetDefaultProxyConfiguration", in_True, NULL },
    { "URLDownloadToFileA", um_URLDownloadToFileA, NULL }, { "URLDownloadToFileW", um_URLDownloadToFileW, NULL },
    { "URLDownloadToCacheFileA", um_URLDownloadToCacheFileA, NULL }, { "URLDownloadToCacheFileW", um_URLDownloadToCacheFileW, NULL },
    { NULL, NULL, NULL }
};
