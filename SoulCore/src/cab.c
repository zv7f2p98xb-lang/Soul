// SoulCore - Microsoft Cabinet (MS-CAB) extractor: stored, MSZIP and LZX folders. Pure libc + inflate.h.
#include "msi.h"
#include "inflate.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CAB_MAX_FOLDER ((size_t)1 << 30)

static void cab_err(char *err, size_t n, const char *msg) {
    if (err && n) snprintf(err, n, "%s", msg);
}

static uint32_t rd16(const uint8_t *p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8); }
static uint32_t rd32(const uint8_t *p) { return rd16(p) | (rd16(p + 2) << 16); }

// Skips a NUL-terminated string at *pos; returns 0 on success.
static int skip_cstr(const uint8_t *cab, size_t len, size_t *pos) {
    while (*pos < len && cab[*pos]) (*pos)++;
    if (*pos >= len) return -1;
    (*pos)++;
    return 0;
}

// Decodes all CFDATA blocks of one folder into a malloc'ed buffer. Returns 0 or -1 (message in err).
static int decode_folder(const uint8_t *cab, size_t len, size_t data_off, unsigned nblocks, unsigned ctype,
                         size_t cb_data_res, uint8_t **out, size_t *out_len, char *err, size_t errlen) {
    uint8_t *buf = NULL;
    size_t cap = 0, used = 0, pos = data_off;
    for (unsigned b = 0; b < nblocks; b++) {
        if (pos > len || len - pos < 8 + cb_data_res) { cab_err(err, errlen, "cab: truncated CFDATA"); goto fail; }
        unsigned cb = rd16(cab + pos + 4), cu = rd16(cab + pos + 6);
        pos += 8 + cb_data_res;
        if (cb > len - pos) { cab_err(err, errlen, "cab: CFDATA beyond end of file"); goto fail; }
        if (cu == 0) { cab_err(err, errlen, "cab: data block continued in another cabinet (multi-cabinet sets are not supported)"); goto fail; }
        if (used + cu > CAB_MAX_FOLDER) { cab_err(err, errlen, "cab: folder too large"); goto fail; }
        if (used + cu > cap) {
            size_t ncap = cap ? cap * 2 : 65536;
            while (ncap < used + cu) ncap *= 2;
            uint8_t *nb = realloc(buf, ncap);
            if (!nb) { cab_err(err, errlen, "cab: out of memory"); goto fail; }
            buf = nb; cap = ncap;
        }
        if (ctype == 0) {
            if (cb != cu) { cab_err(err, errlen, "cab: bad stored block"); goto fail; }
            memcpy(buf + used, cab + pos, cu);
        } else {
            if (cb < 2 || cab[pos] != 'C' || cab[pos + 1] != 'K') { cab_err(err, errlen, "cab: bad MSZIP signature"); goto fail; }
            if (cu > 32768) { cab_err(err, errlen, "cab: bad MSZIP block size"); goto fail; }
            size_t dl = used < 32768 ? used : 32768;
            long r = soul_inflate(cab + pos + 2, cb - 2, buf + used, cu, dl ? buf + used - dl : NULL, dl);
            if (r != (long)cu) { cab_err(err, errlen, "cab: corrupt MSZIP data"); goto fail; }
        }
        used += cu;
        pos += cb;
    }
    *out = buf ? buf : malloc(1);
    if (!*out) { cab_err(err, errlen, "cab: out of memory"); return -1; }
    *out_len = used;
    return 0;
fail:
    free(buf);
    return -1;
}


// ---- LZX ------------------------------------------------------------------------------------------------------------
// Decoder for the LZX variant used in cabinets (window 2^15..2^21, 32 KB frames, Intel E8 translation). The input is the
// concatenation of the folder's CFDATA payloads; the output buffer doubles as the window (E8 translation runs last).
#define LZX_FRAME 32768
#define LZX_MAXMAIN (256 + 8 * 50)
#define LZX_NLEN 249
#define LZX_HBITS 10

typedef struct {
    uint16_t tab[1 << LZX_HBITS];   // (sym << 5) | len for codes up to LZX_HBITS bits; 0 = longer or invalid
    uint16_t sorted[LZX_MAXMAIN];   // symbols ordered by (length, symbol)
    uint32_t first[18];             // first canonical code of each length
    uint16_t count[18], offs[18];
} LzxHuff;

typedef struct {
    const uint8_t *in;
    size_t ilen, ip;
    uint32_t bb;
    int bl;
} LzxBits;

static inline void lzx_fill(LzxBits *b, int n) {
    if (b->bl < n) {
        uint32_t w = 0;
        if (b->ip + 1 < b->ilen) w = (uint32_t)b->in[b->ip] | ((uint32_t)b->in[b->ip + 1] << 8);
        else if (b->ip < b->ilen) w = b->in[b->ip];
        if (b->ip <= b->ilen + 8) b->ip += 2;    // reading past the end yields zeros; lzx_overrun() catches it
        b->bb |= w << (16 - b->bl);
        b->bl += 16;
    }
}
static inline int lzx_overrun(const LzxBits *b) { return b->ip > b->ilen + 4; }
static inline uint32_t lzx_peek(LzxBits *b, int n) { lzx_fill(b, n); return b->bb >> (32 - n); }
static inline void lzx_drop(LzxBits *b, int n) { b->bb <<= n; b->bl -= n; }
static inline uint32_t lzx_bits(LzxBits *b, int n) {
    if (n == 0) return 0;
    uint32_t v = lzx_peek(b, n);
    lzx_drop(b, n);
    return v;
}
// Returns the stream to a 16-bit boundary: drops the rest of the current word and gives back whole preloaded words.
static void lzx_align(LzxBits *b, int skip_full) {
    int rem = b->bl & 15;
    size_t words = (size_t)(b->bl >> 4);
    b->ip -= words * 2;
    if (!rem && skip_full) b->ip += 2;     // already aligned: an uncompressed block header skips one whole word
    b->bb = 0;
    b->bl = 0;
}

// Builds the decoding tables; returns -1 for over-subscribed codes (incomplete ones are accepted).
static int lzx_build(LzxHuff *h, const uint8_t *len, int n) {
    memset(h->tab, 0, sizeof h->tab);
    memset(h->count, 0, sizeof h->count);
    for (int i = 0; i < n; i++) h->count[len[i]]++;
    h->count[0] = 0;
    int left = 1;
    for (int l = 1; l <= 16; l++) {
        left = left * 2 - h->count[l];
        if (left < 0) return -1;
    }
    uint32_t code = 0;
    uint16_t off = 0;
    for (int l = 1; l <= 16; l++) {
        code = (code + h->count[l - 1]) << 1;
        h->first[l] = code;
        h->offs[l] = off;
        off = (uint16_t)(off + h->count[l]);
    }
    uint16_t pos[18];
    memcpy(pos, h->offs, sizeof pos);
    for (int i = 0; i < n; i++) {
        int l = len[i];
        if (!l) continue;
        uint32_t c = h->first[l] + (uint32_t)(pos[l] - h->offs[l]);
        h->sorted[pos[l]++] = (uint16_t)i;
        if (l <= LZX_HBITS) {
            uint32_t base = c << (LZX_HBITS - l), cnt = 1u << (LZX_HBITS - l);
            for (uint32_t k = 0; k < cnt; k++) h->tab[base + k] = (uint16_t)((i << 5) | l);
        }
    }
    return 0;
}

// Decodes one symbol; -1 when the bits match no code.
static inline int lzx_sym(LzxBits *b, const LzxHuff *h) {
    lzx_fill(b, 16);
    uint16_t e = h->tab[b->bb >> (32 - LZX_HBITS)];
    if (e) {
        lzx_drop(b, e & 31);
        return e >> 5;
    }
    uint32_t code = b->bb >> 16;
    for (int l = LZX_HBITS + 1; l <= 16; l++) {
        uint32_t c = code >> (16 - l);
        if (c >= h->first[l] && c - h->first[l] < h->count[l]) {
            lzx_drop(b, l);
            return h->sorted[h->offs[l] + (c - h->first[l])];
        }
    }
    return -1;
}

// Reads delta-coded code lengths for symbols first..last-1 (pretree format). Returns 0 or -1.
static int lzx_read_lens(LzxBits *b, uint8_t *lens, int first, int last) {
    uint8_t pl[20];
    LzxHuff pt;
    for (int i = 0; i < 20; i++) pl[i] = (uint8_t)lzx_bits(b, 4);
    if (lzx_build(&pt, pl, 20)) return -1;
    int i = first;
    while (i < last) {
        int s = lzx_sym(b, &pt);
        if (s < 0 || lzx_overrun(b)) return -1;
        if (s <= 16) {
            lens[i] = (uint8_t)((lens[i] + 17 - s) % 17);
            i++;
            continue;
        }
        int run, val = 0;
        if (s == 17) run = 4 + (int)lzx_bits(b, 4);
        else if (s == 18) run = 20 + (int)lzx_bits(b, 5);
        else {
            run = 4 + (int)lzx_bits(b, 1);
            int z = lzx_sym(b, &pt);
            if (z < 0 || z > 16) return -1;
            val = (lens[i] + 17 - z) % 17;
        }
        if (run > last - i) return -1;
        while (run--) lens[i++] = (uint8_t)val;
    }
    return 0;
}

typedef struct {
    LzxBits b;
    LzxHuff main, len, aligned;
    uint8_t mainlen[LZX_MAXMAIN], lenlen[LZX_NLEN + 8];
    uint32_t R[3];
    uint32_t e8size;
    int btype, first_block;
    size_t brem, blen;
} LzxState;

// Decodes the folder: cu[i] bytes per frame (each <= 32768, all but the last must be 32768).
static int lzx_decode(const uint8_t *in, size_t ilen, int wbits, const uint16_t *cu, unsigned nframes,
                      uint8_t **out_p, size_t *out_len, char *err, size_t errlen) {
    static const uint8_t eb_tab[] = {0,0,0,0,1,1,2,2,3,3,4,4,5,5,6,6,7,7,8,8,9,9,10,10,11,11,12,12,13,13,14,14,15,15,16,16,17,17,17,17,17,17,17,17,17,17,17,17,17,17};
    uint32_t base[52];
    int nslots = wbits == 20 ? 42 : wbits == 21 ? 50 : wbits * 2;
    base[0] = 0;
    for (int s = 1; s <= nslots; s++) base[s] = base[s - 1] + (1u << eb_tab[s - 1]);
    size_t window = (size_t)1 << wbits;
    int nmain = 256 + 8 * nslots;
    LzxState *z = calloc(1, sizeof *z);
    uint8_t *out = NULL;
    size_t cap = 0, pos = 0;
    const char *why = "cab: corrupt LZX data";
    if (!z) { cab_err(err, errlen, "cab: out of memory"); return -1; }
    z->b.in = in;
    z->b.ilen = ilen;
    z->R[0] = z->R[1] = z->R[2] = 1;
    z->first_block = 1;
    LzxBits *b = &z->b;
    for (unsigned f = 0; f < nframes; f++) {
        size_t fsz = cu[f];
        if (fsz != LZX_FRAME && f + 1 != nframes) { why = "cab: bad LZX frame size"; goto fail; }
        if (pos + fsz > cap) {
            size_t ncap = cap ? cap * 2 : 65536;
            while (ncap < pos + fsz) ncap *= 2;
            uint8_t *nb = realloc(out, ncap);
            if (!nb) { why = "cab: out of memory"; goto fail; }
            out = nb; cap = ncap;
        }
        size_t fend = pos + fsz;
        while (pos < fend) {
            if (z->brem == 0) {
                if (z->btype == 3 && (z->blen & 1)) {       // odd-sized uncompressed block: one pad byte
                    if (b->ip >= b->ilen) goto fail;
                    b->ip++;
                }
                if (z->first_block) {
                    z->first_block = 0;
                    if (lzx_bits(b, 1)) {
                        z->e8size = lzx_bits(b, 16) << 16;
                        z->e8size |= lzx_bits(b, 16);
                    }
                }
                int t = (int)lzx_bits(b, 3);
                uint32_t hi = lzx_bits(b, 16), lo = lzx_bits(b, 8);
                z->blen = z->brem = ((size_t)hi << 8) | lo;
                z->btype = t;
                if (t < 1 || t > 3 || lzx_overrun(b)) goto fail;
                if (t == 3) {
                    lzx_align(b, 1);
                    if (b->ilen - (b->ip < b->ilen ? b->ip : b->ilen) < 12) goto fail;
                    for (int i = 0; i < 3; i++) z->R[i] = (uint32_t)in[b->ip + 4 * i] | ((uint32_t)in[b->ip + 4 * i + 1] << 8) | ((uint32_t)in[b->ip + 4 * i + 2] << 16) | ((uint32_t)in[b->ip + 4 * i + 3] << 24);
                    b->ip += 12;
                } else {
                    if (t == 2) {
                        uint8_t al[8];
                        for (int i = 0; i < 8; i++) al[i] = (uint8_t)lzx_bits(b, 3);
                        if (lzx_build(&z->aligned, al, 8)) goto fail;
                    }
                    if (lzx_read_lens(b, z->mainlen, 0, 256) || lzx_read_lens(b, z->mainlen, 256, nmain) ||
                        lzx_read_lens(b, z->lenlen, 0, LZX_NLEN) || lzx_overrun(b)) goto fail;
                    if (lzx_build(&z->main, z->mainlen, nmain) || lzx_build(&z->len, z->lenlen, LZX_NLEN)) goto fail;
                }
                if (z->brem == 0) continue;
            }
            size_t run = z->brem < fend - pos ? z->brem : fend - pos;
            z->brem -= run;
            if (z->btype == 3) {
                if (b->ip > b->ilen || run > b->ilen - b->ip) goto fail;
                memcpy(out + pos, in + b->ip, run);
                b->ip += run;
                pos += run;
                continue;
            }
            size_t rend = pos + run;
            while (pos < rend) {
                int s = lzx_sym(b, &z->main);
                if (s < 0 || lzx_overrun(b)) goto fail;
                if (s < 256) { out[pos++] = (uint8_t)s; continue; }
                s -= 256;
                int slot = s >> 3;
                size_t mlen = (size_t)(s & 7) + 2;
                if ((s & 7) == 7) {
                    int l = lzx_sym(b, &z->len);
                    if (l < 0) goto fail;
                    mlen += (size_t)l;
                }
                uint32_t off;
                if (slot == 0) off = z->R[0];
                else if (slot == 1) { off = z->R[1]; z->R[1] = z->R[0]; z->R[0] = off; }
                else if (slot == 2) { off = z->R[2]; z->R[2] = z->R[0]; z->R[0] = off; }
                else {
                    int eb = eb_tab[slot];
                    uint32_t extra;
                    if (z->btype == 2 && eb >= 3) {
                        extra = eb > 3 ? lzx_bits(b, eb - 3) << 3 : 0;
                        int a = lzx_sym(b, &z->aligned);
                        if (a < 0) goto fail;
                        extra += (uint32_t)a;
                    } else if (eb > 16) {
                        extra = lzx_bits(b, eb - 16) << 16;
                        extra |= lzx_bits(b, 16);
                    } else extra = lzx_bits(b, eb);
                    off = base[slot] + extra - 2;
                    z->R[2] = z->R[1]; z->R[1] = z->R[0]; z->R[0] = off;
                }
                if (off == 0 || off > pos || off > window || mlen > rend - pos) goto fail;
                const uint8_t *src = out + pos - off;
                uint8_t *dst = out + pos;
                if (off >= mlen) memcpy(dst, src, mlen);
                else for (size_t k = 0; k < mlen; k++) dst[k] = src[k];
                pos += mlen;
            }
            if (lzx_overrun(b)) goto fail;
        }
        // frames end on a 16-bit boundary
        lzx_align(b, 0);
        if (b->ip > b->ilen) goto fail;
    }
    // Intel E8 call translation (undone per frame, first 1 GB only), applied after decoding because matches
    // refer to the untranslated data.
    if (z->e8size) {
        int32_t fsz32 = (int32_t)z->e8size;
        for (size_t fp = 0, fi = 0; fp < pos && fi < 32768; fp += LZX_FRAME, fi++) {
            size_t fl = pos - fp < LZX_FRAME ? pos - fp : LZX_FRAME;
            if (fl <= 10) continue;
            uint8_t *d = out + fp;
            size_t i = 0, end = fl - 10;
            while (i < end) {
                if (d[i] != 0xE8) { i++; continue; }
                int32_t curpos = (int32_t)(fp + i);
                int32_t a = (int32_t)((uint32_t)d[i + 1] | ((uint32_t)d[i + 2] << 8) | ((uint32_t)d[i + 3] << 16) | ((uint32_t)d[i + 4] << 24));
                if (a >= -curpos && a < fsz32) {
                    int32_t rel = a >= 0 ? a - curpos : (int32_t)((uint32_t)a + (uint32_t)fsz32);
                    d[i + 1] = (uint8_t)rel; d[i + 2] = (uint8_t)(rel >> 8); d[i + 3] = (uint8_t)(rel >> 16); d[i + 4] = (uint8_t)(rel >> 24);
                }
                i += 5;
            }
        }
    }
    free(z);
    *out_p = out ? out : malloc(1);
    if (!*out_p) { cab_err(err, errlen, "cab: out of memory"); return -1; }
    *out_len = pos;
    return 0;
fail:
    cab_err(err, errlen, why);
    free(out);
    free(z);
    return -1;
}

// Collects the folder's CFDATA blocks and runs the LZX decoder over them.
static int decode_lzx_folder(const uint8_t *cab, size_t len, size_t data_off, unsigned nblocks, unsigned typecomp,
                             size_t cb_data_res, uint8_t **out, size_t *out_len, char *err, size_t errlen) {
    int wbits = (int)((typecomp >> 8) & 0x1F);
    if (wbits < 15 || wbits > 21) { cab_err(err, errlen, "cab: bad LZX window size"); return -1; }
    uint8_t *in = malloc(len < data_off ? 1 : len - data_off + 1);
    uint16_t *cu = malloc(sizeof(uint16_t) * (nblocks + 1));
    size_t used = 0, pos = data_off, total = 0;
    int rc = -1;
    if (!in || !cu) { cab_err(err, errlen, "cab: out of memory"); goto done; }
    for (unsigned b = 0; b < nblocks; b++) {
        if (pos > len || len - pos < 8 + cb_data_res) { cab_err(err, errlen, "cab: truncated CFDATA"); goto done; }
        unsigned cb = rd16(cab + pos + 4), u = rd16(cab + pos + 6);
        pos += 8 + cb_data_res;
        if (cb > len - pos) { cab_err(err, errlen, "cab: CFDATA beyond end of file"); goto done; }
        if (u == 0 || u > LZX_FRAME) { cab_err(err, errlen, "cab: bad LZX block size (multi-cabinet sets are not supported)"); goto done; }
        total += u;
        if (total > CAB_MAX_FOLDER) { cab_err(err, errlen, "cab: folder too large"); goto done; }
        memcpy(in + used, cab + pos, cb);
        used += cb;
        cu[b] = (uint16_t)u;
        pos += cb;
    }
    rc = lzx_decode(in, used, wbits, cu, nblocks, out, out_len, err, errlen);
done:
    free(in);
    free(cu);
    return rc;
}

static char *latin1_to_utf8(const uint8_t *s, size_t n) {
    char *o = malloc(n * 2 + 1), *p = o;
    if (!o) return NULL;
    for (size_t i = 0; i < n; i++) {
        if (s[i] < 0x80) *p++ = (char)s[i];
        else { *p++ = (char)(0xC0 | (s[i] >> 6)); *p++ = (char)(0x80 | (s[i] & 0x3F)); }
    }
    *p = 0;
    return o;
}

int cab_extract(const uint8_t *cab, size_t len, CabFileFn fn, void *ctx, char *err, size_t errlen) {
    if (err && errlen) err[0] = 0;
    if (!cab || len < 36 || memcmp(cab, "MSCF", 4) != 0) { cab_err(err, errlen, "cab: not a cabinet (bad signature)"); return -1; }
    size_t coff_files = rd32(cab + 16);
    unsigned nfolders = rd16(cab + 26), nfiles = rd16(cab + 28), flags = rd16(cab + 30);
    size_t pos = 36, cb_folder_res = 0, cb_data_res = 0;
    if (cab[25] != 1) { cab_err(err, errlen, "cab: unsupported cabinet version"); return -1; }
    if (flags & 4) {
        if (len < 40) { cab_err(err, errlen, "cab: truncated header"); return -1; }
        size_t cbh = rd16(cab + 36);
        cb_folder_res = cab[38];
        cb_data_res = cab[39];
        pos = 40 + cbh;
        if (pos > len) { cab_err(err, errlen, "cab: truncated header"); return -1; }
    }
    if (flags & 1) {
        if (skip_cstr(cab, len, &pos) || skip_cstr(cab, len, &pos)) { cab_err(err, errlen, "cab: truncated header"); return -1; }
    }
    if (flags & 2) {
        if (skip_cstr(cab, len, &pos) || skip_cstr(cab, len, &pos)) { cab_err(err, errlen, "cab: truncated header"); return -1; }
    }
    size_t fsz = 8 + cb_folder_res;
    if (nfolders == 0 && nfiles) { cab_err(err, errlen, "cab: files without folders"); return -1; }
    if (pos > len || (size_t)nfolders * fsz > len - pos) { cab_err(err, errlen, "cab: truncated folder table"); return -1; }
    size_t folders_pos = pos;
    // Validate the file table once (bounds and names) so extraction can walk it without rechecks.
    if (coff_files > len) { cab_err(err, errlen, "cab: bad file table offset"); return -1; }
    size_t p = coff_files;
    for (unsigned i = 0; i < nfiles; i++) {
        if (len - p < 16) { cab_err(err, errlen, "cab: truncated file table"); return -1; }
        unsigned ifold = rd16(cab + p + 8);
        if (ifold >= 0xFFFD) { cab_err(err, errlen, "cab: file continued in another cabinet (multi-cabinet sets are not supported)"); return -1; }
        if (ifold >= nfolders) { cab_err(err, errlen, "cab: bad folder index"); return -1; }
        p += 16;
        if (skip_cstr(cab, len, &p)) { cab_err(err, errlen, "cab: truncated file name"); return -1; }
    }
    for (unsigned f = 0; f < nfolders; f++) {
        // Skip folders without files cheaply.
        int has = 0;
        size_t q = coff_files;
        for (unsigned i = 0; i < nfiles; i++) {
            if (rd16(cab + q + 8) == f) { has = 1; break; }
            q += 16;
            skip_cstr(cab, len, &q);
        }
        if (!has) continue;
        const uint8_t *fo = cab + folders_pos + (size_t)f * fsz;
        size_t data_off = rd32(fo);
        unsigned nblk = rd16(fo + 4), typecomp = rd16(fo + 6), ctype = typecomp & 0x0F;
        if (ctype == 2) { cab_err(err, errlen, "Quantum sıkıştırması henüz desteklenmiyor"); return -1; }
        if (ctype > 3) { cab_err(err, errlen, "cab: unknown compression type"); return -1; }
        if (data_off > len) { cab_err(err, errlen, "cab: bad folder data offset"); return -1; }
        uint8_t *buf = NULL;
        size_t blen = 0;
        if (ctype == 3 ? decode_lzx_folder(cab, len, data_off, nblk, typecomp, cb_data_res, &buf, &blen, err, errlen)
                       : decode_folder(cab, len, data_off, nblk, ctype, cb_data_res, &buf, &blen, err, errlen)) return -1;
        q = coff_files;
        for (unsigned i = 0; i < nfiles; i++) {
            size_t fp = q;
            q += 16;
            size_t ns = q;
            skip_cstr(cab, len, &q);
            if (rd16(cab + fp + 8) != f) continue;
            size_t cbf = rd32(cab + fp), off = rd32(cab + fp + 4);
            unsigned attr = rd16(cab + fp + 14);
            if (off > blen || cbf > blen - off) {
                free(buf);
                cab_err(err, errlen, "cab: file extends beyond folder data");
                return -1;
            }
            size_t nl = q - 1 - ns;
            char *name;
            if (attr & 0x80) { name = malloc(nl + 1); if (name) { memcpy(name, cab + ns, nl); name[nl] = 0; } }
            else name = latin1_to_utf8(cab + ns, nl);
            if (!name) { free(buf); cab_err(err, errlen, "cab: out of memory"); return -1; }
            int stop = fn ? fn(ctx, name, buf + off, cbf, attr) : 0;
            free(name);
            if (stop) { free(buf); return 0; }
        }
        free(buf);
    }
    return 0;
}
