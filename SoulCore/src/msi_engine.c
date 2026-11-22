// SoulCore - Windows Installer engine: runs the install / uninstall sequences of an .msi package.
// Part 1: utilities, properties, directories, formatted strings, conditions.
#include "msi_priv.h"
#include <ctype.h>
#include <errno.h>
#include <sys/stat.h>
#include <unistd.h>
#include <time.h>
#include <stdarg.h>
#include <fcntl.h>

extern const char *soul_csidl_path(int csidl);

#define HKCR 0x80000000u
#define HKCU 0x80000001u
#define HKLM 0x80000002u
#define HKU 0x80000003u

// INSTALLSTATE
#define IS_UNKNOWN (-1)
#define IS_ADVERTISED 1
#define IS_ABSENT 2
#define IS_LOCAL 3
#define IS_SOURCE 4
#define IS_DEFAULT 5

typedef struct { char *name, *val; } MsiProp;
typedef struct { char *p; size_t n, cap; } Buf;

static void buf_add(Buf *b, const char *s, size_t n) {
    if (b->n + n + 1 > b->cap) { b->cap = (b->n + n + 1) * 2 + 64; b->p = realloc(b->p, b->cap); }
    memcpy(b->p + b->n, s, n);
    b->n += n;
    b->p[b->n] = 0;
}
static void buf_str(Buf *b, const char *s) { buf_add(b, s, strlen(s)); }
static void buf_init(Buf *b) { b->p = malloc(64); b->cap = 64; b->n = 0; b->p[0] = 0; }

struct MsiSess {
    uint32_t hnd;
    X86State *st;
    SoulProcess *owner;
    MsiDb *db;
    char pkg_host[1100], pkg_win[700], src_dir[700];
    int x64;
    MsiProp *props; int nprops, capprops;
    // features / components (parallel to the table rows)
    int nf, nc;
    int *f_inst, *f_act, *f_level;
    int *c_inst, *c_act, *c_cond;
    char **dir_cache; int nd;
    char **src_cache;
    int costed;
    int level;            // INSTALLLEVEL
    int mode;             // 0 install, 1 remove, 2 reinstall
    int installed;        // product was registered before the run
    int deferred;
    char *ca_data;
    int cancelled;
    char **done; int ndone, capdone;   // standard actions already run
    char **ca_ran; int nca_ran, capca_ran;
    int total, step;
    char code[48];
    int reg_product, flag_pc, flag_pf, remove_all;
    int ui_seq_done_n;
    int committed;
    int in_ui_seq;
    char **created_keys; int ncreated;
};

static void S_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void S_log(const char *fmt, ...) {
    char b[1500];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(b, sizeof b, fmt, ap);
    va_end(ap);
    soul_log("[msi] %s\n", b);
}

static const char *Sx(MsiSess *s, const char *table, int row, const char *col) {
    const char *v = msidb_str(s->db, table, row, col);
    return v ? v : "";
}

static char *xfmt(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static char *xfmt(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    char *r = NULL;
    if (vasprintf(&r, fmt, ap) < 0) r = strdup("");
    va_end(ap);
    return r;
}

// the part after '|' of "short|long"
static const char *long_name(const char *n) {
    const char *b = strchr(n, '|');
    return b ? b + 1 : n;
}
static char *name_of(const char *n, int use_short) {
    const char *b = strchr(n, '|');
    if (!b) return strdup(n);
    return use_short ? strndup(n, (size_t)(b - n)) : strdup(b + 1);
}

// ---- host paths -----------------------------------------------------------------------------
static int host_of(const char *win, char *host, size_t n) { return path_w2h(win, host, n); }

// creates a Windows directory and every missing parent
static void ensure_dir_win(const char *win) {
    char n[1300], cur[1300];
    path_normalize_win(win, n, sizeof n);
    size_t len = strlen(n);
    while (len > 3 && n[len - 1] == '\\') n[--len] = 0;
    snprintf(cur, sizeof cur, "%.2s\\", n);
    const char *p = n + 3;
    char host[2600];
    while (*p) {
        const char *e = strchr(p, '\\');
        size_t l = e ? (size_t)(e - p) : strlen(p);
        size_t cl = strlen(cur);
        snprintf(cur + cl, sizeof cur - cl, "%.*s", (int)l, p);
        host_of(cur, host, sizeof host);
        mkdir(host, 0755);
        cl = strlen(cur);
        snprintf(cur + cl, sizeof cur - cl, "\\");
        p += l;
        if (*p == '\\') p++;
    }
}
static void ensure_parent_win(const char *win_file) {
    char t[1300];
    snprintf(t, sizeof t, "%s", win_file);
    char *sl = strrchr(t, '\\');
    if (sl) { *sl = 0; ensure_dir_win(t); }
}
static int file_exists_host(const char *h) { struct stat sb; return stat(h, &sb) == 0; }
static int file_exists_win(const char *w) { char h[2600]; host_of(w, h, sizeof h); return file_exists_host(h); }
static int write_host_file(const char *host, const void *data, size_t len) {
    unlink(host);
    int fd = open(host, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return -1;
    const uint8_t *p = data;
    while (len) { ssize_t w = write(fd, p, len > (1u << 20) ? (1u << 20) : len); if (w <= 0) { close(fd); return -1; } p += w; len -= (size_t)w; }
    close(fd);
    return 0;
}
static uint8_t *read_host_file(const char *host, size_t *len) {
    FILE *f = fopen(host, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *b = malloc((size_t)n + 1);
    if (n > 0 && fread(b, 1, (size_t)n, f) != (size_t)n) { fclose(f); free(b); return NULL; }
    fclose(f);
    b[n] = 0;
    *len = (size_t)n;
    return b;
}
static int copy_host_file(const char *from, const char *to) {
    size_t n;
    uint8_t *d = read_host_file(from, &n);
    if (!d) return -1;
    int r = write_host_file(to, d, n);
    free(d);
    return r;
}
static char *with_slash(const char *p) {
    size_t n = strlen(p);
    char *r = malloc(n + 2);
    memcpy(r, p, n + 1);
    if (n && r[n - 1] != '\\') { r[n] = '\\'; r[n + 1] = 0; }
    return r;
}

// ---- properties -------------------------------------------------------------------------------
static int prop_index(MsiSess *s, const char *name) {
    for (int i = 0; i < s->nprops; i++) if (!strcmp(s->props[i].name, name)) return i;
    return -1;
}
static void clear_dir_caches(MsiSess *s) {
    for (int i = 0; i < s->nd; i++) { free(s->dir_cache[i]); s->dir_cache[i] = NULL; free(s->src_cache[i]); s->src_cache[i] = NULL; }
}
void msis_setprop(MsiSess *s, const char *name, const char *value) {
    int i = prop_index(s, name);
    if (!value || !*value) {
        if (i >= 0) { free(s->props[i].name); free(s->props[i].val); s->props[i] = s->props[--s->nprops]; }
    } else if (i >= 0) { free(s->props[i].val); s->props[i].val = strdup(value); }
    else {
        if (s->nprops == s->capprops) { s->capprops = s->capprops ? s->capprops * 2 : 64; s->props = realloc(s->props, sizeof(MsiProp) * (size_t)s->capprops); }
        s->props[s->nprops].name = strdup(name);
        s->props[s->nprops].val = strdup(value);
        s->nprops++;
    }
    clear_dir_caches(s);
}
static void setprop_int(MsiSess *s, const char *name, long v) { char b[32]; snprintf(b, sizeof b, "%ld", v); msis_setprop(s, name, b); }

static int allusers(MsiSess *s) {
    int i = prop_index(s, "ALLUSERS");
    return i >= 0 && s->props[i].val[0] && strcmp(s->props[i].val, "0") != 0;
}

// standard folders that depend on the package architecture / ALLUSERS
static char *builtin_get(MsiSess *s, const char *n) {
    int au = allusers(s);
    if (!strcmp(n, "ROOTDRIVE")) return strdup("C:\\");
    if (!strcmp(n, "ProgramFilesFolder")) return strdup("C:\\Program Files (x86)\\");
    if (!strcmp(n, "ProgramFiles64Folder")) return strdup(s->x64 ? "C:\\Program Files\\" : "C:\\Program Files (x86)\\");
    if (!strcmp(n, "CommonFilesFolder")) return strdup("C:\\Program Files (x86)\\Common Files\\");
    if (!strcmp(n, "CommonFiles64Folder")) return strdup(s->x64 ? "C:\\Program Files\\Common Files\\" : "C:\\Program Files (x86)\\Common Files\\");
    if (!strcmp(n, "SystemFolder") || !strcmp(n, "System64Folder")) return with_slash(soul_csidl_path(0x25));
    if (!strcmp(n, "WindowsFolder")) return with_slash(soul_csidl_path(0x24));
    if (!strcmp(n, "WindowsVolume")) return strdup("C:\\");
    if (!strcmp(n, "TempFolder")) return strdup("C:\\users\\soul\\Temp\\");
    if (!strcmp(n, "AppDataFolder")) return with_slash(soul_csidl_path(0x1A));
    if (!strcmp(n, "LocalAppDataFolder")) return with_slash(soul_csidl_path(0x1C));
    if (!strcmp(n, "CommonAppDataFolder")) return with_slash(soul_csidl_path(0x23));
    if (!strcmp(n, "DesktopFolder")) return with_slash(soul_csidl_path(au ? 0x19 : 0x10));
    if (!strcmp(n, "ProgramMenuFolder")) return with_slash(soul_csidl_path(au ? 0x17 : 0x02));
    if (!strcmp(n, "StartMenuFolder")) return with_slash(soul_csidl_path(au ? 0x16 : 0x0B));
    if (!strcmp(n, "StartupFolder")) return with_slash(soul_csidl_path(au ? 0x18 : 0x07));
    if (!strcmp(n, "PersonalFolder")) return with_slash(soul_csidl_path(0x05));
    if (!strcmp(n, "FontsFolder")) return with_slash(soul_csidl_path(0x14));
    if (!strcmp(n, "SendToFolder")) return with_slash(soul_csidl_path(0x09));
    if (!strcmp(n, "TemplateFolder")) return with_slash(soul_csidl_path(au ? 0x2D : 0x15));
    if (!strcmp(n, "FavoritesFolder")) return with_slash(soul_csidl_path(0x06));
    if (!strcmp(n, "MyPicturesFolder")) return with_slash(soul_csidl_path(0x27));
    if (!strcmp(n, "AdminToolsFolder")) return xfmt("%s\\Administrative Tools\\", soul_csidl_path(au ? 0x17 : 0x02));
    return NULL;
}

static int dir_row(MsiSess *s, const char *id) { return msidb_find(s->db, "Directory", "Directory", id); }
static char *dir_path_row(MsiSess *s, int row, int depth);

// target path of a directory key (trailing backslash)
char *msis_dir_path(MsiSess *s, const char *dir) {
    int r = dir_row(s, dir);
    if (r < 0) {
        int i = prop_index(s, dir);
        char *b = builtin_get(s, dir);
        if (b) return b;
        (void)i;
        return NULL;
    }
    return dir_path_row(s, r, 0);
}

static char *dir_path_row(MsiSess *s, int row, int depth) {
    if (row < s->nd && s->dir_cache[row]) return strdup(s->dir_cache[row]);
    const char *id = Sx(s, "Directory", row, "Directory");
    char *res = NULL;
    int pi = prop_index(s, id);
    if (pi >= 0) res = with_slash(s->props[pi].val);
    else if (!strcmp(id, "TARGETDIR")) res = strdup("C:\\");
    else if ((res = builtin_get(s, id)) != NULL) { }
    else if (depth > 40) res = strdup("C:\\");
    else {
        const char *par = Sx(s, "Directory", row, "Directory_Parent");
        char *pp;
        if (*par && strcmp(par, id) != 0) {
            int pr = dir_row(s, par);
            pp = pr >= 0 ? dir_path_row(s, pr, depth + 1) : msis_dir_path(s, "TARGETDIR");
        } else pp = msis_dir_path(s, "TARGETDIR");
        if (!pp) pp = strdup("C:\\");
        const char *dd = Sx(s, "Directory", row, "DefaultDir");
        char *tgt = strdup(dd);
        char *colon = strchr(tgt, ':');
        if (colon) *colon = 0;
        const char *nm = long_name(tgt);
        if (!*nm || !strcmp(nm, ".")) res = pp;
        else { res = xfmt("%s%s\\", pp, nm); free(pp); }
        free(tgt);
    }
    if (row < s->nd) s->dir_cache[row] = strdup(res);
    return res;
}

static char *src_path_row(MsiSess *s, int row, int depth) {
    if (row < s->nd && s->src_cache[row]) return strdup(s->src_cache[row]);
    const char *id = Sx(s, "Directory", row, "Directory");
    char *res;
    if (!strcmp(id, "TARGETDIR") || depth > 40) {
        char *sd = msis_prop(s, "SourceDir");
        res = sd ? with_slash(sd) : strdup("C:\\");
        free(sd);
    } else {
        const char *par = Sx(s, "Directory", row, "Directory_Parent");
        char *pp;
        int pr = *par && strcmp(par, id) ? dir_row(s, par) : -1;
        if (pr >= 0) pp = src_path_row(s, pr, depth + 1);
        else { int tr = dir_row(s, "TARGETDIR"); pp = tr >= 0 ? src_path_row(s, tr, depth + 1) : strdup(s->src_dir); }
        const char *dd = Sx(s, "Directory", row, "DefaultDir");
        const char *colon = strchr(dd, ':');
        char *nm = strdup(colon ? colon + 1 : dd);
        const char *ln = long_name(nm);
        // sources are located by the long names (SHORTFILENAMES is not supported)
        if (!*ln || !strcmp(ln, ".")) res = pp;
        else { res = xfmt("%s%s\\", pp, ln); free(pp); }
        free(nm);
    }
    if (row < s->nd) s->src_cache[row] = strdup(res);
    return res;
}
char *msis_src_path(MsiSess *s, const char *dir) {
    int r = dir_row(s, dir);
    return r < 0 ? NULL : src_path_row(s, r, 0);
}
int msis_set_target(MsiSess *s, const char *dir, const char *path) {
    if (dir_row(s, dir) < 0) return 267;
    char *p = with_slash(path);
    msis_setprop(s, dir, p);
    free(p);
    return 0;
}

char *msis_prop(MsiSess *s, const char *name) {
    if (!strcmp(name, "CustomActionData") && s->deferred) return s->ca_data ? strdup(s->ca_data) : NULL;
    int i = prop_index(s, name);
    if (i >= 0) return strdup(s->props[i].val);
    char *b = builtin_get(s, name);
    if (b) return b;
    if (dir_row(s, name) >= 0) return msis_dir_path(s, name);
    return NULL;
}

// ---- files and components ----------------------------------------------------------------------
static int comp_row(MsiSess *s, const char *id) { return msidb_find(s->db, "Component", "Component", id); }
static int feat_row(MsiSess *s, const char *id) { return msidb_find(s->db, "Feature", "Feature", id); }
static int file_row(MsiSess *s, const char *key) { return msidb_find(s->db, "File", "File", key); }

static char *comp_dir_path(MsiSess *s, int crow) {
    const char *d = Sx(s, "Component", crow, "Directory_");
    char *p = msis_dir_path(s, d);
    return p ? p : strdup("C:\\");
}
// full Windows path an installed file will have
static char *file_target(MsiSess *s, int frow) {
    int c = comp_row(s, Sx(s, "File", frow, "Component_"));
    char *dir = c >= 0 ? comp_dir_path(s, c) : strdup("C:\\");
    char *nm = name_of(Sx(s, "File", frow, "FileName"), 0);
    char *r = xfmt("%s%s", dir, nm);
    free(dir); free(nm);
    return r;
}
// key path of a component: a file path, or a registry path "\HKLM\key\name"; NULL if none
static char *comp_keypath(MsiSess *s, int crow, int *is_file) {
    const char *kp = Sx(s, "Component", crow, "KeyPath");
    int attrs = msidb_int(s->db, "Component", crow, "Attributes");
    if (is_file) *is_file = 0;
    if (!*kp) return NULL;
    if (attrs != MSI_NULL_INT && (attrs & 4)) {
        int r = msidb_find(s->db, "Registry", "Registry", kp);
        if (r < 0) return NULL;
        static const char *roots[] = { "HKCR", "HKCU", "HKLM", "HKU" };
        int root = msidb_int(s->db, "Registry", r, "Root");
        char *key = msis_format(s, Sx(s, "Registry", r, "Key"), NULL, NULL);
        char *nm = msis_format(s, Sx(s, "Registry", r, "Name"), NULL, NULL);
        char *res = xfmt("%s\\%s\\%s", root >= 0 && root < 4 ? roots[root] : (allusers(s) ? "HKLM" : "HKCU"), key, nm);
        free(key); free(nm);
        return res;
    }
    int f = file_row(s, kp);
    if (f < 0) return NULL;
    if (is_file) *is_file = 1;
    return file_target(s, f);
}

// ---- formatted strings ---------------------------------------------------------------------------
static int comp_install_state(MsiSess *s, int crow, int action);
static int feat_state_get(MsiSess *s, int frow, int action);

static char *env_get(const char *name) {
    HleState *h = hs();
    size_t n = strlen(name);
    for (int i = 0; i < h->env_count; i++)
        if (strncasecmp(h->env[i], name, n) == 0 && h->env[i][n] == '=') return strdup(h->env[i] + n + 1);
    return NULL;
}

static void fmt_core(MsiSess *s, const char *in, size_t inlen, const MsiFmtRec *rec, Buf *out, int depth) {
    size_t i = 0;
    while (i < inlen) {
        char c = in[i];
        if (c != '[' || depth > 16) { buf_add(out, &c, 1); i++; continue; }
        // find the matching bracket
        size_t j = i + 1;
        int nest = 1;
        while (j < inlen) {
            if (in[j] == '[') nest++;
            else if (in[j] == ']' && --nest == 0) break;
            j++;
        }
        if (j >= inlen) { buf_add(out, &c, 1); i++; continue; }
        const char *inner = in + i + 1;
        size_t il = j - i - 1;
        i = j + 1;
        if (il == 2 && inner[0] == '\\') { buf_add(out, inner + 1, 1); continue; }
        if (il == 1 && inner[0] == '~') { buf_add(out, "\0", 1); continue; }
        char *name;
        if (memchr(inner, '[', il)) {
            Buf nb;
            buf_init(&nb);
            fmt_core(s, inner, il, rec, &nb, depth + 1);
            name = nb.p;
        } else name = strndup(inner, il);
        char *val = NULL;
        if (name[0] == '#' || name[0] == '!') {
            int f = file_row(s, name + 1);
            if (f >= 0) val = file_target(s, f);
        } else if (name[0] == '$') {
            int c2 = comp_row(s, name + 1);
            if (c2 >= 0) {
                int isf;
                val = comp_keypath(s, c2, &isf);
                if (!val || !isf) { free(val); val = comp_dir_path(s, c2); }
            }
        } else if (name[0] == '%') {
            val = env_get(name + 1);
        } else if (rec && name[0] && strspn(name, "0123456789") == strlen(name)) {
            int n = atoi(name);
            if (n >= 0 && n < rec->n && rec->f[n]) val = strdup(rec->f[n]);
            else if (n >= rec->n || !rec->f[n]) val = strdup("");
        } else val = msis_prop(s, name);
        if (val) buf_str(out, val);
        free(val);
        free(name);
    }
}
char *msis_format(MsiSess *s, const char *tmpl, const MsiFmtRec *rec, size_t *outlen) {
    Buf b;
    buf_init(&b);
    if (tmpl) fmt_core(s, tmpl, strlen(tmpl), rec, &b, 0);
    if (outlen) *outlen = b.n;
    return b.p;
}
static char *fmt(MsiSess *s, const char *t) { return msis_format(s, t, NULL, NULL); }

// ---- conditions ----------------------------------------------------------------------------------
typedef struct { int is_int; long long i; char *str; } CVal;
typedef struct { MsiSess *s; const char *p; int err; } CP;

static void cv_free(CVal *v) { free(v->str); v->str = NULL; }
static int cv_true(const CVal *v) { return v->is_int ? v->i != 0 : (v->str && *v->str); }
static CVal cv_int(long long i) { CVal v = { 1, i, NULL }; return v; }
static void c_ws(CP *c) { while (*c->p == ' ' || *c->p == '\t' || *c->p == '\r' || *c->p == '\n') c->p++; }
static int c_kw(CP *c, const char *kw) {
    c_ws(c);
    size_t n = strlen(kw);
    if (strncasecmp(c->p, kw, n) == 0 && !isalnum((unsigned char)c->p[n]) && c->p[n] != '_' && c->p[n] != '.') { c->p += n; return 1; }
    return 0;
}
static int str_is_int(const char *s, long long *out) {
    if (!*s) return 0;
    char *e;
    long long v = strtoll(s, &e, 10);
    if (*e || e == s) return 0;
    *out = v;
    return 1;
}
static CVal cv_from_string(const char *s) {
    long long v;
    CVal r = { 0, 0, NULL };
    if (str_is_int(s, &v)) { r.is_int = 1; r.i = v; }
    else r.str = strdup(s);
    return r;
}
static CVal c_expr(CP *c);

static CVal c_value(CP *c) {
    c_ws(c);
    CVal v = cv_int(0);
    if (*c->p == '(') {
        c->p++;
        v = c_expr(c);
        c_ws(c);
        if (*c->p == ')') c->p++;
        else c->err = 1;
        return v;
    }
    if (*c->p == '"') {
        const char *e = strchr(c->p + 1, '"');
        if (!e) { c->err = 1; c->p += strlen(c->p); return v; }
        char *raw = strndup(c->p + 1, (size_t)(e - c->p - 1));
        c->p = e + 1;
        char *f = fmt(c->s, raw);
        free(raw);
        v.is_int = 0;
        v.str = f;
        return v;
    }
    if (isdigit((unsigned char)*c->p) || (*c->p == '-' && isdigit((unsigned char)c->p[1]))) {
        char *e;
        v.i = strtoll(c->p, &e, 10);
        c->p = e;
        return v;
    }
    char kind = 0;
    if (*c->p == '%' || *c->p == '&' || *c->p == '!' || *c->p == '$' || *c->p == '?') { kind = *c->p; c->p++; }
    const char *b = c->p;
    while (isalnum((unsigned char)*c->p) || *c->p == '_' || *c->p == '.') c->p++;
    if (c->p == b) { c->err = 1; return v; }
    char *name = strndup(b, (size_t)(c->p - b));
    if (kind == '%') {
        char *e = env_get(name);
        v = cv_from_string(e ? e : "");
        free(e);
    } else if (kind == '&' || kind == '!') {
        int f = feat_row(c->s, name);
        v = cv_int(f < 0 ? IS_UNKNOWN : feat_state_get(c->s, f, kind == '&'));
    } else if (kind == '$' || kind == '?') {
        int cr = comp_row(c->s, name);
        v = cv_int(cr < 0 ? IS_UNKNOWN : comp_install_state(c->s, cr, kind == '$'));
    } else {
        char *pv = msis_prop(c->s, name);
        v = cv_from_string(pv ? pv : "");
        free(pv);
    }
    free(name);
    return v;
}

static CVal c_cmp(CP *c) {
    CVal l = c_value(c);
    if (c->err) return l;
    c_ws(c);
    int ci = 0;
    const char *p = c->p;
    if (*p == '~') { ci = 1; p++; }
    int op = 0;   // 1 = 2 <> 3 < 4 > 5 <= 6 >= 7 >< 8 << 9 >>
    if (p[0] == '=') { op = 1; p++; }
    else if (p[0] == '<' && p[1] == '>') { op = 2; p += 2; }
    else if (p[0] == '<' && p[1] == '=') { op = 5; p += 2; }
    else if (p[0] == '<' && p[1] == '<') { op = 8; p += 2; }
    else if (p[0] == '<') { op = 3; p++; }
    else if (p[0] == '>' && p[1] == '<') { op = 7; p += 2; }
    else if (p[0] == '>' && p[1] == '=') { op = 6; p += 2; }
    else if (p[0] == '>' && p[1] == '>') { op = 9; p += 2; }
    else if (p[0] == '>') { op = 4; p++; }
    if (!op) return l;
    c->p = p;
    CVal r = c_value(c);
    int res = 0;
    if (l.is_int && r.is_int && op <= 6) {
        long long a = l.i, b = r.i;
        res = op == 1 ? a == b : op == 2 ? a != b : op == 3 ? a < b : op == 4 ? a > b : op == 5 ? a <= b : a >= b;
    } else {
        char ta[32], tb[32];
        const char *a = l.is_int ? (snprintf(ta, sizeof ta, "%lld", l.i), ta) : (l.str ? l.str : "");
        const char *b = r.is_int ? (snprintf(tb, sizeof tb, "%lld", r.i), tb) : (r.str ? r.str : "");
        int cmp = ci ? strcasecmp(a, b) : strcmp(a, b);
        size_t la = strlen(a), lb = strlen(b);
        switch (op) {
        case 1: res = cmp == 0; break;
        case 2: res = cmp != 0; break;
        case 3: res = cmp < 0; break;
        case 4: res = cmp > 0; break;
        case 5: res = cmp <= 0; break;
        case 6: res = cmp >= 0; break;
        case 7: res = ci ? strcasestr(a, b) != NULL : strstr(a, b) != NULL; break;
        case 8: res = lb <= la && (ci ? strncasecmp(a, b, lb) : strncmp(a, b, lb)) == 0; break;
        case 9: res = lb <= la && (ci ? strcasecmp(a + la - lb, b) : strcmp(a + la - lb, b)) == 0; break;
        }
    }
    cv_free(&l);
    cv_free(&r);
    return cv_int(res);
}
static CVal c_not(CP *c) {
    if (c_kw(c, "NOT")) { CVal v = c_not(c); int t = !cv_true(&v); cv_free(&v); return cv_int(t); }
    return c_cmp(c);
}
static CVal c_and(CP *c) {
    CVal l = c_not(c);
    while (!c->err && c_kw(c, "AND")) { CVal r = c_not(c); int t = cv_true(&l) && cv_true(&r); cv_free(&l); cv_free(&r); l = cv_int(t); }
    return l;
}
static CVal c_xor(CP *c) {
    CVal l = c_and(c);
    while (!c->err && c_kw(c, "XOR")) { CVal r = c_and(c); int t = cv_true(&l) != cv_true(&r); cv_free(&l); cv_free(&r); l = cv_int(t); }
    return l;
}
static CVal c_or(CP *c) {
    CVal l = c_xor(c);
    while (!c->err && c_kw(c, "OR")) { CVal r = c_xor(c); int t = cv_true(&l) || cv_true(&r); cv_free(&l); cv_free(&r); l = cv_int(t); }
    return l;
}
static CVal c_eqv(CP *c) {
    CVal l = c_or(c);
    while (!c->err && c_kw(c, "EQV")) { CVal r = c_or(c); int t = cv_true(&l) == cv_true(&r); cv_free(&l); cv_free(&r); l = cv_int(t); }
    return l;
}
static CVal c_expr(CP *c) {
    CVal l = c_eqv(c);
    while (!c->err && c_kw(c, "IMP")) { CVal r = c_eqv(c); int t = !cv_true(&l) || cv_true(&r); cv_free(&l); cv_free(&r); l = cv_int(t); }
    return l;
}
int msis_eval(MsiSess *s, const char *cond) {
    if (!cond) return 1;
    CP c = { s, cond, 0 };
    c_ws(&c);
    if (!*c.p) return 1;
    CVal v = c_expr(&c);
    c_ws(&c);
    if (*c.p) c.err = 1;
    int r = c.err ? -1 : cv_true(&v);
    cv_free(&v);
    return r;
}
// condition of a table: empty = true; a syntax error counts as false
static int cond_ok(MsiSess *s, const char *cond) {
    if (!cond || !*cond) return 1;
    int r = msis_eval(s, cond);
    if (r < 0) S_log("koşul çözümlenemedi: %s", cond);
    return r > 0;
}

// ---- product records (Soul's own installer database in the registry) ---------------------------------
#define STATE_ROOT "Software\\Soul\\Installer\\Products\\"
#define UNINST_ROOT "Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\"

static void state_key(const char *code, const char *sub, char *out, size_t n) {
    if (sub && *sub) snprintf(out, n, STATE_ROOT "%s\\%s", code, sub);
    else snprintf(out, n, STATE_ROOT "%s", code);
}
static char *reg_get_str(uint32_t root, const char *key, const char *name) {
    uint32_t type = 0, size = 0;
    if (reg_host_get(root, key, name, &type, NULL, &size) != 0) return NULL;
    char *buf = calloc(1, (size_t)size + 8);
    uint32_t sz = size;
    if (reg_host_get(root, key, name, &type, buf, &sz) != 0) { free(buf); return NULL; }
    if (type == 4 && size >= 4) { uint32_t v; memcpy(&v, buf, 4); free(buf); return xfmt("%u", v); }
    return buf;
}
static void state_set(const char *code, const char *sub, const char *name, const char *val) {
    char k[400];
    state_key(code, sub, k, sizeof k);
    reg_host_set(HKLM, k, name, 1, val, 0);
}
static char *state_get(const char *code, const char *sub, const char *name) {
    char k[400];
    state_key(code, sub, k, sizeof k);
    return reg_get_str(HKLM, k, name);
}
static int state_has(const char *code, const char *sub, const char *name) {
    char k[400];
    uint32_t sz = 0, type;
    state_key(code, sub, k, sizeof k);
    return reg_host_get(HKLM, k, name, &type, NULL, &sz) == 0;
}
int msi_product_registered(const char *code) {
    char k[400];
    state_key(code, NULL, k, sizeof k);
    return reg_host_key_exists(HKLM, k);
}
void msi_package_local(const char *code, char *win, size_t n) { snprintf(win, n, "C:\\Windows\\Installer\\%s.msi", code); }

static int feat_state_get(MsiSess *s, int f, int action) { return f >= 0 && f < s->nf ? (action ? s->f_act[f] : s->f_inst[f]) : IS_UNKNOWN; }
static int comp_install_state(MsiSess *s, int c, int action) { return c >= 0 && c < s->nc ? (action ? s->c_act[c] : s->c_inst[c]) : IS_UNKNOWN; }

static void load_installed_state(MsiSess *s) {
    s->installed = s->code[0] && msi_product_registered(s->code);
    for (int i = 0; i < s->nf; i++) s->f_inst[i] = s->installed && state_has(s->code, "Features", Sx(s, "Feature", i, "Feature")) ? IS_LOCAL : IS_UNKNOWN;
    for (int i = 0; i < s->nc; i++) s->c_inst[i] = s->installed && state_has(s->code, "Components", Sx(s, "Component", i, "Component")) ? IS_LOCAL : IS_UNKNOWN;
}

// ---- opening a package --------------------------------------------------------------------------
MsiDb *msis_db(MsiSess *s) { return s->db; }
void msis_set_handle(MsiSess *s, uint32_t h) { s->hnd = h; }
uint32_t msis_handle(MsiSess *s) { return s->hnd; }
void msis_set_state(MsiSess *s, X86State *st) { s->st = st; s->owner = g_proc; }
int msis_x64(MsiSess *s) { return s->x64; }
int msis_in_deferred(MsiSess *s) { return s->deferred; }
const char *msis_ca_data(MsiSess *s) { return s->ca_data; }

static void set_standard_props(MsiSess *s) {
    msis_setprop(s, "ROOTDRIVE", "C:\\");
    msis_setprop(s, "VersionNT", "603");
    if (s->x64) { msis_setprop(s, "VersionNT64", "603"); msis_setprop(s, "Msix64", "6"); }
    else msis_setprop(s, "Intel", "6");
    msis_setprop(s, "WindowsBuild", "9600");
    msis_setprop(s, "ServicePackLevel", "0");
    msis_setprop(s, "ServicePackLevelMinor", "0");
    msis_setprop(s, "VersionMsi", "5.00");
    msis_setprop(s, "MsiNTProductType", "1");
    msis_setprop(s, "PhysicalMemory", "8192");
    msis_setprop(s, "ScreenX", "1024");
    msis_setprop(s, "ScreenY", "768");
    msis_setprop(s, "ColorBits", "32");
    msis_setprop(s, "UserLanguageID", "1055");
    msis_setprop(s, "SystemLanguageID", "1055");
    msis_setprop(s, "Privileged", "1");
    msis_setprop(s, "AdminUser", "1");
    msis_setprop(s, "ComputerName", "SOUL-PC");
    msis_setprop(s, "LogonUser", "soul");
    msis_setprop(s, "USERNAME", "soul");
    msis_setprop(s, "SourceDir", s->src_dir);
    msis_setprop(s, "OriginalDatabase", s->pkg_win);
    msis_setprop(s, "DATABASE", s->pkg_win);
    msis_setprop(s, "SHORTFILENAMES", "");
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    char b[64];
    snprintf(b, sizeof b, "%02d/%02d/%04d", tm.tm_mon + 1, tm.tm_mday, tm.tm_year + 1900);
    msis_setprop(s, "Date", b);
    snprintf(b, sizeof b, "%02d:%02d:%02d", tm.tm_hour, tm.tm_min, tm.tm_sec);
    msis_setprop(s, "Time", b);
}

MsiSess *msis_open(const char *pkg_host, const char *pkg_win, char *err, size_t errlen) {
    MsiDb *db = msidb_open(pkg_host, err, errlen);
    if (!db) return NULL;
    MsiSess *s = calloc(1, sizeof *s);
    s->db = db;
    s->owner = g_proc;
    snprintf(s->pkg_host, sizeof s->pkg_host, "%s", pkg_host);
    snprintf(s->pkg_win, sizeof s->pkg_win, "%s", pkg_win);
    snprintf(s->src_dir, sizeof s->src_dir, "%s", pkg_win);
    char *sl = strrchr(s->src_dir, '\\');
    if (sl) sl[1] = 0; else snprintf(s->src_dir, sizeof s->src_dir, "C:\\");
    const char *tpl = msidb_summary_str(db, 7);
    s->x64 = tpl && (strncasecmp(tpl, "x64", 3) == 0 || strncasecmp(tpl, "Amd64", 5) == 0 || strncasecmp(tpl, "Intel64", 7) == 0);
    s->nf = msidb_rows(db, "Feature");
    s->nc = msidb_rows(db, "Component");
    s->nd = msidb_rows(db, "Directory");
    s->f_inst = calloc((size_t)s->nf + 1, sizeof(int)); s->f_act = calloc((size_t)s->nf + 1, sizeof(int)); s->f_level = calloc((size_t)s->nf + 1, sizeof(int));
    s->c_inst = calloc((size_t)s->nc + 1, sizeof(int)); s->c_act = calloc((size_t)s->nc + 1, sizeof(int)); s->c_cond = calloc((size_t)s->nc + 1, sizeof(int));
    s->dir_cache = calloc((size_t)s->nd + 1, sizeof(char *));
    s->src_cache = calloc((size_t)s->nd + 1, sizeof(char *));
    for (int i = 0; i < s->nf; i++) { s->f_inst[i] = s->f_act[i] = IS_UNKNOWN; s->f_level[i] = msidb_int(db, "Feature", i, "Level"); }
    for (int i = 0; i < s->nc; i++) { s->c_inst[i] = s->c_act[i] = IS_UNKNOWN; }
    s->level = 1;
    set_standard_props(s);
    int np = msidb_rows(db, "Property");
    for (int i = 0; i < np; i++) {
        const char *n = msidb_str(db, "Property", i, "Property");
        const char *v = msidb_str(db, "Property", i, "Value");
        if (n && v) msis_setprop(s, n, v);
    }
    const char *pc = msidb_str(db, "Property", msidb_find(db, "Property", "Property", "ProductCode"), "Value");
    snprintf(s->code, sizeof s->code, "%s", pc ? pc : "");
    const char *pk = msidb_summary_str(db, 9);
    if (pk) msis_setprop(s, "PackageCode", pk);
    load_installed_state(s);
    if (s->installed) msis_setprop(s, "Installed", "1");
    return s;
}

void msis_free(MsiSess *s) {
    if (!s) return;
    msidb_close(s->db);
    for (int i = 0; i < s->nprops; i++) { free(s->props[i].name); free(s->props[i].val); }
    free(s->props);
    clear_dir_caches(s);
    free(s->dir_cache); free(s->src_cache);
    free(s->f_inst); free(s->f_act); free(s->f_level); free(s->c_inst); free(s->c_act); free(s->c_cond);
    for (int i = 0; i < s->ndone; i++) free(s->done[i]);
    free(s->done);
    for (int i = 0; i < s->nca_ran; i++) free(s->ca_ran[i]);
    free(s->ca_ran);
    for (int i = 0; i < s->ncreated; i++) free(s->created_keys[i]);
    free(s->created_keys);
    free(s->ca_data);
    free(s);
}

// ---- costing: feature and component states ---------------------------------------------------------------
static int list_has(const char *list, const char *name) {
    if (!list) return 0;
    size_t n = strlen(name);
    const char *p = list;
    while (*p) {
        while (*p == ',' || *p == ';' || *p == ' ') p++;
        const char *e = p;
        while (*e && *e != ',' && *e != ';') e++;
        const char *te = e;
        while (te > p && te[-1] == ' ') te--;
        if ((size_t)(te - p) == n && strncmp(p, name, n) == 0) return 1;
        p = e;
    }
    return 0;
}
static int list_all(const char *list) { return list && strcasecmp(list, "ALL") == 0; }

int msis_set_feature(MsiSess *s, const char *f, int state) {
    int r = feat_row(s, f);
    if (r < 0) return 1606;
    s->f_act[r] = state == IS_DEFAULT || state == IS_SOURCE ? IS_LOCAL : state;
    // recompute the components of this feature
    int n = msidb_rows(s->db, "FeatureComponents");
    for (int i = 0; i < n; i++) {
        if (strcmp(Sx(s, "FeatureComponents", i, "Feature_"), f)) continue;
        int c = comp_row(s, Sx(s, "FeatureComponents", i, "Component_"));
        if (c >= 0 && state != IS_ABSENT) s->c_act[c] = IS_LOCAL;
        else if (c >= 0) {
            // absent unless another feature still needs it
            int need = 0;
            for (int k = 0; k < n && !need; k++) {
                if (strcmp(Sx(s, "FeatureComponents", k, "Component_"), Sx(s, "Component", c, "Component"))) continue;
                int fr = feat_row(s, Sx(s, "FeatureComponents", k, "Feature_"));
                if (fr >= 0 && s->f_act[fr] == IS_LOCAL) need = 1;
            }
            s->c_act[c] = need ? IS_LOCAL : IS_ABSENT;
        }
    }
    return 0;
}
int msis_feature_state(MsiSess *s, const char *f, int *inst, int *act) {
    int r = feat_row(s, f);
    if (r < 0) return 1606;
    if (inst) *inst = s->f_inst[r];
    if (act) *act = s->f_act[r];
    return 0;
}
int msis_set_component(MsiSess *s, const char *c, int state) {
    int r = comp_row(s, c);
    if (r < 0) return 1607;
    s->c_act[r] = state == IS_DEFAULT || state == IS_SOURCE ? IS_LOCAL : state;
    return 0;
}
int msis_component_state(MsiSess *s, const char *c, int *inst, int *act) {
    int r = comp_row(s, c);
    if (r < 0) return 1607;
    if (inst) *inst = s->c_inst[r];
    if (act) *act = s->c_act[r];
    return 0;
}
void msis_set_level(MsiSess *s, int level) { s->level = level; setprop_int(s, "INSTALLLEVEL", level); }

static void do_cost(MsiSess *s) {
    char *il = msis_prop(s, "INSTALLLEVEL");
    s->level = il && atoi(il) > 0 ? atoi(il) : 1;
    free(il);
    for (int i = 0; i < s->nf; i++) s->f_level[i] = msidb_int(s->db, "Feature", i, "Level");
    int ncond = msidb_rows(s->db, "Condition");
    for (int i = 0; i < ncond; i++) {
        int f = feat_row(s, Sx(s, "Condition", i, "Feature_"));
        if (f >= 0 && cond_ok(s, Sx(s, "Condition", i, "Condition"))) s->f_level[f] = msidb_int(s->db, "Condition", i, "Level");
    }
    char *addlocal = msis_prop(s, "ADDLOCAL"), *adddef = msis_prop(s, "ADDDEFAULT"), *addsrc = msis_prop(s, "ADDSOURCE");
    char *remove = msis_prop(s, "REMOVE"), *reinstall = msis_prop(s, "REINSTALL");
    int maint = 0;
    for (int i = 0; i < s->nf; i++) if (s->f_inst[i] == IS_LOCAL) maint = 1;
    for (int i = 0; i < s->nf; i++) {
        const char *id = Sx(s, "Feature", i, "Feature");
        int lv = s->f_level[i];
        int act;
        if (maint) act = s->f_inst[i] == IS_LOCAL ? IS_LOCAL : IS_ABSENT;
        else act = lv > 0 && lv <= s->level ? IS_LOCAL : IS_ABSENT;
        if (lv > 0 && (list_all(addlocal) || list_all(adddef) || list_all(addsrc))) act = IS_LOCAL;
        if (list_has(addlocal, id) || list_has(adddef, id) || list_has(addsrc, id)) act = IS_LOCAL;
        if (reinstall && *reinstall && (list_all(reinstall) || list_has(reinstall, id)) && s->f_inst[i] == IS_LOCAL) act = IS_LOCAL;
        if (list_all(remove) || list_has(remove, id)) act = IS_ABSENT;
        s->f_act[i] = act;
    }
    free(addlocal); free(adddef); free(addsrc); free(remove); free(reinstall);
    for (int i = 0; i < s->nc; i++) s->c_act[i] = IS_ABSENT;
    int n = msidb_rows(s->db, "FeatureComponents");
    for (int i = 0; i < n; i++) {
        int f = feat_row(s, Sx(s, "FeatureComponents", i, "Feature_"));
        int c = comp_row(s, Sx(s, "FeatureComponents", i, "Component_"));
        if (f >= 0 && c >= 0 && s->f_act[f] == IS_LOCAL) s->c_act[c] = IS_LOCAL;
    }
    for (int i = 0; i < s->nc; i++) {
        s->c_cond[i] = cond_ok(s, Sx(s, "Component", i, "Condition"));
        if (!s->c_cond[i]) s->c_act[i] = IS_ABSENT;
    }
    s->costed = 1;
}

// ---- external UI --------------------------------------------------------------------------------------
typedef struct { uint64_t fn; uint32_t filter; uint64_t ctx; int wide; int level; } UiCfg;
static UiCfg g_ui = { 0, 0, 0, 0, 1 };
SOUL_PROCESS_VAR(g_ui)

void msi_ui_set(uint64_t fn, uint32_t filter, uint64_t ctx, int wide, uint64_t *prev_fn) {
    if (prev_fn) *prev_fn = g_ui.fn;
    g_ui.fn = fn; g_ui.filter = filter; g_ui.ctx = ctx; g_ui.wide = wide;
}
int msi_ui_level_set(int level) {
    int old = g_ui.level;
    if (level != 0x40 && level != 0) g_ui.level = level;   // 0 = query only
    return old;
}

int msis_ui_message(MsiSess *s, uint32_t type, const char *text) {
    if (!g_ui.fn || !s || s->owner != g_proc || !s->st) return 0;
    uint32_t m = type & 0xFF000000u;
    if (!(g_ui.filter & (1u << (m >> 24)))) return 0;
    uint64_t arg;
    void *mem;
    if (g_ui.wide) mem = u2w_gdup(text);
    else { size_t n = strlen(text) + 1; mem = g_malloc(n); memcpy(mem, text, n); }
    arg = H2G(mem);
    uint64_t args[3] = { g_ui.ctx, type, arg };
    int r = (int)(int32_t)soul_guest_call(s->st, g_ui.fn, 3, args);
    g_free(mem);
    if (r == 2) s->cancelled = 1;
    soul_yield();
    return r;
}
static void ui_msg(MsiSess *s, uint32_t type, const char *text) { msis_ui_message(s, type, text); }

static const struct { const char *a, *d; } k_action_text[] = {
    { "InstallFiles", "Dosyalar kopyalanıyor" }, { "RemoveFiles", "Dosyalar kaldırılıyor" },
    { "WriteRegistryValues", "Kayıt defteri değerleri yazılıyor" }, { "RemoveRegistryValues", "Kayıt defteri değerleri siliniyor" },
    { "CreateShortcuts", "Kısayollar oluşturuluyor" }, { "RemoveShortcuts", "Kısayollar siliniyor" },
    { "CostInitialize", "Disk alanı hesaplanıyor" }, { "FileCost", "Dosya maliyeti hesaplanıyor" }, { "CostFinalize", "Disk alanı hesaplanıyor" },
    { "InstallInitialize", "Kurulum başlatılıyor" }, { "InstallFinalize", "Kurulum tamamlanıyor" },
    { "RegisterProduct", "Ürün kaydediliyor" }, { "PublishFeatures", "Özellikler yayınlanıyor" }, { "PublishProduct", "Ürün yayınlanıyor" },
    { "ProcessComponents", "Bileşenler işleniyor" }, { "WriteIniValues", "INI değerleri yazılıyor" }, { "CreateFolders", "Klasörler oluşturuluyor" },
    { "AppSearch", "Yüklü programlar aranıyor" }, { "LaunchConditions", "Başlatma koşulları denetleniyor" },
    { "RemoveExistingProducts", "Eski sürümler kaldırılıyor" }, { "FindRelatedProducts", "İlgili ürünler aranıyor" },
};
static void ui_action_start(MsiSess *s, const char *name) {
    int r = msidb_find(s->db, "ActionText", "Action", name);
    const char *d = r >= 0 ? msidb_str(s->db, "ActionText", r, "Description") : NULL;
    if (!d || !*d) for (size_t i = 0; i < sizeof k_action_text / sizeof k_action_text[0]; i++) if (!strcmp(k_action_text[i].a, name)) d = k_action_text[i].d;
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    char b[600];
    snprintf(b, sizeof b, "Action %02d:%02d:%02d: %s. %s", tm.tm_hour, tm.tm_min, tm.tm_sec, name, d ? d : "");
    S_log("%s", b);
    ui_msg(s, IM_ACTIONSTART, b);
}
static void ui_progress_reset(MsiSess *s, int total) {
    char b[100];
    snprintf(b, sizeof b, "1: 0 2: %d 3: 0 4: 0", total);
    ui_msg(s, IM_PROGRESS, b);
}
static void ui_progress_step(MsiSess *s, int incr) {
    char b[60];
    snprintf(b, sizeof b, "1: 2 2: %d", incr);
    s->step++;
    ui_msg(s, IM_PROGRESS, b);
}
static void ui_error(MsiSess *s, const char *text) { ui_msg(s, IM_ERROR | 0x10, text); }

// ---- registry roots ---------------------------------------------------------------------------------------
static uint32_t reg_root(MsiSess *s, int root) {
    switch (root) {
    case 0: return HKCR;
    case 1: return HKCU;
    case 2: return HKLM;
    case 3: return HKU;
    default: return allusers(s) ? HKLM : HKCU;
    }
}

// ---- version compare / PE helpers ---------------------------------------------------------------------------------
static int ver_cmp(const char *a, const char *b) {
    while (*a || *b) {
        long x = strtol(a, (char **)&a, 10), y = strtol(b, (char **)&b, 10);
        if (x != y) return x < y ? -1 : 1;
        if (*a == '.') a++;
        if (*b == '.') b++;
        if ((*a && !isdigit((unsigned char)*a) && *a != '.') || (*b && !isdigit((unsigned char)*b) && *b != '.')) break;
    }
    return 0;
}
static int pe_machine_host(const char *host) {
    FILE *f = fopen(host, "rb");
    if (!f) return 0;
    uint8_t h[64];
    int m = 0;
    if (fread(h, 1, 64, f) == 64 && h[0] == 'M' && h[1] == 'Z') {
        uint32_t off;
        memcpy(&off, h + 60, 4);
        uint8_t pe[6];
        if (fseek(f, off, SEEK_SET) == 0 && fread(pe, 1, 6, f) == 6 && !memcmp(pe, "PE\0\0", 4)) m = pe[4] | (pe[5] << 8);
    }
    fclose(f);
    return m;
}

typedef struct { const uint8_t *d; size_t n; uint32_t nsec; const uint8_t *sec; } PeImg;
static int pe_rva2off(const PeImg *p, uint32_t rva, uint32_t *off) {
    for (uint32_t i = 0; i < p->nsec; i++) {
        const uint8_t *s = p->sec + 40 * i;
        uint32_t va, vs, raw;
        memcpy(&vs, s + 8, 4); memcpy(&va, s + 12, 4); memcpy(&raw, s + 20, 4);
        if (rva >= va && rva < va + (vs ? vs : 1)) { *off = rva - va + raw; return 1; }
    }
    return 0;
}
// FILEVERSION of a PE file as "a.b.c.d" and the language of its version resource
int msi_pe_version(const char *host_path, char *ver, size_t vn, char *lang, size_t ln) {
    size_t n;
    uint8_t *d = read_host_file(host_path, &n);
    if (!d) return ERROR_FILE_NOT_FOUND;
    int rc = 1620;
    PeImg p = { d, n, 0, NULL };
    uint32_t pe;
    if (n < 0x100 || d[0] != 'M') goto out;
    memcpy(&pe, d + 60, 4);
    if (pe + 24 > n || memcmp(d + pe, "PE\0\0", 4)) goto out;
    uint16_t nsec, optsz;
    memcpy(&nsec, d + pe + 6, 2);
    memcpy(&optsz, d + pe + 20, 2);
    p.nsec = nsec;
    p.sec = d + pe + 24 + optsz;
    uint16_t magic;
    memcpy(&magic, d + pe + 24, 2);
    uint32_t rsrc_rva, rsrc_sz;
    memcpy(&rsrc_rva, d + pe + 24 + (magic == 0x20b ? 112 : 96) + 16, 4);
    memcpy(&rsrc_sz, d + pe + 24 + (magic == 0x20b ? 112 : 96) + 20, 4);
    (void)rsrc_sz;
    uint32_t base;
    if (!rsrc_rva || !pe_rva2off(&p, rsrc_rva, &base)) { rc = 1620; goto out; }
    // walk type 16 -> first name -> first language
    uint32_t cur = base;
    uint32_t langid = 0;
    for (int level = 0; level < 3; level++) {
        if (cur + 16 > n) goto out;
        uint16_t nn, ni;
        memcpy(&nn, d + cur + 12, 2); memcpy(&ni, d + cur + 14, 2);
        uint32_t ent = cur + 16, found = 0, off = 0, id = 0;
        for (int i = 0; i < nn + ni && ent + 8 <= n; i++, ent += 8) {
            uint32_t nm, o;
            memcpy(&nm, d + ent, 4); memcpy(&o, d + ent + 4, 4);
            if (level == 0 && !(nm & 0x80000000u) && nm == 16) { found = 1; off = o; break; }
            if (level > 0) { found = 1; off = o; id = nm; break; }
        }
        if (!found) goto out;
        if (level == 2) langid = id;
        if (level < 2) { if (!(off & 0x80000000u)) goto out; cur = base + (off & 0x7FFFFFFFu); }
        else {
            uint32_t rva, sz, doff;
            if (base + off + 8 > n) goto out;
            memcpy(&rva, d + base + off, 4); memcpy(&sz, d + base + off + 4, 4);
            if (!pe_rva2off(&p, rva, &doff) || doff + sz > n) goto out;
            // VS_VERSIONINFO: find the VS_FIXEDFILEINFO signature
            for (uint32_t k = doff; k + 20 < doff + sz && k + 20 < n; k += 4) {
                uint32_t sig;
                memcpy(&sig, d + k, 4);
                if (sig == 0xFEEF04BDu) {
                    uint32_t ms, ls;
                    memcpy(&ms, d + k + 8, 4); memcpy(&ls, d + k + 12, 4);
                    if (ver) snprintf(ver, vn, "%u.%u.%u.%u", ms >> 16, ms & 0xFFFF, ls >> 16, ls & 0xFFFF);
                    if (lang) snprintf(lang, ln, "%u", langid);
                    rc = 0;
                    goto out;
                }
            }
            goto out;
        }
    }
out:
    free(d);
    return rc;
}

// ---- custom actions -----------------------------------------------------------------------------------------------
#include "msiexec_stub.h"

static int g_tmpn;
static char *temp_name(const char *ext) {
    return xfmt("C:\\users\\soul\\Temp\\MSI%x%x.%s", (unsigned)(g_proc ? g_proc->pid : 0), (unsigned)++g_tmpn, ext);
}
static int wait_process(SoulProcess *p) {
    while (!p->exited) gil_wait(100);
    return p->exit_code;
}

// loads a program the same way Explorer does; > 32 = started
static int start_exe(const char *file, const char *params, const char *dir, int wait, int *exit_code) {
    SoulProcess *p = NULL;
    uint64_t r = shell_execute_file(file, params, dir, &p);
    if (r <= 32) return 1603;
    *exit_code = 0;
    if (wait && p) *exit_code = wait_process(p);
    return 0;
}

static void ensure_helper(void) {
    const char *win = "C:\\Windows\\System32\\msiexec.exe";
    if (file_exists_win(win)) return;
    if (!k_msiexec_stub_len) return;
    ensure_parent_win(win);
    char h[2600];
    host_of(win, h, sizeof h);
    write_host_file(h, k_msiexec_stub, k_msiexec_stub_len);
}

// runs the entry point of a custom action DLL; the DLL bitness decides where it runs
static int run_dll_ca(MsiSess *s, const char *dll_win, const char *entry, uint32_t *ret) {
    char host[2600];
    host_of(dll_win, host, sizeof host);
    int machine = pe_machine_host(host);
    int want = g_mode32 ? 0x14C : 0x8664;
    if (machine && machine != want) {
        if (machine != 0x14C) { S_log("özel eylem DLL'i bu işlemde çalışamaz (mimari 0x%x)", machine); return 1603; }
        ensure_helper();
        char params[1400];
        snprintf(params, sizeof params, "/ca \"%s\" %s %u", dll_win, entry, s->hnd);
        int code = 0;
        if (start_exe("C:\\Windows\\System32\\msiexec.exe", params, "C:\\Windows", 1, &code) != 0) return 1603;
        *ret = (uint32_t)code;
        return 0;
    }
    X86State *st = s->st;
    uint64_t ll = hle_get_proc(g_proc, "kernel32", "LoadLibraryW");
    uint64_t gp = hle_get_proc(g_proc, "kernel32", "GetProcAddress");
    uint64_t fl = hle_get_proc(g_proc, "kernel32", "FreeLibrary");
    if (!ll || !gp || !fl) return 1603;
    uint16_t *w = u2w_gdup(dll_win);
    uint64_t a[2] = { H2G(w), 0 };
    uint64_t mod = soul_guest_call(st, ll, 1, a);
    g_free(w);
    if (g_mode32) mod &= 0xFFFFFFFFull;
    if (!mod) { S_log("özel eylem DLL'i yüklenemedi: %s", dll_win); return 1603; }
    size_t el = strlen(entry) + 1;
    char *en = g_malloc(el);
    memcpy(en, entry, el);
    a[0] = mod; a[1] = H2G(en);
    uint64_t fn = soul_guest_call(st, gp, 2, a);
    g_free(en);
    if (g_mode32) fn &= 0xFFFFFFFFull;
    if (!fn) { S_log("özel eylem girişi bulunamadı: %s", entry); a[0] = mod; soul_guest_call(st, fl, 1, a); return 1603; }
    a[0] = s->hnd;
    *ret = (uint32_t)soul_guest_call(st, fn, 1, a);
    a[0] = mod;
    soul_guest_call(st, fl, 1, a);
    return 0;
}

static void split_cmdline(const char *cmd, char **file, char **params) {
    while (*cmd == ' ') cmd++;
    if (*cmd == '"') {
        const char *e = strchr(cmd + 1, '"');
        if (e) { *file = strndup(cmd + 1, (size_t)(e - cmd - 1)); cmd = e + 1; while (*cmd == ' ') cmd++; *params = strdup(cmd); return; }
    }
    // an unquoted program path may contain blanks: take the text up to ".exe"
    const char *x = strcasestr(cmd, ".exe");
    const char *e;
    if (x && (x[4] == 0 || x[4] == ' ')) e = x + 4;
    else { e = strchr(cmd, ' '); if (!e) e = cmd + strlen(cmd); }
    *file = strndup(cmd, (size_t)(e - cmd));
    while (*e == ' ') e++;
    *params = strdup(e);
}

static int ca_ran_has(MsiSess *s, const char *name) {
    for (int i = 0; i < s->nca_ran; i++) if (!strcmp(s->ca_ran[i], name)) return 1;
    return 0;
}
static void ca_ran_add(MsiSess *s, const char *name) {
    if (s->nca_ran == s->capca_ran) { s->capca_ran = s->capca_ran ? s->capca_ran * 2 : 8; s->ca_ran = realloc(s->ca_ran, sizeof(char *) * (size_t)s->capca_ran); }
    s->ca_ran[s->nca_ran++] = strdup(name);
}

static int run_ca(MsiSess *s, int crow, int ui_seq) {
    const char *name = Sx(s, "CustomAction", crow, "Action");
    int type = msidb_int(s->db, "CustomAction", crow, "Type");
    if (type == MSI_NULL_INT) type = 0;
    int base = type & 0x3F;
    const char *source = Sx(s, "CustomAction", crow, "Source");
    const char *target = Sx(s, "CustomAction", crow, "Target");
    if ((type & 0x700) == 0x500 || (type & 0x700) == 0x600) { S_log("özel eylem %s: geri alma/onay eylemi atlandı", name); return 0; }
    if (ui_seq && (type & 0x400)) { S_log("özel eylem %s: betik eylemi UI dizisinde atlandı", name); return 0; }
    int once = (type & 0x400) ? 0 : (type & 0x300);
    if (once == 0x200 && ca_ran_has(s, name)) return 0;
    if ((once == 0x100 || once == 0x300) && !ui_seq && ca_ran_has(s, name)) return 0;
    ca_ran_add(s, name);
    int rc = 0;
    uint32_t ret = 0;
    int deferred = (type & 0x400) != 0;
    char *data = NULL;
    if (deferred) { int pi = prop_index(s, name); data = pi >= 0 ? strdup(s->props[pi].val) : NULL; }
    S_log("özel eylem %s (tür 0x%x)", name, type);
    switch (base) {
    case 51: {
        char *v = fmt(s, target);
        msis_setprop(s, source, v);
        free(v);
        break;
    }
    case 35: {
        char *v = fmt(s, target);
        msis_set_target(s, source, v);
        free(v);
        break;
    }
    case 19: {
        char *v = fmt(s, target);
        ui_error(s, v);
        free(v);
        return 1603;
    }
    case 1: case 17: {
        char *dll = NULL, *tmp = NULL;
        if (base == 1) {
            int br = msidb_find(s->db, "Binary", "Name", source);
            size_t len;
            uint8_t *d = br >= 0 ? msidb_cell_stream(s->db, "Binary", br, "Data", &len) : NULL;
            if (!d) { S_log("Binary kaydı bulunamadı: %s", source); return 1603; }
            tmp = temp_name("dll");
            ensure_parent_win(tmp);
            char h[2600];
            host_of(tmp, h, sizeof h);
            write_host_file(h, d, len);
            free(d);
            dll = tmp;
        } else {
            int fr = file_row(s, source);
            if (fr < 0) return 1603;
            dll = file_target(s, fr);
        }
        s->deferred = deferred;
        free(s->ca_data);
        s->ca_data = data; data = NULL;
        int r = run_dll_ca(s, dll, target, &ret);
        s->deferred = 0;
        if (r) rc = r;
        else rc = (int)ret;
        if (tmp) { char h[2600]; host_of(tmp, h, sizeof h); unlink(h); }
        free(dll);
        break;
    }
    case 2: case 18: case 34: case 50: {
        char *file = NULL, *params = NULL, *dir = NULL, *tmp = NULL;
        if (base == 2) {
            int br = msidb_find(s->db, "Binary", "Name", source);
            size_t len;
            uint8_t *d = br >= 0 ? msidb_cell_stream(s->db, "Binary", br, "Data", &len) : NULL;
            if (!d) return 1603;
            tmp = temp_name("exe");
            ensure_parent_win(tmp);
            char h[2600];
            host_of(tmp, h, sizeof h);
            write_host_file(h, d, len);
            free(d);
            file = strdup(tmp);
            params = fmt(s, target);
        } else if (base == 18) {
            int fr = file_row(s, source);
            if (fr < 0) return 1603;
            file = file_target(s, fr);
            params = fmt(s, target);
        } else if (base == 34) {
            dir = msis_dir_path(s, source);
            char *cmd = fmt(s, target);
            split_cmdline(cmd, &file, &params);
            free(cmd);
        } else {
            file = msis_prop(s, source);
            if (!file) return 1603;
            params = fmt(s, target);
        }
        char *all = xfmt("\"%s\" %s", file, params);
        (void)all;
        int code = 0;
        s->deferred = deferred;
        free(s->ca_data);
        s->ca_data = data; data = NULL;
        int r = start_exe(file, params, dir ? dir : NULL, !(type & 0x80), &code);
        s->deferred = 0;
        rc = r ? r : code;
        if (r) S_log("program başlatılamadı: %s", file);
        free(all); free(file); free(params); free(dir);
        if (tmp) { char h[2600]; host_of(tmp, h, sizeof h); unlink(h); free(tmp); }
        break;
    }
    default:
        S_log("özel eylem türü %d desteklenmiyor (betik): %s atlandı", base, name);
        break;
    }
    free(data);
    if (rc == 259) return 259;
    if (rc == 1626 || rc == 0) return 0;
    if (type & 0x40) { S_log("özel eylem %s: dönüş kodu %d yok sayıldı", name, rc); return 0; }
    if (rc == 1602) return 1602;
    char m[300];
    snprintf(m, sizeof m, "Özel eylem %s başarısız oldu (kod %d).", name, rc);
    ui_error(s, m);
    return rc == 1 ? 1603 : rc;
}

// ---- standard actions -----------------------------------------------------------------------------------------------
static int to_install(MsiSess *s, int c) { return c >= 0 && s->c_act[c] == IS_LOCAL; }
static int to_remove(MsiSess *s, int c) {
    if (c < 0 || s->c_act[c] != IS_ABSENT || s->c_inst[c] != IS_LOCAL) return 0;
    int a = msidb_int(s->db, "Component", c, "Attributes");
    return !(a != MSI_NULL_INT && (a & 0x10)); // permanent components stay
}
static int comp_of(MsiSess *s, const char *id) { return comp_row(s, id); }

static int builtin_get_check(MsiSess *s, const char *k) { char *b = builtin_get(s, k); int r = b != NULL; free(b); return r; }
static void rmdir_chain(MsiSess *s, const char *dirkey) {
    // removes the directory and its empty parents up to the first standard folder ("." DefaultDir) / TARGETDIR
    int depth = 0;
    char key[256];
    snprintf(key, sizeof key, "%s", dirkey);
    while (depth++ < 30) {
        int r = dir_row(s, key);
        if (r < 0 || !strcmp(key, "TARGETDIR")) return;
        const char *dd = Sx(s, "Directory", r, "DefaultDir");
        char *t = strdup(dd);
        char *colon = strchr(t, ':');
        if (colon) *colon = 0;
        int dot = !strcmp(long_name(t), ".");
        free(t);
        if (dot || builtin_get_check(s, key)) return;
        char *p = msis_dir_path(s, key);
        if (!p) return;
        size_t n = strlen(p);
        while (n > 3 && p[n - 1] == '\\') p[--n] = 0;
        char h[2600];
        host_of(p, h, sizeof h);
        free(p);
        if (rmdir(h) != 0) return;
        const char *par = Sx(s, "Directory", r, "Directory_Parent");
        if (!*par) return;
        snprintf(key, sizeof key, "%s", par);
    }
}

static int count_work(MsiSess *s) {
    int total = 0;
    int nf = msidb_rows(s->db, "File");
    for (int i = 0; i < nf; i++) if (to_install(s, comp_of(s, Sx(s, "File", i, "Component_")))) total++;
    int nr = msidb_rows(s->db, "Registry");
    for (int i = 0; i < nr; i++) if (to_install(s, comp_of(s, Sx(s, "Registry", i, "Component_")))) total++;
    return total;
}

static int act_remove_files(MsiSess *s) {
    int nf = msidb_rows(s->db, "File");
    // 1. files of components that go away
    for (int i = 0; i < nf; i++) {
        int c = comp_of(s, Sx(s, "File", i, "Component_"));
        if (!to_remove(s, c)) continue;
        char *t = file_target(s, i);
        char h[2600];
        host_of(t, h, sizeof h);
        if (unlink(h) == 0) S_log("silindi: %s", t);
        free(t);
        ui_progress_step(s, 1);
        if (s->cancelled) return 1602;
    }
    // 2. RemoveFile table
    int nr = msidb_rows(s->db, "RemoveFile");
    for (int i = 0; i < nr; i++) {
        int c = comp_of(s, Sx(s, "RemoveFile", i, "Component_"));
        int mode = msidb_int(s->db, "RemoveFile", i, "InstallMode");
        int ok = ((mode & 1) && to_install(s, c)) || ((mode & 2) && (to_remove(s, c) || (c >= 0 && s->c_act[c] == IS_ABSENT && s->installed)));
        if (!ok) continue;
        const char *dprop = Sx(s, "RemoveFile", i, "DirProperty");
        char *dir = msis_dir_path(s, dprop);
        if (!dir) { char *pv = msis_prop(s, dprop); if (pv) { dir = with_slash(pv); free(pv); } }
        if (!dir) continue;
        char *fname = fmt(s, long_name(Sx(s, "RemoveFile", i, "FileName")));
        char h[2600];
        host_of(dir, h, sizeof h);
        if (!*fname) {
            size_t n = strlen(h);
            while (n > 1 && h[n - 1] == '/') h[--n] = 0;
            if (rmdir(h) == 0) S_log("klasör silindi: %s", dir);
        } else {
            DIR *d = opendir(h);
            if (d) {
                struct dirent *de;
                while ((de = readdir(d))) {
                    if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
                    if (!wild_match(fname, de->d_name)) continue;
                    char fp[2700];
                    snprintf(fp, sizeof fp, "%s/%s", h, de->d_name);
                    struct stat sb;
                    if (stat(fp, &sb) == 0 && S_ISREG(sb.st_mode)) { unlink(fp); S_log("silindi: %s", fp); }
                }
                closedir(d);
            }
        }
        free(fname); free(dir);
    }
    // 3. empty component folders
    for (int c = 0; c < s->nc; c++) if (to_remove(s, c)) rmdir_chain(s, Sx(s, "Component", c, "Directory_"));
    return 0;
}

// ---- file installation: cabinets and uncompressed sources ----------------------------------------------------------------
typedef struct { MsiSess *s; char **keys; int *rows; int n; int *done; int err; char errmsg[300]; } CabCtx;

static int install_file_data(MsiSess *s, int frow, const uint8_t *data, size_t len) {
    char *t = file_target(s, frow);
    ensure_parent_win(t);
    char h[2600];
    host_of(t, h, sizeof h);
    int c = comp_of(s, Sx(s, "File", frow, "Component_"));
    int attrs = c >= 0 ? msidb_int(s->db, "Component", c, "Attributes") : 0;
    int skip = (attrs != MSI_NULL_INT && (attrs & 0x80) && file_exists_host(h));
    int rc = 0;
    if (!skip) {
        if (write_host_file(h, data, len) != 0) {
            char m[1300];
            snprintf(m, sizeof m, "%s dosyası yazılamadı.", t);
            ui_error(s, m);
            rc = 1603;
        }
    }
    if (!rc) {
        char *dir = c >= 0 ? comp_dir_path(s, c) : strdup("");
        char m[2000];
        snprintf(m, sizeof m, "File: %s, Directory: %s, Size: %zu", long_name(Sx(s, "File", frow, "FileName")), dir, len);
        free(dir);
        ui_msg(s, IM_ACTIONDATA, m);
        ui_progress_step(s, 1);
    }
    free(t);
    return rc;
}

typedef struct { const char *key; int row; } KR;
static int cmpkr(const void *a, const void *b) { return strcmp(((const KR *)a)->key, ((const KR *)b)->key); }
static int cab_file_cb(void *ctx, const char *name, const uint8_t *data, size_t len, uint32_t attribs) {
    CabCtx *cc = ctx;
    // keys[] is sorted; rows[] / done[] follow the same order through an index array stored in the key pointer table
    int lo = 0, hi = cc->n - 1, idx = -1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        int c = strcmp(name, cc->keys[mid]);
        if (c == 0) { idx = mid; break; }
        if (c < 0) hi = mid - 1; else lo = mid + 1;
    }
    if (idx < 0) return 0;
    if (cc->done[idx]) return 0;
    cc->done[idx] = 1;
    int r = install_file_data(cc->s, cc->rows[idx], data, len);
    if (r) { cc->err = r; return 1; }
    if (cc->s->cancelled) { cc->err = 1602; return 1; }
    return 0;
}

typedef struct { int row; int last; } MediaRef;
static int media_cmp(const void *a, const void *b) { return ((const MediaRef *)a)->last - ((const MediaRef *)b)->last; }

static int act_install_files(MsiSess *s) {
    int nf = msidb_rows(s->db, "File");
    int nm = msidb_rows(s->db, "Media");
    MediaRef *media = calloc((size_t)nm + 1, sizeof *media);
    for (int i = 0; i < nm; i++) { media[i].row = i; int l = msidb_int(s->db, "Media", i, "LastSequence"); media[i].last = l == MSI_NULL_INT ? 0x7FFFFFFF : l; }
    qsort(media, (size_t)nm, sizeof *media, media_cmp);
    int32_t flags = msidb_summary_int(s->db, 15);
    int sum_compressed = flags != MSI_NULL_INT && (flags & 2);
    int rc = 0;
    // group files per media
    int *fmedia = malloc(sizeof(int) * ((size_t)nf + 1));
    int *ffromcab = malloc(sizeof(int) * ((size_t)nf + 1));
    for (int i = 0; i < nf; i++) {
        fmedia[i] = -1;
        ffromcab[i] = 0;
        if (!to_install(s, comp_of(s, Sx(s, "File", i, "Component_")))) { fmedia[i] = -2; continue; }
        int seq = msidb_int(s->db, "File", i, "Sequence");
        int attr = msidb_int(s->db, "File", i, "Attributes");
        int compressed = sum_compressed;
        if (attr != MSI_NULL_INT) { if (attr & 0x4000) compressed = 1; else if (attr & 0x2000) compressed = 0; }
        for (int m = 0; m < nm; m++) if (seq != MSI_NULL_INT && seq <= media[m].last) { fmedia[i] = m; break; }
        if (fmedia[i] >= 0 && compressed && *Sx(s, "Media", media[fmedia[i]].row, "Cabinet")) ffromcab[i] = 1;
    }
    // uncompressed files first, straight from the source tree
    for (int i = 0; i < nf && !rc; i++) {
        if (fmedia[i] == -2 || ffromcab[i]) continue;
        int c = comp_of(s, Sx(s, "File", i, "Component_"));
        char *sd = c >= 0 ? msis_src_path(s, Sx(s, "Component", c, "Directory_")) : NULL;
        char *nm = name_of(Sx(s, "File", i, "FileName"), 0);
        char *src = xfmt("%s%s", sd ? sd : s->src_dir, nm);
        char h[2600];
        host_of(src, h, sizeof h);
        size_t len;
        uint8_t *d = read_host_file(h, &len);
        if (!d) {
            char m[1500];
            snprintf(m, sizeof m, "Kaynak dosya bulunamadı: %s", src);
            ui_error(s, m);
            rc = 1603;
        } else { rc = install_file_data(s, i, d, len); free(d); }
        free(sd); free(nm); free(src);
        if (!rc && s->cancelled) rc = 1602;
    }
    // cabinets
    for (int m = 0; m < nm && !rc; m++) {
        int cnt = 0;
        for (int i = 0; i < nf; i++) if (ffromcab[i] && fmedia[i] == m) cnt++;
        if (!cnt) continue;
        const char *cab = Sx(s, "Media", media[m].row, "Cabinet");
        uint8_t *data;
        size_t len = 0;
        if (cab[0] == '#') data = msidb_stream(s->db, cab + 1, &len);
        else {
            char *p = xfmt("%s%s", s->src_dir, cab);
            char h[2600];
            host_of(p, h, sizeof h);
            data = read_host_file(h, &len);
            free(p);
        }
        if (!data) {
            char mm[600];
            snprintf(mm, sizeof mm, "Kurulum kaynağı bulunamadı: %s", cab);
            ui_error(s, mm);
            rc = 1603;
            break;
        }
        CabCtx cc = { s, malloc(sizeof(char *) * (size_t)cnt), malloc(sizeof(int) * (size_t)cnt), 0, calloc((size_t)cnt, sizeof(int)), 0, "" };
        // sort file keys: pair array
        KR *kr = malloc(sizeof(KR) * (size_t)cnt);
        int k = 0;
        for (int i = 0; i < nf; i++) if (ffromcab[i] && fmedia[i] == m) { kr[k].key = Sx(s, "File", i, "File"); kr[k].row = i; k++; }
        qsort(kr, (size_t)cnt, sizeof(KR), cmpkr);
        for (int i = 0; i < cnt; i++) { cc.keys[i] = (char *)kr[i].key; cc.rows[i] = kr[i].row; }
        cc.n = cnt;
        char err[300] = "";
        int r = cab_extract(data, len, cab_file_cb, &cc, err, sizeof err);
        if (cc.err) rc = cc.err;
        else if (r != 0) {
            char mm[700];
            if (strstr(err, "LZX") || strstr(err, "not supported") || strstr(err, "desteklen"))
                snprintf(mm, sizeof mm, "Kurulum paketindeki '%s' dosyası LZX ile sıkıştırılmış; Soul bu sıkıştırmayı henüz desteklemiyor. (%s)", cab, err);
            else snprintf(mm, sizeof mm, "'%s' dosyası okunamadı: %s", cab, err);
            ui_error(s, mm);
            rc = 1603;
        } else {
            for (int i = 0; i < cnt; i++) if (!cc.done[i]) {
                char mm[700];
                snprintf(mm, sizeof mm, "'%s' içinde %s dosyası yok.", cab, cc.keys[i]);
                ui_error(s, mm);
                rc = 1603;
                break;
            }
        }
        free(kr); free(cc.keys); free(cc.rows); free(cc.done); free(data);
    }
    free(media); free(fmedia); free(ffromcab);
    return rc;
}

// ---- registry ---------------------------------------------------------------------------------------------------------------
static int hexval(int c) { return isdigit(c) ? c - '0' : (tolower(c) - 'a' + 10); }

static void reg_create_key(uint32_t root, const char *key) {
    if (reg_host_key_exists(root, key)) return;
    reg_host_set(root, key, "", 1, "", 0);
    reg_host_delete_value(root, key, "");
}

static void write_reg_row(MsiSess *s, int r) {
    int root = reg_root(s, msidb_int(s->db, "Registry", r, "Root"));
    char *key = fmt(s, Sx(s, "Registry", r, "Key"));
    const char *rawname = Sx(s, "Registry", r, "Name");
    const char *raw = msidb_str(s->db, "Registry", r, "Value");
    if (!strcmp(rawname, "-")) { free(key); return; }
    if (!strcmp(rawname, "+") || !strcmp(rawname, "*")) { reg_create_key((uint32_t)root, key); free(key); return; }
    char *name = fmt(s, rawname);
    if (!raw) { // no value: only the key (default value stays untouched)
        if (!*name) reg_create_key((uint32_t)root, key);
        else reg_host_set((uint32_t)root, key, name, 1, "", 0);
        free(name); free(key);
        return;
    }
    if (raw[0] == '#' && raw[1] == '#') {
        char *v = fmt(s, raw + 1);
        reg_host_set((uint32_t)root, key, name, 1, v, 0);
        free(v);
    } else if (raw[0] == '#' && raw[1] == 'x') {
        char *v = fmt(s, raw + 2);
        size_t n = strlen(v) / 2;
        uint8_t *b = malloc(n + 1);
        for (size_t i = 0; i < n; i++) b[i] = (uint8_t)(hexval(v[2 * i]) * 16 + hexval(v[2 * i + 1]));
        reg_host_set((uint32_t)root, key, name, 3, b, (uint32_t)n);
        free(b); free(v);
    } else if (raw[0] == '#' && raw[1] == '%') {
        char *v = fmt(s, raw + 2);
        reg_host_set((uint32_t)root, key, name, 2, v, 0);
        free(v);
    } else if (raw[0] == '#' && (isdigit((unsigned char)raw[1]) || raw[1] == '-' || raw[1] == '+' || raw[1] == '[')) {
        char *v = fmt(s, raw + 1);
        uint32_t d = (uint32_t)strtol(v, NULL, 10);
        reg_host_set((uint32_t)root, key, name, 4, &d, 4);
        free(v);
    } else if (strstr(raw, "[~]")) {
        size_t n;
        char *v = msis_format(s, raw, NULL, &n);
        // [~]a[~]b[~] -> a\0b\0\0
        char *start = v;
        if (n && v[0] == 0) { start++; n--; }
        char *m = calloc(n + 3, 1);
        memcpy(m, start, n);
        size_t total = n;
        if (total == 0 || m[total - 1] != 0) total++;
        total++;
        reg_host_set((uint32_t)root, key, name, 7, m, (uint32_t)total);
        free(m); free(v);
    } else {
        char *v = fmt(s, raw);
        reg_host_set((uint32_t)root, key, name, 1, v, 0);
        free(v);
    }
    free(name); free(key);
}

static int act_write_registry(MsiSess *s) {
    int n = msidb_rows(s->db, "Registry");
    for (int i = 0; i < n; i++) {
        if (!to_install(s, comp_of(s, Sx(s, "Registry", i, "Component_")))) continue;
        write_reg_row(s, i);
        ui_progress_step(s, 1);
        if (s->cancelled) return 1602;
    }
    return 0;
}
static int act_remove_registry(MsiSess *s) {
    int n = msidb_rows(s->db, "Registry");
    for (int i = 0; i < n; i++) {
        if (!to_remove(s, comp_of(s, Sx(s, "Registry", i, "Component_")))) continue;
        uint32_t root = reg_root(s, msidb_int(s->db, "Registry", i, "Root"));
        char *key = fmt(s, Sx(s, "Registry", i, "Key"));
        const char *rn = Sx(s, "Registry", i, "Name");
        if (!strcmp(rn, "-") || !strcmp(rn, "*")) reg_host_delete_tree(root, key);
        else if (strcmp(rn, "+")) { char *nm = fmt(s, rn); reg_host_delete_value(root, key, nm); free(nm); }
        free(key);
    }
    return 0;
}

// ---- INI files ------------------------------------------------------------------------------------------------------------------
static void ini_edit(const char *host, const char *section, const char *key, const char *value, int mode /*0 set, 1 create, 2 remove, 3 add tag, 4 remove tag*/) {
    size_t len = 0;
    uint8_t *d = read_host_file(host, &len);
    char *text = d ? (char *)d : strdup("");
    Buf out;
    buf_init(&out);
    int in_sec = 0, sec_found = 0, key_done = 0;
    char *p = text;
    char secline[300];
    snprintf(secline, sizeof secline, "[%s]", section);
    while (*p) {
        char *e = strchr(p, '\n');
        size_t ll = e ? (size_t)(e - p) : strlen(p);
        char *line = strndup(p, ll);
        char *tr = line;
        size_t tl = strlen(tr);
        while (tl && (tr[tl - 1] == '\r' || tr[tl - 1] == ' ')) tr[--tl] = 0;
        int keep = 1;
        if (tr[0] == '[') {
            if (in_sec && !key_done && mode != 2 && mode != 4) { buf_str(&out, key); buf_str(&out, "="); buf_str(&out, value ? value : ""); buf_str(&out, "\r\n"); key_done = 1; }
            in_sec = strcasecmp(tr, secline) == 0;
            if (in_sec) sec_found = 1;
        } else if (in_sec) {
            char *eq = strchr(tr, '=');
            size_t kl = strlen(key);
            if (eq && (size_t)(eq - tr) == kl && strncasecmp(tr, key, kl) == 0) {
                if (mode == 0) { buf_str(&out, key); buf_str(&out, "="); buf_str(&out, value ? value : ""); buf_str(&out, "\r\n"); keep = 0; key_done = 1; }
                else if (mode == 1) { key_done = 1; }
                else if (mode == 2) { keep = 0; key_done = 1; }
                else if (mode == 3 || mode == 4) {
                    const char *old = eq + 1;
                    if (mode == 3 && !strstr(old, value)) { buf_str(&out, tr); buf_str(&out, ","); buf_str(&out, value); buf_str(&out, "\r\n"); keep = 0; }
                    key_done = 1;
                    if (mode == 4) {
                        keep = 0;
                        Buf nb; buf_init(&nb);
                        buf_str(&nb, key); buf_str(&nb, "=");
                        char *cp = strdup(old), *sv = NULL;
                        int first = 1;
                        for (char *tk = strtok_r(cp, ",", &sv); tk; tk = strtok_r(NULL, ",", &sv)) {
                            if (!strcmp(tk, value)) continue;
                            if (!first) buf_str(&nb, ",");
                            buf_str(&nb, tk);
                            first = 0;
                        }
                        free(cp);
                        buf_str(&out, nb.p); buf_str(&out, "\r\n");
                        free(nb.p);
                    }
                }
            }
        }
        if (keep) { buf_str(&out, tr); buf_str(&out, "\r\n"); }
        free(line);
        p += ll;
        if (*p == '\n') p++;
    }
    if (mode != 2 && mode != 4) {
        if (!sec_found) { buf_str(&out, secline); buf_str(&out, "\r\n"); }
        if (!key_done) { buf_str(&out, key); buf_str(&out, "="); buf_str(&out, value ? value : ""); buf_str(&out, "\r\n"); }
    }
    write_host_file(host, out.p, out.n);
    free(out.p);
    free(text);
}
static char *ini_file_host(MsiSess *s, const char *dirprop, const char *fname, char *host, size_t hn) {
    char *dir = msis_dir_path(s, dirprop);
    if (!dir) { char *pv = msis_prop(s, dirprop); if (pv) { dir = with_slash(pv); free(pv); } }
    if (!dir) dir = strdup("C:\\Windows\\");
    char *f = name_of(fname, 0);
    char *w = xfmt("%s%s", dir, f);
    ensure_parent_win(w);
    host_of(w, host, hn);
    free(dir); free(f);
    return w;
}
static int act_write_ini(MsiSess *s) {
    int n = msidb_rows(s->db, "IniFile");
    for (int i = 0; i < n; i++) {
        if (!to_install(s, comp_of(s, Sx(s, "IniFile", i, "Component_")))) continue;
        char host[2600];
        char *w = ini_file_host(s, Sx(s, "IniFile", i, "DirProperty"), Sx(s, "IniFile", i, "FileName"), host, sizeof host);
        char *sec = fmt(s, Sx(s, "IniFile", i, "Section")), *key = fmt(s, Sx(s, "IniFile", i, "Key")), *val = fmt(s, Sx(s, "IniFile", i, "Value"));
        int act = msidb_int(s->db, "IniFile", i, "Action");
        ini_edit(host, sec, key, val, act == 1 ? 1 : act == 3 ? 3 : 0);
        free(w); free(sec); free(key); free(val);
    }
    return 0;
}
static int act_remove_ini(MsiSess *s) {
    int n = msidb_rows(s->db, "IniFile");
    for (int i = 0; i < n; i++) {
        if (!to_remove(s, comp_of(s, Sx(s, "IniFile", i, "Component_")))) continue;
        int act = msidb_int(s->db, "IniFile", i, "Action");
        char host[2600];
        char *w = ini_file_host(s, Sx(s, "IniFile", i, "DirProperty"), Sx(s, "IniFile", i, "FileName"), host, sizeof host);
        char *sec = fmt(s, Sx(s, "IniFile", i, "Section")), *key = fmt(s, Sx(s, "IniFile", i, "Key")), *val = fmt(s, Sx(s, "IniFile", i, "Value"));
        if (act == 3) ini_edit(host, sec, key, val, 4);
        else ini_edit(host, sec, key, val, 2);
        free(w); free(sec); free(key); free(val);
    }
    n = msidb_rows(s->db, "RemoveIniFile");
    for (int i = 0; i < n; i++) {
        int c = comp_of(s, Sx(s, "RemoveIniFile", i, "Component_"));
        if (!(to_remove(s, c) || to_install(s, c))) continue;
        int act = msidb_int(s->db, "RemoveIniFile", i, "Action");
        if (act != 2 && act != 4) continue;
        char host[2600];
        char *w = ini_file_host(s, Sx(s, "RemoveIniFile", i, "DirProperty"), Sx(s, "RemoveIniFile", i, "FileName"), host, sizeof host);
        char *sec = fmt(s, Sx(s, "RemoveIniFile", i, "Section")), *key = fmt(s, Sx(s, "RemoveIniFile", i, "Key")), *val = fmt(s, Sx(s, "RemoveIniFile", i, "Value"));
        ini_edit(host, sec, key, val, act);
        free(w); free(sec); free(key); free(val);
    }
    return 0;
}

// ---- environment strings (stored in the registry only) --------------------------------------------------------------------------
static int act_write_env(MsiSess *s, int remove) {
    int n = msidb_rows(s->db, "Environment");
    for (int i = 0; i < n; i++) {
        int c = comp_of(s, Sx(s, "Environment", i, "Component_"));
        if (remove ? !to_remove(s, c) : !to_install(s, c)) continue;
        const char *nm = Sx(s, "Environment", i, "Name");
        int system = 0, rem_on_uninstall = 0, create_only = 0, do_remove_now = 0;
        while (*nm == '=' || *nm == '+' || *nm == '-' || *nm == '!' || *nm == '*') {
            if (*nm == '+') create_only = 1;
            else if (*nm == '-') rem_on_uninstall = 1;
            else if (*nm == '!') do_remove_now = 1;
            else if (*nm == '*') system = 1;
            nm++;
        }
        const char *vraw = Sx(s, "Environment", i, "Value");
        uint32_t root = system ? HKLM : HKCU;
        const char *key = system ? "System\\CurrentControlSet\\Control\\Session Manager\\Environment" : "Environment";
        char *name = fmt(s, nm);
        if (remove) { if (!rem_on_uninstall && !do_remove_now && !*vraw) { /* nothing */ } reg_host_delete_value(root, key, name); }
        else if (do_remove_now) reg_host_delete_value(root, key, name);
        else {
            char *old = reg_get_str(root, key, name);
            char *v = fmt(s, vraw);
            if (create_only && old) { }
            else {
                Buf b; buf_init(&b);
                const char *tag = strstr(vraw, "[~]");
                if (tag && old && *old) {
                    if (tag == vraw) { buf_str(&b, old); buf_str(&b, v + 0); }
                    else { buf_str(&b, v); buf_str(&b, old); }
                    // v contains an embedded NUL for [~]; build explicitly
                    free(b.p); buf_init(&b);
                    size_t pre = (size_t)(tag - vraw);
                    char *pv = strndup(vraw, pre), *pf = fmt(s, pv), *sv = fmt(s, tag + 3);
                    buf_str(&b, pf); buf_str(&b, old); buf_str(&b, sv);
                    free(pv); free(pf); free(sv);
                } else if (tag) {
                    char *pv = strndup(vraw, (size_t)(tag - vraw)), *pf = fmt(s, pv), *sv = fmt(s, tag + 3);
                    buf_str(&b, pf); buf_str(&b, sv);
                    free(pv); free(pf); free(sv);
                } else buf_str(&b, v);
                reg_host_set(root, key, name, 2, b.p, 0);
                free(b.p);
            }
            free(old); free(v);
        }
        free(name);
    }
    return 0;
}

// ---- shortcuts ----------------------------------------------------------------------------------------------------------------------
static char *shortcut_win_path(MsiSess *s, int r) {
    char *dir = msis_dir_path(s, Sx(s, "Shortcut", r, "Directory_"));
    if (!dir) return NULL;
    char *nm = name_of(Sx(s, "Shortcut", r, "Name"), 0);
    char *p = xfmt("%s%s.lnk", dir, nm);
    free(dir); free(nm);
    return p;
}
static int act_create_shortcuts(MsiSess *s) {
    int n = msidb_rows(s->db, "Shortcut");
    for (int i = 0; i < n; i++) {
        if (!to_install(s, comp_of(s, Sx(s, "Shortcut", i, "Component_")))) continue;
        char *lnk = shortcut_win_path(s, i);
        if (!lnk) continue;
        const char *traw = Sx(s, "Shortcut", i, "Target");
        char *target = NULL;
        int fr = feat_row(s, traw);
        if (fr >= 0) { // advertised shortcut: the key file of the feature's first component
            int nfc = msidb_rows(s->db, "FeatureComponents");
            for (int k = 0; k < nfc && !target; k++) {
                if (strcmp(Sx(s, "FeatureComponents", k, "Feature_"), traw)) continue;
                int c = comp_of(s, Sx(s, "FeatureComponents", k, "Component_"));
                if (c < 0) continue;
                int isf;
                char *kp = comp_keypath(s, c, &isf);
                if (kp && isf) target = kp; else free(kp);
            }
        } else target = fmt(s, traw);
        if (!target) target = strdup("");
        char *args = fmt(s, Sx(s, "Shortcut", i, "Arguments"));
        char *desc = fmt(s, Sx(s, "Shortcut", i, "Description"));
        char *wk = NULL;
        const char *wkraw = Sx(s, "Shortcut", i, "WkDir");
        if (*wkraw) { wk = msis_dir_path(s, wkraw); if (!wk) wk = fmt(s, wkraw); }
        char *icon = NULL;
        const char *ik = Sx(s, "Shortcut", i, "Icon_");
        if (*ik) {
            int ir = msidb_find(s->db, "Icon", "Name", ik);
            size_t len;
            uint8_t *d = ir >= 0 ? msidb_cell_stream(s->db, "Icon", ir, "Data", &len) : NULL;
            if (d) {
                icon = xfmt("C:\\Windows\\Installer\\%s\\%s", s->code, ik);
                ensure_parent_win(icon);
                char h[2600];
                host_of(icon, h, sizeof h);
                write_host_file(h, d, len);
                free(d);
            }
        }
        int idx = msidb_int(s->db, "Shortcut", i, "IconIndex");
        int show = msidb_int(s->db, "Shortcut", i, "ShowCmd");
        if (idx == MSI_NULL_INT) idx = 0;
        if (show == MSI_NULL_INT || show == 0) show = 1;
        ensure_parent_win(lnk);
        char h[2600];
        host_of(lnk, h, sizeof h);
        if (soul_lnk_write(h, target, *args ? args : NULL, wk, *desc ? desc : NULL, icon, idx, show) != 0) S_log("kısayol yazılamadı: %s", lnk);
        else S_log("kısayol: %s", lnk);
        free(lnk); free(target); free(args); free(desc); free(wk); free(icon);
        ui_progress_step(s, 1);
    }
    return 0;
}
static int act_remove_shortcuts(MsiSess *s) {
    int n = msidb_rows(s->db, "Shortcut");
    for (int i = 0; i < n; i++) {
        if (!to_remove(s, comp_of(s, Sx(s, "Shortcut", i, "Component_")))) continue;
        char *lnk = shortcut_win_path(s, i);
        if (!lnk) continue;
        char h[2600];
        host_of(lnk, h, sizeof h);
        unlink(h);
        free(lnk);
    }
    return 0;
}

// ---- folders, MoveFiles, DuplicateFiles ------------------------------------------------------------------------------------------------
static int act_create_folders(MsiSess *s) {
    int n = msidb_rows(s->db, "CreateFolder");
    for (int i = 0; i < n; i++) {
        if (!to_install(s, comp_of(s, Sx(s, "CreateFolder", i, "Component_")))) continue;
        char *p = msis_dir_path(s, Sx(s, "CreateFolder", i, "Directory_"));
        if (p) { ensure_dir_win(p); free(p); }
    }
    return 0;
}
static char *dir_or_prop(MsiSess *s, const char *k) {
    if (!k || !*k) return NULL;
    char *d = msis_dir_path(s, k);
    if (d) return d;
    char *pv = msis_prop(s, k);
    if (pv) { d = with_slash(pv); free(pv); }
    return d;
}
static int act_duplicate_files(MsiSess *s) {
    int n = msidb_rows(s->db, "DuplicateFile");
    for (int i = 0; i < n; i++) {
        if (!to_install(s, comp_of(s, Sx(s, "DuplicateFile", i, "Component_")))) continue;
        int fr = file_row(s, Sx(s, "DuplicateFile", i, "File_"));
        char *dest = dir_or_prop(s, Sx(s, "DuplicateFile", i, "DestFolder"));
        if (fr < 0 || !dest) { free(dest); continue; }
        char *src = file_target(s, fr);
        const char *dn = Sx(s, "DuplicateFile", i, "DestName");
        char *nm = *dn ? name_of(dn, 0) : name_of(Sx(s, "File", fr, "FileName"), 0);
        char *dst = xfmt("%s%s", dest, nm);
        ensure_parent_win(dst);
        char hs[2600], hd[2600];
        host_of(src, hs, sizeof hs); host_of(dst, hd, sizeof hd);
        copy_host_file(hs, hd);
        free(dest); free(src); free(nm); free(dst);
    }
    return 0;
}
static int act_move_files(MsiSess *s) {
    int n = msidb_rows(s->db, "MoveFile");
    for (int i = 0; i < n; i++) {
        if (!to_install(s, comp_of(s, Sx(s, "MoveFile", i, "Component_")))) continue;
        char *sd = dir_or_prop(s, Sx(s, "MoveFile", i, "SourceFolder")), *dd = dir_or_prop(s, Sx(s, "MoveFile", i, "DestFolder"));
        const char *sn = Sx(s, "MoveFile", i, "SourceName"), *dn = Sx(s, "MoveFile", i, "DestName");
        int move = msidb_int(s->db, "MoveFile", i, "Options") == 1;
        if (sd && dd && *sn) {
            char hs[2600];
            host_of(sd, hs, sizeof hs);
            DIR *d = opendir(hs);
            if (d) {
                struct dirent *de;
                while ((de = readdir(d))) {
                    if (de->d_name[0] == '.' || !wild_match(sn, de->d_name)) continue;
                    char *dst = xfmt("%s%s", dd, *dn ? dn : de->d_name);
                    ensure_parent_win(dst);
                    char from[2800], hd[2600];
                    snprintf(from, sizeof from, "%s/%s", hs, de->d_name);
                    host_of(dst, hd, sizeof hd);
                    copy_host_file(from, hd);
                    if (move) unlink(from);
                    free(dst);
                }
                closedir(d);
            }
        }
        free(sd); free(dd);
    }
    return 0;
}

// ---- AppSearch ---------------------------------------------------------------------------------------------------------------------------------
static int file_matches_sig(MsiSess *s, const char *sig, const char *host) {
    int r = msidb_find(s->db, "Signature", "Signature", sig);
    if (r < 0) return 1;
    struct stat sb;
    if (stat(host, &sb) != 0) return 0;
    int mn = msidb_int(s->db, "Signature", r, "MinSize"), mx = msidb_int(s->db, "Signature", r, "MaxSize");
    if (mn != MSI_NULL_INT && sb.st_size < mn) return 0;
    if (mx != MSI_NULL_INT && sb.st_size > mx) return 0;
    const char *vmin = msidb_str(s->db, "Signature", r, "MinVersion"), *vmax = msidb_str(s->db, "Signature", r, "MaxVersion");
    if ((vmin && *vmin) || (vmax && *vmax)) {
        char v[64];
        if (msi_pe_version(host, v, sizeof v, NULL, 0) != 0) return 0;
        if (vmin && *vmin && ver_cmp(v, vmin) < 0) return 0;
        if (vmax && *vmax && ver_cmp(v, vmax) > 0) return 0;
    }
    return 1;
}
static char *find_file_depth(MsiSess *s, const char *sig, const char *dirhost, const char *dirwin, const char *fname, int depth) {
    char h[2800];
    snprintf(h, sizeof h, "%s/%s", dirhost, fname);
    if (file_exists_host(h) && file_matches_sig(s, sig, h)) return xfmt("%s%s", dirwin, fname);
    if (depth <= 0) return NULL;
    DIR *d = opendir(dirhost);
    if (!d) return NULL;
    struct dirent *de;
    char *res = NULL;
    while (!res && (de = readdir(d))) {
        if (de->d_name[0] == '.') continue;
        char sub[2800];
        snprintf(sub, sizeof sub, "%s/%s", dirhost, de->d_name);
        struct stat sb;
        if (stat(sub, &sb) != 0 || !S_ISDIR(sb.st_mode)) continue;
        char *w = xfmt("%s%s\\", dirwin, de->d_name);
        res = find_file_depth(s, sig, sub, w, fname, depth - 1);
        free(w);
    }
    closedir(d);
    return res;
}
// result of a locator (malloc'ed path) or NULL
static char *locate_sig(MsiSess *s, const char *sig, int depth) {
    if (depth > 8) return NULL;
    int rl = msidb_find(s->db, "RegLocator", "Signature_", sig);
    if (rl >= 0) {
        int root = msidb_int(s->db, "RegLocator", rl, "Root");
        uint32_t rr = reg_root(s, root == MSI_NULL_INT ? 2 : root);
        char *key = fmt(s, Sx(s, "RegLocator", rl, "Key"));
        const char *nm = msidb_str(s->db, "RegLocator", rl, "Name");
        char *nmf = nm ? fmt(s, nm) : strdup("");
        int type = msidb_int(s->db, "RegLocator", rl, "Type");
        if (type == MSI_NULL_INT) type = 0;
        uint32_t t = 0, sz = 0;
        char *res = NULL;
        if (reg_host_get(rr, key, nmf, &t, NULL, &sz) == 0) {
            char *buf = calloc(1, sz + 8);
            sz += 0;
            uint32_t sz2 = sz;
            reg_host_get(rr, key, nmf, &t, buf, &sz2);
            char *val = t == 4 ? xfmt("#%d", *(int32_t *)buf) : strdup(buf);
            if ((type & 0xF) == 2) res = val;
            else {
                // a path: strip quotes
                char *v = val;
                size_t l = strlen(v);
                if (l > 1 && v[0] == '"' && v[l - 1] == '"') { v[l - 1] = 0; v++; }
                char *w = strdup(v);
                free(val);
                char h[2600];
                host_of(w, h, sizeof h);
                int isdir = 0;
                struct stat sb;
                if (stat(h, &sb) == 0) isdir = S_ISDIR(sb.st_mode);
                if ((type & 0xF) == 0) { // directory
                    if (!isdir) { char *sl = strrchr(w, '\\'); if (sl) *sl = 0; }
                    int sr = msidb_find(s->db, "Signature", "Signature", sig);
                    if (sr >= 0) {
                        char *dw = with_slash(w), dh[2600];
                        host_of(dw, dh, sizeof dh);
                        char *f = find_file_depth(s, sig, dh, dw, long_name(Sx(s, "Signature", sr, "FileName")), 0);
                        free(dw); free(w);
                        res = f;
                    } else { size_t k = strlen(w); while (k > 3 && w[k - 1] == '\\') w[--k] = 0; res = w; }
                } else { // file path
                    if (stat(h, &sb) == 0 && !isdir && file_matches_sig(s, sig, h)) res = w; else free(w);
                }
            }
            free(buf);
        }
        free(key); free(nmf);
        return res;
    }
    int dl = msidb_find(s->db, "DrLocator", "Signature_", sig);
    if (dl >= 0) {
        const char *par = Sx(s, "DrLocator", dl, "Parent");
        char *base = *par ? locate_sig(s, par, depth + 1) : NULL;
        const char *praw = msidb_str(s->db, "DrLocator", dl, "Path");
        char *path = NULL;
        if (praw && *praw) { char *pf = fmt(s, praw); if (base) { path = xfmt("%s\\%s", base, pf); free(pf); } else path = pf; }
        else if (base) path = strdup(base);
        free(base);
        if (!path) return NULL;
        int dep = msidb_int(s->db, "DrLocator", dl, "Depth");
        if (dep == MSI_NULL_INT) dep = 0;
        char *dw = with_slash(path), dh[2600];
        free(path);
        host_of(dw, dh, sizeof dh);
        struct stat sb;
        if (stat(dh, &sb) != 0 || !S_ISDIR(sb.st_mode)) { free(dw); return NULL; }
        int sr = msidb_find(s->db, "Signature", "Signature", sig);
        char *res = NULL;
        if (sr >= 0) res = find_file_depth(s, sig, dh, dw, long_name(Sx(s, "Signature", sr, "FileName")), dep);
        else { size_t k = strlen(dw); while (k > 3 && dw[k - 1] == '\\') dw[--k] = 0; res = strdup(dw); }
        free(dw);
        return res;
    }
    int cl = msidb_find(s->db, "CompLocator", "Signature_", sig);
    if (cl >= 0) {
        char out[1200];
        int st = msi_component_path(NULL, Sx(s, "CompLocator", cl, "ComponentId"), out, sizeof out);
        if (st != IS_LOCAL) return NULL;
        int type = msidb_int(s->db, "CompLocator", cl, "Type");
        if (type != 1) { char *sl = strrchr(out, '\\'); if (sl) *sl = 0; }
        return strdup(out);
    }
    return NULL;
}
static int act_app_search(MsiSess *s) {
    int n = msidb_rows(s->db, "AppSearch");
    for (int i = 0; i < n; i++) {
        const char *prop = Sx(s, "AppSearch", i, "Property"), *sig = Sx(s, "AppSearch", i, "Signature_");
        char *r = locate_sig(s, sig, 0);
        if (r) { msis_setprop(s, prop, r); S_log("AppSearch %s = %s", prop, r); free(r); }
    }
    return 0;
}
static int act_launch_conditions(MsiSess *s) {
    int n = msidb_rows(s->db, "LaunchCondition");
    for (int i = 0; i < n; i++) {
        const char *cond = Sx(s, "LaunchCondition", i, "Condition");
        int r = msis_eval(s, cond);
        if (r < 0) { S_log("başlatma koşulu çözümlenemedi: %s", cond); continue; }
        if (!r) {
            char *m = fmt(s, Sx(s, "LaunchCondition", i, "Description"));
            ui_error(s, m);
            S_log("başlatma koşulu sağlanmadı: %s", cond);
            free(m);
            return 1603;
        }
    }
    return 0;
}

// ---- registered products ---------------------------------------------------------------------------------------------------------------------
static int str_cmp_p(const void *a, const void *b) { return strcmp((const char *)a, (const char *)b); }
static int list_products(char codes[][40], int max) {
    char h[2600];
    host_of("C:\\Windows\\Installer", h, sizeof h);
    DIR *d = opendir(h);
    int n = 0;
    if (!d) return 0;
    struct dirent *de;
    while (n < max && (de = readdir(d))) {
        size_t l = strlen(de->d_name);
        if (l == 38 + 4 && de->d_name[0] == '{' && !strcasecmp(de->d_name + l - 4, ".msi")) {
            char c[40];
            snprintf(c, sizeof c, "%.38s", de->d_name);
            if (msi_product_registered(c)) memcpy(codes[n++], c, 40);
        }
    }
    closedir(d);
    qsort(codes, (size_t)n, 40, str_cmp_p);
    return n;
}
int msi_enum_products(int index, char *code) {
    static char codes[256][40];
    int n = list_products(codes, 256);
    if (index < 0 || index >= n) return 259;
    snprintf(code, 39, "%s", codes[index]);
    return 0;
}
int msi_product_info(const char *code, const char *attr, char *out, size_t outlen) {
    if (!msi_product_registered(code)) return 2;
    static const struct { const char *attr, *name; } map[] = {
        { "InstalledProductName", "ProductName" }, { "ProductName", "ProductName" }, { "VersionString", "Version" },
        { "InstallLocation", "InstallLocation" }, { "InstallSource", "InstallSource" }, { "InstallDate", "InstallDate" },
        { "Publisher", "Publisher" }, { "LocalPackage", "LocalPackage" }, { "PackageCode", "PackageCode" },
        { "Language", "Language" }, { "InstalledLanguage", "Language" }, { "HelpLink", "HelpLink" }, { "HelpTelephone", "HelpTelephone" },
        { "URLInfoAbout", "URLInfoAbout" }, { "URLUpdateInfo", "URLUpdateInfo" }, { "ProductID", "ProductID" }, { "RegCompany", "RegCompany" },
        { "RegOwner", "RegOwner" }, { "ProductIcon", "ProductIcon" }, { "Transforms", "Transforms" }, { "AssignmentType", "AssignmentType" },
        { "PackageName", "PackageName" }, { "InstanceType", "InstanceType" }, { "AuthorizedLUAApp", "AuthorizedLUAApp" },
        { "Version", "VersionNumber" }, { "VersionMajor", "VersionMajor" }, { "VersionMinor", "VersionMinor" },
    };
    for (size_t i = 0; i < sizeof map / sizeof map[0]; i++) {
        if (strcasecmp(map[i].attr, attr)) continue;
        char *v;
        if (!strcmp(map[i].name, "VersionNumber")) {
            char *ver = state_get(code, NULL, "Version");
            unsigned a = 0, b = 0, c = 0;
            if (ver) sscanf(ver, "%u.%u.%u", &a, &b, &c);
            free(ver);
            v = xfmt("%u", (a << 24) | (b << 16) | (c & 0xFFFF));
        } else if (!strcmp(map[i].name, "AssignmentType")) v = strdup("1");
        else if (!strcmp(map[i].name, "PackageName")) { char *lp = state_get(code, NULL, "LocalPackage"); v = lp ? strdup(strrchr(lp, '\\') ? strrchr(lp, '\\') + 1 : lp) : strdup(""); free(lp); }
        else if (!strcmp(map[i].name, "VersionMajor") || !strcmp(map[i].name, "VersionMinor")) {
            char *ver = state_get(code, NULL, "Version");
            unsigned a = 0, b = 0;
            if (ver) sscanf(ver, "%u.%u", &a, &b);
            free(ver);
            v = xfmt("%u", map[i].name[7] == 'a' ? a : b);
        } else { v = state_get(code, NULL, map[i].name); if (!v) v = strdup(""); }
        snprintf(out, outlen, "%s", v);
        free(v);
        return 0;
    }
    return 1;
}
int msi_component_path(const char *product, const char *comp_id, char *out, size_t outlen) {
    char up[64];
    size_t i = 0;
    for (; comp_id[i] && i < 60; i++) up[i] = (char)toupper((unsigned char)comp_id[i]);
    up[i] = 0;
    char codes[256][40];
    int n = list_products(codes, 256);
    for (int k = 0; k < n; k++) {
        if (product && *product && strcasecmp(product, codes[k])) continue;
        char *v = state_get(codes[k], "ComponentIds", up);
        if (v) {
            snprintf(out, outlen, "%s", v);
            free(v);
            return IS_LOCAL;
        }
    }
    return IS_UNKNOWN;
}
int msi_feature_installed(const char *product, const char *feature) {
    if (!msi_product_registered(product)) return IS_UNKNOWN;
    return state_has(product, "Features", feature) ? IS_LOCAL : IS_UNKNOWN;
}

// ---- upgrades ----------------------------------------------------------------------------------------------------------------------------------
static int act_find_related(MsiSess *s) {
    int n = msidb_rows(s->db, "Upgrade");
    if (!n) return 0;
    static char codes[256][40];
    int np = list_products(codes, 256);
    char (*local)[40] = malloc((size_t)np * 40 + 40);
    memcpy(local, codes, (size_t)np * 40);
    for (int i = 0; i < n; i++) {
        const char *uc = Sx(s, "Upgrade", i, "UpgradeCode");
        const char *vmin = Sx(s, "Upgrade", i, "VersionMin"), *vmax = Sx(s, "Upgrade", i, "VersionMax");
        int attrs = msidb_int(s->db, "Upgrade", i, "Attributes");
        if (attrs == MSI_NULL_INT) attrs = 0;
        const char *ap = Sx(s, "Upgrade", i, "ActionProperty");
        if (!*ap) continue;
        Buf found;
        buf_init(&found);
        for (int k = 0; k < np; k++) {
            if (!strcasecmp(local[k], s->code)) continue;
            char *pu = state_get(local[k], NULL, "UpgradeCode");
            char *pv = state_get(local[k], NULL, "Version");
            int match = pu && !strcasecmp(pu, uc);
            if (match && pv) {
                if (*vmin) { int c = ver_cmp(pv, vmin); if (c < 0 || (c == 0 && !(attrs & 0x100))) match = 0; }
                if (*vmax) { int c = ver_cmp(pv, vmax); if (c > 0 || (c == 0 && !(attrs & 0x200))) match = 0; }
            }
            if (match) { if (found.n) buf_str(&found, ";"); buf_str(&found, local[k]); }
            free(pu); free(pv);
        }
        if (found.n) { msis_setprop(s, ap, found.p); S_log("ilgili ürün (%s): %s", ap, found.p); }
        free(found.p);
    }
    free(local);
    return 0;
}
static int run_nested_remove(MsiSess *parent, const char *code);
static int act_remove_existing(MsiSess *s) {
    int n = msidb_rows(s->db, "Upgrade");
    for (int i = 0; i < n; i++) {
        int attrs = msidb_int(s->db, "Upgrade", i, "Attributes");
        if (attrs == MSI_NULL_INT) attrs = 0;
        if (attrs & 2) continue;
        const char *ap = Sx(s, "Upgrade", i, "ActionProperty");
        char *v = *ap ? msis_prop(s, ap) : NULL;
        if (!v) continue;
        char *save = NULL;
        for (char *c = strtok_r(v, ";", &save); c; c = strtok_r(NULL, ";", &save)) {
            S_log("eski ürün kaldırılıyor: %s", c);
            int r = run_nested_remove(s, c);
            if (r && r != 1605 && !(attrs & 4)) { free(v); return r; }
        }
        free(v);
    }
    return 0;
}

// ---- registration -------------------------------------------------------------------------------------------------------------------------------
static void rm_dir_flat(const char *host) {
    DIR *d = opendir(host);
    if (d) {
        struct dirent *de;
        while ((de = readdir(d))) {
            if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
            char p[2800];
            snprintf(p, sizeof p, "%s/%s", host, de->d_name);
            unlink(p);
        }
        closedir(d);
    }
    rmdir(host);
}

static void set_dword(uint32_t root, const char *key, const char *name, uint32_t v) { reg_host_set(root, key, name, 4, &v, 4); }
static void set_str(uint32_t root, const char *key, const char *name, const char *v) { reg_host_set(root, key, name, 1, v ? v : "", 0); }

static void register_product(MsiSess *s) {
    char key[400];
    char pkgwin[300];
    msi_package_local(s->code, pkgwin, sizeof pkgwin);
    ensure_parent_win(pkgwin);
    char hp[2600];
    host_of(pkgwin, hp, sizeof hp);
    if (strcmp(hp, s->pkg_host) != 0) copy_host_file(s->pkg_host, hp);
    char *name = msis_prop(s, "ProductName"), *ver = msis_prop(s, "ProductVersion"), *pub = msis_prop(s, "Manufacturer");
    char *loc = msis_prop(s, "ARPINSTALLLOCATION");
    const char *cands[] = { "INSTALLDIR", "INSTALLFOLDER", "APPLICATIONFOLDER", "INSTALLLOCATION", "APPLICATIONROOTDIRECTORY" };
    for (size_t i = 0; !loc && i < sizeof cands / sizeof cands[0]; i++) {
        if (dir_row(s, cands[i]) >= 0) loc = msis_dir_path(s, cands[i]);
    }
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    char date[16];
    snprintf(date, sizeof date, "%04d%02d%02d", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday);
    snprintf(key, sizeof key, UNINST_ROOT "%s", s->code);
    set_str(HKLM, key, "DisplayName", name ? name : "");
    set_str(HKLM, key, "DisplayVersion", ver ? ver : "");
    set_str(HKLM, key, "Publisher", pub ? pub : "");
    set_str(HKLM, key, "InstallLocation", loc ? loc : "");
    set_str(HKLM, key, "InstallSource", s->src_dir);
    set_str(HKLM, key, "InstallDate", date);
    char *us = xfmt("MsiExec.exe /X%s", s->code), *mp = xfmt("MsiExec.exe /I%s", s->code);
    set_str(HKLM, key, "UninstallString", us);
    set_str(HKLM, key, "ModifyPath", mp);
    free(us); free(mp);
    set_dword(HKLM, key, "WindowsInstaller", 1);
    {
        unsigned a = 0, b = 0;
        if (ver) sscanf(ver, "%u.%u", &a, &b);
        set_dword(HKLM, key, "VersionMajor", a);
        set_dword(HKLM, key, "VersionMinor", b);
    }
    // estimated size in KB
    uint64_t total = 0;
    int nf = msidb_rows(s->db, "File");
    for (int i = 0; i < nf; i++) {
        int sz = msidb_int(s->db, "File", i, "FileSize");
        if (sz != MSI_NULL_INT && to_install(s, comp_of(s, Sx(s, "File", i, "Component_")))) total += (uint64_t)sz;
    }
    set_dword(HKLM, key, "EstimatedSize", (uint32_t)(total / 1024));
    char *icon = msis_prop(s, "ARPPRODUCTICON");
    if (icon) {
        int ir = msidb_find(s->db, "Icon", "Name", icon);
        size_t len;
        uint8_t *d = ir >= 0 ? msidb_cell_stream(s->db, "Icon", ir, "Data", &len) : NULL;
        if (d) {
            char *iw = xfmt("C:\\Windows\\Installer\\%s\\%s", s->code, icon), ih[2600];
            ensure_parent_win(iw);
            host_of(iw, ih, sizeof ih);
            write_host_file(ih, d, len);
            set_str(HKLM, key, "DisplayIcon", iw);
            free(iw); free(d);
        }
        free(icon);
    }
    static const struct { const char *prop, *val; int dw; } arp[] = {
        { "ARPNOMODIFY", "NoModify", 1 }, { "ARPNOREPAIR", "NoRepair", 1 }, { "ARPNOREMOVE", "NoRemove", 1 }, { "ARPSYSTEMCOMPONENT", "SystemComponent", 1 },
        { "ARPHELPLINK", "HelpLink", 0 }, { "ARPURLINFOABOUT", "URLInfoAbout", 0 }, { "ARPURLUPDATEINFO", "URLUpdateInfo", 0 },
        { "ARPHELPTELEPHONE", "HelpTelephone", 0 }, { "ARPCONTACT", "Contact", 0 }, { "ARPCOMMENTS", "Comments", 0 }, { "ARPREADME", "Readme", 0 },
    };
    for (size_t i = 0; i < sizeof arp / sizeof arp[0]; i++) {
        char *v = msis_prop(s, arp[i].prop);
        if (!v) continue;
        if (arp[i].dw) set_dword(HKLM, key, arp[i].val, 1);
        else set_str(HKLM, key, arp[i].val, v);
        free(v);
    }
    // Soul's own record
    state_set(s->code, NULL, "LocalPackage", pkgwin);
    char *uc = msis_prop(s, "UpgradeCode"), *lang = msis_prop(s, "ProductLanguage"), *pid = msis_prop(s, "PackageCode");
    state_set(s->code, NULL, "ProductName", name);
    state_set(s->code, NULL, "Version", ver);
    state_set(s->code, NULL, "Publisher", pub);
    state_set(s->code, NULL, "UpgradeCode", uc);
    state_set(s->code, NULL, "Language", lang);
    state_set(s->code, NULL, "PackageCode", pid);
    state_set(s->code, NULL, "InstallLocation", loc);
    state_set(s->code, NULL, "InstallSource", s->src_dir);
    state_set(s->code, NULL, "InstallDate", date);
    int nd = msidb_rows(s->db, "Directory");
    for (int i = 0; i < nd; i++) {
        const char *id = Sx(s, "Directory", i, "Directory");
        char *p = msis_dir_path(s, id);
        if (p) { state_set(s->code, "Dirs", id, p); free(p); }
    }
    free(name); free(ver); free(pub); free(loc); free(uc); free(lang); free(pid);
}

static void unregister_product(MsiSess *s) {
    char key[400];
    snprintf(key, sizeof key, UNINST_ROOT "%s", s->code);
    reg_host_delete_tree(HKLM, key);
    state_key(s->code, NULL, key, sizeof key);
    reg_host_delete_tree(HKLM, key);
    char pkgwin[300], h[2600];
    msi_package_local(s->code, pkgwin, sizeof pkgwin);
    host_of(pkgwin, h, sizeof h);
    if (strcmp(h, s->pkg_host) != 0) unlink(h);
    else unlink(h); // the running copy is no longer needed (the database stays open in memory)
    char *dw = xfmt("C:\\Windows\\Installer\\%s", s->code);
    host_of(dw, h, sizeof h);
    rm_dir_flat(h);
    free(dw);
}

// writes the final feature / component state (InstallFinalize)
static void commit_state(MsiSess *s) {
    if (s->committed) return;
    s->committed = 1;
    int removing = s->remove_all;
    int any_local = 0;
    for (int i = 0; i < s->nf; i++) if (s->f_act[i] == IS_LOCAL) any_local = 1;
    if (removing && !any_local) {
        if (s->installed) { unregister_product(s); S_log("ürün kaydı silindi: %s", s->code); }
        return;
    }
    if (!s->reg_product && !s->installed) return;
    register_product(s);
    for (int i = 0; i < s->nf; i++) {
        const char *id = Sx(s, "Feature", i, "Feature");
        if (s->f_act[i] == IS_LOCAL) state_set(s->code, "Features", id, "local");
        else { char k[400]; state_key(s->code, "Features", k, sizeof k); reg_host_delete_value(HKLM, k, id); }
    }
    for (int i = 0; i < s->nc; i++) {
        const char *id = Sx(s, "Component", i, "Component");
        char k[400], k2[400];
        state_key(s->code, "Components", k, sizeof k);
        state_key(s->code, "ComponentIds", k2, sizeof k2);
        char up[64];
        size_t n = 0;
        for (const char *p = Sx(s, "Component", i, "ComponentId"); *p && n < 60; p++) up[n++] = (char)toupper((unsigned char)*p);
        up[n] = 0;
        if (s->c_act[i] == IS_LOCAL) {
            int isf;
            char *kp = comp_keypath(s, i, &isf);
            if (!kp) kp = comp_dir_path(s, i);
            reg_host_set(HKLM, k, id, 1, kp, 0);
            if (*up) reg_host_set(HKLM, k2, up, 1, kp, 0);
            free(kp);
        } else if (s->c_inst[i] == IS_LOCAL && !to_remove(s, i) ) { /* permanent: record stays */ }
        else {
            reg_host_delete_value(HKLM, k, id);
            if (*up) reg_host_delete_value(HKLM, k2, up);
        }
    }
}

// ---- action dispatch and sequences ------------------------------------------------------------------------------------------------------------------
static int done_has(MsiSess *s, const char *n) {
    for (int i = 0; i < s->ndone; i++) if (!strcmp(s->done[i], n)) return 1;
    return 0;
}
static void done_add(MsiSess *s, const char *n) {
    if (s->ndone == s->capdone) { s->capdone = s->capdone ? s->capdone * 2 : 16; s->done = realloc(s->done, sizeof(char *) * (size_t)s->capdone); }
    s->done[s->ndone++] = strdup(n);
}
static int work_total(MsiSess *s) {
    int t = count_work(s);
    int nf = msidb_rows(s->db, "File");
    for (int i = 0; i < nf; i++) if (to_remove(s, comp_of(s, Sx(s, "File", i, "Component_")))) t++;
    return t ? t : 1;
}

static const char *const k_skipped[] = {
    "SelfRegModules", "SelfUnregModules", "RegisterTypeLibraries", "UnregisterTypeLibraries", "InstallServices", "DeleteServices", "StartServices",
    "StopServices", "RegisterFonts", "UnregisterFonts", "InstallODBC", "RemoveODBC", "RegisterClassInfo", "UnregisterClassInfo", "RegisterProgIdInfo",
    "UnregisterProgIdInfo", "RegisterExtensionInfo", "UnregisterExtensionInfo", "RegisterMIMEInfo", "UnregisterMIMEInfo", "RegisterUser", "ScheduleReboot",
    "ForceReboot", "ResolveSource", "PublishComponents", "UnpublishComponents", "RegisterComPlus", "UnregisterComPlus", "MigrateFeatureStates", "MsiPublishAssemblies",
    "MsiUnpublishAssemblies", "BindImage", "PatchFiles", "IsolateComponents", "RMCCPSearch", "SetODBCFolders", "CCPSearch", "InstallValidate", "DisableRollback",
    "ValidateProductID", "FileCost", "CostInitialize", "ExecuteAction", NULL
};

static int run_std(MsiSess *s, const char *a, int *handled) {
    *handled = 1;
    if (!strcmp(a, "AppSearch")) return act_app_search(s);
    if (!strcmp(a, "LaunchConditions")) return act_launch_conditions(s);
    if (!strcmp(a, "FindRelatedProducts")) return act_find_related(s);
    if (!strcmp(a, "RemoveExistingProducts")) return act_remove_existing(s);
    if (!strcmp(a, "CostFinalize")) { do_cost(s); return 0; }
    if (!strcmp(a, "InstallInitialize")) { if (!s->costed) do_cost(s); s->total = work_total(s); s->step = 0; ui_progress_reset(s, s->total); return 0; }
    if (!strcmp(a, "ProcessComponents")) { s->flag_pc = 1; return 0; }
    if (!strcmp(a, "UnpublishFeatures") || !strcmp(a, "PublishFeatures")) { s->flag_pf = 1; return 0; }
    if (!strcmp(a, "RegisterProduct")) { s->reg_product = !s->remove_all; return 0; }
    if (!strcmp(a, "PublishProduct") || !strcmp(a, "UnpublishProduct")) return 0;
    if (!strcmp(a, "InstallFinalize")) { commit_state(s); return 0; }
    // actions below need the costing result
    if (!s->costed) do_cost(s);
    if (!strcmp(a, "RemoveRegistryValues")) return act_remove_registry(s);
    if (!strcmp(a, "RemoveShortcuts")) return act_remove_shortcuts(s);
    if (!strcmp(a, "RemoveFiles") || !strcmp(a, "RemoveFolders")) return act_remove_files(s);
    if (!strcmp(a, "RemoveIniValues")) return act_remove_ini(s);
    if (!strcmp(a, "RemoveEnvironmentStrings")) return act_write_env(s, 1);
    if (!strcmp(a, "CreateFolders")) return act_create_folders(s);
    if (!strcmp(a, "InstallFiles")) return act_install_files(s);
    if (!strcmp(a, "MoveFiles")) return act_move_files(s);
    if (!strcmp(a, "DuplicateFiles")) return act_duplicate_files(s);
    if (!strcmp(a, "RemoveDuplicateFiles")) return 0;
    if (!strcmp(a, "WriteRegistryValues")) return act_write_registry(s);
    if (!strcmp(a, "WriteIniValues")) return act_write_ini(s);
    if (!strcmp(a, "WriteEnvironmentStrings")) return act_write_env(s, 0);
    if (!strcmp(a, "CreateShortcuts")) return act_create_shortcuts(s);
    *handled = 0;
    return 0;
}

static int is_once_action(const char *a) {
    return !strcmp(a, "AppSearch") || !strcmp(a, "LaunchConditions") || !strcmp(a, "FindRelatedProducts") || !strcmp(a, "CostInitialize") ||
           !strcmp(a, "CostFinalize") || !strcmp(a, "FileCost") || !strcmp(a, "ValidateProductID");
}

static int run_action(MsiSess *s, const char *name, int ui_seq) {
    if (s->cancelled) return 1602;
    int cr = msidb_find(s->db, "CustomAction", "Action", name);
    if (cr >= 0 && !(!strcmp(name, "CostFinalize"))) {
        ui_action_start(s, name);
        int r = run_ca(s, cr, ui_seq);
        return s->cancelled && !r ? 1602 : r;
    }
    if (ui_seq && msidb_find(s->db, "Dialog", "Dialog", name) >= 0) { S_log("iletişim kutusu atlandı: %s", name); return 0; }
    if (is_once_action(name) && done_has(s, name)) return 0;
    for (int i = 0; k_skipped[i]; i++) if (!strcmp(k_skipped[i], name)) {
        if (!strcmp(name, "CostInitialize") || !strcmp(name, "FileCost") || !strcmp(name, "ValidateProductID")) { done_add(s, name); return 0; }
        if (strcmp(name, "ExecuteAction")) S_log("eylem atlandı: %s", name);
        return 0;
    }
    int handled;
    ui_action_start(s, name);
    int r = run_std(s, name, &handled);
    if (!handled) { S_log("bilinmeyen eylem atlandı: %s", name); return 0; }
    if (is_once_action(name) && !r) done_add(s, name);
    if (s->cancelled && !r) return 1602;
    soul_yield();
    return r;
}

typedef struct { int seq; int row; } SeqRow;
static int seq_cmp(const void *a, const void *b) {
    const SeqRow *x = a, *y = b;
    return x->seq != y->seq ? x->seq - y->seq : x->row - y->row;
}
static int run_sequence(MsiSess *s, const char *table, int ui) {
    int n = msidb_rows(s->db, table);
    SeqRow *rows = malloc(sizeof(SeqRow) * ((size_t)n + 1));
    int k = 0;
    for (int i = 0; i < n; i++) {
        int sq = msidb_int(s->db, table, i, "Sequence");
        if (sq == MSI_NULL_INT || sq <= 0) continue;
        rows[k].seq = sq; rows[k].row = i; k++;
    }
    qsort(rows, (size_t)k, sizeof(SeqRow), seq_cmp);
    int rc = 0;
    for (int i = 0; i < k && !rc; i++) {
        const char *name = Sx(s, table, rows[i].row, "Action");
        const char *cond = Sx(s, table, rows[i].row, "Condition");
        if (!cond_ok(s, cond)) continue;
        if (!strcmp(name, "ExecuteAction")) continue;
        int r = run_action(s, name, ui);
        if (r == 259) break;
        rc = r;
    }
    free(rows);
    return rc;
}

int msis_do_action(MsiSess *s, const char *action) {
    int cr = msidb_find(s->db, "CustomAction", "Action", action);
    if (cr < 0) {
        int h = 0;
        int known = 0;
        // standard actions are known by run_std; the dry check below avoids running unknown ones
        static const char *const names[] = { "AppSearch", "LaunchConditions", "FindRelatedProducts", "RemoveExistingProducts", "CostInitialize", "FileCost", "CostFinalize",
            "InstallValidate", "InstallInitialize", "ProcessComponents", "RemoveRegistryValues", "RemoveShortcuts", "RemoveFiles", "RemoveFolders", "CreateFolders",
            "InstallFiles", "MoveFiles", "DuplicateFiles", "WriteRegistryValues", "WriteIniValues", "RemoveIniValues", "CreateShortcuts", "RegisterProduct",
            "PublishFeatures", "PublishProduct", "InstallFinalize", "ValidateProductID", "WriteEnvironmentStrings", "RemoveEnvironmentStrings", "UnpublishFeatures", NULL };
        for (int i = 0; names[i]; i++) if (!strcmp(names[i], action)) known = 1;
        if (!known) return 1626;
        (void)h;
    }
    s->in_ui_seq = 0;
    int r = run_action(s, action, 0);
    return r == 259 ? 0 : r;
}

// ---- command line properties ------------------------------------------------------------------------------------------------------------------
static void apply_cmdline(MsiSess *s, const char *cmd, int apply) {
    const char *p = cmd;
    while (p && *p) {
        while (*p == ' ' || *p == '\t') p++;
        if (!*p) break;
        const char *b = p;
        while (*p && *p != '=' && *p != ' ') p++;
        if (*p != '=') { while (*p && *p != ' ') p++; continue; }
        char *name = strndup(b, (size_t)(p - b));
        p++;
        Buf v;
        buf_init(&v);
        if (*p == '"') {
            p++;
            while (*p) {
                if (*p == '"') { if (p[1] == '"') { buf_add(&v, "\"", 1); p += 2; continue; } p++; break; }
                buf_add(&v, p, 1);
                p++;
            }
        } else {
            while (*p && *p != ' ') {
                if (*p == '"') { p++; while (*p && *p != '"') { buf_add(&v, p, 1); p++; } if (*p) p++; continue; }
                buf_add(&v, p, 1);
                p++;
            }
        }
        if (apply) { msis_setprop(s, name, v.p); S_log("özellik: %s=%s", name, v.p); }
        free(name);
        free(v.p);
    }
}
static int cmdline_has(const char *cmd, const char *name) {
    size_t n = strlen(name);
    for (const char *p = cmd; p && *p; p++) {
        if ((p == cmd || p[-1] == ' ') && !strncmp(p, name, n) && p[n] == '=') return 1;
    }
    return 0;
}

int msis_run(MsiSess *s, int action, const char *cmdline) {
    int own = 0;
    if (!s->hnd) { s->hnd = msih_new_session(s); own = 1; }
    if (!s->st) { s->st = hle_state_of(); s->owner = g_proc; }
    cmdline = cmdline ? cmdline : "";
    ensure_helper();
    load_installed_state(s);
    int maint = action != 0 || cmdline_has(cmdline, "REMOVE") || cmdline_has(cmdline, "REINSTALL");
    if (s->installed && maint) {
        int nd = msidb_rows(s->db, "Directory");
        for (int i = 0; i < nd; i++) {
            const char *id = Sx(s, "Directory", i, "Directory");
            char *v = state_get(s->code, "Dirs", id);
            if (v) { msis_setprop(s, id, v); free(v); }
        }
    }
    if (s->installed) msis_setprop(s, "Installed", "1");
    apply_cmdline(s, cmdline, 1);
    if (action == 1) msis_setprop(s, "REMOVE", "ALL");
    if (action == 2) { msis_setprop(s, "REINSTALL", "ALL"); char *rm = msis_prop(s, "REINSTALLMODE"); if (!rm) msis_setprop(s, "REINSTALLMODE", "omus"); free(rm); }
    char *rem = msis_prop(s, "REMOVE");
    s->remove_all = rem && !strcasecmp(rem, "ALL");
    free(rem);
    setprop_int(s, "UILevel", g_ui.level);
    s->mode = action;
    s->costed = 0;
    s->committed = 0;
    s->cancelled = 0;
    S_log("%s: %s (%s)", s->remove_all ? "kaldırma" : "kurulum", s->pkg_win, s->code);
    ui_msg(s, IM_INITIALIZE, "");
    char *pn = msis_prop(s, "ProductName");
    char cd[400];
    snprintf(cd, sizeof cd, "1: 0 2: 1055 3: 1252");
    ui_msg(s, IM_COMMONDATA, cd);
    snprintf(cd, sizeof cd, "1: 1 2: %s", pn ? pn : "");
    ui_msg(s, IM_COMMONDATA, cd);
    free(pn);
    int rc = 0;
    if ((g_ui.level & 7) != 2 && msidb_table_exists(s->db, "InstallUISequence")) rc = run_sequence(s, "InstallUISequence", 1);
    if (!rc) {
        if (!msidb_table_exists(s->db, "InstallExecuteSequence")) {
            ui_error(s, "Paket geçersiz: InstallExecuteSequence tablosu yok.");
            rc = 1620;
        } else rc = run_sequence(s, "InstallExecuteSequence", 0);
    }
    if (s->cancelled) rc = 1602;
    if (rc == 0 && !s->committed && (s->reg_product || s->remove_all)) commit_state(s);
    ui_msg(s, IM_TERMINATE, "");
    S_log("sonuç: %d", rc);
    if (own) { msih_release(s->hnd); s->hnd = 0; }
    return rc;
}

// ---- nested removal (upgrades) ----------------------------------------------------------------------------------------------------------------
int msi_remove_product(const char *code, X86State *st) {
    if (!msi_product_registered(code)) return 1605;
    char win[300], host[2600], err[300];
    msi_package_local(code, win, sizeof win);
    host_of(win, host, sizeof host);
    MsiSess *s = msis_open(host, win, err, sizeof err);
    if (!s) { S_log("eski paket açılamadı: %s", err); return 1619; }
    s->st = st;
    s->owner = g_proc;
    int r = msis_run(s, 1, "");
    msis_free(s);
    return r;
}
static int run_nested_remove(MsiSess *parent, const char *code) { return msi_remove_product(code, parent->st); }
