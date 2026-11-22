// SoulCore - IStream objects implemented by the host: a growable memory stream (CreateStreamOnHGlobal,
// SHCreateMemStream) and a file stream (SHCreateStreamOnFile*), plus helpers that read and write guest
// IStreams through their vtables (used by GDI+).
#include "stream.h"
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <errno.h>

#define S_OK 0u
#define S_FALSE 1u
#define E_NOINTERFACE 0x80004002u
#define E_POINTER 0x80004003u
#define E_INVALIDARG 0x80070057u
#define E_OUTOFMEMORY 0x8007000Eu
#define E_NOTIMPL 0x80004001u
#define STG_E_INVALIDFUNCTION 0x80030001u
#define STG_E_ACCESSDENIED 0x80030005u
#define STG_E_INVALIDPOINTER 0x80030009u
#define STG_E_MEDIUMFULL 0x80030070u
#define HR_FILE_NOT_FOUND 0x80070002u
#define HR_ACCESS_DENIED 0x80070005u

typedef struct StreamObj {
    int refs;
    int file;            // 0 memory, 1 file
    uint8_t *buf;        // memory: guest visible block (the HGLOBAL)
    size_t size, cap;
    int free_buf;        // delete the block on release
    int fd, writable;
    uint64_t pos;
    uint64_t obj;        // guest address of the COM object
} StreamObj;

static uint64_t g_stream_vt; // per process (thunks live in the process's address space)
SOUL_PROCESS_VAR(g_stream_vt)

static StreamObj *so_of(uint64_t this_) {
    if (!this_) return NULL;
    uint8_t *o = G2H(this_);
    StreamObj *s;
    memcpy(&s, o + 8, sizeof s);
    return s;
}

static int guid_eq(uint64_t p, uint32_t d1) {
    uint8_t g[16];
    memcpy(g, G2H(p), 16);
    uint32_t a = g[0] | (g[1] << 8) | ((uint32_t)g[2] << 16) | ((uint32_t)g[3] << 24);
    static const uint8_t tail[12] = { 0, 0, 0, 0, 0xC0, 0, 0, 0, 0, 0, 0, 0x46 };
    return a == d1 && !memcmp(g + 4, tail, 12);
}

HLE(is_QueryInterface) {
    uint64_t riid = ARG(1), ppv = ARG(2);
    if (!ppv) { RET(E_POINTER); return; }
    if (riid && (guid_eq(riid, 0) || guid_eq(riid, 0xC) || guid_eq(riid, 0xA))) {
        StreamObj *s = so_of(ARG(0));
        if (s) s->refs++;
        wr_ptr(ppv, ARG(0));
        RET(S_OK);
        return;
    }
    wr_ptr(ppv, 0);
    RET(E_NOINTERFACE);
}
HLE(is_AddRef) { StreamObj *s = so_of(ARG(0)); RET(s ? ++s->refs : 0); }
HLE(is_Release) {
    StreamObj *s = so_of(ARG(0));
    if (!s) { RET(0); return; }
    if (--s->refs > 0) { RET(s->refs); return; }
    if (s->file) { if (s->fd >= 0) close(s->fd); }
    else if (s->free_buf && s->buf) g_free(s->buf);
    g_free(G2H(s->obj));
    free(s);
    RET(0);
}

static uint64_t stream_size(StreamObj *s) {
    if (!s->file) return s->size;
    struct stat sb;
    return fstat(s->fd, &sb) == 0 ? (uint64_t)sb.st_size : 0;
}

static int mem_grow(StreamObj *s, size_t need) {
    if (need <= s->cap) return 1;
    size_t nc = s->cap ? s->cap : 256;
    while (nc < need) nc *= 2;
    uint8_t *nb = s->buf ? g_realloc(s->buf, nc) : g_calloc(1, nc);
    if (!nb) return 0;
    if (s->buf) memset(nb + s->cap, 0, nc - s->cap);
    s->buf = nb; s->cap = nc;
    return 1;
}

static uint32_t so_read(StreamObj *s, void *dst, size_t cb, size_t *got) {
    *got = 0;
    if (s->file) {
        ssize_t r = pread(s->fd, dst, cb, (off_t)s->pos);
        if (r < 0) return STG_E_ACCESSDENIED;
        *got = (size_t)r; s->pos += (uint64_t)r;
    } else {
        size_t n = s->pos >= s->size ? 0 : (s->size - (size_t)s->pos < cb ? s->size - (size_t)s->pos : cb);
        if (n) memcpy(dst, s->buf + s->pos, n);
        *got = n; s->pos += n;
    }
    return S_OK;
}
static uint32_t so_write(StreamObj *s, const void *src, size_t cb, size_t *put) {
    *put = 0;
    if (s->file) {
        if (!s->writable) return STG_E_ACCESSDENIED;
        ssize_t r = pwrite(s->fd, src, cb, (off_t)s->pos);
        if (r < 0) return STG_E_MEDIUMFULL;
        *put = (size_t)r; s->pos += (uint64_t)r;
        return S_OK;
    }
    if (cb > (size_t)1 << 30 || s->pos > (uint64_t)1 << 30) return STG_E_MEDIUMFULL;
    size_t end = (size_t)s->pos + cb;
    if (!mem_grow(s, end)) return STG_E_MEDIUMFULL;
    memcpy(s->buf + s->pos, src, cb);
    if (end > s->size) s->size = end;
    *put = cb; s->pos = end;
    return S_OK;
}

HLE(is_Read) {
    StreamObj *s = so_of(ARG(0));
    uint64_t pv = ARG(1), pcb = ARG(3);
    uint32_t cb = (uint32_t)ARG(2);
    if (!s || !pv) { RET(STG_E_INVALIDPOINTER); return; }
    size_t got;
    uint32_t hr = so_read(s, G2H(pv), cb, &got);
    if (pcb) mem_wr(pcb, 4, got);
    RET(hr != S_OK ? hr : (got < cb ? S_FALSE : S_OK));
}
HLE(is_Write) {
    StreamObj *s = so_of(ARG(0));
    uint64_t pv = ARG(1), pcb = ARG(3);
    if (!s || !pv) { RET(STG_E_INVALIDPOINTER); return; }
    size_t put;
    uint32_t hr = so_write(s, G2H(pv), (uint32_t)ARG(2), &put);
    if (pcb) mem_wr(pcb, 4, put);
    RET(hr);
}
static uint64_t arg64(X86State *st, int n) { return g_mode32 ? (ARG(n) | (ARG(n + 1) << 32)) : ARG(n); }
HLE(is_Seek) {
    StreamObj *s = so_of(ARG(0));
    int64_t mv = (int64_t)arg64(st, 1);
    uint32_t org = (uint32_t)ARG(g_mode32 ? 3 : 2);
    uint64_t pn = ARG(g_mode32 ? 4 : 3);
    if (!s) { RET(STG_E_INVALIDPOINTER); return; }
    int64_t base;
    if (org == 0) base = 0; else if (org == 1) base = (int64_t)s->pos; else if (org == 2) base = (int64_t)stream_size(s);
    else { RET(STG_E_INVALIDFUNCTION); return; }
    if (base + mv < 0) { RET(STG_E_INVALIDFUNCTION); return; }
    s->pos = (uint64_t)(base + mv);
    if (pn) mem_wr(pn, 8, s->pos);
    RET(S_OK);
}
HLE(is_SetSize) {
    StreamObj *s = so_of(ARG(0));
    uint64_t sz = arg64(st, 1);
    if (!s) { RET(STG_E_INVALIDPOINTER); return; }
    if (s->file) { RET(s->writable && ftruncate(s->fd, (off_t)sz) == 0 ? S_OK : STG_E_ACCESSDENIED); return; }
    if (sz > (uint64_t)1 << 30 || !mem_grow(s, (size_t)sz)) { RET(STG_E_MEDIUMFULL); return; }
    if (sz > s->size) memset(s->buf + s->size, 0, (size_t)sz - s->size);
    s->size = (size_t)sz;
    RET(S_OK);
}
HLE(is_CopyTo) { // (this, pstm, cb (64-bit), pcbRead, pcbWritten)
    StreamObj *s = so_of(ARG(0));
    uint64_t dst = ARG(1), cb = arg64(st, 2);
    uint64_t pr = ARG(g_mode32 ? 4 : 3), pw = ARG(g_mode32 ? 5 : 4);
    if (!s || !dst) { RET(STG_E_INVALIDPOINTER); return; }
    uint64_t total = 0, written = 0;
    uint8_t *tmp = malloc(65536);
    uint32_t hr = tmp ? S_OK : E_OUTOFMEMORY;
    while (cb && tmp) {
        size_t got;
        so_read(s, tmp, cb > 65536 ? 65536 : (size_t)cb, &got);
        if (!got) break;
        total += got; cb -= got;
        if (stream_write_all(st, dst, tmp, got)) { hr = STG_E_MEDIUMFULL; break; }
        written += got;
    }
    free(tmp);
    if (pr) mem_wr(pr, 8, total);
    if (pw) mem_wr(pw, 8, written);
    RET(hr);
}
HLE(is_Commit) { RET(S_OK); }
HLE(is_Revert) { RET(S_OK); }
HLE(is_Lock) { RET(STG_E_INVALIDFUNCTION); }
HLE(is_Stat) {
    StreamObj *s = so_of(ARG(0));
    uint64_t p = ARG(1);
    if (!s || !p) { RET(STG_E_INVALIDPOINTER); return; }
    size_t total = g_mode32 ? 72 : 80;
    memset(G2H(p), 0, total);
    mem_wr(p + (g_mode32 ? 4 : 8), 4, 2); // STGTY_STREAM
    mem_wr(p + 8 + (g_mode32 ? 0 : 8), 8, stream_size(s));
    mem_wr(p + (g_mode32 ? 40 : 48), 4, s->file ? (s->writable ? 2 : 0) : 2);
    RET(S_OK);
}
HLE(is_Clone) {
    StreamObj *s = so_of(ARG(0));
    uint64_t pp = ARG(1);
    if (!s || !pp) { RET(STG_E_INVALIDPOINTER); return; }
    wr_ptr(pp, 0);
    RET(E_NOTIMPL);
}

typedef struct { const char *name; HleFn fn; int n32; } SMethod;

static uint64_t stream_vtable(void) {
    if (g_stream_vt) return g_stream_vt;
    static const SMethod m[] = {
        { "QueryInterface", is_QueryInterface, 3 }, { "AddRef", is_AddRef, 1 }, { "Release", is_Release, 1 },
        { "Read", is_Read, 4 }, { "Write", is_Write, 4 }, { "Seek", is_Seek, 5 }, { "SetSize", is_SetSize, 3 },
        { "CopyTo", is_CopyTo, 6 }, { "Commit", is_Commit, 2 }, { "Revert", is_Revert, 1 }, { "LockRegion", is_Lock, 6 },
        { "UnlockRegion", is_Lock, 6 }, { "Stat", is_Stat, 3 }, { "Clone", is_Clone, 2 },
    };
    int n = (int)(sizeof m / sizeof m[0]);
    uint8_t *t = g_calloc((size_t)n, (size_t)PSZ);
    for (int i = 0; i < n; i++) {
        char name[64];
        snprintf(name, sizeof name, "IStream::%s", m[i].name);
        uint32_t id = hle_thunk_for(g_proc, m[i].fn, "ole32", name);
        g_proc->thunks[id].argbytes = g_mode32 ? (uint16_t)(4 * m[i].n32) : 0;
        wr_ptr(H2G(t) + (uint64_t)i * PSZ, g_proc->thunk_base + (uint64_t)id * SOUL_THUNK_STRIDE);
    }
    g_stream_vt = H2G(t);
    return g_stream_vt;
}

static uint64_t so_new(StreamObj *s) {
    uint8_t *o = g_calloc(1, 16);
    if (!o) { free(s); return 0; }
    wr_ptr(H2G(o), stream_vtable());
    memcpy(o + 8, &s, sizeof s);
    s->refs = 1;
    s->obj = H2G(o);
    return s->obj;
}

uint64_t stream_create_mem(const void *data, size_t len) {
    StreamObj *s = calloc(1, sizeof *s);
    if (!s) return 0;
    s->free_buf = 1;
    if (len) {
        if (!mem_grow(s, len)) { free(s); return 0; }
        memcpy(s->buf, data, len);
        s->size = len;
    }
    return so_new(s);
}

void ole_CreateStreamOnHGlobal(X86State *st) {
    uint64_t hg = ARG(0), pp = ARG(2);
    if (!pp) { RET(E_INVALIDARG); return; }
    StreamObj *s = calloc(1, sizeof *s);
    if (!s) { RET(E_OUTOFMEMORY); return; }
    s->free_buf = ARG(1) != 0;
    if (hg) { s->buf = G2H(hg); s->cap = s->size = g_msize(s->buf); }
    else { s->free_buf = 1; mem_grow(s, 256); }
    uint64_t o = so_new(s);
    if (!o) { RET(E_OUTOFMEMORY); return; }
    wr_ptr(pp, o);
    RET(S_OK);
}
void ole_GetHGlobalFromStream(X86State *st) {
    StreamObj *s = so_of(ARG(0));
    if (!s || s->file || !ARG(1)) { RET(E_INVALIDARG); return; }
    wr_ptr(ARG(1), H2G(s->buf));
    RET(S_OK);
}
void shl_SHCreateMemStream(X86State *st) {
    uint64_t init = ARG(0); uint32_t n = (uint32_t)ARG(1);
    uint64_t o = stream_create_mem(init && n ? G2H(init) : NULL, init ? n : 0);
    RET(o);
}

static void file_stream(X86State *st, const char *win, uint32_t mode, int create, uint64_t pp) {
    if (!pp) { RET(E_INVALIDARG); return; }
    wr_ptr(pp, 0);
    char host[2600];
    path_w2h(win, host, sizeof host);
    int acc = mode & 3, flags = acc == 0 ? O_RDONLY : O_RDWR;
    if (create || (mode & 0x1000)) flags |= O_CREAT | O_TRUNC;
    int fd = open(host, flags, 0644);
    if (fd < 0 && acc == 0) fd = -1;
    if (fd < 0) { RET(errno == EACCES ? HR_ACCESS_DENIED : HR_FILE_NOT_FOUND); return; }
    StreamObj *s = calloc(1, sizeof *s);
    s->file = 1; s->fd = fd; s->writable = acc != 0;
    uint64_t o = so_new(s);
    if (!o) { close(fd); RET(E_OUTOFMEMORY); return; }
    wr_ptr(pp, o);
    RET(S_OK);
}
void shl_SHCreateStreamOnFileA(X86State *st) {
    file_stream(st, (const char *)PTR(0), (uint32_t)ARG(1), 0, ARG(2));
}
void shl_SHCreateStreamOnFileW(X86State *st) {
    char *u = w2u_dup((const uint16_t *)PTR(0));
    file_stream(st, u, (uint32_t)ARG(1), 0, ARG(2));
    free(u);
}
void shl_SHCreateStreamOnFileEx(X86State *st) { // (file, mode, attributes, create, template, ppstm)
    char *u = w2u_dup((const uint16_t *)PTR(0));
    file_stream(st, u, (uint32_t)ARG(1), (int)ARG(3), ARG(5));
    free(u);
}

// ---- guest IStream access ---------------------------------------------------------------------------

static uint64_t vcall_fn(uint64_t obj, int i) { return rd_ptr(rd_ptr(obj) + (uint64_t)i * PSZ); }

uint8_t *stream_read_all(X86State *st, uint64_t pstm, size_t *len) {
    *len = 0;
    if (!pstm) return NULL;
    uint64_t vt = rd_ptr(pstm);
    uint8_t *out = NULL; size_t n = 0, cap = 0;
    if (vt == g_stream_vt && g_stream_vt) { // our own object: no guest calls
        StreamObj *s = so_of(pstm);
        if (!s) return NULL;
        uint64_t sz = stream_size(s);
        size_t rest = s->pos >= sz ? 0 : (size_t)(sz - s->pos);
        out = malloc(rest + 1);
        if (!out) return NULL;
        size_t got;
        so_read(s, out, rest, &got);
        *len = got;
        return out;
    }
    uint8_t *chunk = g_malloc(65536);
    uint64_t pcb = H2G(g_malloc(8));
    if (!chunk || !pcb) { g_free(chunk); return NULL; }
    for (;;) {
        mem_wr(pcb, 4, 0);
        uint64_t a[4] = { pstm, H2G(chunk), 65536, pcb };
        uint32_t hr = (uint32_t)soul_guest_call(st, vcall_fn(pstm, 3), 4, a);
        uint32_t got = (uint32_t)mem_rd(pcb, 4);
        if ((int32_t)hr < 0 && !got) break;
        if (got > 65536) got = 65536;
        if (n + got + 1 > cap) {
            size_t nc = cap ? cap * 2 : 65536;
            while (nc < n + got + 1) nc *= 2;
            uint8_t *nb = realloc(out, nc);
            if (!nb) break;
            out = nb; cap = nc;
        }
        memcpy(out + n, chunk, got);
        n += got;
        if (!got || n > ((size_t)1 << 30)) break;
    }
    g_free(chunk); g_free(G2H(pcb));
    if (!out) out = malloc(1);
    *len = n;
    return out;
}

int stream_write_all(X86State *st, uint64_t pstm, const void *data, size_t len) {
    if (!pstm) return 1;
    uint64_t vt = rd_ptr(pstm);
    if (vt == g_stream_vt && g_stream_vt) {
        StreamObj *s = so_of(pstm);
        size_t put;
        return s && so_write(s, data, len, &put) == S_OK && put == len ? 0 : 1;
    }
    size_t done = 0;
    uint8_t *chunk = g_malloc(65536);
    uint64_t pcb = H2G(g_malloc(8));
    int rc = 0;
    while (done < len && chunk) {
        size_t c = len - done > 65536 ? 65536 : len - done;
        memcpy(chunk, (const uint8_t *)data + done, c);
        mem_wr(pcb, 4, 0);
        uint64_t a[4] = { pstm, H2G(chunk), c, pcb };
        uint32_t hr = (uint32_t)soul_guest_call(st, vcall_fn(pstm, 4), 4, a);
        uint32_t put = (uint32_t)mem_rd(pcb, 4);
        if ((int32_t)hr < 0 || !put) { rc = 1; break; }
        done += put;
    }
    g_free(chunk); g_free(G2H(pcb));
    return rc;
}
