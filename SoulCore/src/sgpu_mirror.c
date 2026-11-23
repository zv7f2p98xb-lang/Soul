// SoulCore - sgpu "mirror" debug backend (SOUL_GPU=mirror).
//
// Behaves like a GPU with its own memory, but rasterises with the CPU rasteriser:
//  * every render-target buffer (colour / depth / stencil, keyed by its memory pointer) gets a private
//    shadow copy, created on first use from the real memory;
//  * every texture is copied (keyed by SgTexture.id, refreshed only when .version changes) and the
//    CPU rasteriser samples the COPY (the texture struct's storage pointers are swapped for the duration
//    of the draw), so a missing sg_texture_touch() shows up as a stale texture;
//  * draws and clears go to the shadows only; shadow -> real memory happens in sync_to_cpu, real ->
//    shadow in cpu_wrote; so a missing sync / cpu_wrote call in a front end shows up as stale pixels;
//  * every N-th draw/clear is declined (-1) to exercise the CPU fallback path.
//
// Environment: SOUL_GPU_MIRROR_DECLINE=N (default 7, 0 = never decline),
//              SOUL_GPU_MIRROR_STRICT=1 abort on protocol violations (otherwise they are logged to stderr).
#include "sgpu.h"
#include "sgpu_program.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct MBuf {
    const void *key;
    size_t esz, n;       // element size and count held by the shadow
    void *shadow;
    int dirty;           // shadow newer than real memory
} MBuf;

typedef struct MTex {
    uint64_t id;
    uint32_t version;
    int w, h, depth, kind, levels;
    uint8_t *rgba;
    uint8_t *mip[SG_MAX_MIPS];
} MTex;

static struct {
    int inited, decline, strict;
    unsigned long counter;
    MBuf *bufs; int nbufs, capbufs;
    MTex *texs; int ntexs, captexs;
} M;

static void violation(const char *msg) {
    fprintf(stderr, "[sgpu-mirror] protocol violation: %s\n", msg);
    if (M.strict) abort();
}

// ---- buffers ----------------------------------------------------------------------------------------------------------

static MBuf *buf_find(const void *key) {
    for (int i = 0; i < M.nbufs; i++) if (M.bufs[i].key == key) return &M.bufs[i];
    return NULL;
}
static void buf_drop(const void *key) {
    if (!key) return;
    for (int i = 0; i < M.nbufs; i++) if (M.bufs[i].key == key) {
        free(M.bufs[i].shadow);
        M.bufs[i] = M.bufs[--M.nbufs];
        return;
    }
}
// Returns the shadow for `key` holding at least n elements, creating / growing it from real memory.
static MBuf *buf_get(const void *key, size_t esz, size_t n) {
    MBuf *b = buf_find(key);
    if (b && b->n >= n && b->esz == esz) return b;
    if (b) {
        if (b->dirty) memcpy((void *)(uintptr_t)key, b->shadow, b->n * b->esz);
        free(b->shadow);
    } else {
        if (M.nbufs == M.capbufs) {
            M.capbufs = M.capbufs ? M.capbufs * 2 : 16;
            M.bufs = realloc(M.bufs, (size_t)M.capbufs * sizeof *M.bufs);
        }
        b = &M.bufs[M.nbufs++];
    }
    b->key = key; b->esz = esz; b->n = n; b->dirty = 0;
    b->shadow = malloc(n * esz + 1);
    memcpy(b->shadow, key, n * esz);
    return b;
}

static void buf_sync(const void *key) {
    MBuf *b = key ? buf_find(key) : NULL;
    if (b && b->dirty) { memcpy((void *)(uintptr_t)key, b->shadow, b->n * b->esz); b->dirty = 0; }
}

static size_t tn(const SgTarget *t) { return (size_t)(t->w > 0 ? t->w : 0) * (size_t)(t->h > 0 ? t->h : 0); }

// Maps the buffers of t to their shadows (marking them dirty when `write`).
static void map_target(const SgTarget *t, SgTarget *o, int write) {
    *o = *t;
    size_t n = tn(t);
    if (t->color) { MBuf *b = buf_get(t->color, 4, n); o->color = b->shadow; if (write) b->dirty = 1; }
    if (t->depth) { MBuf *b = buf_get(t->depth, 4, n); o->depth = b->shadow; if (write) b->dirty = 1; }
    if (t->stencil) { MBuf *b = buf_get(t->stencil, 1, n); o->stencil = b->shadow; if (write) b->dirty = 1; }
}

// ---- textures ---------------------------------------------------------------------------------------------------------

static MTex *tex_find(uint64_t id) {
    for (int i = 0; i < M.ntexs; i++) if (M.texs[i].id == id) return &M.texs[i];
    return NULL;
}
static void tex_free_entry(MTex *e) {
    for (int l = 0; l < e->levels && l < SG_MAX_MIPS; l++) if (l > 0) free(e->mip[l]);
    free(e->rgba);
    e->rgba = NULL; e->levels = 0;
}
static size_t level_bytes(const SgTexture *t, int l) {
    int w = l == 0 || t->levels <= 1 ? t->w : t->mip_w[l], h = l == 0 || t->levels <= 1 ? t->h : t->mip_h[l];
    int d = 1;
    if (t->kind == SG_TEX_VOLUME) d = l == 0 || t->levels <= 1 ? (t->depth > 0 ? t->depth : 1) : (t->mip_d[l] > 0 ? t->mip_d[l] : 1);
    int f = t->kind == SG_TEX_CUBE ? 6 : 1;
    if (w < 0) w = 0;
    if (h < 0) h = 0;
    return (size_t)w * (size_t)h * (size_t)d * (size_t)f * 4;
}
static void tex_load(MTex *e, const SgTexture *t) {
    tex_free_entry(e);
    e->version = t->version; e->w = t->w; e->h = t->h; e->depth = t->depth; e->kind = t->kind; e->levels = t->levels > 1 ? t->levels : 1;
    if (e->levels > SG_MAX_MIPS) e->levels = SG_MAX_MIPS;
    for (int l = 0; l < e->levels; l++) {
        size_t n = level_bytes(t, l);
        const uint8_t *src = l == 0 ? t->rgba : t->mip[l];
        uint8_t *dst = malloc(n + 1);
        if (src) memcpy(dst, src, n); else memset(dst, 0, n);
        if (l == 0) e->rgba = dst; else e->mip[l] = dst;
    }
    e->mip[0] = e->rgba;
}
static MTex *tex_get(SgTexture *t) {
    uint64_t id = sg_texture_id(t);
    MTex *e = tex_find(id);
    if (!e) {
        if (M.ntexs == M.captexs) {
            M.captexs = M.captexs ? M.captexs * 2 : 16;
            M.texs = realloc(M.texs, (size_t)M.captexs * sizeof *M.texs);
        }
        e = &M.texs[M.ntexs++];
        memset(e, 0, sizeof *e);
        e->id = id;
        tex_load(e, t);
        return e;
    }
    if (e->version != t->version) tex_load(e, t);
    else if (e->w != t->w || e->h != t->h || e->kind != t->kind || e->depth != t->depth || e->levels != (t->levels > 1 ? t->levels : 1)) {
        violation("texture layout changed without sg_texture_touch()");
        tex_load(e, t);
    }
    return e;
}

typedef struct Swap { SgTexture *t; uint8_t *rgba; uint8_t *mip[SG_MAX_MIPS]; } Swap;

static int swap_in(Swap *sw, int n, SgTexture *t) {
    if (!t || !t->rgba) return n;
    for (int i = 0; i < n; i++) if (sw[i].t == t) return n;
    MTex *e = tex_get(t);
    sw[n].t = t; sw[n].rgba = t->rgba;
    memcpy(sw[n].mip, t->mip, sizeof t->mip);
    t->rgba = e->rgba;
    for (int l = 0; l < e->levels; l++) t->mip[l] = e->mip[l];
    return n + 1;
}
static void swap_out(Swap *sw, int n) {
    for (int i = n - 1; i >= 0; i--) { sw[i].t->rgba = sw[i].rgba; memcpy(sw[i].t->mip, sw[i].mip, sizeof sw[i].mip); }
}
static int collect_and_swap(Swap *sw, const SgState *s) {
    int n = 0;
    for (int u = 0; u < SG_UNITS; u++) if (s->tex_on[u]) n = swap_in(sw, n, s->tex[u]);
    if (s->program) for (int i = 0; i < SGP_MAX_SAMPLERS; i++) n = swap_in(sw, n, (SgTexture *)(uintptr_t)s->program->samplers[i].tex);
    return n;
}

// ---- hooks ------------------------------------------------------------------------------------------------------------

static int declined(void) {
    M.counter++;
    return M.decline > 0 && M.counter % (unsigned long)M.decline == 0;
}

static int mi_draw(void *self, SgTarget *t, const SgState *s, int prim, const SgVertex *v, int n) {
    (void)self;
    if (declined()) return -1;
    SgTarget st;
    map_target(t, &st, 1);
    Swap sw[SG_UNITS + SGP_MAX_SAMPLERS];
    int ns = collect_and_swap(sw, s);
    switch (prim) {
    case SG_PRIM_TRIANGLES: sg_cpu_triangles(&st, s, v, n); break;
    case SG_PRIM_LINES: sg_cpu_lines(&st, s, v, n); break;
    default: sg_cpu_points(&st, s, v, n); break;
    }
    swap_out(sw, ns);
    return 0;
}

static int mi_clear(void *self, SgTarget *t, const SgState *s, int flags, uint32_t argb, float z, int stencil) {
    (void)self;
    if (declined()) return -1;
    SgTarget st;
    map_target(t, &st, 1);
    sg_cpu_clear_ex(&st, s, flags, argb, z, (uint8_t)stencil);
    return 0;
}

static void mi_sync(void *self, SgTarget *t) {
    (void)self;
    buf_sync(t->color); buf_sync(t->depth); buf_sync(t->stencil);
}

static void refresh(const void *key, size_t esz, size_t n, int x, int y, int w, int h, int tw) {
    if (!key || !buf_find(key)) return;      // never used on the "GPU": nothing to refresh
    MBuf *b = buf_get(key, esz, n);
    if (w > 0 && h > 0 && tw > 0) {
        if (b->dirty) violation("partial cpu_wrote on a target that was not synced to the CPU first");
        for (int r = y; r < y + h; r++) {
            if (r < 0 || (size_t)(r + 1) * (size_t)tw > b->n) continue;
            int x0 = x < 0 ? 0 : x, x1 = x + w > tw ? tw : x + w;
            if (x0 < x1) memcpy((char *)b->shadow + ((size_t)r * (size_t)tw + (size_t)x0) * esz, (const char *)key + ((size_t)r * (size_t)tw + (size_t)x0) * esz, (size_t)(x1 - x0) * esz);
        }
    } else {
        memcpy(b->shadow, key, n * esz);
        b->dirty = 0;
    }
}

static void mi_cpu_wrote(void *self, SgTarget *t, int x, int y, int w, int h) {
    (void)self;
    size_t n = tn(t);
    refresh(t->color, 4, n, x, y, w, h, t->w);
    refresh(t->depth, 4, n, 0, 0, 0, 0, 0);
    refresh(t->stencil, 1, n, 0, 0, 0, 0, 0);
}

static void mi_target_gone(void *self, const void *color, const void *depth, const void *stencil) {
    (void)self;
    buf_drop(color); buf_drop(depth); buf_drop(stencil);
}

static void mi_texture_gone(void *self, SgTexture *t) {
    (void)self;
    for (int i = 0; i < M.ntexs; i++) if (M.texs[i].id == t->id) {
        tex_free_entry(&M.texs[i]);
        M.texs[i] = M.texs[--M.ntexs];
        return;
    }
}

static void mi_end_frame(void *self) { (void)self; }

static SgBackend g_mirror = { "mirror", NULL, mi_draw, mi_clear, mi_sync, mi_cpu_wrote, mi_target_gone, mi_texture_gone, mi_end_frame };

const SgBackend *sg_mirror_backend(void) {
    if (!M.inited) {
        M.inited = 1;
        const char *d = getenv("SOUL_GPU_MIRROR_DECLINE");
        M.decline = d && *d ? atoi(d) : 7;
        const char *st = getenv("SOUL_GPU_MIRROR_STRICT");
        M.strict = st && *st && *st != '0';
    }
    return &g_mirror;
}
