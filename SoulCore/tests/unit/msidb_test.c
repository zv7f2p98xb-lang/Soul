// Unit test for src/msi_db.c and src/cab.c. Usage: msidb_test <tests/msi dir> [fuzz-rounds]
#include "msi.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int g_pass, g_fail;
static void report(const char *name, int ok) {
    printf("%s %s\n", ok ? "PASS" : "FAIL", name);
    if (ok) g_pass++; else g_fail++;
}

static const char *g_dir;

static uint8_t *slurp(const char *rel, size_t *n) {
    char p[1024];
    snprintf(p, sizeof p, "%s/%s", g_dir, rel);
    FILE *f = fopen(p, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *b = malloc((size_t)sz + 1);
    *n = fread(b, 1, (size_t)sz, f);
    b[*n] = 0;
    fclose(f);
    return b;
}

static int cmpstr(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }

// ---- IDT (msiinfo export) comparison ------------------------------------------------------------
typedef struct { char **l; int n, cap; } Lines;
static void lines_add(Lines *L, const char *s, size_t n) {
    if (L->n == L->cap) { L->cap = L->cap ? L->cap * 2 : 16; L->l = realloc(L->l, (size_t)L->cap * sizeof(char *)); }
    char *d = malloc(n + 1);
    memcpy(d, s, n);
    d[n] = 0;
    L->l[L->n++] = d;
}
static void lines_free(Lines *L) { for (int i = 0; i < L->n; i++) free(L->l[i]); free(L->l); memset(L, 0, sizeof *L); }

static void split_idt(const char *text, Lines *L) {
    const char *p = text;
    while (*p) {
        const char *e = strchr(p, '\n');
        size_t n = e ? (size_t)(e - p) : strlen(p);
        size_t m = n;
        if (m && p[m - 1] == '\r') m--;
        if (m) lines_add(L, p, m);
        p += n + (e ? 1 : 0);
    }
}

static void append(char **buf, size_t *len, size_t *cap, const char *s) {
    size_t n = strlen(s);
    if (*len + n + 1 > *cap) { *cap = (*len + n + 1) * 2; *buf = realloc(*buf, *cap); }
    memcpy(*buf + *len, s, n + 1);
    *len += n;
}

static char *typecode(MsiDb *db, const char *t, int c) {
    static char b[32];
    int raw = msidb_column_raw_type(db, t, c), k = msidb_column_type(db, t, c);
    char ch = k == 's' ? ((raw & 0x200) ? 'l' : 's') : k == 'i' ? 'i' : 'v';
    if (raw & 0x1000) ch = (char)(ch - 32);
    snprintf(b, sizeof b, "%c%d", ch, raw & 0xFF);
    return b;
}

static int check_table(MsiDb *db, const char *t, char *why, size_t wl) {
    char p[256];
    snprintf(p, sizeof p, "expected/%s.idt", t);
    size_t n;
    uint8_t *raw = slurp(p, &n);
    if (!raw) { snprintf(why, wl, "no expected file"); return 0; }
    Lines exp = {0}, got = {0};
    split_idt((char *)raw, &exp);
    free(raw);
    int ok = 1;
    int nc = msidb_column_count(db, t);
    char *hdr[3] = {0};
    size_t len[3] = {0}, cap[3] = {0};
    int keys = 0;
    for (int c = 0; c < nc; c++) {
        if (c) for (int h = 0; h < 2; h++) append(&hdr[h], &len[h], &cap[h], "\t");
        append(&hdr[0], &len[0], &cap[0], msidb_column_name(db, t, c));
        append(&hdr[1], &len[1], &cap[1], typecode(db, t, c));
    }
    append(&hdr[2], &len[2], &cap[2], t);
    for (int c = 0; c < nc; c++)
        if (msidb_column_is_key(db, t, c)) { append(&hdr[2], &len[2], &cap[2], "\t"); append(&hdr[2], &len[2], &cap[2], msidb_column_name(db, t, c)); keys++; }
    if (exp.n < 3) { snprintf(why, wl, "expected file too short"); ok = 0; }
    for (int h = 0; ok && h < 3; h++)
        if (strcmp(hdr[h] ? hdr[h] : "", exp.l[h]) != 0) { snprintf(why, wl, "header %d: got '%s' want '%s'", h, hdr[h], exp.l[h]); ok = 0; }
    for (int h = 0; h < 3; h++) free(hdr[h]);
    int rows = msidb_rows(db, t);
    if (ok && rows != exp.n - 3) { snprintf(why, wl, "row count %d want %d", rows, exp.n - 3); ok = 0; }
    if (ok) {
        for (int r = 0; r < rows; r++) {
            char *line = NULL;
            size_t ll = 0, lc = 0;
            for (int c = 0; c < nc; c++) {
                if (c) append(&line, &ll, &lc, "\t");
                const char *cn = msidb_column_name(db, t, c);
                int k = msidb_column_type(db, t, c);
                if (k == 's') { const char *s = msidb_str(db, t, r, cn); append(&line, &ll, &lc, s ? s : ""); }
                else if (k == 'i') {
                    int32_t v = msidb_int(db, t, r, cn);
                    char b[16] = "";
                    if (v != MSI_NULL_INT) snprintf(b, sizeof b, "%d", (int)v);
                    append(&line, &ll, &lc, b);
                } else {
                    size_t sl;
                    uint8_t *sd = msidb_cell_stream(db, t, r, cn, &sl);
                    append(&line, &ll, &lc, sd ? "" : "");
                    free(sd);
                }
            }
            lines_add(&got, line ? line : "", line ? strlen(line) : 0);
            free(line);
        }
        if (rows > 0) qsort(got.l, (size_t)got.n, sizeof(char *), cmpstr);
        if (rows > 0) qsort(exp.l + 3, (size_t)(exp.n - 3), sizeof(char *), cmpstr);
        for (int r = 0; r < rows && ok; r++)
            if (strcmp(got.l[r], exp.l[r + 3]) != 0) { snprintf(why, wl, "row differs: got '%s' want '%s'", got.l[r], exp.l[r + 3]); ok = 0; }
    }
    lines_free(&exp);
    lines_free(&got);
    return ok;
}

// ---- helpers --------------------------------------------------------------------------------------
typedef struct { const char *dir; int n, bad; } CabCheck;
static const char *src_for(const char *name) {
    if (!strcmp(name, "ekTxt")) return "files/data/ek.txt";
    if (!strcmp(name, "readmeTxt")) return "files/readme.txt";
    if (!strcmp(name, "blobBin")) return "files/blob.bin";
    if (!strcmp(name, "mixed.dat")) return "cabs/mixed.dat";
    if (!strcmp(name, "runs.dat")) return "cabs/runs.dat";
    return NULL;
}
static int cab_cb(void *ctx, const char *name, const uint8_t *data, size_t len, uint32_t attribs) {
    CabCheck *cc = ctx;
    const char *s = src_for(name);
    size_t n = 0;
    uint8_t *ref = s ? slurp(s, &n) : NULL;
    cc->n++;
    if (!ref || n != len || memcmp(ref, data, len) != 0) { cc->bad++; printf("  mismatch: %s (%zu vs %zu)\n", name, len, n); }
    free(ref);
    return 0;
}

static const char *query_first(MsiDb *db, const char *sql, const char *const *params, int np, const char *col, int rowidx, char *out, size_t on) {
    char err[200];
    MsiView *v = msiview_open(db, sql, err, sizeof err);
    out[0] = 0;
    if (!v) { snprintf(out, on, "ERR:%s", err); return out; }
    if (msiview_execute(v, params, np)) { msiview_close(v); snprintf(out, on, "EXECERR"); return out; }
    int r, i = 0;
    while ((r = msiview_fetch(v)) >= 0) {
        if (i++ == rowidx) {
            const char *s = msidb_str(db, msiview_table(v), r, col);
            snprintf(out, on, "%s", s ? s : "(null)");
            break;
        }
    }
    msiview_close(v);
    return out;
}

// Counts rows and concatenates the values of `col` joined by ',' into out.
static int query_all(MsiDb *db, const char *sql, const char *const *params, int np, const char *col, char *out, size_t on) {
    char err[200];
    MsiView *v = msiview_open(db, sql, err, sizeof err);
    out[0] = 0;
    if (!v) { snprintf(out, on, "ERR:%s", err); return -1; }
    if (msiview_execute(v, params, np)) { msiview_close(v); snprintf(out, on, "EXECERR"); return -1; }
    int r, cnt = 0;
    while ((r = msiview_fetch(v)) >= 0) {
        const char *s = msidb_str(db, msiview_table(v), r, col);
        if (cnt++) strncat(out, ",", on - strlen(out) - 1);
        strncat(out, s ? s : "(null)", on - strlen(out) - 1);
    }
    msiview_close(v);
    return cnt;
}

// ---- fuzzing ---------------------------------------------------------------------------------------
static uint32_t g_rng = 0x1234567;
static uint32_t rnd(void) { g_rng = g_rng * 1664525u + 1013904223u; return g_rng >> 8; }

static void mutate(uint8_t *b, size_t n, int mode) {
    int k = 1 + (int)(rnd() % 8);
    for (int i = 0; i < k; i++) {
        size_t pos;
        switch (rnd() % 3) {
        case 0: pos = rnd() % (n < 600 ? n : 600); break;
        case 1: pos = n - 1 - rnd() % (n < 30000 ? n : 30000); break;
        default: pos = rnd() % n;
        }
        (void)mode;
        b[pos] = (rnd() % 4 == 0) ? 0xFF : (uint8_t)rnd();
    }
}

static void exercise_db(MsiDb *db) {
    int nt = msidb_table_count(db);
    for (int i = 0; i < nt; i++) {
        const char *t = msidb_table_name(db, i);
        int nr = msidb_rows(db, t), nc = msidb_column_count(db, t);
        for (int r = 0; r < nr && r < 50; r++)
            for (int c = 0; c < nc; c++) {
                const char *cn = msidb_column_name(db, t, c);
                msidb_str(db, t, r, cn);
                msidb_int(db, t, r, cn);
            }
        if (nc) msidb_find(db, t, msidb_column_name(db, t, 0), "x");
    }
    for (int s = 0; s < msidb_stream_count(db); s++) {
        size_t n;
        uint8_t *d = msidb_stream(db, msidb_stream_name(db, s), &n);
        if (d && !strcmp(msidb_stream_name(db, s), "cab1.cab")) {
            char err[200];
            cab_extract(d, n, NULL, NULL, err, sizeof err);
        }
        free(d);
    }
    for (int p = 1; p < 20; p++) { msidb_summary_str(db, p); msidb_summary_int(db, p); }
    char err[200];
    MsiView *v = msiview_open(db, "SELECT * FROM `File` WHERE `FileSize` > 5 ORDER BY `File`", err, sizeof err);
    if (v) { msiview_execute(v, NULL, 0); while (msiview_fetch(v) >= 0) {} msiview_close(v); }
}

static int fuzz_msi(int rounds) {
    size_t n;
    uint8_t *orig = slurp("sample.msi", &n);
    if (!orig) return 0;
    char tmpl[512];
    const char *td = getenv("TMPDIR");
    snprintf(tmpl, sizeof tmpl, "%s/msifuzzXXXXXX", td && *td ? td : "/tmp");
    int fd = mkstemp(tmpl);
    if (fd < 0) { free(orig); return 0; }
    close(fd);
    uint8_t *w = malloc(n);
    int opened = 0;
    for (int i = 0; i < rounds; i++) {
        size_t wn = n;
        memcpy(w, orig, n);
        if (i % 5 == 4) wn = rnd() % n;           // truncation
        else mutate(w, n, 0);
        FILE *f = fopen(tmpl, "wb");
        fwrite(w, 1, wn, f);
        fclose(f);
        char err[200];
        MsiDb *db = msidb_open(tmpl, err, sizeof err);
        if (db) { opened++; exercise_db(db); msidb_close(db); }
    }
    unlink(tmpl);
    free(w);
    free(orig);
    printf("  msi fuzz: %d rounds, %d still opened\n", rounds, opened);
    return 1;
}


// ---- LZX cabinets: tests/msi/lzx/cabs/*.cab vs. tests/msi/lzx/src/* (manifest.txt: "cab file size crc32") ------------------
typedef struct { int n, bad, order_bad; char names[16][40]; } LzxCheck;
static int lzx_cb(void *ctx, const char *name, const uint8_t *data, size_t len, uint32_t attribs) {
    LzxCheck *lc = ctx;
    char rel[100];
    size_t n = 0;
    snprintf(rel, sizeof rel, "lzx/src/%s", name);
    uint8_t *ref = slurp(rel, &n);
    if (lc->n < 16) snprintf(lc->names[lc->n], 40, "%s", name);
    lc->n++;
    if (!ref || n != len || (len && memcmp(ref, data, len) != 0)) { lc->bad++; printf("  mismatch: %s (%zu vs %zu)\n", name, len, ref ? n : 0); }
    free(ref);
    return 0;
}
static int fuzz_lzx(const char *rel, int rounds) {
    size_t n;
    uint8_t *orig = slurp(rel, &n);
    if (!orig) return 0;
    uint8_t *w = malloc(n);
    int ok = 0;
    for (int i = 0; i < rounds; i++) {
        size_t wn = n;
        memcpy(w, orig, n);
        char err[200];
        switch (i % 4) {
        case 0: wn = rnd() % n; break;                                          // truncation
        case 1: {                                                               // damage inside the first block header / trees
            size_t lo = 36 + 8 * (n > 400 ? 2 : 1), span = n - lo < 700 ? n - lo : 700;
            for (int j = 0, k = 1 + (int)(rnd() % 3); j < k; j++) w[lo + rnd() % span] ^= (uint8_t)(1u << (rnd() % 8));
            break;
        }
        case 2: for (int j = 0, k = 1 + (int)(rnd() % 8); j < k; j++) w[rnd() % n] = (uint8_t)rnd(); break;
        default: w[rnd() % n] ^= (uint8_t)(1u << (rnd() % 8)); break;           // single bit flip anywhere
        }
        if (cab_extract(w, wn, NULL, NULL, err, sizeof err) == 0) ok++;
    }
    free(w);
    free(orig);
    printf("  lzx fuzz %s: %d rounds, %d still extracted\n", rel, rounds, ok);
    return 1;
}

static int fuzz_cab(const char *rel, int rounds) {
    size_t n;
    uint8_t *orig = slurp(rel, &n);
    if (!orig) return 0;
    uint8_t *w = malloc(n);
    int ok = 0;
    for (int i = 0; i < rounds; i++) {
        size_t wn = n;
        memcpy(w, orig, n);
        if (i % 5 == 4) wn = rnd() % n;
        else {
            int k = 1 + (int)(rnd() % 6);
            for (int j = 0; j < k; j++) {
                size_t pos = (rnd() % 2) ? rnd() % (n < 120 ? n : 120) : rnd() % n;
                w[pos] = (uint8_t)rnd();
            }
        }
        char err[200];
        if (cab_extract(w, wn, NULL, NULL, err, sizeof err) == 0) ok++;
    }
    free(w);
    free(orig);
    printf("  cab fuzz %s: %d rounds, %d still extracted\n", rel, rounds, ok);
    return 1;
}

int main(int argc, char **argv) {
    g_dir = argc > 1 ? argv[1] : ".";
    int rounds = argc > 2 ? atoi(argv[2]) : 600;
    char path[1024], err[256], why[512], buf[512];
    snprintf(path, sizeof path, "%s/sample.msi", g_dir);
    MsiDb *db = msidb_open(path, err, sizeof err);
    report("open sample.msi", db != NULL);
    if (!db) { printf("  %s\n", err); printf("passed=%d failed=%d\n", g_pass, g_fail); return 1; }
    report("codepage 0 or 1252", msidb_codepage(db) == 0 || msidb_codepage(db) == 1252);

    // --- every table against msiinfo export
    int nt = msidb_table_count(db), checked = 0;
    for (int i = 0; i < nt; i++) {
        const char *t = msidb_table_name(db, i);
        if (t[0] == '_') continue;
        why[0] = 0;
        char nm[300];
        snprintf(nm, sizeof nm, "table %s matches msiinfo export", t);
        int ok = check_table(db, t, why, sizeof why);
        report(nm, ok);
        if (!ok) printf("  %s\n", why);
        checked++;
    }
    report("tables found (>= 20)", checked >= 20);
    report("table_exists/missing", msidb_table_exists(db, "File") && !msidb_table_exists(db, "Nope") && msidb_rows(db, "Nope") == 0);
    report("column_index", msidb_column_index(db, "File", "FileSize") == 3 && msidb_column_index(db, "File", "Zzz") == -1);
    report("str of missing is NULL, int missing is NULL_INT", msidb_str(db, "File", 0, "Zzz") == NULL && msidb_int(db, "File", 99, "FileSize") == MSI_NULL_INT);
    int r = msidb_find(db, "File", "FileSize", "46");
    report("find integer column", r >= 0 && strcmp(msidb_str(db, "File", r, "File"), "readmeTxt") == 0);
    r = msidb_find(db, "Property", "Property", "ProductName");
    report("UTF-8 conversion (Turkish chars)", r >= 0 && strcmp(msidb_str(db, "Property", r, "Value"), "Soul \xC3\x96rnek Uygulama") == 0);
    report("find missing", msidb_find(db, "Property", "Property", "NoSuchProp") == -1);

    // --- summary information (compare with the msiinfo export of _SummaryInformation)
    {
        size_t n;
        uint8_t *raw = slurp("expected/_SummaryInformation.idt", &n);
        Lines L = {0};
        if (raw) split_idt((char *)raw, &L);
        free(raw);
        int ok = L.n > 4, checked_props = 0;
        for (int i = 3; i < L.n; i++) {
            int pid = atoi(L.l[i]);
            const char *val = strchr(L.l[i], '\t');
            if (!val) continue;
            val++;
            if (pid == 12 || pid == 13) continue;       // timestamps are not part of the API
            checked_props++;
            if (pid == 1 || pid == 14 || pid == 15 || pid == 19) {
                if (msidb_summary_int(db, pid) != atoi(val)) { ok = 0; printf("  suminfo int %d: got %d want %s\n", pid, (int)msidb_summary_int(db, pid), val); }
            } else {
                const char *s = msidb_summary_str(db, pid);
                if (!s || strcmp(s, val) != 0) { ok = 0; printf("  suminfo str %d: got '%s' want '%s'\n", pid, s ? s : "(null)", val); }
            }
        }
        lines_free(&L);
        report("summary info matches msiinfo", ok && checked_props >= 10);
        report("summary missing pid", msidb_summary_str(db, 99) == NULL && msidb_summary_int(db, 99) == MSI_NULL_INT);
        const char *s3 = msidb_summary_str(db, 3);
        report("summary UTF-8 subject", s3 && strcmp(s3, "Soul \xC3\xB6rnek paketi") == 0);
    }

    // --- SQL
    {
        const char *p1[] = {"CompMain"};
        int n = query_all(db, "SELECT * FROM `File` WHERE `Component_` = ? ORDER BY `File`", p1, 1, "File", buf, sizeof buf);
        report("sql: WHERE = ? ORDER BY", n == 2 && !strcmp(buf, "blobBin,readmeTxt"));
        n = query_all(db, "select `FileName` from `File` where `FileSize` = 19 or `FileSize` = 46 order by `FileSize`", NULL, 0, "FileName", buf, sizeof buf);
        report("sql: OR on integers, lowercase keywords", n == 2 && !strcmp(buf, "ek.txt,readme.txt"));
        n = query_all(db, "SELECT Value FROM Property WHERE Property = 'SOULPROP' AND Value IS NOT NULL", NULL, 0, "Value", buf, sizeof buf);
        report("sql: AND, IS NOT NULL, bare names", n == 1 && !strcmp(buf, "Merhaba"));
        n = query_all(db, "SELECT `File` FROM `File` WHERE `Version` IS NULL ORDER BY `File`", NULL, 0, "File", buf, sizeof buf);
        report("sql: IS NULL", n == 3 && !strcmp(buf, "blobBin,ekTxt,readmeTxt"));
        n = query_all(db, "SELECT `File` FROM `File` WHERE `Version` IS NOT NULL", NULL, 0, "File", buf, sizeof buf);
        report("sql: IS NOT NULL empty", n == 0);
        n = query_all(db, "SELECT `Name` FROM `Registry` WHERE (`Root` = 2 AND `Name` = 'Yol') OR `Name` = 'Kisayol' ORDER BY `Name`", NULL, 0, "Name", buf, sizeof buf);
        report("sql: parentheses AND/OR", n == 2 && !strcmp(buf, "Kisayol,Yol"));
        const char *p2[] = {"2", "Sayi"};
        n = query_all(db, "SELECT `Name` FROM `Registry` WHERE `Root` = ? AND `Name` <> ?", p2, 2, "Name", buf, sizeof buf);
        report("sql: two ? markers, <>, integer param", n == 1 && !strcmp(buf, "Yol"));
        n = query_all(db, "SELECT `File` FROM `File` ORDER BY `FileSize`, `File`", NULL, 0, "File", buf, sizeof buf);
        report("sql: ORDER BY integer column", n == 3 && !strcmp(buf, "ekTxt,readmeTxt,blobBin"));
        n = query_all(db, "SELECT `File` FROM `File` WHERE `Component_` = 'CompMain' AND `FileSize` = '100000'", NULL, 0, "File", buf, sizeof buf);
        report("sql: integer column vs quoted number", n == 1 && !strcmp(buf, "blobBin"));
        MsiView *v = msiview_open(db, "SELECT `File`, `FileName` FROM `File`", err, sizeof err);
        report("sql: view metadata", v && msiview_column_count(v) == 2 && !strcmp(msiview_column_name(v, 1), "FileName") && !strcmp(msiview_table(v), "File"));
        msiview_close(v);
        v = msiview_open(db, "SELECT * FROM `Registry`", err, sizeof err);
        report("sql: star selects all columns", v && msiview_column_count(v) == msidb_column_count(db, "Registry"));
        msiview_close(v);
        v = msiview_open(db, "SELECT `File` FROM `File` WHERE `Component_` = ?", err, sizeof err);
        report("sql: execute with too few params fails", v && msiview_execute(v, NULL, 0) != 0);
        msiview_close(v);
        report("sql: unknown table", msiview_open(db, "SELECT * FROM `Nope`", err, sizeof err) == NULL && err[0]);
        report("sql: unknown column", msiview_open(db, "SELECT `Zzz` FROM `File`", err, sizeof err) == NULL && err[0]);
        report("sql: syntax error", msiview_open(db, "SELECT FROM", err, sizeof err) == NULL && msiview_open(db, "SELECT * FROM `File` WHERE (`File` = 'a'", err, sizeof err) == NULL);
        query_first(db, "SELECT `Property` FROM `Property` WHERE `Value` = ?", (const char *const[]){"1.2.3"}, 1, "Property", 0, buf, sizeof buf);
        report("sql: query_first", !strcmp(buf, "ProductVersion"));
    }

    // --- streams and embedded cabinet
    {
        int found = 0;
        for (int i = 0; i < msidb_stream_count(db); i++)
            if (!strcmp(msidb_stream_name(db, i), "cab1.cab")) found = 1;
        report("stream list contains cab1.cab", found);
        const char *cabname = msidb_str(db, "Media", 0, "Cabinet");
        report("Media.Cabinet is #cab1.cab", cabname && !strcmp(cabname, "#cab1.cab"));
        size_t cl = 0;
        uint8_t *cab = msidb_stream(db, cabname ? cabname + 1 : "", &cl);
        report("embedded cabinet stream read", cab && cl > 1000);
        CabCheck cc = {g_dir, 0, 0};
        int rc = cab ? cab_extract(cab, cl, cab_cb, &cc, err, sizeof err) : -1;
        report("embedded cabinet: 3 files byte-identical", rc == 0 && cc.n == 3 && cc.bad == 0);
        if (rc) printf("  %s\n", err);
        free(cab);
        size_t sl;
        report("missing stream is NULL", msidb_stream(db, "nothing.here", &sl) == NULL);
    }
    msidb_close(db);

    // --- standalone cabinets
    {
        const char *cabs[] = {"cabs/stored.cab", "cabs/mszip.cab"};
        for (int i = 0; i < 2; i++) {
            size_t n;
            uint8_t *c = slurp(cabs[i], &n);
            CabCheck cc = {g_dir, 0, 0};
            int rc = c ? cab_extract(c, n, cab_cb, &cc, err, sizeof err) : -1;
            char nm[100];
            snprintf(nm, sizeof nm, "cab %s extracts identically", cabs[i]);
            report(nm, rc == 0 && cc.n == 2 && cc.bad == 0);
            if (rc) printf("  %s\n", err);
            if (c && i == 1) {
                // unsupported compression types give clean errors
                uint8_t *m = malloc(n);
                memcpy(m, c, n);
                size_t fo = 36 + 0;    // first CFFOLDER (no reserve fields, no prev/next)
                m[fo + 6] = 3;
                int r1 = cab_extract(m, n, NULL, NULL, err, sizeof err);
                report("cab MSZIP data declared as LZX gives a clean error", r1 == -1 && err[0]);
                m[fo + 6] = 2;
                r1 = cab_extract(m, n, NULL, NULL, err, sizeof err);
                report("cab Quantum reports unsupported", r1 == -1 && strstr(err, "Quantum") != NULL);
                free(m);
            }
            free(c);
        }
        // LZX cabinets
        {
            size_t mn;
            uint8_t *man = slurp("lzx/manifest.txt", &mn);
            report("lzx manifest present", man && mn > 50);
            int ncabs = 0, allok = 1;
            const char *list[] = {"lzx15.cab", "lzx16.cab", "lzx21.cab", "lzxlate.cab", "lzxe8.cab", "lzxtiny.cab"};
            for (int i = 0; i < 6; i++) {
                char rel[100], nm[140];
                size_t cn;
                snprintf(rel, sizeof rel, "lzx/cabs/%s", list[i]);
                uint8_t *c = slurp(rel, &cn);
                LzxCheck lc;
                memset(&lc, 0, sizeof lc);
                int rc = c ? cab_extract(c, cn, lzx_cb, &lc, err, sizeof err) : -1;
                // expected file list: manifest lines of this cabinet, in order
                int want = 0, order_ok = 1;
                for (char *p = man ? (char *)man : NULL; p && p < (char *)man + mn;) {
                    char *e = memchr(p, '\n', (char *)man + mn - p);
                    size_t ll = e ? (size_t)(e - p) : (size_t)((char *)man + mn - p);
                    char line[200];
                    snprintf(line, sizeof line, "%.*s", (int)(ll < 199 ? ll : 199), p);
                    char cabn[64], fn[64];
                    unsigned long sz;
                    if (sscanf(line, "%63s %63s %lu", cabn, fn, &sz) == 3 && !strcmp(cabn, list[i])) {
                        if (want < 16 && strcmp(lc.names[want], fn)) order_ok = 0;
                        want++;
                    }
                    p = e ? e + 1 : (char *)man + mn;
                }
                snprintf(nm, sizeof nm, "lzx %s: %d files extract byte-identically", list[i], want);
                int good = c && rc == 0 && lc.n == want && want > 0 && lc.bad == 0 && order_ok;
                report(nm, good);
                if (rc) printf("  %s\n", err);
                allok &= good;
                ncabs++;
                free(c);
            }
            report("lzx: all cabinets ok", allok && ncabs == 6);
            free(man);
            // a callback that stops early, and extraction without a callback
            size_t cn;
            uint8_t *c = slurp("lzx/cabs/lzx15.cab", &cn);
            LzxCheck lc;
            memset(&lc, 0, sizeof lc);
            report("lzx: extraction without callback", c && cab_extract(c, cn, NULL, NULL, err, sizeof err) == 0);
            free(c);
        }
        report("cab rejects garbage", cab_extract((const uint8_t *)"hello world, this is not a cabinet!!!!!!!!", 42, NULL, NULL, err, sizeof err) == -1);
    }

    // --- robustness
    {
        char nm[100];
        report("fuzz: sample.msi corruption", fuzz_msi(rounds));
        snprintf(nm, sizeof nm, "fuzz: mszip cab corruption");
        report(nm, fuzz_cab("cabs/mszip.cab", rounds));
        report("fuzz: stored cab corruption", fuzz_cab("cabs/stored.cab", rounds / 3));
        {
            const char *list[] = {"lzx/cabs/lzx15.cab", "lzx/cabs/lzx16.cab", "lzx/cabs/lzx21.cab", "lzx/cabs/lzxlate.cab", "lzx/cabs/lzxe8.cab", "lzx/cabs/lzxtiny.cab"};
            int ok = 1;
            for (int i = 0; i < 6; i++) ok &= fuzz_lzx(list[i], i == 2 ? rounds / 2 : rounds);
            report("fuzz: lzx cab corruption and truncation", ok);
        }
        report("open nonexistent file fails cleanly", msidb_open("/nonexistent/file.msi", err, sizeof err) == NULL && err[0]);
        report("open non-msi fails cleanly", msidb_open(path[0] ? "/etc/passwd" : "", err, sizeof err) == NULL && err[0]);
    }

    printf("passed=%d failed=%d\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
