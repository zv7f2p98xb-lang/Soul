// SoulCore - host-side reader for Windows Installer databases (.msi = OLE compound file) with a
// small SQL subset. Pure libc. Follows the behaviour of Wine's msi (table.c, string.c, suminfo.c).
#include "msi.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CFB_FREESECT 0xFFFFFFFFu
#define CFB_ENDOFCHAIN 0xFFFFFFFEu

static void set_err(char *err, size_t n, const char *fmt, const char *a) {
    if (err && n) snprintf(err, n, fmt, a ? a : "");
}
static uint32_t rd16(const uint8_t *p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8); }
static uint32_t rd32(const uint8_t *p) { return rd16(p) | (rd16(p + 2) << 16); }

// ---------------------------------------------------------------------------------------------
// Code pages -> UTF-8
// ---------------------------------------------------------------------------------------------
static const uint16_t cp1252_hi[32] = {
    0x20AC, 0x0081, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021, 0x02C6, 0x2030, 0x0160, 0x2039, 0x0152, 0x008D, 0x017D, 0x008F,
    0x0090, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014, 0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0x009D, 0x017E, 0x0178};

static unsigned cp_char(uint8_t b, int cp) {
    if (b < 0x80) return b;
    if (b < 0xA0) return cp1252_hi[b - 0x80];
    if (cp == 1254) {
        switch (b) {
        case 0xD0: return 0x011E;
        case 0xDD: return 0x0130;
        case 0xDE: return 0x015E;
        case 0xF0: return 0x011F;
        case 0xFD: return 0x0131;
        case 0xFE: return 0x015F;
        }
    }
    return b;
}

static size_t put_utf8(char *o, unsigned c) {
    if (c < 0x80) { o[0] = (char)c; return 1; }
    if (c < 0x800) { o[0] = (char)(0xC0 | (c >> 6)); o[1] = (char)(0x80 | (c & 0x3F)); return 2; }
    o[0] = (char)(0xE0 | (c >> 12)); o[1] = (char)(0x80 | ((c >> 6) & 0x3F)); o[2] = (char)(0x80 | (c & 0x3F));
    return 3;
}

// Converts n bytes in code page cp to a malloc'ed UTF-8 string (embedded NUL ends the string).
static char *cp_to_utf8(const uint8_t *s, size_t n, int cp) {
    size_t k = 0;
    while (k < n && s[k]) k++;
    n = k;
    char *o = malloc(n * 3 + 1);
    if (!o) return NULL;
    if (cp == 65001) { memcpy(o, s, n); o[n] = 0; return o; }
    size_t w = 0;
    for (size_t i = 0; i < n; i++) w += put_utf8(o + w, cp_char(s[i], cp));
    o[w] = 0;
    return o;
}

// True when s is well-formed UTF-8 containing at least one multi-byte sequence. Some writers (wixl)
// store UTF-8 text in the summary stream while declaring code page 1252, so summary strings are sniffed.
static int looks_utf8(const uint8_t *s, size_t n) {
    int multi = 0;
    for (size_t i = 0; i < n && s[i]; ) {
        unsigned c = s[i], k = c < 0x80 ? 0 : (c >= 0xC2 && c < 0xE0) ? 1 : (c >= 0xE0 && c < 0xF0) ? 2 : (c >= 0xF0 && c < 0xF5) ? 3 : 9;
        if (k == 9) return 0;
        for (unsigned j = 1; j <= k; j++)
            if (i + j >= n || (s[i + j] & 0xC0) != 0x80) return 0;
        if (k) multi = 1;
        i += k + 1;
    }
    return multi;
}

// ---------------------------------------------------------------------------------------------
// OLE compound file
// ---------------------------------------------------------------------------------------------
typedef struct {
    char *name;       // decoded stream name
    uint32_t start;
    uint32_t size;
    int visible;      // shown by msidb_stream_count (no table streams / property sets)
} MsiStream;

typedef struct { uint8_t *f; size_t len; uint32_t ssz, mssz, cutoff; size_t nsect;
                 uint32_t *fat; size_t nfat;
                 uint32_t *minifat; size_t nmini;
                 uint8_t *mini; size_t minilen; } Cfb;

static uint8_t *cfb_sector(const Cfb *c, uint32_t s) {
    if ((size_t)s >= c->nsect) return NULL;
    return c->f + ((size_t)s + 1) * c->ssz;
}

// Reads a FAT chain of unknown length until end-of-chain; returns malloc'ed data and its length.
static uint8_t *cfb_chain_all(const Cfb *c, uint32_t start, size_t *outlen) {
    size_t cnt = 0;
    uint32_t s = start;
    while (s != CFB_ENDOFCHAIN && s != CFB_FREESECT && cnt <= c->nsect) {
        if ((size_t)s >= c->nsect || (size_t)s >= c->nfat) return NULL;
        cnt++;
        s = c->fat[s];
    }
    if (cnt > c->nsect) return NULL;
    uint8_t *buf = malloc(cnt * c->ssz + 1);
    if (!buf) return NULL;
    s = start;
    for (size_t i = 0; i < cnt; i++) { memcpy(buf + i * c->ssz, cfb_sector(c, s), c->ssz); s = c->fat[s]; }
    *outlen = cnt * c->ssz;
    return buf;
}

static uint8_t *cfb_read_fat(const Cfb *c, uint32_t start, size_t size) {
    if (size > c->nsect * (size_t)c->ssz) return NULL;
    uint8_t *buf = malloc(size + 1);
    if (!buf) return NULL;
    size_t got = 0;
    uint32_t s = start;
    while (got < size) {
        if ((size_t)s >= c->nsect || (size_t)s >= c->nfat) { free(buf); return NULL; }
        size_t n = size - got < c->ssz ? size - got : c->ssz;
        memcpy(buf + got, cfb_sector(c, s), n);
        got += n;
        s = c->fat[s];
    }
    return buf;
}

static uint8_t *cfb_read_mini(const Cfb *c, uint32_t start, size_t size) {
    if (size > c->minilen) return NULL;
    uint8_t *buf = malloc(size + 1);
    if (!buf) return NULL;
    size_t got = 0;
    uint32_t s = start;
    while (got < size) {
        if ((size_t)s >= c->nmini || (size_t)s * c->mssz >= c->minilen) { free(buf); return NULL; }
        size_t n = size - got < c->mssz ? size - got : c->mssz;
        if (n > c->minilen - (size_t)s * c->mssz) n = c->minilen - (size_t)s * c->mssz;
        memcpy(buf + got, c->mini + (size_t)s * c->mssz, n);
        got += n;
        s = c->minifat[s];
    }
    return buf;
}

static uint8_t *cfb_read_stream(const Cfb *c, const MsiStream *st, size_t *len) {
    uint8_t *r = st->size < c->cutoff ? cfb_read_mini(c, st->start, st->size) : cfb_read_fat(c, st->start, st->size);
    if (r) *len = st->size;
    return r;
}

// ---------------------------------------------------------------------------------------------
// Database
// ---------------------------------------------------------------------------------------------
typedef struct {
    char *name;
    unsigned type;   // raw type word
    char kind;       // 's' 'i' 'v'
    int width;
    size_t prefix;   // sum of widths of the preceding columns (column-major offset factor)
} MsiCol;

typedef struct {
    char *name;
    int ncols;
    MsiCol *cols;
    uint8_t *data;
    size_t datalen;
    int nrows;
} MsiTable;

typedef struct { int pid; int vt; int32_t i; char *s; } SumProp;

struct MsiDb {
    Cfb cfb;
    MsiStream *streams; int nstreams;
    int *visible; int nvisible;
    int codepage;
    int strw;
    char **str; size_t nstr;
    MsiTable *tables; int ntables;
    SumProp *sum; int nsum;
};

static int mime2utf(unsigned x) {
    if (x < 10) return '0' + (int)x;
    if (x < 36) return 'A' + (int)x - 10;
    if (x < 62) return 'a' + (int)x - 36;
    return x == 62 ? '.' : '_';
}

static char *decode_stream_name(const uint8_t *p, size_t nunits) {
    char *o = malloc(nunits * 6 + 1);
    if (!o) return NULL;
    size_t w = 0;
    for (size_t i = 0; i < nunits; i++) {
        unsigned ch = rd16(p + i * 2);
        if (!ch) break;
        if (ch == 0x4840) { o[w++] = '!'; continue; }
        if (ch >= 0x3800 && ch < 0x4800) {
            ch -= 0x3800;
            o[w++] = (char)mime2utf(ch & 0x3F);
            o[w++] = (char)mime2utf((ch >> 6) & 0x3F);
        } else if (ch >= 0x4800 && ch < 0x4840) {
            o[w++] = (char)mime2utf(ch - 0x4800);
        } else {
            w += put_utf8(o + w, ch);
        }
    }
    o[w] = 0;
    return o;
}

static int cfb_load(MsiDb *db, uint8_t *file, size_t flen, char *err, size_t errlen) {
    Cfb *c = &db->cfb;
    static const uint8_t sig[8] = {0xD0, 0xCF, 0x11, 0xE0, 0xA1, 0xB1, 0x1A, 0xE1};
    if (flen < 512 || memcmp(file, sig, 8) != 0) { set_err(err, errlen, "not an OLE compound file (.msi)%s", ""); return -1; }
    unsigned sh = rd16(file + 0x1E), msh = rd16(file + 0x20);
    if ((sh != 9 && sh != 12) || msh < 2 || msh > sh) { set_err(err, errlen, "unsupported compound file sector size%s", ""); return -1; }
    c->ssz = 1u << sh;
    c->mssz = 1u << msh;
    c->cutoff = rd32(file + 0x38);
    size_t padded = (flen + c->ssz - 1) / c->ssz * c->ssz;
    c->f = calloc(padded + 1, 1);
    if (!c->f) { set_err(err, errlen, "out of memory%s", ""); return -1; }
    memcpy(c->f, file, flen);
    c->len = padded;
    c->nsect = padded / c->ssz - 1;
    uint32_t nfatsec = rd32(file + 0x2C), dir_start = rd32(file + 0x30), minifat_start = rd32(file + 0x3C);
    uint32_t ndifat = rd32(file + 0x48), difat_start = rd32(file + 0x44);
    if (nfatsec > c->nsect || ndifat > c->nsect) { set_err(err, errlen, "corrupt compound file header%s", ""); return -1; }
    // FAT
    uint32_t per = c->ssz / 4;
    c->fat = calloc((size_t)nfatsec * per + 1, 4);
    if (!c->fat) { set_err(err, errlen, "out of memory%s", ""); return -1; }
    c->nfat = (size_t)nfatsec * per;
    uint32_t got = 0;
    uint32_t difat_sec = difat_start;
    size_t di = 0;       // index into the current DIFAT array
    uint32_t cur_cnt = 109;
    const uint8_t *cur = file + 0x4C;
    uint32_t sectors_left = ndifat;
    while (got < nfatsec) {
        if (di >= cur_cnt) {
            if (!sectors_left || difat_sec == CFB_ENDOFCHAIN || difat_sec == CFB_FREESECT) { set_err(err, errlen, "corrupt DIFAT%s", ""); return -1; }
            cur = cfb_sector(c, difat_sec);
            if (!cur) { set_err(err, errlen, "corrupt DIFAT%s", ""); return -1; }
            cur_cnt = per - 1;
            di = 0;
            sectors_left--;
            difat_sec = rd32(cur + (size_t)cur_cnt * 4);
        }
        uint32_t s = rd32(cur + di * 4);
        di++;
        if (s == CFB_FREESECT) continue;
        uint8_t *sp = cfb_sector(c, s);
        if (!sp) { set_err(err, errlen, "corrupt FAT sector index%s", ""); return -1; }
        for (uint32_t k = 0; k < per; k++) c->fat[(size_t)got * per + k] = rd32(sp + (size_t)k * 4);
        got++;
    }
    // Directory
    size_t dlen = 0;
    uint8_t *dir = cfb_chain_all(c, dir_start, &dlen);
    if (!dir || dlen < 128) { free(dir); set_err(err, errlen, "corrupt directory%s", ""); return -1; }
    // MiniFAT and mini stream
    if (minifat_start != CFB_ENDOFCHAIN && minifat_start != CFB_FREESECT) {
        size_t ml = 0;
        uint8_t *mf = cfb_chain_all(c, minifat_start, &ml);
        if (!mf) { free(dir); set_err(err, errlen, "corrupt MiniFAT%s", ""); return -1; }
        c->nmini = ml / 4;
        c->minifat = calloc(c->nmini + 1, 4);
        if (!c->minifat) { free(mf); free(dir); set_err(err, errlen, "out of memory%s", ""); return -1; }
        for (size_t k = 0; k < c->nmini; k++) c->minifat[k] = rd32(mf + k * 4);
        free(mf);
    }
    uint32_t root_start = rd32(dir + 116), root_size = rd32(dir + 120);
    if (c->nmini && root_size) {
        c->mini = cfb_read_fat(c, root_start, root_size);
        if (!c->mini) { free(dir); set_err(err, errlen, "corrupt mini stream%s", ""); return -1; }
        c->minilen = root_size;
    }
    // Streams
    size_t nent = dlen / 128;
    db->streams = calloc(nent + 1, sizeof(MsiStream));
    if (!db->streams) { free(dir); set_err(err, errlen, "out of memory%s", ""); return -1; }
    for (size_t e = 1; e < nent; e++) {
        const uint8_t *d = dir + e * 128;
        if (d[66] != 2) continue;
        unsigned nl = rd16(d + 64);
        if (nl < 2 || nl > 64) continue;
        MsiStream *st = &db->streams[db->nstreams];
        st->name = decode_stream_name(d, nl / 2 - 1);
        if (!st->name) continue;
        st->start = rd32(d + 116);
        st->size = rd32(d + 120);
        st->visible = st->name[0] != '!' && st->name[0] != 5;
        db->nstreams++;
    }
    free(dir);
    db->visible = calloc((size_t)db->nstreams + 1, sizeof(int));
    if (!db->visible) { set_err(err, errlen, "out of memory%s", ""); return -1; }
    for (int i = 0; i < db->nstreams; i++)
        if (db->streams[i].visible) db->visible[db->nvisible++] = i;
    return 0;
}

static MsiStream *find_stream(MsiDb *db, const char *name) {
    for (int i = 0; i < db->nstreams; i++)
        if (strcmp(db->streams[i].name, name) == 0) return &db->streams[i];
    return NULL;
}

static uint8_t *read_named(MsiDb *db, const char *name, size_t *len) {
    MsiStream *st = find_stream(db, name);
    return st ? cfb_read_stream(&db->cfb, st, len) : NULL;
}

// ---------------------------------------------------------------------------------------------
// String pool, tables
// ---------------------------------------------------------------------------------------------
static int load_strings(MsiDb *db, char *err, size_t errlen) {
    size_t plen = 0, dlen = 0;
    uint8_t *pool = read_named(db, "!_StringPool", &plen);
    uint8_t *sd = read_named(db, "!_StringData", &dlen);
    if (!pool || plen < 4) { free(pool); free(sd); set_err(err, errlen, "missing _StringPool%s", ""); return -1; }
    uint32_t h = rd32(pool);
    db->codepage = (int)(h & 0xFFFF);
    db->strw = (h & 0x80000000u) ? 3 : 2;
    int cp = db->codepage;
    size_t n = plen / 4;
    db->str = calloc(n + 1, sizeof(char *));
    if (!db->str) { free(pool); free(sd); set_err(err, errlen, "out of memory%s", ""); return -1; }
    db->nstr = n;                    // ids 1 .. n-1 valid
    size_t off = 0, pp = 4, id = 1;
    while (pp + 4 <= plen) {
        uint32_t len = rd16(pool + pp), refs = rd16(pool + pp + 2);
        pp += 4;
        if (len == 0 && refs != 0) {
            if (pp + 4 > plen) break;
            len = rd32(pool + pp);
            pp += 4;
        }
        if (id >= n + 1) break;
        if (len || refs) {
            if (!sd || len > dlen || off > dlen - len) { free(pool); free(sd); set_err(err, errlen, "corrupt _StringData%s", ""); return -1; }
            db->str[id] = cp_to_utf8(sd + off, len, cp == 65001 ? 65001 : cp);
            off += len;
        }
        id++;
    }
    db->nstr = id;
    free(pool);
    free(sd);
    return 0;
}

static const char *strid(MsiDb *db, uint32_t id) {
    return id && id < db->nstr ? db->str[id] : NULL;
}

static uint32_t read_le(const uint8_t *p, int w) {
    uint32_t v = 0;
    for (int i = 0; i < w; i++) v |= (uint32_t)p[i] << (8 * i);
    return v;
}

static int ncmp_find_table(MsiDb *db, const char *name) {
    if (!name) return -1;
    for (int i = 0; i < db->ntables; i++)
        if (strcmp(db->tables[i].name, name) == 0) return i;
    return -1;
}

static MsiTable *get_table(MsiDb *db, const char *name) {
    int i = ncmp_find_table(db, name);
    return i < 0 ? NULL : &db->tables[i];
}

static int load_tables(MsiDb *db, char *err, size_t errlen) {
    size_t tlen = 0, clen = 0;
    uint8_t *tt = read_named(db, "!_Tables", &tlen);
    uint8_t *ct = read_named(db, "!_Columns", &clen);
    if (!tt) { tlen = 0; }
    if (!ct) { clen = 0; }
    int w = db->strw;
    size_t nt = tlen / w;
    db->tables = calloc(nt + 1, sizeof(MsiTable));
    if (!db->tables) { free(tt); free(ct); set_err(err, errlen, "out of memory%s", ""); return -1; }
    for (size_t i = 0; i < nt; i++) {
        const char *nm = strid(db, read_le(tt + i * w, w));
        if (!nm) continue;
        db->tables[db->ntables].name = strdup(nm);
        if (!db->tables[db->ntables].name) continue;
        db->ntables++;
    }
    // _Columns: Table(strref) Number(2) Name(strref) Type(2), column-major
    size_t rs = (size_t)w * 2 + 4, nr = clen / rs;
    const uint8_t *c_tab = ct, *c_num = ct ? ct + nr * w : NULL, *c_nam = ct ? ct + nr * (w + 2) : NULL, *c_typ = ct ? ct + nr * (2 * w + 2) : NULL;
    for (size_t r = 0; r < nr; r++) {
        const char *tn = strid(db, read_le(c_tab + r * w, w));
        MsiTable *t = get_table(db, tn);
        if (!t) continue;
        t->ncols++;     // count first
    }
    for (int i = 0; i < db->ntables; i++) {
        MsiTable *t = &db->tables[i];
        int cnt = t->ncols;
        t->ncols = 0;
        if (cnt) t->cols = calloc((size_t)cnt, sizeof(MsiCol));
        if (!t->cols) { t->ncols = 0; continue; }
        t->ncols = cnt;     // temporarily the capacity
    }
    for (size_t r = 0; r < nr; r++) {
        const char *tn = strid(db, read_le(c_tab + r * w, w));
        MsiTable *t = get_table(db, tn);
        if (!t || !t->cols) continue;
        uint32_t numraw = read_le(c_num + r * 2, 2);
        int num = (int)numraw - 0x8000;
        if (num < 1 || num > t->ncols) continue;
        MsiCol *col = &t->cols[num - 1];
        const char *cn = strid(db, read_le(c_nam + r * w, w));
        if (!cn || col->name) continue;
        col->name = strdup(cn);
        col->type = read_le(c_typ + r * 2, 2) ^ 0x8000;
    }
    free(tt);
    free(ct);
    // Finish columns; a gap in the numbering invalidates the table's columns.
    for (int i = 0; i < db->ntables; i++) {
        MsiTable *t = &db->tables[i];
        int ok = t->cols != NULL;
        for (int c = 0; ok && c < t->ncols; c++) if (!t->cols[c].name) ok = 0;
        if (!ok) {
            for (int c = 0; t->cols && c < t->ncols; c++) free(t->cols[c].name);
            free(t->cols);
            t->cols = NULL;
            t->ncols = 0;
            continue;
        }
        size_t prefix = 0;
        for (int c = 0; c < t->ncols; c++) {
            MsiCol *col = &t->cols[c];
            unsigned ty = col->type;
            if ((ty & ~0x1000u & 0xFFFFu) == (0x0800u | 0x0100u)) { col->kind = 'v'; col->width = 2; }
            else if (ty & 0x0800) { col->kind = 's'; col->width = w; }
            else {
                col->kind = 'i';
                col->width = (ty & 0xFF) == 4 ? 4 : ((ty & 0xFF) == 1 ? 1 : 2);
            }
            col->prefix = prefix;
            prefix += (size_t)col->width;
        }
        // Row data
        if (!t->ncols) continue;
        char sn[600];
        snprintf(sn, sizeof sn, "!%s", t->name);
        size_t dl = 0;
        uint8_t *d = read_named(db, sn, &dl);
        if (d) {
            t->data = d;
            t->datalen = dl;
            t->nrows = (int)(dl / prefix > 0x7FFFFFFF ? 0x7FFFFFFF : dl / prefix);
        }
    }
    return 0;
}

// Raw cell value (stored form).
static uint32_t cell_raw(const MsiTable *t, int row, int col) {
    const MsiCol *c = &t->cols[col];
    return read_le(t->data + c->prefix * (size_t)t->nrows + (size_t)row * (size_t)c->width, c->width);
}

static int32_t cell_int(const MsiTable *t, int row, int col) {
    const MsiCol *c = &t->cols[col];
    uint32_t raw = cell_raw(t, row, col);
    if (!raw) return MSI_NULL_INT;
    if (c->width == 4) return (int32_t)(raw ^ 0x80000000u);
    if (c->width == 1) return (int)raw - 0x80;
    return (int)raw - 0x8000;
}

static const char *cell_str(MsiDb *db, const MsiTable *t, int row, int col) {
    if (t->cols[col].kind != 's') return NULL;
    return strid(db, cell_raw(t, row, col));
}

// ---------------------------------------------------------------------------------------------
// Summary information (OLE property set)
// ---------------------------------------------------------------------------------------------
static void load_summary(MsiDb *db) {
    size_t len = 0;
    uint8_t *b = read_named(db, "\x05SummaryInformation", &len);
    if (!b) return;
    if (len < 48 || rd32(b + 24) < 1) { free(b); return; }
    size_t sec = rd32(b + 44);
    if (sec > len || len - sec < 8) { free(b); return; }
    uint32_t np = rd32(b + sec + 4);
    if (np > (len - sec - 8) / 8) { free(b); return; }
    db->sum = calloc((size_t)np + 1, sizeof(SumProp));
    if (!db->sum) { free(b); return; }
    int cp = 1252;
    for (int pass = 0; pass < 2; pass++) {
        for (uint32_t i = 0; i < np; i++) {
            const uint8_t *e = b + sec + 8 + (size_t)i * 8;
            uint32_t pid = rd32(e), off = rd32(e + 4);
            size_t vp = sec + off;
            if (off > len || vp > len || len - vp < 8) continue;
            int vt = (int)(rd32(b + vp) & 0xFFFF);
            if (pass == 0) {
                if (pid == 1 && vt == 2) cp = (int)rd16(b + vp + 4);
                continue;
            }
            SumProp *sp = &db->sum[db->nsum];
            sp->pid = (int)pid;
            sp->vt = vt;
            sp->i = MSI_NULL_INT;
            if (vt == 2) sp->i = pid == 1 ? (int32_t)rd16(b + vp + 4) : (int32_t)(int16_t)rd16(b + vp + 4);
            else if (vt == 3) sp->i = (int32_t)rd32(b + vp + 4);
            else if (vt == 30) {
                uint32_t sl = rd32(b + vp + 4);
                if (sl > len - vp - 8) continue;
                sp->s = cp_to_utf8(b + vp + 8, sl, looks_utf8(b + vp + 8, sl) ? 65001 : cp);
            } else continue;
            db->nsum++;
        }
    }
    free(b);
}

const char *msidb_summary_str(MsiDb *db, int pid) {
    for (int i = 0; db && i < db->nsum; i++)
        if (db->sum[i].pid == pid) return db->sum[i].s;
    return NULL;
}

int32_t msidb_summary_int(MsiDb *db, int pid) {
    for (int i = 0; db && i < db->nsum; i++)
        if (db->sum[i].pid == pid && !db->sum[i].s) return db->sum[i].i;
    return MSI_NULL_INT;
}

// ---------------------------------------------------------------------------------------------
// Public database API
// ---------------------------------------------------------------------------------------------
MsiDb *msidb_open(const char *host_path, char *err, size_t errlen) {
    if (err && errlen) err[0] = 0;
    FILE *f = host_path ? fopen(host_path, "rb") : NULL;
    if (!f) { set_err(err, errlen, "cannot open %s", host_path); return NULL; }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz < 0 || sz > 0x7FFFFFF0L) { fclose(f); set_err(err, errlen, "bad file size%s", ""); return NULL; }
    uint8_t *buf = malloc((size_t)sz + 1);
    if (!buf) { fclose(f); set_err(err, errlen, "out of memory%s", ""); return NULL; }
    size_t n = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    MsiDb *db = calloc(1, sizeof *db);
    if (!db) { free(buf); set_err(err, errlen, "out of memory%s", ""); return NULL; }
    if (cfb_load(db, buf, n, err, errlen) || load_strings(db, err, errlen) || load_tables(db, err, errlen)) {
        free(buf);
        msidb_close(db);
        return NULL;
    }
    free(buf);
    load_summary(db);
    return db;
}

void msidb_close(MsiDb *db) {
    if (!db) return;
    for (int i = 0; i < db->nstreams; i++) free(db->streams[i].name);
    free(db->streams);
    free(db->visible);
    for (size_t i = 0; i < db->nstr; i++) free(db->str[i]);
    free(db->str);
    for (int i = 0; i < db->ntables; i++) {
        MsiTable *t = &db->tables[i];
        for (int c = 0; c < t->ncols; c++) free(t->cols[c].name);
        free(t->cols);
        free(t->data);
        free(t->name);
    }
    free(db->tables);
    for (int i = 0; i < db->nsum; i++) free(db->sum[i].s);
    free(db->sum);
    free(db->cfb.f); free(db->cfb.fat); free(db->cfb.minifat); free(db->cfb.mini);
    free(db);
}

int msidb_codepage(MsiDb *db) { return db ? db->codepage : 0; }
int msidb_table_count(MsiDb *db) { return db ? db->ntables : 0; }
const char *msidb_table_name(MsiDb *db, int i) { return db && i >= 0 && i < db->ntables ? db->tables[i].name : NULL; }
int msidb_table_exists(MsiDb *db, const char *table) { return db && get_table(db, table) != NULL; }
int msidb_rows(MsiDb *db, const char *table) { MsiTable *t = db ? get_table(db, table) : NULL; return t ? t->nrows : 0; }
int msidb_column_count(MsiDb *db, const char *table) { MsiTable *t = db ? get_table(db, table) : NULL; return t ? t->ncols : 0; }

static const MsiCol *get_col(MsiDb *db, const char *table, int col) {
    MsiTable *t = db ? get_table(db, table) : NULL;
    return t && col >= 0 && col < t->ncols ? &t->cols[col] : NULL;
}
const char *msidb_column_name(MsiDb *db, const char *table, int col) { const MsiCol *c = get_col(db, table, col); return c ? c->name : NULL; }
int msidb_column_type(MsiDb *db, const char *table, int col) { const MsiCol *c = get_col(db, table, col); return c ? c->kind : 0; }
int msidb_column_raw_type(MsiDb *db, const char *table, int col) { const MsiCol *c = get_col(db, table, col); return c ? (int)c->type : -1; }
int msidb_column_is_key(MsiDb *db, const char *table, int col) { const MsiCol *c = get_col(db, table, col); return c ? (c->type & 0x2000) != 0 : 0; }

static int col_index(const MsiTable *t, const char *column) {
    if (!t || !column) return -1;
    for (int i = 0; i < t->ncols; i++)
        if (strcmp(t->cols[i].name, column) == 0) return i;
    return -1;
}
int msidb_column_index(MsiDb *db, const char *table, const char *column) { return db ? col_index(get_table(db, table), column) : -1; }

const char *msidb_str(MsiDb *db, const char *table, int row, const char *column) {
    MsiTable *t = db ? get_table(db, table) : NULL;
    int c = col_index(t, column);
    if (c < 0 || row < 0 || row >= t->nrows) return NULL;
    return cell_str(db, t, row, c);
}

int32_t msidb_int(MsiDb *db, const char *table, int row, const char *column) {
    MsiTable *t = db ? get_table(db, table) : NULL;
    int c = col_index(t, column);
    if (c < 0 || row < 0 || row >= t->nrows) return MSI_NULL_INT;
    if (t->cols[c].kind == 's') return MSI_NULL_INT;
    if (t->cols[c].kind == 'v') return cell_raw(t, row, c) ? 1 : MSI_NULL_INT;
    return cell_int(t, row, c);
}

// Text of a cell (strings as is, integers in decimal); returns 0 and fills buf, or -1 for NULL.
static int cell_text(MsiDb *db, const MsiTable *t, int row, int col, char *buf, size_t n) {
    if (t->cols[col].kind == 's') {
        const char *s = cell_str(db, t, row, col);
        if (!s) return -1;
        snprintf(buf, n, "%s", s);
        return 0;
    }
    if (t->cols[col].kind == 'v') return -1;
    int32_t v = cell_int(t, row, col);
    if (v == MSI_NULL_INT) return -1;
    snprintf(buf, n, "%d", (int)v);
    return 0;
}

int msidb_find(MsiDb *db, const char *table, const char *column, const char *value) {
    MsiTable *t = db ? get_table(db, table) : NULL;
    int c = col_index(t, column);
    if (c < 0 || !value) return -1;
    char buf[64];
    for (int r = 0; r < t->nrows; r++) {
        if (t->cols[c].kind == 's') {
            const char *s = cell_str(db, t, r, c);
            if (s && strcmp(s, value) == 0) return r;
        } else if (t->cols[c].kind == 'i') {
            if (cell_text(db, t, r, c, buf, sizeof buf) == 0 && strcmp(buf, value) == 0) return r;
        }
    }
    return -1;
}

int msidb_stream_count(MsiDb *db) { return db ? db->nvisible : 0; }
const char *msidb_stream_name(MsiDb *db, int i) {
    return db && i >= 0 && i < db->nvisible ? db->streams[db->visible[i]].name : NULL;
}
uint8_t *msidb_stream(MsiDb *db, const char *name, size_t *len) {
    size_t dummy;
    if (!db || !name) return NULL;
    return read_named(db, name, len ? len : &dummy);
}

uint8_t *msidb_cell_stream(MsiDb *db, const char *table, int row, const char *column, size_t *len) {
    MsiTable *t = db ? get_table(db, table) : NULL;
    int c = col_index(t, column);
    if (c < 0 || row < 0 || row >= t->nrows || t->cols[c].kind != 'v' || !cell_raw(t, row, c)) return NULL;
    char name[1024];
    size_t w = (size_t)snprintf(name, sizeof name, "%s", t->name);
    for (int k = 0; k < t->ncols; k++) {
        if (!(t->cols[k].type & 0x2000)) continue;
        char part[512];
        if (cell_text(db, t, row, k, part, sizeof part)) continue;
        if (w + strlen(part) + 2 >= sizeof name) return NULL;
        w += (size_t)snprintf(name + w, sizeof name - w, ".%s", part);
    }
    return msidb_stream(db, name, len);
}

// ---------------------------------------------------------------------------------------------
// SQL subset
// ---------------------------------------------------------------------------------------------
enum { C_AND, C_OR, C_NOT, C_CMP, C_ISNULL, C_NOTNULL };
enum { OP_EQ, OP_NE, OP_LT, OP_GT, OP_LE, OP_GE };
enum { V_STR, V_INT, V_PARAM };

typedef struct Cond {
    int kind;
    struct Cond *l, *r;
    int col, op, vkind, pidx;
    char *s;
} Cond;

struct MsiView {
    MsiDb *db;
    MsiTable *table;
    char *tname;
    int ncols; int *cols;
    Cond *where;
    int nord; int *ord; int *ord_desc;
    int nparams;
    char **params; int nbound;
    int executed;
    int *rows; int nrows, pos;
};

typedef struct { const char *p; char err[160]; } Lex;
typedef struct { int type; char text[512]; } Tok;   // type: 0 end, 1 ident, 2 string, 3 number, 4 punct, -1 error
enum { T_END, T_ID, T_STR, T_NUM, T_PUNCT, T_ERR };

static void lex_next(Lex *lx, Tok *t) {
    const char *p = lx->p;
    while (*p && isspace((unsigned char)*p)) p++;
    t->text[0] = 0;
    if (!*p) { t->type = T_END; lx->p = p; return; }
    size_t n = 0;
    if (*p == '`' || *p == '\'') {
        char q = *p++;
        while (*p && *p != q) { if (n < sizeof t->text - 1) t->text[n++] = *p; p++; }
        if (!*p) { t->type = T_ERR; snprintf(lx->err, sizeof lx->err, "unterminated quote"); lx->p = p; return; }
        p++;
        t->type = q == '`' ? T_ID : T_STR;
        // quoted identifiers are marked by text; keywords can't be quoted idents (flag via type only)
    } else if (isalpha((unsigned char)*p) || *p == '_') {
        while (isalnum((unsigned char)*p) || *p == '_' || *p == '$' || *p == '#') { if (n < sizeof t->text - 1) t->text[n++] = *p; p++; }
        t->type = T_ID;
    } else if (isdigit((unsigned char)*p) || (*p == '-' && isdigit((unsigned char)p[1]))) {
        t->text[n++] = *p++;
        while (isdigit((unsigned char)*p)) { if (n < sizeof t->text - 1) t->text[n++] = *p; p++; }
        t->type = T_NUM;
    } else {
        t->type = T_PUNCT;
        t->text[n++] = *p++;
        if ((t->text[0] == '<' && (*p == '>' || *p == '=')) || (t->text[0] == '>' && *p == '=')) t->text[n++] = *p++;
    }
    t->text[n] = 0;
    lx->p = p;
}

typedef struct { Lex lx; Tok t; int quoted; MsiView *v; char *msg; size_t msglen; int depth; } Parser;

static void pnext(Parser *ps) {
    const char *before = ps->lx.p;
    while (*before && isspace((unsigned char)*before)) before++;
    ps->quoted = *before == '`';
    lex_next(&ps->lx, &ps->t);
}
static int perr(Parser *ps, const char *fmt, const char *a) {
    if (ps->msg && ps->msglen) {
        snprintf(ps->msg, ps->msglen, fmt, a);
    }
    return -1;
}
static int is_kw(Parser *ps, const char *kw) {
    return ps->t.type == T_ID && !ps->quoted && strcasecmp(ps->t.text, kw) == 0;
}
static int is_punct(Parser *ps, char c) { return ps->t.type == T_PUNCT && ps->t.text[0] == c && ps->t.text[1] == 0; }

static void cond_free(Cond *c) {
    if (!c) return;
    cond_free(c->l);
    cond_free(c->r);
    free(c->s);
    free(c);
}

static int parse_column(Parser *ps, int *out) {
    if (ps->t.type != T_ID) return perr(ps, "column name expected%s", "");
    char name[512];
    snprintf(name, sizeof name, "%s", ps->t.text);
    pnext(ps);
    if (is_punct(ps, '.')) {      // `Table`.`Column`
        pnext(ps);
        if (ps->t.type != T_ID) return perr(ps, "column name expected%s", "");
        snprintf(name, sizeof name, "%s", ps->t.text);
        pnext(ps);
    }
    int c = col_index(ps->v->table, name);
    if (c < 0) return perr(ps, "unknown column '%s'", name);
    *out = c;
    return 0;
}

static Cond *parse_or(Parser *ps);

static Cond *parse_primary(Parser *ps) {
    if (ps->depth > 64) { perr(ps, "expression too deeply nested%s", ""); return NULL; }
    if (is_punct(ps, '(')) {
        pnext(ps);
        ps->depth++;
        Cond *c = parse_or(ps);
        ps->depth--;
        if (!c) return NULL;
        if (!is_punct(ps, ')')) { cond_free(c); perr(ps, "')' expected%s", ""); return NULL; }
        pnext(ps);
        return c;
    }
    if (is_kw(ps, "NOT")) {
        pnext(ps);
        ps->depth++;
        Cond *in = parse_primary(ps);
        ps->depth--;
        if (!in) return NULL;
        Cond *c = calloc(1, sizeof *c);
        if (!c) { cond_free(in); return NULL; }
        c->kind = C_NOT;
        c->l = in;
        return c;
    }
    int col;
    if (parse_column(ps, &col)) return NULL;
    Cond *c = calloc(1, sizeof *c);
    if (!c) return NULL;
    c->col = col;
    if (is_kw(ps, "IS")) {
        pnext(ps);
        c->kind = C_ISNULL;
        if (is_kw(ps, "NOT")) { c->kind = C_NOTNULL; pnext(ps); }
        if (!is_kw(ps, "NULL")) { cond_free(c); perr(ps, "NULL expected%s", ""); return NULL; }
        pnext(ps);
        return c;
    }
    c->kind = C_CMP;
    if (ps->t.type != T_PUNCT) { cond_free(c); perr(ps, "comparison operator expected%s", ""); return NULL; }
    const char *o = ps->t.text;
    if (!strcmp(o, "=")) c->op = OP_EQ;
    else if (!strcmp(o, "<>")) c->op = OP_NE;
    else if (!strcmp(o, "<")) c->op = OP_LT;
    else if (!strcmp(o, ">")) c->op = OP_GT;
    else if (!strcmp(o, "<=")) c->op = OP_LE;
    else if (!strcmp(o, ">=")) c->op = OP_GE;
    else { cond_free(c); perr(ps, "bad operator '%s'", o); return NULL; }
    pnext(ps);
    if (ps->t.type == T_STR) { c->vkind = V_STR; c->s = strdup(ps->t.text); }
    else if (ps->t.type == T_NUM) { c->vkind = V_INT; c->s = strdup(ps->t.text); }
    else if (is_punct(ps, '?')) { c->vkind = V_PARAM; c->pidx = ps->v->nparams++; }
    else { cond_free(c); perr(ps, "value expected%s", ""); return NULL; }
    if (c->vkind != V_PARAM && !c->s) { cond_free(c); return NULL; }
    pnext(ps);
    return c;
}

static Cond *parse_and(Parser *ps) {
    Cond *l = parse_primary(ps);
    while (l && is_kw(ps, "AND")) {
        pnext(ps);
        Cond *r = parse_primary(ps);
        Cond *n = r ? calloc(1, sizeof *n) : NULL;
        if (!n) { cond_free(l); cond_free(r); return NULL; }
        n->kind = C_AND; n->l = l; n->r = r;
        l = n;
    }
    return l;
}

static Cond *parse_or(Parser *ps) {
    Cond *l = parse_and(ps);
    while (l && is_kw(ps, "OR")) {
        pnext(ps);
        Cond *r = parse_and(ps);
        Cond *n = r ? calloc(1, sizeof *n) : NULL;
        if (!n) { cond_free(l); cond_free(r); return NULL; }
        n->kind = C_OR; n->l = l; n->r = r;
        l = n;
    }
    return l;
}

void msiview_close(MsiView *v) {
    if (!v) return;
    free(v->tname);
    free(v->cols);
    cond_free(v->where);
    free(v->ord);
    free(v->ord_desc);
    for (int i = 0; i < v->nbound; i++) free(v->params[i]);
    free(v->params);
    free(v->rows);
    free(v);
}

MsiView *msiview_open(MsiDb *db, const char *sql, char *err, size_t errlen) {
    if (err && errlen) err[0] = 0;
    if (!db || !sql) { set_err(err, errlen, "bad arguments%s", ""); return NULL; }
    MsiView *v = calloc(1, sizeof *v);
    if (!v) { set_err(err, errlen, "out of memory%s", ""); return NULL; }
    v->db = db;
    Parser ps;
    memset(&ps, 0, sizeof ps);
    ps.lx.p = sql;
    ps.v = v;
    ps.msg = err;
    ps.msglen = errlen;
    // Parse the select list as names first (the table comes later), resolve afterwards.
    char **names = NULL; size_t nnames = 0, cap = 0;
    int star = 0, ok = 0;
    pnext(&ps);
    if (!is_kw(&ps, "SELECT")) { perr(&ps, "SELECT expected%s", ""); goto done; }
    pnext(&ps);
    if (is_kw(&ps, "DISTINCT")) pnext(&ps);
    if (is_punct(&ps, '*')) { star = 1; pnext(&ps); }
    else for (;;) {
        if (ps.t.type != T_ID) { perr(&ps, "column name expected%s", ""); goto done; }
        char nm[512];
        snprintf(nm, sizeof nm, "%s", ps.t.text);
        pnext(&ps);
        if (is_punct(&ps, '.')) {
            pnext(&ps);
            if (ps.t.type != T_ID) { perr(&ps, "column name expected%s", ""); goto done; }
            snprintf(nm, sizeof nm, "%s", ps.t.text);
            pnext(&ps);
        }
        if (nnames == cap) {
            cap = cap ? cap * 2 : 8;
            char **nn = realloc(names, cap * sizeof *nn);
            if (!nn) goto done;
            names = nn;
        }
        names[nnames] = strdup(nm);
        if (!names[nnames]) goto done;
        nnames++;
        if (is_punct(&ps, ',')) { pnext(&ps); continue; }
        break;
    }
    if (!is_kw(&ps, "FROM")) { perr(&ps, "FROM expected%s", ""); goto done; }
    pnext(&ps);
    if (ps.t.type != T_ID) { perr(&ps, "table name expected%s", ""); goto done; }
    v->table = get_table(db, ps.t.text);
    if (!v->table || !v->table->ncols) { perr(&ps, "unknown table '%s'", ps.t.text); goto done; }
    v->tname = strdup(v->table->name);
    pnext(&ps);
    if (star) {
        v->ncols = v->table->ncols;
        v->cols = malloc((size_t)v->ncols * sizeof(int));
        if (!v->cols) goto done;
        for (int i = 0; i < v->ncols; i++) v->cols[i] = i;
    } else {
        v->cols = malloc(nnames * sizeof(int));
        if (!v->cols) goto done;
        for (size_t i = 0; i < nnames; i++) {
            int c = col_index(v->table, names[i]);
            if (c < 0) { perr(&ps, "unknown column '%s'", names[i]); goto done; }
            v->cols[v->ncols++] = c;
        }
    }
    if (is_kw(&ps, "WHERE")) {
        pnext(&ps);
        v->where = parse_or(&ps);
        if (!v->where) { if (err && !err[0]) perr(&ps, "bad WHERE clause%s", ""); goto done; }
    }
    if (is_kw(&ps, "ORDER")) {
        pnext(&ps);
        if (!is_kw(&ps, "BY")) { perr(&ps, "BY expected%s", ""); goto done; }
        pnext(&ps);
        for (;;) {
            int c;
            if (parse_column(&ps, &c)) goto done;
            int desc = 0;
            if (is_kw(&ps, "DESC")) { desc = 1; pnext(&ps); }
            else if (is_kw(&ps, "ASC")) pnext(&ps);
            int *no = realloc(v->ord, (size_t)(v->nord + 1) * sizeof(int));
            if (!no) goto done;
            v->ord = no;
            int *nd = realloc(v->ord_desc, (size_t)(v->nord + 1) * sizeof(int));
            if (!nd) goto done;
            v->ord_desc = nd;
            v->ord[v->nord] = c;
            v->ord_desc[v->nord++] = desc;
            if (is_punct(&ps, ',')) { pnext(&ps); continue; }
            break;
        }
    }
    if (ps.t.type != T_END) { perr(&ps, "unexpected '%s'", ps.t.text); goto done; }
    ok = 1;
done:
    if (ps.lx.err[0] && err && !err[0]) snprintf(err, errlen, "%s", ps.lx.err);
    for (size_t i = 0; i < nnames; i++) free(names[i]);
    free(names);
    if (!ok) {
        if (err && errlen && !err[0]) snprintf(err, errlen, "SQL error");
        msiview_close(v);
        return NULL;
    }
    return v;
}

const char *msiview_table(MsiView *v) { return v ? v->tname : NULL; }
int msiview_column_count(MsiView *v) { return v ? v->ncols : 0; }
const char *msiview_column_name(MsiView *v, int i) {
    return v && i >= 0 && i < v->ncols ? v->table->cols[v->cols[i]].name : NULL;
}

static int parse_int_text(const char *s, int32_t *out) {
    if (!s || !*s) return -1;
    char *end;
    long long x = strtoll(s, &end, 10);
    if (*end || x < INT32_MIN || x > INT32_MAX) return -1;
    *out = (int32_t)x;
    return 0;
}

static int cond_eval(MsiView *v, const Cond *c, int row) {
    MsiTable *t = v->table;
    switch (c->kind) {
    case C_AND: return cond_eval(v, c->l, row) && cond_eval(v, c->r, row);
    case C_OR: return cond_eval(v, c->l, row) || cond_eval(v, c->r, row);
    case C_NOT: return !cond_eval(v, c->l, row);
    case C_ISNULL: case C_NOTNULL: {
        const MsiCol *col = &t->cols[c->col];
        int isnull = col->kind == 's' ? cell_str(v->db, t, row, c->col) == NULL : cell_raw(t, row, c->col) == 0;
        return c->kind == C_ISNULL ? isnull : !isnull;
    }
    default: break;
    }
    const MsiCol *col = &t->cols[c->col];
    const char *val = c->vkind == V_PARAM ? (c->pidx < v->nbound ? v->params[c->pidx] : NULL) : c->s;
    if (col->kind == 'v') return 0;
    int cmp = 0, null_cell, null_val = val == NULL;
    if (col->kind == 's') {
        const char *s = cell_str(v->db, t, row, c->col);
        null_cell = s == NULL;
        if (!null_cell && !null_val) cmp = strcmp(s, val);
    } else {
        int32_t x = cell_int(t, row, c->col), y = 0;
        null_cell = x == MSI_NULL_INT;
        if (!null_val && parse_int_text(val, &y)) return c->op == OP_NE;
        if (!null_cell && !null_val) cmp = x < y ? -1 : x > y;
    }
    if (null_cell || null_val) {
        if (c->op == OP_EQ) return null_cell && null_val;
        if (c->op == OP_NE) return !(null_cell && null_val);
        return 0;
    }
    switch (c->op) {
    case OP_EQ: return cmp == 0;
    case OP_NE: return cmp != 0;
    case OP_LT: return cmp < 0;
    case OP_GT: return cmp > 0;
    case OP_LE: return cmp <= 0;
    default: return cmp >= 0;
    }
}

static int row_cmp(MsiView *v, int a, int b) {
    MsiTable *t = v->table;
    for (int k = 0; k < v->nord; k++) {
        int c = v->ord[k], r = 0;
        if (t->cols[c].kind == 's') {
            const char *x = cell_str(v->db, t, a, c), *y = cell_str(v->db, t, b, c);
            if (!x || !y) r = (x != NULL) - (y != NULL);
            else r = strcmp(x, y);
        } else if (t->cols[c].kind == 'i') {
            int32_t x = cell_int(t, a, c), y = cell_int(t, b, c);
            r = x < y ? -1 : x > y;
        }
        if (r) return v->ord_desc[k] ? -r : r;
    }
    return a < b ? -1 : a > b;
}

static void sort_rows(MsiView *v, int *rows, int n, int *tmp) {
    if (n < 2) return;
    int h = n / 2;
    sort_rows(v, rows, h, tmp);
    sort_rows(v, rows + h, n - h, tmp);
    int i = 0, j = h, k = 0;
    while (i < h && j < n) tmp[k++] = row_cmp(v, rows[i], rows[j]) <= 0 ? rows[i++] : rows[j++];
    while (i < h) tmp[k++] = rows[i++];
    while (j < n) tmp[k++] = rows[j++];
    memcpy(rows, tmp, (size_t)n * sizeof(int));
}

int msiview_execute(MsiView *v, const char *const *params, int nparams) {
    if (!v) return -1;
    if (nparams < v->nparams) return -1;
    for (int i = 0; i < v->nbound; i++) free(v->params[i]);
    free(v->params);
    v->params = NULL;
    v->nbound = 0;
    if (v->nparams) {
        v->params = calloc((size_t)v->nparams, sizeof(char *));
        if (!v->params) return -1;
        v->nbound = v->nparams;
        for (int i = 0; i < v->nparams; i++) {
            const char *p = params ? params[i] : NULL;
            if (p) {
                v->params[i] = strdup(p);
                if (!v->params[i]) return -1;
            }
        }
    }
    free(v->rows);
    v->nrows = v->pos = 0;
    v->rows = malloc(((size_t)v->table->nrows + 1) * sizeof(int));
    if (!v->rows) return -1;
    for (int r = 0; r < v->table->nrows; r++)
        if (!v->where || cond_eval(v, v->where, r)) v->rows[v->nrows++] = r;
    if (v->nord && v->nrows > 1) {
        int *tmp = malloc((size_t)v->nrows * sizeof(int));
        if (!tmp) return -1;
        sort_rows(v, v->rows, v->nrows, tmp);
        free(tmp);
    }
    v->executed = 1;
    return 0;
}

int msiview_fetch(MsiView *v) {
    if (!v) return -1;
    if (!v->executed && msiview_execute(v, NULL, 0)) return -1;
    return v->pos < v->nrows ? v->rows[v->pos++] : -1;
}
