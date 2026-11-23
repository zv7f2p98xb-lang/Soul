// SoulCore - Direct3D 9: swap chains, the device object and its state (render/sampler/texture-stage states,
// transforms, lights, state blocks, shader constants, resource creation and copying, presentation).
#include "d3d9_priv.h"

#define PP_SPEC "4444444P444444" // W H Fmt Count MSType MSQuality Effect hDeviceWindow Windowed AutoDS AutoDSFmt Flags Refresh Interval

static int rd_rect(uint64_t p, int32_t *rc) {
    if (!p) return 0;
    for (int i = 0; i < 4; i++) rc[i] = (int32_t)mem_rd(p + 4 * (uint64_t)i, 4);
    return 1;
}
static int samp_idx(uint32_t s) { if (s < 16) return (int)s; if (s == 256) return 16; if (s >= 257 && s <= 260) return 17 + (int)(s - 257); return -1; }
static D9Dev *DV(X86State *st) { return (D9Dev *)G2H(ARG(0)); }

// ---- defaults ------------------------------------------------------------------------------------------------------
static void ident(float *m) { memset(m, 0, 64); m[0] = m[5] = m[10] = m[15] = 1; }
static void state_init(D9State *s, int w, int h, int zenable) {
    memset(s, 0, sizeof *s);
    uint32_t *r = s->rs;
    r[7] = zenable ? 1 : 0; r[8] = 3; r[9] = 2; r[14] = 1; r[16] = 1; r[19] = 2; r[20] = 1; r[22] = 3; r[23] = 4; r[25] = 8;
    r[34] = 0; r[36] = f2u(0.0f); r[37] = f2u(1.0f); r[38] = f2u(1.0f);
    r[53] = r[54] = r[55] = 1; r[56] = 8; r[58] = r[59] = 0xFFFFFFFFu; r[60] = 0xFFFFFFFFu;
    r[136] = 1; r[137] = 1; r[141] = 1; r[142] = 1; r[145] = 1; r[146] = 2;
    r[154] = f2u(1.0f); r[155] = f2u(1.0f); r[158] = f2u(1.0f); r[161] = 1; r[162] = 0xFFFFFFFFu; r[166] = f2u(64.0f);
    r[168] = 0xF; r[171] = 1; r[172] = 3; r[173] = 1; r[178] = f2u(1.0f); r[179] = f2u(1.0f);
    r[186] = r[187] = r[188] = 1; r[189] = 8; r[190] = r[191] = r[192] = 0xF; r[193] = 0xFFFFFFFFu;
    r[207] = 2; r[208] = 1; r[209] = 1;
    for (int i = 0; i < D9_NSAMP; i++) { s->ss[i][1] = s->ss[i][2] = s->ss[i][3] = 1; s->ss[i][5] = s->ss[i][6] = 1; s->ss[i][10] = 1; }
    for (int i = 0; i < 8; i++) {
        uint32_t *t = s->tss[i];
        t[1] = i == 0 ? 4 : 1; t[2] = 2; t[3] = 1; t[4] = i == 0 ? 2 : 1; t[5] = 2; t[6] = 1;
        t[11] = (uint32_t)i; t[26] = 1; t[27] = 1; t[28] = 1;
    }
    for (int i = 0; i < 25; i++) ident(s->xf[i]);
    s->vp = (D3DViewport9){ 0, 0, (uint32_t)w, (uint32_t)h, 0.0f, 1.0f };
    s->scissor[0] = 0; s->scissor[1] = 0; s->scissor[2] = w; s->scissor[3] = h;
    for (int i = 0; i < 4; i++) { s->mat.diffuse[i] = i < 3 ? 1.0f : 0.0f; s->mat.ambient[i] = i < 3 ? 1.0f : 0.0f; }
    for (int i = 0; i < 16; i++) s->stream[i].freq = 1;
    s->npatch = 0;
}
void d9_dev_default_state(D9Dev *dev) {
    int w = dev->rt ? dev->rt->w : 1, h = dev->rt ? dev->rt->h : 1;
    state_init(&dev->st, w, h, dev->ds != NULL);
}

// ---- reference handling of state pointers -------------------------------------------------------------------------------
static void ref_set(D9Obj **slot, D9Obj *n) {
    if (n) d9_addref(n);
    D9Obj *old = *slot;
    *slot = n;
    if (old) d9_release(old);
}
void d9_dev_release_state(D9Dev *d) {
    D9State *s = &d->st;
    for (int i = 0; i < D9_NSAMP; i++) ref_set(&s->tex[i], NULL);
    for (int i = 0; i < 16; i++) ref_set((D9Obj **)&s->stream[i].vb, NULL);
    ref_set((D9Obj **)&s->ib, NULL);
    ref_set((D9Obj **)&s->vs, NULL);
    ref_set((D9Obj **)&s->ps, NULL);
    ref_set((D9Obj **)&s->decl, NULL);
}

// ---- swap chains ----------------------------------------------------------------------------------------------------------
static void client_size(uint64_t hwnd, int *w, int *h) {
    Win *win = win_get(hwnd);
    if (win && win->w > 0 && win->h > 0) { *w = win->w; *h = win->h; return; }
    *w = 640; *h = 480;
}
D9Swap *d9_swap_create(D9Dev *dev, uint64_t pp, int implicit, uint32_t *hr) {
    *hr = D3DERR_INVALIDCALL;
    uint32_t w = (uint32_t)gs_get(pp, PP_SPEC, 0), h = (uint32_t)gs_get(pp, PP_SPEC, 1), fmt = (uint32_t)gs_get(pp, PP_SPEC, 2);
    uint32_t cnt = (uint32_t)gs_get(pp, PP_SPEC, 3), mst = (uint32_t)gs_get(pp, PP_SPEC, 4);
    uint64_t hwnd = gs_get(pp, PP_SPEC, 7);
    if (!hwnd) hwnd = dev->focus;
    if (mst != 0) return NULL;
    if (!w || !h) { int cw, ch; client_size(hwnd, &cw, &ch); if (!w) w = (uint32_t)cw; if (!h) h = (uint32_t)ch; }
    if (!fmt) fmt = F_X8R8G8B8;
    if (!cnt) cnt = 1;
    if (cnt > 3) cnt = 3;
    if (w > 8192 || h > 8192) return NULL;
    if (!d9_fmt_rt_ok((int)fmt)) { *hr = D3DERR_INVALIDCALL; return NULL; }
    D9Swap *sw = d9_new(OT_SWAP, sizeof *sw);
    if (!sw) { *hr = E_OUTOFMEMORY_; return NULL; }
    d9_obj_setvt(&sw->o, VT_SWAP);
    sw->o.dev = dev;
    sw->w = w; sw->h = h; sw->fmt = fmt; sw->nback = cnt;
    sw->effect = (uint32_t)gs_get(pp, PP_SPEC, 6);
    sw->hwnd = hwnd;
    sw->windowed = (int)gs_get(pp, PP_SPEC, 8);
    sw->autods = (int)gs_get(pp, PP_SPEC, 9);
    sw->depth_fmt = (uint32_t)gs_get(pp, PP_SPEC, 10);
    sw->flags = (uint32_t)gs_get(pp, PP_SPEC, 11);
    sw->refresh = (uint32_t)gs_get(pp, PP_SPEC, 12);
    sw->interval = (uint32_t)gs_get(pp, PP_SPEC, 13);
    sw->implicit = implicit;
    for (uint32_t i = 0; i < cnt; i++) {
        D9Surf *b = d9_surf_create(dev, (int)w, (int)h, (int)fmt, USAGE_RENDERTARGET, POOL_DEFAULT, 1, 0, &sw->o);
        b->is_back = 1;
        for (size_t k = 0; k < (size_t)w * (size_t)h; k++) b->argb[k] = 0xFF000000u;
        if (b->shadow) b->native_stale = 1;
        sw->back[i] = b;
    }
    // write the resolved values back like Direct3D does
    gs_set(pp, PP_SPEC, 0, w); gs_set(pp, PP_SPEC, 1, h); gs_set(pp, PP_SPEC, 2, fmt); gs_set(pp, PP_SPEC, 3, cnt);
    d9_hold(&sw->o);
    *hr = D3D_OK;
    return sw;
}
void d9_swap_destroy(D9Swap *sw) {
    for (int i = 0; i < 4; i++) if (sw->back[i]) {
        d9_surf_free_storage(sw->back[i]);
        d9_priv_free_all(&sw->back[i]->o);
        d9_free_block(&sw->back[i]->o);
        sw->back[i] = NULL;
    }
}

uint32_t d9_present(D9Dev *dev, D9Swap *sw, const int32_t *src, const int32_t *dst, uint64_t hwnd) {
    D9Surf *b = sw->back[0];
    if (!b) return D3DERR_INVALIDCALL;
    if (!hwnd) hwnd = sw->hwnd;
    Win *w = win_get(hwnd);
    if (w) {
        Win *top = win_top(w);
        int ox, oy, x0, y0, x1, y1;
        if (top && top->pixels && w->w > 0 && w->h > 0) {
            win_client_origin(w, &ox, &oy);
            if (win_visible_rect(w, &x0, &y0, &x1, &y1)) {
                if (b->shadow && b->argb_stale) d9_surf_from_native(b);
                int sr[4] = { 0, 0, b->w, b->h }, dr[4] = { 0, 0, w->w, w->h };
                if (src) { memcpy(sr, src, 16); if (sr[0] < 0) sr[0] = 0; if (sr[1] < 0) sr[1] = 0; if (sr[2] > b->w) sr[2] = b->w; if (sr[3] > b->h) sr[3] = b->h; }
                if (dst) memcpy(dr, dst, 16);
                int sw_ = sr[2] - sr[0], sh_ = sr[3] - sr[1], dw = dr[2] - dr[0], dh = dr[3] - dr[1];
                if (sw_ > 0 && sh_ > 0 && dw > 0 && dh > 0) {
                    for (int y = 0; y < dh; y++) {
                        int cy = dr[1] + y, py = cy + oy;
                        if (cy < 0 || cy >= w->h || py < y0 || py >= y1 || py >= top->fb_h) continue;
                        int syy = sr[1] + (dh == sh_ ? y : (int)((int64_t)y * sh_ / dh));
                        const uint32_t *srow = b->argb + (size_t)syy * (size_t)b->w;
                        uint32_t *drow = top->pixels + (size_t)py * (size_t)top->fb_w;
                        for (int x = 0; x < dw; x++) {
                            int cx = dr[0] + x, px = cx + ox;
                            if (cx < 0 || cx >= w->w || px < x0 || px >= x1 || px >= top->fb_w) continue;
                            int sxx = sr[0] + (dw == sw_ ? x : (int)((int64_t)x * sw_ / dw));
                            drow[px] = srow[sxx] | 0xFF000000u;
                        }
                    }
                    win_touch(top);
                }
            }
        }
    }
    sw->present_count++;
    // pace to the display unless the program asked for immediate presentation
    double now = soul_now();
    if (sw->interval != 0x80000000u && dev->last_present > 0) {
        double next = dev->last_present + 1.0 / 60.0;
        if (now < next) { gil_wait((int64_t)((next - now) * 1000.0)); now = soul_now(); }
    }
    dev->last_present = now;
    return D3D_OK;
}

// ---- device creation ------------------------------------------------------------------------------------------------------
static uint32_t make_ds(D9Dev *dev, D9Swap *sw) {
    if (!sw->autods) return D3D_OK;
    if (!d9_fmt_is_depth((int)sw->depth_fmt)) return D3DERR_INVALIDCALL;
    D9Surf *ds = d9_surf_create(dev, (int)sw->w, (int)sw->h, (int)sw->depth_fmt, USAGE_DEPTHSTENCIL, POOL_DEFAULT, 0, 1, NULL);
    if (!ds) return E_OUTOFMEMORY_;
    ref_set((D9Obj **)&dev->ds, &ds->o);
    return D3D_OK;
}
D9Dev *d9_dev_create(D9Obj *d3d, uint32_t adapter, uint32_t devtype, uint64_t focus, uint32_t behavior, uint64_t pp, int ex, uint32_t *hr) {
    *hr = D3DERR_INVALIDCALL;
    if (adapter != 0 || (devtype != 1 && devtype != 2)) { *hr = adapter ? D3DERR_INVALIDCALL : D3DERR_NOTAVAILABLE; return NULL; }
    D9Dev *dev = d9_new(OT_DEV, sizeof *dev);
    if (!dev) { *hr = E_OUTOFMEMORY_; return NULL; }
    d9_obj_setvt(&dev->o, ex ? VT_DEVEX : VT_DEV);
    dev->ex = ex;
    dev->d3d = d3d;
    d9_hold(d3d);
    dev->adapter = adapter; dev->devtype = devtype; dev->focus = focus; dev->behavior = behavior;
    dev->maxlatency = 3;
    D9Swap *sw = d9_swap_create(dev, pp, 1, hr);
    if (!sw) { d9_dev_destroy(dev); d9_free_block(&dev->o); return NULL; }
    dev->swap[0] = sw;
    dev->nswap = 1;
    ref_set((D9Obj **)&dev->rt, &sw->back[0]->o);
    uint32_t r = make_ds(dev, sw);
    if (r != D3D_OK) { *hr = r; d9_dev_destroy(dev); d9_free_block(&dev->o); return NULL; }
    d9_dev_default_state(dev);
    for (int i = 0; i < 256 * 3; i++) dev->gamma[i] = (uint16_t)((i & 255) * 257);
    dev->o.refs = 1;
    *hr = D3D_OK;
    return dev;
}
void d9_dev_teardown(D9Dev *d) {
    if (d->torn || d9_exiting) return;
    d->torn = 1;
    d->o.irefs++;                  // keep the block alive while its references are dropped
    if (d->rec) { D9SB *sb = d->rec; d->rec = NULL; d9_release(&sb->o); }
    d9_dev_release_state(d);
    ref_set((D9Obj **)&d->rt, NULL);
    ref_set((D9Obj **)&d->ds, NULL);
    for (int i = 0; i < d->nfvfc; i++) { D9Decl *dc = d->fvfc[i]; d->fvfc[i] = NULL; d9_unhold(&dc->o); }
    d->nfvfc = 0;
    for (int i = 0; i < d->nswap; i++) if (d->swap[i]) { D9Swap *s = d->swap[i]; d->swap[i] = NULL; d9_unhold(&s->o); }
    if (d->d3d) { D9Obj *o = d->d3d; d->d3d = NULL; d9_unhold(o); }
    d->o.irefs--;
}
void d9_dev_destroy(D9Dev *d) {
    d9_dev_teardown(d);
    free(d->vbuf); free(d->obuf); free(d->vmap); free(d->tmp); free(d->pal);
    d->vbuf = NULL; d->obuf = NULL; d->vmap = NULL; d->tmp = NULL; d->pal = NULL;
}

// ---- state blocks -----------------------------------------------------------------------------------------------------------
static void copy_masked(D9State *dst, const D9State *src, const D9Mask *m) {
    for (int i = 0; i < 256; i++) if (m->rs[i]) dst->rs[i] = src->rs[i];
    for (int i = 0; i < D9_NSAMP; i++) for (int j = 0; j < 16; j++) if (m->ss[i][j]) dst->ss[i][j] = src->ss[i][j];
    for (int i = 0; i < 8; i++) for (int j = 0; j < 33; j++) if (m->tss[i][j]) dst->tss[i][j] = src->tss[i][j];
    for (int i = 0; i < D9_NSAMP; i++) if (m->tex[i]) ref_set(&dst->tex[i], src->tex[i]);
    for (int i = 0; i < 25; i++) if (m->xf[i]) memcpy(dst->xf[i], src->xf[i], 64);
    if (m->vp) dst->vp = src->vp;
    if (m->mat) dst->mat = src->mat;
    if (m->scissor) memcpy(dst->scissor, src->scissor, 16);
    if (m->vs) ref_set((D9Obj **)&dst->vs, (D9Obj *)src->vs);
    if (m->ps) ref_set((D9Obj **)&dst->ps, (D9Obj *)src->ps);
    if (m->decl) ref_set((D9Obj **)&dst->decl, (D9Obj *)src->decl);
    if (m->fvf) dst->fvf = src->fvf;
    if (m->ib) ref_set((D9Obj **)&dst->ib, (D9Obj *)src->ib);
    if (m->sw) dst->sw_vp = src->sw_vp;
    if (m->npatch) dst->npatch = src->npatch;
    for (int i = 0; i < D9_NLIGHT; i++) {
        if (m->light[i]) { dst->light[i] = src->light[i]; dst->light_set[i] = src->light_set[i]; }
        if (m->light_on[i]) dst->light_on[i] = src->light_on[i];
    }
    for (int i = 0; i < 6; i++) if (m->clip[i]) memcpy(dst->clip[i], src->clip[i], 16);
    for (int i = 0; i < 16; i++) {
        if (m->stream[i]) { ref_set((D9Obj **)&dst->stream[i].vb, (D9Obj *)src->stream[i].vb); dst->stream[i].offset = src->stream[i].offset; dst->stream[i].stride = src->stream[i].stride; }
        if (m->freq[i]) dst->stream[i].freq = src->stream[i].freq;
    }
    for (int i = 0; i < 256; i++) { if (m->vsf[i]) memcpy(dst->vsc.f[i], src->vsc.f[i], 16); if (m->psf[i]) memcpy(dst->psc.f[i], src->psc.f[i], 16); }
    for (int i = 0; i < 16; i++) {
        if (m->vsi[i]) memcpy(dst->vsc.i[i], src->vsc.i[i], 16);
        if (m->psi[i]) memcpy(dst->psc.i[i], src->psc.i[i], 16);
        if (m->vsb[i]) dst->vsc.b[i] = src->vsc.b[i];
        if (m->psb[i]) dst->psc.b[i] = src->psc.b[i];
    }
}
static D9SB *sb_new(D9Dev *dev, int type) {
    D9SB *sb = d9_new(OT_SB, sizeof *sb);
    if (!sb) return NULL;
    d9_obj_setvt(&sb->o, VT_SB);
    sb->o.dev = dev;
    d9_hold(&dev->o);
    sb->st = calloc(1, sizeof(D9State));
    sb->type = type;
    return sb;
}
void d9_sb_destroy(D9SB *sb) {
    if (sb->st && !d9_exiting) {
        D9State *s = sb->st;
        for (int i = 0; i < D9_NSAMP; i++) ref_set(&s->tex[i], NULL);
        for (int i = 0; i < 16; i++) ref_set((D9Obj **)&s->stream[i].vb, NULL);
        ref_set((D9Obj **)&s->ib, NULL); ref_set((D9Obj **)&s->vs, NULL); ref_set((D9Obj **)&s->ps, NULL); ref_set((D9Obj **)&s->decl, NULL);
    }
    free(sb->st);
    sb->st = NULL;
}
static const uint8_t k_vertex_rs[] = { 28, 34, 35, 36, 37, 38, 48, 136, 137, 139, 140, 141, 142, 143, 145, 146, 147, 148, 151, 152, 154, 155, 156, 157, 158, 159, 160,
                                       163, 166, 167, 170, 172, 173, 178, 179, 184 };
static void mask_all(D9Mask *m) { memset(m, 1, sizeof *m); }
static void mask_for_type(D9Mask *m, int type) {
    memset(m, 0, sizeof *m);
    if (type == 1) { mask_all(m); return; }
    int vert = type == 3;
    uint8_t isv[256] = { 0 };
    for (size_t i = 0; i < sizeof k_vertex_rs; i++) isv[k_vertex_rs[i]] = 1;
    for (int i = 1; i < 256; i++) m->rs[i] = vert ? isv[i] : !isv[i];
    if (vert) {
        m->vs = m->decl = m->fvf = m->sw = m->npatch = 1;
        memset(m->light, 1, sizeof m->light); memset(m->light_on, 1, sizeof m->light_on);
        memset(m->vsf, 1, sizeof m->vsf); memset(m->vsi, 1, sizeof m->vsi); memset(m->vsb, 1, sizeof m->vsb);
        memset(m->freq, 1, sizeof m->freq);
        m->mat = 1;
        memset(m->clip, 1, sizeof m->clip);
        for (int i = 0; i < 8; i++) { m->tss[i][11] = 1; m->tss[i][24] = 1; }
        for (int i = 16; i < 21; i++) { memset(m->ss[i], 1, 16); m->tex[i] = 1; }
    } else {
        m->ps = 1;
        memset(m->psf, 1, sizeof m->psf); memset(m->psi, 1, sizeof m->psi); memset(m->psb, 1, sizeof m->psb);
        for (int i = 0; i < 16; i++) { memset(m->ss[i], 1, 16); m->tex[i] = 1; }
        for (int i = 0; i < 8; i++) for (int j = 1; j < 33; j++) if (j != 11 && j != 24) m->tss[i][j] = 1;
        m->vp = 1; m->scissor = 1;
    }
}

// ---- device methods: general ----------------------------------------------------------------------------------------------------
#define M_OK(name) static void name(X86State *st) { (void)st; RET(D3D_OK); }
M_OK(dv_TestCooperativeLevel)
static void dv_GetAvailableTextureMem(X86State *st) { RET(0x40000000u); }
M_OK(dv_EvictManagedResources)
static void dv_GetDirect3D(X86State *st) {
    D9Dev *d = DV(st);
    if (!ARG(1)) { RET(D3DERR_INVALIDCALL); return; }
    d9_addref(d->d3d);
    wr_ptr(ARG(1), H2G(d->d3d));
    RET(D3D_OK);
}
static void dv_GetDeviceCaps(X86State *st) {
    D9Dev *d = DV(st);
    if (!ARG(1)) { RET(D3DERR_INVALIDCALL); return; }
    uint32_t c[76];
    d9_caps(c);
    c[0] = d->devtype;
    memcpy(G2H(ARG(1)), c, sizeof c);
    RET(D3D_OK);
}
static void dv_GetDisplayMode(X86State *st) { // (this, swapchain, D3DDISPLAYMODE *)
    int w, h;
    d9_screen_size(&w, &h);
    if (ARG(1) != 0 || !ARG(2)) { RET(D3DERR_INVALIDCALL); return; }
    d9_wr32(ARG(2), (uint32_t)w); d9_wr32(ARG(2) + 4, (uint32_t)h); d9_wr32(ARG(2) + 8, 60); d9_wr32(ARG(2) + 12, F_X8R8G8B8);
    RET(D3D_OK);
}
static void dv_GetCreationParameters(X86State *st) {
    D9Dev *d = DV(st);
    if (!ARG(1)) { RET(D3DERR_INVALIDCALL); return; }
    gs_set(ARG(1), "44P4", 0, d->adapter); gs_set(ARG(1), "44P4", 1, d->devtype); gs_set(ARG(1), "44P4", 2, d->focus); gs_set(ARG(1), "44P4", 3, d->behavior);
    RET(D3D_OK);
}
M_OK(dv_SetCursorProperties)
static void dv_SetCursorPosition(X86State *st) { (void)st; }
static void dv_ShowCursor(X86State *st) { RET(1); }
static void dv_CreateAdditionalSwapChain(X86State *st) { // (this, pp, ppSwap)
    D9Dev *d = DV(st);
    if (!ARG(1) || !ARG(2) || d->nswap >= D9_MAXSWAP) { RET(D3DERR_INVALIDCALL); return; }
    uint32_t hr;
    D9Swap *sw = d9_swap_create(d, ARG(1), 0, &hr);
    if (!sw) { RET(hr); return; }
    d->swap[d->nswap++] = sw;
    d9_addref(&sw->o);
    wr_ptr(ARG(2), H2G(sw));
    RET(D3D_OK);
}
static void dv_GetSwapChain(X86State *st) {
    D9Dev *d = DV(st);
    uint32_t i = (uint32_t)ARG(1);
    if (!ARG(2) || i >= (uint32_t)d->nswap) { RET(D3DERR_INVALIDCALL); return; }
    d9_addref(&d->swap[i]->o);
    wr_ptr(ARG(2), H2G(d->swap[i]));
    RET(D3D_OK);
}
static void dv_GetNumberOfSwapChains(X86State *st) { RET(DV(st)->nswap); }

static uint32_t do_reset(D9Dev *d, uint64_t pp) {
    if (!pp) return D3DERR_INVALIDCALL;
    D9Swap *old = d->swap[0];
    // back buffers must not be referenced outside the device
    for (uint32_t i = 0; i < old->nback; i++) {
        uint32_t own = (old->back[i] == d->rt) ? 1u : 0u;
        if (old->back[i]->o.refs > own) return D3DERR_INVALIDCALL;
    }
    uint32_t hr;
    D9Swap *sw = d9_swap_create(d, pp, 1, &hr);
    if (!sw) return hr;
    if (d->rec) { D9SB *sb = d->rec; d->rec = NULL; d9_release(&sb->o); }
    d9_dev_release_state(d);
    ref_set((D9Obj **)&d->rt, NULL);
    ref_set((D9Obj **)&d->ds, NULL);
    d->swap[0] = sw;
    d9_unhold(&old->o);
    ref_set((D9Obj **)&d->rt, &sw->back[0]->o);
    make_ds(d, sw);
    d9_dev_default_state(d);
    d->in_scene = 0;
    return D3D_OK;
}
static void dv_Reset(X86State *st) { RET(do_reset(DV(st), ARG(1))); }
static void dv_Present(X86State *st) { // (this, srcRect, dstRect, hwnd, dirtyRegion)
    D9Dev *d = DV(st);
    int32_t s[4], t[4];
    int hs = rd_rect(ARG(1), s), ht = rd_rect(ARG(2), t);
    RET(d9_present(d, d->swap[0], hs ? s : NULL, ht ? t : NULL, ARG(3)));
}
static uint32_t get_back(D9Dev *d, uint32_t sc, uint32_t ib, uint64_t pp) {
    if (!pp || sc >= (uint32_t)d->nswap) return D3DERR_INVALIDCALL;
    D9Swap *sw = d->swap[sc];
    if (ib >= sw->nback) return D3DERR_INVALIDCALL;
    d9_addref(&sw->back[ib]->o);
    wr_ptr(pp, H2G(sw->back[ib]));
    return D3D_OK;
}
static void dv_GetBackBuffer(X86State *st) { RET(get_back(DV(st), (uint32_t)ARG(1), (uint32_t)ARG(2), ARG(4))); }
static void dv_GetRasterStatus(X86State *st) { // (this, swapchain, D3DRASTER_STATUS *)
    D9Dev *d = DV(st);
    if (!ARG(2) || ARG(1) >= (uint64_t)d->nswap) { RET(D3DERR_INVALIDCALL); return; }
    double f = soul_now() * 60.0;
    f -= floor(f);
    uint32_t h = d->swap[0]->h ? d->swap[0]->h : 480;
    uint32_t line = (uint32_t)(f * (double)(h + 20));
    d9_wr32(ARG(2), line >= h ? 1 : 0);
    d9_wr32(ARG(2) + 4, line >= h ? 0 : line);
    RET(D3D_OK);
}
M_OK(dv_SetDialogBoxMode)
static void dv_SetGammaRamp(X86State *st) { // (this, swapchain, flags, ramp)
    D9Dev *d = DV(st);
    if (ARG(1) != 0 || !ARG(3)) return;
    memcpy(d->gamma, G2H(ARG(3)), sizeof d->gamma);
}
static void dv_GetGammaRamp(X86State *st) {
    D9Dev *d = DV(st);
    if (ARG(1) != 0 || !ARG(2)) return;
    memcpy(G2H(ARG(2)), d->gamma, sizeof d->gamma);
}

// ---- resource creation -----------------------------------------------------------------------------------------------------------
static void dv_CreateTexture(X86State *st) { // (this, w, h, levels, usage, fmt, pool, ppTex, shared)
    D9Dev *d = DV(st);
    if (!ARG(7)) { RET(D3DERR_INVALIDCALL); return; }
    wr_ptr(ARG(7), 0);
    uint32_t hr;
    D9Tex *t = d9_tex_create(d, SG_TEX_2D, (int)ARG(1), (int)ARG(2), 1, (int)ARG(3), (int)ARG(4), (int)ARG(5), (int)ARG(6), &hr);
    if (!t) { RET(hr); return; }
    d9_addref(&t->o);
    wr_ptr(ARG(7), H2G(t));
    RET(D3D_OK);
}
static void dv_CreateVolumeTexture(X86State *st) { // (this, w, h, d, levels, usage, fmt, pool, pp, shared)
    D9Dev *d = DV(st);
    if (!ARG(8)) { RET(D3DERR_INVALIDCALL); return; }
    wr_ptr(ARG(8), 0);
    uint32_t hr;
    D9Tex *t = d9_tex_create(d, SG_TEX_VOLUME, (int)ARG(1), (int)ARG(2), (int)ARG(3), (int)ARG(4), (int)ARG(5), (int)ARG(6), (int)ARG(7), &hr);
    if (!t) { RET(hr); return; }
    d9_addref(&t->o);
    wr_ptr(ARG(8), H2G(t));
    RET(D3D_OK);
}
static void dv_CreateCubeTexture(X86State *st) { // (this, edge, levels, usage, fmt, pool, pp, shared)
    D9Dev *d = DV(st);
    if (!ARG(6)) { RET(D3DERR_INVALIDCALL); return; }
    wr_ptr(ARG(6), 0);
    uint32_t hr;
    D9Tex *t = d9_tex_create(d, SG_TEX_CUBE, (int)ARG(1), (int)ARG(1), 1, (int)ARG(2), (int)ARG(3), (int)ARG(4), (int)ARG(5), &hr);
    if (!t) { RET(hr); return; }
    d9_addref(&t->o);
    wr_ptr(ARG(6), H2G(t));
    RET(D3D_OK);
}
static void dv_CreateVertexBuffer(X86State *st) { // (this, length, usage, fvf, pool, pp, shared)
    D9Dev *d = DV(st);
    if (!ARG(5) || ARG(1) == 0 || ARG(1) > 0x20000000u) { RET(D3DERR_INVALIDCALL); return; }
    D9Buf *b = d9_buf_create(d, 0, (uint32_t)ARG(1), (uint32_t)ARG(2), (uint32_t)ARG(3), (uint32_t)ARG(4));
    if (!b) { RET(E_OUTOFMEMORY_); return; }
    d9_addref(&b->o);
    wr_ptr(ARG(5), H2G(b));
    RET(D3D_OK);
}
static void dv_CreateIndexBuffer(X86State *st) { // (this, length, usage, format, pool, pp, shared)
    D9Dev *d = DV(st);
    if (!ARG(5) || ARG(1) == 0 || ARG(1) > 0x20000000u || (ARG(3) != F_INDEX16 && ARG(3) != F_INDEX32)) { RET(D3DERR_INVALIDCALL); return; }
    D9Buf *b = d9_buf_create(d, 1, (uint32_t)ARG(1), (uint32_t)ARG(2), (uint32_t)ARG(3), (uint32_t)ARG(4));
    if (!b) { RET(E_OUTOFMEMORY_); return; }
    d9_addref(&b->o);
    wr_ptr(ARG(5), H2G(b));
    RET(D3D_OK);
}
static uint32_t create_rt(D9Dev *d, int w, int h, int fmt, int ms, uint64_t pp) {
    if (!pp) return D3DERR_INVALIDCALL;
    wr_ptr(pp, 0);
    if (w < 1 || h < 1 || w > 8192 || h > 8192 || !d9_fmt_rt_ok(fmt) || ms != 0) return D3DERR_INVALIDCALL;
    D9Surf *s = d9_surf_create(d, w, h, fmt, USAGE_RENDERTARGET, POOL_DEFAULT, 1, 0, NULL);
    if (!s) return E_OUTOFMEMORY_;
    d9_addref(&s->o);
    wr_ptr(pp, H2G(s));
    return D3D_OK;
}
static uint32_t create_ds(D9Dev *d, int w, int h, int fmt, int ms, uint64_t pp) {
    if (!pp) return D3DERR_INVALIDCALL;
    wr_ptr(pp, 0);
    if (w < 1 || h < 1 || w > 8192 || h > 8192 || !d9_fmt_is_depth(fmt) || ms != 0) return D3DERR_INVALIDCALL;
    D9Surf *s = d9_surf_create(d, w, h, fmt, USAGE_DEPTHSTENCIL, POOL_DEFAULT, 0, 1, NULL);
    if (!s) return E_OUTOFMEMORY_;
    d9_addref(&s->o);
    wr_ptr(pp, H2G(s));
    return D3D_OK;
}
static uint32_t create_plain(D9Dev *d, int w, int h, int fmt, int pool, uint64_t pp) {
    if (!pp) return D3DERR_INVALIDCALL;
    wr_ptr(pp, 0);
    if (w < 1 || h < 1 || w > 8192 || h > 8192 || !d9_fmt_known(fmt)) return D3DERR_INVALIDCALL;
    D9Surf *s = d9_surf_create(d, w, h, fmt, 0, pool, 0, 0, NULL);
    if (!s) return E_OUTOFMEMORY_;
    d9_addref(&s->o);
    wr_ptr(pp, H2G(s));
    return D3D_OK;
}
static void dv_CreateRenderTarget(X86State *st) { RET(create_rt(DV(st), (int)ARG(1), (int)ARG(2), (int)ARG(3), (int)ARG(4), ARG(7))); }
static void dv_CreateDepthStencilSurface(X86State *st) { RET(create_ds(DV(st), (int)ARG(1), (int)ARG(2), (int)ARG(3), (int)ARG(4), ARG(7))); }
static void dv_CreateOffscreenPlainSurface(X86State *st) { RET(create_plain(DV(st), (int)ARG(1), (int)ARG(2), (int)ARG(3), (int)ARG(4), ARG(5))); }
static void dv_CreateRenderTargetEx(X86State *st) { RET(create_rt(DV(st), (int)ARG(1), (int)ARG(2), (int)ARG(3), (int)ARG(4), ARG(7))); }
static void dv_CreateOffscreenPlainSurfaceEx(X86State *st) { RET(create_plain(DV(st), (int)ARG(1), (int)ARG(2), (int)ARG(3), (int)ARG(4), ARG(5))); }
static void dv_CreateDepthStencilSurfaceEx(X86State *st) { RET(create_ds(DV(st), (int)ARG(1), (int)ARG(2), (int)ARG(3), (int)ARG(4), ARG(7))); }

// ---- copying -----------------------------------------------------------------------------------------------------------------------
static D9Surf *SF(uint64_t a) { D9Obj *o = (D9Obj *)G2H(a); return a && o->type == OT_SURF ? (D9Surf *)o : NULL; }
static void dv_UpdateSurface(X86State *st) { // (this, src, srcRect, dst, dstPoint)
    D9Surf *s = SF(ARG(1)), *t = SF(ARG(3));
    if (!s || !t || s->depth || t->depth) { RET(D3DERR_INVALIDCALL); return; }
    int32_t sr[4], dr[4];
    int hs = rd_rect(ARG(2), sr);
    if (!hs) { sr[0] = 0; sr[1] = 0; sr[2] = s->w; sr[3] = s->h; }
    int32_t dx = 0, dy = 0;
    if (ARG(4)) { dx = (int32_t)mem_rd(ARG(4), 4); dy = (int32_t)mem_rd(ARG(4) + 4, 4); }
    dr[0] = dx; dr[1] = dy; dr[2] = dx + (sr[2] - sr[0]); dr[3] = dy + (sr[3] - sr[1]);
    RET(d9_surf_copy(t, dr, s, sr, 1));
}
static void dv_UpdateTexture(X86State *st) { // (this, src, dst)
    D9Obj *a = (D9Obj *)G2H(ARG(1)), *b = (D9Obj *)G2H(ARG(2));
    if (!ARG(1) || !ARG(2) || a->type != OT_TEX || b->type != OT_TEX || a->kind != b->kind) { RET(D3DERR_INVALIDCALL); return; }
    D9Tex *s = (D9Tex *)a, *t = (D9Tex *)b;
    int faces = s->o.kind == SG_TEX_CUBE ? 6 : 1;
    uint32_t hr = D3D_OK;
    for (int dl = 0; dl < t->levels; dl++) {
        int sl = -1;
        for (int k = 0; k < s->levels; k++) {
            int w = s->w >> k; if (w < 1) w = 1;
            int h = s->h >> k; if (h < 1) h = 1;
            int dw = t->w >> dl; if (dw < 1) dw = 1;
            int dh = t->h >> dl; if (dh < 1) dh = 1;
            if (w == dw && h == dh) { sl = k; break; }
        }
        if (sl < 0) { hr = D3DERR_INVALIDCALL; continue; }
        if (t->o.kind == SG_TEX_VOLUME) {
            D9Vol *sv = s->vol[sl], *tv = t->vol[dl];
            if (sv->fmt == tv->fmt && sv->w == tv->w && sv->h == tv->h && sv->d == tv->d) { memcpy(tv->data, sv->data, (size_t)sv->slicepitch * (size_t)sv->d); tv->modified = 1; t->dirty = 1; }
            continue;
        }
        for (int f = 0; f < faces; f++) { uint32_t r = d9_surf_copy(t->surf[f][dl], NULL, s->surf[f][sl], NULL, 1); if (r != D3D_OK) hr = r; }
    }
    t->dirty = 1;
    RET(hr);
}
static void dv_GetRenderTargetData(X86State *st) { // (this, renderTarget, destSurface)
    D9Surf *s = SF(ARG(1)), *t = SF(ARG(2));
    if (!s || !t || s->depth || t->depth || s->w != t->w || s->h != t->h) { RET(D3DERR_INVALIDCALL); return; }
    RET(d9_surf_copy(t, NULL, s, NULL, 1));
}
static void dv_GetFrontBufferData(X86State *st) { // (this, swapchain, dest)
    D9Dev *d = DV(st);
    D9Surf *t = SF(ARG(2));
    if (ARG(1) >= (uint64_t)d->nswap || !t || t->depth) { RET(D3DERR_INVALIDCALL); return; }
    RET(d9_surf_copy(t, NULL, d->swap[ARG(1)]->back[0], NULL, 1));
}
static void dv_StretchRect(X86State *st) { // (this, src, srcRect, dst, dstRect, filter)
    D9Surf *s = SF(ARG(1)), *t = SF(ARG(3));
    if (!s || !t) { RET(D3DERR_INVALIDCALL); return; }
    int32_t sr[4], dr[4];
    int hs = rd_rect(ARG(2), sr), ht = rd_rect(ARG(4), dr);
    if (s->depth || t->depth) {
        if (!(s->depth && t->depth) || s->w != t->w || s->h != t->h) { RET(D3DERR_INVALIDCALL); return; }
        memcpy(t->depth, s->depth, (size_t)s->w * (size_t)s->h * sizeof(float));
        memcpy(t->stencil, s->stencil, (size_t)s->w * (size_t)s->h);
        RET(D3D_OK);
        return;
    }
    RET(d9_surf_copy(t, ht ? dr : NULL, s, hs ? sr : NULL, (int)ARG(5)));
}
static void dv_ColorFill(X86State *st) { // (this, surface, rect, color)
    D9Surf *s = SF(ARG(1));
    if (!s || s->depth) { RET(D3DERR_INVALIDCALL); return; }
    int32_t r[4];
    int hr_ = rd_rect(ARG(2), r);
    if (!hr_) { r[0] = 0; r[1] = 0; r[2] = s->w; r[3] = s->h; }
    if (r[0] < 0 || r[1] < 0 || r[2] > s->w || r[3] > s->h || r[0] >= r[2] || r[1] >= r[3]) { RET(D3DERR_INVALIDCALL); return; }
    size_t n = (size_t)(r[2] - r[0]) * (size_t)(r[3] - r[1]);
    uint32_t *buf = malloc(n * 4);
    for (size_t i = 0; i < n; i++) buf[i] = (uint32_t)ARG(3);
    int ok = d9_surf_write_argb(s, r, buf);
    free(buf);
    RET(ok ? D3D_OK : D3DERR_INVALIDCALL);
}
static void dv_SetRenderTarget(X86State *st) { // (this, index, surface)
    D9Dev *d = DV(st);
    uint32_t idx = (uint32_t)ARG(1);
    D9Surf *s = SF(ARG(2));
    if (idx >= 4) { RET(D3DERR_INVALIDCALL); return; }
    if (idx > 0) { RET(ARG(2) ? D3DERR_INVALIDCALL : D3D_OK); return; }
    if (!s || !s->argb) { RET(D3DERR_INVALIDCALL); return; }
    ref_set((D9Obj **)&d->rt, &s->o);
    d->st.vp = (D3DViewport9){ 0, 0, (uint32_t)s->w, (uint32_t)s->h, 0.0f, 1.0f };
    d->st.scissor[0] = 0; d->st.scissor[1] = 0; d->st.scissor[2] = s->w; d->st.scissor[3] = s->h;
    RET(D3D_OK);
}
static void dv_GetRenderTarget(X86State *st) {
    D9Dev *d = DV(st);
    uint32_t idx = (uint32_t)ARG(1);
    if (!ARG(2) || idx >= 4) { RET(D3DERR_INVALIDCALL); return; }
    wr_ptr(ARG(2), 0);
    if (idx > 0 || !d->rt) { RET(D3DERR_NOTFOUND); return; }
    d9_addref(&d->rt->o);
    wr_ptr(ARG(2), H2G(d->rt));
    RET(D3D_OK);
}
static void dv_SetDepthStencilSurface(X86State *st) {
    D9Dev *d = DV(st);
    D9Surf *s = ARG(1) ? SF(ARG(1)) : NULL;
    if (ARG(1) && (!s || !s->depth)) { RET(D3DERR_INVALIDCALL); return; }
    ref_set((D9Obj **)&d->ds, s ? &s->o : NULL);
    RET(D3D_OK);
}
static void dv_GetDepthStencilSurface(X86State *st) {
    D9Dev *d = DV(st);
    if (!ARG(1)) { RET(D3DERR_INVALIDCALL); return; }
    wr_ptr(ARG(1), 0);
    if (!d->ds) { RET(D3DERR_NOTFOUND); return; }
    d9_addref(&d->ds->o);
    wr_ptr(ARG(1), H2G(d->ds));
    RET(D3D_OK);
}
static void dv_BeginScene(X86State *st) { D9Dev *d = DV(st); if (d->in_scene) { RET(D3DERR_INVALIDCALL); return; } d->in_scene = 1; RET(D3D_OK); }
static void dv_EndScene(X86State *st) { D9Dev *d = DV(st); if (!d->in_scene) { RET(D3DERR_INVALIDCALL); return; } d->in_scene = 0; RET(D3D_OK); }
static void dv_Clear(X86State *st) { // (this, count, rects, flags, color, z, stencil)
    RET(d9_clear(DV(st), (uint32_t)ARG(1), ARG(2), (uint32_t)ARG(3), (uint32_t)ARG(4), d9_farg(st, 5), (uint32_t)ARG(6)));
}

// ---- transforms, viewport, material, lights ---------------------------------------------------------------------------------------
#define TGT D9State *S = dev->rec ? dev->rec->st : &dev->st; D9Mask *M = dev->rec ? &dev->rec->m : NULL
static int xf_idx(uint32_t t) { if (t == 2 || t == 3) return (int)t; if (t >= 16 && t < 24) return (int)t; if (t == 256) return 24; return -1; }
static void dv_SetTransform(X86State *st) {
    D9Dev *dev = DV(st);
    int i = xf_idx((uint32_t)ARG(1));
    if (!ARG(2)) { RET(D3DERR_INVALIDCALL); return; }
    if (i < 0) { RET((ARG(1) > 256 && ARG(1) < 512) ? D3D_OK : D3DERR_INVALIDCALL); return; }
    TGT;
    memcpy(S->xf[i], G2H(ARG(2)), 64);
    if (M) M->xf[i] = 1;
    RET(D3D_OK);
}
static void dv_GetTransform(X86State *st) {
    D9Dev *dev = DV(st);
    int i = xf_idx((uint32_t)ARG(1));
    if (!ARG(2)) { RET(D3DERR_INVALIDCALL); return; }
    if (i < 0) { float m[16]; ident(m); memcpy(G2H(ARG(2)), m, 64); RET((ARG(1) > 256 && ARG(1) < 512) ? D3D_OK : D3DERR_INVALIDCALL); return; }
    memcpy(G2H(ARG(2)), dev->st.xf[i], 64);
    RET(D3D_OK);
}
static void dv_MultiplyTransform(X86State *st) {
    D9Dev *dev = DV(st);
    int i = xf_idx((uint32_t)ARG(1));
    if (!ARG(2)) { RET(D3DERR_INVALIDCALL); return; }
    if (i < 0) { RET((ARG(1) > 256 && ARG(1) < 512) ? D3D_OK : D3DERR_INVALIDCALL); return; }
    TGT;
    float a[16], t[16];
    memcpy(a, G2H(ARG(2)), 64);
    for (int r = 0; r < 4; r++) for (int c = 0; c < 4; c++) { float s = 0; for (int k = 0; k < 4; k++) s += a[r * 4 + k] * S->xf[i][k * 4 + c]; t[r * 4 + c] = s; }  // pMatrix x State (row vectors: pMatrix applies first)
    memcpy(S->xf[i], t, 64);
    if (M) M->xf[i] = 1;
    RET(D3D_OK);
}
static void dv_SetViewport(X86State *st) {
    D9Dev *dev = DV(st);
    if (!ARG(1)) { RET(D3DERR_INVALIDCALL); return; }
    TGT;
    D3DViewport9 v;
    memcpy(&v, G2H(ARG(1)), sizeof v);
    if (dev->rt && !dev->rec && (v.x + v.w > (uint32_t)dev->rt->w || v.y + v.h > (uint32_t)dev->rt->h || !v.w || !v.h)) { RET(D3DERR_INVALIDCALL); return; }
    S->vp = v;
    if (M) M->vp = 1;
    RET(D3D_OK);
}
static void dv_GetViewport(X86State *st) { D9Dev *dev = DV(st); if (!ARG(1)) { RET(D3DERR_INVALIDCALL); return; } memcpy(G2H(ARG(1)), &dev->st.vp, sizeof dev->st.vp); RET(D3D_OK); }
static void dv_SetMaterial(X86State *st) {
    D9Dev *dev = DV(st);
    if (!ARG(1)) { RET(D3DERR_INVALIDCALL); return; }
    TGT;
    memcpy(&S->mat, G2H(ARG(1)), sizeof S->mat);
    if (M) M->mat = 1;
    RET(D3D_OK);
}
static void dv_GetMaterial(X86State *st) { D9Dev *dev = DV(st); if (!ARG(1)) { RET(D3DERR_INVALIDCALL); return; } memcpy(G2H(ARG(1)), &dev->st.mat, sizeof dev->st.mat); RET(D3D_OK); }
static void dv_SetLight(X86State *st) {
    D9Dev *dev = DV(st);
    uint32_t i = (uint32_t)ARG(1);
    if (!ARG(2) || i >= D9_NLIGHT) { RET(D3DERR_INVALIDCALL); return; }
    D3DLight9 l;
    memcpy(&l, G2H(ARG(2)), sizeof l);
    if (l.type < 1 || l.type > 3) { RET(D3DERR_INVALIDCALL); return; }
    TGT;
    S->light[i] = l; S->light_set[i] = 1;
    if (M) M->light[i] = 1;
    RET(D3D_OK);
}
static void dv_GetLight(X86State *st) {
    D9Dev *dev = DV(st);
    uint32_t i = (uint32_t)ARG(1);
    if (!ARG(2) || i >= D9_NLIGHT || !dev->st.light_set[i]) { RET(D3DERR_INVALIDCALL); return; }
    memcpy(G2H(ARG(2)), &dev->st.light[i], sizeof(D3DLight9));
    RET(D3D_OK);
}
static void dv_LightEnable(X86State *st) {
    D9Dev *dev = DV(st);
    uint32_t i = (uint32_t)ARG(1);
    if (i >= D9_NLIGHT) { RET(D3DERR_INVALIDCALL); return; }
    TGT;
    if (!S->light_set[i] && !dev->rec) {
        D3DLight9 *l = &S->light[i];
        memset(l, 0, sizeof *l);
        l->type = 1; l->diffuse[0] = l->diffuse[1] = l->diffuse[2] = 1; l->dir[2] = 1;
        S->light_set[i] = 1;
    }
    S->light_on[i] = ARG(2) ? 1 : 0;
    if (M) M->light_on[i] = 1;
    RET(D3D_OK);
}
static void dv_GetLightEnable(X86State *st) {
    D9Dev *dev = DV(st);
    uint32_t i = (uint32_t)ARG(1);
    if (!ARG(2) || i >= D9_NLIGHT || !dev->st.light_set[i]) { RET(D3DERR_INVALIDCALL); return; }
    d9_wr32(ARG(2), dev->st.light_on[i]);
    RET(D3D_OK);
}
static void dv_SetClipPlane(X86State *st) {
    D9Dev *dev = DV(st);
    uint32_t i = (uint32_t)ARG(1);
    if (!ARG(2) || i >= 6) { RET(D3DERR_INVALIDCALL); return; }
    TGT;
    memcpy(S->clip[i], G2H(ARG(2)), 16);
    if (M) M->clip[i] = 1;
    RET(D3D_OK);
}
static void dv_GetClipPlane(X86State *st) {
    D9Dev *dev = DV(st);
    uint32_t i = (uint32_t)ARG(1);
    if (!ARG(2) || i >= 6) { RET(D3DERR_INVALIDCALL); return; }
    memcpy(G2H(ARG(2)), dev->st.clip[i], 16);
    RET(D3D_OK);
}

// ---- render / sampler / stage states ------------------------------------------------------------------------------------------------
static void dv_SetRenderState(X86State *st) {
    D9Dev *dev = DV(st);
    uint32_t i = (uint32_t)ARG(1);
    if (i == 0 || i >= 256) { RET(D3DERR_INVALIDCALL); return; }
    TGT;
    S->rs[i] = (uint32_t)ARG(2);
    if (M) M->rs[i] = 1;
    RET(D3D_OK);
}
static void dv_GetRenderState(X86State *st) {
    D9Dev *dev = DV(st);
    uint32_t i = (uint32_t)ARG(1);
    if (i == 0 || i >= 256 || !ARG(2)) { RET(D3DERR_INVALIDCALL); return; }
    d9_wr32(ARG(2), dev->st.rs[i]);
    RET(D3D_OK);
}
static void dv_SetSamplerState(X86State *st) { // (this, sampler, type, value)
    D9Dev *dev = DV(st);
    int s = samp_idx((uint32_t)ARG(1));
    uint32_t t = (uint32_t)ARG(2);
    if (s < 0 || t < 1 || t > 13) { RET(D3DERR_INVALIDCALL); return; }
    TGT;
    S->ss[s][t] = (uint32_t)ARG(3);
    if (M) M->ss[s][t] = 1;
    RET(D3D_OK);
}
static void dv_GetSamplerState(X86State *st) {
    D9Dev *dev = DV(st);
    int s = samp_idx((uint32_t)ARG(1));
    uint32_t t = (uint32_t)ARG(2);
    if (s < 0 || t < 1 || t > 13 || !ARG(3)) { RET(D3DERR_INVALIDCALL); return; }
    d9_wr32(ARG(3), dev->st.ss[s][t]);
    RET(D3D_OK);
}
static void dv_SetTextureStageState(X86State *st) { // (this, stage, type, value)
    D9Dev *dev = DV(st);
    uint32_t s = (uint32_t)ARG(1), t = (uint32_t)ARG(2);
    if (s >= 8 || t < 1 || t > 32) { RET(D3DERR_INVALIDCALL); return; }
    TGT;
    S->tss[s][t] = (uint32_t)ARG(3);
    if (M) M->tss[s][t] = 1;
    RET(D3D_OK);
}
static void dv_GetTextureStageState(X86State *st) {
    D9Dev *dev = DV(st);
    uint32_t s = (uint32_t)ARG(1), t = (uint32_t)ARG(2);
    if (s >= 8 || t < 1 || t > 32 || !ARG(3)) { RET(D3DERR_INVALIDCALL); return; }
    d9_wr32(ARG(3), dev->st.tss[s][t]);
    RET(D3D_OK);
}
uint32_t d9_bind_texture(D9Dev *dev, uint32_t stage, D9Obj *tex) {
    int s = samp_idx(stage);
    if (s < 0) return D3DERR_INVALIDCALL;
    if (tex && tex->type != OT_TEX) return D3DERR_INVALIDCALL;
    TGT;
    ref_set(&S->tex[s], tex);
    if (M) M->tex[s] = 1;
    return D3D_OK;
}
static void dv_SetTexture(X86State *st) { RET(d9_bind_texture(DV(st), (uint32_t)ARG(1), ARG(2) ? (D9Obj *)G2H(ARG(2)) : NULL)); }
static void dv_GetTexture(X86State *st) {
    D9Dev *dev = DV(st);
    int s = samp_idx((uint32_t)ARG(1));
    if (s < 0 || !ARG(2)) { RET(D3DERR_INVALIDCALL); return; }
    D9Obj *t = dev->st.tex[s];
    if (t) d9_addref(t);
    wr_ptr(ARG(2), H2G(t));
    RET(D3D_OK);
}
static void dv_ValidateDevice(X86State *st) { if (ARG(1)) d9_wr32(ARG(1), 1); RET(D3D_OK); }
static void dv_SetPaletteEntries(X86State *st) { // (this, palnum, PALETTEENTRY *)
    D9Dev *d = DV(st);
    if (!ARG(2) || ARG(1) > 0xFFFF) { RET(D3DERR_INVALIDCALL); return; }
    if (!d->pal) d->pal = calloc(256, 256 * 4);
    uint32_t *p = d->pal[ARG(1) & 255];
    const uint8_t *e = G2H(ARG(2));
    for (int i = 0; i < 256; i++) p[i] = 0xFF000000u | ((uint32_t)e[4 * i] << 16) | ((uint32_t)e[4 * i + 1] << 8) | e[4 * i + 2];
    RET(D3D_OK);
}
static void dv_GetPaletteEntries(X86State *st) {
    D9Dev *d = DV(st);
    if (!ARG(2) || ARG(1) > 0xFFFF) { RET(D3DERR_INVALIDCALL); return; }
    uint8_t *e = G2H(ARG(2));
    for (int i = 0; i < 256; i++) {
        uint32_t c = d->pal ? d->pal[ARG(1) & 255][i] : 0;
        e[4 * i] = (uint8_t)(c >> 16); e[4 * i + 1] = (uint8_t)(c >> 8); e[4 * i + 2] = (uint8_t)c; e[4 * i + 3] = 0xFF;
    }
    RET(D3D_OK);
}
static void dv_SetCurrentTexturePalette(X86State *st) { DV(st)->cur_pal = (uint32_t)ARG(1); RET(D3D_OK); }
static void dv_GetCurrentTexturePalette(X86State *st) { if (!ARG(1)) { RET(D3DERR_INVALIDCALL); return; } d9_wr32(ARG(1), DV(st)->cur_pal); RET(D3D_OK); }
static void dv_SetScissorRect(X86State *st) {
    D9Dev *dev = DV(st);
    int32_t r[4];
    if (!rd_rect(ARG(1), r)) { RET(D3DERR_INVALIDCALL); return; }
    TGT;
    memcpy(S->scissor, r, 16);
    if (M) M->scissor = 1;
    RET(D3D_OK);
}
static void dv_GetScissorRect(X86State *st) { D9Dev *dev = DV(st); if (!ARG(1)) { RET(D3DERR_INVALIDCALL); return; } memcpy(G2H(ARG(1)), dev->st.scissor, 16); RET(D3D_OK); }
static void dv_SetSoftwareVertexProcessing(X86State *st) { D9Dev *dev = DV(st); TGT; S->sw_vp = ARG(1) != 0; if (M) M->sw = 1; RET(D3D_OK); }
static void dv_GetSoftwareVertexProcessing(X86State *st) { RET(DV(st)->st.sw_vp); }
static void dv_SetNPatchMode(X86State *st) { D9Dev *dev = DV(st); TGT; S->npatch = d9_farg(st, 1); if (M) M->npatch = 1; RET(D3D_OK); }
static void dv_GetNPatchMode(X86State *st) { if (!g_mode32) st->xmm[0].f[0] = DV(st)->st.npatch; RET(0); }

// ---- drawing ---------------------------------------------------------------------------------------------------------------------------
static void dv_DrawPrimitive(X86State *st) { RET(d9_draw(DV(st), (int)ARG(1), 0, 0, 0, (uint32_t)ARG(2), (uint32_t)ARG(3), 0, NULL, 0, NULL, 0)); }
static void dv_DrawIndexedPrimitive(X86State *st) { // (this, type, base, minIndex, numVerts, startIndex, primCount)
    RET(d9_draw(DV(st), (int)ARG(1), (int)(int32_t)ARG(2), (uint32_t)ARG(3), (uint32_t)ARG(4), (uint32_t)ARG(5), (uint32_t)ARG(6), 1, NULL, 0, NULL, 0));
}
static void dv_DrawPrimitiveUP(X86State *st) { // (this, type, primCount, pData, stride)
    D9Dev *d = DV(st);
    if (!ARG(3) || !ARG(4)) { RET(D3DERR_INVALIDCALL); return; }
    uint32_t hr = d9_draw(d, (int)ARG(1), 0, 0, 0, 0, (uint32_t)ARG(2), 0, G2H(ARG(3)), (uint32_t)ARG(4), NULL, 0);
    ref_set((D9Obj **)&d->st.stream[0].vb, NULL);
    RET(hr);
}
static void dv_DrawIndexedPrimitiveUP(X86State *st) { // (this, type, minIndex, numVerts, primCount, pIndex, indexFmt, pVerts, stride)
    D9Dev *d = DV(st);
    if (!ARG(5) || !ARG(7) || !ARG(8)) { RET(D3DERR_INVALIDCALL); return; }
    uint32_t hr = d9_draw(d, (int)ARG(1), 0, (uint32_t)ARG(2), (uint32_t)ARG(3), 0, (uint32_t)ARG(4), 1, G2H(ARG(7)), (uint32_t)ARG(8), G2H(ARG(5)), ARG(6) == F_INDEX32);
    ref_set((D9Obj **)&d->st.stream[0].vb, NULL);
    ref_set((D9Obj **)&d->st.ib, NULL);
    RET(hr);
}
static void dv_ProcessVertices(X86State *st) { // (this, srcStart, destIndex, count, destBuffer, decl, flags)
    D9Buf *b = ARG(4) ? (D9Buf *)G2H(ARG(4)) : NULL;
    D9Decl *dc = ARG(5) ? (D9Decl *)G2H(ARG(5)) : NULL;
    RET(d9_process_vertices(DV(st), (uint32_t)ARG(1), (uint32_t)ARG(2), (uint32_t)ARG(3), b, dc, (uint32_t)ARG(6)));
}

// ---- declarations, FVF, shaders, streams ----------------------------------------------------------------------------------------------------
static void dv_CreateVertexDeclaration(X86State *st) { // (this, D3DVERTEXELEMENT9 *, ppDecl)
    D9Dev *d = DV(st);
    if (!ARG(1) || !ARG(2)) { RET(D3DERR_INVALIDCALL); return; }
    D9Elem e[17];
    int n = 0;
    for (;;) {
        uint64_t p = ARG(1) + 8 * (uint64_t)n;
        uint16_t stream = (uint16_t)mem_rd(p, 2);
        if (stream == 0xFF) break;
        if (n >= 16) { RET(D3DERR_INVALIDCALL); return; }
        e[n].stream = stream; e[n].offset = (uint16_t)mem_rd(p + 2, 2);
        e[n].type = (uint8_t)mem_rd(p + 4, 1); e[n].method = (uint8_t)mem_rd(p + 5, 1);
        e[n].usage = (uint8_t)mem_rd(p + 6, 1); e[n].index = (uint8_t)mem_rd(p + 7, 1);
        if (e[n].type > 16 || stream > 15) { RET(D3DERR_INVALIDCALL); return; }
        n++;
    }
    if (n == 0) { RET(D3DERR_INVALIDCALL); return; }
    D9Decl *dc = d9_decl_create(d, e, n, 0);
    if (!dc) { RET(E_OUTOFMEMORY_); return; }
    d9_addref(&dc->o);
    wr_ptr(ARG(2), H2G(dc));
    RET(D3D_OK);
}
static void dv_SetVertexDeclaration(X86State *st) {
    D9Dev *dev = DV(st);
    D9Decl *dc = ARG(1) ? (D9Decl *)G2H(ARG(1)) : NULL;
    if (dc && dc->o.type != OT_DECL) { RET(D3DERR_INVALIDCALL); return; }
    TGT;
    ref_set((D9Obj **)&S->decl, dc ? &dc->o : NULL);
    S->fvf = dc ? dc->fvf : 0;
    if (M) { M->decl = 1; M->fvf = 1; }
    RET(D3D_OK);
}
static void dv_GetVertexDeclaration(X86State *st) {
    D9Dev *dev = DV(st);
    if (!ARG(1)) { RET(D3DERR_INVALIDCALL); return; }
    D9Decl *dc = dev->st.decl;
    if (dc) d9_addref(&dc->o);
    wr_ptr(ARG(1), H2G(dc));
    RET(D3D_OK);
}
static void dv_SetFVF(X86State *st) {
    D9Dev *dev = DV(st);
    uint32_t fvf = (uint32_t)ARG(1);
    TGT;
    if (fvf == 0) { RET(D3D_OK); return; }
    D9Decl *dc = NULL;
    for (int i = 0; i < dev->nfvfc; i++) if (dev->fvfc[i]->fvf == fvf) { dc = dev->fvfc[i]; break; }
    if (!dc) {
        dc = d9_decl_from_fvf(dev, fvf);
        if (!dc) { RET(E_OUTOFMEMORY_); return; }
        if (dev->nfvfc < 16) { dev->fvfc[dev->nfvfc++] = dc; d9_hold(&dc->o); }
        else { dev->fvfc[0] = dc; d9_hold(&dc->o); }
    }
    ref_set((D9Obj **)&S->decl, &dc->o);
    S->fvf = fvf;
    if (M) { M->decl = 1; M->fvf = 1; }
    RET(D3D_OK);
}
static void dv_GetFVF(X86State *st) { if (!ARG(1)) { RET(D3DERR_INVALIDCALL); return; } d9_wr32(ARG(1), DV(st)->st.fvf); RET(D3D_OK); }
static uint32_t make_shader(D9Dev *d, uint64_t fn, uint64_t pp, int is_ps) {
    if (!fn || !pp) return D3DERR_INVALIDCALL;
    wr_ptr(pp, 0);
    const uint32_t *tk = G2H(fn);
    size_t n = 0;
    for (; n < 65536; n++) {
        uint32_t t = tk[n];
        if (t == 0x0000FFFFu) { n++; break; }
        if ((t & 0xFFFF) == 0xFFFE) n += (t >> 16) & 0x7FFF;
    }
    char err[128];
    D3dShader *sh = d3dsh_create(tk, n, err, sizeof err);
    if (!sh) return D3DERR_INVALIDCALL;
    if ((d3dsh_is_pixel(sh) != 0) != is_ps) { d3dsh_free(sh); return D3DERR_INVALIDCALL; }
    D9Shader *s = d9_new(is_ps ? OT_PS : OT_VS, sizeof *s);
    if (!s) { d3dsh_free(sh); return E_OUTOFMEMORY_; }
    d9_obj_setvt(&s->o, is_ps ? VT_PS : VT_VS);
    s->o.dev = d;
    d9_hold(&d->o);
    s->sh = sh;
    s->is_ps = is_ps;
    d9_addref(&s->o);
    wr_ptr(pp, H2G(s));
    return D3D_OK;
}
void d9_shader_free(D9Shader *s) { if (s->sh) d3dsh_free(s->sh); s->sh = NULL; }
static void dv_CreateVertexShader(X86State *st) { RET(make_shader(DV(st), ARG(1), ARG(2), 0)); }
static void dv_CreatePixelShader(X86State *st) { RET(make_shader(DV(st), ARG(1), ARG(2), 1)); }
static void dv_SetVertexShader(X86State *st) {
    D9Dev *dev = DV(st);
    D9Shader *s = ARG(1) ? (D9Shader *)G2H(ARG(1)) : NULL;
    if (s && s->o.type != OT_VS) { RET(D3DERR_INVALIDCALL); return; }
    TGT;
    ref_set((D9Obj **)&S->vs, s ? &s->o : NULL);
    if (M) M->vs = 1;
    RET(D3D_OK);
}
static void dv_GetVertexShader(X86State *st) { D9Dev *d = DV(st); if (!ARG(1)) { RET(D3DERR_INVALIDCALL); return; } if (d->st.vs) d9_addref(&d->st.vs->o); wr_ptr(ARG(1), H2G(d->st.vs)); RET(D3D_OK); }
static void dv_SetPixelShader(X86State *st) {
    D9Dev *dev = DV(st);
    D9Shader *s = ARG(1) ? (D9Shader *)G2H(ARG(1)) : NULL;
    if (s && s->o.type != OT_PS) { RET(D3DERR_INVALIDCALL); return; }
    TGT;
    ref_set((D9Obj **)&S->ps, s ? &s->o : NULL);
    if (M) M->ps = 1;
    RET(D3D_OK);
}
static void dv_GetPixelShader(X86State *st) { D9Dev *d = DV(st); if (!ARG(1)) { RET(D3DERR_INVALIDCALL); return; } if (d->st.ps) d9_addref(&d->st.ps->o); wr_ptr(ARG(1), H2G(d->st.ps)); RET(D3D_OK); }

static void set_constf(X86State *st, int ps) {
    D9Dev *dev = DV(st);
    uint32_t start = (uint32_t)ARG(1), cnt = (uint32_t)ARG(3);
    if (!ARG(2) || start + cnt > (ps ? 224u : 256u) || start + cnt < start) { RET(D3DERR_INVALIDCALL); return; }
    TGT;
    D3dShaderConsts *c = ps ? &S->psc : &S->vsc;
    memcpy(c->f[start], G2H(ARG(2)), (size_t)cnt * 16);
    if (M) for (uint32_t i = 0; i < cnt; i++) (ps ? M->psf : M->vsf)[start + i] = 1;
    RET(D3D_OK);
}
static void get_constf(X86State *st, int ps) {
    D9Dev *dev = DV(st);
    uint32_t start = (uint32_t)ARG(1), cnt = (uint32_t)ARG(3);
    if (!ARG(2) || start + cnt > (ps ? 224u : 256u) || start + cnt < start) { RET(D3DERR_INVALIDCALL); return; }
    memcpy(G2H(ARG(2)), (ps ? &dev->st.psc : &dev->st.vsc)->f[start], (size_t)cnt * 16);
    RET(D3D_OK);
}
static void set_consti(X86State *st, int ps) {
    D9Dev *dev = DV(st);
    uint32_t start = (uint32_t)ARG(1), cnt = (uint32_t)ARG(3);
    if (!ARG(2) || start + cnt > 16 || start + cnt < start) { RET(D3DERR_INVALIDCALL); return; }
    TGT;
    D3dShaderConsts *c = ps ? &S->psc : &S->vsc;
    memcpy(c->i[start], G2H(ARG(2)), (size_t)cnt * 16);
    if (M) for (uint32_t i = 0; i < cnt; i++) (ps ? M->psi : M->vsi)[start + i] = 1;
    RET(D3D_OK);
}
static void get_consti(X86State *st, int ps) {
    D9Dev *dev = DV(st);
    uint32_t start = (uint32_t)ARG(1), cnt = (uint32_t)ARG(3);
    if (!ARG(2) || start + cnt > 16 || start + cnt < start) { RET(D3DERR_INVALIDCALL); return; }
    memcpy(G2H(ARG(2)), (ps ? &dev->st.psc : &dev->st.vsc)->i[start], (size_t)cnt * 16);
    RET(D3D_OK);
}
static void set_constb(X86State *st, int ps) {
    D9Dev *dev = DV(st);
    uint32_t start = (uint32_t)ARG(1), cnt = (uint32_t)ARG(3);
    if (!ARG(2) || start + cnt > 16 || start + cnt < start) { RET(D3DERR_INVALIDCALL); return; }
    TGT;
    D3dShaderConsts *c = ps ? &S->psc : &S->vsc;
    for (uint32_t i = 0; i < cnt; i++) { c->b[start + i] = d9_rd32(ARG(2) + 4 * (uint64_t)i) != 0; if (M) (ps ? M->psb : M->vsb)[start + i] = 1; }
    RET(D3D_OK);
}
static void get_constb(X86State *st, int ps) {
    D9Dev *dev = DV(st);
    uint32_t start = (uint32_t)ARG(1), cnt = (uint32_t)ARG(3);
    if (!ARG(2) || start + cnt > 16 || start + cnt < start) { RET(D3DERR_INVALIDCALL); return; }
    for (uint32_t i = 0; i < cnt; i++) d9_wr32(ARG(2) + 4 * (uint64_t)i, (ps ? dev->st.psc : dev->st.vsc).b[start + i] ? 1 : 0);
    RET(D3D_OK);
}
static void dv_SetVSF(X86State *st) { set_constf(st, 0); }
static void dv_GetVSF(X86State *st) { get_constf(st, 0); }
static void dv_SetVSI(X86State *st) { set_consti(st, 0); }
static void dv_GetVSI(X86State *st) { get_consti(st, 0); }
static void dv_SetVSB(X86State *st) { set_constb(st, 0); }
static void dv_GetVSB(X86State *st) { get_constb(st, 0); }
static void dv_SetPSF(X86State *st) { set_constf(st, 1); }
static void dv_GetPSF(X86State *st) { get_constf(st, 1); }
static void dv_SetPSI(X86State *st) { set_consti(st, 1); }
static void dv_GetPSI(X86State *st) { get_consti(st, 1); }
static void dv_SetPSB(X86State *st) { set_constb(st, 1); }
static void dv_GetPSB(X86State *st) { get_constb(st, 1); }

static void dv_SetStreamSource(X86State *st) { // (this, stream, vb, offset, stride)
    D9Dev *dev = DV(st);
    uint32_t s = (uint32_t)ARG(1);
    D9Buf *b = ARG(2) ? (D9Buf *)G2H(ARG(2)) : NULL;
    if (s >= 16 || (b && b->o.type != OT_VB)) { RET(D3DERR_INVALIDCALL); return; }
    TGT;
    ref_set((D9Obj **)&S->stream[s].vb, b ? &b->o : NULL);
    S->stream[s].offset = b ? (uint32_t)ARG(3) : 0;
    S->stream[s].stride = b ? (uint32_t)ARG(4) : 0;
    if (M) M->stream[s] = 1;
    RET(D3D_OK);
}
static void dv_GetStreamSource(X86State *st) {
    D9Dev *dev = DV(st);
    uint32_t s = (uint32_t)ARG(1);
    if (s >= 16 || !ARG(2)) { RET(D3DERR_INVALIDCALL); return; }
    D9Buf *b = dev->st.stream[s].vb;
    if (b) d9_addref(&b->o);
    wr_ptr(ARG(2), H2G(b));
    if (ARG(3)) d9_wr32(ARG(3), dev->st.stream[s].offset);
    if (ARG(4)) d9_wr32(ARG(4), dev->st.stream[s].stride);
    RET(D3D_OK);
}
static void dv_SetStreamSourceFreq(X86State *st) {
    D9Dev *dev = DV(st);
    uint32_t s = (uint32_t)ARG(1);
    if (s >= 16) { RET(D3DERR_INVALIDCALL); return; }
    TGT;
    S->stream[s].freq = (uint32_t)ARG(2);
    if (M) M->freq[s] = 1;
    RET(D3D_OK);
}
static void dv_GetStreamSourceFreq(X86State *st) {
    D9Dev *dev = DV(st);
    uint32_t s = (uint32_t)ARG(1);
    if (s >= 16 || !ARG(2)) { RET(D3DERR_INVALIDCALL); return; }
    d9_wr32(ARG(2), dev->st.stream[s].freq);
    RET(D3D_OK);
}
static void dv_SetIndices(X86State *st) {
    D9Dev *dev = DV(st);
    D9Buf *b = ARG(1) ? (D9Buf *)G2H(ARG(1)) : NULL;
    if (b && b->o.type != OT_IB) { RET(D3DERR_INVALIDCALL); return; }
    TGT;
    ref_set((D9Obj **)&S->ib, b ? &b->o : NULL);
    if (M) M->ib = 1;
    RET(D3D_OK);
}
static void dv_GetIndices(X86State *st) {
    D9Dev *dev = DV(st);
    if (!ARG(1)) { RET(D3DERR_INVALIDCALL); return; }
    D9Buf *b = dev->st.ib;
    if (b) d9_addref(&b->o);
    wr_ptr(ARG(1), H2G(b));
    RET(D3D_OK);
}
static void dv_Invalid(X86State *st) { RET(D3DERR_INVALIDCALL); }
static void dv_CreateQuery(X86State *st) {
    D9Dev *d = DV(st);
    int t = (int)ARG(1);
    if (!ARG(2)) { RET(t == 8 || t == 9 || t == 10 || t == 11 || t == 12 ? D3D_OK : D3DERR_NOTAVAILABLE); return; }
    wr_ptr(ARG(2), 0);
    D9Query *q = d9_query_create(d, t);
    if (!q) { RET(D3DERR_NOTAVAILABLE); return; }
    d9_addref(&q->o);
    wr_ptr(ARG(2), H2G(q));
    RET(D3D_OK);
}
static void dv_SetClipStatus(X86State *st) { RET(D3D_OK); }
static void dv_GetClipStatus(X86State *st) { if (!ARG(1)) { RET(D3DERR_INVALIDCALL); return; } d9_wr32(ARG(1), 0); d9_wr32(ARG(1) + 4, 0); RET(D3D_OK); }

// ---- state block methods --------------------------------------------------------------------------------------------------------------------
static void dv_CreateStateBlock(X86State *st) { // (this, type, ppSB)
    D9Dev *d = DV(st);
    uint32_t t = (uint32_t)ARG(1);
    if (t < 1 || t > 3 || !ARG(2) || d->rec) { RET(D3DERR_INVALIDCALL); return; }
    D9SB *sb = sb_new(d, (int)t);
    if (!sb) { RET(E_OUTOFMEMORY_); return; }
    mask_for_type(&sb->m, (int)t);
    copy_masked(sb->st, &d->st, &sb->m);
    d9_addref(&sb->o);
    wr_ptr(ARG(2), H2G(sb));
    RET(D3D_OK);
}
static void dv_BeginStateBlock(X86State *st) {
    D9Dev *d = DV(st);
    if (d->rec) { RET(D3DERR_INVALIDCALL); return; }
    D9SB *sb = sb_new(d, 0);
    if (!sb) { RET(E_OUTOFMEMORY_); return; }
    d9_addref(&sb->o);
    d->rec = sb;
    RET(D3D_OK);
}
static void dv_EndStateBlock(X86State *st) {
    D9Dev *d = DV(st);
    if (!d->rec || !ARG(1)) { RET(D3DERR_INVALIDCALL); return; }
    D9SB *sb = d->rec;
    d->rec = NULL;
    wr_ptr(ARG(1), H2G(sb));
    RET(D3D_OK);
}
static void sb_Capture(X86State *st) { D9SB *sb = D9OBJ(D9SB, 0); copy_masked(sb->st, &sb->o.dev->st, &sb->m); RET(D3D_OK); }
static void sb_Apply(X86State *st) { D9SB *sb = D9OBJ(D9SB, 0); copy_masked(&sb->o.dev->st, sb->st, &sb->m); RET(D3D_OK); }

// ---- Ex methods ------------------------------------------------------------------------------------------------------------------------------------
static void dv_PresentEx(X86State *st) { // (this, src, dst, hwnd, dirty, flags)
    D9Dev *d = DV(st);
    int32_t s[4], t[4];
    int hs = rd_rect(ARG(1), s), ht = rd_rect(ARG(2), t);
    RET(d9_present(d, d->swap[0], hs ? s : NULL, ht ? t : NULL, ARG(3)));
}
static void dv_GetGPUThreadPriority(X86State *st) { if (ARG(1)) d9_wr32(ARG(1), 0); RET(D3D_OK); }
static void dv_GetMaximumFrameLatency(X86State *st) { if (ARG(1)) d9_wr32(ARG(1), DV(st)->maxlatency); RET(D3D_OK); }
static void dv_SetMaximumFrameLatency(X86State *st) { DV(st)->maxlatency = (uint32_t)ARG(1); RET(D3D_OK); }
static void dv_ResetEx(X86State *st) { RET(do_reset(DV(st), ARG(1))); }
static void dv_GetDisplayModeEx(X86State *st) { // (this, swapchain, D3DDISPLAYMODEEX *, rotation *)
    int w, h;
    d9_screen_size(&w, &h);
    if (ARG(1) != 0) { RET(D3DERR_INVALIDCALL); return; }
    if (ARG(2)) { uint64_t p = ARG(2); d9_wr32(p, 24); d9_wr32(p + 4, (uint32_t)w); d9_wr32(p + 8, (uint32_t)h); d9_wr32(p + 12, 60); d9_wr32(p + 16, F_X8R8G8B8); d9_wr32(p + 20, 1); }
    if (ARG(3)) d9_wr32(ARG(3), 1);
    RET(D3D_OK);
}

// ---- swap chain methods -----------------------------------------------------------------------------------------------------------------------------
#define SW(n) ((D9Swap *)G2H(ARG(n)))
static void sc_Present(X86State *st) { // (this, src, dst, hwnd, dirty, flags)
    D9Swap *sw = SW(0);
    int32_t s[4], t[4];
    int hs = rd_rect(ARG(1), s), ht = rd_rect(ARG(2), t);
    RET(d9_present(sw->o.dev, sw, hs ? s : NULL, ht ? t : NULL, ARG(3)));
}
static void sc_GetFrontBufferData(X86State *st) {
    D9Swap *sw = SW(0);
    D9Surf *t = SF(ARG(1));
    if (!t || t->depth) { RET(D3DERR_INVALIDCALL); return; }
    RET(d9_surf_copy(t, NULL, sw->back[0], NULL, 1));
}
static void sc_GetBackBuffer(X86State *st) { // (this, index, type, pp)
    D9Swap *sw = SW(0);
    uint32_t i = (uint32_t)ARG(1);
    if (!ARG(3) || i >= sw->nback) { RET(D3DERR_INVALIDCALL); return; }
    d9_addref(&sw->back[i]->o);
    wr_ptr(ARG(3), H2G(sw->back[i]));
    RET(D3D_OK);
}
static void sc_GetRasterStatus(X86State *st) { if (!ARG(1)) { RET(D3DERR_INVALIDCALL); return; } d9_wr32(ARG(1), 0); d9_wr32(ARG(1) + 4, 0); RET(D3D_OK); }
static void sc_GetDisplayMode(X86State *st) {
    int w, h;
    d9_screen_size(&w, &h);
    if (!ARG(1)) { RET(D3DERR_INVALIDCALL); return; }
    d9_wr32(ARG(1), (uint32_t)w); d9_wr32(ARG(1) + 4, (uint32_t)h); d9_wr32(ARG(1) + 8, 60); d9_wr32(ARG(1) + 12, F_X8R8G8B8);
    RET(D3D_OK);
}
static void sc_GetDevice(X86State *st) {
    D9Swap *sw = SW(0);
    if (!ARG(1)) { RET(D3DERR_INVALIDCALL); return; }
    d9_addref(&sw->o.dev->o);
    wr_ptr(ARG(1), H2G(sw->o.dev));
    RET(D3D_OK);
}
static void sc_GetPresentParameters(X86State *st) {
    D9Swap *sw = SW(0);
    uint64_t pp = ARG(1);
    if (!pp) { RET(D3DERR_INVALIDCALL); return; }
    gs_set(pp, PP_SPEC, 0, sw->w); gs_set(pp, PP_SPEC, 1, sw->h); gs_set(pp, PP_SPEC, 2, sw->fmt); gs_set(pp, PP_SPEC, 3, sw->nback);
    gs_set(pp, PP_SPEC, 4, 0); gs_set(pp, PP_SPEC, 5, 0); gs_set(pp, PP_SPEC, 6, sw->effect); gs_set(pp, PP_SPEC, 7, sw->hwnd);
    gs_set(pp, PP_SPEC, 8, (uint64_t)sw->windowed); gs_set(pp, PP_SPEC, 9, (uint64_t)sw->autods); gs_set(pp, PP_SPEC, 10, sw->depth_fmt);
    gs_set(pp, PP_SPEC, 11, sw->flags); gs_set(pp, PP_SPEC, 12, sw->refresh); gs_set(pp, PP_SPEC, 13, sw->interval);
    RET(D3D_OK);
}
static void sc_GetLastPresentCount(X86State *st) { if (ARG(1)) d9_wr32(ARG(1), SW(0)->present_count); RET(D3D_OK); }
static void sc_GetPresentStats(X86State *st) { RET(D3DERR_INVALIDCALL); }

// ---- tables -------------------------------------------------------------------------------------------------------------------------------------------
void d9_m_QI(X86State *st);
void d9_m_AddRef(X86State *st);
void d9_m_Release(X86State *st);
#define U3 { "QueryInterface", d9_m_QI, 3 }, { "AddRef", d9_m_AddRef, 1 }, { "Release", d9_m_Release, 1 }
#define DEVM \
    U3, \
    { "TestCooperativeLevel", dv_TestCooperativeLevel, 1 }, { "GetAvailableTextureMem", dv_GetAvailableTextureMem, 1 }, { "EvictManagedResources", dv_EvictManagedResources, 1 }, \
    { "GetDirect3D", dv_GetDirect3D, 2 }, { "GetDeviceCaps", dv_GetDeviceCaps, 2 }, { "GetDisplayMode", dv_GetDisplayMode, 3 }, \
    { "GetCreationParameters", dv_GetCreationParameters, 2 }, { "SetCursorProperties", dv_SetCursorProperties, 4 }, { "SetCursorPosition", dv_SetCursorPosition, 4 }, \
    { "ShowCursor", dv_ShowCursor, 2 }, { "CreateAdditionalSwapChain", dv_CreateAdditionalSwapChain, 3 }, { "GetSwapChain", dv_GetSwapChain, 3 }, \
    { "GetNumberOfSwapChains", dv_GetNumberOfSwapChains, 1 }, { "Reset", dv_Reset, 2 }, { "Present", dv_Present, 5 }, { "GetBackBuffer", dv_GetBackBuffer, 5 }, \
    { "GetRasterStatus", dv_GetRasterStatus, 3 }, { "SetDialogBoxMode", dv_SetDialogBoxMode, 2 }, { "SetGammaRamp", dv_SetGammaRamp, 4 }, { "GetGammaRamp", dv_GetGammaRamp, 3 }, \
    { "CreateTexture", dv_CreateTexture, 9 }, { "CreateVolumeTexture", dv_CreateVolumeTexture, 10 }, { "CreateCubeTexture", dv_CreateCubeTexture, 8 }, \
    { "CreateVertexBuffer", dv_CreateVertexBuffer, 7 }, { "CreateIndexBuffer", dv_CreateIndexBuffer, 7 }, { "CreateRenderTarget", dv_CreateRenderTarget, 9 }, \
    { "CreateDepthStencilSurface", dv_CreateDepthStencilSurface, 9 }, { "UpdateSurface", dv_UpdateSurface, 5 }, { "UpdateTexture", dv_UpdateTexture, 3 }, \
    { "GetRenderTargetData", dv_GetRenderTargetData, 3 }, { "GetFrontBufferData", dv_GetFrontBufferData, 3 }, { "StretchRect", dv_StretchRect, 6 }, \
    { "ColorFill", dv_ColorFill, 4 }, { "CreateOffscreenPlainSurface", dv_CreateOffscreenPlainSurface, 7 }, { "SetRenderTarget", dv_SetRenderTarget, 3 }, \
    { "GetRenderTarget", dv_GetRenderTarget, 3 }, { "SetDepthStencilSurface", dv_SetDepthStencilSurface, 2 }, { "GetDepthStencilSurface", dv_GetDepthStencilSurface, 2 }, \
    { "BeginScene", dv_BeginScene, 1 }, { "EndScene", dv_EndScene, 1 }, { "Clear", dv_Clear, 7 }, { "SetTransform", dv_SetTransform, 3 }, { "GetTransform", dv_GetTransform, 3 }, \
    { "MultiplyTransform", dv_MultiplyTransform, 3 }, { "SetViewport", dv_SetViewport, 2 }, { "GetViewport", dv_GetViewport, 2 }, { "SetMaterial", dv_SetMaterial, 2 }, \
    { "GetMaterial", dv_GetMaterial, 2 }, { "SetLight", dv_SetLight, 3 }, { "GetLight", dv_GetLight, 3 }, { "LightEnable", dv_LightEnable, 3 }, \
    { "GetLightEnable", dv_GetLightEnable, 3 }, { "SetClipPlane", dv_SetClipPlane, 3 }, { "GetClipPlane", dv_GetClipPlane, 3 }, { "SetRenderState", dv_SetRenderState, 3 }, \
    { "GetRenderState", dv_GetRenderState, 3 }, { "CreateStateBlock", dv_CreateStateBlock, 3 }, { "BeginStateBlock", dv_BeginStateBlock, 1 }, \
    { "EndStateBlock", dv_EndStateBlock, 2 }, { "SetClipStatus", dv_SetClipStatus, 2 }, { "GetClipStatus", dv_GetClipStatus, 2 }, { "GetTexture", dv_GetTexture, 3 }, \
    { "SetTexture", dv_SetTexture, 3 }, { "GetTextureStageState", dv_GetTextureStageState, 4 }, { "SetTextureStageState", dv_SetTextureStageState, 4 }, \
    { "GetSamplerState", dv_GetSamplerState, 4 }, { "SetSamplerState", dv_SetSamplerState, 4 }, { "ValidateDevice", dv_ValidateDevice, 2 }, \
    { "SetPaletteEntries", dv_SetPaletteEntries, 3 }, { "GetPaletteEntries", dv_GetPaletteEntries, 3 }, { "SetCurrentTexturePalette", dv_SetCurrentTexturePalette, 2 }, \
    { "GetCurrentTexturePalette", dv_GetCurrentTexturePalette, 2 }, { "SetScissorRect", dv_SetScissorRect, 2 }, { "GetScissorRect", dv_GetScissorRect, 2 }, \
    { "SetSoftwareVertexProcessing", dv_SetSoftwareVertexProcessing, 2 }, { "GetSoftwareVertexProcessing", dv_GetSoftwareVertexProcessing, 1 }, \
    { "SetNPatchMode", dv_SetNPatchMode, 2 }, { "GetNPatchMode", dv_GetNPatchMode, 1 }, { "DrawPrimitive", dv_DrawPrimitive, 4 }, \
    { "DrawIndexedPrimitive", dv_DrawIndexedPrimitive, 7 }, { "DrawPrimitiveUP", dv_DrawPrimitiveUP, 5 }, { "DrawIndexedPrimitiveUP", dv_DrawIndexedPrimitiveUP, 9 }, \
    { "ProcessVertices", dv_ProcessVertices, 7 }, { "CreateVertexDeclaration", dv_CreateVertexDeclaration, 3 }, { "SetVertexDeclaration", dv_SetVertexDeclaration, 2 }, \
    { "GetVertexDeclaration", dv_GetVertexDeclaration, 2 }, { "SetFVF", dv_SetFVF, 2 }, { "GetFVF", dv_GetFVF, 2 }, { "CreateVertexShader", dv_CreateVertexShader, 3 }, \
    { "SetVertexShader", dv_SetVertexShader, 2 }, { "GetVertexShader", dv_GetVertexShader, 2 }, { "SetVertexShaderConstantF", dv_SetVSF, 4 }, \
    { "GetVertexShaderConstantF", dv_GetVSF, 4 }, { "SetVertexShaderConstantI", dv_SetVSI, 4 }, { "GetVertexShaderConstantI", dv_GetVSI, 4 }, \
    { "SetVertexShaderConstantB", dv_SetVSB, 4 }, { "GetVertexShaderConstantB", dv_GetVSB, 4 }, { "SetStreamSource", dv_SetStreamSource, 5 }, \
    { "GetStreamSource", dv_GetStreamSource, 5 }, { "SetStreamSourceFreq", dv_SetStreamSourceFreq, 3 }, { "GetStreamSourceFreq", dv_GetStreamSourceFreq, 3 }, \
    { "SetIndices", dv_SetIndices, 2 }, { "GetIndices", dv_GetIndices, 2 }, { "CreatePixelShader", dv_CreatePixelShader, 3 }, { "SetPixelShader", dv_SetPixelShader, 2 }, \
    { "GetPixelShader", dv_GetPixelShader, 2 }, { "SetPixelShaderConstantF", dv_SetPSF, 4 }, { "GetPixelShaderConstantF", dv_GetPSF, 4 }, \
    { "SetPixelShaderConstantI", dv_SetPSI, 4 }, { "GetPixelShaderConstantI", dv_GetPSI, 4 }, { "SetPixelShaderConstantB", dv_SetPSB, 4 }, \
    { "GetPixelShaderConstantB", dv_GetPSB, 4 }, { "DrawRectPatch", dv_Invalid, 4 }, { "DrawTriPatch", dv_Invalid, 4 }, { "DeletePatch", dv_Invalid, 2 }, \
    { "CreateQuery", dv_CreateQuery, 3 }
static const D9Method m_dev[] = { DEVM };
static const D9Method m_devex[] = { DEVM,
    { "SetConvolutionMonoKernel", dv_TestCooperativeLevel, 5 }, { "ComposeRects", dv_TestCooperativeLevel, 9 }, { "PresentEx", dv_PresentEx, 6 },
    { "GetGPUThreadPriority", dv_GetGPUThreadPriority, 2 }, { "SetGPUThreadPriority", dv_TestCooperativeLevel, 2 }, { "WaitForVBlank", dv_TestCooperativeLevel, 2 },
    { "CheckResourceResidency", dv_TestCooperativeLevel, 3 }, { "SetMaximumFrameLatency", dv_SetMaximumFrameLatency, 2 }, { "GetMaximumFrameLatency", dv_GetMaximumFrameLatency, 2 },
    { "CheckDeviceState", dv_TestCooperativeLevel, 2 }, { "CreateRenderTargetEx", dv_CreateRenderTargetEx, 10 }, { "CreateOffscreenPlainSurfaceEx", dv_CreateOffscreenPlainSurfaceEx, 8 },
    { "CreateDepthStencilSurfaceEx", dv_CreateDepthStencilSurfaceEx, 10 }, { "ResetEx", dv_ResetEx, 3 }, { "GetDisplayModeEx", dv_GetDisplayModeEx, 4 } };
static const D9Method m_swap[] = { U3, { "Present", sc_Present, 6 }, { "GetFrontBufferData", sc_GetFrontBufferData, 2 }, { "GetBackBuffer", sc_GetBackBuffer, 4 },
    { "GetRasterStatus", sc_GetRasterStatus, 2 }, { "GetDisplayMode", sc_GetDisplayMode, 2 }, { "GetDevice", sc_GetDevice, 2 },
    { "GetPresentParameters", sc_GetPresentParameters, 2 }, { "GetLastPresentCount", sc_GetLastPresentCount, 2 }, { "GetPresentStats", sc_GetPresentStats, 2 },
    { "GetDisplayModeEx", dv_GetDisplayModeEx, 3 } };
static void sb_GetDevice(X86State *st) {
    D9SB *sb = D9OBJ(D9SB, 0);
    if (!ARG(1)) { RET(D3DERR_INVALIDCALL); return; }
    d9_addref(&sb->o.dev->o);
    wr_ptr(ARG(1), H2G(sb->o.dev));
    RET(D3D_OK);
}
static const D9Method m_sb[] = { U3, { "GetDevice", sb_GetDevice, 2 }, { "Capture", sb_Capture, 1 }, { "Apply", sb_Apply, 1 } };
#define N(a) ((int)(sizeof(a) / sizeof((a)[0])))
void d9_register_dev_vtables(void) {
    d9_vt_register(VT_DEV, "IDirect3DDevice9", m_dev, N(m_dev));
    d9_vt_register(VT_DEVEX, "IDirect3DDevice9Ex", m_devex, N(m_devex));
    d9_vt_register(VT_SWAP, "IDirect3DSwapChain9", m_swap, N(m_swap));
    d9_vt_register(VT_SB, "IDirect3DStateBlock9", m_sb, N(m_sb));
}
