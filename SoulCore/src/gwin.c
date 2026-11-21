// SoulCore - guest address space for 32-bit (x86) programs.
//
// iOS keeps the low 4 GB of every process unmapped, so a 32-bit Windows program
// cannot use host addresses directly. Instead Soul reserves a 4 GB window and the
// guest address a lives at host address g_gbase + a. Everything the program can
// see (its images, stacks, TEB/PEB, heaps, API thunks and every buffer an API hands
// back) is allocated inside this window. 64-bit programs keep g_gbase = 0.
#include "hle.h"
#include <sys/mman.h>
#ifdef __APPLE__
#include <malloc/malloc.h>
#define HOST_MSIZE(p) malloc_size(p)
#else
#include <malloc.h>
#define HOST_MSIZE(p) malloc_usable_size(p)
#endif

uint64_t g_gbase;
int g_mode32;

#define GRAN 0x10000ull                    // Windows allocation granularity (64 KB)
#define NGRAN (0x100000000ull / GRAN)
static uint8_t *g_used;                     // one byte per granule
static void *g_reservation;
static size_t g_reservation_size;
static uint64_t g_hint;

int gwin_init(void) {
    if (g_reservation) return 1;
    g_reservation_size = (size_t)0x100000000ull + GRAN;
    void *r = mmap(NULL, g_reservation_size, PROT_NONE, MAP_PRIVATE | MAP_ANON | MAP_NORESERVE, -1, 0);
    if (r == MAP_FAILED) return 0;
    g_reservation = r;
    g_hint = 0;
    g_gbase = ((uint64_t)(uintptr_t)r + GRAN - 1) & ~(GRAN - 1);
    g_used = calloc(NGRAN, 1);
    g_used[0] = 1;                          // NULL page region
    for (uint64_t a = 0x7FFE0000ull; a < 0x80000000ull; a += GRAN) g_used[a / GRAN] = 1; // KUSER_SHARED_DATA area
    for (uint64_t a = 0xFFFF0000ull; a < 0x100000000ull; a += GRAN) g_used[a / GRAN] = 1;
    return 1;
}

void gwin_release(void) {
    if (!g_reservation) return;
    munmap(g_reservation, g_reservation_size);
    g_reservation = NULL;
    free(g_used);
    g_used = NULL;
    g_gbase = 0;
}

static int range_free(uint64_t a, uint64_t n) {
    if (a + n > 0x100000000ull) return 0;
    for (uint64_t g = a / GRAN; g < (a + n) / GRAN; g++) if (g_used[g]) return 0;
    return 1;
}

// Maps size bytes of zeroed read/write memory. want = 0: anywhere. Returns the guest address or 0.
uint64_t gwin_map(uint64_t want, size_t size) {
    if (!g_reservation || !size) return 0;
    uint64_t n = ((uint64_t)size + GRAN - 1) & ~(GRAN - 1);
    uint64_t a = 0;
    want &= ~(GRAN - 1);
    if (want && range_free(want, n)) a = want;
    if (!a) {
        // next fit between 1 MB and 2 GB (images are loaded first, at their preferred bases)
        const uint64_t lo = 0x00110000ull, hi = 0x7FFE0000ull;
        if (g_hint < lo || g_hint >= hi) g_hint = lo;
        for (int pass = 0; pass < 2 && !a; pass++) {
            uint64_t from = pass ? lo : g_hint, to = pass ? g_hint : hi;
            for (uint64_t c = from; c + n <= to; c += GRAN) {
                if (g_used[c / GRAN]) continue;
                uint64_t e = c;
                while (e < c + n && !g_used[e / GRAN]) e += GRAN;
                if (e >= c + n) { a = c; break; }
                c = e; // skip the used granule that ended this run
            }
        }
        if (a) g_hint = a + n;
    }
    if (!a) return 0;
    void *p = mmap((void *)(uintptr_t)(g_gbase + a), (size_t)n, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON | MAP_FIXED, -1, 0);
    if (p == MAP_FAILED) return 0;
    for (uint64_t g = a / GRAN; g < (a + n) / GRAN; g++) g_used[g] = 1;
    return a;
}

void gwin_unmap(uint64_t a, size_t size) {
    if (!g_reservation || !a) return;
    uint64_t n = ((uint64_t)size + GRAN - 1) & ~(GRAN - 1);
    mmap((void *)(uintptr_t)(g_gbase + a), (size_t)n, PROT_NONE, MAP_PRIVATE | MAP_ANON | MAP_FIXED | MAP_NORESERVE, -1, 0);
    for (uint64_t g = a / GRAN; g < (a + n) / GRAN && g < NGRAN; g++) g_used[g] = 0;
}

int gwin_contains(uint64_t a) { return g_reservation && a < 0x100000000ull && g_used[a / GRAN]; }

// ---- heap for memory handed to the guest (malloc, HeapAlloc, strings returned by APIs) -------------
// 64-bit programs use the host heap; 32-bit programs get blocks inside the window.

#define HMAGIC 0x50414548u
typedef struct HHdr { uint32_t magic, cls; uint64_t size; } HHdr; // 16 bytes, keeps 16-byte alignment
#define NCLS 12                                                // 16 B .. 32 KB
static void *g_free_list[NCLS];
static uint8_t *g_arena, *g_arena_end;

static int cls_for(size_t n) { int c = 0; size_t s = 16; while (s < n && c < NCLS) { s <<= 1; c++; } return c; }
static size_t cls_size(int c) { return (size_t)16 << c; }

static void *gh_alloc(size_t n) {
    if (n == 0) n = 1;
    if (n > cls_size(NCLS - 1) - sizeof(HHdr)) {
        uint64_t g = gwin_map(0, n + sizeof(HHdr));
        if (!g) return NULL;
        HHdr *h = G2H(g);
        h->magic = HMAGIC; h->cls = 0xFFFF; h->size = n;
        return h + 1;
    }
    int c = cls_for(n + sizeof(HHdr));
    HHdr *h;
    if (g_free_list[c]) {
        h = g_free_list[c];
        g_free_list[c] = *(void **)(h + 1);
    } else {
        size_t sz = cls_size(c);
        if (!g_arena || g_arena + sz > g_arena_end) {
            uint64_t g = gwin_map(0, 1 << 20);
            if (!g) return NULL;
            g_arena = G2H(g); g_arena_end = g_arena + (1 << 20);
        }
        h = (HHdr *)(void *)g_arena;
        g_arena += sz;
    }
    h->magic = HMAGIC; h->cls = (uint32_t)c; h->size = n;
    return h + 1;
}
static void gh_free(void *p) {
    if (!p) return;
    HHdr *h = (HHdr *)p - 1;
    if (h->magic != HMAGIC) return; // not ours (or double free): ignore instead of corrupting
    h->magic = 0;
    if (h->cls == 0xFFFF) { gwin_unmap(H2G(h), h->size + sizeof(HHdr)); return; }
    *(void **)(h + 1) = g_free_list[h->cls];
    g_free_list[h->cls] = h;
}

void *g_malloc(size_t n) { return g_mode32 ? gh_alloc(n) : malloc(n ? n : 1); }
void *g_calloc(size_t a, size_t b) {
    if (!g_mode32) return calloc(a ? a : 1, b ? b : 1);
    size_t n = a * b;
    void *p = gh_alloc(n);
    if (p) memset(p, 0, n ? n : 1);
    return p;
}
void g_free(void *p) { if (g_mode32) gh_free(p); else free(p); }
size_t g_msize(void *p) {
    if (!p) return 0;
    if (!g_mode32) return HOST_MSIZE(p);
    HHdr *h = (HHdr *)p - 1;
    return h->magic == HMAGIC ? (size_t)h->size : 0;
}
void *g_realloc(void *p, size_t n) {
    if (!g_mode32) return realloc(p, n);
    if (!p) return gh_alloc(n);
    if (!n) { gh_free(p); return NULL; }
    HHdr *h = (HHdr *)p - 1;
    if (h->magic != HMAGIC) return NULL;
    if (h->cls != 0xFFFF && n + sizeof(HHdr) <= cls_size((int)h->cls)) { h->size = n; return p; }
    void *q = gh_alloc(n);
    if (!q) return NULL;
    memcpy(q, p, h->size < n ? h->size : n);
    gh_free(p);
    return q;
}
char *g_strdup(const char *s) { size_t n = strlen(s) + 1; char *d = g_malloc(n); memcpy(d, s, n); return d; }
void gheap_reset(void) { memset(g_free_list, 0, sizeof g_free_list); g_arena = g_arena_end = NULL; }

void *g_pages(size_t size) {
    if (g_mode32) { uint64_t a = gwin_map(0, size); return a ? G2H(a) : NULL; }
    void *p = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    return p == MAP_FAILED ? NULL : p;
}
void g_pages_free(void *p, size_t size) {
    if (!p) return;
    if (g_mode32) gwin_unmap(H2G(p), size);
    else munmap(p, size);
}
