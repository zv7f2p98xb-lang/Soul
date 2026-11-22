// SoulCore - shlwapi path and string helpers (A and W versions), activation contexts.
#include "hle.h"
#include <ctype.h>
#include <strings.h>
#include <sys/stat.h>
#include <dirent.h>

#define CSTR(n) ((const char *)G2H(hle_arg(st, (n))))
#define WSTR(n) ((const uint16_t *)G2H(hle_arg(st, (n))))

// Guest strings of 1-byte (A) or 2-byte (W) units.
static inline uint32_t U(uint64_t s, size_t i, int w) { return w ? ((const uint16_t *)G2H(s))[i] : ((const uint8_t *)G2H(s))[i]; }
static inline void PU(uint64_t s, size_t i, uint32_t c, int w) { if (w) ((uint16_t *)G2H(s))[i] = (uint16_t)c; else ((uint8_t *)G2H(s))[i] = (uint8_t)c; }
static size_t LEN(uint64_t s, int w) { size_t n = 0; while (U(s, n, w)) n++; return n; }
static uint64_t AT(uint64_t s, size_t i, int w) { return s + i * (w ? 2 : 1); }
static void CPY(uint64_t d, uint64_t s, int w) { size_t n = LEN(s, w); memmove(G2H(d), G2H(s), (n + 1) * (w ? 2 : 1)); }
static int SEP(uint32_t c) { return c == '\\' || c == '/'; }
static uint32_t LOW(uint32_t c) { return uc_lower(c); }
static char *gs8(uint64_t p, int w) { if (!p) return NULL; return w ? w2u_dup((const uint16_t *)G2H(p)) : strdup((const char *)G2H(p)); }
static void put8(uint64_t dst, const char *s, size_t cap, int w) {
    if (w) { size_t n = u2w(s, -1, (uint16_t *)G2H(dst), cap); ((uint16_t *)G2H(dst))[n < cap ? n : cap - 1] = 0; }
    else { size_t n = strlen(s); if (n >= cap) n = cap - 1; memcpy(G2H(dst), s, n); ((char *)G2H(dst))[n] = 0; }
}

// ---- finding parts -------------------------------------------------------------------------------------

static size_t find_ext(uint64_t p, int w) {
    size_t dot = (size_t)-1, i = 0;
    for (uint32_t c; (c = U(p, i, w)); i++) {
        if (SEP(c) || c == ' ') dot = (size_t)-1;
        else if (c == '.') dot = i;
    }
    return dot == (size_t)-1 ? i : dot;
}
static size_t find_name(uint64_t p, int w) {
    size_t last = 0;
    for (size_t i = 0; U(p, i, w); i++) {
        uint32_t c = U(p, i, w), n = U(p, i + 1, w);
        if ((SEP(c) || c == ':') && n && !SEP(n)) last = i + 1;
    }
    return last;
}
static size_t root_len(uint64_t p, int w) { // "C:\" -> 3, "\" -> 1, "\\server\share\" -> after share
    if (U(p, 0, w) && U(p, 1, w) == ':') return SEP(U(p, 2, w)) ? 3 : 2;
    if (SEP(U(p, 0, w)) && SEP(U(p, 1, w))) {
        size_t i = 2; int parts = 0;
        while (U(p, i, w) && parts < 2) { if (SEP(U(p, i, w))) parts++; i++; }
        return i;
    }
    return SEP(U(p, 0, w)) ? 1 : 0;
}
static void path_find_ext(X86State *st, int w) { RET(ARG(0) ? AT(ARG(0), find_ext(ARG(0), w), w) : 0); }
static void path_find_name(X86State *st, int w) { RET(ARG(0) ? AT(ARG(0), find_name(ARG(0), w), w) : 0); }
static void path_get_args(X86State *st, int w) {
    uint64_t p = ARG(0);
    int q = 0;
    size_t i = 0;
    for (uint32_t c; (c = U(p, i, w)); i++) {
        if (c == '"') q = !q;
        else if (c == ' ' && !q) { RET(AT(p, i + 1, w)); return; }
    }
    RET(AT(p, i, w));
}
static void path_skip_root(X86State *st, int w) { size_t r = root_len(ARG(0), w); RET(r ? AT(ARG(0), r, w) : 0); }

// ---- modifying -------------------------------------------------------------------------------------------

static void path_remove_ext(X86State *st, int w) { uint64_t p = ARG(0); PU(p, find_ext(p, w), 0, w); RET(0); }
static void path_strip_path(X86State *st, int w) { uint64_t p = ARG(0); size_t n = find_name(p, w); if (n) CPY(p, AT(p, n, w), w); RET(0); }
static void path_remove_file_spec(X86State *st, int w) {
    uint64_t p = ARG(0);
    size_t len = LEN(p, w), r = root_len(p, w), cut = (size_t)-1;
    for (size_t i = r; i < len; i++) if (SEP(U(p, i, w))) cut = i;
    if (cut == (size_t)-1) {
        if (r < len) { PU(p, r, 0, w); RET(1); return; }
        RET(0);
        return;
    }
    PU(p, cut, 0, w);
    RET(1);
}
static void path_add_backslash(X86State *st, int w) {
    uint64_t p = ARG(0);
    size_t n = LEN(p, w);
    if (n && !SEP(U(p, n - 1, w))) { PU(p, n, '\\', w); PU(p, n + 1, 0, w); n++; }
    RET(AT(p, n, w));
}
static void path_remove_backslash(X86State *st, int w) {
    uint64_t p = ARG(0);
    size_t n = LEN(p, w);
    if (n && SEP(U(p, n - 1, w)) && n > root_len(p, w)) { PU(p, n - 1, 0, w); n--; }
    RET(AT(p, n ? n - 1 : 0, w));
}
static void path_rename_ext(X86State *st, int w) {
    uint64_t p = ARG(0), e = ARG(1);
    size_t at = find_ext(p, w);
    size_t i = 0;
    for (uint32_t c; (c = U(e, i, w)); i++) PU(p, at + i, c, w);
    PU(p, at + i, 0, w);
    RET(1);
}
static void path_add_ext(X86State *st, int w) {
    uint64_t p = ARG(0), e = ARG(1);
    size_t at = find_ext(p, w);
    if (U(p, at, w)) { RET(0); return; } // already has one
    const char *def = ".exe";
    size_t i = 0;
    if (e) for (uint32_t c; (c = U(e, i, w)); i++) PU(p, at + i, c, w);
    else for (; def[i]; i++) PU(p, at + i, (uint8_t)def[i], w);
    PU(p, at + i, 0, w);
    RET(1);
}
static void canonicalize(char *s) { // resolves "." and ".." components (backslash-separated)
    char tmp[2048];
    snprintf(tmp, sizeof tmp, "%s", s);
    for (char *c = tmp; *c; c++) if (*c == '/') *c = '\\';
    size_t len = strlen(tmp), rl = 0;
    if (tmp[0] && tmp[1] == ':') rl = tmp[2] == '\\' ? 3 : 2;
    else if (tmp[0] == '\\' && tmp[1] == '\\') rl = 2;
    else if (tmp[0] == '\\') rl = 1;
    int trail = len > rl && tmp[len - 1] == '\\';
    char *parts[256];
    int n = 0;
    char *save = NULL;
    for (char *t = strtok_r(tmp + rl, "\\", &save); t; t = strtok_r(NULL, "\\", &save)) {
        if (!strcmp(t, ".")) continue;
        if (!strcmp(t, "..")) { if (n) n--; continue; }
        if (n < 256) parts[n++] = t;
    }
    char out[2048];
    size_t o = 0;
    memcpy(out, s, rl);
    for (size_t i = 0; i < rl; i++) out[i] = s[i] == '/' ? '\\' : s[i];
    o = rl;
    for (int i = 0; i < n; i++) {
        size_t l = strlen(parts[i]);
        if (o + l + 2 >= sizeof out) break;
        if (i) out[o++] = '\\';
        memcpy(out + o, parts[i], l);
        o += l;
    }
    if (trail && n) out[o++] = '\\';
    out[o] = 0;
    strcpy(s, out);
}
static void path_canonicalize(X86State *st, int w) {
    char *s = gs8(ARG(1), w);
    char buf[2048];
    snprintf(buf, sizeof buf, "%s", s ? s : "");
    free(s);
    canonicalize(buf);
    if (!buf[0]) strcpy(buf, "\\");
    put8(ARG(0), buf, 260, w);
    RET(1);
}
static void path_combine(X86State *st, int w) {
    char *dir = gs8(ARG(1), w), *file = gs8(ARG(2), w);
    char buf[2048];
    if (!dir && !file) { RET(0); free(dir); free(file); return; }
    if (file && (file[0] == '\\' || (file[0] && file[1] == ':'))) {
        if (file[0] == '\\' && file[1] != '\\' && dir && dir[0] && dir[1] == ':') snprintf(buf, sizeof buf, "%c:%s", dir[0], file);
        else snprintf(buf, sizeof buf, "%s", file);
    } else if (dir && *dir && file && *file) {
        size_t l = strlen(dir);
        snprintf(buf, sizeof buf, "%s%s%s", dir, (dir[l - 1] == '\\' || dir[l - 1] == '/') ? "" : "\\", file);
    } else snprintf(buf, sizeof buf, "%s", dir && *dir ? dir : file ? file : "");
    free(dir); free(file);
    canonicalize(buf);
    put8(ARG(0), buf, 260, w);
    RET(ARG(0));
}
static void path_append(X86State *st, int w) {
    char *dir = gs8(ARG(0), w), *more = gs8(ARG(1), w);
    char buf[2048];
    const char *m = more ? more : "";
    while (*m == '\\' || *m == '/') m++;
    size_t l = dir ? strlen(dir) : 0;
    snprintf(buf, sizeof buf, "%s%s%s", dir ? dir : "", l && dir[l - 1] != '\\' && *m ? "\\" : "", m);
    free(dir); free(more);
    canonicalize(buf);
    put8(ARG(0), buf, 260, w);
    RET(1);
}
static void path_quote_spaces(X86State *st, int w) {
    char *s = gs8(ARG(0), w);
    if (s && strchr(s, ' ') && s[0] != '"') {
        char buf[2048];
        snprintf(buf, sizeof buf, "\"%s\"", s);
        put8(ARG(0), buf, 260, w);
        RET(1);
    } else RET(0);
    free(s);
}
static void path_unquote_spaces(X86State *st, int w) {
    uint64_t p = ARG(0);
    size_t n = LEN(p, w);
    if (n >= 2 && U(p, 0, w) == '"' && U(p, n - 1, w) == '"') {
        for (size_t i = 0; i + 2 < n; i++) PU(p, i, U(p, i + 1, w), w);
        PU(p, n - 2, 0, w);
    }
    RET(0);
}
static void path_strip_to_root(X86State *st, int w) { uint64_t p = ARG(0); size_t r = root_len(p, w); PU(p, r, 0, w); RET(r != 0); }
static void path_remove_args(X86State *st, int w) {
    uint64_t p = ARG(0);
    int q = 0;
    for (size_t i = 0; U(p, i, w); i++) {
        uint32_t c = U(p, i, w);
        if (c == '"') q = !q;
        else if (c == ' ' && !q) { PU(p, i, 0, w); break; }
    }
    RET(0);
}

// ---- questions ---------------------------------------------------------------------------------------

static void path_is_relative(X86State *st, int w) { uint64_t p = ARG(0); RET(!p || !(SEP(U(p, 0, w)) || (U(p, 0, w) && U(p, 1, w) == ':'))); }
static void path_is_root(X86State *st, int w) {
    uint64_t p = ARG(0);
    size_t n = LEN(p, w), r = root_len(p, w);
    RET(r && (n == r || (n == r + 1 && SEP(U(p, r, w)))));
}
static void path_is_unc(X86State *st, int w) { uint64_t p = ARG(0); RET(SEP(U(p, 0, w)) && SEP(U(p, 1, w))); }
static int wild(const char *pat, const char *s) { return wild_match(pat, s); }
static void path_match_spec(X86State *st, int w) {
    char *f = gs8(ARG(0), w), *spec = gs8(ARG(1), w);
    int ok = 0;
    if (f && spec) {
        const char *bs = strrchr(f, '\\');
        const char *name = bs ? bs + 1 : f;
        char *save = NULL;
        for (char *p = strtok_r(spec, ";", &save); p && !ok; p = strtok_r(NULL, ";", &save)) {
            while (*p == ' ') p++;
            if (*p && (wild(p, name) || wild(p, f))) ok = 1;
        }
    }
    free(f); free(spec);
    RET(ok);
}
static void path_is_same_root(X86State *st, int w) {
    size_t a = root_len(ARG(0), w), b = root_len(ARG(1), w);
    int same = a && a == b;
    for (size_t i = 0; same && i < a; i++) if (LOW(U(ARG(0), i, w)) != LOW(U(ARG(1), i, w))) same = 0;
    RET(same);
}
static void path_is_prefix(X86State *st, int w) { // (prefix, path)
    size_t n = LEN(ARG(0), w);
    int ok = 1;
    for (size_t i = 0; i < n && ok; i++) if (LOW(U(ARG(0), i, w)) != LOW(U(ARG(1), i, w))) ok = 0;
    RET(ok && (SEP(U(ARG(1), n, w)) || !U(ARG(1), n, w) || (n && SEP(U(ARG(0), n - 1, w)))));
}
static void path_common_prefix(X86State *st, int w) {
    uint64_t a = ARG(0), b = ARG(1);
    size_t i = 0, last = 0;
    for (;; i++) {
        uint32_t x = U(a, i, w), y = U(b, i, w);
        if (!x || !y || LOW(x) != LOW(y)) { if ((!x || SEP(x)) && (!y || SEP(y))) last = i; break; }
        if (SEP(x)) last = i;
    }
    if (ARG(2)) { for (size_t k = 0; k < last; k++) PU(ARG(2), k, U(a, k, w), w); PU(ARG(2), last, 0, w); }
    RET(last);
}
static void path_is_dir_empty(X86State *st, int w) {
    char *s = gs8(ARG(0), w);
    char h[2048];
    path_w2h(s ? s : "", h, sizeof h);
    free(s);
    DIR *d = opendir(h);
    if (!d) { RET(0); return; }
    int empty = 1;
    struct dirent *e;
    while ((e = readdir(d))) if (strcmp(e->d_name, ".") && strcmp(e->d_name, "..")) { empty = 0; break; }
    closedir(d);
    RET(empty);
}
static void path_compact(X86State *st, int w) { // PathCompactPathEx(out, src, max, flags)
    char *s = gs8(ARG(1), w);
    uint32_t max = (uint32_t)ARG(2);
    char buf[2048];
    snprintf(buf, sizeof buf, "%s", s ? s : "");
    free(s);
    if (max && strlen(buf) >= max) {
        const char *bs = strrchr(buf, '\\');
        const char *name = bs ? bs : buf;
        char out[2048];
        size_t keep = max > strlen(name) + 4 ? max - strlen(name) - 4 : 0;
        if (keep) snprintf(out, sizeof out, "%.*s...%s", (int)keep, buf, name);
        else snprintf(out, sizeof out, "...%.*s", (int)(max > 4 ? max - 4 : 0), name + (strlen(name) > max - 4 ? strlen(name) - (max - 4) : 0));
        strcpy(buf, out);
    }
    put8(ARG(0), buf, max ? max : 1, w);
    RET(1);
}
static void path_drive_number(X86State *st, int w) {
    uint64_t p = ARG(0);
    uint32_t c = U(p, 0, w);
    RET(c && U(p, 1, w) == ':' && isalpha((int)(c & 0x7F)) ? (uint64_t)(toupper((int)c) - 'A') : (uint64_t)-1);
}
static void path_build_root(X86State *st, int w) {
    int d = (int)ARG(1);
    if (d < 0 || d > 25) { RET(ARG(0)); return; }
    PU(ARG(0), 0, (uint32_t)('A' + d), w); PU(ARG(0), 1, ':', w); PU(ARG(0), 2, '\\', w); PU(ARG(0), 3, 0, w);
    RET(ARG(0));
}
static void path_search_qualify(X86State *st, int w) { // (path, buffer, size)
    char *s = gs8(ARG(0), w);
    char norm[1200];
    path_normalize_win(s ? s : "", norm, sizeof norm);
    free(s);
    put8(ARG(1), norm, (size_t)(uint32_t)ARG(2), w);
    RET(1);
}

// ---- strings ----------------------------------------------------------------------------------------------

static int cmp_units(uint64_t a, uint64_t b, int w, int icase, long n) {
    for (long i = 0; n < 0 || i < n; i++) {
        uint32_t x = U(a, (size_t)i, w), y = U(b, (size_t)i, w);
        if (icase) { x = LOW(x); y = LOW(y); }
        if (x != y) return x < y ? -1 : 1;
        if (!x) return 0;
    }
    return 0;
}
static void str_cmp(X86State *st, int w, int icase, int counted) { RET((uint64_t)(int64_t)cmp_units(ARG(0), ARG(1), w, icase, counted ? (long)(int32_t)ARG(2) : -1)); }
static void str_str(X86State *st, int w, int icase) {
    uint64_t h = ARG(0), n = ARG(1);
    if (!h || !n) { RET(0); return; }
    size_t nl = LEN(n, w);
    for (size_t i = 0; U(h, i, w); i++) if (!cmp_units(AT(h, i, w), n, w, icase, (long)nl)) { RET(AT(h, i, w)); return; }
    RET(nl == 0 ? h : 0);
}
static void str_chr(X86State *st, int w, int icase, int rev) {
    uint64_t s = ARG(0);
    uint32_t c = (uint32_t)(ARG(rev ? 2 : 1) & (w ? 0xFFFF : 0xFF));
    if (rev) { // StrRChr(start, end, ch)
        size_t end = ARG(1) ? (size_t)((ARG(1) - s) / (w ? 2 : 1)) : LEN(s, w);
        for (size_t i = end; i-- > 0;) if ((icase ? LOW(U(s, i, w)) == LOW(c) : U(s, i, w) == c)) { RET(AT(s, i, w)); return; }
        RET(0);
        return;
    }
    for (size_t i = 0; U(s, i, w); i++) if ((icase ? LOW(U(s, i, w)) == LOW(c) : U(s, i, w) == c)) { RET(AT(s, i, w)); return; }
    RET(0);
}
static void str_to_int(X86State *st, int w) {
    char *s = gs8(ARG(0), w);
    long v = s ? strtol(s, NULL, 10) : 0;
    free(s);
    RET((uint64_t)(int64_t)(int32_t)v);
}
static void str_to_int_ex(X86State *st, int w) { // (string, flags, &result): STIF_SUPPORT_HEX = 1
    char *s = gs8(ARG(0), w);
    char *end = NULL;
    const char *p = s ? s : "";
    while (*p == ' ') p++;
    long long v = ((uint32_t)ARG(1) & 1) && p[0] == '0' && (p[1] == 'x' || p[1] == 'X') ? strtoll(p + 2, &end, 16) : strtoll(p, &end, 10);
    int ok = end && end != p;
    free(s);
    if (ARG(2)) mem_wr(ARG(2), 4, (uint64_t)v);
    RET(ok);
}
static void str_trim(X86State *st, int w) { // (string, characters to trim)
    uint64_t p = ARG(0), t = ARG(1);
    size_t n = LEN(p, w), tl = LEN(t, w), a = 0, b = n;
    #define IN_SET(c) ({ int _f = 0; for (size_t _k = 0; _k < tl; _k++) if (U(t, _k, w) == (c)) _f = 1; _f; })
    while (a < b && IN_SET(U(p, a, w))) a++;
    while (b > a && IN_SET(U(p, b - 1, w))) b--;
    #undef IN_SET
    if (a == 0 && b == n) { RET(0); return; }
    for (size_t i = a; i < b; i++) PU(p, i - a, U(p, i, w), w);
    PU(p, b - a, 0, w);
    RET(1);
}
static void str_dup(X86State *st, int w) {
    size_t n = (LEN(ARG(0), w) + 1) * (w ? 2 : 1);
    void *d = g_malloc(n);
    memcpy(d, PTR(0), n);
    RET(d);
}
static void byte_size(X86State *st, int w, uint64_t v, uint64_t buf, uint32_t cap) {
    char s[64];
    if (v < 1024) snprintf(s, sizeof s, "%llu bytes", (unsigned long long)v);
    else {
        static const char *u[] = { "KB", "MB", "GB", "TB", "PB" };
        double d = (double)v / 1024.0;
        int k = 0;
        while (d >= 1024.0 && k < 4) { d /= 1024.0; k++; }
        if (d < 10) snprintf(s, sizeof s, "%.2f %s", d, u[k]);
        else if (d < 100) snprintf(s, sizeof s, "%.1f %s", d, u[k]);
        else snprintf(s, sizeof s, "%.0f %s", d, u[k]);
    }
    if (buf && cap) put8(buf, s, cap, w);
    RET(buf);
}

#define PAIR(base, fn) HLE(sw_##base##A) { fn(st, 0); } HLE(sw_##base##W) { fn(st, 1); }
PAIR(PathFindExtension, path_find_ext)
PAIR(PathFindFileName, path_find_name)
PAIR(PathGetArgs, path_get_args)
PAIR(PathSkipRoot, path_skip_root)
PAIR(PathRemoveExtension, path_remove_ext)
PAIR(PathStripPath, path_strip_path)
PAIR(PathRemoveFileSpec, path_remove_file_spec)
PAIR(PathAddBackslash, path_add_backslash)
PAIR(PathRemoveBackslash, path_remove_backslash)
PAIR(PathRenameExtension, path_rename_ext)
PAIR(PathAddExtension, path_add_ext)
PAIR(PathCanonicalize, path_canonicalize)
PAIR(PathCombine, path_combine)
PAIR(PathAppend, path_append)
PAIR(PathQuoteSpaces, path_quote_spaces)
PAIR(PathUnquoteSpaces, path_unquote_spaces)
PAIR(PathStripToRoot, path_strip_to_root)
PAIR(PathRemoveArgs, path_remove_args)
PAIR(PathIsRelative, path_is_relative)
PAIR(PathIsRoot, path_is_root)
PAIR(PathIsUNC, path_is_unc)
PAIR(PathMatchSpec, path_match_spec)
PAIR(PathIsSameRoot, path_is_same_root)
PAIR(PathIsPrefix, path_is_prefix)
PAIR(PathCommonPrefix, path_common_prefix)
PAIR(PathIsDirectoryEmpty, path_is_dir_empty)
PAIR(PathCompactPathEx, path_compact)
PAIR(PathGetDriveNumber, path_drive_number)
PAIR(PathBuildRoot, path_build_root)
PAIR(PathSearchAndQualify, path_search_qualify)
PAIR(StrToInt, str_to_int)
PAIR(StrToIntEx, str_to_int_ex)
PAIR(StrTrim, str_trim)
PAIR(StrDup, str_dup)
HLE(sw_StrCmpA) { str_cmp(st, 0, 0, 0); }
HLE(sw_StrCmpW) { str_cmp(st, 1, 0, 0); }
HLE(sw_StrCmpIA) { str_cmp(st, 0, 1, 0); }
HLE(sw_StrCmpIW) { str_cmp(st, 1, 1, 0); }
HLE(sw_StrCmpNA) { str_cmp(st, 0, 0, 1); }
HLE(sw_StrCmpNW) { str_cmp(st, 1, 0, 1); }
HLE(sw_StrCmpNIA) { str_cmp(st, 0, 1, 1); }
HLE(sw_StrCmpNIW) { str_cmp(st, 1, 1, 1); }
HLE(sw_StrStrA) { str_str(st, 0, 0); }
HLE(sw_StrStrW) { str_str(st, 1, 0); }
HLE(sw_StrStrIA) { str_str(st, 0, 1); }
HLE(sw_StrStrIW) { str_str(st, 1, 1); }
HLE(sw_StrChrA) { str_chr(st, 0, 0, 0); }
HLE(sw_StrChrW) { str_chr(st, 1, 0, 0); }
HLE(sw_StrChrIA) { str_chr(st, 0, 1, 0); }
HLE(sw_StrChrIW) { str_chr(st, 1, 1, 0); }
HLE(sw_StrRChrA) { str_chr(st, 0, 0, 1); }
HLE(sw_StrRChrW) { str_chr(st, 1, 0, 1); }
HLE(sw_StrRChrIW) { str_chr(st, 1, 1, 1); }
HLE(sw_StrCpyW) { CPY(ARG(0), ARG(1), 1); RET(ARG(0)); }
HLE(sw_StrCpyNW) { // (dst, src, count incl. NUL)
    int n = (int)(int32_t)ARG(2);
    if (n <= 0) { RET(ARG(0)); return; }
    int i = 0;
    for (; i < n - 1 && U(ARG(1), (size_t)i, 1); i++) PU(ARG(0), (size_t)i, U(ARG(1), (size_t)i, 1), 1);
    PU(ARG(0), (size_t)i, 0, 1);
    RET(ARG(0));
}
HLE(sw_StrCatW) { CPY(AT(ARG(0), LEN(ARG(0), 1), 1), ARG(1), 1); RET(ARG(0)); }
HLE(sw_StrCatBuffW) { // (dst, src, size)
    size_t n = LEN(ARG(0), 1), cap = (size_t)(uint32_t)ARG(2);
    for (size_t i = 0; n + 1 < cap && U(ARG(1), i, 1); i++, n++) PU(ARG(0), n, U(ARG(1), i, 1), 1);
    if (cap) PU(ARG(0), n < cap ? n : cap - 1, 0, 1);
    RET(ARG(0));
}
HLE(sw_StrFormatByteSizeW) { byte_size(st, 1, g_mode32 ? (ARG(0) | ARG(1) << 32) : ARG(0), g_mode32 ? ARG(2) : ARG(1), (uint32_t)(g_mode32 ? ARG(3) : ARG(2))); }
HLE(sw_StrFormatByteSize64A) { byte_size(st, 0, g_mode32 ? (ARG(0) | ARG(1) << 32) : ARG(0), g_mode32 ? ARG(2) : ARG(1), (uint32_t)(g_mode32 ? ARG(3) : ARG(2))); }
HLE(sw_StrFormatByteSizeA) { byte_size(st, 0, (uint32_t)ARG(0), ARG(1), (uint32_t)ARG(2)); }
HLE(sw_StrFormatKBSizeW) {
    uint64_t v = g_mode32 ? (ARG(0) | ARG(1) << 32) : ARG(0);
    char s[64];
    snprintf(s, sizeof s, "%llu KB", (unsigned long long)((v + 1023) / 1024));
    uint64_t buf = g_mode32 ? ARG(2) : ARG(1);
    put8(buf, s, (size_t)(uint32_t)(g_mode32 ? ARG(3) : ARG(2)), 1);
    RET(buf);
}
HLE(sw_StrCmpLogicalW) { // digits compare by value
    const uint16_t *a = WSTR(0), *b = WSTR(1);
    while (*a && *b) {
        if (*a >= '0' && *a <= '9' && *b >= '0' && *b <= '9') {
            unsigned long long x = 0, y = 0;
            while (*a >= '0' && *a <= '9') x = x * 10 + (unsigned)(*a++ - '0');
            while (*b >= '0' && *b <= '9') y = y * 10 + (unsigned)(*b++ - '0');
            if (x != y) { RET((uint64_t)(x < y ? -1 : 1)); return; }
            continue;
        }
        uint32_t x = LOW(*a), y = LOW(*b);
        if (x != y) { RET((uint64_t)(x < y ? -1 : 1)); return; }
        a++; b++;
    }
    RET((uint64_t)(*a ? 1 : *b ? -1 : 0));
}
HLE(sw_ChrCmpIW) { RET(LOW((uint16_t)ARG(0)) != LOW((uint16_t)ARG(1))); }

// ---- activation contexts (side-by-side manifests): accepted, nothing to switch -------------------------------

HLE(k32_CreateActCtx) { RET(0x7A00); }
HLE(k32_ActivateActCtx) { if (ARG(1)) wr_ptr(ARG(1), 1); RET(1); }
HLE(k32_DeactivateActCtx) { RET(1); }
HLE(k32_ReleaseActCtx) { RET(0); }
HLE(k32_GetCurrentActCtx) { if (ARG(0)) wr_ptr(ARG(0), 0); RET(1); }
HLE(k32_QueryActCtxW) { set_last_error(st, ERROR_INVALID_PARAMETER); RET(0); }
HLE(k32_FindActCtxSectionString) { set_last_error(st, 14001); RET(0); } // ERROR_SXS_KEY_NOT_FOUND-ish

#define P2(n) { #n "A", sw_##n##A, NULL }, { #n "W", sw_##n##W, NULL }
const HleEntry hle_shlwapi_table[] = {
    P2(PathFindExtension), P2(PathFindFileName), P2(PathGetArgs), P2(PathSkipRoot), P2(PathRemoveExtension), P2(PathStripPath),
    P2(PathRemoveFileSpec), P2(PathAddBackslash), P2(PathRemoveBackslash), P2(PathRenameExtension), P2(PathAddExtension),
    P2(PathCanonicalize), P2(PathCombine), P2(PathAppend), P2(PathQuoteSpaces), P2(PathUnquoteSpaces), P2(PathStripToRoot),
    P2(PathRemoveArgs), P2(PathIsRelative), P2(PathIsRoot), P2(PathIsUNC), P2(PathMatchSpec), P2(PathIsSameRoot), P2(PathIsPrefix),
    P2(PathCommonPrefix), P2(PathIsDirectoryEmpty), P2(PathCompactPathEx), P2(PathGetDriveNumber), P2(PathBuildRoot),
    P2(PathSearchAndQualify), P2(StrToInt), P2(StrToIntEx), P2(StrTrim), P2(StrDup),
    { "StrCmpA", sw_StrCmpA, NULL }, { "StrCmpW", sw_StrCmpW, NULL }, { "StrCmpIA", sw_StrCmpIA, NULL }, { "StrCmpIW", sw_StrCmpIW, NULL },
    { "StrCmpNA", sw_StrCmpNA, NULL }, { "StrCmpNW", sw_StrCmpNW, NULL }, { "StrCmpNIA", sw_StrCmpNIA, NULL }, { "StrCmpNIW", sw_StrCmpNIW, NULL },
    { "StrStrA", sw_StrStrA, NULL }, { "StrStrW", sw_StrStrW, NULL }, { "StrStrIA", sw_StrStrIA, NULL }, { "StrStrIW", sw_StrStrIW, NULL },
    { "StrChrA", sw_StrChrA, NULL }, { "StrChrW", sw_StrChrW, NULL }, { "StrChrIA", sw_StrChrIA, NULL }, { "StrChrIW", sw_StrChrIW, NULL },
    { "StrRChrA", sw_StrRChrA, NULL }, { "StrRChrW", sw_StrRChrW, NULL }, { "StrRChrIW", sw_StrRChrIW, NULL },
    { "StrCpyW", sw_StrCpyW, NULL }, { "StrCpyNW", sw_StrCpyNW, NULL }, { "StrCatW", sw_StrCatW, NULL }, { "StrCatBuffW", sw_StrCatBuffW, NULL },
    { "StrFormatByteSizeW", sw_StrFormatByteSizeW, NULL }, { "StrFormatByteSizeEx", sw_StrFormatByteSizeW, NULL },
    { "StrFormatByteSize64A", sw_StrFormatByteSize64A, NULL }, { "StrFormatByteSizeA", sw_StrFormatByteSizeA, NULL },
    { "StrFormatKBSizeW", sw_StrFormatKBSizeW, NULL }, { "StrCmpLogicalW", sw_StrCmpLogicalW, NULL }, { "ChrCmpIW", sw_ChrCmpIW, NULL },
    { "CreateActCtxA", k32_CreateActCtx, NULL }, { "CreateActCtxW", k32_CreateActCtx, NULL }, { "ActivateActCtx", k32_ActivateActCtx, NULL },
    { "DeactivateActCtx", k32_DeactivateActCtx, NULL }, { "ReleaseActCtx", k32_ReleaseActCtx, NULL }, { "AddRefActCtx", k32_ReleaseActCtx, NULL },
    { "GetCurrentActCtx", k32_GetCurrentActCtx, NULL }, { "QueryActCtxW", k32_QueryActCtxW, NULL },
    { "FindActCtxSectionStringW", k32_FindActCtxSectionString, NULL }, { "FindActCtxSectionStringA", k32_FindActCtxSectionString, NULL },
    { NULL, NULL, NULL }
};
