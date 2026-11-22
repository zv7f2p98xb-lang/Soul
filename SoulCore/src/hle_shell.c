// SoulCore - shell32 / ole32 basics and the other APIs installers lean on: file operations,
// shell item IDs, file information, INI ("profile") files, SearchPath, token checks.
#include "hle.h"
#include "gui.h"
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>

#define CSTR(n) ((const char *)G2H(hle_arg(st, (n))))
#define WSTR(n) ((const uint16_t *)G2H(hle_arg(st, (n))))
#define ARGI(n) ((int)(int32_t)hle_arg(st, (n)))
#define FAIL(err) do { set_last_error(st, (err)); RET(0); return; } while (0)

int soul_message_box(const char *title, const char *text, unsigned type); // hle_misc.c
const char *soul_csidl_path(int csidl);                                  // hle_misc.c
uint64_t iml_system(int large);                                          // comctl.c
const char *env_get_value(const char *name);                             // hle_kernel32.c

static char *gstr(uint64_t p, int wide) {
    if (!p) return NULL;
    return wide ? w2u_dup((const uint16_t *)G2H(p)) : strdup((const char *)G2H(p));
}
static uint64_t put_str(uint64_t buf, uint64_t cap, const char *s, int wide) {
    if (wide) {
        size_t n = u2w(s, -1, NULL, 0);
        if (!buf || n + 1 > cap) return n + 1;
        u2w(s, -1, (uint16_t *)G2H(buf), cap);
        return n;
    }
    size_t n = strlen(s);
    if (!buf || n + 1 > cap) return n + 1;
    memcpy(G2H(buf), s, n + 1);
    return n;
}
// Copies at most cap-1 units and always terminates (cap > 0); returns units copied.
static size_t put_trunc(uint64_t buf, size_t cap, const char *s, int wide) {
    if (!buf || !cap) return 0;
    if (wide) {
        uint16_t *tmp = u2w_dup(s);
        size_t n = wstrlen(tmp);
        if (n > cap - 1) n = cap - 1;
        memcpy(G2H(buf), tmp, n * 2);
        ((uint16_t *)G2H(buf))[n] = 0;
        free(tmp);
        return n;
    }
    size_t n = strlen(s);
    if (n > cap - 1) n = cap - 1;
    memcpy(G2H(buf), s, n);
    ((char *)G2H(buf))[n] = 0;
    return n;
}
// Double-NUL terminated string lists ("a\0b\0\0").
static int read_multi(uint64_t p, int wide, char ***out) {
    int n = 0, cap = 8;
    char **v = malloc(sizeof(char *) * (size_t)cap);
    if (p) {
        for (;;) {
            char *s = gstr(p, wide);
            if (!s || !*s) { free(s); break; }
            if (n == cap) { cap *= 2; v = realloc(v, sizeof(char *) * (size_t)cap); }
            v[n++] = s;
            if (wide) p += (wstrlen((const uint16_t *)G2H(p)) + 1) * 2;
            else p += strlen((const char *)G2H(p)) + 1;
        }
    }
    *out = v;
    return n;
}
static void free_multi(char **v, int n) { for (int i = 0; i < n; i++) free(v[i]); free(v); }
// Writes a list of strings as a double-NUL list with GetPrivateProfileString semantics:
// returns the units written without the final NUL, or cap-2 when the buffer was too small.
static uint32_t put_multi(uint64_t buf, uint32_t cap, char **items, int n, int wide) {
    if (!buf || cap < 2) return 0;
    size_t o = 0;
    int trunc = 0;
    for (int i = 0; i < n && !trunc; i++) {
        uint16_t *w = wide ? u2w_dup(items[i]) : NULL;
        size_t len = wide ? wstrlen(w) : strlen(items[i]);
        if (o + len + 2 > cap) { len = cap - 2 > o ? cap - 2 - o : 0; trunc = 1; }
        if (wide) memcpy((uint16_t *)G2H(buf) + o, w, len * 2); else memcpy((char *)G2H(buf) + o, items[i], len);
        o += len;
        if (wide) ((uint16_t *)G2H(buf))[o] = 0; else ((char *)G2H(buf))[o] = 0;
        o++;
        free(w);
    }
    if (wide) ((uint16_t *)G2H(buf))[o] = 0; else ((char *)G2H(buf))[o] = 0;
    return trunc ? cap - 2 : (uint32_t)o;
}
static void host_of(const char *win, char *host, size_t cap) { path_w2h(win ? win : "", host, cap); }

// ---- host file tree helpers -------------------------------------------------------------------

static int is_dir(const char *h) { struct stat sb; return stat(h, &sb) == 0 && S_ISDIR(sb.st_mode); }
static int exists(const char *h) { struct stat sb; return stat(h, &sb) == 0; }

static int copy_file_host(const char *src, const char *dst) {
    int in = open(src, O_RDONLY);
    if (in < 0) return -1;
    int out = open(dst, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (out < 0) { close(in); return -1; }
    char buf[65536];
    ssize_t n;
    int ok = 0;
    while ((n = read(in, buf, sizeof buf)) > 0) if (write(out, buf, (size_t)n) != n) { ok = -1; break; }
    close(in);
    close(out);
    return ok;
}
static int copy_tree(const char *src, const char *dst) {
    if (!is_dir(src)) return copy_file_host(src, dst);
    mkdir(dst, 0755);
    DIR *d = opendir(src);
    if (!d) return -1;
    struct dirent *de;
    int r = 0;
    while ((de = readdir(d))) {
        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
        char a[2048], b[2048];
        snprintf(a, sizeof a, "%s/%s", src, de->d_name);
        snprintf(b, sizeof b, "%s/%s", dst, de->d_name);
        if (copy_tree(a, b)) r = -1;
    }
    closedir(d);
    return r;
}
static int remove_tree(const char *p) {
    if (!is_dir(p)) return unlink(p);
    DIR *d = opendir(p);
    if (d) {
        struct dirent *de;
        while ((de = readdir(d))) {
            if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
            char a[2048];
            snprintf(a, sizeof a, "%s/%s", p, de->d_name);
            remove_tree(a);
        }
        closedir(d);
    }
    return rmdir(p);
}
static int mkdirs(const char *h) {
    char tmp[2048];
    snprintf(tmp, sizeof tmp, "%s", h);
    for (char *s = tmp + 1; *s; s++) if (*s == '/') { *s = 0; mkdir(tmp, 0755); *s = '/'; }
    return mkdir(tmp, 0755) == 0 || errno == EEXIST ? 0 : -1;
}

// ---- SHFileOperation -------------------------------------------------------------------------------

#define FO_MOVE 1
#define FO_COPY 2
#define FO_DELETE 3
#define FO_RENAME 4
#define FOF_MULTIDESTFILES 0x1

// Expands a source (Windows path, possibly with wildcards in the last part) into host paths.
static int expand_source(const char *win, char ***out) {
    int n = 0, cap = 8;
    char **v = malloc(sizeof(char *) * (size_t)cap);
    char norm[1200];
    path_normalize_win(win, norm, sizeof norm);
    char *bs = strrchr(norm, '\\');
    const char *last = bs ? bs + 1 : norm;
    if (strpbrk(last, "*?")) {
        char dir[1200], pat[300], hdir[2048];
        size_t dl = bs ? (size_t)(bs - norm) : 0;
        snprintf(dir, sizeof dir, "%.*s", (int)dl, norm);
        snprintf(pat, sizeof pat, "%s", last);
        host_of(dir, hdir, sizeof hdir);
        DIR *d = opendir(hdir);
        if (d) {
            struct dirent *de;
            while ((de = readdir(d))) {
                if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..") || !wild_match(pat, de->d_name)) continue;
                if (n == cap) { cap *= 2; v = realloc(v, sizeof(char *) * (size_t)cap); }
                char h[2600];
                snprintf(h, sizeof h, "%s/%s", hdir, de->d_name);
                v[n++] = strdup(h);
            }
            closedir(d);
        }
    } else {
        char h[2048];
        host_of(norm, h, sizeof h);
        if (exists(h)) v[n++] = strdup(h);
    }
    *out = v;
    return n;
}
static const char *base_name(const char *h) { const char *s = strrchr(h, '/'); return s ? s + 1 : h; }

static int file_operation(int func, uint32_t flags, char **from, int nfrom, char **to, int nto) {
    int err = 0;
    for (int i = 0; i < nfrom; i++) {
        char **src;
        int ns = expand_source(from[i], &src);
        int wild = strpbrk(from[i], "*?") != NULL;
        if (ns == 0 && func != FO_DELETE) err = 2;
        for (int k = 0; k < ns; k++) {
            if (func == FO_DELETE) { if (remove_tree(src[k])) err = 0x78; continue; }
            const char *dwin = (flags & FOF_MULTIDESTFILES) && i < nto ? to[i] : nto ? to[0] : "";
            char dst[2048];
            host_of(dwin, dst, sizeof dst);
            char target[2600];
            // into an existing folder, or into a new folder when several items are copied
            int into = is_dir(dst) && !(func == FO_RENAME);
            if (!into && !(flags & FOF_MULTIDESTFILES) && (nfrom > 1 || wild) && func != FO_RENAME) { mkdirs(dst); into = 1; }
            if (into) snprintf(target, sizeof target, "%s/%s", dst, base_name(src[k]));
            else {
                snprintf(target, sizeof target, "%s", dst);
                char parent[2600];
                snprintf(parent, sizeof parent, "%s", target);
                char *sl = strrchr(parent, '/');
                if (sl && sl != parent) { *sl = 0; mkdirs(parent); }
            }
            if (func == FO_COPY) { if (copy_tree(src[k], target)) err = 0x75; }
            else if (rename(src[k], target) != 0) {
                if (copy_tree(src[k], target) == 0) remove_tree(src[k]);
                else err = 0x7C;
            }
        }
        free_multi(src, ns);
    }
    return err;
}
static void sh_file_op(X86State *st, int wide) {
    // SHFILEOPSTRUCT is byte-packed in 32-bit programs (shellapi.h uses pshpack1 there).
    uint64_t s = ARG(0);
    int m32 = g_mode32;
    uint32_t func = (uint32_t)mem_rd(s + (m32 ? 4 : 8), 4);
    uint64_t pfrom = rd_ptr(s + (m32 ? 8 : 16)), pto = rd_ptr(s + (m32 ? 12 : 24));
    uint32_t flags = (uint32_t)mem_rd(s + (m32 ? 16 : 32), 2);
    char **from, **to;
    int nf = read_multi(pfrom, wide, &from), nt = read_multi(pto, wide, &to);
    int r = file_operation((int)func, flags, from, nf, to, nt);
    mem_wr(s + (m32 ? 18 : 36), 4, 0); // fAnyOperationsAborted
    free_multi(from, nf);
    free_multi(to, nt);
    RET(r);
}
HLE(sh_SHFileOperationA) { sh_file_op(st, 0); }
HLE(sh_SHFileOperationW) { sh_file_op(st, 1); }

// ---- item ID lists (PIDLs) -----------------------------------------------------------------------
// Soul's PIDLs hold one item: { USHORT cb; "SOUL"; UTF-8 path; NUL } followed by a 0 terminator.
// The empty list is the desktop.

static uint64_t pidl_from_path(const char *win) {
    char norm[1200];
    path_normalize_win(win, norm, sizeof norm);
    size_t l = strlen(norm) + 1;
    size_t cb = 2 + 4 + l;
    uint8_t *p = g_malloc(cb + 2);
    p[0] = (uint8_t)cb; p[1] = (uint8_t)(cb >> 8);
    memcpy(p + 2, "SOUL", 4);
    memcpy(p + 6, norm, l);
    p[cb] = p[cb + 1] = 0;
    return H2G(p);
}
static int pidl_to_path(uint64_t pidl, char *out, size_t cap) {
    if (!pidl) return 0;
    uint16_t cb = (uint16_t)mem_rd(pidl, 2);
    if (cb == 0) { snprintf(out, cap, "%s", soul_csidl_path(0)); return 1; }
    if (cb < 7 || memcmp(G2H(pidl + 2), "SOUL", 4) != 0) return 0;
    snprintf(out, cap, "%s", (const char *)G2H(pidl + 6));
    return 1;
}
static size_t pidl_size(uint64_t pidl) {
    size_t n = 0;
    while (pidl && mem_rd(pidl + n, 2)) n += (size_t)mem_rd(pidl + n, 2);
    return n + 2;
}
HLE(sh_ILFree) { g_free(PTR(0)); RET(0); }
HLE(sh_ILClone) {
    if (!ARG(0)) { RET(0); return; }
    size_t n = pidl_size(ARG(0));
    void *c = g_malloc(n);
    memcpy(c, PTR(0), n);
    RET(c);
}
HLE(sh_ILGetSize) { RET(ARG(0) ? pidl_size(ARG(0)) : 0); }
HLE(sh_ILCombine) {
    char a[1200] = "", path[1300];
    if (!pidl_to_path(ARG(0), a, sizeof a)) { RET(0); return; }
    char b[1200] = "";
    if (ARG(1) && mem_rd(ARG(1), 2)) {
        pidl_to_path(ARG(1), b, sizeof b);
        const char *bs = strrchr(b, '\\');
        snprintf(path, sizeof path, "%s\\%s", a, bs ? bs + 1 : b);
    } else snprintf(path, sizeof path, "%s", a);
    RET(pidl_from_path(path));
}
HLE(sh_ILCreateFromPathA) { RET(pidl_from_path(CSTR(0))); }
HLE(sh_ILCreateFromPathW) { char *s = w2u_dup(WSTR(0)); RET(pidl_from_path(s)); free(s); }
static void path_from_idlist(X86State *st, int wide) {
    char p[1200];
    if (!pidl_to_path(ARG(0), p, sizeof p)) { if (ARG(1)) put_trunc(ARG(1), 260, "", wide); RET(0); return; }
    put_trunc(ARG(1), 260, p, wide);
    RET(1);
}
HLE(sh_SHGetPathFromIDListA) { path_from_idlist(st, 0); }
HLE(sh_SHGetPathFromIDListW) { path_from_idlist(st, 1); }
HLE(sh_SHGetPathFromIDListEx) { // (pidl, path, cch, flags)
    char p[1200];
    if (!pidl_to_path(ARG(0), p, sizeof p)) { RET(0); return; }
    put_trunc(ARG(1), (size_t)ARG(2), p, 1);
    RET(1);
}
HLE(sh_SHCloneSpecialIDList) { RET(pidl_from_path(soul_csidl_path((int)ARG(1)))); }
HLE(sh_ILFindLastID) { RET(ARG(0)); } // Soul's lists have one item
HLE(sh_ILRemoveLastID) { RET(0); }
HLE(sh_ILIsEqual) {
    char a[1200], b[1200];
    RET(pidl_to_path(ARG(0), a, sizeof a) && pidl_to_path(ARG(1), b, sizeof b) && !strcasecmp(a, b));
}
HLE(sh_Shell_GetImageLists) { if (ARG(0)) wr_ptr(ARG(0), iml_system(1)); if (ARG(1)) wr_ptr(ARG(1), iml_system(0)); RET(1); }
HLE(sh_SHGetSpecialFolderLocation) { wr_ptr(ARG(2), pidl_from_path(soul_csidl_path((int)ARG(1)))); RET(0); }
HLE(sh_SHGetFolderLocation) { wr_ptr(hle_arg(st, 4), pidl_from_path(soul_csidl_path((int)ARG(1)))); RET(0); }
HLE(sh_SHParseDisplayName) {
    char *s = w2u_dup(WSTR(0));
    wr_ptr(ARG(2), pidl_from_path(s));
    if (hle_arg(st, 4)) mem_wr(hle_arg(st, 4), 4, 0);
    free(s);
    RET(0);
}

// ---- SHGetFileInfo ------------------------------------------------------------------------------

#define SHGFI_ICON 0x100
#define SHGFI_DISPLAYNAME 0x200
#define SHGFI_TYPENAME 0x400
#define SHGFI_ATTRIBUTES 0x800
#define SHGFI_EXETYPE 0x2000
#define SHGFI_SYSICONINDEX 0x4000
#define SHGFI_SMALLICON 0x1
#define SHGFI_PIDL 0x8
#define SHGFI_USEFILEATTRIBUTES 0x10

static void file_info(X86State *st, int wide) {
    uint32_t flags = (uint32_t)hle_arg(st, 4);
    uint64_t info = ARG(2);
    char win[1200] = "";
    if (flags & SHGFI_PIDL) { if (!pidl_to_path(ARG(0), win, sizeof win)) { RET(0); return; } }
    else { char *s = gstr(ARG(0), wide); snprintf(win, sizeof win, "%s", s ? s : ""); free(s); }
    char h[2048];
    host_of(win, h, sizeof h);
    struct stat sb;
    int have = stat(h, &sb) == 0;
    int dir = (flags & SHGFI_USEFILEATTRIBUTES) ? (ARG(1) & 0x10) != 0 : have && S_ISDIR(sb.st_mode);
    if (!have && !(flags & SHGFI_USEFILEATTRIBUTES) && !(flags & SHGFI_EXETYPE)) { RET(0); return; }
    const char *bs = strrchr(win, '\\');
    const char *name = bs && bs[1] ? bs + 1 : win;
    const char *dot = strrchr(name, '.');
    int exe = !dir && dot && !strcasecmp(dot, ".exe");
    int icon = dir ? 1 : exe ? 2 : 0;
    if (flags & SHGFI_EXETYPE) {
        if (!exe || !have) { RET(0); return; }
        int m = pe_machine(h);
        RET(m ? 0x0A004550u : 0); // "PE", Windows 10 subsystem version
        return;
    }
    int m32 = g_mode32, cs = wide ? 2 : 1;
    uint64_t o_icon = 0, o_iicon = m32 ? 4 : 8, o_attr = m32 ? 8 : 12, o_name = m32 ? 12 : 16, o_type = o_name + 260u * (uint64_t)cs;
    if (info) {
        if (flags & SHGFI_ICON) wr_ptr(info + o_icon, 0x60000 + 0x200 + (dir ? 0x10 : 0));
        mem_wr(info + o_iicon, 4, (uint32_t)icon);
        if (flags & SHGFI_ATTRIBUTES) mem_wr(info + o_attr, 4, dir ? 0xF0000177u : 0x40000177u);
        if (flags & SHGFI_DISPLAYNAME) {
            char disp[300];
            if (strlen(win) <= 3 && win[1] == ':') snprintf(disp, sizeof disp, "Local Disk (%c:)", toupper((unsigned char)win[0]));
            else snprintf(disp, sizeof disp, "%s", name);
            put_trunc(info + o_name, 260, disp, wide);
        }
        if (flags & SHGFI_TYPENAME) {
            char type[100];
            if (dir) snprintf(type, sizeof type, "File folder");
            else if (exe) snprintf(type, sizeof type, "Application");
            else if (dot && dot[1]) { snprintf(type, sizeof type, "%s File", dot + 1); for (char *c = type; *c && *c != ' '; c++) *c = (char)toupper((unsigned char)*c); }
            else snprintf(type, sizeof type, "File");
            put_trunc(info + o_type, 80, type, wide);
        }
    }
    if (flags & SHGFI_SYSICONINDEX) { RET(iml_system(!(flags & SHGFI_SMALLICON))); return; }
    RET(1);
}
HLE(sh_SHGetFileInfoA) { file_info(st, 0); }
HLE(sh_SHGetFileInfoW) { file_info(st, 1); }

// ---- other shell32 -----------------------------------------------------------------------------

static void create_dir_ex(X86State *st, int wide, int arg) {
    char *s = gstr(ARG(arg), wide);
    char h[2048];
    host_of(s, h, sizeof h);
    free(s);
    if (is_dir(h)) { RET(ERROR_ALREADY_EXISTS); return; }
    RET(mkdirs(h) == 0 ? ERROR_SUCCESS : ERROR_PATH_NOT_FOUND);
}
HLE(sh_SHCreateDirectoryExA) { create_dir_ex(st, 0, 1); }
HLE(sh_SHCreateDirectoryExW) { create_dir_ex(st, 1, 1); }
HLE(sh_SHCreateDirectory) { create_dir_ex(st, 1, 1); }
HLE_STUB_RET(sh_IsUserAnAdmin, 1)
HLE_STUB_RET(sh_SHChangeNotify, 0)
HLE_STUB_RET(sh_SHAddToRecentDocs, 0)
HLE(sh_SHBrowseForFolder) {
    soul_log("[api] Klasör seçme penceresi (SHBrowseForFolder) henüz yok: iptal edildi\n");
    RET(0);
}
HLE_STUB_RET(sh_Shell_NotifyIcon, 1)
HLE(sh_PathFileExistsA) { char h[2048]; host_of(CSTR(0), h, sizeof h); RET(exists(h)); }
HLE(sh_PathFileExistsW) { char *s = w2u_dup(WSTR(0)); char h[2048]; host_of(s, h, sizeof h); free(s); RET(exists(h)); }
HLE(sh_PathIsDirectoryW) { char *s = w2u_dup(WSTR(0)); char h[2048]; host_of(s, h, sizeof h); free(s); RET(is_dir(h) ? 0x10 : 0); }
HLE(sh_PathIsDirectoryA) { char h[2048]; host_of(CSTR(0), h, sizeof h); RET(is_dir(h) ? 0x10 : 0); }

// ---- ole32 ------------------------------------------------------------------------------------------

HLE_STUB_RET(ole_OleInitialize, 0)
HLE_STUB_RET(ole_OleUninitialize, 0)
static int hexv(int c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1; }
static int parse_guid(const char *s, uint8_t g[16]) {
    if (*s == '{') s++;
    static const int groups[5] = { 8, 4, 4, 4, 12 };
    uint8_t raw[16];
    int o = 0;
    for (int k = 0; k < 5; k++) {
        for (int i = 0; i < groups[k]; i += 2) {
            int a = hexv(s[0]), b = a < 0 ? -1 : hexv(s[1]);
            if (a < 0 || b < 0) return 0;
            raw[o++] = (uint8_t)(a * 16 + b);
            s += 2;
        }
        if (k < 4) { if (*s != '-') return 0; s++; }
    }
    // Data1..Data3 are little-endian numbers
    g[0] = raw[3]; g[1] = raw[2]; g[2] = raw[1]; g[3] = raw[0];
    g[4] = raw[5]; g[5] = raw[4]; g[6] = raw[7]; g[7] = raw[6];
    memcpy(g + 8, raw + 8, 8);
    return 1;
}
HLE(ole_IIDFromString) {
    char *s = ARG(0) ? w2u_dup(WSTR(0)) : NULL;
    uint8_t g[16];
    int ok = s && parse_guid(s, g);
    free(s);
    if (!ok) { RET(0x800401F3u); return; } // CO_E_CLASSSTRING
    memcpy(PTR(1), g, 16);
    RET(0);
}
HLE(ole_StringFromGUID2) {
    const uint8_t *g = PTR(0);
    uint32_t d1; uint16_t d2, d3;
    memcpy(&d1, g, 4); memcpy(&d2, g + 4, 2); memcpy(&d3, g + 6, 2);
    char s[64];
    snprintf(s, sizeof s, "{%08X-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X}", d1, d2, d3, g[8], g[9], g[10], g[11], g[12], g[13], g[14], g[15]);
    if ((uint32_t)ARG(2) < 39) { RET(0); return; }
    u2w(s, -1, PTR(1), 39);
    RET(39);
}
HLE(ole_CoCreateGuid) {
    uint8_t *g = PTR(0);
    for (int i = 0; i < 16; i++) g[i] = (uint8_t)(random() >> 7);
    g[7] = (uint8_t)((g[7] & 0x0F) | 0x40);
    g[8] = (uint8_t)((g[8] & 0x3F) | 0x80);
    RET(0);
}

// ---- INI files (GetPrivateProfileString and friends) -------------------------------------------------

typedef struct Ini { char **line; int n, cap; int utf16, bom8; } Ini;

static void ini_path(const char *file, char *host, size_t cap) {
    char win[1200];
    if (!file || !*file) file = "win.ini";
    if (!strchr(file, '\\') && !strchr(file, '/')) snprintf(win, sizeof win, "C:\\windows\\%s", file);
    else snprintf(win, sizeof win, "%s", file);
    host_of(win, host, cap);
}
static void ini_add(Ini *ini, int at, const char *s) {
    if (ini->n == ini->cap) { ini->cap = ini->cap ? ini->cap * 2 : 32; ini->line = realloc(ini->line, sizeof(char *) * (size_t)ini->cap); }
    memmove(ini->line + at + 1, ini->line + at, sizeof(char *) * (size_t)(ini->n - at));
    ini->line[at] = strdup(s);
    ini->n++;
}
static void ini_del(Ini *ini, int at) {
    free(ini->line[at]);
    memmove(ini->line + at, ini->line + at + 1, sizeof(char *) * (size_t)(ini->n - at - 1));
    ini->n--;
}
static void ini_load(const char *host, Ini *ini) {
    memset(ini, 0, sizeof *ini);
    FILE *f = fopen(host, "rb");
    if (!f) return;
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *raw = malloc((size_t)len + 2);
    len = (long)fread(raw, 1, (size_t)len, f);
    fclose(f);
    raw[len] = raw[len + 1] = 0;
    char *text;
    if (len >= 2 && (uint8_t)raw[0] == 0xFF && (uint8_t)raw[1] == 0xFE) {
        ini->utf16 = 1;
        size_t nw = (size_t)(len - 2) / 2;
        uint16_t *w = malloc((nw + 1) * 2);
        memcpy(w, raw + 2, nw * 2);
        w[nw] = 0;
        text = w2u_dup(w);
        free(w);
    } else if (len >= 3 && !memcmp(raw, "\xEF\xBB\xBF", 3)) {
        ini->bom8 = 1;
        text = strdup(raw + 3);
    } else text = strdup(raw);
    free(raw);
    for (char *s = text; *s;) {
        char *e = strchr(s, '\n');
        size_t l = e ? (size_t)(e - s) : strlen(s);
        if (l && s[l - 1] == '\r') l--;
        char *ln = malloc(l + 1);
        memcpy(ln, s, l);
        ln[l] = 0;
        ini_add(ini, ini->n, ln);
        free(ln);
        if (!e) break;
        s = e + 1;
    }
    free(text);
}
static void ini_save(const char *host, Ini *ini) {
    char parent[2048];
    snprintf(parent, sizeof parent, "%s", host);
    char *sl = strrchr(parent, '/');
    if (sl) { *sl = 0; mkdirs(parent); }
    FILE *f = fopen(host, "wb");
    if (!f) return;
    if (ini->utf16) fwrite("\xFF\xFE", 1, 2, f);
    else if (ini->bom8) fwrite("\xEF\xBB\xBF", 1, 3, f);
    for (int i = 0; i < ini->n; i++) {
        char *l = malloc(strlen(ini->line[i]) + 3);
        sprintf(l, "%s\r\n", ini->line[i]);
        if (ini->utf16) { uint16_t *w = u2w_dup(l); fwrite(w, 2, wstrlen(w), f); free(w); }
        else fputs(l, f);
        free(l);
    }
    fclose(f);
}
static void ini_free(Ini *ini) { for (int i = 0; i < ini->n; i++) free(ini->line[i]); free(ini->line); }
static const char *skip_ws(const char *s) { while (*s == ' ' || *s == '\t') s++; return s; }
// Section name of a "[name]" line (into out) or 0.
static int ini_header(const char *l, char *out, size_t cap) {
    l = skip_ws(l);
    if (*l != '[') return 0;
    const char *e = strchr(l, ']');
    if (!e) e = l + strlen(l);
    const char *b = skip_ws(l + 1);
    size_t n = (size_t)(e - b);
    while (n && (b[n - 1] == ' ' || b[n - 1] == '\t')) n--;
    snprintf(out, cap, "%.*s", (int)n, b);
    return 1;
}
// Key and value of a "key = value" line; returns 0 for comments and lines without '='.
static int ini_kv(const char *l, char *key, size_t kcap, const char **val) {
    l = skip_ws(l);
    if (*l == ';' || *l == '[' || !*l) return 0;
    const char *eq = strchr(l, '=');
    if (!eq) return 0;
    size_t n = (size_t)(eq - l);
    while (n && (l[n - 1] == ' ' || l[n - 1] == '\t')) n--;
    snprintf(key, kcap, "%.*s", (int)n, l);
    *val = skip_ws(eq + 1);
    return 1;
}
static int ini_section(Ini *ini, const char *sec) {
    char name[512];
    for (int i = 0; i < ini->n; i++) if (ini_header(ini->line[i], name, sizeof name) && !strcasecmp(name, sec)) return i;
    return -1;
}
static int ini_section_end(Ini *ini, int s) {
    char name[512];
    int i = s + 1;
    while (i < ini->n && !ini_header(ini->line[i], name, sizeof name)) i++;
    return i;
}
static int ini_key(Ini *ini, int s, const char *key) {
    char k[512];
    const char *v;
    int e = ini_section_end(ini, s);
    for (int i = s + 1; i < e; i++) if (ini_kv(ini->line[i], k, sizeof k, &v) && !strcasecmp(k, key)) return i;
    return -1;
}
static char *ini_value(const char *v) {
    size_t n = strlen(v);
    while (n && (v[n - 1] == ' ' || v[n - 1] == '\t')) n--;
    if (n >= 2 && ((v[0] == '"' && v[n - 1] == '"') || (v[0] == '\'' && v[n - 1] == '\''))) { v++; n -= 2; }
    char *r = malloc(n + 1);
    memcpy(r, v, n);
    r[n] = 0;
    return r;
}

static uint32_t profile_get(const char *sec, const char *key, const char *def, uint64_t buf, uint32_t cap, const char *file, int wide) {
    char h[2048];
    ini_path(file, h, sizeof h);
    Ini ini;
    ini_load(h, &ini);
    uint32_t r = 0;
    if (!sec || !key) {
        int n = 0, cap2 = 16;
        char **items = malloc(sizeof(char *) * (size_t)cap2);
        char name[512], k[512];
        const char *v;
        if (!sec) {
            for (int i = 0; i < ini.n; i++) if (ini_header(ini.line[i], name, sizeof name)) {
                if (n == cap2) { cap2 *= 2; items = realloc(items, sizeof(char *) * (size_t)cap2); }
                items[n++] = strdup(name);
            }
        } else {
            int s = ini_section(&ini, sec);
            int e = s >= 0 ? ini_section_end(&ini, s) : 0;
            for (int i = s + 1; s >= 0 && i < e; i++) if (ini_kv(ini.line[i], k, sizeof k, &v)) {
                if (n == cap2) { cap2 *= 2; items = realloc(items, sizeof(char *) * (size_t)cap2); }
                items[n++] = strdup(k);
            }
        }
        r = put_multi(buf, cap, items, n, wide);
        free_multi(items, n);
    } else {
        int s = ini_section(&ini, sec);
        int i = s >= 0 ? ini_key(&ini, s, key) : -1;
        char *val;
        if (i >= 0) { char k[512]; const char *v; ini_kv(ini.line[i], k, sizeof k, &v); val = ini_value(v); }
        else {
            val = strdup(def ? def : "");
            size_t n = strlen(val);
            while (n && val[n - 1] == ' ') val[--n] = 0;
        }
        r = (uint32_t)put_trunc(buf, cap, val, wide);
        free(val);
    }
    ini_free(&ini);
    return r;
}
static int profile_set(const char *sec, const char *key, const char *val, const char *file) {
    if (!sec) return 1; // flush the (non-existent) cache
    char h[2048];
    ini_path(file, h, sizeof h);
    Ini ini;
    ini_load(h, &ini);
    int s = ini_section(&ini, sec);
    if (!key) { // delete the section
        if (s >= 0) { int e = ini_section_end(&ini, s); for (int i = e - 1; i >= s; i--) ini_del(&ini, i); }
    } else if (!val) { // delete the key
        int i = s >= 0 ? ini_key(&ini, s, key) : -1;
        if (i >= 0) ini_del(&ini, i);
    } else {
        char *line = malloc(strlen(key) + strlen(val) + 2);
        sprintf(line, "%s=%s", key, val);
        if (s < 0) {
            if (ini.n && ini.line[ini.n - 1][0]) ini_add(&ini, ini.n, "");
            char *hdr = malloc(strlen(sec) + 3);
            sprintf(hdr, "[%s]", sec);
            ini_add(&ini, ini.n, hdr);
            free(hdr);
            ini_add(&ini, ini.n, line);
        } else {
            int i = ini_key(&ini, s, key);
            if (i >= 0) { free(ini.line[i]); ini.line[i] = strdup(line); }
            else {
                int e = ini_section_end(&ini, s);
                while (e > s + 1 && !*skip_ws(ini.line[e - 1])) e--; // before the blank lines that end the section
                ini_add(&ini, e, line);
            }
        }
        free(line);
    }
    ini_save(h, &ini);
    ini_free(&ini);
    return 1;
}
static void get_profile_string(X86State *st, int wide, int has_file) {
    char *sec = gstr(ARG(0), wide), *key = gstr(ARG(1), wide), *def = gstr(ARG(2), wide);
    char *file = has_file ? gstr(hle_arg(st, 5), wide) : NULL;
    RET(profile_get(sec, key, def, ARG(3), (uint32_t)hle_arg(st, 4), file, wide));
    free(sec); free(key); free(def); free(file);
}
HLE(k32_GetPrivateProfileStringA) { get_profile_string(st, 0, 1); }
HLE(k32_GetPrivateProfileStringW) { get_profile_string(st, 1, 1); }
HLE(k32_GetProfileStringA) { get_profile_string(st, 0, 0); }
HLE(k32_GetProfileStringW) { get_profile_string(st, 1, 0); }
static void write_profile_string(X86State *st, int wide, int has_file) {
    char *sec = gstr(ARG(0), wide), *key = gstr(ARG(1), wide), *val = gstr(ARG(2), wide);
    char *file = has_file ? gstr(ARG(3), wide) : NULL;
    RET(profile_set(sec, key, val, file));
    free(sec); free(key); free(val); free(file);
}
HLE(k32_WritePrivateProfileStringA) { write_profile_string(st, 0, 1); }
HLE(k32_WritePrivateProfileStringW) { write_profile_string(st, 1, 1); }
HLE(k32_WriteProfileStringA) { write_profile_string(st, 0, 0); }
HLE(k32_WriteProfileStringW) { write_profile_string(st, 1, 0); }
static void get_profile_int(X86State *st, int wide, int has_file) {
    char *sec = gstr(ARG(0), wide), *key = gstr(ARG(1), wide), *file = has_file ? gstr(ARG(3), wide) : NULL;
    int def = ARGI(2);
    char h[2048];
    ini_path(file, h, sizeof h);
    Ini ini;
    ini_load(h, &ini);
    int s = sec ? ini_section(&ini, sec) : -1;
    int i = s >= 0 && key ? ini_key(&ini, s, key) : -1;
    uint32_t r = (uint32_t)def;
    if (i >= 0) {
        char k[512];
        const char *v;
        ini_kv(ini.line[i], k, sizeof k, &v);
        char *val = ini_value(v);
        r = (uint32_t)strtol(val, NULL, 10);
        if (!isdigit((unsigned char)*skip_ws(val)) && *skip_ws(val) != '-') r = 0;
        free(val);
    }
    ini_free(&ini);
    free(sec); free(key); free(file);
    RET(r);
}
HLE(k32_GetPrivateProfileIntA) { get_profile_int(st, 0, 1); }
HLE(k32_GetPrivateProfileIntW) { get_profile_int(st, 1, 1); }
HLE(k32_GetProfileIntA) { get_profile_int(st, 0, 0); }
HLE(k32_GetProfileIntW) { get_profile_int(st, 1, 0); }
static void get_profile_section(X86State *st, int wide) {
    char *sec = gstr(ARG(0), wide), *file = gstr(ARG(3), wide);
    char h[2048];
    ini_path(file, h, sizeof h);
    Ini ini;
    ini_load(h, &ini);
    int s = sec ? ini_section(&ini, sec) : -1;
    int n = 0;
    char **items = malloc(sizeof(char *) * (size_t)(ini.n + 1));
    if (s >= 0) {
        int e = ini_section_end(&ini, s);
        for (int i = s + 1; i < e; i++) {
            const char *l = skip_ws(ini.line[i]);
            if (!*l || *l == ';') continue;
            char k[512];
            const char *v;
            if (ini_kv(l, k, sizeof k, &v)) {
                char *val = ini_value(v);
                char *item = malloc(strlen(k) + strlen(val) + 2);
                sprintf(item, "%s=%s", k, val);
                free(val);
                items[n++] = item;
            } else items[n++] = strdup(l);
        }
    }
    RET(put_multi(ARG(1), (uint32_t)ARG(2), items, n, wide));
    free_multi(items, n);
    ini_free(&ini);
    free(sec); free(file);
}
HLE(k32_GetPrivateProfileSectionA) { get_profile_section(st, 0); }
HLE(k32_GetPrivateProfileSectionW) { get_profile_section(st, 1); }
static void write_profile_section(X86State *st, int wide) {
    char *sec = gstr(ARG(0), wide), *file = gstr(ARG(2), wide);
    char **items;
    int n = read_multi(ARG(1), wide, &items);
    char h[2048];
    ini_path(file, h, sizeof h);
    Ini ini;
    ini_load(h, &ini);
    int s = sec ? ini_section(&ini, sec) : -1;
    if (s >= 0) { int e = ini_section_end(&ini, s); for (int i = e - 1; i > s; i--) ini_del(&ini, i); }
    else if (sec) {
        char *hdr = malloc(strlen(sec) + 3);
        sprintf(hdr, "[%s]", sec);
        ini_add(&ini, ini.n, hdr);
        free(hdr);
        s = ini.n - 1;
    }
    for (int i = 0; s >= 0 && i < n; i++) ini_add(&ini, s + 1 + i, items[i]);
    ini_save(h, &ini);
    ini_free(&ini);
    free_multi(items, n);
    free(sec); free(file);
    RET(1);
}
HLE(k32_WritePrivateProfileSectionA) { write_profile_section(st, 0); }
HLE(k32_WritePrivateProfileSectionW) { write_profile_section(st, 1); }
static void get_section_names(X86State *st, int wide) {
    char *file = gstr(ARG(2), wide);
    RET(profile_get(NULL, NULL, NULL, ARG(0), (uint32_t)ARG(1), file, wide));
    free(file);
}
HLE(k32_GetPrivateProfileSectionNamesA) { get_section_names(st, 0); }
HLE(k32_GetPrivateProfileSectionNamesW) { get_section_names(st, 1); }

// ---- other kernel32 ----------------------------------------------------------------------------

HLE(k32_MulDiv) { // a * b / c rounded half away from zero; -1 on overflow or division by zero
    int64_t a = ARGI(0), b = ARGI(1), c = ARGI(2);
    if (c == 0) { RET((uint32_t)-1); return; }
    int64_t p = a * b, ap = p < 0 ? -p : p, ac = c < 0 ? -c : c;
    int64_t q = (ap + ac / 2) / ac;
    if ((p < 0) != (c < 0)) q = -q;
    if (q > INT32_MAX || q < INT32_MIN) q = -1;
    RET((uint32_t)(int32_t)q);
}
static int search_one(const char *dir, const char *name, const char *ext, char *out, size_t cap) {
    char win[1400], h[2048];
    if (dir) snprintf(win, sizeof win, "%s\\%s%s", dir, name, ext);
    else snprintf(win, sizeof win, "%s%s", name, ext);
    char norm[1400];
    path_normalize_win(win, norm, sizeof norm);
    host_of(norm, h, sizeof h);
    struct stat sb;
    if (stat(h, &sb) || S_ISDIR(sb.st_mode)) return 0;
    snprintf(out, cap, "%s", norm);
    return 1;
}
static void search_path(X86State *st, int wide) {
    char *path = gstr(ARG(0), wide), *name = gstr(ARG(1), wide), *ext = gstr(ARG(2), wide);
    char found[1400] = "";
    int ok = 0;
    if (name) {
        const char *bs = strrchr(name, '\\');
        const char *base = bs ? bs + 1 : name;
        const char *e = ext && !strchr(base, '.') ? ext : "";
        if (strchr(name, '\\') || strchr(name, ':')) ok = search_one(NULL, name, e, found, sizeof found);
        else {
            char dirs[4096];
            if (path) snprintf(dirs, sizeof dirs, "%s", path);
            else {
                char exe_dir[600];
                snprintf(exe_dir, sizeof exe_dir, "%s", g_proc->exe_win);
                char *b = strrchr(exe_dir, '\\');
                if (b) *b = 0;
                const char *envp = env_get_value("PATH");
                snprintf(dirs, sizeof dirs, "%s;%s;C:\\windows\\system32;C:\\windows;%s", exe_dir, g_proc->cwd_win, envp ? envp : "");
            }
            for (char *d = strtok(dirs, ";"); d && !ok; d = strtok(NULL, ";")) if (*d) ok = search_one(d, name, e, found, sizeof found);
        }
    }
    free(path); free(name); free(ext);
    if (!ok) FAIL(ERROR_FILE_NOT_FOUND);
    uint64_t r = put_str(hle_arg(st, 4), (uint32_t)ARG(3), found, wide);
    if (hle_arg(st, 5) && r < (uint32_t)ARG(3)) {
        const char *bs = strrchr(found, '\\');
        size_t off = bs ? (size_t)(bs + 1 - found) : 0;
        if (wide) off = u2w(found, (long)off, NULL, 0);
        wr_ptr(hle_arg(st, 5), hle_arg(st, 4) + off * (wide ? 2 : 1));
    }
    RET(r);
}
HLE(k32_SearchPathA) { search_path(st, 0); }
HLE(k32_SearchPathW) { search_path(st, 1); }
static void disk_space(uint64_t *free_b, uint64_t *total_b) {
    struct statvfs sv;
    if (statvfs(g_proc->drive_c, &sv) == 0) {
        *free_b = (uint64_t)sv.f_bavail * sv.f_frsize;
        *total_b = (uint64_t)sv.f_blocks * sv.f_frsize;
    } else { *free_b = 64ull << 30; *total_b = 128ull << 30; }
}
HLE(k32_GetDiskFreeSpaceA) { // (root, sectorsPerCluster, bytesPerSector, freeClusters, totalClusters)
    uint64_t fb, tb;
    disk_space(&fb, &tb);
    uint64_t fc = fb / 4096, tc = tb / 4096;
    if (ARG(1)) mem_wr(ARG(1), 4, 8);
    if (ARG(2)) mem_wr(ARG(2), 4, 512);
    if (ARG(3)) mem_wr(ARG(3), 4, fc > 0xFFFFFFFFu ? 0xFFFFFFFFu : fc);
    if (hle_arg(st, 4)) mem_wr(hle_arg(st, 4), 4, tc > 0xFFFFFFFFu ? 0xFFFFFFFFu : tc);
    RET(1);
}
HLE(k32_GetDiskFreeSpaceExA) { // (dir, freeToCaller, total, totalFree)
    uint64_t fb, tb;
    disk_space(&fb, &tb);
    if (ARG(1)) mem_wr(ARG(1), 8, fb);
    if (ARG(2)) mem_wr(ARG(2), 8, tb);
    if (ARG(3)) mem_wr(ARG(3), 8, fb);
    RET(1);
}

// ---- user32 / advapi32 ----------------------------------------------------------------------------

HLE(u32_CharPrevW) { const uint16_t *s = WSTR(0), *p = WSTR(1); RET(p > s ? p - 1 : s); }
static void message_box_indirect(X86State *st, int wide) {
    // MSGBOXPARAMS: cbSize hwndOwner hInstance lpszText lpszCaption dwStyle ...
    uint64_t m = ARG(0);
    const char *L = "4PPPP4";
    char *text = gstr(gs_get(m, L, 3), wide), *cap = gstr(gs_get(m, L, 4), wide);
    int r = soul_message_box(cap ? cap : "", text ? text : "", (unsigned)gs_get(m, L, 5));
    free(text); free(cap);
    RET(r);
}
HLE(u32_MessageBoxIndirectA) { message_box_indirect(st, 0); }
HLE(u32_MessageBoxIndirectW) { message_box_indirect(st, 1); }
HLE(u32_ExitWindowsEx) {
    soul_log("[Soul] Program bilgisayarı yeniden başlatmak istedi; Soul'da gerekmez, atlandı.\n");
    set_last_error(st, 1314); // ERROR_PRIVILEGE_NOT_HELD
    RET(0);
}
HLE(adv_AdjustTokenPrivileges) { set_last_error(st, 0); RET(1); }
HLE(adv_LookupPrivilegeValue) { if (ARG(2)) { mem_wr(ARG(2), 4, 20); mem_wr(ARG(2) + 4, 4, 0); } RET(1); }
HLE(adv_CheckTokenMembership) { if (ARG(2)) mem_wr(ARG(2), 4, 1); RET(1); }
HLE(adv_AllocateAndInitializeSid) {
    // (authority, count, sub0..sub7, &sid): SID { rev, count, authority[6], sub[count] }
    int n = (int)(ARG(1) & 0xFF);
    uint8_t *sid = g_calloc(1, 8 + 4 * (size_t)(n > 8 ? 8 : n));
    sid[0] = 1; sid[1] = (uint8_t)n;
    memcpy(sid + 2, PTR(0), 6);
    for (int i = 0; i < n && i < 8; i++) { uint32_t v = (uint32_t)hle_arg(st, 2 + i); memcpy(sid + 8 + 4 * i, &v, 4); }
    wr_ptr(hle_arg(st, 10), H2G(sid));
    RET(1);
}
HLE(adv_FreeSid) { g_free(PTR(0)); RET(0); }
HLE(adv_EqualSid) {
    const uint8_t *a = PTR(0), *b = PTR(1);
    RET(a && b && a[1] == b[1] && !memcmp(a, b, 8 + 4 * (size_t)a[1]));
}
HLE(adv_GetLengthSid) { const uint8_t *a = PTR(0); RET(a ? 8 + 4 * (uint32_t)a[1] : 0); }
HLE(adv_IsValidSid) { const uint8_t *a = PTR(0); RET(a && a[0] == 1); }

// ---- user interface languages (Delphi's runtime calls these directly on Vista and later) ---------------

static void ui_languages(X86State *st, int flags_arg, int num_arg, int buf_arg, int cch_arg) {
    uint32_t flags = (uint32_t)hle_arg(st, flags_arg);
    const char *lang = (flags & 0x4) ? "0409" : "en-US"; // MUI_LANGUAGE_ID or MUI_LANGUAGE_NAME
    uint32_t need = (uint32_t)strlen(lang) + 2;           // double-NUL list
    if (num_arg >= 0 && hle_arg(st, num_arg)) mem_wr(hle_arg(st, num_arg), 4, 1);
    uint64_t cchp = hle_arg(st, cch_arg), buf = hle_arg(st, buf_arg);
    uint32_t cap = cchp ? (uint32_t)mem_rd(cchp, 4) : 0;
    if (cchp) mem_wr(cchp, 4, need);
    if (!buf) { RET(1); return; }
    if (cap < need) FAIL(ERROR_INSUFFICIENT_BUFFER);
    uint16_t *w = (uint16_t *)G2H(buf);
    for (uint32_t i = 0; i + 2 < need; i++) w[i] = (uint16_t)lang[i];
    w[need - 2] = 0; w[need - 1] = 0;
    RET(1);
}
HLE(k32_GetThreadPreferredUILanguages) { ui_languages(st, 0, 1, 2, 3); }
HLE(k32_GetUserPreferredUILanguages) { ui_languages(st, 0, 1, 2, 3); }
HLE(k32_GetSystemPreferredUILanguages) { ui_languages(st, 0, 1, 2, 3); }
HLE(k32_GetProcessPreferredUILanguages) { ui_languages(st, 0, 1, 2, 3); }
HLE(k32_SetThreadPreferredUILanguages) { if (hle_arg(st, 2)) mem_wr(hle_arg(st, 2), 4, 1); RET(1); }
HLE(k32_SetProcessPreferredUILanguages) { RET(1); }
HLE(k32_GetThreadUILanguage) { RET(0x409); }
HLE(k32_SetThreadUILanguage) { RET(ARG(0) ? ARG(0) : 0x409); }
HLE(k32_SetThreadLocale) { RET(1); }
HLE(k32_GetThreadLocale) { RET(0x409); }
HLE(k32_SetSearchPathMode) { RET(1); }
HLE(k32_SetProcessDEPPolicy) { RET(1); }
HLE(k32_GetProcessDEPPolicy) { if (ARG(1)) mem_wr(ARG(1), 4, 1); if (ARG(2)) mem_wr(ARG(2), 4, 1); RET(1); }

// ---- atoms (one table for global and local atoms, shared by every process) -------------------------

typedef struct Atom { char name[256]; int refs; } Atom;
static Atom g_atoms[0x4000 - 1];
static int g_atom_n;
const char *atom_name(uint32_t a) {
    if (a >= 0xC000 && a - 0xC000 < (uint32_t)g_atom_n && g_atoms[a - 0xC000].refs > 0) return g_atoms[a - 0xC000].name;
    return NULL;
}
static uint32_t atom_find(const char *s) {
    if (s[0] == '#') { long v = strtol(s + 1, NULL, 10); return v > 0 && v < 0xC000 ? (uint32_t)v : 0; }
    for (int i = 0; i < g_atom_n; i++) if (g_atoms[i].refs > 0 && !strcasecmp(g_atoms[i].name, s)) return 0xC000u + (uint32_t)i;
    return 0;
}
static uint32_t atom_add(const char *s) {
    uint32_t a = atom_find(s);
    if (a >= 0xC000) { g_atoms[a - 0xC000].refs++; return a; }
    if (a) return a;
    int slot = -1;
    for (int i = 0; i < g_atom_n; i++) if (g_atoms[i].refs <= 0) { slot = i; break; }
    if (slot < 0) { if (g_atom_n >= (int)(sizeof g_atoms / sizeof g_atoms[0])) return 0; slot = g_atom_n++; }
    snprintf(g_atoms[slot].name, sizeof g_atoms[slot].name, "%s", s);
    g_atoms[slot].refs = 1;
    return 0xC000u + (uint32_t)slot;
}
static char *atom_arg(X86State *st, int wide) {
    if (ARG(0) < 0x10000) { char b[16]; snprintf(b, sizeof b, "#%u", (unsigned)ARG(0)); return strdup(b); }
    return gstr(ARG(0), wide);
}
static void add_atom(X86State *st, int wide) { char *s = atom_arg(st, wide); RET(s ? atom_add(s) : 0); free(s); }
static void find_atom(X86State *st, int wide) { char *s = atom_arg(st, wide); uint32_t a = s ? atom_find(s) : 0; free(s); if (!a) FAIL(ERROR_FILE_NOT_FOUND); RET(a); }
static void atom_get_name(X86State *st, int wide) {
    uint32_t a = (uint32_t)(ARG(0) & 0xFFFF);
    char b[16];
    const char *n = atom_name(a);
    if (!n && a && a < 0xC000) { snprintf(b, sizeof b, "#%u", a); n = b; }
    if (!n) FAIL(ERROR_INVALID_HANDLE);
    RET(put_trunc(ARG(1), (size_t)(uint32_t)ARG(2), n, wide));
}
HLE(k32_GlobalAddAtomA) { add_atom(st, 0); }
HLE(k32_GlobalAddAtomW) { add_atom(st, 1); }
HLE(k32_GlobalFindAtomA) { find_atom(st, 0); }
HLE(k32_GlobalFindAtomW) { find_atom(st, 1); }
HLE(k32_GlobalGetAtomNameA) { atom_get_name(st, 0); }
HLE(k32_GlobalGetAtomNameW) { atom_get_name(st, 1); }
HLE(k32_GlobalDeleteAtom) {
    uint32_t a = (uint32_t)(ARG(0) & 0xFFFF);
    if (a >= 0xC000 && a - 0xC000 < (uint32_t)g_atom_n && g_atoms[a - 0xC000].refs > 0) g_atoms[a - 0xC000].refs--;
    RET(0);
}

// ---- user32 case conversion (buffers) ------------------------------------------------------------------

HLE(u32_CharUpperBuffW) { uint16_t *p = PTR(0); uint32_t n = (uint32_t)ARG(1); for (uint32_t i = 0; p && i < n; i++) p[i] = (uint16_t)uc_upper(p[i]); RET(n); }
HLE(u32_CharLowerBuffW) { uint16_t *p = PTR(0); uint32_t n = (uint32_t)ARG(1); for (uint32_t i = 0; p && i < n; i++) p[i] = (uint16_t)uc_lower(p[i]); RET(n); }
HLE(u32_CharUpperBuffA) { char *p = PTR(0); uint32_t n = (uint32_t)ARG(1); for (uint32_t i = 0; p && i < n; i++) p[i] = (char)toupper((unsigned char)p[i]); RET(n); }
HLE(u32_CharLowerBuffA) { char *p = PTR(0); uint32_t n = (uint32_t)ARG(1); for (uint32_t i = 0; p && i < n; i++) p[i] = (char)tolower((unsigned char)p[i]); RET(n); }
HLE(u32_IsCharUpperW) { uint32_t c = (uint16_t)ARG(0); RET(uc_lower(c) != c); }
HLE(u32_IsCharLowerW) { uint32_t c = (uint16_t)ARG(0); RET(uc_upper(c) != c); }
HLE(k32_EnumCalendarInfo) { RET(1); }
uint64_t k32_virtual_query(uint64_t addr, uint8_t *b); // hle_kernel32.c
HLE(k32_VirtualQueryEx) { // the calling process only
    if (ARG(0) != (uint64_t)-1 && ARG(0) != 0) {
        HObj *o = h_get(ARG(0));
        if (!o || o->type != H_PROCESS || o->ptr != g_proc) FAIL(ERROR_ACCESS_DENIED);
    }
    RET(k32_virtual_query(ARG(1), PTR(2)));
}

// ---- wtsapi32 / misc system information ------------------------------------------------------------------

HLE(wts_RegisterSessionNotification) { RET(1); }
HLE(wts_QuerySessionInformation) { RET(0); }
HLE(k32_GetLogicalProcessorInformation) { // one core with one logical processor
    uint64_t buf = ARG(0), lenp = ARG(1);
    uint32_t need = g_mode32 ? 24 : 32; // SYSTEM_LOGICAL_PROCESSOR_INFORMATION
    uint32_t cap = lenp ? (uint32_t)mem_rd(lenp, 4) : 0;
    if (lenp) mem_wr(lenp, 4, need);
    if (!buf || cap < need) FAIL(ERROR_INSUFFICIENT_BUFFER);
    memset(G2H(buf), 0, need);
    wr_ptr(buf, 1);                     // ProcessorMask
    mem_wr(buf + (uint64_t)PSZ, 4, 0);  // RelationProcessorCore
    RET(1);
}

// ---- uxtheme: visual styles are off (programs then draw the classic look themselves) -----------------------

#define E_FAIL_HR 0x80004005u
HLE(ux_False) { RET(0); }
HLE(ux_Ok) { RET(0); }
HLE(ux_Fail) { RET(E_FAIL_HR); }
HLE(ux_BeginBufferedPaint) { // (hdcTarget, rect, format, params, &hdc): no buffering, paint straight into the target
    if (hle_arg(st, 4)) wr_ptr(hle_arg(st, 4), ARG(0));
    RET(0);
}
HLE(ux_GetThemeSysColor) { RET(gui_sys_color((int)ARG(1))); }
HLE(ux_GetCurrentThemeName) { if (ARG(0) && ARG(1)) mem_wr(ARG(0), 2, 0); RET(E_FAIL_HR); }
HLE(ux_GetThemeAppProperties) { RET(0); }

// ---- security: the user is an elevated administrator; descriptors and ACLs are accepted -------------------

// S-1-5-21-1-2-3-1000 (the user) and S-1-5-32-544 (Administrators)
static uint64_t make_sid(int admin) {
    uint8_t *s = g_calloc(1, 28);
    s[0] = 1; s[7] = 5;
    if (admin) { s[1] = 2; uint32_t v[2] = { 32, 544 }; memcpy(s + 8, v, 8); }
    else { s[1] = 5; uint32_t v[5] = { 21, 1, 2, 3, 1000 }; memcpy(s + 8, v, 20); }
    return H2G(s);
}
HLE(adv_GetTokenInformation) { // (token, class, buffer, length, &returned)
    uint32_t cls = (uint32_t)ARG(1), cap = (uint32_t)ARG(3);
    uint64_t buf = ARG(2), ret = hle_arg(st, 4);
    uint32_t need;
    switch (cls) {
    case 1: case 4: case 5: need = (uint32_t)(2 * PSZ) + 28; break;     // TokenUser / Owner / PrimaryGroup
    case 2: need = 4 + (uint32_t)(2 * PSZ) + 28; break;                  // TokenGroups: one group
    case 18: case 20: case 8: case 9: need = 4; break;                  // ElevationType, Elevation, Type, ImpersonationLevel
    case 25: need = (uint32_t)(2 * PSZ) + 12; break;                     // TokenIntegrityLevel
    default: if (ret) mem_wr(ret, 4, 0); FAIL(ERROR_INVALID_PARAMETER);
    }
    if (ret) mem_wr(ret, 4, need);
    if (!buf || cap < need) FAIL(ERROR_INSUFFICIENT_BUFFER);
    memset(G2H(buf), 0, need);
    switch (cls) {
    case 1: case 4: case 5: { uint64_t sid = buf + (uint64_t)(2 * PSZ); memcpy(G2H(sid), G2H(make_sid(cls == 5 ? 0 : 0)), 28); wr_ptr(buf, sid); break; }
    case 2: { uint64_t sid = buf + 4 + (uint64_t)(2 * PSZ); mem_wr(buf, 4, 1); memcpy(G2H(sid), G2H(make_sid(1)), 16); wr_ptr(buf + (g_mode32 ? 4 : 8), sid); mem_wr(buf + (g_mode32 ? 8 : 16), 4, 0xF); break; }
    case 18: mem_wr(buf, 4, 2); break;  // TokenElevationTypeFull
    case 20: mem_wr(buf, 4, 1); break;  // elevated
    case 8: mem_wr(buf, 4, 1); break;   // TokenPrimary
    case 9: mem_wr(buf, 4, 2); break;
    case 25: { // S-1-16-12288 (high integrity)
        uint64_t sid = buf + (uint64_t)(2 * PSZ);
        uint8_t s[12] = { 1, 1, 0, 0, 0, 0, 0, 16, 0, 0x30, 0, 0 };
        memcpy(G2H(sid), s, 12);
        wr_ptr(buf, sid);
        mem_wr(buf + (uint64_t)PSZ, 4, 0x20);
        break;
    }
    }
    RET(1);
}
HLE(adv_True) { RET(1); }
HLE(adv_Success) { RET(0); }
HLE(adv_InitializeSecurityDescriptor) { if (ARG(0)) { memset(PTR(0), 0, g_mode32 ? 20 : 40); ((uint8_t *)PTR(0))[0] = 1; } RET(1); }
HLE(adv_GetSecurityDescriptorDacl) { if (ARG(1)) mem_wr(ARG(1), 4, 0); if (ARG(2)) wr_ptr(ARG(2), 0); if (ARG(3)) mem_wr(ARG(3), 4, 0); RET(1); }
HLE(adv_ConvertStringSecurityDescriptor) { // (string, revision, &descriptor, &size)
    uint64_t sd = H2G(g_calloc(1, 64));
    ((uint8_t *)G2H(sd))[0] = 1;
    if (ARG(2)) wr_ptr(ARG(2), sd);
    if (ARG(3)) mem_wr(ARG(3), 4, 64);
    RET(1);
}
HLE(adv_SetEntriesInAcl) { if (hle_arg(st, 3)) wr_ptr(hle_arg(st, 3), H2G(g_calloc(1, 8))); RET(0); }
HLE(adv_GetNamedSecurityInfo) { for (int i = 3; i < 8; i++) if (hle_arg(st, i)) wr_ptr(hle_arg(st, i), 0); RET(0); }
static void sid_string(X86State *st, int wide) { // (sid, &string): LocalFree'd by the caller
    const uint8_t *s = PTR(0);
    if (!s) FAIL(ERROR_INVALID_PARAMETER);
    char out[200];
    uint64_t auth = 0;
    for (int i = 2; i < 8; i++) auth = auth << 8 | s[i];
    int n = snprintf(out, sizeof out, "S-%u-%llu", s[0], (unsigned long long)auth);
    for (int i = 0; i < s[1] && i < 15; i++) { uint32_t v; memcpy(&v, s + 8 + 4 * i, 4); n += snprintf(out + n, sizeof out - (size_t)n, "-%u", v); }
    wr_ptr(ARG(1), wide ? H2G(u2w_gdup(out)) : H2G(g_strdup(out)));
    RET(1);
}
HLE(adv_ConvertSidToStringSidW) { sid_string(st, 1); }
HLE(adv_ConvertSidToStringSidA) { sid_string(st, 0); }
HLE(adv_ConvertStringSidToSid) { if (ARG(1)) wr_ptr(ARG(1), make_sid(0)); RET(1); }
static void lookup_account_sid(X86State *st, int wide) { // (system, sid, name, &cchName, domain, &cchDomain, &use)
    const char *name = "soul", *dom = "IPAD";
    uint64_t np = ARG(3), dp = hle_arg(st, 5);
    uint32_t nc = np ? (uint32_t)mem_rd(np, 4) : 0, dc = dp ? (uint32_t)mem_rd(dp, 4) : 0;
    if (nc < 5 || dc < 5) { if (np) mem_wr(np, 4, 5); if (dp) mem_wr(dp, 4, 5); FAIL(ERROR_INSUFFICIENT_BUFFER); }
    put_trunc(ARG(2), nc, name, wide);
    put_trunc(hle_arg(st, 4), dc, dom, wide);
    mem_wr(np, 4, 4); mem_wr(dp, 4, 4);
    if (hle_arg(st, 6)) mem_wr(hle_arg(st, 6), 4, 1); // SidTypeUser
    RET(1);
}
HLE(adv_LookupAccountSidW) { lookup_account_sid(st, 1); }
HLE(adv_LookupAccountSidA) { lookup_account_sid(st, 0); }

// ---- Restart Manager: no other program holds the installer's files ---------------------------------------

HLE(rm_StartSession) { if (ARG(0)) mem_wr(ARG(0), 4, 1); if (ARG(2)) mem_wr(ARG(2), 2, 0); RET(0); }
HLE(rm_Success) { RET(0); }
HLE(rm_GetList) { if (ARG(1)) mem_wr(ARG(1), 4, 0); if (ARG(2)) mem_wr(ARG(2), 4, 0); if (hle_arg(st, 4)) mem_wr(hle_arg(st, 4), 4, 0); RET(0); }

// ---- DPI: Soul's screen is 96 DPI and every program is DPI aware ------------------------------------------

HLE(shc_GetProcessDpiAwareness) { if (ARG(1)) mem_wr(ARG(1), 4, 1); RET(0); }
HLE(u_GetAwarenessFromDpiAwarenessContext) { RET(1); }
HLE(u_IsValidDpiAwarenessContext) { RET(1); }
HLE(u_AreDpiAwarenessContextsEqual) { RET(ARG(0) == ARG(1)); }
HLE(u_GetWindowDpiAwarenessContext) { RET((uint64_t)-2); }
HLE(u_GetDpi96) { RET(96); }

// ---- Interlocked* (exported by 32-bit kernel32; the GIL makes them atomic) -----------------------------

HLE(k32_InterlockedIncrement) { uint32_t v = (uint32_t)mem_rd(ARG(0), 4) + 1; mem_wr(ARG(0), 4, v); RET((uint64_t)(int64_t)(int32_t)v); }
HLE(k32_InterlockedDecrement) { uint32_t v = (uint32_t)mem_rd(ARG(0), 4) - 1; mem_wr(ARG(0), 4, v); RET((uint64_t)(int64_t)(int32_t)v); }
HLE(k32_InterlockedExchange) { uint32_t o = (uint32_t)mem_rd(ARG(0), 4); mem_wr(ARG(0), 4, (uint32_t)ARG(1)); RET(o); }
HLE(k32_InterlockedExchangeAdd) { uint32_t o = (uint32_t)mem_rd(ARG(0), 4); mem_wr(ARG(0), 4, o + (uint32_t)ARG(1)); RET(o); }
HLE(k32_InterlockedCompareExchange) { // (dest, exchange, comparand)
    uint32_t o = (uint32_t)mem_rd(ARG(0), 4);
    if (o == (uint32_t)ARG(2)) mem_wr(ARG(0), 4, (uint32_t)ARG(1));
    RET(o);
}
HLE(k32_InterlockedCompareExchange64) { // (dest, exchange, comparand): 64-bit values (two stack slots each in 32-bit programs)
    uint64_t ex = g_mode32 ? (ARG(1) | (ARG(2) << 32)) : ARG(1), cmp = g_mode32 ? (ARG(3) | (hle_arg(st, 4) << 32)) : ARG(2);
    uint64_t o = mem_rd(ARG(0), 8);
    if (o == cmp) mem_wr(ARG(0), 8, ex);
    RET(o);
}

// ---- gdiplus (not available yet): startup fails cleanly ---------------------------------------------------

HLE(gp_GdiplusStartup) { if (ARG(0)) wr_ptr(ARG(0), 0); RET(6); } // NotImplemented
HLE(gp_GdiplusShutdown) { RET(0); }
HLE(k32_SetHandleCount) { RET(ARG(0)); }

// ---- resources -------------------------------------------------------------------------------------

#define ERROR_RESOURCE_TYPE_NOT_FOUND 1813
HLE(k32_FindResourceA) { uint64_t e = pe_resource_entry(ARG(0), ARG(2), ARG(1), 0); if (!e) FAIL(ERROR_RESOURCE_TYPE_NOT_FOUND); RET(e); }
HLE(k32_FindResourceW) { uint64_t e = pe_resource_entry(ARG(0), ARG(2), ARG(1), 1); if (!e) FAIL(ERROR_RESOURCE_TYPE_NOT_FOUND); RET(e); }
HLE(k32_FindResourceExA) { uint64_t e = pe_resource_entry(ARG(0), ARG(1), ARG(2), 0); if (!e) FAIL(ERROR_RESOURCE_TYPE_NOT_FOUND); RET(e); }
HLE(k32_FindResourceExW) { uint64_t e = pe_resource_entry(ARG(0), ARG(1), ARG(2), 1); if (!e) FAIL(ERROR_RESOURCE_TYPE_NOT_FOUND); RET(e); }
HLE(k32_LoadResource) { RET(pe_resource_data(ARG(0), ARG(1), NULL)); }
HLE(k32_LockResource) { RET(ARG(0)); }
HLE(k32_SizeofResource) { uint32_t n = 0; if (!ARG(1)) { RET(0); return; } pe_resource_data(ARG(0), ARG(1), &n); RET(n); }
HLE(k32_FreeResource) { RET(0); }

// ---- netapi32 -------------------------------------------------------------------------------------------

HLE(net_NetWkstaGetInfo) { // level 100: WKSTA_INFO_100 { platform_id, computername, langroup, ver_major, ver_minor }
    if ((uint32_t)ARG(1) != 100 || !ARG(2)) { RET(124); return; } // ERROR_INVALID_LEVEL
    const char *L = "4PP44";
    uint64_t b = H2G(g_calloc(1, gs_size(L)));
    gs_set(b, L, 0, 500);
    gs_set(b, L, 1, H2G(u2w_gdup("IPAD")));
    gs_set(b, L, 2, H2G(u2w_gdup("WORKGROUP")));
    gs_set(b, L, 3, 10);
    gs_set(b, L, 4, 0);
    wr_ptr(ARG(2), b);
    RET(0);
}
HLE(net_NetApiBufferFree) { g_free(PTR(0)); RET(0); }

// ---- ordinal-only exports ---------------------------------------------------------------------------

typedef struct OrdName { const char *dll; int ord; const char *name; } OrdName;
static const OrdName k_ordinals[] = {
    { "comctl32", 17, "InitCommonControls" }, { "comctl32", 410, "SetWindowSubclass" }, { "comctl32", 412, "RemoveWindowSubclass" },
    { "comctl32", 413, "DefSubclassProc" }, { "comctl32", 345, "TaskDialogIndirect" }, { "comctl32", 344, "TaskDialog" },
    { "comctl32", 380, "LoadIconMetric" }, { "comctl32", 381, "LoadIconWithScaleDown" }, { "comctl32", 236, "Str_SetPtrW" },
    { "shell32", 680, "IsUserAnAdmin" }, { "shell32", 155, "ILFree" }, { "shell32", 18, "ILClone" }, { "shell32", 25, "ILCombine" },
    { "shell32", 89, "SHCloneSpecialIDList" }, { "shell32", 16, "ILFindLastID" }, { "shell32", 17, "ILRemoveLastID" },
    { "shell32", 21, "ILIsEqual" }, { "shell32", 162, "SHSimpleIDListFromPath" }, { "shell32", 190, "ILCreateFromPathA" },
    { "shell32", 189, "ILCreateFromPathA" }, { "shell32", 45, "PathFileExistsA" }, { "shell32", 71, "Shell_GetImageLists" },
    { "shell32", 152, "ILGetSize" }, { "shell32", 157, "ILCreateFromPathW" }, { "shell32", 165, "SHCreateDirectory" },
    { "shell32", 195, "SHFree" }, { "shell32", 196, "SHAlloc" },
    { "oleaut32", 2, "SysAllocString" }, { "oleaut32", 4, "SysAllocStringLen" }, { "oleaut32", 6, "SysFreeString" },
    { "oleaut32", 7, "SysStringLen" }, { "oleaut32", 8, "VariantInit" }, { "oleaut32", 9, "VariantClear" },
    { "oleaut32", 10, "VariantCopy" }, { "oleaut32", 12, "VariantChangeType" }, { "oleaut32", 149, "SysStringByteLen" },
    { "oleaut32", 150, "SysAllocStringByteLen" }, { "oleaut32", 3, "SysReAllocString" }, { "oleaut32", 5, "SysReAllocStringLen" },
    { "shlwapi", 437, "IsOS" },
    { NULL, 0, NULL }
};
const char *hle_ordinal_name(const char *dll, int ord) {
    char key[64];
    size_t i = 0;
    for (; dll[i] && dll[i] != '.' && i < sizeof key - 1; i++) key[i] = (char)tolower((unsigned char)dll[i]);
    key[i] = 0;
    for (const OrdName *o = k_ordinals; o->dll; o++) if (o->ord == ord && !strcmp(o->dll, key)) return o->name;
    return NULL;
}
HLE(sh_SHAlloc) { RET(g_malloc(ARG(0) ? ARG(0) : 1)); }

const HleEntry hle_shell_table[] = {
    { "SHFileOperationA", sh_SHFileOperationA, NULL }, { "SHFileOperationW", sh_SHFileOperationW, NULL },
    { "ILFree", sh_ILFree, NULL }, { "SHFree", sh_ILFree, NULL }, { "SHAlloc", sh_SHAlloc, NULL }, { "ILClone", sh_ILClone, NULL },
    { "ILGetSize", sh_ILGetSize, NULL }, { "SHCloneSpecialIDList", sh_SHCloneSpecialIDList, NULL }, { "ILFindLastID", sh_ILFindLastID, NULL },
    { "ILRemoveLastID", sh_ILRemoveLastID, NULL }, { "ILIsEqual", sh_ILIsEqual, NULL }, { "SHSimpleIDListFromPath", sh_ILCreateFromPathW, NULL },
    { "Shell_GetImageLists", sh_Shell_GetImageLists, NULL }, { "ILCombine", sh_ILCombine, NULL },
    { "ILCreateFromPathA", sh_ILCreateFromPathA, NULL }, { "ILCreateFromPathW", sh_ILCreateFromPathW, NULL },
    { "SHGetPathFromIDListA", sh_SHGetPathFromIDListA, NULL }, { "SHGetPathFromIDListW", sh_SHGetPathFromIDListW, NULL },
    { "SHGetPathFromIDList", sh_SHGetPathFromIDListA, NULL }, { "SHGetPathFromIDListEx", sh_SHGetPathFromIDListEx, NULL },
    { "SHGetSpecialFolderLocation", sh_SHGetSpecialFolderLocation, NULL }, { "SHGetFolderLocation", sh_SHGetFolderLocation, NULL },
    { "SHParseDisplayName", sh_SHParseDisplayName, NULL },
    { "SHGetFileInfoA", sh_SHGetFileInfoA, NULL }, { "SHGetFileInfoW", sh_SHGetFileInfoW, NULL }, { "SHGetFileInfo", sh_SHGetFileInfoA, NULL },
    { "SHCreateDirectoryExA", sh_SHCreateDirectoryExA, NULL }, { "SHCreateDirectoryExW", sh_SHCreateDirectoryExW, NULL },
    { "SHCreateDirectory", sh_SHCreateDirectory, NULL }, { "IsUserAnAdmin", sh_IsUserAnAdmin, NULL },
    { "SHChangeNotify", sh_SHChangeNotify, NULL }, { "SHAddToRecentDocs", sh_SHAddToRecentDocs, NULL },
    { "SHBrowseForFolderA", sh_SHBrowseForFolder, NULL }, { "SHBrowseForFolderW", sh_SHBrowseForFolder, NULL },
    { "SHBrowseForFolder", sh_SHBrowseForFolder, NULL },
    { "Shell_NotifyIconA", sh_Shell_NotifyIcon, NULL }, { "Shell_NotifyIconW", sh_Shell_NotifyIcon, NULL },
    { "PathFileExistsA", sh_PathFileExistsA, NULL }, { "PathFileExistsW", sh_PathFileExistsW, NULL },
    { "PathIsDirectoryA", sh_PathIsDirectoryA, NULL }, { "PathIsDirectoryW", sh_PathIsDirectoryW, NULL },
    { "OleInitialize", ole_OleInitialize, NULL }, { "OleUninitialize", ole_OleUninitialize, NULL },
    { "IIDFromString", ole_IIDFromString, NULL }, { "CLSIDFromString", ole_IIDFromString, NULL },
    { "StringFromGUID2", ole_StringFromGUID2, NULL }, { "CoCreateGuid", ole_CoCreateGuid, NULL },
    { "GetPrivateProfileStringA", k32_GetPrivateProfileStringA, NULL }, { "GetPrivateProfileStringW", k32_GetPrivateProfileStringW, NULL },
    { "GetProfileStringA", k32_GetProfileStringA, NULL }, { "GetProfileStringW", k32_GetProfileStringW, NULL },
    { "WritePrivateProfileStringA", k32_WritePrivateProfileStringA, NULL }, { "WritePrivateProfileStringW", k32_WritePrivateProfileStringW, NULL },
    { "WriteProfileStringA", k32_WriteProfileStringA, NULL }, { "WriteProfileStringW", k32_WriteProfileStringW, NULL },
    { "GetPrivateProfileIntA", k32_GetPrivateProfileIntA, NULL }, { "GetPrivateProfileIntW", k32_GetPrivateProfileIntW, NULL },
    { "GetProfileIntA", k32_GetProfileIntA, NULL }, { "GetProfileIntW", k32_GetProfileIntW, NULL },
    { "GetPrivateProfileSectionA", k32_GetPrivateProfileSectionA, NULL }, { "GetPrivateProfileSectionW", k32_GetPrivateProfileSectionW, NULL },
    { "WritePrivateProfileSectionA", k32_WritePrivateProfileSectionA, NULL }, { "WritePrivateProfileSectionW", k32_WritePrivateProfileSectionW, NULL },
    { "GetPrivateProfileSectionNamesA", k32_GetPrivateProfileSectionNamesA, NULL }, { "GetPrivateProfileSectionNamesW", k32_GetPrivateProfileSectionNamesW, NULL },
    { "MulDiv", k32_MulDiv, NULL }, { "SearchPathA", k32_SearchPathA, NULL }, { "SearchPathW", k32_SearchPathW, NULL },
    { "GetDiskFreeSpaceA", k32_GetDiskFreeSpaceA, NULL }, { "GetDiskFreeSpaceW", k32_GetDiskFreeSpaceA, NULL },
    { "GetDiskFreeSpaceExA", k32_GetDiskFreeSpaceExA, NULL }, { "GetDiskFreeSpaceExW", k32_GetDiskFreeSpaceExA, NULL },
    { "CharPrevW", u32_CharPrevW, NULL }, { "MessageBoxIndirectA", u32_MessageBoxIndirectA, NULL }, { "MessageBoxIndirectW", u32_MessageBoxIndirectW, NULL },
    { "ExitWindowsEx", u32_ExitWindowsEx, NULL },
    { "AdjustTokenPrivileges", adv_AdjustTokenPrivileges, NULL }, { "LookupPrivilegeValueA", adv_LookupPrivilegeValue, NULL },
    { "LookupPrivilegeValueW", adv_LookupPrivilegeValue, NULL }, { "CheckTokenMembership", adv_CheckTokenMembership, NULL },
    { "AllocateAndInitializeSid", adv_AllocateAndInitializeSid, NULL },
    { "RmStartSession", rm_StartSession, NULL }, { "RmEndSession", rm_Success, NULL }, { "RmRegisterResources", rm_Success, NULL },
    { "RmGetList", rm_GetList, NULL }, { "RmShutdown", rm_Success, NULL }, { "RmRestart", rm_Success, NULL }, { "FreeSid", adv_FreeSid, NULL }, { "EqualSid", adv_EqualSid, NULL },
    { "GetLengthSid", adv_GetLengthSid, NULL }, { "IsValidSid", adv_IsValidSid, NULL },
    { "GetThreadPreferredUILanguages", k32_GetThreadPreferredUILanguages, NULL }, { "GetUserPreferredUILanguages", k32_GetUserPreferredUILanguages, NULL },
    { "GetSystemPreferredUILanguages", k32_GetSystemPreferredUILanguages, NULL }, { "GetProcessPreferredUILanguages", k32_GetProcessPreferredUILanguages, NULL },
    { "SetThreadPreferredUILanguages", k32_SetThreadPreferredUILanguages, NULL }, { "SetProcessPreferredUILanguages", k32_SetProcessPreferredUILanguages, NULL },
    { "GetThreadUILanguage", k32_GetThreadUILanguage, NULL }, { "SetThreadUILanguage", k32_SetThreadUILanguage, NULL },
    { "SetThreadLocale", k32_SetThreadLocale, NULL }, { "GetThreadLocale", k32_GetThreadLocale, NULL },
    { "SetSearchPathMode", k32_SetSearchPathMode, NULL }, { "SetProcessDEPPolicy", k32_SetProcessDEPPolicy, NULL },
    { "GetProcessDEPPolicy", k32_GetProcessDEPPolicy, NULL },
    { "InterlockedIncrement", k32_InterlockedIncrement, NULL }, { "InterlockedDecrement", k32_InterlockedDecrement, NULL },
    { "InterlockedExchange", k32_InterlockedExchange, NULL }, { "InterlockedExchangeAdd", k32_InterlockedExchangeAdd, NULL },
    { "InterlockedCompareExchange", k32_InterlockedCompareExchange, NULL }, { "InterlockedCompareExchange64", k32_InterlockedCompareExchange64, NULL },
    { "GlobalAddAtomA", k32_GlobalAddAtomA, NULL }, { "GlobalAddAtomW", k32_GlobalAddAtomW, NULL },
    { "AddAtomA", k32_GlobalAddAtomA, NULL }, { "AddAtomW", k32_GlobalAddAtomW, NULL },
    { "GlobalFindAtomA", k32_GlobalFindAtomA, NULL }, { "GlobalFindAtomW", k32_GlobalFindAtomW, NULL },
    { "FindAtomA", k32_GlobalFindAtomA, NULL }, { "FindAtomW", k32_GlobalFindAtomW, NULL },
    { "GlobalGetAtomNameA", k32_GlobalGetAtomNameA, NULL }, { "GlobalGetAtomNameW", k32_GlobalGetAtomNameW, NULL },
    { "GetAtomNameA", k32_GlobalGetAtomNameA, NULL }, { "GetAtomNameW", k32_GlobalGetAtomNameW, NULL },
    { "GlobalDeleteAtom", k32_GlobalDeleteAtom, NULL }, { "DeleteAtom", k32_GlobalDeleteAtom, NULL },
    { "CharUpperBuffW", u32_CharUpperBuffW, NULL }, { "CharLowerBuffW", u32_CharLowerBuffW, NULL },
    { "CharUpperBuffA", u32_CharUpperBuffA, NULL }, { "CharLowerBuffA", u32_CharLowerBuffA, NULL },
    { "IsCharUpperW", u32_IsCharUpperW, NULL }, { "IsCharLowerW", u32_IsCharLowerW, NULL },
    { "EnumCalendarInfoW", k32_EnumCalendarInfo, NULL }, { "VirtualQueryEx", k32_VirtualQueryEx, NULL }, { "EnumCalendarInfoA", k32_EnumCalendarInfo, NULL },
    { "FindResourceA", k32_FindResourceA, NULL }, { "FindResourceW", k32_FindResourceW, NULL },
    { "FindResourceExA", k32_FindResourceExA, NULL }, { "FindResourceExW", k32_FindResourceExW, NULL },
    { "LoadResource", k32_LoadResource, NULL }, { "LockResource", k32_LockResource, NULL }, { "SizeofResource", k32_SizeofResource, NULL },
    { "FreeResource", k32_FreeResource, NULL },
    { "NetWkstaGetInfo", net_NetWkstaGetInfo, NULL },
    { "GdiplusStartup", gp_GdiplusStartup, NULL }, { "GdiplusShutdown", gp_GdiplusShutdown, NULL },
    { "SetHandleCount", k32_SetHandleCount, NULL },
    { "GetTokenInformation", adv_GetTokenInformation, NULL }, { "InitializeSecurityDescriptor", adv_InitializeSecurityDescriptor, NULL },
    { "SetSecurityDescriptorDacl", adv_True, NULL }, { "SetSecurityDescriptorOwner", adv_True, NULL }, { "SetSecurityDescriptorGroup", adv_True, NULL },
    { "SetSecurityDescriptorSacl", adv_True, NULL }, { "GetSecurityDescriptorDacl", adv_GetSecurityDescriptorDacl, NULL },
    { "InitializeAcl", adv_True, NULL }, { "AddAccessAllowedAce", adv_True, NULL }, { "AddAccessAllowedAceEx", adv_True, NULL },
    { "AddAccessDeniedAce", adv_True, NULL }, { "IsValidSecurityDescriptor", adv_True, NULL }, { "SetKernelObjectSecurity", adv_True, NULL },
    { "SetFileSecurityW", adv_True, NULL }, { "SetFileSecurityA", adv_True, NULL },
    { "ConvertStringSecurityDescriptorToSecurityDescriptorW", adv_ConvertStringSecurityDescriptor, NULL },
    { "ConvertStringSecurityDescriptorToSecurityDescriptorA", adv_ConvertStringSecurityDescriptor, NULL },
    { "SetEntriesInAclW", adv_SetEntriesInAcl, NULL }, { "SetEntriesInAclA", adv_SetEntriesInAcl, NULL },
    { "SetNamedSecurityInfoW", adv_Success, NULL }, { "SetNamedSecurityInfoA", adv_Success, NULL },
    { "GetNamedSecurityInfoW", adv_GetNamedSecurityInfo, NULL }, { "GetNamedSecurityInfoA", adv_GetNamedSecurityInfo, NULL },
    { "ConvertSidToStringSidW", adv_ConvertSidToStringSidW, NULL }, { "ConvertSidToStringSidA", adv_ConvertSidToStringSidA, NULL },
    { "ConvertStringSidToSidW", adv_ConvertStringSidToSid, NULL }, { "ConvertStringSidToSidA", adv_ConvertStringSidToSid, NULL },
    { "LookupAccountSidW", adv_LookupAccountSidW, NULL }, { "LookupAccountSidA", adv_LookupAccountSidA, NULL },
    { "GetProcessDpiAwareness", shc_GetProcessDpiAwareness, NULL }, { "GetAwarenessFromDpiAwarenessContext", u_GetAwarenessFromDpiAwarenessContext, NULL },
    { "IsValidDpiAwarenessContext", u_IsValidDpiAwarenessContext, NULL }, { "AreDpiAwarenessContextsEqual", u_AreDpiAwarenessContextsEqual, NULL },
    { "GetWindowDpiAwarenessContext", u_GetWindowDpiAwarenessContext, NULL }, { "GetDpiFromDpiAwarenessContext", u_GetDpi96, NULL },
    { "GetSystemDpiForProcess", u_GetDpi96, NULL }, { "GetDpiForShellUIComponent", u_GetDpi96, NULL },
    { "IsThemeActive", ux_False, NULL }, { "IsAppThemed", ux_False, NULL }, { "IsCompositionActive", ux_False, NULL },
    { "OpenThemeData", ux_False, NULL }, { "OpenThemeDataEx", ux_False, NULL }, { "OpenThemeDataForDpi", ux_False, NULL },
    { "GetWindowTheme", ux_False, NULL }, { "CloseThemeData", ux_Ok, NULL }, { "EnableThemeDialogTexture", ux_Ok, NULL },
    { "EnableTheming", ux_Ok, NULL }, { "SetThemeAppProperties", ux_Ok, NULL }, { "GetThemeAppProperties", ux_GetThemeAppProperties, NULL },
    { "DrawThemeBackground", ux_Fail, NULL }, { "DrawThemeBackgroundEx", ux_Fail, NULL }, { "DrawThemeText", ux_Fail, NULL },
    { "DrawThemeTextEx", ux_Fail, NULL }, { "DrawThemeEdge", ux_Fail, NULL }, { "DrawThemeIcon", ux_Fail, NULL },
    { "DrawThemeParentBackground", ux_Fail, NULL }, { "DrawThemeParentBackgroundEx", ux_Fail, NULL },
    { "GetThemePartSize", ux_Fail, NULL }, { "GetThemeColor", ux_Fail, NULL }, { "GetThemeInt", ux_Fail, NULL },
    { "GetThemeMargins", ux_Fail, NULL }, { "GetThemeFont", ux_Fail, NULL }, { "GetThemeTextExtent", ux_Fail, NULL },
    { "GetThemeBackgroundContentRect", ux_Fail, NULL }, { "GetThemeBackgroundExtent", ux_Fail, NULL },
    { "GetThemeBackgroundRegion", ux_Fail, NULL }, { "GetThemeBool", ux_Fail, NULL }, { "GetThemeEnumValue", ux_Fail, NULL },
    { "GetThemeMetric", ux_Fail, NULL }, { "GetThemePosition", ux_Fail, NULL }, { "GetThemeRect", ux_Fail, NULL },
    { "GetThemeString", ux_Fail, NULL }, { "GetThemeTextMetrics", ux_Fail, NULL }, { "GetThemeTransitionDuration", ux_Fail, NULL },
    { "HitTestThemeBackground", ux_Fail, NULL }, { "IsThemeBackgroundPartiallyTransparent", ux_False, NULL },
    { "IsThemePartDefined", ux_False, NULL }, { "IsThemeDialogTextureEnabled", ux_False, NULL },
    { "GetThemeSysColor", ux_GetThemeSysColor, NULL }, { "GetThemeSysColorBrush", ux_False, NULL }, { "GetThemeSysFont", ux_Fail, NULL },
    { "GetThemeSysSize", ux_False, NULL }, { "GetThemeSysBool", ux_False, NULL }, { "GetThemeSysInt", ux_Fail, NULL },
    { "GetCurrentThemeName", ux_GetCurrentThemeName, NULL },
    { "BufferedPaintInit", ux_Ok, NULL }, { "BufferedPaintUnInit", ux_Ok, NULL }, { "BeginBufferedPaint", ux_BeginBufferedPaint, NULL },
    { "EndBufferedPaint", ux_Ok, NULL }, { "BufferedPaintSetAlpha", ux_Ok, NULL }, { "BufferedPaintClear", ux_Ok, NULL },
    { "BufferedPaintRenderAnimation", ux_False, NULL }, { "BufferedPaintStopAllAnimations", ux_Ok, NULL },
    { "GetBufferedPaintBits", ux_Fail, NULL }, { "GetBufferedPaintDC", ux_False, NULL }, { "GetBufferedPaintTargetDC", ux_False, NULL },
    { "BeginBufferedAnimation", ux_False, NULL }, { "EndBufferedAnimation", ux_Ok, NULL },
    { "WTSRegisterSessionNotification", wts_RegisterSessionNotification, NULL }, { "WTSUnRegisterSessionNotification", wts_RegisterSessionNotification, NULL },
    { "WTSRegisterSessionNotificationEx", wts_RegisterSessionNotification, NULL }, { "WTSUnRegisterSessionNotificationEx", wts_RegisterSessionNotification, NULL },
    { "WTSQuerySessionInformationW", wts_QuerySessionInformation, NULL }, { "WTSQuerySessionInformationA", wts_QuerySessionInformation, NULL },
    { "GetLogicalProcessorInformation", k32_GetLogicalProcessorInformation, NULL }, { "NetApiBufferFree", net_NetApiBufferFree, NULL },
    { NULL, NULL, NULL }
};
