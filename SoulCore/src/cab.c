// SoulCore - Microsoft Cabinet (MS-CAB) extractor: stored and MSZIP folders. Pure libc + inflate.h.
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
        unsigned nblk = rd16(fo + 4), ctype = rd16(fo + 6) & 0x0F;
        if (ctype == 3) { cab_err(err, errlen, "LZX sıkıştırması henüz desteklenmiyor"); return -1; }
        if (ctype == 2) { cab_err(err, errlen, "Quantum sıkıştırması henüz desteklenmiyor"); return -1; }
        if (ctype > 3) { cab_err(err, errlen, "cab: unknown compression type"); return -1; }
        if (data_off > len) { cab_err(err, errlen, "cab: bad folder data offset"); return -1; }
        uint8_t *buf = NULL;
        size_t blen = 0;
        if (decode_folder(cab, len, data_off, nblk, ctype, cb_data_res, &buf, &blen, err, errlen)) return -1;
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
