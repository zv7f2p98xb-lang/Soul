// SoulCore - sgpu Metal backend (SOUL_GPU=metal, or soul_gpu_enable(1)).
//
// Runs the sgpu draws on the GPU. Same structure as the "mirror" debug backend (sgpu_mirror.c): the CPU
// memory stays the source of truth whenever the CPU looks at it; the backend keeps shadow resources.
//
//  * colour target  (keyed by its memory pointer)  -> private MTLPixelFormatBGRA8Unorm texture
//  * depth/stencil  (keyed by its depth and/or stencil pointers; one texture per pair)
//                   -> Depth32Float_Stencil8 / Depth32Float / Stencil8 private texture
//  * CPU -> GPU transfers (first use, cpu_wrote) go through a shared staging buffer and a blit, so they are
//    ordered with the queued draws; GPU -> CPU (sync_to_cpu) is a blit into a shared buffer, a commit + wait and a
//    memcpy. Only resources that were drawn to ("dirty") are read back.
//  * draws are batched: one render encoder stays open per (colour, depth/stencil) pair; a command buffer is
//    committed on sync, end_frame, when it grows too large, or when the visibility buffer is full.
//  * shaders come from sgsl (MSL text), libraries/pipelines/depth-stencil states/samplers/textures are cached.
//
// Declined to the CPU (draw returns -1): see the DR_* reasons below (no shader translation, shader compile or
// pipeline failure, lines wider than one pixel, border colours Metal cannot express, odd textures, absurd
// target sizes, allocation failures, no depth buffer for a shader that writes depth). Clears: only full-target
// clears (no scissor, all colour channels writable, stencil write mask 0xFF unless SG_CLEAR_NOMASK); partial
// clears return -1 and are done by the CPU after a sync.
//
// Occlusion counting: sg_pixels_drawn() (sgpu.c) adds sg_metal_pixels_drawn(). The first call turns on Metal
// visibility counting for the following draws (draws before it are not counted); every call after that waits for
// the GPU and returns the accumulated number of fragments that passed the depth/stencil tests.
//
// Environment: SOUL_GPU_DEBUG=1 prints the counters once per second (and shader compile errors with the MSL).
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <float.h>
#include <stdarg.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "sgpu.h"
#include "sgpu_program.h"
#include "sgsl.h"

#if !__has_feature(objc_arc)
#error "sgpu_metal.m must be compiled with -fobjc-arc (Xcode: CLANG_ENABLE_OBJC_ARC = YES)"
#endif

#define PK(p) [NSNumber numberWithUnsignedLongLong:(unsigned long long)(uintptr_t)(p)]

enum {
    kChunk = 4 << 20,          // staging / vertex arena chunk
    kVisSlots = 4096,          // visibility counters per command buffer
    kMaxPending = 3,           // committed, unfinished command buffers before the CPU waits
    kMemo = 16,
    kMaxProgs = 1024,
    kMaxDim = 16384,
};
#define SGM_BATCH_LIMIT ((size_t)64 << 20)
#define SGM_MAX_PIXELS ((size_t)8192 * 8192)

enum { DR_TRANSLATE, DR_COMPILE, DR_PIPELINE, DR_LINE_WIDTH, DR_BORDER, DR_TEXTURE, DR_TARGET, DR_ALLOC, DR_NODEPTH, DR_PARTIAL_CLEAR, DR_COUNT };
static const char *const kReason[DR_COUNT] = { "translate", "compile", "pipeline", "line-width", "border", "texture", "target", "alloc", "no-depth", "partial-clear" };

static struct {
    unsigned long long draws, clears, declined[DR_COUNT], libs, pipes, uploads, readbacks, commits;
} g_st, g_stPrev;

// ---- resource classes -------------------------------------------------------------------------------------------

@interface SgmColor : NSObject { @public id<MTLTexture> tex; const void *key; int w, h; BOOL dirty; } @end
@implementation SgmColor @end

@interface SgmDS : NSObject { @public id<MTLTexture> tex; const void *dkey, *skey; int w, h; BOOL hasD, hasS, dirty; } @end
@implementation SgmDS @end

@interface SgmTex : NSObject { @public id<MTLTexture> tex, srgb; uint64_t tid; uint32_t version; int w, h, depth, kind, levels; } @end
@implementation SgmTex @end

@interface SgmProg : NSObject { @public id<MTLFunction> vs, fs; SgslInfo info; BOOL failed; NSMutableDictionary *pipes; } @end
@implementation SgmProg @end

@interface SgmChunk : NSObject { @public id<MTLBuffer> buf; size_t size; } @end
@implementation SgmChunk @end

@interface SgmBatch : NSObject { @public id<MTLCommandBuffer> cb; NSMutableArray *chunks; id<MTLBuffer> vis; int visCount; size_t bytes; } @end
@implementation SgmBatch @end

@interface SgmMemo : NSObject { @public BOOL used; uint64_t pk; int prim; SgState m; SgmProg *prog; } @end
@implementation SgmMemo @end

// ---- globals (every entry point runs under the sgpu global lock) ---------------------------------------------------

static BOOL g_inited, g_debug, g_wantCounts;
static id<MTLDevice> g_dev;
static id<MTLCommandQueue> g_q;
static char g_devname[128] = "?";
static BOOL g_unified;

static id<MTLCommandBuffer> g_cb;
static id<MTLRenderCommandEncoder> g_enc;
static SgmColor *g_encC;
static SgmDS *g_encD;
static BOOL g_encVis;
static SgmBatch *g_batch;
static SgmChunk *g_cur;
static size_t g_curOff;
static NSMutableArray *g_pending, *g_free;
static id<MTLBuffer> g_rb[3];
static unsigned long long g_visTotal;

static NSMutableDictionary *g_colors, *g_dsmap, *g_texs, *g_progs, *g_samplers, *g_dsstates;
static NSMutableArray *g_memo;
static int g_memoNext;
static int g_failLogged;
static double g_lastPrint;
static unsigned g_tick;

static void dbg(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void dbg(const char *fmt, ...) {
    if (!g_debug) return;
    va_list ap;
    va_start(ap, fmt);
    fputs("[sgpu-metal] ", stderr);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
}

static int decline(int reason) { g_st.declined[reason]++; return -1; }

// Metal errors of library / function / pipeline creation. The first few distinct messages are printed once each when
// SOUL_GPU_DEBUG=1; the first and the last one are also offered to soulrun --gpu-stats (sg_metal_last_error).
enum { kMaxDistinctErrors = 8 };
static char g_errFirst[200], g_errLast[200];
static NSMutableSet *g_errSeen;

static void err_clean(char *dst, size_t dl, const char *src) {
    size_t n = 0;
    for (; *src && n + 1 < dl; src++) dst[n++] = (*src == '\n' || *src == '\r' || *src == '\t') ? ' ' : *src == '"' ? '\'' : *src;
    dst[n] = 0;
}

static void record_error(const char *stage, NSError *e, const char *context) {
    const char *msg = e ? [[e description] UTF8String] : NULL;
    if (!msg) msg = "(no NSError)";
    char line[200];
    err_clean(line, sizeof line, msg);
    char full[240];
    snprintf(full, sizeof full, "%s: %s", stage, line);
    err_clean(g_errLast, sizeof g_errLast, full);
    if (!g_errFirst[0]) memcpy(g_errFirst, g_errLast, sizeof g_errFirst);
    if (!g_debug) return;
    if (!g_errSeen) g_errSeen = [NSMutableSet set];
    NSString *k = [NSString stringWithFormat:@"%s|%s", stage, msg];
    if ([g_errSeen containsObject:k] || g_errSeen.count >= (NSUInteger)kMaxDistinctErrors) return;
    [g_errSeen addObject:k];
    fprintf(stderr, "[sgpu-metal] %s failed (error %lu of at most %d distinct): %s\n[sgpu-metal]   context: %s\n",
            stage, (unsigned long)g_errSeen.count, (int)kMaxDistinctErrors, msg, context ? context : "-");
}

// ---- command buffers, encoders, arena -------------------------------------------------------------------------------

static void enc_end(void) {
    if (g_enc) { [g_enc endEncoding]; g_enc = nil; }
}

static BOOL cb_get(void) {
    if (g_cb) return YES;
    g_cb = [g_q commandBuffer];
    if (!g_cb) return NO;
    g_batch = [SgmBatch new];
    g_batch->chunks = [NSMutableArray array];
    g_cur = nil; g_curOff = 0;
    return YES;
}

// size bytes of shared memory for this batch (256-byte aligned). Returns the chunk and the offset in it.
static SgmChunk *arena_alloc(size_t bytes, size_t *off) {
    if (!cb_get()) return nil;
    size_t need = (bytes + 255) & ~(size_t)255;
    if (need == 0) need = 256;
    if (g_cur && g_curOff + need <= g_cur->size) { *off = g_curOff; g_curOff += need; return g_cur; }
    SgmChunk *c = nil;
    for (NSUInteger i = 0; i < g_free.count; i++) {
        SgmChunk *f = g_free[i];
        if (f->size >= need) { c = f; [g_free removeObjectAtIndex:i]; break; }
    }
    if (!c) {
        size_t want = need > (size_t)kChunk ? need : (size_t)kChunk;
        if (want > (size_t)[g_dev maxBufferLength]) return nil;
        id<MTLBuffer> b = [g_dev newBufferWithLength:want options:MTLResourceStorageModeShared];
        if (!b) return nil;
        c = [SgmChunk new];
        c->buf = b; c->size = want;
    }
    [g_batch->chunks addObject:c];
    g_batch->bytes += c->size;
    *off = 0;
    if (need <= (size_t)kChunk) { g_cur = c; g_curOff = need; }
    return c;
}

static void reap(void) {
    for (NSUInteger i = 0; i < g_pending.count;) {
        SgmBatch *b = g_pending[i];
        MTLCommandBufferStatus st = b->cb.status;
        if (st == MTLCommandBufferStatusCompleted || st == MTLCommandBufferStatusError) {
            if (st == MTLCommandBufferStatusError) dbg("command buffer failed: %s", b->cb.error ? [[b->cb.error description] UTF8String] : "?");
            else if (b->vis) {
                const uint64_t *p = (const uint64_t *)b->vis.contents;
                for (int k = 0; k < b->visCount; k++) g_visTotal += p[k];
            }
            for (SgmChunk *c in b->chunks) if (c->size <= (size_t)kChunk && g_free.count < 8) [g_free addObject:c];
            [g_pending removeObjectAtIndex:i];
        } else i++;
    }
}

static void wait_all(void) {
    for (SgmBatch *b in [g_pending copy]) [b->cb waitUntilCompleted];
    reap();
}

// Ends the open encoder and commits the open command buffer; wait = also wait for everything committed.
static void batch_commit(BOOL wait) {
    enc_end();
    if (g_cb) {
        [g_cb commit];
        g_st.commits++;
        g_batch->cb = g_cb;
        [g_pending addObject:g_batch];
        g_cb = nil; g_batch = nil; g_cur = nil; g_curOff = 0;
    }
    if (wait) wait_all();
    else {
        reap();
        if (g_pending.count > (NSUInteger)kMaxPending) { [((SgmBatch *)g_pending[0])->cb waitUntilCompleted]; reap(); }
    }
}

static id<MTLBuffer> rb_get(int idx, size_t bytes) {
    if (g_rb[idx] && g_rb[idx].length >= bytes) return g_rb[idx];
    if (bytes > (size_t)[g_dev maxBufferLength]) return nil;
    g_rb[idx] = [g_dev newBufferWithLength:bytes options:MTLResourceStorageModeShared];
    return g_rb[idx];
}

static BOOL enc_for(SgmColor *c, SgmDS *d) {
    if (g_enc && g_encC == c && g_encD == d) return YES;
    enc_end();
    if (!cb_get()) return NO;
    MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
    rp.colorAttachments[0].texture = c->tex;
    rp.colorAttachments[0].loadAction = MTLLoadActionLoad;
    rp.colorAttachments[0].storeAction = MTLStoreActionStore;
    if (d && d->hasD) {
        rp.depthAttachment.texture = d->tex;
        rp.depthAttachment.loadAction = MTLLoadActionLoad;
        rp.depthAttachment.storeAction = MTLStoreActionStore;
    }
    if (d && d->hasS) {
        rp.stencilAttachment.texture = d->tex;
        rp.stencilAttachment.loadAction = MTLLoadActionLoad;
        rp.stencilAttachment.storeAction = MTLStoreActionStore;
    }
    g_encVis = NO;
    if (g_wantCounts) {
        if (!g_batch->vis) {
            g_batch->vis = [g_dev newBufferWithLength:8 * kVisSlots options:MTLResourceStorageModeShared];
            if (g_batch->vis) memset(g_batch->vis.contents, 0, 8 * kVisSlots);
        }
        if (g_batch->vis) { rp.visibilityResultBuffer = g_batch->vis; g_encVis = YES; }
    }
    g_enc = [g_cb renderCommandEncoderWithDescriptor:rp];
    if (!g_enc) return NO;
    g_encC = c; g_encD = d;
    return YES;
}

// ---- targets ----------------------------------------------------------------------------------------------------------

static BOOL dims_ok(const SgTarget *t) {
    return t->w > 0 && t->h > 0 && t->w <= kMaxDim && t->h <= kMaxDim && (size_t)t->w * (size_t)t->h <= SGM_MAX_PIXELS;
}

// Blit (CPU memory -> texture region) through a staging buffer, ordered with the queued draws.
static BOOL upload_color(SgmColor *c, int x, int y, int w, int h) {
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > c->w) w = c->w - x;
    if (y + h > c->h) h = c->h - y;
    if (w <= 0 || h <= 0) return YES;
    size_t off;
    SgmChunk *ch = arena_alloc((size_t)w * (size_t)h * 4, &off);
    if (!ch) return NO;
    uint8_t *dst = (uint8_t *)ch->buf.contents + off;
    const uint32_t *src = (const uint32_t *)c->key;
    for (int r = 0; r < h; r++) memcpy(dst + (size_t)r * (size_t)w * 4, src + (size_t)(y + r) * (size_t)c->w + (size_t)x, (size_t)w * 4);
    enc_end();
    id<MTLBlitCommandEncoder> bl = [g_cb blitCommandEncoder];
    if (!bl) return NO;
    [bl copyFromBuffer:ch->buf sourceOffset:off sourceBytesPerRow:(NSUInteger)w * 4 sourceBytesPerImage:(NSUInteger)w * (NSUInteger)h * 4
            sourceSize:MTLSizeMake((NSUInteger)w, (NSUInteger)h, 1) toTexture:c->tex destinationSlice:0 destinationLevel:0
     destinationOrigin:MTLOriginMake((NSUInteger)x, (NSUInteger)y, 0)];
    [bl endEncoding];
    g_st.uploads++;
    return YES;
}

static BOOL upload_ds(SgmDS *r, BOOL depthPlane) {
    size_t esz = depthPlane ? 4 : 1, n = (size_t)r->w * (size_t)r->h;
    size_t off;
    SgmChunk *ch = arena_alloc(n * esz, &off);
    if (!ch) return NO;
    memcpy((uint8_t *)ch->buf.contents + off, depthPlane ? r->dkey : r->skey, n * esz);
    enc_end();
    id<MTLBlitCommandEncoder> bl = [g_cb blitCommandEncoder];
    if (!bl) return NO;
    MTLBlitOption opt = MTLBlitOptionNone;
    if (r->hasD && r->hasS) opt = depthPlane ? MTLBlitOptionDepthFromDepthStencil : MTLBlitOptionStencilFromDepthStencil;
    [bl copyFromBuffer:ch->buf sourceOffset:off sourceBytesPerRow:(NSUInteger)r->w * esz sourceBytesPerImage:n * esz
            sourceSize:MTLSizeMake((NSUInteger)r->w, (NSUInteger)r->h, 1) toTexture:r->tex destinationSlice:0 destinationLevel:0
     destinationOrigin:MTLOriginMake(0, 0, 0) options:opt];
    [bl endEncoding];
    g_st.uploads++;
    return YES;
}

// GPU -> CPU. Blits into the shared read-back buffers, commits, waits, copies into the CPU memory.
static BOOL color_readback(SgmColor *c) {
    size_t n = (size_t)c->w * (size_t)c->h;
    id<MTLBuffer> rb = rb_get(0, n * 4);
    if (!rb || !cb_get()) return NO;
    enc_end();
    id<MTLBlitCommandEncoder> bl = [g_cb blitCommandEncoder];
    if (!bl) return NO;
    [bl copyFromTexture:c->tex sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0)
             sourceSize:MTLSizeMake((NSUInteger)c->w, (NSUInteger)c->h, 1) toBuffer:rb destinationOffset:0
      destinationBytesPerRow:(NSUInteger)c->w * 4 destinationBytesPerImage:n * 4];
    [bl endEncoding];
    batch_commit(YES);
    memcpy((void *)(uintptr_t)c->key, rb.contents, n * 4);
    c->dirty = NO;
    g_st.readbacks++;
    return YES;
}

static BOOL ds_readback(SgmDS *r, BOOL wantD, BOOL wantS) {
    wantD = wantD && r->hasD && r->dkey;
    wantS = wantS && r->hasS && r->skey;
    if (!wantD && !wantS) { r->dirty = NO; return YES; }
    size_t n = (size_t)r->w * (size_t)r->h;
    id<MTLBuffer> rbd = wantD ? rb_get(1, n * 4) : nil, rbs = wantS ? rb_get(2, n) : nil;
    if ((wantD && !rbd) || (wantS && !rbs) || !cb_get()) return NO;
    enc_end();
    id<MTLBlitCommandEncoder> bl = [g_cb blitCommandEncoder];
    if (!bl) return NO;
    BOOL combined = r->hasD && r->hasS;
    MTLSize sz = MTLSizeMake((NSUInteger)r->w, (NSUInteger)r->h, 1);
    if (wantD)
        [bl copyFromTexture:r->tex sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0) sourceSize:sz toBuffer:rbd destinationOffset:0
          destinationBytesPerRow:(NSUInteger)r->w * 4 destinationBytesPerImage:n * 4 options:combined ? MTLBlitOptionDepthFromDepthStencil : MTLBlitOptionNone];
    if (wantS)
        [bl copyFromTexture:r->tex sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0) sourceSize:sz toBuffer:rbs destinationOffset:0
          destinationBytesPerRow:(NSUInteger)r->w destinationBytesPerImage:n options:combined ? MTLBlitOptionStencilFromDepthStencil : MTLBlitOptionNone];
    [bl endEncoding];
    batch_commit(YES);
    if (wantD) memcpy((void *)(uintptr_t)r->dkey, rbd.contents, n * 4);
    if (wantS) memcpy((void *)(uintptr_t)r->skey, rbs.contents, n);
    r->dirty = NO;
    g_st.readbacks++;
    return YES;
}

static SgmColor *color_get(const SgTarget *t, BOOL upload) {
    NSNumber *k = PK(t->color);
    SgmColor *c = g_colors[k];
    if (c && (c->w != t->w || c->h != t->h)) {   // size change: bring the CPU memory up to date, then rebuild from it
        if (c->dirty) color_readback(c);
        [g_colors removeObjectForKey:k];
        c = nil;
    }
    if (c) return c;
    MTLTextureDescriptor *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                                                  width:(NSUInteger)t->w height:(NSUInteger)t->h mipmapped:NO];
    d.usage = MTLTextureUsageRenderTarget;
    d.storageMode = MTLStorageModePrivate;
    id<MTLTexture> tex = [g_dev newTextureWithDescriptor:d];
    if (!tex) return nil;
    c = [SgmColor new];
    c->tex = tex; c->key = t->color; c->w = t->w; c->h = t->h; c->dirty = NO;
    if (upload && !upload_color(c, 0, 0, t->w, t->h)) return nil;
    g_colors[k] = c;
    return c;
}

static void ds_unmap(SgmDS *r) {
    if (r->dkey && g_dsmap[PK(r->dkey)] == r) [g_dsmap removeObjectForKey:PK(r->dkey)];
    if (r->skey && g_dsmap[PK(r->skey)] == r) [g_dsmap removeObjectForKey:PK(r->skey)];
}

// Depth/stencil resource for the (depth, stencil) pointers of t. upD/upS: initialise that plane from CPU memory
// when the resource is created (not needed when the caller clears it right away).
static SgmDS *ds_get(const SgTarget *t, BOOL upD, BOOL upS) {
    const void *dk = t->depth, *sk = t->stencil;
    if (!dk && !sk) return nil;
    SgmDS *ed = dk ? g_dsmap[PK(dk)] : nil, *es = sk ? g_dsmap[PK(sk)] : nil;
    SgmDS *r = ed ? ed : es;
    if (r && r->dkey == dk && r->skey == sk && r->w == t->w && r->h == t->h) return r;
    SgmDS *list[2] = { ed, (es && es != ed) ? es : nil };   // a different pairing or size: write back and rebuild
    for (int i = 0; i < 2; i++) {
        if (!list[i]) continue;
        if (list[i]->dirty) ds_readback(list[i], YES, YES);
        ds_unmap(list[i]);
    }
    MTLPixelFormat fmt = (dk && sk) ? MTLPixelFormatDepth32Float_Stencil8 : dk ? MTLPixelFormatDepth32Float : MTLPixelFormatStencil8;
    MTLTextureDescriptor *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:fmt width:(NSUInteger)t->w height:(NSUInteger)t->h mipmapped:NO];
    d.usage = MTLTextureUsageRenderTarget;
    d.storageMode = MTLStorageModePrivate;
    id<MTLTexture> tex = [g_dev newTextureWithDescriptor:d];
    if (!tex) return nil;
    r = [SgmDS new];
    r->tex = tex; r->dkey = dk; r->skey = sk; r->w = t->w; r->h = t->h; r->hasD = dk != NULL; r->hasS = sk != NULL; r->dirty = NO;
    if ((dk && upD && !upload_ds(r, YES)) || (sk && upS && !upload_ds(r, NO))) return nil;
    if (dk) g_dsmap[PK(dk)] = r;
    if (sk) g_dsmap[PK(sk)] = r;
    return r;
}

// ---- textures and samplers --------------------------------------------------------------------------------------------

static MTLStorageMode tex_storage(void) {
#if TARGET_OS_OSX
    if (!g_unified) return MTLStorageModeManaged;
#endif
    return MTLStorageModeShared;
}

static int dim_at(int base, int l) { int v = base >> l; return v > 0 ? v : 1; }

static SgmTex *tex_build(SgTexture *tx, uint64_t tid) {
    int kind = tx->kind;
    if (kind != SG_TEX_2D && kind != SG_TEX_CUBE && kind != SG_TEX_VOLUME) return nil;
    if (!tx->rgba || tx->w <= 0 || tx->h <= 0 || tx->w > kMaxDim || tx->h > kMaxDim) return nil;
    int depth = 1;
    if (kind == SG_TEX_VOLUME) { depth = tx->depth > 0 ? tx->depth : 1; if (depth > 2048) return nil; }
    if (kind == SG_TEX_CUBE && tx->w != tx->h) return nil;
    int L = tx->levels > 1 ? tx->levels : 1;
    if (L > SG_MAX_MIPS) L = SG_MAX_MIPS;
    for (int l = 1; l < L; l++) {   // the chain must be the standard one with all data present, else cut it
        int ed = kind == SG_TEX_VOLUME ? dim_at(depth, l) : 1;
        if (!tx->mip[l] || tx->mip_w[l] != dim_at(tx->w, l) || tx->mip_h[l] != dim_at(tx->h, l) || (kind == SG_TEX_VOLUME && tx->mip_d[l] != ed)) { L = l; break; }
    }
    MTLTextureDescriptor *d;
    if (kind == SG_TEX_CUBE) d = [MTLTextureDescriptor textureCubeDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm size:(NSUInteger)tx->w mipmapped:NO];
    else if (kind == SG_TEX_VOLUME) {
        d = [[MTLTextureDescriptor alloc] init];
        d.textureType = MTLTextureType3D;
        d.pixelFormat = MTLPixelFormatRGBA8Unorm;
        d.width = (NSUInteger)tx->w; d.height = (NSUInteger)tx->h; d.depth = (NSUInteger)depth;
    } else d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:(NSUInteger)tx->w height:(NSUInteger)tx->h mipmapped:NO];
    d.mipmapLevelCount = (NSUInteger)L;
    d.usage = MTLTextureUsageShaderRead | MTLTextureUsagePixelFormatView;
    d.storageMode = tex_storage();
    id<MTLTexture> tex = [g_dev newTextureWithDescriptor:d];
    if (!tex) return nil;
    for (int l = 0; l < L; l++) {
        const uint8_t *src = l == 0 ? tx->rgba : tx->mip[l];
        int lw = dim_at(tx->w, l), lh = dim_at(tx->h, l);
        if (kind == SG_TEX_VOLUME) {
            int ld = dim_at(depth, l);
            [tex replaceRegion:MTLRegionMake3D(0, 0, 0, (NSUInteger)lw, (NSUInteger)lh, (NSUInteger)ld) mipmapLevel:(NSUInteger)l slice:0
                     withBytes:src bytesPerRow:(NSUInteger)lw * 4 bytesPerImage:(NSUInteger)lw * (NSUInteger)lh * 4];
        } else {
            int faces = kind == SG_TEX_CUBE ? 6 : 1;
            for (int f = 0; f < faces; f++)
                [tex replaceRegion:MTLRegionMake2D(0, 0, (NSUInteger)lw, (NSUInteger)lh) mipmapLevel:(NSUInteger)l slice:(NSUInteger)f
                         withBytes:src + (size_t)f * (size_t)lw * (size_t)lh * 4 bytesPerRow:(NSUInteger)lw * 4 bytesPerImage:0];
        }
    }
    SgmTex *m = [SgmTex new];
    m->tex = tex; m->tid = tid; m->version = tx->version; m->w = tx->w; m->h = tx->h; m->depth = tx->depth; m->kind = kind;
    m->levels = tx->levels > 1 ? tx->levels : 1;
    g_st.uploads++;
    return m;
}

static SgmTex *tex_get(const SgTexture *ctx) {
    SgTexture *tx = (SgTexture *)(uintptr_t)ctx;   // sg_texture_id assigns the id lazily
    uint64_t id = sg_texture_id(tx);
    NSNumber *k = [NSNumber numberWithUnsignedLongLong:id];
    SgmTex *m = g_texs[k];
    if (m && m->version == tx->version && m->w == tx->w && m->h == tx->h && m->kind == tx->kind && m->depth == tx->depth &&
        m->levels == (tx->levels > 1 ? tx->levels : 1)) return m;
    m = tex_build(tx, id);   // a changed texture is a NEW MTLTexture: queued draws keep the old one
    if (!m) { [g_texs removeObjectForKey:k]; return nil; }
    g_texs[k] = m;
    return m;
}

static MTLSamplerAddressMode addr_mode(int a) {
    switch (a) {
    case SG_ADDR_MIRROR: return MTLSamplerAddressModeMirrorRepeat;
    case SG_ADDR_CLAMP: return MTLSamplerAddressModeClampToEdge;
    case SG_ADDR_BORDER: return MTLSamplerAddressModeClampToBorderColor;
    case SG_ADDR_MIRROR_ONCE: return MTLSamplerAddressModeMirrorClampToEdge;
    default: return MTLSamplerAddressModeRepeat;
    }
}

static id<MTLSamplerState> sampler_get(const SgSampler *sp, int kind) {
    BOOL border = sp->address_u == SG_ADDR_BORDER || sp->address_v == SG_ADDR_BORDER || (kind == SG_TEX_VOLUME && sp->address_w == SG_ADDR_BORDER);
    MTLSamplerBorderColor bc = MTLSamplerBorderColorTransparentBlack;
    if (border) {
        if (sp->border_argb == 0x00000000u) bc = MTLSamplerBorderColorTransparentBlack;
        else if (sp->border_argb == 0xFF000000u) bc = MTLSamplerBorderColorOpaqueBlack;
        else if (sp->border_argb == 0xFFFFFFFFu) bc = MTLSamplerBorderColorOpaqueWhite;
        else return nil;
    }
    int aniso = (sp->max_anisotropy > 1 && sp->min_filter == SG_FILTER_LINEAR && kind == SG_TEX_2D) ? (sp->max_anisotropy > 16 ? 16 : sp->max_anisotropy) : 1;
    int maxmip = sp->max_mip_level > 0 ? sp->max_mip_level : 0;
    char kb[160];
    snprintf(kb, sizeof kb, "%d.%d.%d|%d.%d.%d|%d|%d|%d", sp->min_filter, sp->mag_filter, sp->mip_filter, sp->address_u, sp->address_v,
             kind == SG_TEX_VOLUME ? sp->address_w : 0, border ? (int)bc + 1 : 0, aniso, maxmip);
    NSString *key = [NSString stringWithUTF8String:kb];
    id<MTLSamplerState> ss = g_samplers[key];
    if (ss) return ss;
    MTLSamplerDescriptor *d = [[MTLSamplerDescriptor alloc] init];
    d.minFilter = sp->min_filter == SG_FILTER_LINEAR ? MTLSamplerMinMagFilterLinear : MTLSamplerMinMagFilterNearest;
    d.magFilter = sp->mag_filter == SG_FILTER_LINEAR ? MTLSamplerMinMagFilterLinear : MTLSamplerMinMagFilterNearest;
    d.mipFilter = sp->mip_filter == SG_FILTER_LINEAR ? MTLSamplerMipFilterLinear : sp->mip_filter == SG_FILTER_POINT ? MTLSamplerMipFilterNearest : MTLSamplerMipFilterNotMipmapped;
    d.sAddressMode = addr_mode(sp->address_u);
    d.tAddressMode = addr_mode(sp->address_v);
    d.rAddressMode = addr_mode(kind == SG_TEX_VOLUME ? sp->address_w : SG_ADDR_CLAMP);
    if (border) d.borderColor = bc;
    d.maxAnisotropy = (NSUInteger)aniso;
    d.lodMinClamp = (float)maxmip;
    d.lodMaxClamp = FLT_MAX;
    ss = [g_dev newSamplerStateWithDescriptor:d];
    if (ss) g_samplers[key] = ss;
    return ss;
}

// ---- shader programs, pipelines, depth/stencil states -------------------------------------------------------------------

static void memo_mask(SgState *m, const SgState *s) {
    // Everything that does NOT change the generated source (uniforms and fixed-function state) is zeroed, so
    // consecutive draws of one material hit the memo without calling sgsl.
    memcpy(m, s, sizeof *m);
    m->vp_x = m->vp_y = m->vp_w = m->vp_h = 0;
    m->depth_near = m->depth_far = 0;
    m->scissor = m->sc_x = m->sc_y = m->sc_w = m->sc_h = 0;
    m->depth_test = m->depth_write = m->depth_func = 0;
    m->blend = m->blend_src = m->blend_dst = 0;
    m->blend_sep_alpha = m->blend_src_a = m->blend_dst_a = m->blend_op = m->blend_op_a = 0;
    memset(m->blend_color, 0, sizeof m->blend_color);
    m->alpha_ref = 0;
    m->cull = m->cull_back = m->cull_front = m->front_ccw = 0;
    for (int u = 0; u < SG_UNITS; u++) { m->tex[u] = s->tex[u] ? (SgTexture *)(uintptr_t)1 : NULL; memset(m->env_color[u], 0, sizeof m->env_color[u]); }
    memset(m->fog_color, 0, sizeof m->fog_color);
    m->mask_r = m->mask_g = m->mask_b = m->mask_a = 0;
    m->line_width = m->point_size = m->polygon_offset = 0;
    m->point_size_min = m->point_size_max = 0;
    m->depth_bias = m->slope_scale_bias = 0;
    m->pixel_ctx = NULL;
    m->program = s->program ? (const struct SgProgram *)(uintptr_t)1 : NULL;
    memset(m->stencil, 0, sizeof m->stencil);
    m->stencil_test = m->stencil_two_sided = 0;
    memset(m->clip_plane, 0, sizeof m->clip_plane);
}

static SgmProg *prog_failed(void) {
    SgmProg *p = [SgmProg new];
    p->failed = YES;
    return p;
}

static SgmProg *prog_compile(const SgState *s, int prim) {
    char err[256] = { 0 };
    SgslInfo info;
    memset(&info, 0, sizeof info);
    char *src = sgsl_generate_prim(s, prim, SGSL_MSL, &info, err, sizeof err);
    if (!src) {
        g_st.declined[DR_TRANSLATE]++;
        if (g_failLogged < 20) { g_failLogged++; dbg("not translated (%s): drawn on the CPU", err); }
        return prog_failed();
    }
    NSNumber *k = [NSNumber numberWithUnsignedLongLong:info.key ^ ((unsigned long long)(prim + 1) * 0x9E3779B97F4A7C15ULL)];
    SgmProg *p = g_progs[k];
    if (p) { free(src); return p; }
    if (g_progs.count >= (NSUInteger)kMaxProgs) { [g_progs removeAllObjects]; for (SgmMemo *m in g_memo) m->used = NO; }
    p = [SgmProg new];
    p->info = info;
    p->pipes = [NSMutableDictionary dictionary];
    NSString *msl = [NSString stringWithUTF8String:src];
    if (!msl) { free(src); p->failed = YES; g_progs[k] = p; return p; }
    MTLCompileOptions *opts = [[MTLCompileOptions alloc] init];
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
    opts.fastMathEnabled = NO;   // the D3D arithmetic relies on 0 * inf = 0 and exact NaN propagation
#pragma clang diagnostic pop
    NSError *e = nil;
    id<MTLLibrary> lib = [g_dev newLibraryWithSource:msl options:opts error:&e];
    g_st.libs++;
    if (lib) {
        p->vs = [lib newFunctionWithName:@"sg_vs"];
        p->fs = [lib newFunctionWithName:@"sg_fs"];
    }
    if (!p->vs || !p->fs) {
        p->failed = YES;
        {
            char ctx[160];
            snprintf(ctx, sizeof ctx, "program key %016llx prim %d, library %s, sg_vs %s, sg_fs %s", (unsigned long long)info.key, prim,
                     lib ? "ok" : "FAILED", p->vs ? "ok" : "missing", p->fs ? "ok" : "missing");
            record_error(lib ? "newFunctionWithName" : "newLibraryWithSource", e, ctx);
        }
        if (g_debug && g_failLogged < 20) {
            g_failLogged++;
            fprintf(stderr, "[sgpu-metal] shader compile failed: %s\n--- MSL ---\n%s\n-----------\n", e ? [[e description] UTF8String] : "no functions", src);
        }
    }
    free(src);
    g_progs[k] = p;
    return p;
}

static SgmProg *prog_get(const SgState *s, int prim) {
    uint64_t pk = sg_program_key(s);
    SgState m;
    memo_mask(&m, s);
    for (SgmMemo *e in g_memo)
        if (e->used && e->pk == pk && e->prim == prim && memcmp(&e->m, &m, sizeof m) == 0) {
            if (e->prog->failed) g_st.declined[e->prog->pipes ? DR_COMPILE : DR_TRANSLATE]++;
            return e->prog;
        }
    SgmProg *p = prog_compile(s, prim);
    SgmMemo *e = g_memo[(NSUInteger)(g_memoNext++ % kMemo)];
    e->used = YES; e->pk = pk; e->prim = prim; e->m = m; e->prog = p;
    if (p->failed && p->pipes) g_st.declined[DR_COMPILE]++;
    return p;
}

static MTLBlendFactor blend_factor(int f) {
    switch (f) {
    case SG_ZERO: return MTLBlendFactorZero;
    case SG_ONE: return MTLBlendFactorOne;
    case SG_SRC_COLOR: return MTLBlendFactorSourceColor;
    case SG_ONE_MINUS_SRC_COLOR: return MTLBlendFactorOneMinusSourceColor;
    case SG_SRC_ALPHA: return MTLBlendFactorSourceAlpha;
    case SG_ONE_MINUS_SRC_ALPHA: return MTLBlendFactorOneMinusSourceAlpha;
    case SG_DST_ALPHA: return MTLBlendFactorDestinationAlpha;
    case SG_ONE_MINUS_DST_ALPHA: return MTLBlendFactorOneMinusDestinationAlpha;
    case SG_DST_COLOR: return MTLBlendFactorDestinationColor;
    case SG_ONE_MINUS_DST_COLOR: return MTLBlendFactorOneMinusDestinationColor;
    case SG_SRC_ALPHA_SATURATE: return MTLBlendFactorSourceAlphaSaturated;
    case SG_CONSTANT_COLOR: return MTLBlendFactorBlendColor;
    case SG_ONE_MINUS_CONSTANT_COLOR: return MTLBlendFactorOneMinusBlendColor;
    case SG_CONSTANT_ALPHA: return MTLBlendFactorBlendAlpha;
    case SG_ONE_MINUS_CONSTANT_ALPHA: return MTLBlendFactorOneMinusBlendAlpha;
    default: return MTLBlendFactorOne;
    }
}
static MTLBlendOperation blend_op(int o) {
    switch (o) {
    case SG_BLEND_SUBTRACT: return MTLBlendOperationSubtract;
    case SG_BLEND_REV_SUBTRACT: return MTLBlendOperationReverseSubtract;
    case SG_BLEND_MIN: return MTLBlendOperationMin;
    case SG_BLEND_MAX: return MTLBlendOperationMax;
    default: return MTLBlendOperationAdd;
    }
}
static MTLCompareFunction cmp_func(int f) {
    switch (f) {
    case SG_NEVER: return MTLCompareFunctionNever;
    case SG_LESS: return MTLCompareFunctionLess;
    case SG_EQUAL: return MTLCompareFunctionEqual;
    case SG_LEQUAL: return MTLCompareFunctionLessEqual;
    case SG_GREATER: return MTLCompareFunctionGreater;
    case SG_NOTEQUAL: return MTLCompareFunctionNotEqual;
    case SG_GEQUAL: return MTLCompareFunctionGreaterEqual;
    default: return MTLCompareFunctionAlways;
    }
}
static MTLStencilOperation stencil_op(int o) {
    switch (o) {
    case SG_SOP_ZERO: return MTLStencilOperationZero;
    case SG_SOP_REPLACE: return MTLStencilOperationReplace;
    case SG_SOP_INCR_SAT: return MTLStencilOperationIncrementClamp;
    case SG_SOP_DECR_SAT: return MTLStencilOperationDecrementClamp;
    case SG_SOP_INVERT: return MTLStencilOperationInvert;
    case SG_SOP_INCR: return MTLStencilOperationIncrementWrap;
    case SG_SOP_DECR: return MTLStencilOperationDecrementWrap;
    default: return MTLStencilOperationKeep;
    }
}

static id<MTLRenderPipelineState> pipe_get(SgmProg *p, SgmDS *d, const SgState *s, int prim) {
    BOOL hasD = d && d->hasD, hasS = d && d->hasS;
    MTLPixelFormat dfmt = hasD ? d->tex.pixelFormat : MTLPixelFormatInvalid, sfmt = hasS ? d->tex.pixelFormat : MTLPixelFormatInvalid;
    int mask = (s->mask_r ? 8 : 0) | (s->mask_g ? 4 : 0) | (s->mask_b ? 2 : 0) | (s->mask_a ? 1 : 0);
    int sa = s->blend_sep_alpha != 0;
    int fa_s = sa ? s->blend_src_a : s->blend_src, fa_d = sa ? s->blend_dst_a : s->blend_dst, oa = sa ? s->blend_op_a : s->blend_op;
    int cls = prim == SG_PRIM_POINTS ? 2 : prim == SG_PRIM_LINES ? 1 : 0;
    char kb[200];
    if (s->blend) snprintf(kb, sizeof kb, "c%d.%lu.%lu.%lu|%d|B%d.%d.%d.%d.%d.%d", cls, (unsigned long)MTLPixelFormatBGRA8Unorm, (unsigned long)dfmt, (unsigned long)sfmt, mask, s->blend_src, s->blend_dst, s->blend_op, fa_s, fa_d, oa);
    else snprintf(kb, sizeof kb, "c%d.%lu.%lu.%lu|%d|N", cls, (unsigned long)MTLPixelFormatBGRA8Unorm, (unsigned long)dfmt, (unsigned long)sfmt, mask);
    NSString *key = [NSString stringWithUTF8String:kb];
    id v = p->pipes[key];
    if (v) return [v isKindOfClass:[NSNull class]] ? nil : (id<MTLRenderPipelineState>)v;
    MTLRenderPipelineDescriptor *pd = [[MTLRenderPipelineDescriptor alloc] init];
    pd.vertexFunction = p->vs;
    pd.fragmentFunction = p->fs;
    pd.vertexDescriptor = nil;   // the vertex function reads buffer(0) by vertex_id: no fixed-function vertex fetch
    pd.rasterSampleCount = 1;
    pd.inputPrimitiveTopology = prim == SG_PRIM_POINTS ? MTLPrimitiveTopologyClassPoint : prim == SG_PRIM_LINES ? MTLPrimitiveTopologyClassLine : MTLPrimitiveTopologyClassTriangle;
    MTLRenderPipelineColorAttachmentDescriptor *ca = pd.colorAttachments[0];
    ca.pixelFormat = MTLPixelFormatBGRA8Unorm;
    ca.writeMask = (mask & 8 ? MTLColorWriteMaskRed : 0) | (mask & 4 ? MTLColorWriteMaskGreen : 0) | (mask & 2 ? MTLColorWriteMaskBlue : 0) | (mask & 1 ? MTLColorWriteMaskAlpha : 0);
    if (s->blend) {
        ca.blendingEnabled = YES;
        ca.sourceRGBBlendFactor = blend_factor(s->blend_src);
        ca.destinationRGBBlendFactor = blend_factor(s->blend_dst);
        ca.rgbBlendOperation = blend_op(s->blend_op);
        ca.sourceAlphaBlendFactor = blend_factor(fa_s);
        ca.destinationAlphaBlendFactor = blend_factor(fa_d);
        ca.alphaBlendOperation = blend_op(oa);
    }
    pd.depthAttachmentPixelFormat = dfmt;
    pd.stencilAttachmentPixelFormat = sfmt;
    NSError *e = nil;
    id<MTLRenderPipelineState> pso = [g_dev newRenderPipelineStateWithDescriptor:pd error:&e];
    g_st.pipes++;
    if (!pso) {
        char ctx[300];
        snprintf(ctx, sizeof ctx, "program key %016llx prim %d, color %lu depth %lu stencil %lu, mask %d, blend %s %d/%d/%d alpha %d/%d/%d, textures %08x",
                 (unsigned long long)p->info.key, prim, (unsigned long)pd.colorAttachments[0].pixelFormat, (unsigned long)dfmt, (unsigned long)sfmt, mask,
                 s->blend ? "on" : "off", s->blend_src, s->blend_dst, s->blend_op, fa_s, fa_d, oa, (unsigned)p->info.texture_mask);
        record_error("newRenderPipelineStateWithDescriptor", e, ctx);
        p->pipes[key] = [NSNull null];
        return nil;
    }
    p->pipes[key] = pso;
    return pso;
}

static MTLStencilDescriptor *stencil_desc(const SgStencilFace *f) {
    MTLStencilDescriptor *sd = [[MTLStencilDescriptor alloc] init];
    sd.stencilCompareFunction = cmp_func(f->func);
    sd.stencilFailureOperation = stencil_op(f->fail_op);
    sd.depthFailureOperation = stencil_op(f->zfail_op);
    sd.depthStencilPassOperation = stencil_op(f->zpass_op);
    sd.readMask = (uint32_t)(f->read_mask & 255);
    sd.writeMask = (uint32_t)(f->write_mask & 255);
    return sd;
}

static id<MTLDepthStencilState> dss_get(SgmDS *d, const SgState *s) {
    BOOL hasD = d && d->hasD, hasS = d && d->hasS;
    int dt = hasD && s->depth_test, dw = dt && s->depth_write, df = dt ? s->depth_func : SG_ALWAYS;
    int st = hasS && s->stencil_test, two = st && s->stencil_two_sided;
    char kb[256];
    int n = snprintf(kb, sizeof kb, "%d.%d.%d|%d", dt, dw, df, st);
    for (int f = 0; st && f < (two ? 2 : 1) && n < 200; f++) {
        const SgStencilFace *sf = &s->stencil[f];
        n += snprintf(kb + n, sizeof kb - (size_t)n, "|%d.%d.%d.%d.%d.%d", sf->func, sf->read_mask & 255, sf->write_mask & 255, sf->fail_op, sf->zfail_op, sf->zpass_op);
    }
    NSString *key = [NSString stringWithUTF8String:kb];
    id<MTLDepthStencilState> ds = g_dsstates[key];
    if (ds) return ds;
    MTLDepthStencilDescriptor *dd = [[MTLDepthStencilDescriptor alloc] init];
    dd.depthCompareFunction = cmp_func(df);
    dd.depthWriteEnabled = dw ? YES : NO;
    if (st) {
        dd.frontFaceStencil = stencil_desc(&s->stencil[0]);
        dd.backFaceStencil = stencil_desc(&s->stencil[two ? 1 : 0]);
    }
    ds = [g_dev newDepthStencilStateWithDescriptor:dd];
    if (ds) g_dsstates[key] = ds;
    return ds;
}

// ---- debug counters ---------------------------------------------------------------------------------------------------

static unsigned long long declined_total(void) {
    unsigned long long n = 0;
    for (int i = 0; i < DR_COUNT; i++) n += g_st.declined[i];
    return n;
}

static unsigned long long declined_prev(void) {
    unsigned long long n = 0;
    for (int i = 0; i < DR_COUNT; i++) n += g_stPrev.declined[i];
    return n;
}

static void debug_tick(void) {
    if (!g_debug) return;
    double now = [[NSProcessInfo processInfo] systemUptime];
    if (now - g_lastPrint < 1.0) return;
    double dt = g_lastPrint > 0 ? now - g_lastPrint : 1.0;
    g_lastPrint = now;
    char reasons[256] = "";
    size_t rl = 0;
    for (int i = 0; i < DR_COUNT; i++) if (g_st.declined[i] && rl < 200) rl += (size_t)snprintf(reasons + rl, sizeof reasons - rl, " %s=%llu", kReason[i], g_st.declined[i]);
    fprintf(stderr, "[sgpu-metal] gpu %.0f draws/s (total %llu), cpu %.0f/s (total %llu:%s), clears %llu, libs %llu, pipes %llu, uploads %.0f/s, readbacks %.0f/s, commits %.0f/s\n",
            (double)(g_st.draws - g_stPrev.draws) / dt, g_st.draws, (double)(declined_total() - declined_prev()) / dt, declined_total(), reasons,
            g_st.clears, g_st.libs, g_st.pipes, (double)(g_st.uploads - g_stPrev.uploads) / dt, (double)(g_st.readbacks - g_stPrev.readbacks) / dt,
            (double)(g_st.commits - g_stPrev.commits) / dt);
    g_stPrev = g_st;
}

// ---- backend hooks ---------------------------------------------------------------------------------------------------

static int draw_impl(SgTarget *t, const SgState *s, int prim, const SgVertex *v, int n) {
    if (!g_dev || !v || !t->color) return -1;
    if (!dims_ok(t)) return decline(DR_TARGET);
    if (prim == SG_PRIM_TRIANGLES) n -= n % 3;
    else if (prim == SG_PRIM_LINES) n &= ~1;
    if (n <= 0) return 0;
    if (prim == SG_PRIM_LINES && s->line_width > 1.001f) return decline(DR_LINE_WIDTH);
    if (prim == SG_PRIM_TRIANGLES && s->cull && s->cull_back && s->cull_front) return 0;   // everything culled
    if (s->vp_w <= 0 || s->vp_h <= 0) return 0;
    size_t vbytes = (size_t)n * sizeof(SgVertex);
    if (vbytes > ((size_t)256 << 20)) return decline(DR_ALLOC);

    // scissor in Metal's top-left window coordinates
    int tw = t->w, th = t->h;
    int sx0 = 0, sy0 = 0, sx1 = tw, sy1 = th;
    if (s->scissor) {
        sx0 = s->sc_x; sx1 = s->sc_x + s->sc_w;
        if (s->vp_top_left) { sy0 = s->sc_y; sy1 = s->sc_y + s->sc_h; }
        else { sy0 = th - (s->sc_y + s->sc_h); sy1 = th - s->sc_y; }
        if (sx0 < 0) sx0 = 0;
        if (sy0 < 0) sy0 = 0;
        if (sx1 > tw) sx1 = tw;
        if (sy1 > th) sy1 = th;
        if (sx1 <= sx0 || sy1 <= sy0) return 0;
    }

    SgmProg *p = prog_get(s, prim);
    if (!p || p->failed) return -1;

    // textures and samplers first: nothing below may fail half way
    id<MTLTexture> texs[16];
    id<MTLSamplerState> samps[16];
    for (int i = 0; i < 16; i++) {
        if (!(p->info.texture_mask & (1u << i))) continue;
        const SgTexture *tx = NULL;
        SgSampler sm;
        memset(&sm, 0, sizeof sm);
        if (s->program) { tx = s->program->samplers[i].tex; sm = s->program->samplers[i].samp; }
        else if (i < SG_UNITS) { tx = s->tex[i]; if (tx) sgsl_gl_sampler(tx, &sm); }
        if (!tx || (p->info.texture_kind[i] >= 0 && p->info.texture_kind[i] != tx->kind)) return decline(DR_TEXTURE);
        SgmTex *mt = tex_get(tx);
        if (!mt) return decline(DR_TEXTURE);
        id<MTLTexture> use = mt->tex;
        if (sm.srgb) {
            if (!mt->srgb) mt->srgb = [mt->tex newTextureViewWithPixelFormat:MTLPixelFormatRGBA8Unorm_sRGB];
            if (!mt->srgb) return decline(DR_TEXTURE);
            use = mt->srgb;
        }
        id<MTLSamplerState> ss = sampler_get(&sm, tx->kind);
        if (!ss) return decline(DR_BORDER);
        texs[i] = use; samps[i] = ss;
    }

    if (g_batch && (g_batch->bytes > SGM_BATCH_LIMIT || (g_wantCounts && g_batch->visCount >= kVisSlots))) batch_commit(NO);

    SgmColor *c = color_get(t, YES);
    if (!c) return decline(DR_ALLOC);
    SgmDS *d = ds_get(t, YES, YES);
    if ((t->depth || t->stencil) && !d) return decline(DR_ALLOC);
    if (p->info.writes_depth && !(d && d->hasD)) return decline(DR_NODEPTH);
    id<MTLRenderPipelineState> pso = pipe_get(p, d, s, prim);
    if (!pso) return decline(DR_PIPELINE);
    id<MTLDepthStencilState> dss = dss_get(d, s);
    if (!dss) return decline(DR_PIPELINE);

    SgslUniforms u;
    memset(&u, 0, sizeof u);
    sgsl_fill_uniforms(s, &u);
    size_t ub = p->info.uniform_bytes;
    if (ub > sizeof u) ub = sizeof u;
    if (ub < 16) ub = 16;
    size_t voff, uoff;
    SgmChunk *vc = arena_alloc(vbytes, &voff);
    if (!vc) return decline(DR_ALLOC);
    memcpy((uint8_t *)vc->buf.contents + voff, v, vbytes);
    SgmChunk *uc = arena_alloc(ub, &uoff);
    if (!uc) return decline(DR_ALLOC);
    memcpy((uint8_t *)uc->buf.contents + uoff, &u, ub);
    if (!enc_for(c, d)) return decline(DR_ALLOC);

    // ---- point of no return ----
    id<MTLRenderCommandEncoder> e = g_enc;
    [e setRenderPipelineState:pso];
    [e setDepthStencilState:dss];
    MTLViewport vp;
    vp.originX = s->vp_x;
    vp.originY = s->vp_top_left ? s->vp_y : th - s->vp_y - s->vp_h;
    vp.width = s->vp_w; vp.height = s->vp_h;
    vp.znear = s->depth_near; vp.zfar = s->depth_far;
    [e setViewport:vp];
    MTLScissorRect sr = { (NSUInteger)sx0, (NSUInteger)sy0, (NSUInteger)(sx1 - sx0), (NSUInteger)(sy1 - sy0) };
    [e setScissorRect:sr];
    [e setFrontFacingWinding:s->front_ccw ? MTLWindingCounterClockwise : MTLWindingClockwise];
    MTLCullMode cm = MTLCullModeNone;
    if (prim == SG_PRIM_TRIANGLES && s->cull) cm = s->cull_back ? MTLCullModeBack : s->cull_front ? MTLCullModeFront : MTLCullModeNone;
    [e setCullMode:cm];
    [e setDepthClipMode:s->depth_clamp ? MTLDepthClipModeClamp : MTLDepthClipModeClip];
    // Metal scales the constant bias by the smallest depth step of the primitive, the CPU uses absolute depth
    // units: assume depths in [0.5, 1) of a float buffer (step 2^-24). Approximation.
    float bias = s->depth_bias + s->polygon_offset;
    [e setDepthBias:bias * 16777216.0f slopeScale:s->slope_scale_bias clamp:0.0f];
    [e setStencilFrontReferenceValue:(uint32_t)(s->stencil[0].ref & 255) backReferenceValue:(uint32_t)(s->stencil[s->stencil_two_sided ? 1 : 0].ref & 255)];
    if (s->blend) [e setBlendColorRed:s->blend_color[0] green:s->blend_color[1] blue:s->blend_color[2] alpha:s->blend_color[3]];
    [e setVertexBuffer:vc->buf offset:voff atIndex:0];
    [e setVertexBuffer:uc->buf offset:uoff atIndex:1];
    [e setFragmentBuffer:uc->buf offset:uoff atIndex:1];
    for (int i = 0; i < 16; i++) {
        if (!(p->info.texture_mask & (1u << i))) continue;
        [e setFragmentTexture:texs[i] atIndex:(NSUInteger)i];
        [e setFragmentSamplerState:samps[i] atIndex:(NSUInteger)i];
    }
    if (g_encVis && g_batch->visCount < kVisSlots) {
        [e setVisibilityResultMode:MTLVisibilityResultModeCounting offset:(NSUInteger)g_batch->visCount * 8];
        g_batch->visCount++;
    } else if (g_encVis) [e setVisibilityResultMode:MTLVisibilityResultModeDisabled offset:0];
    [e drawPrimitives:prim == SG_PRIM_POINTS ? MTLPrimitiveTypePoint : prim == SG_PRIM_LINES ? MTLPrimitiveTypeLine : MTLPrimitiveTypeTriangle
          vertexStart:0 vertexCount:(NSUInteger)n];
    c->dirty = YES;
    if (d && ((d->hasD && s->depth_test && s->depth_write) || (d->hasS && s->stencil_test))) d->dirty = YES;
    g_st.draws++;
    if ((++g_tick & 63) == 0) debug_tick();
    return 0;
}

static int mt_draw(void *self, SgTarget *t, const SgState *s, int prim, const SgVertex *v, int n) {
    @autoreleasepool { return draw_impl(t, s, prim, v, n); }
}

static int clear_impl(SgTarget *t, const SgState *s, int flags, uint32_t argb, float z, int stencil) {
    if (!g_dev) return -1;
    int nomask = (flags & SG_CLEAR_NOMASK) != 0;
    int doC = (flags & SG_CLEAR_COLOR) && t->color;
    int doD = (flags & SG_CLEAR_DEPTH) && t->depth && (nomask || s->depth_write);
    int doS = (flags & SG_CLEAR_STENCIL) && t->stencil;
    if (!doC && !doD && !doS) return 0;
    if (!dims_ok(t)) return decline(DR_TARGET);
    if (s->scissor) {   // only a scissor that covers the whole target is a full clear
        int x0 = s->sc_x, x1 = s->sc_x + s->sc_w, y0, y1;
        if (s->vp_top_left) { y0 = s->sc_y; y1 = s->sc_y + s->sc_h; }
        else { y0 = t->h - (s->sc_y + s->sc_h); y1 = t->h - s->sc_y; }
        if (x0 > 0 || y0 > 0 || x1 < t->w || y1 < t->h) return decline(DR_PARTIAL_CLEAR);
    }
    if (doC && !nomask && !(s->mask_r && s->mask_g && s->mask_b && s->mask_a)) return decline(DR_PARTIAL_CLEAR);
    if (doS && !nomask && (s->stencil[0].write_mask & 255) != 255) return decline(DR_PARTIAL_CLEAR);

    SgmColor *c = nil;
    SgmDS *d = nil;
    if (doC) { c = color_get(t, NO); if (!c) return decline(DR_ALLOC); }
    if (doD || doS) {
        d = ds_get(t, !doD, !doS);
        if (!d) return decline(DR_ALLOC);
    }
    // a colour resource that was just created has no defined contents until the clear pass below fills it
    enc_end();
    if (!cb_get()) return decline(DR_ALLOC);
    MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
    if (c) {
        float inv = 1.0f / 255.0f;
        rp.colorAttachments[0].texture = c->tex;
        rp.colorAttachments[0].loadAction = MTLLoadActionClear;
        rp.colorAttachments[0].storeAction = MTLStoreActionStore;
        rp.colorAttachments[0].clearColor = MTLClearColorMake(((argb >> 16) & 255) * inv, ((argb >> 8) & 255) * inv, (argb & 255) * inv, ((argb >> 24) & 255) * inv);
    }
    if (d && d->hasD) {
        rp.depthAttachment.texture = d->tex;
        rp.depthAttachment.loadAction = doD ? MTLLoadActionClear : MTLLoadActionLoad;
        rp.depthAttachment.storeAction = MTLStoreActionStore;
        rp.depthAttachment.clearDepth = z < 0.0f ? 0.0 : z > 1.0f ? 1.0 : (double)z;
    }
    if (d && d->hasS) {
        rp.stencilAttachment.texture = d->tex;
        rp.stencilAttachment.loadAction = doS ? MTLLoadActionClear : MTLLoadActionLoad;
        rp.stencilAttachment.storeAction = MTLStoreActionStore;
        rp.stencilAttachment.clearStencil = (uint32_t)(stencil & 255);
    }
    id<MTLRenderCommandEncoder> e = [g_cb renderCommandEncoderWithDescriptor:rp];
    if (!e) return decline(DR_ALLOC);
    [e endEncoding];
    if (c) c->dirty = YES;
    if (d && (doD || doS)) d->dirty = YES;
    g_st.clears++;
    return 0;
}

static int mt_clear(void *self, SgTarget *t, const SgState *s, int flags, uint32_t argb, float z, int stencil) {
    @autoreleasepool { return clear_impl(t, s, flags, argb, z, stencil); }
}

static void mt_sync(void *self, SgTarget *t) {
    if (!g_dev) return;
    @autoreleasepool {
        if (t->color) {
            SgmColor *c = g_colors[PK(t->color)];
            if (c && c->dirty) color_readback(c);
        }
        SgmDS *d1 = t->depth ? g_dsmap[PK(t->depth)] : nil, *d2 = t->stencil ? g_dsmap[PK(t->stencil)] : nil;
        if (d1 && d1->dirty) ds_readback(d1, YES, YES);
        if (d2 && d2 != d1 && d2->dirty) ds_readback(d2, YES, YES);
    }
}

static void mt_cpu_wrote(void *self, SgTarget *t, int x, int y, int w, int h) {
    if (!g_dev) return;
    @autoreleasepool {
        if (t->color) {
            SgmColor *c = g_colors[PK(t->color)];
            if (c) {
                if (c->w != t->w || c->h != t->h) [g_colors removeObjectForKey:PK(t->color)];   // rebuilt from CPU memory on next use
                else {
                    BOOL whole = w <= 0 || h <= 0;
                    if (whole) { x = 0; y = 0; w = c->w; h = c->h; }
                    if (!upload_color(c, x, y, w, h)) [g_colors removeObjectForKey:PK(t->color)];   // rebuilt from CPU memory
                    else if (whole) c->dirty = NO;
                }
            }
        }
        const void *keys[2] = { t->depth, t->stencil };
        for (int i = 0; i < 2; i++) {
            if (!keys[i]) continue;
            SgmDS *r = g_dsmap[PK(keys[i])];
            if (!r) continue;
            if (r->w != t->w || r->h != t->h) { ds_unmap(r); continue; }
            if (i == 0 && r->hasD && r->dkey == keys[i]) { if (upload_ds(r, YES)) r->dirty = NO; }
            if (i == 1 && r->hasS && r->skey == keys[i]) { if (upload_ds(r, NO)) r->dirty = NO; }
        }
    }
}

static void mt_target_gone(void *self, const void *color, const void *depth, const void *stencil) {
    if (!g_dev) return;
    @autoreleasepool {
        if (color) [g_colors removeObjectForKey:PK(color)];
        const void *keys[2] = { depth, stencil };
        for (int i = 0; i < 2; i++) {
            if (!keys[i]) continue;
            SgmDS *r = g_dsmap[PK(keys[i])];
            if (!r) continue;
            // the other plane's memory lives on: keep its GPU-side data
            BOOL keepD = r->dkey && r->dkey != depth && r->dkey != stencil, keepS = r->skey && r->skey != depth && r->skey != stencil;
            if (r->dirty && (keepD || keepS)) ds_readback(r, keepD, keepS);
            ds_unmap(r);
        }
    }
}

static void mt_texture_gone(void *self, SgTexture *tex) {
    if (!g_dev || !tex->id) return;
    @autoreleasepool { [g_texs removeObjectForKey:[NSNumber numberWithUnsignedLongLong:tex->id]]; }
}

static void mt_end_frame(void *self) {
    if (!g_dev) return;
    @autoreleasepool {
        batch_commit(NO);
        debug_tick();
    }
}

static SgBackend g_backend = { "metal", NULL, mt_draw, mt_clear, mt_sync, mt_cpu_wrote, mt_target_gone, mt_texture_gone, mt_end_frame };

// ---- public (weakly referenced from sgpu.c) ---------------------------------------------------------------------------------

const SgBackend *sg_metal_backend(void) {
    @autoreleasepool {
        if (!g_inited) {
            g_inited = YES;
            const char *dbgenv = getenv("SOUL_GPU_DEBUG");
            g_debug = dbgenv && *dbgenv && *dbgenv != '0';
            id<MTLDevice> dev = MTLCreateSystemDefaultDevice();
#if TARGET_OS_OSX
            if (!dev) {
                NSArray<id<MTLDevice>> *all = MTLCopyAllDevices();
                if (all.count > 0) dev = all[0];
            }
#endif
            if (dev) {
                id<MTLCommandQueue> q = [dev newCommandQueue];
                if (q) {
                    g_dev = dev; g_q = q;
                    g_unified = dev.hasUnifiedMemory;
                    snprintf(g_devname, sizeof g_devname, "%s", dev.name ? [dev.name UTF8String] : "?");
                    g_pending = [NSMutableArray array]; g_free = [NSMutableArray array];
                    g_colors = [NSMutableDictionary dictionary]; g_dsmap = [NSMutableDictionary dictionary];
                    g_texs = [NSMutableDictionary dictionary]; g_progs = [NSMutableDictionary dictionary];
                    g_samplers = [NSMutableDictionary dictionary]; g_dsstates = [NSMutableDictionary dictionary];
                    g_memo = [NSMutableArray array];
                    for (int i = 0; i < kMemo; i++) [g_memo addObject:[SgmMemo new]];
                    dbg("device %s, unified memory %d", g_devname, (int)g_unified);
                }
            }
        }
        return g_dev ? &g_backend : NULL;
    }
}

// Writes every dirty resource back to the CPU memory and drops all targets and textures (compiled shaders stay).
// By the sgpu rules every tracked pointer is still valid memory (front ends call target_gone before freeing).
void sg_metal_release_all(void) {
    if (!g_dev) return;
    @autoreleasepool {
        for (SgmColor *c in [g_colors allValues]) if (c->dirty) color_readback(c);
        for (SgmDS *r in [[NSSet setWithArray:[g_dsmap allValues]] allObjects]) if (r->dirty) ds_readback(r, YES, YES);
        batch_commit(YES);
        [g_colors removeAllObjects]; [g_dsmap removeAllObjects]; [g_texs removeAllObjects];
        g_encC = nil; g_encD = nil;
        [g_free removeAllObjects];
    }
}

int sg_metal_status(char *buf, size_t len) {
    if (!buf || !len) return g_dev != nil;
    if (!g_dev) { snprintf(buf, len, "Metal kullanılamıyor"); return 0; }
    snprintf(buf, len, "Metal · %s · %llu çizim GPU'da, %llu CPU'ya düştü", g_devname, g_st.draws, declined_total());
    return 1;
}

// Short per-reason summary of the declined draws/clears for the soulrun --gpu-stats line, e.g. "texture=3 alloc=1".
int sg_metal_decline_summary(char *buf, size_t len) {
    if (!buf || !len) return 0;
    buf[0] = 0;
    size_t rl = 0;
    for (int i = 0; i < DR_COUNT; i++)
        if (g_st.declined[i] && rl + 40 < len)
            rl += (size_t)snprintf(buf + rl, len - rl, "%s%s=%llu", rl ? " " : "", kReason[i], g_st.declined[i]);
    return (int)rl;
}

// First and last Metal creation error (library / function / pipeline) for the soulrun --gpu-stats line; 0 when none.
int sg_metal_last_error(char *buf, size_t len) {
    if (!buf || !len) return 0;
    buf[0] = 0;
    if (!g_errLast[0]) return 0;
    if (!strcmp(g_errFirst, g_errLast)) snprintf(buf, len, "%s", g_errLast);
    else snprintf(buf, len, "%s ... %s", g_errFirst, g_errLast);
    return (int)strlen(buf);
}

uint64_t sg_metal_pixels_drawn(void) {
    if (!g_dev) return 0;
    @autoreleasepool {
        g_wantCounts = YES;   // encoders created from now on carry a visibility buffer
        batch_commit(YES);
        return g_visTotal;
    }
}
