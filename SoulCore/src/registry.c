// SoulCore - Windows registry (advapi32 Reg* / shlwapi SH*Value) kept in memory and saved to
// C:\windows\soul_registry.dat so that programs find their settings again on the next start.
#include "hle.h"
#include <ctype.h>
#include <strings.h>
#include <sys/stat.h>

#define REG_NONE 0
#define REG_SZ 1
#define REG_EXPAND_SZ 2
#define REG_BINARY 3
#define REG_DWORD 4
#define REG_MULTI_SZ 7
#define REG_QWORD 11
#define ERROR_MORE_DATA 234
#define ERROR_NO_MORE_ITEMS 259
#define ERROR_UNSUPPORTED_TYPE 1630

typedef struct RVal { char *name; uint32_t type; uint8_t *data; uint32_t size; } RVal;
typedef struct RKey {
    char *name;
    struct RKey *parent;
    struct RKey **kids;
    int nkids;
    RVal *vals;
    int nvals;
    double written;
} RKey;

#define ROOTS 6 // HKCR (an alias of HKLM\SOFTWARE\Classes), HKCU, HKLM, HKU, (unused), HKCC
static RKey *g_root[ROOTS];
#define REG_HANDLE_BASE 0x9000ull
#define MAX_RH 1024
static RKey *g_open[MAX_RH];
static int g_dirty;
static char g_file[1200];

// ---- tree ----------------------------------------------------------------------------------------
static RKey *key_new(RKey *parent, const char *name) {
    RKey *k = calloc(1, sizeof *k);
    k->name = strdup(name);
    k->parent = parent;
    k->written = soul_now();
    if (parent) {
        parent->kids = realloc(parent->kids, sizeof(RKey *) * (size_t)(parent->nkids + 1));
        parent->kids[parent->nkids++] = k;
    }
    return k;
}
static void key_free(RKey *k) {
    for (int i = 0; i < k->nkids; i++) key_free(k->kids[i]);
    for (int i = 0; i < k->nvals; i++) { free(k->vals[i].name); free(k->vals[i].data); }
    free(k->kids); free(k->vals); free(k->name);
    for (int i = 0; i < MAX_RH; i++) if (g_open[i] == k) g_open[i] = NULL;
    free(k);
}
static RKey *child(RKey *k, const char *name, size_t len, int create) {
    for (int i = 0; i < k->nkids; i++)
        if (strlen(k->kids[i]->name) == len && strncasecmp(k->kids[i]->name, name, len) == 0) return k->kids[i];
    if (!create) return NULL;
    char *n = strndup(name, len);
    RKey *c = key_new(k, n);
    free(n);
    g_dirty = 1;
    return c;
}
static RKey *walk(RKey *k, const char *path, int create) {
    if (!k) return NULL;
    while (path && *path) {
        while (*path == '\\') path++;
        if (!*path) break;
        const char *e = strchr(path, '\\');
        size_t len = e ? (size_t)(e - path) : strlen(path);
        k = child(k, path, len, create);
        if (!k) return NULL;
        path += len;
    }
    return k;
}
static RVal *val_find(RKey *k, const char *name) {
    const char *n = name ? name : "";
    for (int i = 0; i < k->nvals; i++) if (strcasecmp(k->vals[i].name, n) == 0) return &k->vals[i];
    return NULL;
}
static void val_set(RKey *k, const char *name, uint32_t type, const void *data, uint32_t size) {
    RVal *v = val_find(k, name);
    if (!v) {
        k->vals = realloc(k->vals, sizeof(RVal) * (size_t)(k->nvals + 1));
        v = &k->vals[k->nvals++];
        v->name = strdup(name ? name : "");
        v->data = NULL;
    }
    free(v->data);
    v->type = type;
    v->size = size;
    v->data = malloc(size ? size : 1);
    if (size) memcpy(v->data, data, size);
    k->written = soul_now();
    g_dirty = 1;
}
static void val_set_str(RKey *k, const char *name, const char *s) {
    uint16_t *w = u2w_dup(s);
    val_set(k, name, REG_SZ, w, (uint32_t)((wstrlen(w) + 1) * 2));
    free(w);
}
static void val_set_dword(RKey *k, const char *name, uint32_t v) { val_set(k, name, REG_DWORD, &v, 4); }

// ---- handles ---------------------------------------------------------------------------------
static RKey *key_of(uint64_t h) {
    uint32_t lo = (uint32_t)h;
    if ((h >> 32) == 0xFFFFFFFFu || (h >> 32) == 0) {
        if (lo >= 0x80000000u && lo <= 0x80000005u) return g_root[lo - 0x80000000u];
    }
    if (h >= REG_HANDLE_BASE && h < REG_HANDLE_BASE + MAX_RH * 4 && (h - REG_HANDLE_BASE) % 4 == 0) return g_open[(h - REG_HANDLE_BASE) / 4];
    return NULL;
}
static uint64_t handle_for(RKey *k) {
    for (int i = 0; i < MAX_RH; i++) if (!g_open[i]) { g_open[i] = k; return REG_HANDLE_BASE + 4ull * (uint64_t)i; }
    return 0;
}

// ---- persistence -----------------------------------------------------------------------------
static void hex_out(FILE *f, const uint8_t *p, size_t n) { for (size_t i = 0; i < n; i++) fprintf(f, "%02x", p[i]); }
static size_t hex_in(const char *s, uint8_t *out, size_t cap) {
    size_t n = 0;
    while (isxdigit((unsigned char)s[0]) && isxdigit((unsigned char)s[1]) && n < cap) {
        unsigned v;
        sscanf(s, "%2x", &v);
        out[n++] = (uint8_t)v;
        s += 2;
    }
    return n;
}
static void save_key(FILE *f, RKey *k, const char *path) {
    fprintf(f, "K ");
    hex_out(f, (const uint8_t *)path, strlen(path));
    fprintf(f, "\n");
    for (int i = 0; i < k->nvals; i++) {
        fprintf(f, "V ");
        hex_out(f, (const uint8_t *)k->vals[i].name, strlen(k->vals[i].name));
        fprintf(f, " %u ", k->vals[i].type);
        hex_out(f, k->vals[i].data, k->vals[i].size);
        fprintf(f, "\n");
    }
    for (int i = 0; i < k->nkids; i++) {
        size_t n = strlen(path) + strlen(k->kids[i]->name) + 2;
        char *p = malloc(n);
        snprintf(p, n, "%s\\%s", path, k->kids[i]->name);
        save_key(f, k->kids[i], p);
        free(p);
    }
}
void reg_save(void) {
    if (!g_dirty || !g_file[0]) return;
    char tmp[1300];
    snprintf(tmp, sizeof tmp, "%s.tmp", g_file);
    FILE *f = fopen(tmp, "w");
    if (!f) return;
    fprintf(f, "SOULREG1\n");
    static const char *names[ROOTS] = { NULL, "HKCU", "HKLM", "HKU", NULL, "HKCC" };
    for (int r = 1; r < ROOTS; r++) if (names[r] && g_root[r]) save_key(f, g_root[r], names[r]);
    fclose(f);
    rename(tmp, g_file);
    g_dirty = 0;
}
static void load(void) {
    FILE *f = fopen(g_file, "r");
    if (!f) return;
    size_t cap = 1 << 16;
    char *line = malloc(cap);
    RKey *cur = NULL;
    if (!fgets(line, (int)cap, f) || strncmp(line, "SOULREG1", 8) != 0) { free(line); fclose(f); return; }
    while (fgets(line, (int)cap, f)) {
        size_t len = strlen(line);
        while (len == cap - 1 && line[len - 1] != '\n') { // long binary values
            cap *= 2;
            line = realloc(line, cap);
            if (!fgets(line + len, (int)(cap - len), f)) break;
            len = strlen(line);
        }
        if (line[0] == 'K') {
            uint8_t path[2048];
            size_t n = hex_in(line + 2, path, sizeof path - 1);
            path[n] = 0;
            char *p = (char *)path, *bs = strchr(p, '\\');
            size_t rl = bs ? (size_t)(bs - p) : strlen(p);
            int r = !strncmp(p, "HKCU", rl) ? 1 : !strncmp(p, "HKLM", rl) ? 2 : !strncmp(p, "HKU", rl) ? 3 : !strncmp(p, "HKCC", rl) ? 5 : -1;
            cur = r >= 0 ? walk(g_root[r], bs ? bs + 1 : "", 1) : NULL;
        } else if (line[0] == 'V' && cur) {
            char *p = line + 2;
            char *sp = strchr(p, ' ');
            if (!sp) continue;
            uint8_t name[1024];
            size_t nn = hex_in(p, name, sizeof name - 1);
            name[nn] = 0;
            uint32_t type = (uint32_t)strtoul(sp + 1, &p, 10);
            while (*p == ' ') p++;
            size_t dl = strlen(p) / 2 + 1;
            uint8_t *data = malloc(dl);
            size_t n = hex_in(p, data, dl);
            val_set(cur, (const char *)name, type, data, (uint32_t)n);
            free(data);
        }
    }
    free(line);
    fclose(f);
}

void reg_init(SoulProcess *p) {
    reg_save();
    for (int i = 0; i < ROOTS; i++) if (g_root[i] && i != 0) { key_free(g_root[i]); }
    memset(g_root, 0, sizeof g_root);
    memset(g_open, 0, sizeof g_open);
    g_root[1] = key_new(NULL, "HKEY_CURRENT_USER");
    g_root[2] = key_new(NULL, "HKEY_LOCAL_MACHINE");
    g_root[3] = key_new(NULL, "HKEY_USERS");
    g_root[5] = key_new(NULL, "HKEY_CURRENT_CONFIG");
    g_root[0] = walk(g_root[2], "SOFTWARE\\Classes", 1);
    snprintf(g_file, sizeof g_file, "%s/windows/soul_registry.dat", p->drive_c);
    load();
    // what programs commonly look up
    RKey *cv = walk(g_root[2], "SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion", 1);
    val_set_str(cv, "ProductName", "Windows 10 Pro");
    val_set_str(cv, "CurrentVersion", "6.3");
    val_set_str(cv, "CurrentBuild", "19045");
    val_set_str(cv, "CurrentBuildNumber", "19045");
    val_set_str(cv, "EditionID", "Professional");
    val_set_str(cv, "DisplayVersion", "22H2");
    val_set_dword(cv, "CurrentMajorVersionNumber", 10);
    val_set_dword(cv, "CurrentMinorVersionNumber", 0);
    val_set_str(cv, "SystemRoot", "C:\\windows");
    RKey *wcv = walk(g_root[2], "SOFTWARE\\Microsoft\\Windows\\CurrentVersion", 1);
    val_set_str(wcv, "ProgramFilesDir", "C:\\Program Files");
    val_set_str(wcv, "CommonFilesDir", "C:\\Program Files\\Common Files");
    RKey *cpu = walk(g_root[2], "HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0", 1);
    val_set_str(cpu, "ProcessorNameString", "Soul x86-64 on Apple Silicon");
    val_set_str(cpu, "Identifier", "Intel64 Family 6 Model 158 Stepping 10");
    val_set_str(cpu, "VendorIdentifier", "GenuineIntel");
    val_set_dword(cpu, "~MHz", 3200);
    RKey *intl = walk(g_root[1], "Control Panel\\International", 1);
    val_set_str(intl, "LocaleName", "tr-TR");
    val_set_str(intl, "sDecimal", ",");
    val_set_str(intl, "sThousand", ".");
    RKey *desk = walk(g_root[1], "Control Panel\\Desktop", 1);
    val_set_str(desk, "LogPixels", "96");
    RKey *shf = walk(g_root[1], "Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Shell Folders", 1);
    val_set_str(shf, "Personal", "C:\\users\\soul\\Documents");
    val_set_str(shf, "Desktop", "C:\\users\\soul\\Desktop");
    val_set_str(shf, "AppData", "C:\\users\\soul\\AppData\\Roaming");
    val_set_str(shf, "Local AppData", "C:\\users\\soul\\AppData\\Local");
    g_dirty = 1;
}
void reg_shutdown(void) { reg_save(); }

// ---- strings between the ANSI (UTF-8) and wide APIs ------------------------------------------
static char *astr(uint64_t p, int wide) {
    if (!p) return NULL;
    return wide ? w2u_dup((const uint16_t *)G2H(p)) : strdup((const char *)G2H(p));
}
static int is_str(uint32_t t) { return t == REG_SZ || t == REG_EXPAND_SZ || t == REG_MULTI_SZ; }

// Converts the data of an ANSI call (UTF-8 strings) to the stored form (UTF-16) and back.
static uint8_t *to_stored(uint32_t type, const uint8_t *data, uint32_t size, uint32_t *out_size) {
    if (!is_str(type) || !data) { uint8_t *c = malloc(size ? size : 1); if (size && data) memcpy(c, data, size); *out_size = data ? size : 0; return c; }
    uint16_t *w = malloc(((size_t)size + 2) * 2);
    size_t n = u2w((const char *)data, (long)size, w, (size_t)size + 2);
    if (n == 0 || w[n - 1] != 0) w[n++] = 0;
    *out_size = (uint32_t)(n * 2);
    return (uint8_t *)w;
}
static uint8_t *from_stored(const RVal *v, uint32_t *out_size) {
    if (!is_str(v->type)) { uint8_t *c = malloc(v->size ? v->size : 1); memcpy(c, v->data, v->size); *out_size = v->size; return c; }
    size_t cap = (size_t)v->size * 2 + 4;
    char *s = malloc(cap);
    size_t n = w2u((const uint16_t *)(const void *)v->data, (long)(v->size / 2), s, cap);
    if (n == 0 || s[n - 1] != 0) s[n++] = 0;
    *out_size = (uint32_t)n;
    return (uint8_t *)s;
}

// Copies a value to the caller's buffers with RegQueryValueEx semantics.
static uint32_t give_value(const RVal *v, int wide, uint64_t type_out, uint64_t data, uint64_t cb_out) {
    uint32_t size;
    uint8_t *d = wide ? NULL : from_stored(v, &size);
    const uint8_t *src = wide ? v->data : d;
    if (wide) size = v->size;
    if (type_out) mem_wr(type_out, 4, v->type);
    uint32_t r = 0;
    if (cb_out) {
        uint32_t cap = (uint32_t)mem_rd(cb_out, 4);
        if (data) {
            if (cap < size) r = ERROR_MORE_DATA;
            else memcpy(G2H(data), src, size);
        }
        mem_wr(cb_out, 4, size);
    } else if (data) r = ERROR_INVALID_PARAMETER;
    free(d);
    return r;
}

// ---- API -------------------------------------------------------------------------------------
static void open_key(X86State *st, int wide, int ex) {
    RKey *k = key_of(ARG(0));
    uint64_t out = ex ? hle_arg(st, 4) : ARG(2);
    if (out) wr_ptr(out, 0);
    if (!k) { RET(ERROR_INVALID_HANDLE); return; }
    char *sub = astr(ARG(1), wide);
    RKey *r = walk(k, sub, 0);
    free(sub);
    if (!r) { RET(ERROR_FILE_NOT_FOUND); return; }
    uint64_t h = handle_for(r);
    if (out) wr_ptr(out, h);
    RET(h ? 0 : ERROR_NOT_ENOUGH_MEMORY);
}
HLE(r_RegOpenKeyExA) { open_key(st, 0, 1); }
HLE(r_RegOpenKeyExW) { open_key(st, 1, 1); }
HLE(r_RegOpenKeyA) { open_key(st, 0, 0); }
HLE(r_RegOpenKeyW) { open_key(st, 1, 0); }
static void create_key(X86State *st, int wide, int ex) {
    RKey *k = key_of(ARG(0));
    uint64_t out = ex ? hle_arg(st, 7) : ARG(2), disp = ex ? hle_arg(st, 8) : 0;
    if (out) wr_ptr(out, 0);
    if (!k) { RET(ERROR_INVALID_HANDLE); return; }
    char *sub = astr(ARG(1), wide);
    RKey *exist = walk(k, sub, 0);
    RKey *r = exist ? exist : walk(k, sub, 1);
    free(sub);
    if (disp) mem_wr(disp, 4, exist ? 2 : 1); // REG_OPENED_EXISTING_KEY / REG_CREATED_NEW_KEY
    uint64_t h = handle_for(r);
    if (out) wr_ptr(out, h);
    RET(0);
}
HLE(r_RegCreateKeyExA) { create_key(st, 0, 1); }
HLE(r_RegCreateKeyExW) { create_key(st, 1, 1); }
HLE(r_RegCreateKeyA) { create_key(st, 0, 0); }
HLE(r_RegCreateKeyW) { create_key(st, 1, 0); }
HLE(r_RegCloseKey) {
    uint64_t h = ARG(0);
    if (h >= REG_HANDLE_BASE && h < REG_HANDLE_BASE + MAX_RH * 4) g_open[(h - REG_HANDLE_BASE) / 4] = NULL;
    reg_save();
    RET(0);
}
HLE(r_RegFlushKey) { reg_save(); RET(0); }
static void query_value(X86State *st, int wide) {
    // (hkey, name, reserved, type*, data*, cbData*)
    RKey *k = key_of(ARG(0));
    if (!k) { RET(ERROR_INVALID_HANDLE); return; }
    char *name = astr(ARG(1), wide);
    RVal *v = val_find(k, name);
    free(name);
    if (!v) { RET(ERROR_FILE_NOT_FOUND); return; }
    RET(give_value(v, wide, ARG(3), hle_arg(st, 4), hle_arg(st, 5)));
}
HLE(r_RegQueryValueExA) { query_value(st, 0); }
HLE(r_RegQueryValueExW) { query_value(st, 1); }
static void set_value(X86State *st, int wide) {
    // (hkey, name, reserved, type, data, cbData)
    RKey *k = key_of(ARG(0));
    if (!k) { RET(ERROR_INVALID_HANDLE); return; }
    char *name = astr(ARG(1), wide);
    uint32_t type = (uint32_t)ARG(3), size = (uint32_t)hle_arg(st, 5);
    const uint8_t *data = (const uint8_t *)G2H(hle_arg(st, 4));
    if (wide || !is_str(type)) val_set(k, name, type, data, data ? size : 0);
    else { uint32_t n; uint8_t *c = to_stored(type, data, size, &n); val_set(k, name, type, c, n); free(c); }
    free(name);
    RET(0);
}
HLE(r_RegSetValueExA) { set_value(st, 0); }
HLE(r_RegSetValueExW) { set_value(st, 1); }
static void set_value_simple(X86State *st, int wide) {
    // RegSetValue(hkey, subkey, REG_SZ, data, cb): sets the default value of subkey
    RKey *k = key_of(ARG(0));
    if (!k) { RET(ERROR_INVALID_HANDLE); return; }
    char *sub = astr(ARG(1), wide), *s = astr(ARG(3), wide);
    RKey *r = walk(k, sub, 1);
    val_set_str(r, "", s ? s : "");
    free(sub); free(s);
    RET(0);
}
HLE(r_RegSetValueA) { set_value_simple(st, 0); }
HLE(r_RegSetValueW) { set_value_simple(st, 1); }
static void query_value_simple(X86State *st, int wide) {
    // RegQueryValue(hkey, subkey, data, cb*): default value of subkey
    RKey *k = key_of(ARG(0));
    if (!k) { RET(ERROR_INVALID_HANDLE); return; }
    char *sub = astr(ARG(1), wide);
    RKey *r = walk(k, sub, 0);
    free(sub);
    RVal *v = r ? val_find(r, "") : NULL;
    if (!r) { RET(ERROR_FILE_NOT_FOUND); return; }
    if (!v) { if (ARG(3)) { uint32_t need = wide ? 2 : 1; if (ARG(2) && mem_rd(ARG(3), 4) >= need) memset(PTR(2), 0, need); mem_wr(ARG(3), 4, need); } RET(0); return; }
    RET(give_value(v, wide, 0, ARG(2), ARG(3)));
}
HLE(r_RegQueryValueA) { query_value_simple(st, 0); }
HLE(r_RegQueryValueW) { query_value_simple(st, 1); }
static uint32_t get_value(uint64_t hkey, uint64_t subkey, uint64_t value, uint32_t flags, uint64_t type, uint64_t data, uint64_t cb, int wide) {
    RKey *k = key_of(hkey);
    if (!k) return ERROR_INVALID_HANDLE;
    char *sub = astr(subkey, wide), *name = astr(value, wide);
    RKey *r = walk(k, sub, 0);
    RVal *v = r ? val_find(r, name) : NULL;
    free(sub); free(name);
    if (!v) return ERROR_FILE_NOT_FOUND;
    uint32_t allowed = flags & 0xFFFF;
    uint32_t bit = v->type == REG_NONE ? 1 : v->type == REG_SZ ? 2 : v->type == REG_EXPAND_SZ ? 4 : v->type == REG_BINARY ? 8 : v->type == REG_DWORD ? 0x10 : v->type == REG_MULTI_SZ ? 0x20 : v->type == REG_QWORD ? 0x40 : 0;
    if (allowed && allowed != 0xFFFF && !(allowed & bit)) return ERROR_UNSUPPORTED_TYPE;
    return give_value(v, wide, type, data, cb);
}
HLE(r_RegGetValueA) { RET(get_value(ARG(0), ARG(1), ARG(2), (uint32_t)ARG(3), hle_arg(st, 4), hle_arg(st, 5), hle_arg(st, 6), 0)); }
HLE(r_RegGetValueW) { RET(get_value(ARG(0), ARG(1), ARG(2), (uint32_t)ARG(3), hle_arg(st, 4), hle_arg(st, 5), hle_arg(st, 6), 1)); }
// shlwapi: SHGetValue(hkey, subkey, value, type*, data*, cb*)
HLE(r_SHGetValueA) { RET(get_value(ARG(0), ARG(1), ARG(2), 0xFFFF, ARG(3), hle_arg(st, 4), hle_arg(st, 5), 0)); }
HLE(r_SHGetValueW) { RET(get_value(ARG(0), ARG(1), ARG(2), 0xFFFF, ARG(3), hle_arg(st, 4), hle_arg(st, 5), 1)); }
static void sh_set_value(X86State *st, int wide) {
    // SHSetValue(hkey, subkey, value, type, data, cb)
    RKey *k = key_of(ARG(0));
    if (!k) { RET(ERROR_INVALID_HANDLE); return; }
    char *sub = astr(ARG(1), wide), *name = astr(ARG(2), wide);
    RKey *r = walk(k, sub, 1);
    uint32_t type = (uint32_t)ARG(3), size = (uint32_t)hle_arg(st, 5);
    const uint8_t *data = (const uint8_t *)G2H(hle_arg(st, 4));
    if (wide || !is_str(type)) val_set(r, name, type, data, size);
    else { uint32_t n; uint8_t *c = to_stored(type, data, size, &n); val_set(r, name, type, c, n); free(c); }
    free(sub); free(name);
    reg_save();
    RET(0);
}
HLE(r_SHSetValueA) { sh_set_value(st, 0); }
HLE(r_SHSetValueW) { sh_set_value(st, 1); }
static uint32_t key_delete_value(RKey *k, const char *name) {
    RVal *v = val_find(k, name);
    if (!v) return ERROR_FILE_NOT_FOUND;
    int i = (int)(v - k->vals);
    free(v->name); free(v->data);
    memmove(k->vals + i, k->vals + i + 1, sizeof(RVal) * (size_t)(k->nvals - i - 1));
    k->nvals--;
    g_dirty = 1;
    return 0;
}
static void delete_value(X86State *st, int wide) {
    RKey *k = key_of(ARG(0));
    if (!k) { RET(ERROR_INVALID_HANDLE); return; }
    char *name = astr(ARG(1), wide);
    uint32_t r = key_delete_value(k, name);
    free(name);
    RET(r);
}
HLE(r_RegDeleteValueA) { delete_value(st, 0); }
HLE(r_RegDeleteValueW) { delete_value(st, 1); }
static uint32_t key_delete(RKey *k, const char *sub, int tree) {
    RKey *r = walk(k, sub, 0);
    int whole = !sub || !*sub;
    if (!r) return ERROR_FILE_NOT_FOUND;
    if (tree && whole) { // RegDeleteTree(hkey, NULL) deletes the contents only
        while (r->nkids) key_free(r->kids[--r->nkids]);
        for (int i = 0; i < r->nvals; i++) { free(r->vals[i].name); free(r->vals[i].data); }
        r->nvals = 0;
        g_dirty = 1;
        return 0;
    }
    if (!r->parent) return ERROR_ACCESS_DENIED;
    RKey *p = r->parent;
    for (int i = 0; i < p->nkids; i++) if (p->kids[i] == r) { memmove(p->kids + i, p->kids + i + 1, sizeof(RKey *) * (size_t)(p->nkids - i - 1)); p->nkids--; break; }
    key_free(r);
    g_dirty = 1;
    return 0;
}
static uint32_t delete_key(uint64_t hkey, uint64_t subkey, int wide, int tree) {
    RKey *k = key_of(hkey);
    if (!k) return ERROR_INVALID_HANDLE;
    char *sub = astr(subkey, wide);
    uint32_t r = key_delete(k, sub, tree);
    free(sub);
    return r;
}
HLE(r_RegDeleteKeyA) { RET(delete_key(ARG(0), ARG(1), 0, 0)); }
HLE(r_RegDeleteKeyW) { RET(delete_key(ARG(0), ARG(1), 1, 0)); }
HLE(r_RegDeleteTreeA) { RET(delete_key(ARG(0), ARG(1), 0, 1)); }
HLE(r_RegDeleteTreeW) { RET(delete_key(ARG(0), ARG(1), 1, 1)); }
HLE(r_SHDeleteKeyA) { RET(delete_key(ARG(0), ARG(1), 0, 0)); }
HLE(r_SHDeleteKeyW) { RET(delete_key(ARG(0), ARG(1), 1, 0)); }
static uint32_t put_name(const char *name, uint64_t buf, uint64_t cch_ptr, int wide) {
    if (!cch_ptr) return ERROR_INVALID_PARAMETER;
    uint32_t cap = (uint32_t)mem_rd(cch_ptr, 4);
    size_t need = wide ? (size_t)u2w(name, -1, NULL, 0) : strlen(name);
    if (!buf || need + 1 > cap) { mem_wr(cch_ptr, 4, (uint32_t)need + (buf ? 0 : 1)); return ERROR_MORE_DATA; }
    if (wide) u2w(name, -1, (uint16_t *)G2H(buf), cap);
    else { memcpy(G2H(buf), name, need); ((char *)G2H(buf))[need] = 0; }
    mem_wr(cch_ptr, 4, (uint32_t)need);
    return 0;
}
static void enum_key(X86State *st, int wide, int ex) {
    RKey *k = key_of(ARG(0));
    uint32_t idx = (uint32_t)ARG(1);
    if (!k) { RET(ERROR_INVALID_HANDLE); return; }
    if (idx >= (uint32_t)k->nkids) { RET(ERROR_NO_MORE_ITEMS); return; }
    const char *name = k->kids[idx]->name;
    if (ex) { RET(put_name(name, ARG(2), ARG(3), wide)); return; }
    // RegEnumKey(hkey, index, name, cchName)
    uint32_t cap = (uint32_t)ARG(3);
    size_t need = strlen(name);
    if (need + 1 > cap) { RET(ERROR_MORE_DATA); return; }
    if (wide) u2w(name, -1, PTR(2), cap); else { memcpy(PTR(2), name, need + 1); }
    RET(0);
}
HLE(r_RegEnumKeyExA) { enum_key(st, 0, 1); }
HLE(r_RegEnumKeyExW) { enum_key(st, 1, 1); }
HLE(r_RegEnumKeyA) { enum_key(st, 0, 0); }
HLE(r_RegEnumKeyW) { enum_key(st, 1, 0); }
static void enum_value(X86State *st, int wide) {
    // (hkey, index, name, cchName*, reserved, type*, data*, cbData*)
    RKey *k = key_of(ARG(0));
    uint32_t idx = (uint32_t)ARG(1);
    if (!k) { RET(ERROR_INVALID_HANDLE); return; }
    if (idx >= (uint32_t)k->nvals) { RET(ERROR_NO_MORE_ITEMS); return; }
    RVal *v = &k->vals[idx];
    uint32_t r = put_name(v->name, ARG(2), ARG(3), wide);
    if (r) { RET(r); return; }
    RET(give_value(v, wide, hle_arg(st, 5), hle_arg(st, 6), hle_arg(st, 7)));
}
HLE(r_RegEnumValueA) { enum_value(st, 0); }
HLE(r_RegEnumValueW) { enum_value(st, 1); }
static void query_info(X86State *st, int wide) {
    // (hkey, class, cchClass*, reserved, subkeys*, maxSubKeyLen*, maxClassLen*, values*, maxValueNameLen*, maxValueLen*, secDesc*, lastWrite*)
    RKey *k = key_of(ARG(0));
    if (!k) { RET(ERROR_INVALID_HANDLE); return; }
    (void)wide;
    uint32_t maxk = 0, maxn = 0, maxv = 0;
    for (int i = 0; i < k->nkids; i++) { uint32_t n = (uint32_t)strlen(k->kids[i]->name); if (n > maxk) maxk = n; }
    for (int i = 0; i < k->nvals; i++) {
        uint32_t n = (uint32_t)strlen(k->vals[i].name), s = k->vals[i].size;
        if (n > maxn) maxn = n;
        if (s > maxv) maxv = s;
    }
    if (ARG(1) && ARG(2)) { if (mem_rd(ARG(2), 4)) mem_wr(ARG(1), wide ? 2 : 1, 0); mem_wr(ARG(2), 4, 0); }
    uint64_t out[8] = { hle_arg(st, 4), hle_arg(st, 5), hle_arg(st, 6), hle_arg(st, 7), hle_arg(st, 8), hle_arg(st, 9), hle_arg(st, 10), hle_arg(st, 11) };
    uint32_t vals[7] = { (uint32_t)k->nkids, maxk, 0, (uint32_t)k->nvals, maxn, maxv, 0 };
    for (int i = 0; i < 7; i++) if (out[i]) mem_wr(out[i], 4, vals[i]);
    if (out[7]) mem_wr(out[7], 8, filetime_from_unix(k->written));
    RET(0);
}
HLE(r_RegQueryInfoKeyA) { query_info(st, 0); }
HLE(r_RegQueryInfoKeyW) { query_info(st, 1); }
HLE(r_RegOpenCurrentUser) { if (ARG(1)) wr_ptr(ARG(1), 0xFFFFFFFF80000001ull); RET(0); }
HLE(r_RegConnectRegistry) { if (hle_arg(st, 2)) wr_ptr(hle_arg(st, 2), ARG(1)); RET(0); }
HLE_STUB_RET(r_RegNotifyChangeKeyValue, 0)
HLE_STUB_RET(r_RegSetKeySecurity, 0)

// ---- host-side API for Soul's own C engines (GIL held) -----------------------------------------
// Strings are UTF-8; they are stored exactly like the ANSI Reg* calls do (UTF-16 inside).
int reg_host_set(uint32_t root, const char *subkey, const char *name, uint32_t type, const void *data, uint32_t size) {
    RKey *r = key_of(root);
    if (!r) return ERROR_FILE_NOT_FOUND;
    r = walk(r, subkey, 1);
    if (!r) return ERROR_FILE_NOT_FOUND;
    if (is_str(type) && type != REG_MULTI_SZ) size = data ? (uint32_t)strlen((const char *)data) + 1 : 0;
    if (is_str(type)) {
        uint32_t n;
        uint8_t *c = to_stored(type, data, size, &n);
        val_set(r, name, type, c, n);
        free(c);
    } else val_set(r, name, type, data, data ? size : 0);
    return 0;
}
int reg_host_get(uint32_t root, const char *subkey, const char *name, uint32_t *type, void *data, uint32_t *size) {
    RKey *r = key_of(root);
    r = r ? walk(r, subkey, 0) : NULL;
    RVal *v = r ? val_find(r, name) : NULL;
    if (!v) return ERROR_FILE_NOT_FOUND;
    uint32_t n;
    uint8_t *d = from_stored(v, &n);
    if (type) *type = v->type;
    int rc = 0;
    if (data && size) {
        if (*size < n) rc = ERROR_MORE_DATA;
        else memcpy(data, d, n);
    }
    if (size) *size = n;
    free(d);
    return rc;
}
int reg_host_delete_value(uint32_t root, const char *subkey, const char *name) {
    RKey *r = key_of(root);
    r = r ? walk(r, subkey, 0) : NULL;
    return r ? (int)key_delete_value(r, name) : ERROR_FILE_NOT_FOUND;
}
int reg_host_delete_tree(uint32_t root, const char *subkey) {
    RKey *r = key_of(root);
    return r ? (int)key_delete(r, subkey, 1) : ERROR_FILE_NOT_FOUND;
}
int reg_host_key_exists(uint32_t root, const char *subkey) {
    RKey *r = key_of(root);
    return r && walk(r, subkey, 0) ? 1 : 0;
}

const HleEntry hle_registry_table[] = {
    { "RegOpenKeyExA", r_RegOpenKeyExA, NULL }, { "RegOpenKeyExW", r_RegOpenKeyExW, NULL }, { "RegOpenKeyA", r_RegOpenKeyA, NULL },
    { "RegOpenKeyW", r_RegOpenKeyW, NULL }, { "RegCreateKeyExA", r_RegCreateKeyExA, NULL }, { "RegCreateKeyExW", r_RegCreateKeyExW, NULL },
    { "RegCreateKeyA", r_RegCreateKeyA, NULL }, { "RegCreateKeyW", r_RegCreateKeyW, NULL }, { "RegCloseKey", r_RegCloseKey, NULL },
    { "RegFlushKey", r_RegFlushKey, NULL }, { "RegQueryValueExA", r_RegQueryValueExA, NULL }, { "RegQueryValueExW", r_RegQueryValueExW, NULL },
    { "RegSetValueExA", r_RegSetValueExA, NULL }, { "RegSetValueExW", r_RegSetValueExW, NULL }, { "RegSetValueA", r_RegSetValueA, NULL },
    { "RegSetValueW", r_RegSetValueW, NULL }, { "RegQueryValueA", r_RegQueryValueA, NULL }, { "RegQueryValueW", r_RegQueryValueW, NULL },
    { "RegGetValueA", r_RegGetValueA, NULL }, { "RegGetValueW", r_RegGetValueW, NULL }, { "RegDeleteValueA", r_RegDeleteValueA, NULL },
    { "RegDeleteValueW", r_RegDeleteValueW, NULL }, { "RegDeleteKeyA", r_RegDeleteKeyA, NULL }, { "RegDeleteKeyW", r_RegDeleteKeyW, NULL },
    { "RegDeleteKeyExA", r_RegDeleteKeyA, NULL }, { "RegDeleteKeyExW", r_RegDeleteKeyW, NULL }, { "RegDeleteTreeA", r_RegDeleteTreeA, NULL },
    { "RegDeleteTreeW", r_RegDeleteTreeW, NULL }, { "RegEnumKeyExA", r_RegEnumKeyExA, NULL }, { "RegEnumKeyExW", r_RegEnumKeyExW, NULL },
    { "RegEnumKeyA", r_RegEnumKeyA, NULL }, { "RegEnumKeyW", r_RegEnumKeyW, NULL }, { "RegEnumValueA", r_RegEnumValueA, NULL },
    { "RegEnumValueW", r_RegEnumValueW, NULL }, { "RegQueryInfoKeyA", r_RegQueryInfoKeyA, NULL }, { "RegQueryInfoKeyW", r_RegQueryInfoKeyW, NULL },
    { "RegOpenCurrentUser", r_RegOpenCurrentUser, NULL }, { "RegConnectRegistryA", r_RegConnectRegistry, NULL },
    { "RegConnectRegistryW", r_RegConnectRegistry, NULL }, { "RegNotifyChangeKeyValue", r_RegNotifyChangeKeyValue, NULL },
    { "RegSetKeySecurity", r_RegSetKeySecurity, NULL }, { "SHGetValueA", r_SHGetValueA, NULL }, { "SHGetValueW", r_SHGetValueW, NULL },
    { "SHSetValueA", r_SHSetValueA, NULL }, { "SHSetValueW", r_SHSetValueW, NULL }, { "SHDeleteKeyA", r_SHDeleteKeyA, NULL },
    { "SHDeleteKeyW", r_SHDeleteKeyW, NULL }, { "SHRegGetValueA", r_RegGetValueA, NULL }, { "SHRegGetValueW", r_RegGetValueW, NULL },
    { NULL, NULL, NULL }
};
