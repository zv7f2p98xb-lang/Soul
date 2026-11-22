// SoulCore - msi.dll: the Windows Installer API (handles, databases, views, records, sessions, install entry points).
// The installation engine itself is in msi_engine.c.
#include "msi_priv.h"
#include <sys/stat.h>

#define ERROR_MORE_DATA 234
#define ERROR_NO_MORE_ITEMS 259
#define ERROR_UNKNOWN_PRODUCT 1605
#define ERROR_UNKNOWN_PROPERTY 1608
#define ERROR_INSTALL_PACKAGE_OPEN_FAILED 1619
#define ERROR_FILE_INVALID 1006
#define ERROR_BAD_QUERY_SYNTAX 1615
#define ERROR_OPEN_FAILED 110

// ---- handles ------------------------------------------------------------------------------------------------------------------
typedef struct { MsiDb *db; int owns; int refs; char path[1100]; } DbObj;
typedef struct { uint8_t kind; int32_t i; char *s; uint8_t *data; size_t len, pos; } Field; // kind 0 null 1 int 2 string 3 stream
typedef struct { int n; Field *f; } Rec;
typedef struct { DbObj *db; MsiView *v; char *table; int ncols; } ViewObj;
typedef struct { MsiSess *s; int owns; } SessObj;
typedef struct { DbObj *db; } SumObj;
enum { MH_FREE, MH_DB, MH_VIEW, MH_REC, MH_SESS, MH_SUM };
#define MAX_MH 16384
static struct { int type; void *p; } g_mh[MAX_MH];   // shared by every guest process (host memory)

static uint32_t mh_new(int type, void *p) {
    for (uint32_t i = 1; i < MAX_MH; i++)
        if (g_mh[i].type == MH_FREE) { g_mh[i].type = type; g_mh[i].p = p; return i; }
    return 0;
}
static void *mh_get(uint32_t h, int type) { return h > 0 && h < MAX_MH && g_mh[h].type == type ? g_mh[h].p : NULL; }

static void db_unref(DbObj *d) {
    if (!d || --d->refs > 0) return;
    if (d->owns) msidb_close(d->db);
    free(d);
}
static void rec_free(Rec *r) {
    for (int i = 0; i < r->n; i++) { free(r->f[i].s); free(r->f[i].data); }
    free(r->f);
    free(r);
}
static Rec *rec_new(int nfields) {
    Rec *r = calloc(1, sizeof *r);
    r->n = nfields + 1;
    r->f = calloc((size_t)r->n, sizeof(Field));
    return r;
}
static void mh_close(uint32_t h) {
    if (h == 0 || h >= MAX_MH || g_mh[h].type == MH_FREE) return;
    void *p = g_mh[h].p;
    switch (g_mh[h].type) {
    case MH_DB: db_unref(p); break;
    case MH_VIEW: { ViewObj *v = p; msiview_close(v->v); free(v->table); db_unref(v->db); free(v); break; }
    case MH_REC: rec_free(p); break;
    case MH_SESS: { SessObj *s = p; if (s->owns) msis_free(s->s); free(s); break; }
    case MH_SUM: { SumObj *s = p; db_unref(s->db); free(s); break; }
    }
    g_mh[h].type = MH_FREE;
    g_mh[h].p = NULL;
}

uint32_t msih_new_session(MsiSess *s) {
    SessObj *o = calloc(1, sizeof *o);
    o->s = s;
    return mh_new(MH_SESS, o);
}
void msih_release(uint32_t h) { mh_close(h); }

// ---- argument helpers ------------------------------------------------------------------------------------------------------------------
static char *gstr(uint64_t addr, int wide) {
    if (!addr) return NULL;
    return wide ? w2u_dup((const uint16_t *)G2H(addr)) : strdup((const char *)G2H(addr));
}
static void put32(uint64_t a, uint32_t v) { if (a) mem_wr(a, 4, v); }

// copies a string to a caller buffer (*pcch in characters); returns a Win32 error
static uint32_t put_out(uint64_t buf, uint64_t pcch, const char *s, int wide) {
    uint16_t *w = wide ? u2w_dup(s) : NULL;
    size_t n = wide ? wstrlen(w) : strlen(s);
    uint32_t cap = pcch ? (uint32_t)mem_rd(pcch, 4) : 0;
    uint32_t rc = 0;
    if (buf) {
        if (!pcch) rc = ERROR_INVALID_PARAMETER;
        else if (cap <= n) {
            if (cap) {
                if (wide) { memcpy(G2H(buf), w, (cap - 1) * 2); ((uint16_t *)G2H(buf))[cap - 1] = 0; }
                else { memcpy(G2H(buf), s, cap - 1); ((char *)G2H(buf))[cap - 1] = 0; }
            }
            rc = ERROR_MORE_DATA;
        } else {
            if (wide) memcpy(G2H(buf), w, (n + 1) * 2); else memcpy(G2H(buf), s, n + 1);
        }
    }
    if (pcch) put32(pcch, (uint32_t)n);
    free(w);
    return rc;
}

static Rec *get_rec(uint32_t h) { return mh_get(h, MH_REC); }
static MsiSess *get_sess(uint32_t h) { SessObj *o = mh_get(h, MH_SESS); return o ? o->s : NULL; }

static int path_to_host(const char *in, char *win, size_t wn, char *host, size_t hn) {
    path_normalize_win(in, win, wn);
    return path_w2h(win, host, hn);
}

// ---- install entry points ---------------------------------------------------------------------------------------------------------------------
static uint32_t install_package(X86State *st, const char *win_in, const char *cmd, int action) {
    char win[1300], host[2600], err[300];
    path_to_host(win_in, win, sizeof win, host, sizeof host);
    struct stat sb;
    if (stat(host, &sb) != 0) return ERROR_INSTALL_PACKAGE_OPEN_FAILED;
    MsiSess *s = msis_open(host, win, err, sizeof err);
    if (!s) { soul_log("[msi] paket açılamadı: %s: %s\n", win, err); return ERROR_INSTALL_PACKAGE_OPEN_FAILED; }
    msis_set_state(s, st);
    uint32_t r = (uint32_t)msis_run(s, action, cmd);
    msis_free(s);
    return r;
}
static uint32_t install_product_code(X86State *st, const char *code, const char *cmd, int action) {
    if (!msi_product_registered(code)) return ERROR_UNKNOWN_PRODUCT;
    char win[300];
    msi_package_local(code, win, sizeof win);
    return install_package(st, win, cmd, action);
}

static void msi_InstallProduct(X86State *st, int w) {
    char *p = gstr(ARG(0), w), *c = gstr(ARG(1), w);
    uint32_t r = p ? install_package(st, p, c ? c : "", 0) : ERROR_INVALID_PARAMETER;
    free(p); free(c);
    RET(r);
}
static void msi_ConfigureProductEx(X86State *st, int w, int ex) {
    char *p = gstr(ARG(0), w), *c = ex ? gstr(ARG(3), w) : NULL;
    int level = (int)(uint32_t)ARG(1), state = (int)(uint32_t)ARG(2);
    char cmd[2200];
    snprintf(cmd, sizeof cmd, "%s", c ? c : "");
    if (level > 0 && level != 0xFFFF) { size_t n = strlen(cmd); snprintf(cmd + n, sizeof cmd - n, " INSTALLLEVEL=%d", level); }
    uint32_t r;
    if (!p) r = ERROR_INVALID_PARAMETER;
    else if (state == 2) r = install_product_code(st, p, cmd, 1);
    else if (state == 3 || state == 5 || state == 4 || state == 1) r = install_product_code(st, p, cmd, 0);
    else r = ERROR_INVALID_PARAMETER;
    free(p); free(c);
    RET(r);
}
static void msi_ReinstallProduct(X86State *st, int w) {
    char *p = gstr(ARG(0), w);
    uint32_t r = p ? install_product_code(st, p, "", 2) : ERROR_INVALID_PARAMETER;
    free(p);
    RET(r);
}

// ---- UI ---------------------------------------------------------------------------------------------------------------------------------------
HLE(msi_SetInternalUI) { int old = msi_ui_level_set((int)(uint32_t)ARG(0)); RET((uint32_t)old); }
static void msi_SetExternalUI(X86State *st, int w) {
    uint64_t prev = 0;
    msi_ui_set(ARG(0), (uint32_t)ARG(1), ARG(2), w, &prev);
    RET(prev);
}
HLE(msi_SetExternalUIRecord) { if (ARG(3)) wr_ptr(ARG(3), 0); RET(ERROR_CALL_NOT_IMPLEMENTED); }
HLE_STUB_RET(msi_EnableLogA, 0)
HLE_STUB_RET(msi_EnableLogW, 0)

// ---- products -----------------------------------------------------------------------------------------------------------------------------------
static void msi_QueryProductState(X86State *st, int w) {
    char *p = gstr(ARG(0), w);
    int r = p && msi_product_registered(p) ? 5 : -1;
    free(p);
    RET((uint32_t)r);
}
static void msi_GetProductInfo(X86State *st, int w) {
    char *p = gstr(ARG(0), w), *a = gstr(ARG(1), w);
    char out[2000];
    uint32_t r;
    if (!p || !a) r = ERROR_INVALID_PARAMETER;
    else {
        int k = msi_product_info(p, a, out, sizeof out);
        r = k == 0 ? put_out(ARG(2), ARG(3), out, w) : k == 1 ? ERROR_UNKNOWN_PROPERTY : ERROR_UNKNOWN_PRODUCT;
    }
    free(p); free(a);
    RET(r);
}
static void msi_GetProductInfoEx(X86State *st, int w) {
    char *p = gstr(ARG(0), w), *a = gstr(ARG(3), w);
    char out[2000];
    uint32_t r;
    if (!p || !a) r = ERROR_INVALID_PARAMETER;
    else {
        int k = msi_product_info(p, a, out, sizeof out);
        r = k == 0 ? put_out(ARG(4), ARG(5), out, w) : k == 1 ? ERROR_UNKNOWN_PROPERTY : ERROR_UNKNOWN_PRODUCT;
    }
    free(p); free(a);
    RET(r);
}
static void put_code(uint64_t buf, const char *code, int w) {
    if (!buf) return;
    if (w) u2w(code, -1, (uint16_t *)G2H(buf), 39);
    else snprintf((char *)G2H(buf), 39, "%s", code);
}
static void msi_EnumProducts(X86State *st, int w) {
    char code[48];
    int r = msi_enum_products((int)(uint32_t)ARG(0), code);
    if (r == 0) put_code(ARG(1), code, w);
    RET(r);
}
static void msi_EnumRelatedProducts(X86State *st, int w) {
    char *uc = gstr(ARG(0), w);
    int want = (int)(uint32_t)ARG(2), seen = 0;
    uint32_t r = ERROR_NO_MORE_ITEMS;
    if (uc) {
        char code[48];
        for (int i = 0; msi_enum_products(i, code) == 0; i++) {
            char u[64];
            if (msi_product_info(code, "UpgradeCodeInternal", u, sizeof u) == 2) continue;
            // the upgrade code is not an MSI product attribute: look it up through the record
            char key[400];
            snprintf(key, sizeof key, "Software\\Soul\\Installer\\Products\\%s", code);
            uint32_t t, sz = sizeof u - 1;
            memset(u, 0, sizeof u);
            if (reg_host_get(0x80000002u, key, "UpgradeCode", &t, u, &sz) != 0) continue;
            if (strcasecmp(u, uc)) continue;
            if (seen++ == want) { put_code(ARG(3), code, w); r = 0; break; }
        }
    }
    free(uc);
    RET(r);
}
static void msi_GetComponentPath(X86State *st, int w) {
    char *p = gstr(ARG(0), w), *c = gstr(ARG(1), w);
    char out[1300];
    int s = c ? msi_component_path(p, c, out, sizeof out) : -1;
    if (s == 3 && ARG(2) && ARG(3)) put_out(ARG(2), ARG(3), out, w);
    else if (ARG(3) && !ARG(2)) put32(ARG(3), s == 3 ? (uint32_t)strlen(out) : 0);
    free(p); free(c);
    RET((uint32_t)s);
}
static void msi_LocateComponent(X86State *st, int w) {
    char *c = gstr(ARG(0), w);
    char out[1300];
    int s = c ? msi_component_path(NULL, c, out, sizeof out) : -1;
    if (s == 3 && ARG(1) && ARG(2)) put_out(ARG(1), ARG(2), out, w);
    free(c);
    RET((uint32_t)s);
}
static void msi_QueryFeatureState(X86State *st, int w) {
    char *p = gstr(ARG(0), w), *f = gstr(ARG(1), w);
    int s = p && f ? msi_feature_installed(p, f) : -1;
    free(p); free(f);
    RET((uint32_t)s);
}
static void msi_GetFileVersion(X86State *st, int w) {
    char *p = gstr(ARG(0), w);
    char win[1300], host[2600], ver[64] = "", lang[32] = "";
    uint32_t r;
    if (!p) r = ERROR_INVALID_PARAMETER;
    else {
        path_to_host(p, win, sizeof win, host, sizeof host);
        int k = msi_pe_version(host, ver, sizeof ver, lang, sizeof lang);
        if (k == ERROR_FILE_NOT_FOUND) r = ERROR_FILE_NOT_FOUND;
        else if (k) r = ERROR_FILE_INVALID;
        else {
            r = 0;
            if (ARG(2)) { uint32_t e = put_out(ARG(1), ARG(2), ver, w); if (e) r = e; }
            if (ARG(4)) { uint32_t e = put_out(ARG(3), ARG(4), lang, w); if (e) r = e; }
        }
    }
    free(p);
    RET(r);
}

// ---- records ------------------------------------------------------------------------------------------------------------------------------------
HLE(msi_CreateRecord) {
    uint32_t n = (uint32_t)ARG(0);
    if (n > 65535) { RET(0); return; }
    RET(mh_new(MH_REC, rec_new((int)n)));
}
HLE(msi_RecordGetFieldCount) { Rec *r = get_rec((uint32_t)ARG(0)); RET(r ? (uint32_t)(r->n - 1) : 0xFFFFFFFFu); }
static void field_clear(Field *f) { free(f->s); free(f->data); memset(f, 0, sizeof *f); }
static void msi_RecordGetString(X86State *st, int w) {
    Rec *r = get_rec((uint32_t)ARG(0));
    uint32_t i = (uint32_t)ARG(1);
    if (!r || i >= (uint32_t)r->n) { RET(ERROR_INVALID_PARAMETER); return; }
    char tmp[32];
    const char *s = "";
    Field *f = &r->f[i];
    if (f->kind == 1) { snprintf(tmp, sizeof tmp, "%d", f->i); s = tmp; }
    else if (f->kind == 2) s = f->s;
    RET(put_out(ARG(2), ARG(3), s, w));
}
HLE(msi_RecordGetInteger) {
    Rec *r = get_rec((uint32_t)ARG(0));
    uint32_t i = (uint32_t)ARG(1);
    if (!r || i >= (uint32_t)r->n) { RET(0x80000000u); return; }
    Field *f = &r->f[i];
    if (f->kind == 1) RET((uint32_t)f->i);
    else if (f->kind == 2) { char *e; long v = strtol(f->s, &e, 10); RET(*f->s && !*e ? (uint32_t)v : 0x80000000u); }
    else RET(0x80000000u);
}
static void msi_RecordSetString(X86State *st, int w) {
    Rec *r = get_rec((uint32_t)ARG(0));
    uint32_t i = (uint32_t)ARG(1);
    if (!r || i >= (uint32_t)r->n) { RET(ERROR_INVALID_PARAMETER); return; }
    field_clear(&r->f[i]);
    char *s = gstr(ARG(2), w);
    if (s && *s) { r->f[i].kind = 2; r->f[i].s = s; } else free(s);
    RET(0);
}
HLE(msi_RecordSetInteger) {
    Rec *r = get_rec((uint32_t)ARG(0));
    uint32_t i = (uint32_t)ARG(1);
    if (!r || i >= (uint32_t)r->n) { RET(ERROR_INVALID_PARAMETER); return; }
    field_clear(&r->f[i]);
    int32_t v = (int32_t)(uint32_t)ARG(2);
    if (v != (int32_t)0x80000000) { r->f[i].kind = 1; r->f[i].i = v; }
    RET(0);
}
HLE(msi_RecordIsNull) {
    Rec *r = get_rec((uint32_t)ARG(0));
    uint32_t i = (uint32_t)ARG(1);
    RET(!r || i >= (uint32_t)r->n || r->f[i].kind == 0);
}
HLE(msi_RecordClearData) {
    Rec *r = get_rec((uint32_t)ARG(0));
    if (!r) { RET(ERROR_INVALID_HANDLE); return; }
    for (int i = 0; i < r->n; i++) field_clear(&r->f[i]);
    RET(0);
}
HLE(msi_RecordDataSize) {
    Rec *r = get_rec((uint32_t)ARG(0));
    uint32_t i = (uint32_t)ARG(1);
    if (!r || i >= (uint32_t)r->n) { RET(0); return; }
    Field *f = &r->f[i];
    RET(f->kind == 1 ? 4 : f->kind == 2 ? strlen(f->s) : f->kind == 3 ? f->len : 0);
}
HLE(msi_RecordReadStream) {
    Rec *r = get_rec((uint32_t)ARG(0));
    uint32_t i = (uint32_t)ARG(1);
    if (!r || i >= (uint32_t)r->n || r->f[i].kind != 3) { RET(ERROR_INVALID_PARAMETER); return; }
    Field *f = &r->f[i];
    uint64_t pcb = ARG(3);
    uint32_t want = pcb ? (uint32_t)mem_rd(pcb, 4) : 0;
    size_t left = f->len - f->pos;
    if (want > left) want = (uint32_t)left;
    if (ARG(2)) memcpy(G2H(ARG(2)), f->data + f->pos, want);
    f->pos += want;
    put32(pcb, want);
    RET(0);
}

// text of a record with its template (MsiFormatRecord); without a session only [n] fields are replaced
static char *format_rec(MsiSess *s, Rec *r) {
    const char **f = calloc((size_t)r->n + 1, sizeof(char *));
    char tmp[64][24];
    for (int i = 0; i < r->n; i++) {
        if (r->f[i].kind == 2) f[i] = r->f[i].s;
        else if (r->f[i].kind == 1 && i < 64) { snprintf(tmp[i], sizeof tmp[i], "%d", r->f[i].i); f[i] = tmp[i]; }
    }
    MsiFmtRec fr = { r->n, f };
    char *out;
    if (r->f[0].kind == 2) {
        if (s) out = msis_format(s, r->f[0].s, &fr, NULL);
        else {
            size_t cap = strlen(r->f[0].s) * 2 + 1024;
            out = malloc(cap);
            size_t o = 0;
            for (const char *p = r->f[0].s; *p && o + 64 < cap;) {
                const char *e;
                if (*p == '[' && (e = strchr(p, ']')) && strspn(p + 1, "0123456789") == (size_t)(e - p - 1) && e > p + 1) {
                    int n = atoi(p + 1);
                    if (n < r->n && f[n]) o += (size_t)snprintf(out + o, cap - o, "%s", f[n]);
                    p = e + 1;
                } else out[o++] = *p++;
            }
            out[o] = 0;
        }
    } else {
        size_t cap = 64;
        for (int i = 1; i < r->n; i++) cap += f[i] ? strlen(f[i]) + 16 : 16;
        out = malloc(cap);
        size_t o = 0;
        out[0] = 0;
        for (int i = 1; i < r->n; i++) if (f[i]) o += (size_t)snprintf(out + o, cap - o, "%s%d: %s", o ? " " : "", i, f[i]);
    }
    free(f);
    return out;
}
static void msi_FormatRecord(X86State *st, int w) {
    MsiSess *s = get_sess((uint32_t)ARG(0));
    Rec *r = get_rec((uint32_t)ARG(1));
    if (!r) { RET(ERROR_INVALID_HANDLE); return; }
    char *t = format_rec(s, r);
    uint32_t rc = put_out(ARG(2), ARG(3), t, w);
    free(t);
    RET(rc);
}

// ---- databases and views ----------------------------------------------------------------------------------------------------------------------------
static void msi_OpenDatabase(X86State *st, int w) {
    char *p = gstr(ARG(0), w);
    uint64_t mode = ARG(1);
    uint32_t r = 0, h = 0;
    if (!p) r = ERROR_INVALID_PARAMETER;
    else if (mode == 3 || mode == 4) r = ERROR_OPEN_FAILED;   // creating databases is not supported
    else {
        char win[1300], host[2600], err[300];
        path_to_host(p, win, sizeof win, host, sizeof host);
        MsiDb *db = msidb_open(host, err, sizeof err);
        if (!db) { r = ERROR_OPEN_FAILED; soul_log("[msi] veritabanı açılamadı: %s: %s\n", win, err); }
        else {
            DbObj *d = calloc(1, sizeof *d);
            d->db = db; d->owns = 1; d->refs = 1;
            snprintf(d->path, sizeof d->path, "%s", host);
            h = mh_new(MH_DB, d);
        }
    }
    free(p);
    if (!r) put32(ARG(2), h);
    RET(r);
}
static void msi_DatabaseOpenView(X86State *st, int w) {
    DbObj *d = mh_get((uint32_t)ARG(0), MH_DB);
    char *q = gstr(ARG(1), w);
    uint32_t r = 0;
    if (!d) r = ERROR_INVALID_HANDLE;
    else if (!q) r = ERROR_INVALID_PARAMETER;
    else {
        char err[300];
        MsiView *v = msiview_open(d->db, q, err, sizeof err);
        if (!v) { r = ERROR_BAD_QUERY_SYNTAX; soul_log("[msi] sorgu hatası: %s (%s)\n", q, err); }
        else {
            ViewObj *o = calloc(1, sizeof *o);
            o->db = d; d->refs++;
            o->v = v;
            o->table = strdup(msiview_table(v) ? msiview_table(v) : "");
            o->ncols = msiview_column_count(v);
            put32(ARG(2), mh_new(MH_VIEW, o));
        }
    }
    free(q);
    RET(r);
}
HLE(msi_ViewExecute) {
    ViewObj *v = mh_get((uint32_t)ARG(0), MH_VIEW);
    if (!v) { RET(ERROR_INVALID_HANDLE); return; }
    Rec *r = ARG(1) ? get_rec((uint32_t)ARG(1)) : NULL;
    const char *params[32];
    char tmp[32][24];
    int n = 0;
    if (r) for (int i = 1; i < r->n && n < 32; i++) {
        if (r->f[i].kind == 2) params[n++] = r->f[i].s;
        else { snprintf(tmp[n], sizeof tmp[n], "%d", r->f[i].i); params[n] = tmp[n]; n++; }
    }
    RET(msiview_execute(v->v, params, n) == 0 ? 0 : ERROR_BAD_QUERY_SYNTAX);
}
HLE(msi_ViewFetch) {
    ViewObj *v = mh_get((uint32_t)ARG(0), MH_VIEW);
    if (!v) { RET(ERROR_INVALID_HANDLE); return; }
    int row = msiview_fetch(v->v);
    if (row < 0) { RET(ERROR_NO_MORE_ITEMS); return; }
    MsiDb *db = v->db->db;
    Rec *r = rec_new(v->ncols);
    for (int c = 0; c < v->ncols; c++) {
        const char *cn = msiview_column_name(v->v, c);
        int ci = msidb_column_index(db, v->table, cn);
        int kind = ci >= 0 ? msidb_column_type(db, v->table, ci) : 's';
        Field *f = &r->f[c + 1];
        if (kind == 's') { const char *s = msidb_str(db, v->table, row, cn); if (s) { f->kind = 2; f->s = strdup(s); } }
        else if (kind == 'i') { int32_t x = msidb_int(db, v->table, row, cn); if (x != MSI_NULL_INT) { f->kind = 1; f->i = x; } }
        else {
            size_t len;
            uint8_t *d = msidb_cell_stream(db, v->table, row, cn, &len);
            if (d) { f->kind = 3; f->data = d; f->len = len; }
        }
    }
    put32(ARG(1), mh_new(MH_REC, r));
    RET(0);
}
HLE(msi_ViewClose) { RET(mh_get((uint32_t)ARG(0), MH_VIEW) ? 0 : ERROR_INVALID_HANDLE); }
HLE(msi_ViewGetColumnInfo) {
    ViewObj *v = mh_get((uint32_t)ARG(0), MH_VIEW);
    if (!v) { RET(ERROR_INVALID_HANDLE); return; }
    int types = (int)(uint32_t)ARG(1) == 1;
    MsiDb *db = v->db->db;
    Rec *r = rec_new(v->ncols);
    for (int c = 0; c < v->ncols; c++) {
        const char *cn = msiview_column_name(v->v, c);
        Field *f = &r->f[c + 1];
        f->kind = 2;
        if (!types) { f->s = strdup(cn); continue; }
        int ci = msidb_column_index(db, v->table, cn);
        int raw = ci >= 0 ? msidb_column_raw_type(db, v->table, ci) : 0x0900;
        int kind = ci >= 0 ? msidb_column_type(db, v->table, ci) : 's';
        char t[16];
        int nullable = (raw & 0x1000) != 0;
        if (kind == 'v') snprintf(t, sizeof t, "%c0", nullable ? 'V' : 'v');
        else if (kind == 'i') snprintf(t, sizeof t, "%c%d", nullable ? 'I' : 'i', raw & 0xFF ? raw & 0xFF : 2);
        else snprintf(t, sizeof t, "%c%d", (raw & 0x200) ? (nullable ? 'L' : 'l') : (nullable ? 'S' : 's'), raw & 0xFF);
        f->s = strdup(t);
    }
    put32(ARG(2), mh_new(MH_REC, r));
    RET(0);
}

// ---- summary information -------------------------------------------------------------------------------------------------------------------------------
static void msi_GetSummaryInformation(X86State *st, int w) {
    DbObj *d = mh_get((uint32_t)ARG(0), MH_DB);
    char *p = gstr(ARG(1), w);
    uint32_t r = 0;
    if (d) d->refs++;
    else if (p) {
        char win[1300], host[2600], err[300];
        path_to_host(p, win, sizeof win, host, sizeof host);
        MsiDb *db = msidb_open(host, err, sizeof err);
        if (!db) r = ERROR_OPEN_FAILED;
        else { d = calloc(1, sizeof *d); d->db = db; d->owns = 1; d->refs = 1; }
    } else r = ERROR_INVALID_PARAMETER;
    if (!r) {
        SumObj *s = calloc(1, sizeof *s);
        s->db = d;
        put32(ARG(3), mh_new(MH_SUM, s));
    }
    free(p);
    RET(r);
}
static void msi_SummaryInfoGetProperty(X86State *st, int w) {
    SumObj *s = mh_get((uint32_t)ARG(0), MH_SUM);
    if (!s) { RET(ERROR_INVALID_HANDLE); return; }
    int pid = (int)(uint32_t)ARG(1);
    MsiDb *db = s->db->db;
    uint32_t type = 0, rc = 0;
    int32_t iv = 0;
    const char *sv = NULL;
    if (pid == 1) { iv = msidb_summary_int(db, 1); type = iv == MSI_NULL_INT ? 0 : 2; }
    else if (pid == 14 || pid == 15 || pid == 16 || pid == 19) { iv = msidb_summary_int(db, pid); type = iv == MSI_NULL_INT ? 0 : 3; }
    else if ((pid >= 2 && pid <= 9) || pid == 18) { sv = msidb_summary_str(db, pid); type = sv ? 30 : 0; }
    else if (pid < 1 || pid > 19) { RET(ERROR_INVALID_PARAMETER); return; }
    put32(ARG(2), type);
    if (type == 2 || type == 3) put32(ARG(3), (uint32_t)iv);
    if (type == 30) rc = put_out(ARG(5), ARG(6), sv, w);
    RET(rc);
}
HLE(msi_SummaryInfoGetPropertyCount) {
    SumObj *s = mh_get((uint32_t)ARG(0), MH_SUM);
    if (!s) { RET(ERROR_INVALID_HANDLE); return; }
    MsiDb *db = s->db->db;
    uint32_t n = 0;
    static const int ints[] = { 1, 14, 15, 16, 19 };
    for (int i = 2; i <= 9; i++) if (msidb_summary_str(db, i)) n++;
    if (msidb_summary_str(db, 18)) n++;
    for (size_t i = 0; i < sizeof ints / sizeof ints[0]; i++) if (msidb_summary_int(db, ints[i]) != MSI_NULL_INT) n++;
    put32(ARG(1), n);
    RET(0);
}

// ---- packages (sessions) ---------------------------------------------------------------------------------------------------------------------------------------
static void msi_OpenPackageX(X86State *st, int w, int ex) {
    char *p = gstr(ARG(0), w);
    uint64_t out = ex ? ARG(2) : ARG(1);
    uint32_t r = 0;
    if (!p) r = ERROR_INVALID_PARAMETER;
    else {
        char win[1300], host[2600], err[300];
        if (p[0] == '#') {
            DbObj *d = mh_get((uint32_t)strtoul(p + 1, NULL, 10), MH_DB);
            if (d) snprintf(host, sizeof host, "%s", d->path), snprintf(win, sizeof win, "%s", d->path);
            else r = ERROR_INVALID_HANDLE;
        } else path_to_host(p, win, sizeof win, host, sizeof host);
        if (!r) {
            MsiSess *s = msis_open(host, win, err, sizeof err);
            if (!s) r = ERROR_INSTALL_PACKAGE_OPEN_FAILED;
            else {
                msis_set_state(s, st);
                SessObj *o = calloc(1, sizeof *o);
                o->s = s; o->owns = 1;
                uint32_t h = mh_new(MH_SESS, o);
                msis_set_handle(s, h);
                put32(out, h);
            }
        }
    }
    free(p);
    RET(r);
}
static void msi_GetProperty(X86State *st, int w) {
    MsiSess *s = get_sess((uint32_t)ARG(0));
    char *n = gstr(ARG(1), w);
    if (!s) { free(n); RET(ERROR_INVALID_HANDLE); return; }
    if (!n) { RET(ERROR_INVALID_PARAMETER); return; }
    char *v = msis_prop(s, n);
    uint32_t r = put_out(ARG(2), ARG(3), v ? v : "", w);
    free(v); free(n);
    RET(r);
}
static void msi_SetProperty(X86State *st, int w) {
    MsiSess *s = get_sess((uint32_t)ARG(0));
    char *n = gstr(ARG(1), w), *v = gstr(ARG(2), w);
    uint32_t r = 0;
    if (!s) r = ERROR_INVALID_HANDLE;
    else if (!n || !*n) r = ERROR_INVALID_PARAMETER;
    else msis_setprop(s, n, v ? v : "");
    free(n); free(v);
    RET(r);
}
static void msi_GetTargetPath(X86State *st, int w) {
    MsiSess *s = get_sess((uint32_t)ARG(0));
    char *d = gstr(ARG(1), w);
    uint32_t r;
    if (!s) r = ERROR_INVALID_HANDLE;
    else {
        char *p = d ? msis_dir_path(s, d) : NULL;
        r = p ? put_out(ARG(2), ARG(3), p, w) : 267;
        free(p);
    }
    free(d);
    RET(r);
}
static void msi_GetSourcePath(X86State *st, int w) {
    MsiSess *s = get_sess((uint32_t)ARG(0));
    char *d = gstr(ARG(1), w);
    uint32_t r;
    if (!s) r = ERROR_INVALID_HANDLE;
    else {
        char *p = d ? msis_src_path(s, d) : NULL;
        r = p ? put_out(ARG(2), ARG(3), p, w) : 267;
        free(p);
    }
    free(d);
    RET(r);
}
static void msi_SetTargetPath(X86State *st, int w) {
    MsiSess *s = get_sess((uint32_t)ARG(0));
    char *d = gstr(ARG(1), w), *p = gstr(ARG(2), w);
    uint32_t r;
    if (!s) r = ERROR_INVALID_HANDLE;
    else if (!d || !p) r = ERROR_INVALID_PARAMETER;
    else r = (uint32_t)msis_set_target(s, d, p);
    free(d); free(p);
    RET(r);
}
HLE(msi_GetMode) {
    MsiSess *s = get_sess((uint32_t)ARG(0));
    uint32_t mode = (uint32_t)ARG(1);
    RET(s && mode == 16 && msis_in_deferred(s) ? 1 : 0);
}
HLE(msi_GetLanguage) {
    MsiSess *s = get_sess((uint32_t)ARG(0));
    char *l = s ? msis_prop(s, "ProductLanguage") : NULL;
    RET(l ? (uint32_t)atoi(l) : 0);
    free(l);
}
static void msi_EvaluateCondition(X86State *st, int w) {
    MsiSess *s = get_sess((uint32_t)ARG(0));
    char *c = gstr(ARG(1), w);
    uint32_t r;
    if (!s) r = 3;
    else if (!c || !*c) r = 2;
    else { int e = msis_eval(s, c); r = e < 0 ? 3 : (uint32_t)e; }
    free(c);
    RET(r);
}
static void msi_DoAction(X86State *st, int w) {
    MsiSess *s = get_sess((uint32_t)ARG(0));
    char *a = gstr(ARG(1), w);
    uint32_t r;
    if (!s) r = ERROR_INVALID_HANDLE;
    else if (!a) r = ERROR_INVALID_PARAMETER;
    else { msis_set_state(s, st); r = (uint32_t)msis_do_action(s, a); }
    free(a);
    RET(r);
}
HLE(msi_ProcessMessage) {
    MsiSess *s = get_sess((uint32_t)ARG(0));
    Rec *r = get_rec((uint32_t)ARG(2));
    if (!s || !r) { RET(0xFFFFFFFFu); return; }
    char *t = format_rec(s, r);
    int a = msis_ui_message(s, (uint32_t)ARG(1), t);
    free(t);
    RET((uint32_t)a);
}
HLE(msi_GetActiveDatabase) {
    MsiSess *s = get_sess((uint32_t)ARG(0));
    if (!s) { RET(0); return; }
    DbObj *d = calloc(1, sizeof *d);
    d->db = msis_db(s); d->owns = 0; d->refs = 1;
    RET(mh_new(MH_DB, d));
}
static void msi_GetFeatureState(X86State *st, int w) {
    MsiSess *s = get_sess((uint32_t)ARG(0));
    char *f = gstr(ARG(1), w);
    uint32_t r;
    int inst = -1, act = -1;
    if (!s) r = ERROR_INVALID_HANDLE;
    else { r = f ? (uint32_t)msis_feature_state(s, f, &inst, &act) : ERROR_INVALID_PARAMETER; }
    if (!r) { put32(ARG(2), (uint32_t)inst); put32(ARG(3), (uint32_t)act); }
    free(f);
    RET(r);
}
static void msi_SetFeatureState(X86State *st, int w) {
    MsiSess *s = get_sess((uint32_t)ARG(0));
    char *f = gstr(ARG(1), w);
    uint32_t r = !s ? ERROR_INVALID_HANDLE : f ? (uint32_t)msis_set_feature(s, f, (int)(uint32_t)ARG(2)) : ERROR_INVALID_PARAMETER;
    free(f);
    RET(r);
}
static void msi_GetComponentState(X86State *st, int w) {
    MsiSess *s = get_sess((uint32_t)ARG(0));
    char *f = gstr(ARG(1), w);
    uint32_t r;
    int inst = -1, act = -1;
    if (!s) r = ERROR_INVALID_HANDLE;
    else r = f ? (uint32_t)msis_component_state(s, f, &inst, &act) : ERROR_INVALID_PARAMETER;
    if (!r) { put32(ARG(2), (uint32_t)inst); put32(ARG(3), (uint32_t)act); }
    free(f);
    RET(r);
}
static void msi_SetComponentState(X86State *st, int w) {
    MsiSess *s = get_sess((uint32_t)ARG(0));
    char *f = gstr(ARG(1), w);
    uint32_t r = !s ? ERROR_INVALID_HANDLE : f ? (uint32_t)msis_set_component(s, f, (int)(uint32_t)ARG(2)) : ERROR_INVALID_PARAMETER;
    free(f);
    RET(r);
}
HLE(msi_SetInstallLevel) {
    MsiSess *s = get_sess((uint32_t)ARG(0));
    if (!s) { RET(ERROR_INVALID_HANDLE); return; }
    msis_set_level(s, (int)(uint32_t)ARG(1));
    RET(0);
}
HLE(msi_CloseHandle) {
    uint32_t h = (uint32_t)ARG(0);
    if (h == 0) { RET(0); return; }
    if (h >= MAX_MH || g_mh[h].type == MH_FREE) { RET(ERROR_INVALID_HANDLE); return; }
    mh_close(h);
    RET(0);
}
HLE(msi_CloseAllHandles) {
    uint32_t n = 0;
    for (uint32_t i = 1; i < MAX_MH; i++) if (g_mh[i].type != MH_FREE) { mh_close(i); n++; }
    RET(n);
}
HLE_STUB_RET(msi_GetLastErrorRecord, 0)

// ---- exports -----------------------------------------------------------------------------------------------------------------------------------------------------------
#define AW(n) \
    HLE(msi_##n##A) { msi_##n(st, 0); } \
    HLE(msi_##n##W) { msi_##n(st, 1); }
AW(InstallProduct)
AW(ReinstallProduct)
AW(SetExternalUI)
AW(QueryProductState)
AW(GetProductInfo)
AW(GetProductInfoEx)
AW(EnumProducts)
AW(EnumRelatedProducts)
AW(GetComponentPath)
AW(LocateComponent)
AW(QueryFeatureState)
AW(GetFileVersion)
AW(RecordGetString)
AW(RecordSetString)
AW(FormatRecord)
AW(OpenDatabase)
AW(DatabaseOpenView)
AW(GetSummaryInformation)
AW(SummaryInfoGetProperty)
AW(GetProperty)
AW(SetProperty)
AW(GetTargetPath)
AW(GetSourcePath)
AW(SetTargetPath)
AW(EvaluateCondition)
AW(DoAction)
AW(GetFeatureState)
AW(SetFeatureState)
AW(GetComponentState)
AW(SetComponentState)
HLE(msi_ConfigureProductA) { msi_ConfigureProductEx(st, 0, 0); }
HLE(msi_ConfigureProductW) { msi_ConfigureProductEx(st, 1, 0); }
HLE(msi_ConfigureProductExA) { msi_ConfigureProductEx(st, 0, 1); }
HLE(msi_ConfigureProductExW) { msi_ConfigureProductEx(st, 1, 1); }
HLE(msi_OpenPackageA) { msi_OpenPackageX(st, 0, 0); }
HLE(msi_OpenPackageW) { msi_OpenPackageX(st, 1, 0); }
HLE(msi_OpenPackageExA) { msi_OpenPackageX(st, 0, 1); }
HLE(msi_OpenPackageExW) { msi_OpenPackageX(st, 1, 1); }

#define E(n) { "Msi" #n, msi_##n, NULL }
#define EAW(n) { "Msi" #n "A", msi_##n##A, NULL }, { "Msi" #n "W", msi_##n##W, NULL }
const HleEntry hle_msi_table[] = {
    EAW(InstallProduct), EAW(ConfigureProduct), EAW(ConfigureProductEx), EAW(ReinstallProduct),
    E(SetInternalUI), EAW(SetExternalUI), E(SetExternalUIRecord), EAW(EnableLog),
    EAW(QueryProductState), EAW(GetProductInfo), EAW(GetProductInfoEx), EAW(EnumProducts), EAW(EnumRelatedProducts),
    EAW(GetComponentPath), EAW(LocateComponent), EAW(QueryFeatureState), EAW(GetFileVersion),
    EAW(OpenPackage), EAW(OpenPackageEx), EAW(OpenDatabase), EAW(DatabaseOpenView), E(ViewExecute), E(ViewFetch), E(ViewClose), E(ViewGetColumnInfo),
    E(CreateRecord), E(RecordGetFieldCount), EAW(RecordGetString), E(RecordGetInteger), EAW(RecordSetString), E(RecordSetInteger),
    E(RecordIsNull), E(RecordReadStream), E(RecordClearData), E(RecordDataSize), EAW(FormatRecord),
    EAW(GetProperty), EAW(SetProperty), EAW(GetTargetPath), EAW(SetTargetPath), EAW(GetSourcePath), E(GetMode), E(GetLanguage),
    EAW(EvaluateCondition), EAW(DoAction), E(ProcessMessage), E(GetActiveDatabase),
    EAW(GetFeatureState), EAW(SetFeatureState), EAW(GetComponentState), EAW(SetComponentState), E(SetInstallLevel),
    EAW(GetSummaryInformation), EAW(SummaryInfoGetProperty), E(SummaryInfoGetPropertyCount),
    E(CloseHandle), E(CloseAllHandles), E(GetLastErrorRecord),
    { NULL, NULL, NULL }
};
