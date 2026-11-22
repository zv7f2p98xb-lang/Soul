// SoulCore - oleaut32: BSTR strings (Delphi's WideString lives on them) and VARIANT basics.
#include "hle.h"
#include <math.h>
#include <strings.h>

// A BSTR points at UTF-16 text preceded by its length in bytes (4 bytes) and followed by a NUL.
static uint64_t bstr_alloc(const void *src, uint32_t bytes) {
    uint8_t *b = g_malloc((size_t)bytes + 6);
    if (!b) return 0;
    memcpy(b, &bytes, 4);
    if (src) memcpy(b + 4, src, bytes); else memset(b + 4, 0, bytes);
    b[4 + bytes] = 0; b[5 + bytes] = 0;
    return H2G(b + 4);
}
static void bstr_free(uint64_t s) { if (s) g_free((uint8_t *)G2H(s) - 4); }
static uint32_t bstr_bytes(uint64_t s) { return s ? (uint32_t)mem_rd(s - 4, 4) : 0; }

HLE(oa_SysAllocString) { RET(ARG(0) ? bstr_alloc(PTR(0), (uint32_t)wstrlen(PTR(0)) * 2) : 0); }
HLE(oa_SysAllocStringLen) { RET(bstr_alloc(ARG(0) ? PTR(0) : NULL, (uint32_t)ARG(1) * 2)); }
HLE(oa_SysAllocStringByteLen) { RET(bstr_alloc(ARG(0) ? PTR(0) : NULL, (uint32_t)ARG(1))); }
HLE(oa_SysFreeString) { bstr_free(ARG(0)); RET(0); }
HLE(oa_SysStringLen) { RET(bstr_bytes(ARG(0)) / 2); }
HLE(oa_SysStringByteLen) { RET(bstr_bytes(ARG(0))); }
static void realloc_bstr(X86State *st, const void *src, uint32_t bytes) {
    uint64_t old = rd_ptr(ARG(0));
    // the source may be the old string itself: copy before freeing
    void *tmp = NULL;
    if (src) { tmp = malloc(bytes ? bytes : 1); memcpy(tmp, src, bytes); }
    uint64_t n = bstr_alloc(tmp, bytes);
    free(tmp);
    if (!n) { RET(0); return; }
    bstr_free(old);
    wr_ptr(ARG(0), n);
    RET(1);
}
HLE(oa_SysReAllocString) { realloc_bstr(st, ARG(1) ? PTR(1) : NULL, ARG(1) ? (uint32_t)wstrlen(PTR(1)) * 2 : 0); }
HLE(oa_SysReAllocStringLen) { realloc_bstr(st, ARG(1) ? PTR(1) : NULL, (uint32_t)ARG(2) * 2); }

// ---- VARIANT -----------------------------------------------------------------------------------------
// { VARTYPE vt; 3 reserved WORDs; value at +8 } — 16 bytes in 32-bit programs, 24 in 64-bit ones.
enum { VT_EMPTY = 0, VT_NULL = 1, VT_I2 = 2, VT_I4 = 3, VT_R4 = 4, VT_R8 = 5, VT_CY = 6, VT_DATE = 7, VT_BSTR = 8, VT_DISPATCH = 9,
       VT_ERROR = 10, VT_BOOL = 11, VT_VARIANT = 12, VT_UNKNOWN = 13, VT_I1 = 16, VT_UI1 = 17, VT_UI2 = 18, VT_UI4 = 19,
       VT_I8 = 20, VT_UI8 = 21, VT_INT = 22, VT_UINT = 23 };
#define VAR_SIZE ((uint64_t)(g_mode32 ? 16 : 24))
#define DISP_E_TYPEMISMATCH 0x80020005u
#define DISP_E_BADVARTYPE 0x80020008u

static void com_call(X86State *st, uint64_t itf, int slot) {
    if (!itf) return;
    uint64_t vt = rd_ptr(itf);
    uint64_t fn = rd_ptr(vt + (uint64_t)slot * PSZ);
    uint64_t args[1] = { itf };
    soul_guest_call(st, fn, 1, args);
}
static void var_clear(X86State *st, uint64_t v) {
    uint16_t vt = (uint16_t)mem_rd(v, 2);
    if (vt == VT_BSTR) bstr_free(rd_ptr(v + 8));
    else if (vt == VT_UNKNOWN || vt == VT_DISPATCH) com_call(st, rd_ptr(v + 8), 2); // Release
    memset(G2H(v), 0, (size_t)VAR_SIZE);
}
HLE(oa_VariantInit) { memset(PTR(0), 0, (size_t)VAR_SIZE); RET(0); }
HLE(oa_VariantClear) { if (ARG(0)) var_clear(st, ARG(0)); RET(0); }
static uint32_t var_copy(X86State *st, uint64_t dst, uint64_t src) {
    if (dst == src) return 0;
    uint8_t tmp[24];
    memcpy(tmp, G2H(src), (size_t)VAR_SIZE);
    var_clear(st, dst);
    memcpy(G2H(dst), tmp, (size_t)VAR_SIZE);
    uint16_t vt = (uint16_t)mem_rd(dst, 2);
    if (vt == VT_BSTR) { uint64_t s = rd_ptr(dst + 8); wr_ptr(dst + 8, s ? bstr_alloc(G2H(s), bstr_bytes(s)) : 0); }
    else if (vt == VT_UNKNOWN || vt == VT_DISPATCH) com_call(st, rd_ptr(dst + 8), 1); // AddRef
    return 0;
}
HLE(oa_VariantCopy) { RET(var_copy(st, ARG(0), ARG(1))); }

static int var_number(uint64_t v, double *out) {
    uint16_t vt = (uint16_t)mem_rd(v, 2);
    uint64_t p = v + 8;
    switch (vt) {
    case VT_EMPTY: *out = 0; return 1;
    case VT_I1: *out = (int8_t)mem_rd(p, 1); return 1;
    case VT_UI1: *out = (uint8_t)mem_rd(p, 1); return 1;
    case VT_I2: *out = (int16_t)mem_rd(p, 2); return 1;
    case VT_UI2: *out = (uint16_t)mem_rd(p, 2); return 1;
    case VT_BOOL: *out = (int16_t)mem_rd(p, 2) ? -1 : 0; return 1;
    case VT_I4: case VT_INT: case VT_ERROR: *out = (int32_t)mem_rd(p, 4); return 1;
    case VT_UI4: case VT_UINT: *out = (uint32_t)mem_rd(p, 4); return 1;
    case VT_I8: *out = (double)(int64_t)mem_rd(p, 8); return 1;
    case VT_UI8: *out = (double)mem_rd(p, 8); return 1;
    case VT_R4: { uint32_t b = (uint32_t)mem_rd(p, 4); float f; memcpy(&f, &b, 4); *out = f; return 1; }
    case VT_R8: case VT_DATE: { uint64_t b = mem_rd(p, 8); memcpy(out, &b, 8); return 1; }
    case VT_CY: *out = (double)(int64_t)mem_rd(p, 8) / 10000.0; return 1;
    case VT_BSTR: {
        uint64_t s = rd_ptr(p);
        char *u = s ? w2u_dup(G2H(s)) : strdup("");
        char *end;
        double d = strtod(u, &end);
        int ok = end != u;
        if (!ok && !strcasecmp(u, "true")) { d = -1; ok = 1; }
        if (!ok && !strcasecmp(u, "false")) { d = 0; ok = 1; }
        free(u);
        *out = d;
        return ok;
    }
    default: return 0;
    }
}
static uint32_t change_type(X86State *st, uint64_t dst, uint64_t src, uint16_t to) {
    uint8_t res[24];
    memset(res, 0, sizeof res);
    uint16_t from = (uint16_t)mem_rd(src, 2);
    if (from == to) return var_copy(st, dst, src);
    double d;
    if (to == VT_BSTR) {
        char buf[64];
        if (from == VT_BOOL) snprintf(buf, sizeof buf, "%s", (int16_t)mem_rd(src + 8, 2) ? "True" : "False");
        else if (!var_number(src, &d)) return DISP_E_TYPEMISMATCH;
        else if (d == floor(d) && fabs(d) < 1e15) snprintf(buf, sizeof buf, "%.0f", d);
        else snprintf(buf, sizeof buf, "%.15g", d);
        uint16_t *w = u2w_dup(buf);
        uint64_t b = bstr_alloc(w, (uint32_t)wstrlen(w) * 2);
        free(w);
        var_clear(st, dst);
        mem_wr(dst, 2, VT_BSTR);
        wr_ptr(dst + 8, b);
        return 0;
    }
    if (!var_number(src, &d)) return DISP_E_TYPEMISMATCH;
    uint64_t bits = 0;
    switch (to) {
    case VT_I1: case VT_UI1: bits = (uint8_t)(int64_t)llround(d); break;
    case VT_I2: case VT_UI2: bits = (uint16_t)(int64_t)llround(d); break;
    case VT_BOOL: bits = d != 0 ? 0xFFFF : 0; break;
    case VT_I4: case VT_UI4: case VT_INT: case VT_UINT: case VT_ERROR: bits = (uint32_t)(int64_t)llround(d); break;
    case VT_I8: case VT_UI8: bits = (uint64_t)llround(d); break;
    case VT_R4: { float f = (float)d; uint32_t b; memcpy(&b, &f, 4); bits = b; break; }
    case VT_R8: case VT_DATE: memcpy(&bits, &d, 8); break;
    case VT_CY: bits = (uint64_t)llround(d * 10000.0); break;
    default: return DISP_E_BADVARTYPE;
    }
    var_clear(st, dst);
    mem_wr(dst, 2, to);
    mem_wr(dst + 8, 8, bits);
    return 0;
}
HLE(oa_VariantChangeType) { RET(change_type(st, ARG(0), ARG(1), (uint16_t)hle_arg(st, 3))); }
HLE(oa_VariantChangeTypeEx) { RET(change_type(st, ARG(0), ARG(1), (uint16_t)hle_arg(st, 4))); }

const HleEntry hle_oleaut_table[] = {
    { "SysAllocString", oa_SysAllocString, NULL }, { "SysAllocStringLen", oa_SysAllocStringLen, NULL },
    { "SysAllocStringByteLen", oa_SysAllocStringByteLen, NULL }, { "SysFreeString", oa_SysFreeString, NULL },
    { "SysStringLen", oa_SysStringLen, NULL }, { "SysStringByteLen", oa_SysStringByteLen, NULL },
    { "SysReAllocString", oa_SysReAllocString, NULL }, { "SysReAllocStringLen", oa_SysReAllocStringLen, NULL },
    { "VariantInit", oa_VariantInit, NULL }, { "VariantClear", oa_VariantClear, NULL }, { "VariantCopy", oa_VariantCopy, NULL },
    { "VariantChangeType", oa_VariantChangeType, NULL }, { "VariantChangeTypeEx", oa_VariantChangeTypeEx, NULL },
    { NULL, NULL, NULL }
};
