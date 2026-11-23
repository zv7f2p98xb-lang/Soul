// SoulCore - Direct3D 9 resources: surfaces, textures, volumes, vertex/index buffers, declarations,
// shaders and queries (objects, formats, locking, conversion to the sgpu texture representation).
#include "d3d9_priv.h"
#include <time.h>

#define SD_OK D3D_OK

// ---- locations in guest structures -------------------------------------------------------------------------
static int rd_rect(uint64_t p, int32_t *rc) {
    if (!p) return 0;
    for (int i = 0; i < 4; i++) rc[i] = (int32_t)mem_rd(p + 4 * (uint64_t)i, 4);
    return 1;
}
static void *gz(size_t n) { return g_calloc(n ? n : 1, 1); }

// ---- generic resource methods --------------------------------------------------------------------------------
static D9Obj *OB(X86State *st, int n) { return (D9Obj *)G2H(ARG(n)); }

static void r_GetDevice(X86State *st) {
    D9Obj *o = OB(st, 0);
    if (!ARG(1)) { RET(D3DERR_INVALIDCALL); return; }
    D9Dev *d = o->dev;
    if (!d) { wr_ptr(ARG(1), 0); RET(D3DERR_INVALIDCALL); return; }
    d9_addref(&d->o);
    wr_ptr(ARG(1), H2G(d));
    RET(D3D_OK);
}
static D9Priv *priv_find(D9Obj *o, const uint8_t *g) {
    for (D9Priv *p = o->priv; p; p = p->next) if (!memcmp(p->guid, g, 16)) return p;
    return NULL;
}
static void priv_free_one(D9Obj *o, D9Priv *p) {
    D9Priv **pp = &o->priv;
    while (*pp && *pp != p) pp = &(*pp)->next;
    if (*pp) *pp = p->next;
    free(p->data);
    free(p);
}
static void r_SetPrivateData(X86State *st) { // (this, REFGUID, void *data, DWORD size, DWORD flags)
    D9Obj *o = OB(st, 0);
    if (!ARG(1) || (!ARG(2) && ARG(3)) || ARG(3) > (64u << 20)) { RET(D3DERR_INVALIDCALL); return; }
    uint8_t g[16];
    memcpy(g, G2H(ARG(1)), 16);
    D9Priv *p = priv_find(o, g);
    if (p) priv_free_one(o, p);
    p = calloc(1, sizeof *p);
    memcpy(p->guid, g, 16);
    p->flags = (uint32_t)ARG(4);
    p->size = (uint32_t)ARG(3);
    p->data = malloc(p->size ? p->size : 1);
    if (p->size) memcpy(p->data, G2H(ARG(2)), p->size);
    p->next = o->priv;
    o->priv = p;
    RET(D3D_OK);
}
static void r_GetPrivateData(X86State *st) { // (this, REFGUID, void *data, DWORD *size)
    D9Obj *o = OB(st, 0);
    if (!ARG(1) || !ARG(3)) { RET(D3DERR_INVALIDCALL); return; }
    uint8_t g[16];
    memcpy(g, G2H(ARG(1)), 16);
    D9Priv *p = priv_find(o, g);
    if (!p) { RET(D3DERR_NOTFOUND); return; }
    uint32_t cap = d9_rd32(ARG(3));
    d9_wr32(ARG(3), p->size);
    if (!ARG(2) || cap < p->size) { RET(D3DERR_MOREDATA); return; }
    memcpy(G2H(ARG(2)), p->data, p->size);
    RET(D3D_OK);
}
static void r_FreePrivateData(X86State *st) {
    D9Obj *o = OB(st, 0);
    uint8_t g[16];
    if (!ARG(1)) { RET(D3DERR_INVALIDCALL); return; }
    memcpy(g, G2H(ARG(1)), 16);
    D9Priv *p = priv_find(o, g);
    if (!p) { RET(D3DERR_NOTFOUND); return; }
    priv_free_one(o, p);
    RET(D3D_OK);
}
static void r_SetPriority(X86State *st) { D9Obj *o = OB(st, 0); uint32_t old = o->priority; o->priority = (uint32_t)ARG(1); RET(old); }
static void r_GetPriority(X86State *st) { RET(OB(st, 0)->priority); }
static void r_PreLoad(X86State *st) { (void)st; }
static void r_GetType(X86State *st) {
    D9Obj *o = OB(st, 0);
    switch (o->type) {
    case OT_SURF: RET(RT_SURFACE); break;
    case OT_VOLUME: RET(RT_VOLUME); break;
    case OT_TEX: RET(o->kind == SG_TEX_CUBE ? RT_CUBETEXTURE : o->kind == SG_TEX_VOLUME ? RT_VOLUMETEXTURE : RT_TEXTURE); break;
    case OT_VB: RET(RT_VERTEXBUFFER); break;
    case OT_IB: RET(RT_INDEXBUFFER); break;
    default: RET(0);
    }
}
void d9_priv_free_all(D9Obj *o) { while (o->priv) priv_free_one(o, o->priv); }

// ---- surfaces ------------------------------------------------------------------------------------------------
D9Surf *d9_surf_create(D9Dev *dev, int w, int h, int fmt, int usage, int pool, int rt, int depth, D9Obj *container) {
    D9Surf *s = d9_new(OT_SURF, sizeof *s);
    if (!s) return NULL;
    d9_obj_setvt(&s->o, VT_SURF);
    s->o.dev = dev;
    s->o.container = container;
    if (!container) d9_hold(&dev->o);
    s->w = w; s->h = h; s->fmt = fmt; s->usage = usage; s->pool = pool;
    if (depth) {
        s->dw = w; s->dh = h; s->depth_fmt = fmt;
        s->depth = malloc((size_t)w * (size_t)h * sizeof(float));
        s->stencil = calloc((size_t)w * (size_t)h, 1);
        for (size_t i = 0; i < (size_t)w * (size_t)h; i++) s->depth[i] = 1.0f;
        return s;
    }
    s->pitch = (int)d9_level_pitch(fmt, w);
    size_t sz = (size_t)s->pitch * d9_level_rows(fmt, h);
    s->data = gz(sz);
    if (rt) {
        if (d9_fmt_is_argb32(fmt)) s->argb = (uint32_t *)(void *)s->data;
        else { s->argb = calloc((size_t)w * (size_t)h, 4); s->shadow = 1; }
    }
    return s;
}
// ---- backend (sgpu) synchronisation of render storage. A surface's `argb` / `depth` / `stencil` memory
// may be mirrored on a GPU backend: sync before the CPU reads (or partly writes) it, cpu_wrote after it wrote.
void d9_surf_sync(D9Surf *s) {
    if (!s->argb) return;
    SgTarget t = { 0 };
    t.color = s->argb; t.w = s->w; t.h = s->h;
    sg_target_sync(&t);
}
void d9_surf_cpu_wrote(D9Surf *s, const int32_t *rc) {
    if (!s->argb) return;
    SgTarget t = { 0 };
    t.color = s->argb; t.w = s->w; t.h = s->h;
    if (rc) sg_target_cpu_wrote(&t, rc[0], rc[1], rc[2] - rc[0], rc[3] - rc[1]);
    else sg_target_cpu_wrote(&t, 0, 0, 0, 0);
}
static void depth_target(D9Surf *s, SgTarget *t) {
    memset(t, 0, sizeof *t);
    t->depth = s->depth; t->stencil = s->stencil; t->w = s->dw; t->h = s->dh;
}
void d9_depth_sync(D9Surf *s) { if (!s->depth) return; SgTarget t; depth_target(s, &t); sg_target_sync(&t); }
void d9_depth_cpu_wrote(D9Surf *s) { if (!s->depth) return; SgTarget t; depth_target(s, &t); sg_target_cpu_wrote(&t, 0, 0, 0, 0); }

void d9_surf_free_storage(D9Surf *s) {
    sg_target_gone(s->argb, s->depth, s->stencil);
    sg_target_gone(NULL, s->sdepth, s->sstencil);
    if (s->shadow) free(s->argb);
    s->argb = NULL;
    g_free(s->data);
    s->data = NULL;
    free(s->depth); free(s->stencil); free(s->sdepth); free(s->sstencil);
    s->depth = NULL; s->stencil = NULL; s->sdepth = NULL; s->sstencil = NULL;
    free(s->dc_buf); s->dc_buf = NULL;
}
void d9_surf_touch(D9Surf *s) {
    s->modified = 1;
    if (s->shadow) s->native_stale = 1;
    if (s->tex) s->tex->dirty = 1;
}
void d9_surf_to_native(D9Surf *s) {
    if (!s->shadow || !s->native_stale) return;
    d9_surf_sync(s);
    for (int y = 0; y < s->h; y++) d9_pack_row(s->fmt, s->argb + (size_t)y * (size_t)s->w, s->data + (size_t)y * (size_t)s->pitch, s->w);
    s->native_stale = 0;
}
void d9_surf_from_native(D9Surf *s) {
    if (!s->shadow) return;
    for (int y = 0; y < s->h; y++) d9_unpack_row(s->fmt, s->data + (size_t)y * (size_t)s->pitch, s->argb + (size_t)y * (size_t)s->w, s->w, NULL);
    s->argb_stale = 0;
    d9_surf_cpu_wrote(s, NULL);
}
uint32_t *d9_surf_argb(D9Surf *s) { return s->argb; }

static const uint32_t *dev_pal(D9Surf *s) {
    if ((s->fmt == F_P8 || s->fmt == F_A8P8) && s->o.dev && s->o.dev->pal) return s->o.dev->pal[s->o.dev->cur_pal & 255];
    return NULL;
}
static int clip_rc(D9Surf *s, const int32_t *rc, int32_t *o) {
    if (rc) { memcpy(o, rc, 16); } else { o[0] = 0; o[1] = 0; o[2] = s->w; o[3] = s->h; }
    return o[0] >= 0 && o[1] >= 0 && o[2] <= s->w && o[3] <= s->h && o[0] < o[2] && o[1] < o[3];
}
int d9_surf_read_argb(D9Surf *s, const int32_t *rc, uint32_t *out) {
    int32_t r[4];
    if (!s->data && !s->argb) return 0;
    if (!clip_rc(s, rc, r)) return 0;
    int rw = r[2] - r[0], rh = r[3] - r[1];
    if (s->argb) {
        d9_surf_sync(s);
        for (int y = 0; y < rh; y++) memcpy(out + (size_t)y * (size_t)rw, s->argb + (size_t)(r[1] + y) * (size_t)s->w + (size_t)r[0], (size_t)rw * 4);
        return 1;
    }
    if (d9_fmt_is_dxt(s->fmt)) {
        uint32_t *full = malloc((size_t)s->w * (size_t)s->h * 4);
        d9_decode_dxt(s->fmt, s->data, s->w, s->h, (size_t)s->pitch, full);
        for (int y = 0; y < rh; y++) memcpy(out + (size_t)y * (size_t)rw, full + (size_t)(r[1] + y) * (size_t)s->w + (size_t)r[0], (size_t)rw * 4);
        free(full);
        return 1;
    }
    int bpp = d9_fmt_bytes(s->fmt);
    const uint32_t *pal = dev_pal(s);
    for (int y = 0; y < rh; y++) d9_unpack_row(s->fmt, s->data + (size_t)(r[1] + y) * (size_t)s->pitch + (size_t)r[0] * (size_t)bpp, out + (size_t)y * (size_t)rw, rw, pal);
    return 1;
}
int d9_surf_write_argb(D9Surf *s, const int32_t *rc, const uint32_t *in) {
    int32_t r[4];
    if (!clip_rc(s, rc, r)) return 0;
    int rw = r[2] - r[0], rh = r[3] - r[1];
    if (s->argb) {
        d9_surf_sync(s);
        for (int y = 0; y < rh; y++) memcpy(s->argb + (size_t)(r[1] + y) * (size_t)s->w + (size_t)r[0], in + (size_t)y * (size_t)rw, (size_t)rw * 4);
        d9_surf_cpu_wrote(s, r);
        d9_surf_touch(s);
        return 1;
    }
    if (!s->data || d9_fmt_is_dxt(s->fmt)) return 0;
    int bpp = d9_fmt_bytes(s->fmt);
    for (int y = 0; y < rh; y++)
        if (!d9_pack_row(s->fmt, in + (size_t)y * (size_t)rw, s->data + (size_t)(r[1] + y) * (size_t)s->pitch + (size_t)r[0] * (size_t)bpp, rw)) return 0;
    d9_surf_touch(s);
    return 1;
}

// Copies/scales src_rc of src into drc of dst (NULL = whole surface). filter: 2 = linear, else point.
uint32_t d9_surf_copy(D9Surf *dst, const int32_t *drc, D9Surf *src, const int32_t *src_rc, int filter) {
    int32_t dr[4], sr[4];
    if (!clip_rc(dst, drc, dr) || !clip_rc(src, src_rc, sr)) return D3DERR_INVALIDCALL;
    int dw = dr[2] - dr[0], dh = dr[3] - dr[1], sw = sr[2] - sr[0], sh = sr[3] - sr[1];
    // identical formats, no scaling, plain memory: copy rows
    if (dst->fmt == src->fmt && dw == sw && dh == sh && !dst->shadow && !src->shadow && dst->data && src->data) {
        d9_surf_sync(src); d9_surf_sync(dst);          // render storage (argb == data) may live on a GPU backend
        if (d9_fmt_is_dxt(dst->fmt)) {
            if ((dr[0] | dr[1] | sr[0] | sr[1]) & 3) return D3DERR_INVALIDCALL;
            size_t bs = dst->fmt == F_DXT1 ? 8 : 16;
            for (int y = 0; y < (dh + 3) / 4; y++)
                memcpy(dst->data + (size_t)(dr[1] / 4 + y) * (size_t)dst->pitch + (size_t)(dr[0] / 4) * bs,
                       src->data + (size_t)(sr[1] / 4 + y) * (size_t)src->pitch + (size_t)(sr[0] / 4) * bs, (size_t)((dw + 3) / 4) * bs);
        } else {
            size_t bpp = (size_t)d9_fmt_bytes(dst->fmt);
            for (int y = 0; y < dh; y++)
                memmove(dst->data + (size_t)(dr[1] + y) * (size_t)dst->pitch + (size_t)dr[0] * bpp, src->data + (size_t)(sr[1] + y) * (size_t)src->pitch + (size_t)sr[0] * bpp, (size_t)dw * bpp);
        }
        d9_surf_cpu_wrote(dst, dr);
        d9_surf_touch(dst);
        return D3D_OK;
    }
    uint32_t *sp = malloc((size_t)sw * (size_t)sh * 4);
    if (!d9_surf_read_argb(src, sr, sp)) { free(sp); return D3DERR_INVALIDCALL; }
    uint32_t *dp = sp;
    if (dw != sw || dh != sh) {
        dp = malloc((size_t)dw * (size_t)dh * 4);
        for (int y = 0; y < dh; y++) for (int x = 0; x < dw; x++) {
            float fx = ((float)x + 0.5f) * (float)sw / (float)dw - 0.5f, fy = ((float)y + 0.5f) * (float)sh / (float)dh - 0.5f;
            if (filter != 2) {
                int ix = (int)floorf(fx + 0.5f), iy = (int)floorf(fy + 0.5f);
                ix = ix < 0 ? 0 : ix >= sw ? sw - 1 : ix; iy = iy < 0 ? 0 : iy >= sh ? sh - 1 : iy;
                dp[(size_t)y * (size_t)dw + (size_t)x] = sp[(size_t)iy * (size_t)sw + (size_t)ix];
                continue;
            }
            int x0 = (int)floorf(fx), y0 = (int)floorf(fy);
            float ax = fx - (float)x0, ay = fy - (float)y0;
            int x1 = x0 + 1, y1 = y0 + 1;
            x0 = x0 < 0 ? 0 : x0 >= sw ? sw - 1 : x0; x1 = x1 < 0 ? 0 : x1 >= sw ? sw - 1 : x1;
            y0 = y0 < 0 ? 0 : y0 >= sh ? sh - 1 : y0; y1 = y1 < 0 ? 0 : y1 >= sh ? sh - 1 : y1;
            uint32_t p00 = sp[(size_t)y0 * (size_t)sw + (size_t)x0], p10 = sp[(size_t)y0 * (size_t)sw + (size_t)x1];
            uint32_t p01 = sp[(size_t)y1 * (size_t)sw + (size_t)x0], p11 = sp[(size_t)y1 * (size_t)sw + (size_t)x1];
            uint32_t o = 0;
            for (int sh_ = 0; sh_ < 32; sh_ += 8) {
                float c = (float)((p00 >> sh_) & 255) * (1 - ax) * (1 - ay) + (float)((p10 >> sh_) & 255) * ax * (1 - ay) +
                          (float)((p01 >> sh_) & 255) * (1 - ax) * ay + (float)((p11 >> sh_) & 255) * ax * ay;
                o |= (uint32_t)(c + 0.5f) << sh_;
            }
            dp[(size_t)y * (size_t)dw + (size_t)x] = o;
        }
    }
    int ok = d9_surf_write_argb(dst, dr, dp);
    if (dp != sp) free(dp);
    free(sp);
    return ok ? D3D_OK : D3DERR_INVALIDCALL;
}

static uint32_t surf_lock(D9Surf *s, uint64_t plr, uint64_t prect, uint32_t flags) {
    int32_t rc[4];
    if (!plr || s->depth) return D3DERR_INVALIDCALL;
    if (s->lock_count) return D3DERR_INVALIDCALL;
    int have = rd_rect(prect, rc);
    int32_t r[4];
    if (!clip_rc(s, have ? rc : NULL, r)) return D3DERR_INVALIDCALL;
    d9_surf_sync(s);
    if (s->shadow) d9_surf_to_native(s);
    size_t off;
    if (d9_fmt_is_dxt(s->fmt)) off = (size_t)(r[1] / 4) * (size_t)s->pitch + (size_t)(r[0] / 4) * (s->fmt == F_DXT1 ? 8 : 16);
    else off = (size_t)r[1] * (size_t)s->pitch + (size_t)r[0] * (size_t)d9_fmt_bytes(s->fmt);
    d9_wr32(plr, (uint32_t)s->pitch);
    wr_ptr(plr + (uint64_t)PSZ, H2G(s->data + off));
    s->lock_count = 1;
    s->lock_flags = (int)flags;
    if (!(flags & LOCK_READONLY)) d9_surf_touch(s);
    return D3D_OK;
}
static uint32_t surf_unlock(D9Surf *s) {
    if (!s->lock_count) return D3DERR_INVALIDCALL;
    s->lock_count = 0;
    if (!(s->lock_flags & LOCK_READONLY)) {
        if (s->shadow) { d9_surf_from_native(s); s->native_stale = 0; }
        else d9_surf_cpu_wrote(s, NULL);
        d9_surf_touch(s);
        s->native_stale = 0;
    }
    return D3D_OK;
}
static void surf_desc(D9Surf *s, uint64_t p) {
    d9_wr32(p, (uint32_t)(s->depth ? s->depth_fmt : s->fmt));
    d9_wr32(p + 4, RT_SURFACE);
    d9_wr32(p + 8, (uint32_t)s->usage);
    d9_wr32(p + 12, (uint32_t)s->pool);
    d9_wr32(p + 16, (uint32_t)s->ms);
    d9_wr32(p + 20, 0);
    d9_wr32(p + 24, (uint32_t)s->w);
    d9_wr32(p + 28, (uint32_t)s->h);
}
static void s_GetContainer(X86State *st) {
    D9Surf *s = D9OBJ(D9Surf, 0);
    if (!ARG(2)) { RET(D3DERR_INVALIDCALL); return; }
    D9Obj *c = s->o.container ? s->o.container : &s->o.dev->o;
    d9_addref(c);
    wr_ptr(ARG(2), H2G(c));
    RET(D3D_OK);
}
static void s_GetDesc(X86State *st) { D9Surf *s = D9OBJ(D9Surf, 0); if (!ARG(1)) { RET(D3DERR_INVALIDCALL); return; } surf_desc(s, ARG(1)); RET(D3D_OK); }
static void s_LockRect(X86State *st) { RET(surf_lock(D9OBJ(D9Surf, 0), ARG(1), ARG(2), (uint32_t)ARG(3))); }
static void s_UnlockRect(X86State *st) { RET(surf_unlock(D9OBJ(D9Surf, 0))); }
static void s_GetDC(X86State *st) {
    D9Surf *s = D9OBJ(D9Surf, 0);
    if (!ARG(1) || s->depth || s->dc_hdc) { RET(D3DERR_INVALIDCALL); return; }
    switch (s->fmt) { case F_X8R8G8B8: case F_A8R8G8B8: case F_R5G6B5: case F_X1R5G5B5: case F_A1R5G5B5: case F_R8G8B8: break; default: RET(D3DERR_INVALIDCALL); return; }
    d9_surf_sync(s);
    if (s->shadow) d9_surf_to_native(s);
    GdiObj *bm;
    uint64_t hbm = gdi_alloc(GO_BITMAP, &bm);
    if (!bm) { RET(D3DERR_OUTOFVIDEOMEMORY); return; }
    s->dc_buf = malloc((size_t)s->w * (size_t)s->h * 4);
    d9_surf_read_argb(s, NULL, s->dc_buf);
    for (size_t i = 0; i < (size_t)s->w * (size_t)s->h; i++) s->dc_buf[i] |= 0xFF000000u;
    bm->surf.w = s->w; bm->surf.h = s->h; bm->surf.bpp = 32; bm->surf.top_down = 1; bm->surf.stride = s->w * 4;
    bm->surf.bits = (uint8_t *)s->dc_buf; bm->owns_bits = 0;
    uint64_t hdc = gdi_create_dc(DC_MEMORY, 0);
    Dc *dc = dc_get(hdc);
    if (!dc) { gdi_free(hbm); free(s->dc_buf); s->dc_buf = NULL; RET(D3DERR_INVALIDCALL); return; }
    dc->bitmap = hbm;
    bm->selected = 1;
    s->dc_hdc = hdc; s->dc_bmp = hbm;
    wr_ptr(ARG(1), hdc);
    RET(D3D_OK);
}
static void s_ReleaseDC(X86State *st) {
    D9Surf *s = D9OBJ(D9Surf, 0);
    if (!s->dc_hdc || ARG(1) != s->dc_hdc) { RET(D3DERR_INVALIDCALL); return; }
    d9_surf_write_argb(s, NULL, s->dc_buf);
    GdiObj *bm = gdi_get(s->dc_bmp, GO_BITMAP);
    if (bm) { bm->selected = 0; bm->surf.bits = NULL; }
    gdi_release_dc(s->dc_hdc);
    gdi_free(s->dc_bmp);
    free(s->dc_buf);
    s->dc_buf = NULL; s->dc_hdc = s->dc_bmp = 0;
    RET(D3D_OK);
}

// ---- volumes ---------------------------------------------------------------------------------------------------
static void vol_desc(D9Vol *v, uint64_t p) {
    d9_wr32(p, (uint32_t)v->fmt); d9_wr32(p + 4, RT_VOLUME); d9_wr32(p + 8, (uint32_t)v->tex->usage); d9_wr32(p + 12, (uint32_t)v->tex->pool);
    d9_wr32(p + 16, (uint32_t)v->w); d9_wr32(p + 20, (uint32_t)v->h); d9_wr32(p + 24, (uint32_t)v->d);
}
static uint32_t vol_lock(D9Vol *v, uint64_t plb, uint64_t pbox, uint32_t flags) {
    if (!plb) return D3DERR_INVALIDCALL;
    int32_t b[6] = { 0, 0, v->w, v->h, 0, v->d };
    if (pbox) for (int i = 0; i < 6; i++) b[i] = (int32_t)mem_rd(pbox + 4 * (uint64_t)i, 4);
    if (b[0] < 0 || b[1] < 0 || b[4] < 0 || b[2] > v->w || b[3] > v->h || b[5] > v->d || b[0] >= b[2] || b[1] >= b[3] || b[4] >= b[5]) return D3DERR_INVALIDCALL;
    size_t off = (size_t)b[4] * (size_t)v->slicepitch;
    if (d9_fmt_is_dxt(v->fmt)) off += (size_t)(b[1] / 4) * (size_t)v->rowpitch + (size_t)(b[0] / 4) * (v->fmt == F_DXT1 ? 8 : 16);
    else off += (size_t)b[1] * (size_t)v->rowpitch + (size_t)b[0] * (size_t)d9_fmt_bytes(v->fmt);
    d9_wr32(plb, (uint32_t)v->rowpitch);
    d9_wr32(plb + 4, (uint32_t)v->slicepitch);
    wr_ptr(plb + 8, H2G(v->data + off));
    v->lock_count++;
    if (!(flags & LOCK_READONLY)) { v->modified = 1; v->tex->dirty = 1; }
    return D3D_OK;
}
static void v_GetContainer(X86State *st) {
    D9Vol *v = D9OBJ(D9Vol, 0);
    if (!ARG(2)) { RET(D3DERR_INVALIDCALL); return; }
    d9_addref(&v->tex->o);
    wr_ptr(ARG(2), H2G(v->tex));
    RET(D3D_OK);
}
static void v_GetDesc(X86State *st) { if (!ARG(1)) { RET(D3DERR_INVALIDCALL); return; } vol_desc(D9OBJ(D9Vol, 0), ARG(1)); RET(D3D_OK); }
static void v_LockBox(X86State *st) { RET(vol_lock(D9OBJ(D9Vol, 0), ARG(1), ARG(2), (uint32_t)ARG(3))); }
static void v_UnlockBox(X86State *st) {
    D9Vol *v = D9OBJ(D9Vol, 0);
    if (!v->lock_count) { RET(D3DERR_INVALIDCALL); return; }
    v->lock_count--;
    v->modified = 1; v->tex->dirty = 1;
    RET(D3D_OK);
}

// ---- textures ----------------------------------------------------------------------------------------------------
D9Tex *d9_tex_create(D9Dev *dev, int kind, int w, int h, int d, int levels, int usage, int fmt, int pool, uint32_t *hr) {
    *hr = D3DERR_INVALIDCALL;
    if (w < 1 || h < 1 || w > 8192 || h > 8192 || (kind == SG_TEX_VOLUME && (d < 1 || d > 2048))) return NULL;
    if (d9_fmt_is_depth(fmt) || !d9_fmt_texture_ok(fmt)) { *hr = D3DERR_INVALIDCALL; return NULL; }
    int rt = (usage & USAGE_RENDERTARGET) != 0;
    if (rt && !d9_fmt_rt_ok(fmt)) { *hr = D3DERR_INVALIDCALL; return NULL; }
    int maxl = 1;
    { int m = w > h ? w : h; if (kind == SG_TEX_VOLUME && d > m) m = d; while (m > 1) { m >>= 1; maxl++; } }
    int autogen = (usage & USAGE_AUTOGENMIPMAP) != 0;
    if (levels <= 0 || levels > maxl) levels = autogen ? 1 : maxl;
    if (levels > SG_MAX_MIPS) levels = SG_MAX_MIPS;
    if (autogen) levels = 1;
    D9Tex *t = d9_new(OT_TEX, sizeof *t);
    if (!t) { *hr = E_OUTOFMEMORY_; return NULL; }
    d9_obj_setvt(&t->o, kind == SG_TEX_CUBE ? VT_CUBE : kind == SG_TEX_VOLUME ? VT_VOLTEX : VT_TEX);
    t->o.kind = kind; t->o.dev = dev;
    d9_hold(&dev->o);
    t->w = w; t->h = kind == SG_TEX_CUBE ? w : h; t->d = kind == SG_TEX_VOLUME ? d : 1;
    t->levels = levels; t->fmt = fmt; t->usage = usage; t->pool = pool; t->is_rt = rt;
    t->autogen_filter = 2;
    t->dirty = 1;
    int faces = kind == SG_TEX_CUBE ? 6 : 1;
    for (int l = 0; l < levels; l++) {
        int lw = t->w >> l; if (lw < 1) lw = 1;
        int lh = t->h >> l; if (lh < 1) lh = 1;
        int ld = t->d >> l; if (ld < 1) ld = 1;
        if (kind == SG_TEX_VOLUME) {
            D9Vol *v = d9_new(OT_VOLUME, sizeof *v);
            d9_obj_setvt(&v->o, VT_VOLUME);
            v->o.dev = dev; v->o.container = &t->o;
            v->w = lw; v->h = lh; v->d = ld; v->fmt = fmt; v->level = l; v->tex = t;
            v->rowpitch = (int)d9_level_pitch(fmt, lw);
            v->slicepitch = v->rowpitch * (int)d9_level_rows(fmt, lh);
            v->data = gz((size_t)v->slicepitch * (size_t)ld);
            v->modified = 1;
            t->vol[l] = v;
            continue;
        }
        for (int f = 0; f < faces; f++) {
            D9Surf *s = d9_surf_create(dev, lw, lh, fmt, usage, pool, rt, 0, &t->o);
            s->tex = t; s->level = l; s->face = f;
            s->modified = 1;
            t->surf[f][l] = s;
        }
    }
    *hr = D3D_OK;
    return t;
}

SgTexture *d9_tex_prepare(D9Tex *t) {
    int dirty = !t->sg_valid || t->dirty;
    if (!dirty && (t->fmt == F_P8 || t->fmt == F_A8P8)) dirty = 1;
    int faces = t->o.kind == SG_TEX_CUBE ? 6 : 1;
    if (!dirty) {
        for (int l = 0; l < t->levels && !dirty; l++) {
            if (t->vol[l] && t->vol[l]->modified) dirty = 1;
            for (int f = 0; f < faces; f++) if (t->surf[f][l] && t->surf[f][l]->modified) dirty = 1;
        }
    }
    if (!dirty) return &t->sg;
    if (t->sg_valid) sg_texture_free(&t->sg);
    memset(&t->sg, 0, sizeof t->sg);
    SgTexture *sg = &t->sg;
    sg->w = t->w; sg->h = t->h; sg->depth = t->d; sg->kind = t->o.kind; sg->format = SG_FMT_RGBA; sg->levels = t->levels;
    sg->min_linear = sg->mag_linear = 1;
    for (int l = 0; l < t->levels; l++) {
        int lw = t->w >> l; if (lw < 1) lw = 1;
        int lh = t->h >> l; if (lh < 1) lh = 1;
        int ld = t->d >> l; if (ld < 1) ld = 1;
        size_t per = (size_t)lw * (size_t)lh * 4;
        uint8_t *buf = malloc(per * (size_t)ld * (size_t)faces + 4);
        if (t->vol[l]) {
            D9Vol *v = t->vol[l];
            uint32_t *row = malloc((size_t)lw * 4);
            if (d9_fmt_is_dxt(t->fmt)) {
                for (int z = 0; z < ld; z++) {
                    uint32_t *full = malloc(per);
                    d9_decode_dxt(t->fmt, v->data + (size_t)z * (size_t)v->slicepitch, lw, lh, (size_t)v->rowpitch, full);
                    for (size_t i = 0; i < (size_t)lw * (size_t)lh; i++) { uint32_t c = full[i]; uint8_t *p = buf + (size_t)z * per + i * 4; p[0] = (uint8_t)(c >> 16); p[1] = (uint8_t)(c >> 8); p[2] = (uint8_t)c; p[3] = (uint8_t)(c >> 24); }
                    free(full);
                }
            } else {
                for (int z = 0; z < ld; z++) for (int y = 0; y < lh; y++) {
                    d9_unpack_row(t->fmt, v->data + (size_t)z * (size_t)v->slicepitch + (size_t)y * (size_t)v->rowpitch, row, lw, NULL);
                    uint8_t *p = buf + (size_t)z * per + (size_t)y * (size_t)lw * 4;
                    for (int x = 0; x < lw; x++) { uint32_t c = row[x]; p[4 * x] = (uint8_t)(c >> 16); p[4 * x + 1] = (uint8_t)(c >> 8); p[4 * x + 2] = (uint8_t)c; p[4 * x + 3] = (uint8_t)(c >> 24); }
                }
            }
            free(row);
            v->modified = 0;
        } else {
            uint32_t *tmp = malloc(per);
            for (int f = 0; f < faces; f++) {
                D9Surf *s = t->surf[f][l];
                if (s->shadow && s->argb_stale) d9_surf_from_native(s);
                if (!d9_surf_read_argb(s, NULL, tmp)) memset(tmp, 0, per);
                uint8_t *p = buf + (size_t)f * per;
                for (size_t i = 0; i < (size_t)lw * (size_t)lh; i++) { uint32_t c = tmp[i]; p[4 * i] = (uint8_t)(c >> 16); p[4 * i + 1] = (uint8_t)(c >> 8); p[4 * i + 2] = (uint8_t)c; p[4 * i + 3] = (uint8_t)(c >> 24); }
                s->modified = 0;
            }
            free(tmp);
        }
        sg->mip[l] = buf; sg->mip_w[l] = lw; sg->mip_h[l] = lh; sg->mip_d[l] = ld;
        if (l == 0) sg->rgba = buf;
    }
    if ((t->usage & USAGE_AUTOGENMIPMAP) && t->levels == 1) sg_texture_build_mips(sg);
    sg_texture_touch(sg);                 // new texel data: new id/version for GPU backends
    t->sg_valid = 1;
    t->dirty = 0;
    return sg;
}

static void tex_free_sg(D9Tex *t) { if (t->sg_valid) sg_texture_free(&t->sg); t->sg_valid = 0; }

static int level_ok(D9Tex *t, uint32_t l) { return l < (uint32_t)t->levels; }
static void t_GetLevelDesc(X86State *st) {
    D9Tex *t = D9OBJ(D9Tex, 0);
    uint32_t l = (uint32_t)ARG(1);
    if (!level_ok(t, l) || !ARG(2)) { RET(D3DERR_INVALIDCALL); return; }
    if (t->o.kind == SG_TEX_VOLUME) { vol_desc(t->vol[l], ARG(2)); RET(D3D_OK); return; }
    surf_desc(t->surf[0][l], ARG(2));
    RET(D3D_OK);
}
static void t_GetSurfaceLevel(X86State *st) {
    D9Tex *t = D9OBJ(D9Tex, 0);
    uint32_t l = (uint32_t)ARG(1);
    if (!level_ok(t, l) || !ARG(2)) { RET(D3DERR_INVALIDCALL); return; }
    D9Surf *s = t->surf[0][l];
    d9_addref(&s->o);
    wr_ptr(ARG(2), H2G(s));
    RET(D3D_OK);
}
static void t_LockRect(X86State *st) { D9Tex *t = D9OBJ(D9Tex, 0); uint32_t l = (uint32_t)ARG(1); RET(level_ok(t, l) ? surf_lock(t->surf[0][l], ARG(2), ARG(3), (uint32_t)ARG(4)) : D3DERR_INVALIDCALL); }
static void t_UnlockRect(X86State *st) { D9Tex *t = D9OBJ(D9Tex, 0); uint32_t l = (uint32_t)ARG(1); RET(level_ok(t, l) ? surf_unlock(t->surf[0][l]) : D3DERR_INVALIDCALL); }
static void t_AddDirtyRect(X86State *st) { D9Tex *t = D9OBJ(D9Tex, 0); t->dirty = 1; for (int l = 0; l < t->levels; l++) if (t->surf[0][l]) t->surf[0][l]->modified = 1; RET(D3D_OK); }
static void c_GetCubeMapSurface(X86State *st) { // (this, face, level, pp)
    D9Tex *t = D9OBJ(D9Tex, 0);
    uint32_t f = (uint32_t)ARG(1), l = (uint32_t)ARG(2);
    if (f > 5 || !level_ok(t, l) || !ARG(3)) { RET(D3DERR_INVALIDCALL); return; }
    D9Surf *s = t->surf[f][l];
    d9_addref(&s->o);
    wr_ptr(ARG(3), H2G(s));
    RET(D3D_OK);
}
static void c_LockRect(X86State *st) { // (this, face, level, pLR, pRect, flags)
    D9Tex *t = D9OBJ(D9Tex, 0);
    uint32_t f = (uint32_t)ARG(1), l = (uint32_t)ARG(2);
    RET(f < 6 && level_ok(t, l) ? surf_lock(t->surf[f][l], ARG(3), ARG(4), (uint32_t)ARG(5)) : D3DERR_INVALIDCALL);
}
static void c_UnlockRect(X86State *st) {
    D9Tex *t = D9OBJ(D9Tex, 0);
    uint32_t f = (uint32_t)ARG(1), l = (uint32_t)ARG(2);
    RET(f < 6 && level_ok(t, l) ? surf_unlock(t->surf[f][l]) : D3DERR_INVALIDCALL);
}
static void c_AddDirtyRect(X86State *st) { D9Tex *t = D9OBJ(D9Tex, 0); t->dirty = 1; for (int f = 0; f < 6; f++) for (int l = 0; l < t->levels; l++) if (t->surf[f][l]) t->surf[f][l]->modified = 1; RET(D3D_OK); }
static void vt_GetVolumeLevel(X86State *st) {
    D9Tex *t = D9OBJ(D9Tex, 0);
    uint32_t l = (uint32_t)ARG(1);
    if (!level_ok(t, l) || !ARG(2)) { RET(D3DERR_INVALIDCALL); return; }
    d9_addref(&t->vol[l]->o);
    wr_ptr(ARG(2), H2G(t->vol[l]));
    RET(D3D_OK);
}
static void vt_LockBox(X86State *st) { D9Tex *t = D9OBJ(D9Tex, 0); uint32_t l = (uint32_t)ARG(1); RET(level_ok(t, l) ? vol_lock(t->vol[l], ARG(2), ARG(3), (uint32_t)ARG(4)) : D3DERR_INVALIDCALL); }
static void vt_UnlockBox(X86State *st) {
    D9Tex *t = D9OBJ(D9Tex, 0);
    uint32_t l = (uint32_t)ARG(1);
    if (!level_ok(t, l) || !t->vol[l]->lock_count) { RET(D3DERR_INVALIDCALL); return; }
    t->vol[l]->lock_count--; t->vol[l]->modified = 1; t->dirty = 1;
    RET(D3D_OK);
}
static void vt_AddDirtyBox(X86State *st) { D9Tex *t = D9OBJ(D9Tex, 0); t->dirty = 1; RET(D3D_OK); }

static void bt_SetLOD(X86State *st) { D9Tex *t = D9OBJ(D9Tex, 0); uint32_t old = t->lod; if (t->pool == POOL_MANAGED) t->lod = (uint32_t)ARG(1) < (uint32_t)t->levels ? (uint32_t)ARG(1) : (uint32_t)t->levels - 1; RET(old); }
static void bt_GetLOD(X86State *st) { RET(D9OBJ(D9Tex, 0)->lod); }
static void bt_GetLevelCount(X86State *st) { RET(D9OBJ(D9Tex, 0)->levels); }
static void bt_SetAutoGenFilterType(X86State *st) { D9Tex *t = D9OBJ(D9Tex, 0); if (!(t->usage & USAGE_AUTOGENMIPMAP)) { RET(D3DERR_INVALIDCALL); return; } t->autogen_filter = (uint32_t)ARG(1); RET(D3D_OK); }
static void bt_GetAutoGenFilterType(X86State *st) { RET(D9OBJ(D9Tex, 0)->autogen_filter); }
static void bt_GenerateMipSubLevels(X86State *st) {
    D9Tex *t = D9OBJ(D9Tex, 0);
    if (!(t->usage & USAGE_AUTOGENMIPMAP) && t->levels > 1 && t->o.kind != SG_TEX_VOLUME && !d9_fmt_is_dxt(t->fmt)) {
        int faces = t->o.kind == SG_TEX_CUBE ? 6 : 1;
        for (int f = 0; f < faces; f++) for (int l = 1; l < t->levels; l++) {
            D9Surf *src = t->surf[f][l - 1], *dst = t->surf[f][l];
            uint32_t *sp = malloc((size_t)src->w * (size_t)src->h * 4), *dp = malloc((size_t)dst->w * (size_t)dst->h * 4);
            d9_surf_read_argb(src, NULL, sp);
            for (int y = 0; y < dst->h; y++) for (int x = 0; x < dst->w; x++) {
                int x0 = x * 2, y0 = y * 2, x1 = x0 + 1 < src->w ? x0 + 1 : x0, y1 = y0 + 1 < src->h ? y0 + 1 : y0;
                uint32_t p[4] = { sp[(size_t)y0 * (size_t)src->w + (size_t)x0], sp[(size_t)y0 * (size_t)src->w + (size_t)x1], sp[(size_t)y1 * (size_t)src->w + (size_t)x0], sp[(size_t)y1 * (size_t)src->w + (size_t)x1] };
                uint32_t o = 0;
                for (int sh = 0; sh < 32; sh += 8) o |= ((((p[0] >> sh) & 255) + ((p[1] >> sh) & 255) + ((p[2] >> sh) & 255) + ((p[3] >> sh) & 255) + 2) / 4) << sh;
                dp[(size_t)y * (size_t)dst->w + (size_t)x] = o;
            }
            d9_surf_write_argb(dst, NULL, dp);
            free(sp); free(dp);
        }
    }
    t->dirty = 1;
}

// ---- buffers -------------------------------------------------------------------------------------------------------
D9Buf *d9_buf_create(D9Dev *dev, int is_ib, uint32_t size, uint32_t usage, uint32_t fvf_or_fmt, uint32_t pool) {
    D9Buf *b = d9_new(OT_VB + (is_ib ? 1 : 0), sizeof *b);
    if (!b) return NULL;
    d9_obj_setvt(&b->o, is_ib ? VT_IB : VT_VB);
    b->o.kind = is_ib; b->o.dev = dev;
    d9_hold(&dev->o);
    b->size = size; b->usage = usage; b->pool = pool;
    if (is_ib) b->fmt = fvf_or_fmt; else b->fvf = fvf_or_fmt;
    b->data = gz(size);
    return b;
}
static void b_Lock(X86State *st) { // (this, offset, size, ppData, flags)
    D9Buf *b = D9OBJ(D9Buf, 0);
    uint32_t off = (uint32_t)ARG(1), sz = (uint32_t)ARG(2);
    if (!ARG(3)) { RET(D3DERR_INVALIDCALL); return; }
    if (off > b->size || (sz && off + sz > b->size)) { RET(D3DERR_INVALIDCALL); return; }
    b->locked = 1; b->lock_off = off; b->lock_size = sz ? sz : b->size - off; b->lock_flags = (uint32_t)ARG(4);
    wr_ptr(ARG(3), H2G(b->data + off));
    RET(D3D_OK);
}
static void b_Unlock(X86State *st) { D9Buf *b = D9OBJ(D9Buf, 0); b->locked = 0; RET(D3D_OK); }
static void b_GetDesc(X86State *st) {
    D9Buf *b = D9OBJ(D9Buf, 0);
    uint64_t p = ARG(1);
    if (!p) { RET(D3DERR_INVALIDCALL); return; }
    if (b->o.kind) { d9_wr32(p, b->fmt); d9_wr32(p + 4, RT_INDEXBUFFER); d9_wr32(p + 8, b->usage); d9_wr32(p + 12, b->pool); d9_wr32(p + 16, b->size); }
    else { d9_wr32(p, F_VERTEXDATA); d9_wr32(p + 4, RT_VERTEXBUFFER); d9_wr32(p + 8, b->usage); d9_wr32(p + 12, b->pool); d9_wr32(p + 16, b->size); d9_wr32(p + 20, b->fvf); }
    RET(D3D_OK);
}

// ---- declarations and shaders ----------------------------------------------------------------------------------
static const uint8_t k_type_size[] = { 4, 8, 12, 16, 4, 4, 4, 8, 4, 4, 8, 4, 8, 4, 4, 4, 8 };
int d9_decl_size(const D9Elem *e, int n, int stream) {
    int sz = 0;
    for (int i = 0; i < n; i++) if (e[i].stream == stream && e[i].type < 17) { int end = e[i].offset + k_type_size[e[i].type]; if (end > sz) sz = end; }
    return sz;
}
D9Decl *d9_decl_create(D9Dev *dev, const D9Elem *e, int n, uint32_t fvf) {
    D9Decl *d = d9_new(OT_DECL, sizeof *d);
    if (!d) return NULL;
    d9_obj_setvt(&d->o, VT_DECL);
    d->o.dev = dev;
    d9_hold(&dev->o);
    if (n > 16) n = 16;
    memcpy(d->e, e, (size_t)n * sizeof(D9Elem));
    d->n = n;
    d->fvf = fvf;
    return d;
}
D9Decl *d9_decl_from_fvf(D9Dev *dev, uint32_t fvf) {
    D9Elem e[17];
    int n = 0, off = 0;
    uint32_t pos = fvf & 0x400E;
    if (pos == 0x4) { e[n++] = (D9Elem){ 0, 0, 3, 0, 9, 0 }; off = 16; }            // XYZRHW -> POSITIONT
    else if (pos) {
        int nb = 0;
        if (pos == 0x4002) { e[n++] = (D9Elem){ 0, 0, 3, 0, 0, 0 }; off = 16; }    // XYZW
        else { e[n++] = (D9Elem){ 0, 0, 2, 0, 0, 0 }; off = 12; nb = pos == 0x2 ? 0 : (int)((pos - 0x2) >> 1); }
        if (nb) {
            int last_ub = (fvf & 0x1000) != 0, last_col = (fvf & 0x8000) != 0;
            int nf = (last_ub || last_col) ? nb - 1 : nb;
            if (nf > 0) { e[n++] = (D9Elem){ 0, (uint16_t)off, (uint8_t)(nf - 1), 0, 1, 0 }; off += 4 * nf; }
            if (last_ub) { e[n++] = (D9Elem){ 0, (uint16_t)off, 5, 0, 2, 0 }; off += 4; }
            else if (last_col) { e[n++] = (D9Elem){ 0, (uint16_t)off, 4, 0, 2, 0 }; off += 4; }
        }
    }
    if (fvf & 0x10) { e[n++] = (D9Elem){ 0, (uint16_t)off, 2, 0, 3, 0 }; off += 12; }
    if (fvf & 0x20) { e[n++] = (D9Elem){ 0, (uint16_t)off, 0, 0, 4, 0 }; off += 4; }
    if (fvf & 0x40) { e[n++] = (D9Elem){ 0, (uint16_t)off, 4, 0, 10, 0 }; off += 4; }
    if (fvf & 0x80) { e[n++] = (D9Elem){ 0, (uint16_t)off, 4, 0, 10, 1 }; off += 4; }
    int nt = (int)((fvf >> 8) & 15);
    for (int i = 0; i < nt && n < 16; i++) {
        int sz = (int)((fvf >> (16 + 2 * i)) & 3);
        static const uint8_t ty[4] = { 1, 2, 3, 0 }, by[4] = { 8, 12, 16, 4 };
        e[n++] = (D9Elem){ 0, (uint16_t)off, ty[sz], 0, 5, (uint8_t)i };
        off += by[sz];
    }
    return d9_decl_create(dev, e, n, fvf);
}
static void dc_GetDeclaration(X86State *st) { // (this, D3DVERTEXELEMENT9 *, UINT *num)
    D9Decl *d = D9OBJ(D9Decl, 0);
    if (!ARG(2)) { RET(D3DERR_INVALIDCALL); return; }
    if (ARG(1)) {
        for (int i = 0; i < d->n; i++) {
            uint64_t p = ARG(1) + 8 * (uint64_t)i;
            mem_wr(p, 2, d->e[i].stream); mem_wr(p + 2, 2, d->e[i].offset);
            mem_wr(p + 4, 1, d->e[i].type); mem_wr(p + 5, 1, d->e[i].method); mem_wr(p + 6, 1, d->e[i].usage); mem_wr(p + 7, 1, d->e[i].index);
        }
        uint64_t p = ARG(1) + 8 * (uint64_t)d->n;
        mem_wr(p, 2, 0xFF); mem_wr(p + 2, 2, 0); mem_wr(p + 4, 1, 17); mem_wr(p + 5, 1, 0); mem_wr(p + 6, 1, 0); mem_wr(p + 7, 1, 0);
    }
    d9_wr32(ARG(2), (uint32_t)d->n + 1);
    RET(D3D_OK);
}
static void sh_GetFunction(X86State *st) { // (this, void *data, UINT *size)
    D9Shader *s = D9OBJ(D9Shader, 0);
    if (!ARG(2)) { RET(D3DERR_INVALIDCALL); return; }
    uint32_t sz = (uint32_t)(d3dsh_token_count(s->sh) * 4);
    uint32_t cap = d9_rd32(ARG(2));
    d9_wr32(ARG(2), sz);
    if (ARG(1)) {
        if (cap < sz) { RET(D3DERR_MOREDATA); return; }
        memcpy(G2H(ARG(1)), d3dsh_tokens(s->sh), sz);
    }
    RET(D3D_OK);
}

// ---- queries ---------------------------------------------------------------------------------------------------
D9Query *d9_query_create(D9Dev *dev, int type) {
    switch (type) { case 8: case 9: case 10: case 11: case 12: break; default: return NULL; }
    D9Query *q = d9_new(OT_QUERY, sizeof *q);
    if (!q) return NULL;
    d9_obj_setvt(&q->o, VT_QUERY);
    q->o.dev = dev;
    d9_hold(&dev->o);
    q->qtype = type;
    return q;
}
static int q_size(int t) { return t == 8 ? 4 : t == 9 ? 4 : t == 10 ? 8 : t == 11 ? 4 : t == 12 ? 8 : 0; }
static void q_GetType(X86State *st) { RET(D9OBJ(D9Query, 0)->qtype); }
static void q_GetDataSize(X86State *st) { RET(q_size(D9OBJ(D9Query, 0)->qtype)); }
static void q_Issue(X86State *st) { // (this, flags): D3DISSUE_END 1, D3DISSUE_BEGIN 2
    D9Query *q = D9OBJ(D9Query, 0);
    uint32_t f = (uint32_t)ARG(1);
    if (q->qtype == 9) {
        if (f & 2) { q->pix0 = sg_pixels_drawn(); q->building = 1; }
        if (f & 1) { q->value = q->building ? sg_pixels_drawn() - q->pix0 : 0; q->building = 0; q->issued = 1; }
    } else if (f & 1) {
        struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
        q->t = (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
        q->issued = 1;
    }
    RET(D3D_OK);
}
static void q_GetData(X86State *st) { // (this, void *data, DWORD size, DWORD flags)
    D9Query *q = D9OBJ(D9Query, 0);
    uint64_t p = ARG(1);
    uint32_t sz = (uint32_t)ARG(2);
    if (!q->issued && q->qtype != 11 && q->qtype != 12) { RET(S_FALSE_); return; }
    if (p && sz) {
        uint64_t v = 0;
        switch (q->qtype) {
        case 8: v = 1; break;
        case 9: v = q->value > 0xFFFFFFFFull ? 0xFFFFFFFFu : q->value; break;
        case 10: v = (uint64_t)(q->t * 1e9); break;
        case 11: v = 0; break;
        case 12: v = 1000000000ull; break;
        }
        int n = q_size(q->qtype);
        if ((int)sz < n) n = (int)sz;
        for (int i = 0; i < n; i++) mem_wr(p + (uint64_t)i, 1, (v >> (8 * i)) & 255);
    }
    RET(D3D_OK);
}

// ---- method tables ----------------------------------------------------------------------------------------------
void d9_m_QI(X86State *st);
void d9_m_AddRef(X86State *st);
void d9_m_Release(X86State *st);
#define U3 { "QueryInterface", d9_m_QI, 3 }, { "AddRef", d9_m_AddRef, 1 }, { "Release", d9_m_Release, 1 }
#define RES { "GetDevice", r_GetDevice, 2 }, { "SetPrivateData", r_SetPrivateData, 5 }, { "GetPrivateData", r_GetPrivateData, 4 }, \
            { "FreePrivateData", r_FreePrivateData, 2 }, { "SetPriority", r_SetPriority, 2 }, { "GetPriority", r_GetPriority, 1 }, \
            { "PreLoad", r_PreLoad, 1 }, { "GetType", r_GetType, 1 }
#define BASETEX { "SetLOD", bt_SetLOD, 2 }, { "GetLOD", bt_GetLOD, 1 }, { "GetLevelCount", bt_GetLevelCount, 1 }, \
                { "SetAutoGenFilterType", bt_SetAutoGenFilterType, 2 }, { "GetAutoGenFilterType", bt_GetAutoGenFilterType, 1 }, \
                { "GenerateMipSubLevels", bt_GenerateMipSubLevels, 1 }

static const D9Method m_surf[] = { U3, RES, { "GetContainer", s_GetContainer, 3 }, { "GetDesc", s_GetDesc, 2 }, { "LockRect", s_LockRect, 4 },
                                   { "UnlockRect", s_UnlockRect, 1 }, { "GetDC", s_GetDC, 2 }, { "ReleaseDC", s_ReleaseDC, 2 } };
static const D9Method m_tex[] = { U3, RES, BASETEX, { "GetLevelDesc", t_GetLevelDesc, 3 }, { "GetSurfaceLevel", t_GetSurfaceLevel, 3 },
                                  { "LockRect", t_LockRect, 5 }, { "UnlockRect", t_UnlockRect, 2 }, { "AddDirtyRect", t_AddDirtyRect, 2 } };
static const D9Method m_cube[] = { U3, RES, BASETEX, { "GetLevelDesc", t_GetLevelDesc, 3 }, { "GetCubeMapSurface", c_GetCubeMapSurface, 4 },
                                   { "LockRect", c_LockRect, 6 }, { "UnlockRect", c_UnlockRect, 3 }, { "AddDirtyRect", c_AddDirtyRect, 3 } };
static const D9Method m_voltex[] = { U3, RES, BASETEX, { "GetLevelDesc", t_GetLevelDesc, 3 }, { "GetVolumeLevel", vt_GetVolumeLevel, 3 },
                                     { "LockBox", vt_LockBox, 5 }, { "UnlockBox", vt_UnlockBox, 2 }, { "AddDirtyBox", vt_AddDirtyBox, 2 } };
static const D9Method m_volume[] = { U3, { "GetDevice", r_GetDevice, 2 }, { "SetPrivateData", r_SetPrivateData, 5 }, { "GetPrivateData", r_GetPrivateData, 4 },
                                     { "FreePrivateData", r_FreePrivateData, 2 }, { "GetContainer", v_GetContainer, 3 }, { "GetDesc", v_GetDesc, 2 },
                                     { "LockBox", v_LockBox, 4 }, { "UnlockBox", v_UnlockBox, 1 } };
static const D9Method m_vb[] = { U3, RES, { "Lock", b_Lock, 5 }, { "Unlock", b_Unlock, 1 }, { "GetDesc", b_GetDesc, 2 } };
static const D9Method m_decl[] = { U3, { "GetDevice", r_GetDevice, 2 }, { "GetDeclaration", dc_GetDeclaration, 3 } };
static const D9Method m_shader[] = { U3, { "GetDevice", r_GetDevice, 2 }, { "GetFunction", sh_GetFunction, 3 } };
static const D9Method m_query[] = { U3, { "GetDevice", r_GetDevice, 2 }, { "GetType", q_GetType, 1 }, { "GetDataSize", q_GetDataSize, 1 },
                                    { "Issue", q_Issue, 2 }, { "GetData", q_GetData, 4 } };
#define N(a) ((int)(sizeof(a) / sizeof((a)[0])))
void d9_register_res_vtables(void) {
    d9_vt_register(VT_SURF, "IDirect3DSurface9", m_surf, N(m_surf));
    d9_vt_register(VT_TEX, "IDirect3DTexture9", m_tex, N(m_tex));
    d9_vt_register(VT_CUBE, "IDirect3DCubeTexture9", m_cube, N(m_cube));
    d9_vt_register(VT_VOLTEX, "IDirect3DVolumeTexture9", m_voltex, N(m_voltex));
    d9_vt_register(VT_VOLUME, "IDirect3DVolume9", m_volume, N(m_volume));
    d9_vt_register(VT_VB, "IDirect3DVertexBuffer9", m_vb, N(m_vb));
    d9_vt_register(VT_IB, "IDirect3DIndexBuffer9", m_vb, N(m_vb));
    d9_vt_register(VT_DECL, "IDirect3DVertexDeclaration9", m_decl, N(m_decl));
    d9_vt_register(VT_VS, "IDirect3DVertexShader9", m_shader, N(m_shader));
    d9_vt_register(VT_PS, "IDirect3DPixelShader9", m_shader, N(m_shader));
    d9_vt_register(VT_QUERY, "IDirect3DQuery9", m_query, N(m_query));
}

// Called by d9_destroy for resource types: frees what the object owns besides its own block.
void d9_res_destroy(D9Obj *o) {
    switch (o->type) {
    case OT_SURF: d9_surf_free_storage((D9Surf *)o); break;
    case OT_VOLUME: g_free(((D9Vol *)o)->data); break;
    case OT_TEX: {
        D9Tex *t = (D9Tex *)o;
        tex_free_sg(t);
        for (int f = 0; f < 6; f++) for (int l = 0; l < SG_MAX_MIPS; l++) if (t->surf[f][l]) { d9_surf_free_storage(t->surf[f][l]); d9_priv_free_all(&t->surf[f][l]->o); d9_free_block(&t->surf[f][l]->o); t->surf[f][l] = NULL; }
        for (int l = 0; l < SG_MAX_MIPS; l++) if (t->vol[l]) { g_free(t->vol[l]->data); d9_priv_free_all(&t->vol[l]->o); d9_free_block(&t->vol[l]->o); t->vol[l] = NULL; }
        break;
    }
    case OT_VS: case OT_PS: d9_shader_free((D9Shader *)o); break;
    case OT_VB: case OT_IB: g_free(((D9Buf *)o)->data); break;
    default: break;
    }
}
