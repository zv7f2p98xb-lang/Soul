// SoulCore - the COM objects installers and programs create most: shell links (IShellLink +
// IPersistFile, written as real .lnk files), IMalloc and ITaskbarList, plus CoCreateInstance.
//
// A COM object here is a block of guest memory whose first pointers are vtables of HLE thunks
// (stdcall methods with "this" as the first argument); the object's state lives on the host.
#include "hle.h"
#include <ctype.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>

#define S_OK 0
#define E_NOINTERFACE 0x80004002u
#define E_POINTER 0x80004003u
#define E_FAIL 0x80004005u
#define REGDB_E_CLASSNOTREG 0x80040154u
#define CLASS_E_NOAGGREGATION 0x80040110u

static char *gstr(uint64_t p, int wide) {
    if (!p) return NULL;
    return wide ? w2u_dup((const uint16_t *)G2H(p)) : strdup((const char *)G2H(p));
}
static void put_trunc(uint64_t buf, size_t cap, const char *s, int wide) {
    if (!buf || !cap) return;
    if (wide) {
        uint16_t *w = u2w_dup(s);
        size_t n = wstrlen(w);
        if (n > cap - 1) n = cap - 1;
        memcpy(G2H(buf), w, n * 2);
        ((uint16_t *)G2H(buf))[n] = 0;
        free(w);
        return;
    }
    size_t n = strlen(s);
    if (n > cap - 1) n = cap - 1;
    memcpy(G2H(buf), s, n);
    ((char *)G2H(buf))[n] = 0;
}

// ---- GUIDs -------------------------------------------------------------------------------------------

typedef struct Guid { uint32_t d1; uint16_t d2, d3; uint8_t d4[8]; } Guid;
static int guid_is(uint64_t p, uint32_t d1, uint16_t d2, uint16_t d3, const char *d4) {
    if (!p) return 0;
    Guid g;
    memcpy(&g, G2H(p), 16);
    return g.d1 == d1 && g.d2 == d2 && g.d3 == d3 && memcmp(g.d4, d4, 8) == 0;
}
#define COMMON_D4 "\xC0\x00\x00\x00\x00\x00\x00\x46"
#define IS_IUNKNOWN(p) guid_is(p, 0x00000000, 0, 0, COMMON_D4)
#define IS_SHELLLINK_CLSID(p) guid_is(p, 0x00021401, 0, 0, COMMON_D4)
#define IS_ISHELLLINKA(p) guid_is(p, 0x000214EE, 0, 0, COMMON_D4)
#define IS_ISHELLLINKW(p) guid_is(p, 0x000214F9, 0, 0, COMMON_D4)
#define IS_IPERSISTFILE(p) guid_is(p, 0x0000010B, 0, 0, COMMON_D4)
#define IS_IPERSIST(p) guid_is(p, 0x0000010C, 0, 0, COMMON_D4)
#define IS_TASKBARLIST_CLSID(p) guid_is(p, 0x56FDF344, 0xFD6D, 0x11D0, "\x95\x8A\x00\x60\x97\xC9\xA0\x90")
#define IS_ITASKBARLIST(p) (guid_is(p, 0x56FDF342, 0xFD6D, 0x11D0, "\x95\x8A\x00\x60\x97\xC9\xA0\x90") || \
                            guid_is(p, 0x602D4995, 0xB13A, 0x429B, "\xA6\x6E\x19\x35\xE4\x4F\x43\x17") || \
                            guid_is(p, 0xEA1AFB91, 0x9E28, 0x4B86, "\x90\xE9\x9E\x9F\x8A\x5E\xEF\xAF") || \
                            guid_is(p, 0xC43DC798, 0x95D1, 0x4BEA, "\x90\x30\xBB\x99\xE2\x98\x3A\x1A"))

// ---- vtables ------------------------------------------------------------------------------------------

typedef struct ComMethod { const char *name; HleFn fn; int nargs; } ComMethod; // nargs includes "this"
enum { VT_LINKW, VT_LINKA, VT_PERSISTFILE, VT_MALLOC, VT_TASKBAR, VT_COUNT };
static uint64_t g_vtbl[VT_COUNT]; // per process: thunk addresses live in its address space
SOUL_PROCESS_VAR(g_vtbl)

static uint64_t vtable(int which, const char *iface, const ComMethod *m, int n) {
    if (g_vtbl[which]) return g_vtbl[which];
    uint8_t *t = g_calloc((size_t)n, (size_t)PSZ);
    for (int i = 0; i < n; i++) {
        char name[96];
        snprintf(name, sizeof name, "%s::%s", iface, m[i].name);
        uint32_t id = hle_thunk_for(g_proc, m[i].fn, "ole32", name);
        g_proc->thunks[id].argbytes = g_mode32 ? (uint16_t)(4 * m[i].nargs) : 0;
        wr_ptr(H2G(t) + (uint64_t)i * PSZ, g_proc->thunk_base + (uint64_t)id * SOUL_THUNK_STRIDE);
    }
    g_vtbl[which] = H2G(t);
    return g_vtbl[which];
}

// ---- shell links -----------------------------------------------------------------------------------------

typedef struct Lnk {
    char target[1024], args[1024], workdir[1024], desc[512], icon[1024], file[1200];
    int icon_index, show, hotkey, refs;
} Lnk;
// object: [IShellLinkW vtbl][IPersistFile vtbl][IShellLinkA vtbl][refs:4][pad:4][Lnk*:8]
#define LNK_SLOT_W 0
#define LNK_SLOT_PF 1
#define LNK_SLOT_A 2
static uint64_t lnk_base(uint64_t self, int slot) { return self - (uint64_t)slot * PSZ; }
static Lnk *lnk_of(uint64_t base) { uint64_t h = mem_rd(base + 3 * (uint64_t)PSZ + 8, 8); return (Lnk *)(uintptr_t)h; }

static uint64_t lnk_new(void);
static uint32_t lnk_qi(uint64_t base, uint64_t riid, uint64_t ppv) {
    if (!ppv) return E_POINTER;
    uint64_t r = 0;
    if (IS_IUNKNOWN(riid) || IS_ISHELLLINKW(riid)) r = base;
    else if (IS_ISHELLLINKA(riid)) r = base + 2 * (uint64_t)PSZ;
    else if (IS_IPERSISTFILE(riid) || IS_IPERSIST(riid)) r = base + (uint64_t)PSZ;
    wr_ptr(ppv, r);
    if (!r) return E_NOINTERFACE;
    lnk_of(base)->refs++;
    return S_OK;
}
static uint32_t lnk_release(uint64_t base) {
    Lnk *l = lnk_of(base);
    if (--l->refs > 0) return (uint32_t)l->refs;
    free(l);
    g_free(G2H(base));
    return 0;
}

// .lnk files (MS-SHLLINK): header, LinkInfo with the local path, then the strings in UTF-16.
static void put16(FILE *f, uint16_t v) { fwrite(&v, 2, 1, f); }
static void put32(FILE *f, uint32_t v) { fwrite(&v, 4, 1, f); }
static void put_lnk_string(FILE *f, const char *s) {
    uint16_t *w = u2w_dup(s);
    size_t n = wstrlen(w);
    put16(f, (uint16_t)n);
    fwrite(w, 2, n, f);
    free(w);
}
static int lnk_write(const Lnk *l, const char *host) {
    FILE *f = fopen(host, "wb");
    if (!f) return -1;
    uint32_t flags = 0x02 | 0x80; // HasLinkInfo | IsUnicode
    if (l->desc[0]) flags |= 0x04;
    if (l->workdir[0]) flags |= 0x10;
    if (l->args[0]) flags |= 0x20;
    if (l->icon[0]) flags |= 0x40;
    put32(f, 0x4C);
    static const uint8_t clsid[16] = { 0x01, 0x14, 0x02, 0, 0, 0, 0, 0, 0xC0, 0, 0, 0, 0, 0, 0, 0x46 };
    fwrite(clsid, 1, 16, f);
    put32(f, flags);
    put32(f, 0x20);                                          // FILE_ATTRIBUTE_ARCHIVE
    uint64_t now = (uint64_t)((double)time(NULL) * 10000000.0) + 116444736000000000ull;
    for (int i = 0; i < 3; i++) fwrite(&now, 8, 1, f);
    put32(f, 0);                                             // file size
    put32(f, (uint32_t)l->icon_index);
    put32(f, (uint32_t)(l->show ? l->show : 1));
    put16(f, (uint16_t)l->hotkey);
    put16(f, 0); put32(f, 0); put32(f, 0);
    // LinkInfo
    char ansi[1024];
    size_t al = 0;
    for (const unsigned char *c = (const unsigned char *)l->target; *c && al < sizeof ansi - 1; c++) {
        if ((*c & 0xC0) == 0x80) continue;
        ansi[al++] = *c < 0x80 ? (char)*c : '?';
    }
    ansi[al] = 0;
    uint16_t *wt = u2w_dup(l->target);
    size_t wl = wstrlen(wt);
    const uint32_t hdr = 0x24, vol = 0x11;
    uint32_t base_off = hdr + vol, suffix_off = base_off + (uint32_t)al + 1;
    uint32_t ubase_off = suffix_off + 1, usuffix_off = ubase_off + (uint32_t)(wl + 1) * 2;
    uint32_t size = usuffix_off + 2;
    put32(f, size); put32(f, hdr); put32(f, 1); put32(f, hdr); put32(f, base_off); put32(f, 0); put32(f, suffix_off);
    put32(f, ubase_off); put32(f, usuffix_off);
    put32(f, vol); put32(f, 3); put32(f, 0x50554C53); put32(f, 0x10); fputc(0, f); // VolumeID (fixed disk "SOUL")
    fwrite(ansi, 1, al + 1, f);
    fputc(0, f);
    fwrite(wt, 2, wl + 1, f);
    put16(f, 0);
    free(wt);
    if (flags & 0x04) put_lnk_string(f, l->desc);
    if (flags & 0x10) put_lnk_string(f, l->workdir);
    if (flags & 0x20) put_lnk_string(f, l->args);
    if (flags & 0x40) put_lnk_string(f, l->icon);
    put32(f, 0); // no extra data
    fclose(f);
    return 0;
}

static void lnk_str(const uint8_t *d, size_t len, size_t *o, int uni, char *out, size_t cap) {
    out[0] = 0;
    if (*o + 2 > len) return;
    uint16_t n;
    memcpy(&n, d + *o, 2);
    *o += 2;
    size_t bytes = (size_t)n * (uni ? 2 : 1);
    if (*o + bytes > len) { *o = len; return; }
    if (uni) {
        uint16_t *w = malloc(((size_t)n + 1) * 2);
        memcpy(w, d + *o, bytes);
        w[n] = 0;
        w2u(w, -1, out, cap);
        free(w);
    } else snprintf(out, cap, "%.*s", (int)n, (const char *)d + *o);
    *o += bytes;
}
static void expand_vars(char *s, size_t cap) {
    if (!strchr(s, '%')) return;
    static const char *const vars[][2] = { { "%ProgramFiles%", "C:\\Program Files" }, { "%ProgramFiles(x86)%", "C:\\Program Files (x86)" },
        { "%SystemRoot%", "C:\\windows" }, { "%windir%", "C:\\windows" }, { "%USERPROFILE%", "C:\\users\\soul" },
        { "%APPDATA%", "C:\\users\\soul\\AppData\\Roaming" }, { "%LOCALAPPDATA%", "C:\\users\\soul\\AppData\\Local" },
        { "%ProgramData%", "C:\\ProgramData" }, { "%ALLUSERSPROFILE%", "C:\\ProgramData" }, { "%SystemDrive%", "C:" }, { NULL, NULL } };
    for (int i = 0; vars[i][0]; i++) {
        char *p = strcasestr(s, vars[i][0]);
        if (!p) continue;
        char tmp[1200];
        snprintf(tmp, sizeof tmp, "%.*s%s%s", (int)(p - s), s, vars[i][1], p + strlen(vars[i][0]));
        snprintf(s, cap, "%s", tmp);
    }
}
int soul_lnk_read(const char *host, SoulLnkInfo *out) {
    memset(out, 0, sizeof *out);
    FILE *f = fopen(host, "rb");
    if (!f) return -1;
    uint8_t *d = malloc(1 << 20);
    size_t len = fread(d, 1, 1 << 20, f);
    fclose(f);
    uint32_t hs_, flags;
    if (len < 0x4C) { free(d); return -1; }
    memcpy(&hs_, d, 4);
    memcpy(&flags, d + 0x14, 4);
    if (hs_ != 0x4C) { free(d); return -1; }
    memcpy(&out->icon_index, d + 0x38, 4);
    size_t o = 0x4C;
    if (flags & 0x01) { uint16_t n; memcpy(&n, d + o, 2); o += 2 + n; } // LinkTargetIDList
    if ((flags & 0x02) && o + 0x1C <= len) {
        uint32_t li_size, li_hdr, li_flags, base_off, suffix_off, ubase = 0, usuffix = 0;
        memcpy(&li_size, d + o, 4); memcpy(&li_hdr, d + o + 4, 4); memcpy(&li_flags, d + o + 8, 4);
        memcpy(&base_off, d + o + 16, 4); memcpy(&suffix_off, d + o + 24, 4);
        if (li_hdr >= 0x24) { memcpy(&ubase, d + o + 28, 4); memcpy(&usuffix, d + o + 32, 4); }
        if ((li_flags & 1) && o + li_size <= len) {
            if (ubase) {
                const uint16_t *w = (const uint16_t *)(void *)(d + o + ubase);
                w2u(w, -1, out->target, sizeof out->target);
            } else snprintf(out->target, sizeof out->target, "%s", (const char *)d + o + base_off);
            char suffix[512] = "";
            if (usuffix) w2u((const uint16_t *)(void *)(d + o + usuffix), -1, suffix, sizeof suffix);
            else if (suffix_off) snprintf(suffix, sizeof suffix, "%s", (const char *)d + o + suffix_off);
            if (suffix[0]) strncat(out->target, suffix, sizeof out->target - strlen(out->target) - 1);
        }
        o += li_size;
    }
    int uni = (flags & 0x80) != 0;
    char rel[1024] = "";
    if (flags & 0x04) lnk_str(d, len, &o, uni, out->description, sizeof out->description);
    if (flags & 0x08) lnk_str(d, len, &o, uni, rel, sizeof rel);
    if (flags & 0x10) lnk_str(d, len, &o, uni, out->workdir, sizeof out->workdir);
    if (flags & 0x20) lnk_str(d, len, &o, uni, out->arguments, sizeof out->arguments);
    if (flags & 0x40) lnk_str(d, len, &o, uni, out->icon, sizeof out->icon);
    // extra data: EnvironmentVariableDataBlock holds the target with %variables%
    while (!out->target[0] && o + 8 <= len) {
        uint32_t bsize, sig;
        memcpy(&bsize, d + o, 4);
        if (bsize < 8 || o + bsize > len) break;
        memcpy(&sig, d + o + 4, 4);
        if (sig == 0xA0000001 && bsize >= 0x314) {
            w2u((const uint16_t *)(void *)(d + o + 0x108), -1, out->target, sizeof out->target);
            if (!out->target[0]) snprintf(out->target, sizeof out->target, "%s", (const char *)d + o + 8);
        }
        o += bsize;
    }
    if (!out->target[0] && rel[0]) snprintf(out->target, sizeof out->target, "%s", rel);
    expand_vars(out->target, sizeof out->target);
    expand_vars(out->icon, sizeof out->icon);
    expand_vars(out->workdir, sizeof out->workdir);
    free(d);
    return out->target[0] ? 0 : -1;
}

#define LNK_THIS(slot) uint64_t base = lnk_base(ARG(0), slot); Lnk *l = lnk_of(base)
#define LNK_METHODS(S, sfx, wide) \
HLE(lnk_QueryInterface##sfx) { RET(lnk_qi(lnk_base(ARG(0), S), ARG(1), ARG(2))); } \
HLE(lnk_AddRef##sfx) { LNK_THIS(S); RET((uint32_t)++l->refs); } \
HLE(lnk_Release##sfx) { RET(lnk_release(lnk_base(ARG(0), S))); } \
HLE(lnk_GetPath##sfx) { LNK_THIS(S); (void)base; put_trunc(ARG(1), (size_t)(uint32_t)ARG(2), l->target, wide); RET(l->target[0] ? S_OK : 1); } \
HLE(lnk_GetIDList##sfx) { RET(E_FAIL); } \
HLE(lnk_SetIDList##sfx) { LNK_THIS(S); (void)base; char p[1200]; uint64_t id = ARG(1); \
    if (id && mem_rd(id, 2) >= 7 && !memcmp(G2H(id + 2), "SOUL", 4)) { snprintf(p, sizeof p, "%s", (const char *)G2H(id + 6)); snprintf(l->target, sizeof l->target, "%s", p); } RET(S_OK); } \
HLE(lnk_GetDescription##sfx) { LNK_THIS(S); (void)base; put_trunc(ARG(1), (size_t)(uint32_t)ARG(2), l->desc, wide); RET(S_OK); } \
HLE(lnk_SetDescription##sfx) { LNK_THIS(S); (void)base; char *v = gstr(ARG(1), wide); snprintf(l->desc, sizeof l->desc, "%s", v ? v : ""); free(v); RET(S_OK); } \
HLE(lnk_GetWorkingDirectory##sfx) { LNK_THIS(S); (void)base; put_trunc(ARG(1), (size_t)(uint32_t)ARG(2), l->workdir, wide); RET(S_OK); } \
HLE(lnk_SetWorkingDirectory##sfx) { LNK_THIS(S); (void)base; char *v = gstr(ARG(1), wide); snprintf(l->workdir, sizeof l->workdir, "%s", v ? v : ""); free(v); RET(S_OK); } \
HLE(lnk_GetArguments##sfx) { LNK_THIS(S); (void)base; put_trunc(ARG(1), (size_t)(uint32_t)ARG(2), l->args, wide); RET(S_OK); } \
HLE(lnk_SetArguments##sfx) { LNK_THIS(S); (void)base; char *v = gstr(ARG(1), wide); snprintf(l->args, sizeof l->args, "%s", v ? v : ""); free(v); RET(S_OK); } \
HLE(lnk_GetHotkey##sfx) { LNK_THIS(S); (void)base; if (ARG(1)) mem_wr(ARG(1), 2, (uint32_t)l->hotkey); RET(S_OK); } \
HLE(lnk_SetHotkey##sfx) { LNK_THIS(S); (void)base; l->hotkey = (int)(ARG(1) & 0xFFFF); RET(S_OK); } \
HLE(lnk_GetShowCmd##sfx) { LNK_THIS(S); (void)base; if (ARG(1)) mem_wr(ARG(1), 4, (uint32_t)(l->show ? l->show : 1)); RET(S_OK); } \
HLE(lnk_SetShowCmd##sfx) { LNK_THIS(S); (void)base; l->show = (int)ARG(1); RET(S_OK); } \
HLE(lnk_GetIconLocation##sfx) { LNK_THIS(S); (void)base; put_trunc(ARG(1), (size_t)(uint32_t)ARG(2), l->icon, wide); if (ARG(3)) mem_wr(ARG(3), 4, (uint32_t)l->icon_index); RET(S_OK); } \
HLE(lnk_SetIconLocation##sfx) { LNK_THIS(S); (void)base; char *v = gstr(ARG(1), wide); snprintf(l->icon, sizeof l->icon, "%s", v ? v : ""); free(v); l->icon_index = (int)(int32_t)ARG(2); RET(S_OK); } \
HLE(lnk_SetRelativePath##sfx) { RET(S_OK); } \
HLE(lnk_Resolve##sfx) { RET(S_OK); } \
HLE(lnk_SetPath##sfx) { LNK_THIS(S); (void)base; char *v = gstr(ARG(1), wide); \
    char norm[1200] = ""; if (v && *v) { if (strchr(v, '\\') || strchr(v, ':')) path_normalize_win(v, norm, sizeof norm); else snprintf(norm, sizeof norm, "%s", v); } \
    snprintf(l->target, sizeof l->target, "%s", norm); free(v); RET(S_OK); }
LNK_METHODS(LNK_SLOT_W, W, 1)
LNK_METHODS(LNK_SLOT_A, A, 0)

#define LNK_TABLE(sfx) { \
    { "QueryInterface", lnk_QueryInterface##sfx, 3 }, { "AddRef", lnk_AddRef##sfx, 1 }, { "Release", lnk_Release##sfx, 1 }, \
    { "GetPath", lnk_GetPath##sfx, 5 }, { "GetIDList", lnk_GetIDList##sfx, 2 }, { "SetIDList", lnk_SetIDList##sfx, 2 }, \
    { "GetDescription", lnk_GetDescription##sfx, 3 }, { "SetDescription", lnk_SetDescription##sfx, 2 }, \
    { "GetWorkingDirectory", lnk_GetWorkingDirectory##sfx, 3 }, { "SetWorkingDirectory", lnk_SetWorkingDirectory##sfx, 2 }, \
    { "GetArguments", lnk_GetArguments##sfx, 3 }, { "SetArguments", lnk_SetArguments##sfx, 2 }, { "GetHotkey", lnk_GetHotkey##sfx, 2 }, \
    { "SetHotkey", lnk_SetHotkey##sfx, 2 }, { "GetShowCmd", lnk_GetShowCmd##sfx, 2 }, { "SetShowCmd", lnk_SetShowCmd##sfx, 2 }, \
    { "GetIconLocation", lnk_GetIconLocation##sfx, 4 }, { "SetIconLocation", lnk_SetIconLocation##sfx, 3 }, \
    { "SetRelativePath", lnk_SetRelativePath##sfx, 3 }, { "Resolve", lnk_Resolve##sfx, 3 }, { "SetPath", lnk_SetPath##sfx, 2 } }
static const ComMethod k_linkw[] = LNK_TABLE(W);
static const ComMethod k_linka[] = LNK_TABLE(A);

HLE(pf_QueryInterface) { RET(lnk_qi(lnk_base(ARG(0), LNK_SLOT_PF), ARG(1), ARG(2))); }
HLE(pf_AddRef) { LNK_THIS(LNK_SLOT_PF); (void)base; RET((uint32_t)++l->refs); }
HLE(pf_Release) { RET(lnk_release(lnk_base(ARG(0), LNK_SLOT_PF))); }
HLE(pf_GetClassID) { if (ARG(1)) { static const uint8_t c[16] = { 0x01, 0x14, 0x02, 0, 0, 0, 0, 0, 0xC0, 0, 0, 0, 0, 0, 0, 0x46 }; memcpy(PTR(1), c, 16); } RET(S_OK); }
HLE(pf_IsDirty) { RET(S_OK); }
HLE(pf_Load) {
    LNK_THIS(LNK_SLOT_PF);
    (void)base;
    char *v = gstr(ARG(1), 1);
    char norm[1200], host[2048];
    path_normalize_win(v ? v : "", norm, sizeof norm);
    free(v);
    path_w2h(norm, host, sizeof host);
    SoulLnkInfo li;
    if (soul_lnk_read(host, &li) != 0) { RET(E_FAIL); return; }
    snprintf(l->target, sizeof l->target, "%s", li.target);
    snprintf(l->args, sizeof l->args, "%s", li.arguments);
    snprintf(l->workdir, sizeof l->workdir, "%s", li.workdir);
    snprintf(l->desc, sizeof l->desc, "%s", li.description);
    snprintf(l->icon, sizeof l->icon, "%s", li.icon);
    l->icon_index = li.icon_index;
    snprintf(l->file, sizeof l->file, "%s", norm);
    RET(S_OK);
}
HLE(pf_Save) {
    LNK_THIS(LNK_SLOT_PF);
    (void)base;
    char *v = ARG(1) ? gstr(ARG(1), 1) : strdup(l->file);
    char norm[1200], host[2048];
    path_normalize_win(v, norm, sizeof norm);
    free(v);
    path_w2h(norm, host, sizeof host);
    // create missing folders (installers usually do, but not always)
    char dir[2048];
    snprintf(dir, sizeof dir, "%s", host);
    for (char *c = dir + 1; *c; c++) if (*c == '/') { *c = 0; mkdir(dir, 0755); *c = '/'; }
    if (lnk_write(l, host) != 0) { RET(E_FAIL); return; }
    snprintf(l->file, sizeof l->file, "%s", norm);
    if (g_proc->verbose) soul_log("[Soul] Kısayol: %s -> %s\n", norm, l->target);
    RET(S_OK);
}
HLE(pf_SaveCompleted) { RET(S_OK); }
HLE(pf_GetCurFile) { LNK_THIS(LNK_SLOT_PF); (void)base; if (ARG(1)) wr_ptr(ARG(1), H2G(u2w_gdup(l->file))); RET(S_OK); }
static const ComMethod k_persistfile[] = {
    { "QueryInterface", pf_QueryInterface, 3 }, { "AddRef", pf_AddRef, 1 }, { "Release", pf_Release, 1 },
    { "GetClassID", pf_GetClassID, 2 }, { "IsDirty", pf_IsDirty, 1 }, { "Load", pf_Load, 3 }, { "Save", pf_Save, 3 },
    { "SaveCompleted", pf_SaveCompleted, 2 }, { "GetCurFile", pf_GetCurFile, 2 },
};

static uint64_t lnk_new(void) {
    uint8_t *o = g_calloc(1, 3 * (size_t)PSZ + 16);
    uint64_t base = H2G(o);
    wr_ptr(base, vtable(VT_LINKW, "IShellLinkW", k_linkw, (int)(sizeof k_linkw / sizeof k_linkw[0])));
    wr_ptr(base + (uint64_t)PSZ, vtable(VT_PERSISTFILE, "IPersistFile", k_persistfile, (int)(sizeof k_persistfile / sizeof k_persistfile[0])));
    wr_ptr(base + 2 * (uint64_t)PSZ, vtable(VT_LINKA, "IShellLinkA", k_linka, (int)(sizeof k_linka / sizeof k_linka[0])));
    Lnk *l = calloc(1, sizeof *l);
    l->refs = 0;
    mem_wr(base + 3 * (uint64_t)PSZ + 8, 8, (uint64_t)(uintptr_t)l);
    return base;
}

// ---- IMalloc ------------------------------------------------------------------------------------------------

HLE(mal_QueryInterface) { if (ARG(2)) wr_ptr(ARG(2), IS_IUNKNOWN(ARG(1)) ? ARG(0) : 0); RET(IS_IUNKNOWN(ARG(1)) ? S_OK : E_NOINTERFACE); }
HLE(mal_AddRef) { RET(1); }
HLE(mal_Release) { RET(1); }
HLE(mal_Alloc) { RET(g_malloc(ARG(1) ? ARG(1) : 1)); }
HLE(mal_Realloc) { RET(g_realloc(PTR(1), ARG(2))); }
HLE(mal_Free) { g_free(PTR(1)); RET(0); }
HLE(mal_GetSize) { RET(ARG(1) ? g_msize(PTR(1)) : (uint64_t)-1); }
HLE(mal_DidAlloc) { RET(1); }
HLE(mal_HeapMinimize) { RET(0); }
static const ComMethod k_malloc[] = {
    { "QueryInterface", mal_QueryInterface, 3 }, { "AddRef", mal_AddRef, 1 }, { "Release", mal_Release, 1 }, { "Alloc", mal_Alloc, 2 },
    { "Realloc", mal_Realloc, 3 }, { "Free", mal_Free, 2 }, { "GetSize", mal_GetSize, 2 }, { "DidAlloc", mal_DidAlloc, 2 },
    { "HeapMinimize", mal_HeapMinimize, 1 },
};
static uint64_t g_malloc_obj;
SOUL_PROCESS_VAR(g_malloc_obj)
static uint64_t malloc_object(void) {
    if (!g_malloc_obj) {
        uint8_t *o = g_calloc(1, (size_t)PSZ);
        g_malloc_obj = H2G(o);
        wr_ptr(g_malloc_obj, vtable(VT_MALLOC, "IMalloc", k_malloc, (int)(sizeof k_malloc / sizeof k_malloc[0])));
    }
    return g_malloc_obj;
}
HLE(ole_CoGetMalloc) { if (ARG(1)) wr_ptr(ARG(1), malloc_object()); RET(S_OK); }
HLE(sh_SHGetMalloc) { if (ARG(0)) wr_ptr(ARG(0), malloc_object()); RET(S_OK); }

// ---- ITaskbarList3 (taskbar progress): accepted and ignored -----------------------------------------------

HLE(tb_QueryInterface) { int ok = IS_IUNKNOWN(ARG(1)) || IS_ITASKBARLIST(ARG(1)); if (ARG(2)) wr_ptr(ARG(2), ok ? ARG(0) : 0); RET(ok ? S_OK : E_NOINTERFACE); }
HLE(tb_AddRef) { RET(1); }
HLE(tb_Ok) { RET(S_OK); }
static const ComMethod k_taskbar[] = {
    { "QueryInterface", tb_QueryInterface, 3 }, { "AddRef", tb_AddRef, 1 }, { "Release", tb_AddRef, 1 }, { "HrInit", tb_Ok, 1 },
    { "AddTab", tb_Ok, 2 }, { "DeleteTab", tb_Ok, 2 }, { "ActivateTab", tb_Ok, 2 }, { "SetActiveAlt", tb_Ok, 2 },
    { "MarkFullscreenWindow", tb_Ok, 3 }, { "SetProgressValue", tb_Ok, 6 }, { "SetProgressState", tb_Ok, 3 },
    { "RegisterTab", tb_Ok, 3 }, { "UnregisterTab", tb_Ok, 2 }, { "SetTabOrder", tb_Ok, 3 }, { "SetTabActive", tb_Ok, 4 },
    { "ThumbBarAddButtons", tb_Ok, 4 }, { "ThumbBarUpdateButtons", tb_Ok, 4 }, { "ThumbBarSetImageList", tb_Ok, 3 },
    { "SetOverlayIcon", tb_Ok, 4 }, { "SetThumbnailTooltip", tb_Ok, 3 }, { "SetThumbnailClip", tb_Ok, 3 },
    { "SetTabProperties", tb_Ok, 3 },
};

// ---- CoCreateInstance ------------------------------------------------------------------------------------------

static uint32_t create_instance(uint64_t clsid, uint64_t outer, uint64_t riid, uint64_t ppv) {
    if (!ppv) return E_POINTER;
    wr_ptr(ppv, 0);
    if (IS_SHELLLINK_CLSID(clsid)) {
        if (outer) return CLASS_E_NOAGGREGATION;
        uint64_t o = lnk_new();
        uint32_t r = lnk_qi(o, riid, ppv);
        if (r != S_OK) { free(lnk_of(o)); g_free(G2H(o)); }
        return r;
    }
    if (IS_TASKBARLIST_CLSID(clsid)) {
        uint8_t *o = g_calloc(1, (size_t)PSZ);
        wr_ptr(H2G(o), vtable(VT_TASKBAR, "ITaskbarList3", k_taskbar, (int)(sizeof k_taskbar / sizeof k_taskbar[0])));
        wr_ptr(ppv, H2G(o));
        return S_OK;
    }
    if (g_proc->verbose && clsid) {
        Guid g;
        memcpy(&g, G2H(clsid), 16);
        soul_log("[api] CoCreateInstance: sınıf {%08X-%04X-%04X-%02X%02X-...} yok\n", g.d1, g.d2, g.d3, g.d4[0], g.d4[1]);
    }
    return REGDB_E_CLASSNOTREG;
}
HLE(ole_CoCreateInstance) { RET(create_instance(ARG(0), ARG(1), ARG(3), hle_arg(st, 4))); }
HLE(ole_CoCreateInstanceEx) { // (clsid, outer, ctx, server, count, MULTI_QI[]) — MULTI_QI: pIID, pItf, hr
    uint64_t mq = hle_arg(st, 5);
    uint32_t n = (uint32_t)hle_arg(st, 4);
    const char *L = "PP4";
    uint32_t first = E_NOINTERFACE;
    for (uint32_t i = 0; i < n; i++) {
        uint64_t e = mq + (uint64_t)i * gs_size(L);
        uint64_t slot = e + gs_off(L, 1);
        uint32_t r = create_instance(ARG(0), ARG(1), gs_get(e, L, 0), slot);
        gs_set(e, L, 2, r);
        if (i == 0) first = r;
    }
    RET(first);
}
HLE(ole_CoGetClassObject) { if (hle_arg(st, 4)) wr_ptr(hle_arg(st, 4), 0); RET(REGDB_E_CLASSNOTREG); }

const HleEntry hle_com_table[] = {
    { "CoCreateInstance", ole_CoCreateInstance, NULL }, { "CoCreateInstanceEx", ole_CoCreateInstanceEx, NULL },
    { "CoGetClassObject", ole_CoGetClassObject, NULL }, { "CoGetMalloc", ole_CoGetMalloc, NULL }, { "SHGetMalloc", sh_SHGetMalloc, NULL },
    { NULL, NULL, NULL }
};
