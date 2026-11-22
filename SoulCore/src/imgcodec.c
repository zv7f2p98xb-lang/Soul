// SoulCore - image codecs: BMP, PNG, GIF, JPEG, ICO/CUR decoders and BMP / PNG encoders.
#include "imgcodec.h"
#include "inflate.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>

#define MAX_PIXELS (1u << 25)

static inline uint32_t rd16(const uint8_t *p) { return p[0] | (p[1] << 8); }
static inline uint32_t rd32(const uint8_t *p) { return p[0] | (p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }
static inline uint32_t be32(const uint8_t *p) { return ((uint32_t)p[0] << 24) | (p[1] << 16) | (p[2] << 8) | p[3]; }
static inline uint32_t be16(const uint8_t *p) { return (p[0] << 8) | p[1]; }

static Img *img_new(int fmt, int w, int h, int nframes) {
    if (w <= 0 || h <= 0 || nframes <= 0 || (uint64_t)w * (uint64_t)h > MAX_PIXELS) return NULL;
    Img *im = calloc(1, sizeof *im);
    if (!im) return NULL;
    im->fmt = fmt; im->w = w; im->h = h; im->nframes = nframes;
    im->fr = calloc((size_t)nframes, sizeof(ImgFrame));
    if (!im->fr) { free(im); return NULL; }
    for (int i = 0; i < nframes; i++) {
        im->fr[i].px = calloc((size_t)w * (size_t)h, 4);
        if (!im->fr[i].px) { img_free(im); return NULL; }
    }
    return im;
}

void img_free(Img *im) {
    if (!im) return;
    if (im->fr) for (int i = 0; i < im->nframes; i++) free(im->fr[i].px);
    free(im->fr);
    free(im);
}

static void scan_alpha(Img *im) {
    im->has_alpha = 0;
    for (int f = 0; f < im->nframes && !im->has_alpha; f++)
        for (size_t i = 0; i < (size_t)im->w * (size_t)im->h; i++)
            if ((im->fr[f].px[i] >> 24) != 255) { im->has_alpha = 1; break; }
}

int img_detect(const uint8_t *d, size_t n) {
    if (n >= 2 && d[0] == 'B' && d[1] == 'M') return IMG_BMP;
    if (n >= 8 && memcmp(d, "\x89PNG\r\n\x1a\n", 8) == 0) return IMG_PNG;
    if (n >= 6 && memcmp(d, "GIF8", 4) == 0 && (d[4] == '7' || d[4] == '9') && d[5] == 'a') return IMG_GIF;
    if (n >= 3 && d[0] == 0xFF && d[1] == 0xD8 && d[2] == 0xFF) return IMG_JPEG;
    if (n >= 6 && d[0] == 0 && d[1] == 0 && (d[2] == 1 || d[2] == 2) && d[3] == 0 && d[4] != 0) return IMG_ICO;
    if (n >= 4 && ((memcmp(d, "II*\0", 4) == 0) || (memcmp(d, "MM\0*", 4) == 0))) return IMG_TIFF;
    return IMG_UNKNOWN;
}

// ---- BMP / DIB -----------------------------------------------------------------------------------------

static int mask_shift(uint32_t m, int *bits) {
    int s = 0;
    if (!m) { *bits = 0; return 0; }
    while (!(m & 1)) { m >>= 1; s++; }
    int b = 0;
    while (m & 1) { m >>= 1; b++; }
    *bits = b;
    return s;
}
static uint32_t mask_get(uint32_t v, uint32_t m, int shift, int bits) {
    if (!m) return 0;
    uint32_t x = (v & m) >> shift;
    if (bits >= 8) return x >> (bits - 8);
    uint32_t r = x << (8 - bits);
    for (int s = bits; s < 8; s <<= 1) r |= r >> s;
    return r & 255;
}

// RLE8 / RLE4 into an index buffer (top-down after flipping by the caller's row order).
static int rle_decode(const uint8_t *s, size_t n, int w, int h, int four, uint8_t *idx, int top_down) {
    size_t p = 0;
    int x = 0, y = 0;
    memset(idx, 0, (size_t)w * (size_t)h);
    #define PUT(v) do { if (x < w && y < h) idx[(size_t)(top_down ? y : h - 1 - y) * (size_t)w + (size_t)x] = (uint8_t)(v); x++; } while (0)
    while (p + 1 < n) {
        int c = s[p++], v = s[p++];
        if (c) {
            for (int i = 0; i < c; i++) PUT(four ? ((i & 1) ? (v & 15) : (v >> 4)) : v);
        } else if (v == 0) { x = 0; y++; }
        else if (v == 1) break;
        else if (v == 2) { if (p + 1 >= n) return 0; x += s[p]; y += s[p + 1]; p += 2; }
        else {
            int cnt = v;
            size_t bytes = four ? (size_t)(cnt + 1) / 2 : (size_t)cnt;
            if (p + bytes > n) return 0;
            for (int i = 0; i < cnt; i++) {
                int b = s[p + (four ? (size_t)(i / 2) : (size_t)i)];
                PUT(four ? ((i & 1) ? (b & 15) : (b >> 4)) : b);
            }
            p += (bytes + 1) & ~(size_t)1;
        }
        if (y > h + 1) return 0;
    }
    #undef PUT
    return 1;
}

// d: the DIB (BITMAPINFOHEADER ...). bits_off: offset of the pixels in d (0 = derive). ico: height is doubled
// and an AND mask follows the colour bits.
static Img *dib_decode(const uint8_t *d, size_t n, size_t bits_off, int ico) {
    if (n < 12) return NULL;
    uint32_t hs = rd32(d);
    int32_t w, h; int bpp, comp = 0; uint32_t used = 0, xp = 0, yp = 0;
    uint32_t mk[4] = { 0, 0, 0, 0 };
    if (hs == 12) { w = (int)rd16(d + 4); h = (int)rd16(d + 6); bpp = (int)rd16(d + 10); }
    else if (hs >= 40 && hs <= 256 && n >= hs) {
        w = (int32_t)rd32(d + 4); h = (int32_t)rd32(d + 8); bpp = (int)rd16(d + 14); comp = (int)rd32(d + 16);
        xp = rd32(d + 24); yp = rd32(d + 28); used = rd32(d + 32);
        if (hs >= 52) { mk[0] = rd32(d + 40); mk[1] = rd32(d + 44); mk[2] = rd32(d + 48); }
        if (hs >= 56) mk[3] = rd32(d + 52);
    } else return NULL;
    int top_down = h < 0;
    if (top_down) h = -h;
    if (ico) h /= 2;
    if (w <= 0 || h <= 0 || w > 32768 || h > 32768) return NULL;
    if (bpp != 1 && bpp != 4 && bpp != 8 && bpp != 16 && bpp != 24 && bpp != 32) return NULL;
    if (comp != 0 && comp != 1 && comp != 2 && comp != 3 && comp != 6) return NULL;
    if ((comp == 1 && bpp != 8) || (comp == 2 && bpp != 4)) return NULL;
    size_t pos = hs;
    if (hs == 40 && (comp == 3 || comp == 6)) {
        size_t nm = comp == 6 ? 4 : 3;
        if (n < pos + nm * 4) return NULL;
        for (size_t i = 0; i < nm; i++) mk[i] = rd32(d + pos + i * 4);
        pos += nm * 4;
    }
    uint32_t pal[256]; int pal_n = 0;
    if (bpp <= 8) {
        pal_n = used ? (int)used : (1 << bpp);
        if (pal_n > 256) pal_n = 256;
        size_t esz = hs == 12 ? 3 : 4;
        if (pos + (size_t)pal_n * esz > n) return NULL;
        for (int i = 0; i < pal_n; i++) {
            const uint8_t *e = d + pos + (size_t)i * esz;
            pal[i] = 0xFF000000u | ((uint32_t)e[2] << 16) | (e[1] << 8) | e[0];
        }
        pos += (size_t)pal_n * esz;
    }
    if (!bits_off) bits_off = pos;
    if (bits_off > n) return NULL;
    const uint8_t *bits = d + bits_off;
    size_t avail = n - bits_off;
    Img *im = img_new(ico ? IMG_ICO : IMG_BMP, w, h, 1);
    if (!im) return NULL;
    im->src_bpp = bpp;
    im->dpix = xp ? (float)xp * 0.0254f : 0; im->dpiy = yp ? (float)yp * 0.0254f : 0;
    uint32_t *out = im->fr[0].px;
    size_t stride = ((size_t)w * (size_t)bpp + 31) / 32 * 4;
    uint8_t *ridx = NULL;
    if (comp == 1 || comp == 2) {
        ridx = malloc((size_t)w * (size_t)h);
        if (!ridx || !rle_decode(bits, avail, w, h, comp == 2, ridx, 1)) { free(ridx); img_free(im); return NULL; }
        // rle_decode wrote top-down; honour the row order flag below
    } else if (stride * (size_t)h > avail) { img_free(im); return NULL; }
    int alpha_seen = 0;
    int rs = 0, gs = 0, bs = 0, as = 0, rb = 0, gb = 0, bb = 0, ab = 0;
    if (bpp == 16 || bpp == 32) {
        if (comp == 3 || comp == 6 || (hs >= 52 && (mk[0] | mk[1] | mk[2]))) {
            rs = mask_shift(mk[0], &rb); gs = mask_shift(mk[1], &gb); bs = mask_shift(mk[2], &bb);
            as = mask_shift(mk[3], &ab);
        } else if (bpp == 16) { mk[0] = 0x7C00; mk[1] = 0x3E0; mk[2] = 0x1F; mk[3] = 0; rs = 10; rb = 5; gs = 5; gb = 5; bs = 0; bb = 5; }
        else { mk[0] = 0xFF0000; mk[1] = 0xFF00; mk[2] = 0xFF; mk[3] = 0; rs = 16; rb = 8; gs = 8; gb = 8; bs = 0; bb = 8; }
    }
    for (int y = 0; y < h; y++) {
        int sy = top_down ? y : h - 1 - y;
        const uint8_t *row = ridx ? NULL : bits + stride * (size_t)sy;
        uint32_t *o = out + (size_t)y * (size_t)w;
        for (int x = 0; x < w; x++) {
            uint32_t px;
            switch (bpp) {
            case 1: px = pal[((row[x >> 3] >> (7 - (x & 7))) & 1) % (pal_n ? pal_n : 1)]; break;
            case 4: { int v = (x & 1) ? (row[x >> 1] & 15) : (row[x >> 1] >> 4); px = v < pal_n ? pal[v] : 0xFF000000u; break; }
            case 8: { int v = ridx ? ridx[(size_t)y * (size_t)w + (size_t)x] : row[x]; px = v < pal_n ? pal[v] : 0xFF000000u; break; }
            case 16: { uint32_t v = rd16(row + x * 2);
                px = 0xFF000000u | (mask_get(v, mk[0], rs, rb) << 16) | (mask_get(v, mk[1], gs, gb) << 8) | mask_get(v, mk[2], bs, bb);
                if (mk[3]) px = (px & 0xFFFFFF) | (mask_get(v, mk[3], as, ab) << 24);
                break; }
            case 24: px = 0xFF000000u | ((uint32_t)row[x * 3 + 2] << 16) | (row[x * 3 + 1] << 8) | row[x * 3]; break;
            default: { uint32_t v = rd32(row + x * 4);
                px = 0xFF000000u | (mask_get(v, mk[0], rs, rb) << 16) | (mask_get(v, mk[1], gs, gb) << 8) | mask_get(v, mk[2], bs, bb);
                if (mk[3] || ico) { uint32_t a = mk[3] ? mask_get(v, mk[3], as, ab) : (v >> 24); px = (px & 0xFFFFFF) | (a << 24); if (a) alpha_seen = 1; }
                break; }
            }
            o[x] = px;
        }
    }
    free(ridx);
    if (bpp <= 8) { im->indexed = 1; im->pal_n = pal_n; memcpy(im->pal, pal, sizeof(uint32_t) * (size_t)pal_n); }
    if (ico) {
        size_t cbytes = stride * (size_t)h;
        size_t ms = ((size_t)w + 31) / 32 * 4;
        int use_mask = bpp != 32 || !alpha_seen;
        if (bpp == 32 && !alpha_seen) for (size_t i = 0; i < (size_t)w * (size_t)h; i++) out[i] |= 0xFF000000u;
        if (use_mask && comp == 0 && bits_off + cbytes + ms * (size_t)h <= n) {
            const uint8_t *m = bits + cbytes;
            for (int y = 0; y < h; y++) {
                int sy = top_down ? y : h - 1 - y;
                for (int x = 0; x < w; x++)
                    if ((m[ms * (size_t)sy + (size_t)(x >> 3)] >> (7 - (x & 7))) & 1) out[(size_t)y * (size_t)w + (size_t)x] = 0;
            }
        }
    }
    scan_alpha(im);
    return im;
}

static Img *bmp_decode(const uint8_t *d, size_t n) {
    if (n < 26) return NULL;
    uint32_t off = rd32(d + 10);
    if (off < 14 || off > n) off = 0;
    return dib_decode(d + 14, n - 14, off ? off - 14 : 0, 0);
}

// ---- ICO / CUR -------------------------------------------------------------------------------------------

static Img *ico_decode(const uint8_t *d, size_t n) {
    if (n < 6) return NULL;
    int cnt = (int)rd16(d + 4);
    if (cnt <= 0 || n < 6 + (size_t)cnt * 16) return NULL;
    int best = -1; long bscore = -1;
    for (int i = 0; i < cnt; i++) {
        const uint8_t *e = d + 6 + (size_t)i * 16;
        uint32_t sz = rd32(e + 8), off = rd32(e + 12);
        if ((uint64_t)off + sz > n || sz < 12) continue;
        int w = e[0] ? e[0] : 256, h = e[1] ? e[1] : 256, bpp = (int)rd16(e + 6);
        long score = (long)w * h * 64 + bpp;
        if (score > bscore) { bscore = score; best = i; }
    }
    if (best < 0) return NULL;
    const uint8_t *e = d + 6 + (size_t)best * 16;
    uint32_t sz = rd32(e + 8), off = rd32(e + 12);
    Img *im;
    if (sz >= 8 && memcmp(d + off, "\x89PNG\r\n\x1a\n", 8) == 0) im = img_decode(d + off, sz);
    else im = dib_decode(d + off, sz, 0, 1);
    if (im) im->fmt = IMG_ICO;
    return im;
}

// ---- PNG -------------------------------------------------------------------------------------------------

typedef struct PngCtx {
    int w, h, depth, ctype, ch, bpp_bits;
    uint32_t pal[256]; int pal_n;
    int has_trns_key; uint32_t key[3];
    uint32_t *out;
} PngCtx;

static uint32_t png_px(const PngCtx *c, const uint8_t *row, int i) {
    int d = c->depth;
    switch (c->ctype) {
    case 0: {
        uint32_t v, k;
        if (d == 16) { v = row[i * 2]; k = be16(row + i * 2); }
        else { int per = 8 / d; v = (row[i / per] >> (8 - d * (i % per + 1))) & ((1u << d) - 1); k = v; if (d < 8) v = v * 255 / ((1u << d) - 1); }
        uint32_t a = (c->has_trns_key && k == c->key[0]) ? 0 : 255;
        return (a << 24) | (v << 16) | (v << 8) | v;
    }
    case 2: {
        uint32_t r, g, b, kr, kg, kb;
        if (d == 16) { r = row[i * 6]; g = row[i * 6 + 2]; b = row[i * 6 + 4]; kr = be16(row + i * 6); kg = be16(row + i * 6 + 2); kb = be16(row + i * 6 + 4); }
        else { r = kr = row[i * 3]; g = kg = row[i * 3 + 1]; b = kb = row[i * 3 + 2]; }
        uint32_t a = (c->has_trns_key && kr == c->key[0] && kg == c->key[1] && kb == c->key[2]) ? 0 : 255;
        return (a << 24) | (r << 16) | (g << 8) | b;
    }
    case 3: {
        int per = 8 / d;
        uint32_t v = (row[i / per] >> (8 - d * (i % per + 1))) & ((1u << d) - 1);
        return (int)v < c->pal_n ? c->pal[v] : 0xFF000000u;
    }
    case 4: {
        uint32_t v, a;
        if (d == 16) { v = row[i * 4]; a = row[i * 4 + 2]; } else { v = row[i * 2]; a = row[i * 2 + 1]; }
        return (a << 24) | (v << 16) | (v << 8) | v;
    }
    default: {
        if (d == 16) return ((uint32_t)row[i * 8 + 6] << 24) | ((uint32_t)row[i * 8] << 16) | ((uint32_t)row[i * 8 + 2] << 8) | row[i * 8 + 4];
        return ((uint32_t)row[i * 4 + 3] << 24) | ((uint32_t)row[i * 4] << 16) | ((uint32_t)row[i * 4 + 1] << 8) | row[i * 4 + 2];
    }
    }
}

static int png_unfilter(uint8_t *data, int rowbytes, int rows, int bpp) {
    for (int y = 0; y < rows; y++) {
        uint8_t *r = data + (size_t)y * (size_t)(rowbytes + 1);
        int ft = r[0];
        uint8_t *cur = r + 1;
        const uint8_t *prev = y ? data + (size_t)(y - 1) * (size_t)(rowbytes + 1) + 1 : NULL;
        if (ft > 4) return 0;
        for (int i = 0; i < rowbytes; i++) {
            int a = i >= bpp ? cur[i - bpp] : 0, b = prev ? prev[i] : 0, c = (prev && i >= bpp) ? prev[i - bpp] : 0;
            int v = cur[i];
            switch (ft) {
            case 1: v += a; break;
            case 2: v += b; break;
            case 3: v += (a + b) >> 1; break;
            case 4: { int p = a + b - c, pa = abs(p - a), pb = abs(p - b), pc = abs(p - c);
                v += (pa <= pb && pa <= pc) ? a : (pb <= pc ? b : c); break; }
            }
            cur[i] = (uint8_t)v;
        }
    }
    return 1;
}

static Img *png_decode(const uint8_t *d, size_t n) {
    PngCtx c; memset(&c, 0, sizeof c);
    size_t p = 8;
    uint8_t *idat = NULL; size_t idat_len = 0;
    int seen_ihdr = 0, interlace = 0;
    float dpix = 0, dpiy = 0;
    uint8_t trns[256]; int trns_n = 0;
    int ended = 0;
    while (p + 12 <= n) {
        uint32_t len = be32(d + p);
        const uint8_t *t = d + p + 4;
        if (len > n - p - 12) break;
        const uint8_t *body = d + p + 8;
        if (!memcmp(t, "IHDR", 4)) {
            if (len < 13) goto bad;
            c.w = (int)be32(body); c.h = (int)be32(body + 4); c.depth = body[8]; c.ctype = body[9]; interlace = body[12];
            if (c.w <= 0 || c.h <= 0 || (uint64_t)c.w * (uint64_t)c.h > MAX_PIXELS) goto bad;
            if (body[10] != 0 || body[11] != 0 || interlace > 1) goto bad;
            switch (c.ctype) { case 0: c.ch = 1; break; case 2: c.ch = 3; break; case 3: c.ch = 1; break; case 4: c.ch = 2; break; case 6: c.ch = 4; break; default: goto bad; }
            if (c.depth != 1 && c.depth != 2 && c.depth != 4 && c.depth != 8 && c.depth != 16) goto bad;
            if ((c.ctype == 2 || c.ctype == 4 || c.ctype == 6) && c.depth < 8) goto bad;
            if (c.ctype == 3 && c.depth == 16) goto bad;
            c.bpp_bits = c.depth * c.ch;
            seen_ihdr = 1;
        } else if (!memcmp(t, "PLTE", 4)) {
            c.pal_n = (int)(len / 3); if (c.pal_n > 256) c.pal_n = 256;
            for (int i = 0; i < c.pal_n; i++) c.pal[i] = 0xFF000000u | ((uint32_t)body[i * 3] << 16) | (body[i * 3 + 1] << 8) | body[i * 3 + 2];
        } else if (!memcmp(t, "tRNS", 4)) {
            trns_n = len > 256 ? 256 : (int)len; memcpy(trns, body, (size_t)trns_n);
        } else if (!memcmp(t, "pHYs", 4) && len >= 9 && body[8] == 1) {
            dpix = (float)be32(body) * 0.0254f; dpiy = (float)be32(body + 4) * 0.0254f;
        } else if (!memcmp(t, "IDAT", 4)) {
            uint8_t *nb = realloc(idat, idat_len + len + 1);
            if (!nb) goto bad;
            idat = nb; memcpy(idat + idat_len, body, len); idat_len += len;
        } else if (!memcmp(t, "IEND", 4)) { ended = 1; break; }
        p += 12 + (size_t)len;
    }
    (void)ended;
    if (!seen_ihdr || !idat_len) goto bad;
    if (c.ctype == 3 && !c.pal_n) goto bad;
    if (c.ctype == 3) for (int i = 0; i < trns_n && i < c.pal_n; i++) c.pal[i] = (c.pal[i] & 0xFFFFFF) | ((uint32_t)trns[i] << 24);
    if (c.ctype == 0 && trns_n >= 2) { c.has_trns_key = 1; c.key[0] = be16(trns); }
    if (c.ctype == 2 && trns_n >= 6) { c.has_trns_key = 1; c.key[0] = be16(trns); c.key[1] = be16(trns + 2); c.key[2] = be16(trns + 4); }
    {
        static const int ax[7] = { 0, 4, 0, 2, 0, 1, 0 }, ay[7] = { 0, 0, 4, 0, 2, 0, 1 };
        static const int dx[7] = { 8, 8, 4, 4, 2, 2, 1 }, dy[7] = { 8, 8, 8, 4, 4, 2, 2 };
        size_t total = 0;
        int passes = interlace ? 7 : 1;
        for (int ps = 0; ps < passes; ps++) {
            int pw = interlace ? (c.w - ax[ps] + dx[ps] - 1) / dx[ps] : c.w;
            int ph = interlace ? (c.h - ay[ps] + dy[ps] - 1) / dy[ps] : c.h;
            if (pw <= 0 || ph <= 0) continue;
            total += (size_t)ph * ((((size_t)pw * (size_t)c.bpp_bits) + 7) / 8 + 1);
        }
        uint8_t *raw = malloc(total + 1);
        if (!raw) goto bad;
        long got = soul_zlib_inflate(idat, idat_len, raw, total);
        if (got < 0 || (size_t)got < total) { free(raw); goto bad; }
        Img *im = img_new(IMG_PNG, c.w, c.h, 1);
        if (!im) { free(raw); goto bad; }
        uint32_t *out = im->fr[0].px;
        size_t off = 0;
        int bpp = (c.bpp_bits + 7) / 8;
        for (int ps = 0; ps < passes; ps++) {
            int pw = interlace ? (c.w - ax[ps] + dx[ps] - 1) / dx[ps] : c.w;
            int ph = interlace ? (c.h - ay[ps] + dy[ps] - 1) / dy[ps] : c.h;
            if (pw <= 0 || ph <= 0) continue;
            int rb = (int)((((size_t)pw * (size_t)c.bpp_bits) + 7) / 8);
            if (!png_unfilter(raw + off, rb, ph, bpp)) { free(raw); img_free(im); goto bad; }
            for (int y = 0; y < ph; y++) {
                const uint8_t *row = raw + off + (size_t)y * (size_t)(rb + 1) + 1;
                int oy = interlace ? ay[ps] + y * dy[ps] : y;
                for (int x = 0; x < pw; x++) {
                    int ox = interlace ? ax[ps] + x * dx[ps] : x;
                    out[(size_t)oy * (size_t)c.w + (size_t)ox] = png_px(&c, row, x);
                }
            }
            off += (size_t)ph * (size_t)(rb + 1);
        }
        free(raw); free(idat);
        im->src_bpp = c.depth * (c.ctype == 3 || c.ctype == 0 ? 1 : c.ch);
        if (c.ctype == 2 || c.ctype == 6) im->src_bpp = c.depth * c.ch;
        if (c.ctype == 3) { im->indexed = 1; im->pal_n = c.pal_n; memcpy(im->pal, c.pal, sizeof c.pal); im->src_bpp = c.depth; }
        if (c.ctype == 0 || c.ctype == 4) im->gray = 1;
        im->dpix = dpix; im->dpiy = dpiy;
        scan_alpha(im);
        return im;
    }
bad:
    free(idat);
    return NULL;
}

// ---- encoders --------------------------------------------------------------------------------------------

static uint32_t crc_table[256];
static void crc_init(void) {
    if (crc_table[1]) return;
    for (uint32_t i = 0; i < 256; i++) { uint32_t c = i; for (int k = 0; k < 8; k++) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1; crc_table[i] = c; }
}
static uint32_t crc32_update(uint32_t crc, const uint8_t *p, size_t n) {
    crc_init();
    crc = ~crc;
    while (n--) crc = crc_table[(crc ^ *p++) & 255] ^ (crc >> 8);
    return ~crc;
}
static void wbe32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v; }
static void wle32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24); }
static void wle16(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }

static size_t png_chunk(uint8_t *o, const char *type, const uint8_t *data, size_t len) {
    wbe32(o, (uint32_t)len);
    memcpy(o + 4, type, 4);
    if (len) memcpy(o + 8, data, len);
    wbe32(o + 8 + len, crc32_update(crc32_update(0, o + 4, 4 + len), NULL, 0));
    return len + 12;
}

uint8_t *img_encode_png(const uint32_t *px, int w, int h, int has_alpha, float dpix, float dpiy, size_t *outlen) {
    if (w <= 0 || h <= 0) return NULL;
    int ch = has_alpha ? 4 : 3;
    size_t rb = (size_t)w * (size_t)ch + 1, raw_len = rb * (size_t)h;
    uint8_t *raw = malloc(raw_len);
    if (!raw) return NULL;
    for (int y = 0; y < h; y++) {
        uint8_t *r = raw + rb * (size_t)y;
        *r++ = 0;
        for (int x = 0; x < w; x++) {
            uint32_t v = px[(size_t)y * (size_t)w + (size_t)x];
            *r++ = (uint8_t)(v >> 16); *r++ = (uint8_t)(v >> 8); *r++ = (uint8_t)v;
            if (has_alpha) *r++ = (uint8_t)(v >> 24);
        }
    }
    // zlib stream of stored blocks
    size_t nblocks = (raw_len + 65534) / 65535;
    if (!nblocks) nblocks = 1;
    size_t zlen = 2 + raw_len + nblocks * 5 + 4;
    uint8_t *z = malloc(zlen);
    uint8_t *out = malloc(zlen + 128);
    if (!z || !out) { free(raw); free(z); free(out); return NULL; }
    size_t zp = 0;
    z[zp++] = 0x78; z[zp++] = 0x01;
    uint32_t s1 = 1, s2 = 0;
    for (size_t i = 0; i < raw_len; i++) { s1 = (s1 + raw[i]) % 65521; s2 = (s2 + s1) % 65521; }
    size_t pos = 0;
    do {
        size_t chunk = raw_len - pos > 65535 ? 65535 : raw_len - pos;
        z[zp++] = (pos + chunk >= raw_len) ? 1 : 0;
        z[zp++] = (uint8_t)chunk; z[zp++] = (uint8_t)(chunk >> 8);
        z[zp++] = (uint8_t)~chunk; z[zp++] = (uint8_t)(~chunk >> 8);
        memcpy(z + zp, raw + pos, chunk); zp += chunk; pos += chunk;
    } while (pos < raw_len);
    wbe32(z + zp, (s2 << 16) | s1); zp += 4;
    size_t o = 0;
    memcpy(out, "\x89PNG\r\n\x1a\n", 8); o = 8;
    uint8_t ihdr[13];
    wbe32(ihdr, (uint32_t)w); wbe32(ihdr + 4, (uint32_t)h); ihdr[8] = 8; ihdr[9] = has_alpha ? 6 : 2; ihdr[10] = ihdr[11] = ihdr[12] = 0;
    o += png_chunk(out + o, "IHDR", ihdr, 13);
    if (dpix > 0 && dpiy > 0) {
        uint8_t ph[9]; wbe32(ph, (uint32_t)(dpix / 0.0254f + 0.5f)); wbe32(ph + 4, (uint32_t)(dpiy / 0.0254f + 0.5f)); ph[8] = 1;
        o += png_chunk(out + o, "pHYs", ph, 9);
    }
    // IDAT is written after growing the buffer
    uint8_t *big = realloc(out, o + zp + 64);
    if (!big) { free(raw); free(z); free(out); return NULL; }
    out = big;
    o += png_chunk(out + o, "IDAT", z, zp);
    o += png_chunk(out + o, "IEND", NULL, 0);
    free(raw); free(z);
    *outlen = o;
    return out;
}

uint8_t *img_encode_bmp(const uint32_t *px, int w, int h, int has_alpha, float dpix, float dpiy, size_t *outlen) {
    if (w <= 0 || h <= 0) return NULL;
    int bpp = has_alpha ? 32 : 24;
    size_t stride = ((size_t)w * (size_t)bpp + 31) / 32 * 4;
    uint32_t hs = has_alpha ? 108 : 40;
    size_t off = 14 + hs, total = off + stride * (size_t)h;
    uint8_t *o = calloc(1, total);
    if (!o) return NULL;
    o[0] = 'B'; o[1] = 'M'; wle32(o + 2, (uint32_t)total); wle32(o + 10, (uint32_t)off);
    uint8_t *b = o + 14;
    wle32(b, hs); wle32(b + 4, (uint32_t)w); wle32(b + 8, (uint32_t)h); wle16(b + 12, 1); wle16(b + 14, (uint32_t)bpp);
    wle32(b + 16, has_alpha ? 3 : 0); wle32(b + 20, (uint32_t)(stride * (size_t)h));
    wle32(b + 24, (uint32_t)(dpix > 0 ? dpix / 0.0254f + 0.5f : 2835)); wle32(b + 28, (uint32_t)(dpiy > 0 ? dpiy / 0.0254f + 0.5f : 2835));
    if (has_alpha) { wle32(b + 40, 0xFF0000); wle32(b + 44, 0xFF00); wle32(b + 48, 0xFF); wle32(b + 52, 0xFF000000u); memcpy(b + 56, "BGRs", 4); }
    for (int y = 0; y < h; y++) {
        uint8_t *r = o + off + stride * (size_t)(h - 1 - y);
        for (int x = 0; x < w; x++) {
            uint32_t v = px[(size_t)y * (size_t)w + (size_t)x];
            if (has_alpha) wle32(r + x * 4, v);
            else { r[x * 3] = (uint8_t)v; r[x * 3 + 1] = (uint8_t)(v >> 8); r[x * 3 + 2] = (uint8_t)(v >> 16); }
        }
    }
    *outlen = total;
    return o;
}

// ---- GIF -------------------------------------------------------------------------------------------------

static int gif_skip_blocks(const uint8_t *d, size_t n, size_t *p) {
    while (*p < n) {
        size_t l = d[(*p)++];
        if (!l) return 1;
        if (*p + l > n) return 0;
        *p += l;
    }
    return 0;
}

// LZW data of one image: returns 1 when all w*h indices were produced (or the stream ended cleanly).
static int gif_lzw(const uint8_t *d, size_t n, size_t *pp, int minbits, uint8_t *idx, size_t total) {
    if (minbits < 1 || minbits > 11) return 0;
    uint16_t prefix[4096]; uint8_t suffix[4096], stack[4097];
    int clear = 1 << minbits, eoi = clear + 1, next = eoi + 1, size = minbits + 1, prev = -1;
    uint32_t acc = 0; int nbits = 0;
    size_t out = 0, p = *pp;
    size_t blk_left = 0;
    int done = 0;
    for (int i = 0; i < clear; i++) { prefix[i] = 0xFFFF; suffix[i] = (uint8_t)i; }
    while (!done) {
        while (nbits < size) {
            if (!blk_left) {
                if (p >= n) { done = 2; break; }
                blk_left = d[p++];
                if (!blk_left) { done = 2; break; }
            }
            if (p >= n) { done = 2; break; }
            acc |= (uint32_t)d[p++] << nbits; nbits += 8; blk_left--;
        }
        if (done) break;
        int code = (int)(acc & ((1u << size) - 1));
        acc >>= size; nbits -= size;
        if (code == clear) { next = eoi + 1; size = minbits + 1; prev = -1; continue; }
        if (code == eoi) { done = 1; break; }
        int cur = code, sp = 0;
        if (prev < 0) {
            if (code >= clear) return 0;
        } else if (code >= next) {
            if (code > next) return 0;
            cur = prev; // KwKwK
        }
        {
            int c2 = cur;
            while (c2 >= 0 && c2 != 0xFFFF && sp < 4096) { stack[sp++] = suffix[c2]; c2 = prefix[c2] == 0xFFFF ? -1 : prefix[c2]; }
            if (sp >= 4096) return 0;
        }
        uint8_t first = stack[sp - 1];
        if (code >= next && prev >= 0) { // KwKwK: append first char
            for (int i = sp; i > 0; i--) stack[i] = stack[i - 1];
            stack[0] = first; sp++;
        }
        while (sp > 0 && out < total) idx[out++] = stack[--sp];
        if (prev >= 0 && next < 4096) {
            prefix[next] = (uint16_t)prev; suffix[next] = first; next++;
            if (next == (1 << size) && size < 12) size++;
        }
        prev = code;
        if (out >= total) { done = 3; }
    }
    // skip the remaining sub-blocks
    if (done == 3 || done == 1) {
        while (blk_left && p < n) { p += blk_left > n - p ? n - p : blk_left; blk_left = 0; }
        size_t q = p;
        if (!gif_skip_blocks(d, n, &q)) q = n;
        p = q;
    }
    *pp = p;
    return out >= total || (out > 0 && done == 1);
}

static Img *gif_decode(const uint8_t *d, size_t n) {
    if (n < 13) return NULL;
    int sw = (int)rd16(d + 6), sh = (int)rd16(d + 8), fl = d[10];
    if (sw <= 0 || sh <= 0 || (uint64_t)sw * (uint64_t)sh > MAX_PIXELS) return NULL;
    size_t p = 13;
    uint32_t gct[256]; int gct_n = 0;
    if (fl & 0x80) {
        gct_n = 2 << (fl & 7);
        if (p + (size_t)gct_n * 3 > n) return NULL;
        for (int i = 0; i < gct_n; i++) gct[i] = 0xFF000000u | ((uint32_t)d[p + i * 3] << 16) | (d[p + i * 3 + 1] << 8) | d[p + i * 3 + 2];
        p += (size_t)gct_n * 3;
    }
    Img *im = calloc(1, sizeof *im);
    if (!im) return NULL;
    im->fmt = IMG_GIF; im->w = sw; im->h = sh; im->src_bpp = 8; im->indexed = 1;
    size_t npx = (size_t)sw * (size_t)sh;
    uint32_t *canvas = calloc(npx, 4), *saved = NULL;
    uint8_t *idx = malloc(npx);
    if (!canvas || !idx) goto fail;
    int gc_disp = 0, gc_trans = -1, gc_delay = 0, have_gc = 0;
    while (p < n) {
        int b = d[p++];
        if (b == 0x3B) break;
        if (b == 0x21) {
            if (p >= n) break;
            int lab = d[p++];
            if (lab == 0xF9 && p + 5 < n && d[p] >= 4) {
                gc_disp = (d[p + 1] >> 2) & 7; gc_trans = (d[p + 1] & 1) ? d[p + 4] : -1; gc_delay = (int)rd16(d + p + 2); have_gc = 1;
            } else if (lab == 0xFF && p + 15 < n && d[p] == 11 && !memcmp(d + p + 1, "NETSCAPE2.0", 11) && d[p + 12] == 3) im->loop = (int)rd16(d + p + 14);
            if (!gif_skip_blocks(d, n, &p)) break;
            continue;
        }
        if (b != 0x2C) break;
        if (p + 9 > n) break;
        int fx = (int)rd16(d + p), fy = (int)rd16(d + p + 2), fw = (int)rd16(d + p + 4), fh = (int)rd16(d + p + 6), ff = d[p + 8];
        p += 9;
        uint32_t lct[256]; const uint32_t *pal = gct; int pn = gct_n;
        if (ff & 0x80) {
            pn = 2 << (ff & 7);
            if (p + (size_t)pn * 3 > n) break;
            for (int i = 0; i < pn; i++) lct[i] = 0xFF000000u | ((uint32_t)d[p + i * 3] << 16) | (d[p + i * 3 + 1] << 8) | d[p + i * 3 + 2];
            p += (size_t)pn * 3; pal = lct;
        }
        if (p >= n) break;
        int minbits = d[p++];
        if (fw <= 0 || fh <= 0 || (uint64_t)fw * (uint64_t)fh > MAX_PIXELS) break;
        uint8_t *fi = malloc((size_t)fw * (size_t)fh);
        if (!fi) goto fail;
        memset(fi, 0, (size_t)fw * (size_t)fh);
        int ok = gif_lzw(d, n, &p, minbits, fi, (size_t)fw * (size_t)fh);
        if (!ok) { free(fi); break; }
        if (gc_disp == 3) { if (!saved) saved = malloc(npx * 4); if (saved) memcpy(saved, canvas, npx * 4); }
        int rows_seen = 0; (void)rows_seen;
        int pass = 0, row = 0, step = 8, start = 0;
        for (int y = 0; y < fh; y++) {
            int dy;
            if (ff & 0x40) {
                dy = row;
                row += step;
                if (row >= fh) { pass++; if (pass == 1) { row = 4; step = 8; } else if (pass == 2) { row = 2; step = 4; } else if (pass == 3) { row = 1; step = 2; } }
                (void)start;
            } else dy = y;
            if (dy >= fh) continue;
            for (int x = 0; x < fw; x++) {
                int cx = fx + x, cy = fy + dy;
                if (cx >= sw || cy >= sh) continue;
                int v = fi[(size_t)y * (size_t)fw + (size_t)x];
                if (v == gc_trans) continue;
                canvas[(size_t)cy * (size_t)sw + (size_t)cx] = v < pn ? pal[v] : 0xFF000000u;
            }
        }
        free(fi);
        ImgFrame *nf = realloc(im->fr, sizeof(ImgFrame) * (size_t)(im->nframes + 1));
        if (!nf) goto fail;
        im->fr = nf;
        nf[im->nframes].px = malloc(npx * 4);
        if (!nf[im->nframes].px) goto fail;
        memcpy(nf[im->nframes].px, canvas, npx * 4);
        nf[im->nframes].delay_cs = have_gc ? gc_delay : 0;
        if (im->nframes == 0) { im->pal_n = pn; memcpy(im->pal, pal, sizeof(uint32_t) * (size_t)pn); }
        im->nframes++;
        if (gc_disp == 2) for (int y = 0; y < fh; y++) for (int x = 0; x < fw; x++) {
            int cx = fx + x, cy = fy + y;
            if (cx < sw && cy < sh) canvas[(size_t)cy * (size_t)sw + (size_t)cx] = 0;
        }
        else if (gc_disp == 3 && saved) memcpy(canvas, saved, npx * 4);
        gc_disp = 0; gc_trans = -1; gc_delay = 0; have_gc = 0;
    }
    free(canvas); free(saved); free(idx);
    if (im->nframes == 0) { img_free(im); return NULL; }
    scan_alpha(im);
    return im;
fail:
    free(canvas); free(saved); free(idx);
    img_free(im);
    return NULL;
}

// ---- JPEG ------------------------------------------------------------------------------------------------

typedef struct JHuff { int mincode[17], maxcode[18], valptr[17]; uint8_t vals[256]; int valid; } JHuff;
typedef struct JComp { int id, h, v, tq, td, ta, bw, bh, cw, ch; int16_t *coef; int pred; uint8_t *plane; } JComp;
typedef struct Jpg {
    const uint8_t *d; size_t n, pos;
    int w, h, nc, prog, restart, hmax, vmax, mcux, mcuy, adobe, adobe_t, got_sof;
    JComp c[4];
    uint16_t qt[4][64];
    JHuff hdc[4], hac[4];
    uint32_t bitbuf; int bitcnt, marker, eobrun, eof;
    float dpix, dpiy;
} Jpg;

static const uint8_t zz[64] = {
    0, 1, 8, 16, 9, 2, 3, 10, 17, 24, 32, 25, 18, 11, 4, 5, 12, 19, 26, 33, 40, 48, 41, 34, 27, 20, 13, 6, 7, 14, 21, 28,
    35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23, 30, 37, 44, 51, 58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63 };

static int j_bit(Jpg *j) {
    if (j->bitcnt == 0) {
        int b = 0;
        if (j->marker || j->pos >= j->n) { b = 0; if (j->pos >= j->n) j->eof++; }
        else {
            b = j->d[j->pos];
            if (b == 0xFF) {
                int nx = j->pos + 1 < j->n ? j->d[j->pos + 1] : 0xD9;
                if (nx == 0) j->pos += 2;
                else if (nx == 0xFF) { j->pos++; return j_bit(j); }
                else { j->marker = nx; b = 0; }
            } else j->pos++;
        }
        j->bitbuf = (uint32_t)b; j->bitcnt = 8;
    }
    j->bitcnt--;
    return (int)((j->bitbuf >> j->bitcnt) & 1);
}
static int j_recv(Jpg *j, int n) { int v = 0; while (n--) v = (v << 1) | j_bit(j); return v; }
static int j_ext(int v, int n) { return v < (1 << (n - 1)) ? v - (1 << n) + 1 : v; }
static int j_dec(Jpg *j, const JHuff *h) {
    int code = 0;
    for (int l = 1; l <= 16; l++) {
        code = (code << 1) | j_bit(j);
        if (h->maxcode[l] >= 0 && code <= h->maxcode[l] && code >= h->mincode[l]) return h->vals[h->valptr[l] + code - h->mincode[l]];
    }
    return -1;
}

static void j_build(JHuff *h, const uint8_t *counts, const uint8_t *vals, int nv) {
    int code = 0, k = 0;
    memset(h->vals, 0, sizeof h->vals);
    memcpy(h->vals, vals, (size_t)(nv > 256 ? 256 : nv));
    for (int l = 1; l <= 16; l++) {
        h->valptr[l] = k; h->mincode[l] = code;
        code += counts[l - 1]; k += counts[l - 1];
        h->maxcode[l] = counts[l - 1] ? code - 1 : -1;
        code <<= 1;
    }
    h->valid = 1;
}

static int j_block(Jpg *j, JComp *c, int16_t *b, int ss, int se, int ah, int al) {
    if (!j->prog) {
        const JHuff *dc = &j->hdc[c->td], *ac = &j->hac[c->ta];
        if (!dc->valid || !ac->valid) return 0;
        int t = j_dec(j, dc);
        if (t < 0 || t > 16) return 0;
        int diff = t ? j_ext(j_recv(j, t), t) : 0;
        c->pred += diff; b[0] = (int16_t)c->pred;
        for (int k = 1; k < 64;) {
            int rs = j_dec(j, ac);
            if (rs < 0) return 0;
            int r = rs >> 4, s = rs & 15;
            if (!s) { if (r == 15) { k += 16; continue; } break; }
            k += r;
            if (k > 63) return 0;
            b[zz[k]] = (int16_t)j_ext(j_recv(j, s), s);
            k++;
        }
        return 1;
    }
    if (ss == 0) {
        if (ah == 0) {
            const JHuff *dc = &j->hdc[c->td];
            if (!dc->valid) return 0;
            int t = j_dec(j, dc);
            if (t < 0 || t > 16) return 0;
            int diff = t ? j_ext(j_recv(j, t), t) : 0;
            c->pred += diff; b[0] = (int16_t)(c->pred * (1 << al));
        } else if (j_bit(j)) b[0] |= (int16_t)(1 << al);
        return 1;
    }
    const JHuff *ac = &j->hac[c->ta];
    if (!ac->valid) return 0;
    if (ah == 0) {
        if (j->eobrun > 0) { j->eobrun--; return 1; }
        for (int k = ss; k <= se;) {
            int rs = j_dec(j, ac);
            if (rs < 0) return 0;
            int r = rs >> 4, s = rs & 15;
            if (!s) {
                if (r < 15) { j->eobrun = (1 << r) - 1; if (r) j->eobrun += j_recv(j, r); break; }
                k += 16; continue;
            }
            k += r;
            if (k > 63) return 0;
            b[zz[k]] = (int16_t)(j_ext(j_recv(j, s), s) * (1 << al));
            k++;
        }
        return 1;
    }
    int p1 = 1 << al, m1 = -(1 << al), k = ss;
    if (j->eobrun == 0) {
        for (; k <= se; k++) {
            int rs = j_dec(j, ac);
            if (rs < 0) return 0;
            int r = rs >> 4, s = rs & 15;
            if (s) s = j_bit(j) ? p1 : m1;
            else if (r != 15) { j->eobrun = 1 << r; if (r) j->eobrun += j_recv(j, r); break; }
            do {
                int16_t *t = &b[zz[k]];
                if (*t != 0) { if (j_bit(j) && (*t & p1) == 0) *t = (int16_t)(*t + (*t >= 0 ? p1 : m1)); }
                else if (--r < 0) break;
                k++;
            } while (k <= se);
            if (s && k <= se) b[zz[k]] = (int16_t)s;
        }
    }
    if (j->eobrun > 0) {
        for (; k <= se; k++) {
            int16_t *t = &b[zz[k]];
            if (*t != 0 && j_bit(j) && (*t & p1) == 0) *t = (int16_t)(*t + (*t >= 0 ? p1 : m1));
        }
        j->eobrun--;
    }
    return 1;
}

static void j_restart(Jpg *j, int ns, JComp **cs) {
    j->bitcnt = 0;
    size_t p = j->pos;
    int guard = 0;
    while (p + 1 < j->n && guard++ < 64 && !(j->d[p] == 0xFF && j->d[p + 1] >= 0xD0 && j->d[p + 1] <= 0xD7)) p++;
    if (p + 1 < j->n && j->d[p] == 0xFF && j->d[p + 1] >= 0xD0 && j->d[p + 1] <= 0xD7) j->pos = p + 2;
    j->marker = 0; j->eobrun = 0;
    for (int i = 0; i < ns; i++) cs[i]->pred = 0;
}

static int j_scan(Jpg *j, int ns, JComp **cs, int ss, int se, int ah, int al) {
    j->bitcnt = 0; j->marker = 0; j->eobrun = 0;
    for (int i = 0; i < ns; i++) cs[i]->pred = 0;
    int count = 0;
    if (ns == 1) {
        JComp *c = cs[0];
        for (int by = 0; by < c->ch; by++)
            for (int bx = 0; bx < c->cw; bx++) {
                if (j->restart && count && count % j->restart == 0) j_restart(j, ns, cs);
                if (!j_block(j, c, c->coef + ((size_t)by * (size_t)c->bw + (size_t)bx) * 64, ss, se, ah, al)) return 0;
                count++;
                if (j->eof > 64) return 0;
            }
        return 1;
    }
    for (int my = 0; my < j->mcuy; my++)
        for (int mx = 0; mx < j->mcux; mx++) {
            if (j->restart && count && count % j->restart == 0) j_restart(j, ns, cs);
            for (int i = 0; i < ns; i++) {
                JComp *c = cs[i];
                for (int v = 0; v < c->v; v++)
                    for (int h = 0; h < c->h; h++) {
                        int bx = mx * c->h + h, by = my * c->v + v;
                        if (!j_block(j, c, c->coef + ((size_t)by * (size_t)c->bw + (size_t)bx) * 64, ss, se, ah, al)) return 0;
                    }
            }
            count++;
            if (j->eof > 64) return 0;
        }
    return 1;
}

static float idct_cos[8][8];
static void idct_block(const int16_t *in, const uint16_t *q, uint8_t *out, int stride) {
    float tmp[64], blk[64];
    if (!idct_cos[0][0]) for (int x = 0; x < 8; x++) for (int u = 0; u < 8; u++) idct_cos[x][u] = (float)(cos((2 * x + 1) * u * M_PI / 16.0) * (u ? 1.0 : M_SQRT1_2) * 0.5);
    for (int i = 0; i < 64; i++) blk[zz[0] * 0 + i] = 0;
    for (int k = 0; k < 64; k++) blk[k] = (float)in[k] * (float)q[k]; // q is stored in natural order
    for (int y = 0; y < 8; y++) // rows
        for (int x = 0; x < 8; x++) { float s = 0; for (int u = 0; u < 8; u++) s += idct_cos[x][u] * blk[y * 8 + u]; tmp[y * 8 + x] = s; }
    for (int x = 0; x < 8; x++)
        for (int y = 0; y < 8; y++) {
            float s = 0;
            for (int v = 0; v < 8; v++) s += idct_cos[y][v] * tmp[v * 8 + x];
            int val = (int)lrintf(s + 128.0f);
            out[y * stride + x] = (uint8_t)(val < 0 ? 0 : val > 255 ? 255 : val);
        }
}

static void jpg_free(Jpg *j) { for (int i = 0; i < 4; i++) { free(j->c[i].coef); free(j->c[i].plane); } }
static inline uint8_t clamp8(int v) { return (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v); }

static Img *jpeg_decode(const uint8_t *d, size_t n) {
    Jpg *j = calloc(1, sizeof *j);
    Img *im = NULL;
    if (!j) return NULL;
    j->d = d; j->n = n; j->pos = 2; j->adobe_t = -1;
    int scans = 0;
    while (j->pos + 4 <= n) {
        if (d[j->pos] != 0xFF) { j->pos++; continue; }
        int m = d[j->pos + 1];
        if (m == 0xFF) { j->pos++; continue; }
        if (m == 0 || (m >= 0xD0 && m <= 0xD8) || m == 0x01) { j->pos += 2; continue; }
        if (m == 0xD9) break;
        size_t len = be16(d + j->pos + 2);
        if (len < 2 || j->pos + 2 + len > n) goto bad;
        const uint8_t *s = d + j->pos + 4;
        size_t sl = len - 2;
        j->pos += 2 + len;
        switch (m) {
        case 0xDB: {
            size_t i = 0;
            while (i < sl) {
                int pq = s[i] >> 4, tq = s[i] & 15; i++;
                if (tq > 3 || i + (pq ? 128u : 64u) > sl) goto bad;
                for (int k = 0; k < 64; k++) { j->qt[tq][zz[k]] = (uint16_t)(pq ? be16(s + i + 2 * (size_t)k) : s[i + (size_t)k]); }
                i += pq ? 128 : 64;
            }
            break; }
        case 0xC4: {
            size_t i = 0;
            while (i + 17 <= sl) {
                int tc = s[i] >> 4, th = s[i] & 15;
                int tot = 0;
                for (int k = 0; k < 16; k++) tot += s[i + 1 + (size_t)k];
                if (th > 3 || tc > 1 || tot > 256 || i + 17 + (size_t)tot > sl) goto bad;
                j_build(tc ? &j->hac[th] : &j->hdc[th], s + i + 1, s + i + 17, tot);
                i += 17 + (size_t)tot;
            }
            break; }
        case 0xDD: if (sl >= 2) j->restart = (int)be16(s); break;
        case 0xEE: if (sl >= 12 && !memcmp(s, "Adobe", 5)) { j->adobe = 1; j->adobe_t = s[11]; } break;
        case 0xE0: if (sl >= 14 && !memcmp(s, "JFIF", 4) && s[7] != 0) {
            float xd = (float)be16(s + 8), yd = (float)be16(s + 10);
            if (s[7] == 1) { j->dpix = xd; j->dpiy = yd; } else if (s[7] == 2) { j->dpix = xd * 2.54f; j->dpiy = yd * 2.54f; }
            } break;
        case 0xC0: case 0xC1: case 0xC2: {
            if (j->got_sof || sl < 6 || s[0] != 8) goto bad;
            j->prog = m == 0xC2;
            j->h = (int)be16(s + 1); j->w = (int)be16(s + 3); j->nc = s[5];
            if (j->w <= 0 || j->h <= 0 || (uint64_t)j->w * (uint64_t)j->h > MAX_PIXELS) goto bad;
            if ((j->nc != 1 && j->nc != 3 && j->nc != 4) || sl < 6 + (size_t)j->nc * 3) goto bad;
            for (int i = 0; i < j->nc; i++) {
                JComp *c = &j->c[i];
                c->id = s[6 + i * 3]; c->h = s[7 + i * 3] >> 4; c->v = s[7 + i * 3] & 15; c->tq = s[8 + i * 3];
                if (c->h < 1 || c->h > 4 || c->v < 1 || c->v > 4 || c->tq > 3) goto bad;
                if (c->h > j->hmax) j->hmax = c->h;
                if (c->v > j->vmax) j->vmax = c->v;
            }
            if (j->nc == 1) { j->c[0].h = j->c[0].v = 1; j->hmax = j->vmax = 1; }
            j->mcux = (j->w + 8 * j->hmax - 1) / (8 * j->hmax); j->mcuy = (j->h + 8 * j->vmax - 1) / (8 * j->vmax);
            for (int i = 0; i < j->nc; i++) {
                JComp *c = &j->c[i];
                c->bw = j->mcux * c->h; c->bh = j->mcuy * c->v;
                int pw = (j->w * c->h + j->hmax - 1) / j->hmax, ph = (j->h * c->v + j->vmax - 1) / j->vmax;
                c->cw = (pw + 7) / 8; c->ch = (ph + 7) / 8;
                c->coef = calloc((size_t)c->bw * (size_t)c->bh * 64, sizeof(int16_t));
                if (!c->coef) goto bad;
            }
            j->got_sof = 1;
            break; }
        case 0xC3: case 0xC5: case 0xC6: case 0xC7: case 0xC9: case 0xCA: case 0xCB: case 0xCD: case 0xCE: case 0xCF: goto bad; // lossless / arithmetic
        case 0xDA: {
            if (!j->got_sof || sl < 1) goto bad;
            int ns = s[0];
            if (ns < 1 || ns > 4 || sl < 1 + (size_t)ns * 2 + 3) goto bad;
            JComp *cs[4];
            for (int i = 0; i < ns; i++) {
                int id = s[1 + i * 2], found = -1;
                for (int k = 0; k < j->nc; k++) if (j->c[k].id == id) found = k;
                if (found < 0) goto bad;
                cs[i] = &j->c[found];
                cs[i]->td = s[2 + i * 2] >> 4; cs[i]->ta = s[2 + i * 2] & 15;
                if (cs[i]->td > 3 || cs[i]->ta > 3) goto bad;
            }
            int ss = s[1 + ns * 2], se = s[2 + ns * 2], ah = s[3 + ns * 2] >> 4, al = s[3 + ns * 2] & 15;
            if (!j->prog) { ss = 0; se = 63; ah = al = 0; }
            if (ss > se || se > 63 || al > 13) goto bad;
            if (!j_scan(j, ns, cs, ss, se, ah, al)) { if (!scans) goto bad; } // later scans may be truncated
            scans++;
            // continue after the entropy data: next marker
            if (j->marker) { j->pos -= 0; }
            break; }
        default: break;
        }
    }
    if (!j->got_sof || !scans) goto bad;
    {
        int W = j->w, H = j->h;
        for (int i = 0; i < j->nc; i++) {
            JComp *c = &j->c[i];
            int pw = c->bw * 8, ph = c->bh * 8;
            c->plane = malloc((size_t)pw * (size_t)ph);
            if (!c->plane) goto bad;
            for (int by = 0; by < c->bh; by++) for (int bx = 0; bx < c->bw; bx++)
                idct_block(c->coef + ((size_t)by * (size_t)c->bw + (size_t)bx) * 64, j->qt[c->tq], c->plane + (size_t)by * 8 * (size_t)pw + (size_t)bx * 8, pw);
        }
        im = img_new(IMG_JPEG, W, H, 1);
        if (!im) goto bad;
        uint8_t *chan[4];
        for (int i = 0; i < 4; i++) chan[i] = NULL;
        for (int i = 0; i < j->nc; i++) {
            JComp *c = &j->c[i];
            chan[i] = malloc((size_t)W * (size_t)H);
            if (!chan[i]) goto bad;
            int pw = c->bw * 8;
            int cwp = (W * c->h + j->hmax - 1) / j->hmax, chp = (H * c->v + j->vmax - 1) / j->vmax;
            float sx = (float)j->hmax / (float)c->h, sy = (float)j->vmax / (float)c->v;
            for (int y = 0; y < H; y++) {
                float fy = ((float)y + 0.5f) / sy - 0.5f;
                int y0 = (int)floorf(fy); float wy = fy - (float)y0;
                int y1 = y0 + 1;
                if (y0 < 0) y0 = 0;
                if (y1 < 0) y1 = 0;
                if (y0 >= chp) y0 = chp - 1;
                if (y1 >= chp) y1 = chp - 1;
                for (int x = 0; x < W; x++) {
                    if (sx == 1.0f && sy == 1.0f) { chan[i][(size_t)y * (size_t)W + (size_t)x] = c->plane[(size_t)y * (size_t)pw + (size_t)x]; continue; }
                    float fx = ((float)x + 0.5f) / sx - 0.5f;
                    int x0 = (int)floorf(fx); float wx = fx - (float)x0;
                    int x1 = x0 + 1;
                    if (x0 < 0) x0 = 0;
                    if (x1 < 0) x1 = 0;
                    if (x0 >= cwp) x0 = cwp - 1;
                    if (x1 >= cwp) x1 = cwp - 1;
                    float a = c->plane[(size_t)y0 * (size_t)pw + (size_t)x0] * (1 - wx) + c->plane[(size_t)y0 * (size_t)pw + (size_t)x1] * wx;
                    float b = c->plane[(size_t)y1 * (size_t)pw + (size_t)x0] * (1 - wx) + c->plane[(size_t)y1 * (size_t)pw + (size_t)x1] * wx;
                    chan[i][(size_t)y * (size_t)W + (size_t)x] = (uint8_t)lrintf(a * (1 - wy) + b * wy);
                }
            }
        }
        int ycc = j->nc == 3 ? !(j->adobe && j->adobe_t == 0) && !(j->c[0].id == 'R' && j->c[1].id == 'G' && j->c[2].id == 'B') : (j->nc == 4 && j->adobe && j->adobe_t == 2);
        uint32_t *out = im->fr[0].px;
        for (size_t i = 0; i < (size_t)W * (size_t)H; i++) {
            int r, g, b;
            if (j->nc == 1) { r = g = b = chan[0][i]; }
            else {
                int a0 = chan[0][i], a1 = chan[1][i], a2 = chan[2][i];
                if (ycc) { r = clamp8((int)lrintf(a0 + 1.402f * (a2 - 128))); g = clamp8((int)lrintf(a0 - 0.344136f * (a1 - 128) - 0.714136f * (a2 - 128))); b = clamp8((int)lrintf(a0 + 1.772f * (a1 - 128))); }
                else { r = a0; g = a1; b = a2; }
                if (j->nc == 4) {
                    int k = chan[3][i];
                    if (j->adobe) {
                        if (ycc) { r = 255 - r; g = 255 - g; b = 255 - b; } // YCCK: libjpeg's C = 255 - R
                        r = r * k / 255; g = g * k / 255; b = b * k / 255; // stored values are inverted inks
                        if (ycc) { /* r,g,b hold C*K/255 with C = 255-R_ycc: already the result */ }
                    } else { r = (255 - r) * (255 - k) / 255; g = (255 - g) * (255 - k) / 255; b = (255 - b) * (255 - k) / 255; }
                }
            }
            out[i] = 0xFF000000u | ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
        }
        for (int i = 0; i < 4; i++) free(chan[i]);
        im->src_bpp = j->nc == 1 ? 8 : 24; im->gray = j->nc == 1;
        im->dpix = j->dpix; im->dpiy = j->dpiy;
    }
    jpg_free(j); free(j);
    return im;
bad:
    jpg_free(j); free(j);
    img_free(im);
    return NULL;
}

// ---- dispatch --------------------------------------------------------------------------------------------

Img *img_decode(const uint8_t *d, size_t n) {
    if (!d) return NULL;
    switch (img_detect(d, n)) {
    case IMG_BMP: return bmp_decode(d, n);
    case IMG_PNG: return png_decode(d, n);
    case IMG_GIF: return gif_decode(d, n);
    case IMG_JPEG: return jpeg_decode(d, n);
    case IMG_ICO: return ico_decode(d, n);
    default: return NULL;
    }
}
