// SoulCore - PE32+ (x86-64 Windows executable / DLL) loader.
#include "hle.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <dirent.h>

#pragma pack(push, 1)
typedef struct { uint16_t Machine, NumberOfSections; uint32_t TimeDateStamp, PointerToSymbolTable, NumberOfSymbols; uint16_t SizeOfOptionalHeader, Characteristics; } FileHeader;
typedef struct { uint32_t VirtualAddress, Size; } DataDir;
typedef struct {
    uint16_t Magic; uint8_t MajorLinker, MinorLinker;
    uint32_t SizeOfCode, SizeOfInitializedData, SizeOfUninitializedData, AddressOfEntryPoint, BaseOfCode;
    uint64_t ImageBase;
    uint32_t SectionAlignment, FileAlignment;
    uint16_t MajorOS, MinorOS, MajorImage, MinorImage, MajorSubsystem, MinorSubsystem;
    uint32_t Win32Version, SizeOfImage, SizeOfHeaders, CheckSum;
    uint16_t Subsystem, DllCharacteristics;
    uint64_t SizeOfStackReserve, SizeOfStackCommit, SizeOfHeapReserve, SizeOfHeapCommit;
    uint32_t LoaderFlags, NumberOfRvaAndSizes;
    DataDir dirs[16];
} OptHeader64;
typedef struct { char Name[8]; uint32_t VirtualSize, VirtualAddress, SizeOfRawData, PointerToRawData, PointerToRelocations, PointerToLinenumbers; uint16_t NumberOfRelocations, NumberOfLinenumbers; uint32_t Characteristics; } SectionHeader;
typedef struct { uint32_t OriginalFirstThunk, TimeDateStamp, ForwarderChain, Name, FirstThunk; } ImportDesc;
typedef struct { uint32_t Characteristics, TimeDateStamp; uint16_t Major, Minor; uint32_t Name, Base, NumberOfFunctions, NumberOfNames, AddressOfFunctions, AddressOfNames, AddressOfNameOrdinals; } ExportDir;
typedef struct { uint64_t StartAddressOfRawData, EndAddressOfRawData, AddressOfIndex, AddressOfCallBacks; uint32_t SizeOfZeroFill, Characteristics; } TlsDir64;
#pragma pack(pop)

enum { DIR_EXPORT = 0, DIR_IMPORT = 1, DIR_RESOURCE = 2, DIR_EXCEPTION = 3, DIR_BASERELOC = 5, DIR_TLS = 9 };

static void lower_copy(char *dst, const char *src, size_t n) {
    size_t i = 0;
    for (; src[i] && i + 1 < n; i++) dst[i] = (char)tolower((unsigned char)src[i]);
    dst[i] = 0;
}

static const char *base_name(const char *p) {
    const char *s = p;
    for (const char *q = p; *q; q++) if (*q == '/' || *q == '\\') s = q + 1;
    return s;
}

static uint8_t *read_file(const char *path, size_t *len) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n <= 0) { fclose(f); return NULL; }
    uint8_t *b = malloc((size_t)n);
    if (b && fread(b, 1, (size_t)n, f) != (size_t)n) { free(b); b = NULL; }
    fclose(f);
    *len = (size_t)n;
    return b;
}

static int parse_headers(const uint8_t *file, size_t len, const FileHeader **fh, const OptHeader64 **oh,
                         const SectionHeader **sh, char *err, size_t errlen) {
    if (len < 0x40 || file[0] != 'M' || file[1] != 'Z') { snprintf(err, errlen, "Geçerli bir Windows programı değil (MZ imzası yok)"); return -1; }
    uint32_t peoff; memcpy(&peoff, file + 0x3C, 4);
    if ((size_t)peoff + 4 + sizeof(FileHeader) > len || memcmp(file + peoff, "PE\0\0", 4) != 0) {
        snprintf(err, errlen, "PE imzası bulunamadı (DOS programları desteklenmiyor)");
        return -1;
    }
    *fh = (const FileHeader *)(file + peoff + 4);
    if ((*fh)->Machine == 0x14C) { snprintf(err, errlen, "32-bit (x86) programlar henüz desteklenmiyor; yalnızca 64-bit (x64) programlar çalışır"); return -1; }
    if ((*fh)->Machine == 0xAA64) { snprintf(err, errlen, "Bu bir ARM64 Windows programı; Soul x64 programları çalıştırır"); return -1; }
    if ((*fh)->Machine != 0x8664) { snprintf(err, errlen, "Desteklenmeyen işlemci mimarisi (0x%04x)", (*fh)->Machine); return -1; }
    *oh = (const OptHeader64 *)((const uint8_t *)*fh + sizeof(FileHeader));
    if ((*oh)->Magic != 0x20B) { snprintf(err, errlen, "PE32+ başlığı bekleniyordu"); return -1; }
    if ((*oh)->dirs[14].Size != 0) { snprintf(err, errlen, ".NET (CLR) programları desteklenmiyor"); return -1; }
    *sh = (const SectionHeader *)((const uint8_t *)*oh + (*fh)->SizeOfOptionalHeader);
    if ((const uint8_t *)(*sh + (*fh)->NumberOfSections) > file + len) { snprintf(err, errlen, "Bozuk bölüm tablosu"); return -1; }
    return 0;
}

static void *map_image(uint64_t preferred, size_t size) {
    void *want = (void *)(uintptr_t)preferred;
#ifdef MAP_FIXED_NOREPLACE
    void *p = mmap(want, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON | MAP_FIXED_NOREPLACE, -1, 0);
    if (p != MAP_FAILED) return p;
#endif
    void *q = mmap(want, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (q == MAP_FAILED) return NULL;
    return q;
}

static int apply_relocs(uint8_t *img, uint64_t delta, const OptHeader64 *oh) {
    const DataDir *d = &oh->dirs[DIR_BASERELOC];
    if (delta == 0) return 0;
    if (!d->VirtualAddress || !d->Size) return -1;
    uint8_t *p = img + d->VirtualAddress, *end = p + d->Size;
    while (p + 8 <= end) {
        uint32_t page, bsize;
        memcpy(&page, p, 4); memcpy(&bsize, p + 4, 4);
        if (bsize < 8) break;
        uint16_t *e = (uint16_t *)(p + 8);
        size_t n = (bsize - 8) / 2;
        for (size_t i = 0; i < n; i++) {
            uint16_t type = e[i] >> 12, off = e[i] & 0xFFF;
            uint8_t *t = img + page + off;
            if (type == 10) { uint64_t v; memcpy(&v, t, 8); v += delta; memcpy(t, &v, 8); }
            else if (type == 3) { uint32_t v; memcpy(&v, t, 4); v += (uint32_t)delta; memcpy(t, &v, 4); }
            else if (type == 0) {}
        }
        p += bsize;
    }
    return 0;
}

PEModule *pe_find_loaded(SoulProcess *p, const char *name) {
    char low[128];
    lower_copy(low, base_name(name), sizeof low);
    size_t ln = strlen(low);
    for (PEModule *m = p->modules; m; m = m->next) {
        if (strcmp(m->name, low) == 0) return m;
        // allow lookups without ".dll"
        size_t mn = strlen(m->name);
        if (mn == ln + 4 && strncmp(m->name, low, ln) == 0 && strcmp(m->name + ln, ".dll") == 0) return m;
    }
    return NULL;
}

uint64_t pe_find_export(PEModule *m, const char *name, int ordinal) {
    if (!m->export_rva) return 0;
    uint8_t *img = (uint8_t *)(uintptr_t)m->base;
    const ExportDir *ed = (const ExportDir *)(img + m->export_rva);
    const uint32_t *funcs = (const uint32_t *)(img + ed->AddressOfFunctions);
    uint32_t idx = UINT32_MAX;
    if (name) {
        const uint32_t *names = (const uint32_t *)(img + ed->AddressOfNames);
        const uint16_t *ords = (const uint16_t *)(img + ed->AddressOfNameOrdinals);
        // binary search, names are sorted
        int lo = 0, hi = (int)ed->NumberOfNames - 1;
        while (lo <= hi) {
            int mid = (lo + hi) / 2;
            int c = strcmp((const char *)(img + names[mid]), name);
            if (c == 0) { idx = ords[mid]; break; }
            if (c < 0) lo = mid + 1; else hi = mid - 1;
        }
        if (idx == UINT32_MAX) {
            for (uint32_t i = 0; i < ed->NumberOfNames; i++)
                if (strcmp((const char *)(img + names[i]), name) == 0) { idx = ords[i]; break; }
        }
    } else {
        idx = (uint32_t)ordinal - ed->Base;
    }
    if (idx == UINT32_MAX || idx >= ed->NumberOfFunctions) return 0;
    uint32_t rva = funcs[idx];
    if (!rva) return 0;
    if (rva >= m->export_rva && rva < m->export_rva + m->export_size) {
        // forwarder "DLL.Function"
        const char *fwd = (const char *)(img + rva);
        char dll[128], fn[256];
        const char *dot = strchr(fwd, '.');
        if (!dot) return 0;
        snprintf(dll, sizeof dll, "%.*s.dll", (int)(dot - fwd), fwd);
        snprintf(fn, sizeof fn, "%s", dot + 1);
        if (fn[0] == '#') return hle_resolve_import(g_proc, dll, NULL, atoi(fn + 1));
        return hle_resolve_import(g_proc, dll, fn, 0);
    }
    return m->base + rva;
}

static int resolve_imports(SoulProcess *p, PEModule *m, const OptHeader64 *oh, char *err, size_t errlen) {
    const DataDir *d = &oh->dirs[DIR_IMPORT];
    if (!d->VirtualAddress) return 0;
    uint8_t *img = (uint8_t *)(uintptr_t)m->base;
    for (const ImportDesc *id = (const ImportDesc *)(img + d->VirtualAddress); id->Name; id++) {
        const char *dll = (const char *)(img + id->Name);
        uint64_t *iat = (uint64_t *)(img + id->FirstThunk);
        const uint64_t *ilt = (const uint64_t *)(img + (id->OriginalFirstThunk ? id->OriginalFirstThunk : id->FirstThunk));
        if (!hle_is_system_dll(dll) && !pe_find_loaded(p, dll)) {
            char e2[256] = {0};
            if (!pe_load_dependency(p, dll, e2, sizeof e2)) {
                if (p->verbose) soul_log("[yükleyici] %s bulunamadı: %s (sahte işlevler kullanılacak)\n", dll, e2);
            }
        }
        for (size_t i = 0; ilt[i]; i++) {
            uint64_t v = ilt[i];
            uint64_t addr;
            if (v & (1ull << 63)) addr = hle_resolve_import(p, dll, NULL, (int)(v & 0xFFFF));
            else addr = hle_resolve_import(p, dll, (const char *)(img + (uint32_t)v + 2), 0);
            if (!addr) { snprintf(err, errlen, "İçe aktarım çözülemedi: %s", dll); return -1; }
            iat[i] = addr;
        }
    }
    return 0;
}

PEModule *pe_load(SoulProcess *p, const char *host_path, const char *win_path, char *err, size_t errlen) {
    size_t len = 0;
    uint8_t *file = read_file(host_path, &len);
    if (!file) { snprintf(err, errlen, "Dosya okunamadı: %s", host_path); return NULL; }
    const FileHeader *fh; const OptHeader64 *oh; const SectionHeader *sh;
    if (parse_headers(file, len, &fh, &oh, &sh, err, errlen) != 0) { free(file); return NULL; }

    size_t size = (oh->SizeOfImage + 0xFFFF) & ~(size_t)0xFFFF;
    uint8_t *img = map_image(oh->ImageBase, size);
    if (!img) { snprintf(err, errlen, "Bellek ayrılamadı (%zu bayt)", size); free(file); return NULL; }
    uint64_t delta = (uint64_t)(uintptr_t)img - oh->ImageBase;
    memcpy(img, file, oh->SizeOfHeaders < len ? oh->SizeOfHeaders : len);
    for (int i = 0; i < fh->NumberOfSections; i++) {
        const SectionHeader *s = &sh[i];
        size_t raw = s->SizeOfRawData;
        if (s->VirtualSize && raw > s->VirtualSize) raw = s->VirtualSize;
        if ((size_t)s->PointerToRawData + raw > len) raw = s->PointerToRawData < len ? len - s->PointerToRawData : 0;
        if (s->VirtualAddress + raw > size) { snprintf(err, errlen, "Bozuk bölüm"); munmap(img, size); free(file); return NULL; }
        if (raw) memcpy(img + s->VirtualAddress, file + s->PointerToRawData, raw);
    }
    // keep a copy of headers we need after freeing the file
    OptHeader64 ohc = *oh;
    uint16_t file_chars = fh->Characteristics;
    free(file);
    if (delta && apply_relocs(img, delta, &ohc) != 0) {
        snprintf(err, errlen, "Program 0x%llx adresine yüklenemedi ve yer değiştirme bilgisi yok",
                 (unsigned long long)ohc.ImageBase);
        munmap(img, size);
        return NULL;
    }
    // Update ImageBase in the in-memory header like the Windows loader does.
    {
        uint32_t peoff; memcpy(&peoff, img + 0x3C, 4);
        OptHeader64 *ohm = (OptHeader64 *)(img + peoff + 4 + sizeof(FileHeader));
        ohm->ImageBase = (uint64_t)(uintptr_t)img;
    }

    PEModule *m = calloc(1, sizeof(PEModule));
    lower_copy(m->name, base_name(host_path), sizeof m->name);
    snprintf(m->host_path, sizeof m->host_path, "%s", host_path);
    snprintf(m->win_path, sizeof m->win_path, "%s", win_path ? win_path : base_name(host_path));
    m->base = (uint64_t)(uintptr_t)img;
    m->size = size;
    m->entry = ohc.AddressOfEntryPoint ? m->base + ohc.AddressOfEntryPoint : 0;
    m->is_dll = (file_chars & 0x2000) != 0;
    m->export_rva = ohc.dirs[DIR_EXPORT].VirtualAddress;
    m->export_size = ohc.dirs[DIR_EXPORT].Size;
    m->tls_rva = ohc.dirs[DIR_TLS].VirtualAddress;
    m->pdata_rva = ohc.dirs[DIR_EXCEPTION].VirtualAddress;
    m->pdata_size = ohc.dirs[DIR_EXCEPTION].Size;
    m->stack_reserve = ohc.SizeOfStackReserve;
    m->subsystem = ohc.Subsystem;
    // Insert before resolving imports so circular imports terminate.
    m->next = p->modules;
    p->modules = m;
    if (p->verbose)
        soul_log("[yükleyici] %s -> 0x%llx (%zu KB)%s\n", m->name, (unsigned long long)m->base, size / 1024,
                 delta ? " (yer değiştirildi)" : "");
    if (resolve_imports(p, m, &ohc, err, errlen) != 0) return NULL;
    return m;
}

static int find_case_insensitive(const char *dir, const char *name, char *out, size_t outlen) {
    char path[1200];
    snprintf(path, sizeof path, "%s/%s", dir, name);
    struct stat sb;
    if (stat(path, &sb) == 0) { snprintf(out, outlen, "%s", path); return 1; }
    // scan directory
    DIR *d = opendir(dir);
    if (!d) return 0;
    struct dirent *e;
    int found = 0;
    while ((e = readdir(d))) {
        if (strcasecmp(e->d_name, name) == 0) { snprintf(out, outlen, "%s/%s", dir, e->d_name); found = 1; break; }
    }
    closedir(d);
    return found;
}

PEModule *pe_load_dependency(SoulProcess *p, const char *dll, char *err, size_t errlen) {
    PEModule *m = pe_find_loaded(p, dll);
    if (m) return m;
    char name[160];
    snprintf(name, sizeof name, "%s", base_name(dll));
    if (!strchr(name, '.')) strncat(name, ".dll", sizeof name - strlen(name) - 1);
    char dir[1024];
    snprintf(dir, sizeof dir, "%s", p->exe_host);
    char *slash = strrchr(dir, '/');
    if (slash) *slash = 0;
    char found[1200];
    char sysdir[1100];
    snprintf(sysdir, sizeof sysdir, "%s/windows/system32", p->drive_c);
    if (!find_case_insensitive(dir, name, found, sizeof found) &&
        !find_case_insensitive(sysdir, name, found, sizeof found)) {
        snprintf(err, errlen, "%s bulunamadı", name);
        return NULL;
    }
    char winp[600];
    snprintf(winp, sizeof winp, "%s", p->exe_win);
    char *bs = strrchr(winp, '\\');
    if (bs) snprintf(bs + 1, sizeof winp - (size_t)(bs + 1 - winp), "%s", name);
    return pe_load(p, found, winp, err, errlen);
}

// Static TLS: every module with a TLS directory gets an index; each guest thread
// gets its own copy of the module's TLS template in TEB->ThreadLocalStoragePointer.
static uint8_t *tls_block(const TlsTemplate *t) {
    uint8_t *blk = calloc(1, t->raw + t->zero + 16);
    if (t->raw) memcpy(blk, (void *)(uintptr_t)t->start, t->raw);
    return blk;
}

static void init_tls(SoulProcess *p, X86State *st, PEModule *m, int index) {
    if (!m->tls_rva || p->tls_count >= SOUL_MAX_TLS_MODULES) return;
    TlsDir64 *td = (TlsDir64 *)(uintptr_t)(m->base + m->tls_rva);
    TlsTemplate *t = &p->tls[p->tls_count++];
    t->start = td->StartAddressOfRawData;
    t->raw = (size_t)(td->EndAddressOfRawData - td->StartAddressOfRawData);
    t->zero = td->SizeOfZeroFill;
    t->index = index;
    t->callbacks = td->AddressOfCallBacks;
    t->module_base = m->base;
    uint64_t *slots = (uint64_t *)(uintptr_t)mem_rd(st->gs_base + 0x58, 8);
    slots[index] = (uint64_t)(uintptr_t)tls_block(t);
    if (td->AddressOfIndex) mem_wr(td->AddressOfIndex, 4, (uint64_t)index);
    if (td->AddressOfCallBacks) {
        for (uint64_t *cb = (uint64_t *)(uintptr_t)td->AddressOfCallBacks; *cb; cb++) {
            uint64_t args[3] = { m->base, 1 /* DLL_PROCESS_ATTACH */, 0 };
            soul_guest_call(st, *cb, 3, args);
        }
    }
}

void pe_init_thread_tls(SoulProcess *p, X86State *st, uint64_t teb, int attach) {
    uint64_t *slots = (uint64_t *)(uintptr_t)mem_rd(teb + 0x58, 8);
    for (int i = 0; i < p->tls_count; i++) slots[p->tls[i].index] = (uint64_t)(uintptr_t)tls_block(&p->tls[i]);
    if (!attach) return;
    for (int i = 0; i < p->tls_count; i++) {
        if (!p->tls[i].callbacks) continue;
        for (uint64_t *cb = (uint64_t *)(uintptr_t)p->tls[i].callbacks; *cb; cb++) {
            uint64_t args[3] = { p->tls[i].module_base, 2 /* DLL_THREAD_ATTACH */, 0 };
            soul_guest_call(st, *cb, 3, args);
        }
    }
    for (PEModule *m = p->modules; m; m = m->next) {
        if (m->is_dll && m->entry && m->initialized) {
            uint64_t args[3] = { m->base, 2, 0 };
            soul_guest_call(st, m->entry, 3, args);
        }
    }
}

// Run TLS callbacks and DllMain for DLLs (dependencies first), then TLS for the exe.
int pe_run_initializers(SoulProcess *p, X86State *st) {
    PEModule *list[256];
    int n = 0;
    for (PEModule *m = p->modules; m && n < 256; m = m->next) list[n++] = m;
    for (int i = 0; i < n; i++) { // deepest dependencies were inserted last => at head
        PEModule *m = list[i];
        if (m == p->main_module || m->initialized) continue;
        m->initialized = 1;
        int index = 1;
        for (int k = 0; k < p->tls_count; k++) if (p->tls[k].index >= index) index = p->tls[k].index + 1;
        init_tls(p, st, m, index);
        if (m->is_dll && m->entry) {
            if (p->verbose) soul_log("[yükleyici] DllMain: %s\n", m->name);
            uint64_t args[3] = { m->base, 1, 0 };
            uint64_t ok = soul_guest_call(st, m->entry, 3, args);
            if (!(ok & 0xFFFFFFFF)) soul_log("[yükleyici] Uyarı: %s DllMain başarısız oldu\n", m->name);
        }
    }
    if (!p->main_module->initialized) {
        p->main_module->initialized = 1;
        init_tls(p, st, p->main_module, 0);
    }
    return 0;
}

void pe_unload_all(SoulProcess *p) {
    PEModule *m = p->modules;
    while (m) {
        PEModule *n = m->next;
        munmap((void *)(uintptr_t)m->base, m->size);
        free(m);
        m = n;
    }
    p->modules = NULL;
}

// ---- inspection & icons ------------------------------------------------

int soul_pe_inspect(const char *path, SoulPEInfo *info) {
    memset(info, 0, sizeof *info);
    size_t len;
    uint8_t *file = read_file(path, &len);
    if (!file) return -1;
    const FileHeader *fh = NULL; const OptHeader64 *oh = NULL; const SectionHeader *sh = NULL;
    char err[256];
    if (len >= 0x40 && file[0] == 'M' && file[1] == 'Z') {
        uint32_t peoff; memcpy(&peoff, file + 0x3C, 4);
        if ((size_t)peoff + 24 < len && memcmp(file + peoff, "PE\0\0", 4) == 0) {
            fh = (const FileHeader *)(file + peoff + 4);
            info->machine = fh->Machine;
            info->is_dll = (fh->Characteristics & 0x2000) != 0;
        }
    }
    if (fh && parse_headers(file, len, &fh, &oh, &sh, err, sizeof err) == 0) {
        info->valid = 1;
        info->is_64bit = 1;
        info->is_gui = oh->Subsystem == 2;
        info->image_base = oh->ImageBase;
        info->size_of_image = oh->SizeOfImage;
        // imports: map RVA -> file offset
        uint32_t irva = oh->dirs[DIR_IMPORT].VirtualAddress;
        size_t pos = 0;
        for (const ImportDesc *id = NULL;;) {
            // resolve rva to file
            #define RVA2OFF(rva) ({ uint32_t _r = (rva); size_t _o = 0; int _ok = 0; \
                for (int _s = 0; _s < fh->NumberOfSections; _s++) \
                    if (_r >= sh[_s].VirtualAddress && _r < sh[_s].VirtualAddress + (sh[_s].SizeOfRawData > sh[_s].VirtualSize ? sh[_s].SizeOfRawData : sh[_s].VirtualSize)) \
                        { _o = sh[_s].PointerToRawData + (_r - sh[_s].VirtualAddress); _ok = 1; break; } \
                _ok && _o < len ? _o : 0; })
            if (!irva) break;
            size_t off = RVA2OFF(irva);
            if (!off) break;
            for (id = (const ImportDesc *)(file + off); (const uint8_t *)(id + 1) <= file + len && id->Name; id++) {
                size_t no = RVA2OFF(id->Name);
                if (!no) break;
                const char *nm = (const char *)(file + no);
                size_t l = strnlen(nm, 64);
                if (pos + l + 2 < sizeof info->imports) {
                    if (pos) info->imports[pos++] = ',';
                    memcpy(info->imports + pos, nm, l);
                    pos += l;
                    info->imports[pos] = 0;
                }
            }
            break;
        }
    } else if (fh) {
        info->valid = 0;
        info->is_64bit = fh->Machine == 0x8664;
    }
    free(file);
    return info->valid ? 0 : -1;
}

void soul_free(void *ptr) { free(ptr); }

// Resource directory helpers (operate on a mapped image laid out by RVA).
typedef struct { uint32_t Characteristics, TimeDateStamp; uint16_t Major, Minor, NumberOfNamedEntries, NumberOfIdEntries; } ResDir;
typedef struct { uint32_t Name, OffsetToData; } ResEntry;
typedef struct { uint32_t OffsetToData, Size, CodePage, Reserved; } ResData;

static const ResEntry *res_find(const uint8_t *rsrc, const ResDir *dir, int id) {
    const ResEntry *e = (const ResEntry *)(dir + 1);
    int n = dir->NumberOfNamedEntries + dir->NumberOfIdEntries;
    for (int i = 0; i < n; i++) {
        if (id < 0) return &e[i]; // first entry
        if (!(e[i].Name & 0x80000000u) && (int)(e[i].Name & 0xFFFF) == id) return &e[i];
    }
    (void)rsrc;
    return NULL;
}

static const ResData *res_leaf(const uint8_t *rsrc, int type, int name) {
    const ResDir *root = (const ResDir *)rsrc;
    const ResEntry *t = res_find(rsrc, root, type);
    if (!t || !(t->OffsetToData & 0x80000000u)) return NULL;
    const ResDir *d2 = (const ResDir *)(rsrc + (t->OffsetToData & 0x7FFFFFFF));
    const ResEntry *n = res_find(rsrc, d2, name);
    if (!n) return NULL;
    if (!(n->OffsetToData & 0x80000000u)) return (const ResData *)(rsrc + n->OffsetToData);
    const ResDir *d3 = (const ResDir *)(rsrc + (n->OffsetToData & 0x7FFFFFFF));
    const ResEntry *l = res_find(rsrc, d3, -1);
    if (!l || (l->OffsetToData & 0x80000000u)) return NULL;
    return (const ResData *)(rsrc + l->OffsetToData);
}

int soul_pe_extract_icon(const char *path, unsigned char **out, size_t *outlen) {
    *out = NULL; *outlen = 0;
    size_t len;
    uint8_t *file = read_file(path, &len);
    if (!file) return -1;
    const FileHeader *fh; const OptHeader64 *oh; const SectionHeader *sh;
    char err[128];
    int rc = -1;
    uint8_t *img = NULL;
    if (parse_headers(file, len, &fh, &oh, &sh, err, sizeof err) != 0) goto done;
    if (!oh->dirs[DIR_RESOURCE].VirtualAddress || oh->SizeOfImage > 512u * 1024 * 1024) goto done;
    img = calloc(1, oh->SizeOfImage);
    if (!img) goto done;
    for (int i = 0; i < fh->NumberOfSections; i++) {
        size_t raw = sh[i].SizeOfRawData;
        if ((size_t)sh[i].PointerToRawData + raw > len) raw = sh[i].PointerToRawData < len ? len - sh[i].PointerToRawData : 0;
        if (sh[i].VirtualAddress + raw > oh->SizeOfImage) raw = oh->SizeOfImage > sh[i].VirtualAddress ? oh->SizeOfImage - sh[i].VirtualAddress : 0;
        memcpy(img + sh[i].VirtualAddress, file + sh[i].PointerToRawData, raw);
    }
    {
        const uint8_t *rsrc = img + oh->dirs[DIR_RESOURCE].VirtualAddress;
        const ResData *grp = res_leaf(rsrc, 14 /* RT_GROUP_ICON */, -1);
        if (!grp) {
            // the first group may be named; res_find(-1) handles that
            goto done;
        }
        const uint8_t *g = img + grp->OffsetToData;
        uint16_t count; memcpy(&count, g + 4, 2);
        int best = -1, bestScore = -1;
        for (int i = 0; i < count; i++) {
            const uint8_t *e = g + 6 + 14 * i;
            int w = e[0] ? e[0] : 256;
            uint16_t bpp; memcpy(&bpp, e + 6, 2);
            int score = w * 64 + bpp;
            if (score > bestScore) { bestScore = score; best = i; }
        }
        if (best < 0) goto done;
        const uint8_t *e = g + 6 + 14 * best;
        uint16_t id; memcpy(&id, e + 12, 2);
        const ResData *ico = res_leaf(rsrc, 3 /* RT_ICON */, id);
        if (!ico || ico->OffsetToData + ico->Size > oh->SizeOfImage) goto done;
        const uint8_t *data = img + ico->OffsetToData;
        if (ico->Size > 8 && memcmp(data, "\x89PNG", 4) == 0) {
            *out = malloc(ico->Size);
            memcpy(*out, data, ico->Size);
            *outlen = ico->Size;
        } else {
            size_t total = 6 + 16 + ico->Size;
            uint8_t *o = malloc(total);
            uint16_t hdr[3] = { 0, 1, 1 };
            memcpy(o, hdr, 6);
            memcpy(o + 6, e, 12);            // width,height,colors,reserved,planes,bpp,size
            uint32_t sz = ico->Size, off = 22;
            memcpy(o + 6 + 8, &sz, 4);
            memcpy(o + 6 + 12, &off, 4);
            memcpy(o + 22, data, ico->Size);
            *out = o;
            *outlen = total;
        }
        rc = 0;
    }
done:
    free(img);
    free(file);
    return rc;
}

// ---- resources of loaded modules -------------------------------------------------------------

static int res_name_eq(const uint8_t *rsrc, uint32_t name_field, uint64_t want, int wide) {
    if (want < 0x10000) return !(name_field & 0x80000000u) && (name_field & 0xFFFF) == want;
    if (!(name_field & 0x80000000u)) return 0;
    const uint8_t *ds = rsrc + (name_field & 0x7FFFFFFF);
    uint16_t len; memcpy(&len, ds, 2);
    const uint16_t *a = (const uint16_t *)(const void *)(ds + 2);
    char tmp[256];
    if (wide) w2u((const uint16_t *)(uintptr_t)want, -1, tmp, sizeof tmp);
    else snprintf(tmp, sizeof tmp, "%s", (const char *)(uintptr_t)want);
    size_t n = strlen(tmp);
    if (n != len) return 0;
    for (size_t i = 0; i < n; i++) {
        uint16_t c = a[i];
        if (c >= 'a' && c <= 'z') c = (uint16_t)(c - 32);
        int d = (unsigned char)tmp[i];
        if (d >= 'a' && d <= 'z') d -= 32;
        if (c != d) return 0;
    }
    return 1;
}

static uint64_t res_key(uint64_t v, int wide) {
    if (v < 0x10000) return v;
    // "#123" means the integer id 123
    if (wide) {
        const uint16_t *w = (const uint16_t *)(uintptr_t)v;
        if (w[0] == '#') { uint64_t n = 0; for (int i = 1; w[i] >= '0' && w[i] <= '9'; i++) n = n * 10 + (w[i] - '0'); return n; }
    } else {
        const char *s = (const char *)(uintptr_t)v;
        if (s[0] == '#') return (uint64_t)strtoul(s + 1, NULL, 10);
    }
    return v;
}

static const ResEntry *res_lookup(const uint8_t *rsrc, const ResDir *dir, uint64_t want, int wide) {
    const ResEntry *e = (const ResEntry *)(dir + 1);
    int n = dir->NumberOfNamedEntries + dir->NumberOfIdEntries;
    for (int i = 0; i < n; i++)
        if (res_name_eq(rsrc, e[i].Name, want, wide)) return &e[i];
    return NULL;
}

const void *pe_resource(uint64_t module_base, uint64_t type, uint64_t name, int wide, uint32_t *size) {
    SoulProcess *p = g_proc;
    if (!module_base && p->main_module) module_base = p->main_module->base;
    PEModule *m = NULL;
    for (PEModule *it = p->modules; it; it = it->next) if (it->base == module_base) m = it;
    if (!m) return NULL;
    const uint8_t *img = (const uint8_t *)(uintptr_t)m->base;
    uint32_t lfanew; memcpy(&lfanew, img + 0x3C, 4);
    const OptHeader64 *oh = (const OptHeader64 *)(const void *)(img + lfanew + 24);
    uint32_t rva = oh->dirs[DIR_RESOURCE].VirtualAddress;
    if (!rva) return NULL;
    const uint8_t *rsrc = img + rva;
    type = res_key(type, wide);
    name = res_key(name, wide);
    const ResEntry *t = res_lookup(rsrc, (const ResDir *)rsrc, type, wide);
    if (!t || !(t->OffsetToData & 0x80000000u)) return NULL;
    const ResDir *d2 = (const ResDir *)(rsrc + (t->OffsetToData & 0x7FFFFFFF));
    const ResEntry *n = res_lookup(rsrc, d2, name, wide);
    if (!n) return NULL;
    const ResData *leaf;
    if (!(n->OffsetToData & 0x80000000u)) leaf = (const ResData *)(rsrc + n->OffsetToData);
    else {
        const ResDir *d3 = (const ResDir *)(rsrc + (n->OffsetToData & 0x7FFFFFFF));
        const ResEntry *l = res_find(rsrc, d3, -1);
        if (!l || (l->OffsetToData & 0x80000000u)) return NULL;
        leaf = (const ResData *)(rsrc + l->OffsetToData);
    }
    if (size) *size = leaf->Size;
    return img + leaf->OffsetToData;
}
