// SoulCore - DEFLATE decoder: stored, fixed and dynamic Huffman blocks, table-driven decoding.
#include "inflate.h"
#include <string.h>

#define FASTBITS 10
#define MAXBITS 15

typedef struct Huff {
    uint16_t count[MAXBITS + 1];
    uint16_t symbol[288];
    uint16_t fast[1 << FASTBITS]; // (len << 12) | symbol, indexed by the next FASTBITS stream bits; 0 = longer code
} Huff;

typedef struct Dec {
    const uint8_t *in;
    size_t len, pos;
    uint64_t buf;
    int cnt;
    int err;
    uint8_t *out;
    size_t cap, op;
    const uint8_t *dict;
    size_t dict_len;
} Dec;

static void refill(Dec *d) {
    while (d->cnt <= 56 && d->pos < d->len) { d->buf |= (uint64_t)d->in[d->pos++] << d->cnt; d->cnt += 8; }
}
static uint32_t getbits(Dec *d, int n) {
    if (n == 0) return 0;
    if (d->cnt < n) refill(d);
    if (d->cnt < n) { d->err = 1; return 0; }
    uint32_t v = (uint32_t)(d->buf & ((1ull << n) - 1));
    d->buf >>= n;
    d->cnt -= n;
    return v;
}

// Builds a canonical Huffman decoder. Returns 0 on success, -1 for an over-subscribed or
// (unless it has at most one symbol) incomplete code.
static int huff_build(Huff *h, const uint8_t *lens, int n) {
    memset(h->count, 0, sizeof h->count);
    memset(h->fast, 0, sizeof h->fast);
    for (int i = 0; i < n; i++) h->count[lens[i]]++;
    int used = n - h->count[0];
    int left = 1;
    for (int l = 1; l <= MAXBITS; l++) {
        left <<= 1;
        left -= h->count[l];
        if (left < 0) return -1;
    }
    if (left > 0 && used > 1) return -1;
    uint16_t offs[MAXBITS + 2];
    offs[1] = 0;
    for (int l = 1; l <= MAXBITS; l++) offs[l + 1] = (uint16_t)(offs[l] + h->count[l]);
    uint16_t next[MAXBITS + 2];
    memcpy(next, offs, sizeof next);
    for (int i = 0; i < n; i++) if (lens[i]) h->symbol[next[lens[i]]++] = (uint16_t)i;
    // fast table
    unsigned code = 0;
    int idx = 0;
    for (int l = 1; l <= FASTBITS; l++) {
        for (int k = 0; k < h->count[l]; k++, idx++, code++) {
            unsigned rev = 0;
            for (int b = 0; b < l; b++) if (code & (1u << b)) rev |= 1u << (l - 1 - b);
            uint16_t ent = (uint16_t)((l << 12) | h->symbol[idx]);
            for (unsigned j = rev; j < (1u << FASTBITS); j += 1u << l) h->fast[j] = ent;
        }
        code <<= 1;
    }
    return 0;
}

static int huff_decode(Dec *d, const Huff *h) {
    if (d->cnt < MAXBITS) refill(d);
    uint16_t e = h->fast[d->buf & ((1u << FASTBITS) - 1)];
    if (e) {
        int l = e >> 12;
        if (l > d->cnt) { d->err = 1; return -1; }
        d->buf >>= l;
        d->cnt -= l;
        return e & 0xFFF;
    }
    int code = 0, first = 0, index = 0;
    uint64_t b = d->buf;
    for (int l = 1; l <= MAXBITS; l++) {
        if (l > d->cnt) { d->err = 1; return -1; }
        code |= (int)(b & 1);
        b >>= 1;
        int count = h->count[l];
        if (code - count < first) {
            d->buf >>= l;
            d->cnt -= l;
            return h->symbol[index + (code - first)];
        }
        index += count;
        first += count;
        first <<= 1;
        code <<= 1;
    }
    d->err = 1;
    return -1;
}

static const uint16_t k_lbase[29] = { 3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258 };
static const uint8_t k_lext[29] = { 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0 };
static const uint16_t k_dbase[30] = { 1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577 };
static const uint8_t k_dext[30] = { 0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13 };

static int codes(Dec *d, const Huff *lit, const Huff *dist) {
    for (;;) {
        int sym = huff_decode(d, lit);
        if (sym < 0) return -1;
        if (sym < 256) {
            if (d->op >= d->cap) return -1;
            d->out[d->op++] = (uint8_t)sym;
        } else if (sym == 256) {
            return 0;
        } else {
            sym -= 257;
            if (sym >= 29) return -1;
            size_t len = (size_t)k_lbase[sym] + getbits(d, k_lext[sym]);
            int ds = huff_decode(d, dist);
            if (ds < 0 || ds >= 30) return -1;
            size_t dd = (size_t)k_dbase[ds] + getbits(d, k_dext[ds]);
            if (d->err) return -1;
            if (dd > d->op + d->dict_len) return -1;
            if (len > d->cap - d->op) return -1;
            uint8_t *o = d->out + d->op;
            if (dd > d->op) { // starts inside the preset dictionary
                size_t from_dict = dd - d->op;
                const uint8_t *s = d->dict + d->dict_len - from_dict;
                size_t n = from_dict < len ? from_dict : len;
                for (size_t i = 0; i < n; i++) *o++ = s[i];
                len -= n;
                d->op += n;
                if (len) { const uint8_t *src = d->out; for (size_t i = 0; i < len; i++) *o++ = src[i]; }
                d->op += len;
                continue;
            }
            const uint8_t *s = o - dd;
            if (dd >= len) memcpy(o, s, len);
            else for (size_t i = 0; i < len; i++) o[i] = s[i];
            d->op += len;
        }
    }
}

static int stored_block(Dec *d) {
    d->buf >>= d->cnt & 7; // drop to a byte boundary
    d->cnt -= d->cnt & 7;
    uint32_t len = getbits(d, 16), nlen = getbits(d, 16);
    if (d->err || (len ^ 0xFFFF) != nlen) return -1;
    // bytes still in the bit buffer come first
    while (len && d->cnt >= 8) {
        if (d->op >= d->cap) return -1;
        d->out[d->op++] = (uint8_t)d->buf;
        d->buf >>= 8; d->cnt -= 8; len--;
    }
    if (!len) return 0;
    if (d->len - d->pos < len || d->cap - d->op < len) return -1;
    memcpy(d->out + d->op, d->in + d->pos, len);
    d->pos += len;
    d->op += len;
    return 0;
}

static int fixed_block(Dec *d) {
    static Huff lit, dist;
    static int ready;
    if (!ready) {
        uint8_t l[288];
        int i = 0;
        for (; i < 144; i++) l[i] = 8;
        for (; i < 256; i++) l[i] = 9;
        for (; i < 280; i++) l[i] = 7;
        for (; i < 288; i++) l[i] = 8;
        huff_build(&lit, l, 288);
        for (i = 0; i < 32; i++) l[i] = 5; // 30 and 31 are invalid but complete the code
        huff_build(&dist, l, 32);
        ready = 1;
    }
    return codes(d, &lit, &dist);
}

static int dynamic_block(Dec *d) {
    static const uint8_t order[19] = { 16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15 };
    int nlen = (int)getbits(d, 5) + 257, ndist = (int)getbits(d, 5) + 1, ncode = (int)getbits(d, 4) + 4;
    if (d->err || nlen > 286 || ndist > 30) return -1;
    uint8_t lens[320];
    memset(lens, 0, sizeof lens);
    for (int i = 0; i < ncode; i++) lens[order[i]] = (uint8_t)getbits(d, 3);
    if (d->err) return -1;
    Huff lencode;
    if (huff_build(&lencode, lens, 19) < 0) return -1;
    uint8_t ll[320];
    int idx = 0;
    while (idx < nlen + ndist) {
        int sym = huff_decode(d, &lencode);
        if (sym < 0) return -1;
        if (sym < 16) ll[idx++] = (uint8_t)sym;
        else {
            int rep, val = 0;
            if (sym == 16) {
                if (idx == 0) return -1;
                val = ll[idx - 1];
                rep = 3 + (int)getbits(d, 2);
            } else if (sym == 17) rep = 3 + (int)getbits(d, 3);
            else rep = 11 + (int)getbits(d, 7);
            if (d->err || idx + rep > nlen + ndist) return -1;
            while (rep--) ll[idx++] = (uint8_t)val;
        }
    }
    if (ll[256] == 0) return -1;
    Huff lit, dist;
    if (huff_build(&lit, ll, nlen) < 0) return -1;
    if (huff_build(&dist, ll + nlen, ndist) < 0) return -1;
    return codes(d, &lit, &dist);
}

long soul_inflate(const uint8_t *in, size_t in_len, uint8_t *out, size_t out_cap, const uint8_t *dict, size_t dict_len) {
    Dec d;
    memset(&d, 0, sizeof d);
    d.in = in; d.len = in_len; d.out = out; d.cap = out_cap;
    if (dict && dict_len) {
        if (dict_len > 32768) { dict += dict_len - 32768; dict_len = 32768; }
        d.dict = dict; d.dict_len = dict_len;
    }
    int last;
    do {
        last = (int)getbits(&d, 1);
        uint32_t type = getbits(&d, 2);
        if (d.err) return -1;
        int r;
        switch (type) {
        case 0: r = stored_block(&d); break;
        case 1: r = fixed_block(&d); break;
        case 2: r = dynamic_block(&d); break;
        default: return -1;
        }
        if (r < 0 || d.err) return -1;
    } while (!last);
    return (long)d.op;
}

long soul_zlib_inflate(const uint8_t *in, size_t in_len, uint8_t *out, size_t out_cap) {
    if (in_len < 2) return -1;
    unsigned cmf = in[0], flg = in[1];
    if ((cmf & 0x0F) != 8 || (cmf >> 4) > 7 || ((cmf << 8) | flg) % 31 != 0 || (flg & 0x20)) return -1;
    return soul_inflate(in + 2, in_len - 2, out, out_cap, NULL, 0);
}
