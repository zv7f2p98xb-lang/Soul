// SoulCore - Direct3D 9 surface formats: sizes, conversion to and from 0xAARRGGBB, DXT decoding.
// Bump formats (V8U8, Q8W8V8U8, ...) keep their signed components as raw bytes (U -> R, V -> G, W -> B, Q -> A).
#include "d3d9_priv.h"

int d9_fmt_is_dxt(int f) { return f == F_DXT1 || f == F_DXT2 || f == F_DXT3 || f == F_DXT4 || f == F_DXT5; }
int d9_fmt_is_depth(int f) {
    switch (f) {
    case F_D16_LOCKABLE: case F_D32: case F_D15S1: case F_D24S8: case F_D24X8: case F_D24X4S4: case F_D16: case F_D32F_LOCKABLE:
    case F_D24FS8: case F_D32_LOCKABLE: case F_S8_LOCKABLE: return 1;
    }
    return 0;
}
int d9_fmt_is_argb32(int f) { return f == F_A8R8G8B8 || f == F_X8R8G8B8; }
int d9_fmt_is_bump(int f) { return f == F_V8U8 || f == F_Q8W8V8U8 || f == F_V16U16 || f == F_L6V5U5 || f == F_X8L8V8U8 || f == F_A2W10V10U10 || f == F_Q16W16V16U16; }

int d9_fmt_bytes(int f) {
    switch (f) {
    case F_R8G8B8: return 3;
    case F_A8R8G8B8: case F_X8R8G8B8: case F_A2B10G10R10: case F_A8B8G8R8: case F_X8B8G8R8: case F_G16R16: case F_A2R10G10B10:
    case F_Q8W8V8U8: case F_V16U16: case F_A2W10V10U10: case F_X8L8V8U8: case F_G16R16F: case F_R32F: return 4;
    case F_R5G6B5: case F_X1R5G5B5: case F_A1R5G5B5: case F_A4R4G4B4: case F_A8R3G3B2: case F_X4R4G4B4: case F_A8P8: case F_A8L8:
    case F_V8U8: case F_L6V5U5: case F_L16: case F_R16F: return 2;
    case F_R3G3B2: case F_A8: case F_P8: case F_L8: case F_A4L4: return 1;
    case F_A16B16G16R16: case F_Q16W16V16U16: case F_A16B16G16R16F: case F_G32R32F: return 8;
    case F_A32B32G32R32F: return 16;
    }
    return 0;
}
int d9_fmt_known(int f) { return d9_fmt_bytes(f) > 0 || d9_fmt_is_dxt(f); }
int d9_fmt_texture_ok(int f) { return d9_fmt_known(f); }
int d9_fmt_rt_ok(int f) {
    switch (f) {
    case F_A8R8G8B8: case F_X8R8G8B8: case F_R5G6B5: case F_X1R5G5B5: case F_A1R5G5B5: case F_A4R4G4B4: case F_A8: case F_A2R10G10B10:
    case F_A2B10G10R10: case F_A8B8G8R8: case F_X8B8G8R8: case F_G16R16: case F_A16B16G16R16: case F_R16F: case F_G16R16F:
    case F_A16B16G16R16F: case F_R32F: case F_G32R32F: case F_A32B32G32R32F: case F_L8: case F_A8L8: return 1;
    }
    return 0;
}
size_t d9_level_pitch(int f, int w) {
    if (d9_fmt_is_dxt(f)) return (size_t)((w + 3) / 4) * (f == F_DXT1 ? 8 : 16);
    return (size_t)w * (size_t)d9_fmt_bytes(f);
}
size_t d9_level_rows(int f, int h) { return d9_fmt_is_dxt(f) ? (size_t)((h + 3) / 4) : (size_t)h; }

float d9_half_to_float(uint16_t h) {
    int s = (h >> 15) & 1, e = (h >> 10) & 31, m = h & 1023;
    float v;
    if (e == 0) v = ldexpf((float)m, -24);
    else if (e == 31) v = m ? NAN : INFINITY;
    else v = ldexpf((float)(m | 1024), e - 25);
    return s ? -v : v;
}
uint16_t d9_float_to_half(float f) {
    uint32_t u = f2u(f);
    uint32_t s = (u >> 16) & 0x8000, e = (u >> 23) & 0xFF, m = u & 0x7FFFFF;
    if (e == 255) return (uint16_t)(s | 0x7C00 | (m ? 0x200 : 0));
    int ne = (int)e - 127 + 15;
    if (ne >= 31) return (uint16_t)(s | 0x7C00);
    if (ne <= 0) {
        if (ne < -10) return (uint16_t)s;
        m |= 0x800000;
        int sh = 14 - ne;
        return (uint16_t)(s | ((m + (1u << (sh - 1))) >> sh));
    }
    return (uint16_t)(s | ((uint32_t)ne << 10) | ((m + 0x1000) >> 13));
}

static inline uint32_t pk(uint32_t a, uint32_t r, uint32_t g, uint32_t b) { return (a << 24) | (r << 16) | (g << 8) | b; }
static inline uint32_t ex5(uint32_t v) { return (v << 3) | (v >> 2); }
static inline uint32_t ex6(uint32_t v) { return (v << 2) | (v >> 4); }
static inline uint32_t ex4(uint32_t v) { return v * 17; }
static inline uint32_t ex3(uint32_t v) { return (v << 5) | (v << 2) | (v >> 1); }
static inline uint32_t ex2(uint32_t v) { return v * 85; }
static inline uint32_t clamp8f(float v) { return !(v > 0) ? 0 : v >= 1 ? 255 : (uint32_t)(v * 255.0f + 0.5f); }

void d9_unpack_row(int fmt, const uint8_t *s, uint32_t *d, int n, const uint32_t *pal) {
    switch (fmt) {
    case F_A8R8G8B8: memcpy(d, s, (size_t)n * 4); break;
    case F_X8R8G8B8: for (int i = 0; i < n; i++) { uint32_t v; memcpy(&v, s + 4 * i, 4); d[i] = v | 0xFF000000u; } break;
    case F_A8B8G8R8: case F_X8B8G8R8:
        for (int i = 0; i < n; i++) { const uint8_t *p = s + 4 * i; d[i] = pk(fmt == F_A8B8G8R8 ? p[3] : 255, p[0], p[1], p[2]); }
        break;
    case F_R8G8B8: for (int i = 0; i < n; i++) { const uint8_t *p = s + 3 * i; d[i] = pk(255, p[2], p[1], p[0]); } break;
    case F_R5G6B5: for (int i = 0; i < n; i++) { uint16_t v; memcpy(&v, s + 2 * i, 2); d[i] = pk(255, ex5(v >> 11), ex6((v >> 5) & 63), ex5(v & 31)); } break;
    case F_X1R5G5B5: case F_A1R5G5B5:
        for (int i = 0; i < n; i++) { uint16_t v; memcpy(&v, s + 2 * i, 2); d[i] = pk(fmt == F_X1R5G5B5 || (v >> 15) ? 255 : 0, ex5((v >> 10) & 31), ex5((v >> 5) & 31), ex5(v & 31)); }
        break;
    case F_A4R4G4B4: case F_X4R4G4B4:
        for (int i = 0; i < n; i++) { uint16_t v; memcpy(&v, s + 2 * i, 2); d[i] = pk(fmt == F_X4R4G4B4 ? 255 : ex4(v >> 12), ex4((v >> 8) & 15), ex4((v >> 4) & 15), ex4(v & 15)); }
        break;
    case F_R3G3B2: for (int i = 0; i < n; i++) { uint8_t v = s[i]; d[i] = pk(255, ex3(v >> 5), ex3((v >> 2) & 7), ex2(v & 3)); } break;
    case F_A8R3G3B2: for (int i = 0; i < n; i++) { uint16_t v; memcpy(&v, s + 2 * i, 2); d[i] = pk(v >> 8, ex3((v >> 5) & 7), ex3((v >> 2) & 7), ex2(v & 3)); } break;
    case F_A8: for (int i = 0; i < n; i++) d[i] = pk(s[i], 0, 0, 0); break;
    case F_L8: for (int i = 0; i < n; i++) d[i] = pk(255, s[i], s[i], s[i]); break;
    case F_A8L8: for (int i = 0; i < n; i++) d[i] = pk(s[2 * i + 1], s[2 * i], s[2 * i], s[2 * i]); break;
    case F_A4L4: for (int i = 0; i < n; i++) { uint32_t l = ex4(s[i] & 15); d[i] = pk(ex4(s[i] >> 4), l, l, l); } break;
    case F_L16: for (int i = 0; i < n; i++) { uint32_t l = s[2 * i + 1]; d[i] = pk(255, l, l, l); } break;
    case F_P8: for (int i = 0; i < n; i++) d[i] = pal ? pal[s[i]] : pk(255, s[i], s[i], s[i]); break;
    case F_A8P8: for (int i = 0; i < n; i++) { uint32_t c = pal ? pal[s[2 * i]] : pk(255, s[2 * i], s[2 * i], s[2 * i]); d[i] = (c & 0xFFFFFFu) | ((uint32_t)s[2 * i + 1] << 24); } break;
    case F_A2R10G10B10: case F_A2B10G10R10:
        for (int i = 0; i < n; i++) {
            uint32_t v; memcpy(&v, s + 4 * i, 4);
            uint32_t a = ex2(v >> 30), c2 = (v >> 20) & 1023, c1 = (v >> 10) & 1023, c0 = v & 1023;
            if (fmt == F_A2R10G10B10) d[i] = pk(a, c2 >> 2, c1 >> 2, c0 >> 2); else d[i] = pk(a, c0 >> 2, c1 >> 2, c2 >> 2);
        }
        break;
    case F_G16R16: for (int i = 0; i < n; i++) { const uint8_t *p = s + 4 * i; d[i] = pk(255, p[1], p[3], 0); } break;
    case F_A16B16G16R16: for (int i = 0; i < n; i++) { const uint8_t *p = s + 8 * i; d[i] = pk(p[7], p[1], p[3], p[5]); } break;
    case F_R16F: for (int i = 0; i < n; i++) { uint16_t h; memcpy(&h, s + 2 * i, 2); d[i] = pk(255, clamp8f(d9_half_to_float(h)), 255, 255); } break;
    case F_G16R16F: for (int i = 0; i < n; i++) { uint16_t h[2]; memcpy(h, s + 4 * i, 4); d[i] = pk(255, clamp8f(d9_half_to_float(h[0])), clamp8f(d9_half_to_float(h[1])), 255); } break;
    case F_A16B16G16R16F:
        for (int i = 0; i < n; i++) { uint16_t h[4]; memcpy(h, s + 8 * i, 8); d[i] = pk(clamp8f(d9_half_to_float(h[3])), clamp8f(d9_half_to_float(h[0])), clamp8f(d9_half_to_float(h[1])), clamp8f(d9_half_to_float(h[2]))); }
        break;
    case F_R32F: for (int i = 0; i < n; i++) { float f; memcpy(&f, s + 4 * i, 4); d[i] = pk(255, clamp8f(f), 255, 255); } break;
    case F_G32R32F: for (int i = 0; i < n; i++) { float f[2]; memcpy(f, s + 8 * i, 8); d[i] = pk(255, clamp8f(f[0]), clamp8f(f[1]), 255); } break;
    case F_A32B32G32R32F: for (int i = 0; i < n; i++) { float f[4]; memcpy(f, s + 16 * i, 16); d[i] = pk(clamp8f(f[3]), clamp8f(f[0]), clamp8f(f[1]), clamp8f(f[2])); } break;
    case F_V8U8: for (int i = 0; i < n; i++) d[i] = pk(255, s[2 * i], s[2 * i + 1], 255); break;
    case F_L6V5U5: for (int i = 0; i < n; i++) { uint16_t v; memcpy(&v, s + 2 * i, 2); d[i] = pk(255, (v & 31) << 3, ((v >> 5) & 31) << 3, (v >> 10) << 2); } break;
    case F_X8L8V8U8: case F_Q8W8V8U8: for (int i = 0; i < n; i++) { const uint8_t *p = s + 4 * i; d[i] = pk(fmt == F_Q8W8V8U8 ? p[3] : 255, p[0], p[1], p[2]); } break;
    case F_V16U16: for (int i = 0; i < n; i++) { const uint8_t *p = s + 4 * i; d[i] = pk(255, p[1], p[3], 255); } break;
    case F_Q16W16V16U16: for (int i = 0; i < n; i++) { const uint8_t *p = s + 8 * i; d[i] = pk(p[7], p[1], p[3], p[5]); } break;
    case F_A2W10V10U10: for (int i = 0; i < n; i++) { uint32_t v; memcpy(&v, s + 4 * i, 4); d[i] = pk(ex2(v >> 30), (v & 1023) >> 2, ((v >> 10) & 1023) >> 2, ((v >> 20) & 1023) >> 2); } break;
    default: for (int i = 0; i < n; i++) d[i] = 0xFF000000u; break;
    }
}

int d9_pack_row(int fmt, const uint32_t *s, uint8_t *d, int n) {
    switch (fmt) {
    case F_A8R8G8B8: memcpy(d, s, (size_t)n * 4); return 1;
    case F_X8R8G8B8: for (int i = 0; i < n; i++) { uint32_t v = s[i] | 0xFF000000u; memcpy(d + 4 * i, &v, 4); } return 1;
    case F_A8B8G8R8: case F_X8B8G8R8:
        for (int i = 0; i < n; i++) { uint32_t c = s[i]; uint8_t *p = d + 4 * i; p[0] = (uint8_t)(c >> 16); p[1] = (uint8_t)(c >> 8); p[2] = (uint8_t)c; p[3] = fmt == F_A8B8G8R8 ? (uint8_t)(c >> 24) : 255; }
        return 1;
    case F_R8G8B8: for (int i = 0; i < n; i++) { uint32_t c = s[i]; uint8_t *p = d + 3 * i; p[0] = (uint8_t)c; p[1] = (uint8_t)(c >> 8); p[2] = (uint8_t)(c >> 16); } return 1;
    case F_R5G6B5: for (int i = 0; i < n; i++) { uint32_t c = s[i]; uint16_t v = (uint16_t)((((c >> 16) & 255) >> 3 << 11) | (((c >> 8) & 255) >> 2 << 5) | ((c & 255) >> 3)); memcpy(d + 2 * i, &v, 2); } return 1;
    case F_X1R5G5B5: case F_A1R5G5B5:
        for (int i = 0; i < n; i++) { uint32_t c = s[i]; uint16_t v = (uint16_t)(((fmt == F_X1R5G5B5 || (c >> 31)) ? 0x8000 : 0) | (((c >> 16) & 255) >> 3 << 10) | (((c >> 8) & 255) >> 3 << 5) | ((c & 255) >> 3)); memcpy(d + 2 * i, &v, 2); }
        return 1;
    case F_A4R4G4B4: case F_X4R4G4B4:
        for (int i = 0; i < n; i++) { uint32_t c = s[i]; uint16_t v = (uint16_t)(((fmt == F_X4R4G4B4 ? 15u : (c >> 28)) << 12) | (((c >> 20) & 15) << 8) | (((c >> 12) & 15) << 4) | ((c >> 4) & 15)); memcpy(d + 2 * i, &v, 2); }
        return 1;
    case F_A8: for (int i = 0; i < n; i++) d[i] = (uint8_t)(s[i] >> 24); return 1;
    case F_L8: for (int i = 0; i < n; i++) d[i] = (uint8_t)((((s[i] >> 16) & 255) * 77 + ((s[i] >> 8) & 255) * 150 + (s[i] & 255) * 29) >> 8); return 1;
    case F_A8L8: for (int i = 0; i < n; i++) { d[2 * i] = (uint8_t)((((s[i] >> 16) & 255) * 77 + ((s[i] >> 8) & 255) * 150 + (s[i] & 255) * 29) >> 8); d[2 * i + 1] = (uint8_t)(s[i] >> 24); } return 1;
    case F_A2R10G10B10: case F_A2B10G10R10:
        for (int i = 0; i < n; i++) {
            uint32_t c = s[i], r = ((c >> 16) & 255) * 1023 / 255, g = ((c >> 8) & 255) * 1023 / 255, b = (c & 255) * 1023 / 255, a = (c >> 30);
            uint32_t v = fmt == F_A2R10G10B10 ? (a << 30) | (r << 20) | (g << 10) | b : (a << 30) | (b << 20) | (g << 10) | r;
            memcpy(d + 4 * i, &v, 4);
        }
        return 1;
    case F_G16R16: for (int i = 0; i < n; i++) { uint32_t c = s[i]; uint16_t v[2] = { (uint16_t)(((c >> 16) & 255) * 257), (uint16_t)(((c >> 8) & 255) * 257) }; memcpy(d + 4 * i, v, 4); } return 1;
    case F_A16B16G16R16: for (int i = 0; i < n; i++) { uint32_t c = s[i]; uint16_t v[4] = { (uint16_t)(((c >> 16) & 255) * 257), (uint16_t)(((c >> 8) & 255) * 257), (uint16_t)((c & 255) * 257), (uint16_t)((c >> 24) * 257) }; memcpy(d + 8 * i, v, 8); } return 1;
    case F_R16F: for (int i = 0; i < n; i++) { uint16_t h = d9_float_to_half(((s[i] >> 16) & 255) / 255.0f); memcpy(d + 2 * i, &h, 2); } return 1;
    case F_G16R16F: for (int i = 0; i < n; i++) { uint16_t h[2] = { d9_float_to_half(((s[i] >> 16) & 255) / 255.0f), d9_float_to_half(((s[i] >> 8) & 255) / 255.0f) }; memcpy(d + 4 * i, h, 4); } return 1;
    case F_A16B16G16R16F:
        for (int i = 0; i < n; i++) { uint32_t c = s[i]; uint16_t h[4] = { d9_float_to_half(((c >> 16) & 255) / 255.0f), d9_float_to_half(((c >> 8) & 255) / 255.0f), d9_float_to_half((c & 255) / 255.0f), d9_float_to_half((c >> 24) / 255.0f) }; memcpy(d + 8 * i, h, 8); }
        return 1;
    case F_R32F: for (int i = 0; i < n; i++) { float f = ((s[i] >> 16) & 255) / 255.0f; memcpy(d + 4 * i, &f, 4); } return 1;
    case F_G32R32F: for (int i = 0; i < n; i++) { float f[2] = { ((s[i] >> 16) & 255) / 255.0f, ((s[i] >> 8) & 255) / 255.0f }; memcpy(d + 8 * i, f, 8); } return 1;
    case F_A32B32G32R32F:
        for (int i = 0; i < n; i++) { uint32_t c = s[i]; float f[4] = { ((c >> 16) & 255) / 255.0f, ((c >> 8) & 255) / 255.0f, (c & 255) / 255.0f, (c >> 24) / 255.0f }; memcpy(d + 16 * i, f, 16); }
        return 1;
    case F_V8U8: for (int i = 0; i < n; i++) { d[2 * i] = (uint8_t)(s[i] >> 16); d[2 * i + 1] = (uint8_t)(s[i] >> 8); } return 1;
    case F_Q8W8V8U8: for (int i = 0; i < n; i++) { uint32_t c = s[i]; uint8_t *p = d + 4 * i; p[0] = (uint8_t)(c >> 16); p[1] = (uint8_t)(c >> 8); p[2] = (uint8_t)c; p[3] = (uint8_t)(c >> 24); } return 1;
    }
    return 0;
}

static uint32_t c565(uint32_t v) { return pk(255, ex5(v >> 11), ex6((v >> 5) & 63), ex5(v & 31)); }
static uint32_t mix(uint32_t a, uint32_t b, int wa, int wb, int den) {
    uint32_t r = 0;
    for (int sh = 0; sh < 32; sh += 8) r |= (((((a >> sh) & 255) * (uint32_t)wa + ((b >> sh) & 255) * (uint32_t)wb) / (uint32_t)den) & 255) << sh;
    return r;
}
void d9_decode_dxt(int fmt, const uint8_t *src, int w, int h, size_t pitch, uint32_t *dst) {
    int bsz = fmt == F_DXT1 ? 8 : 16;
    for (int by = 0; by < (h + 3) / 4; by++) for (int bx = 0; bx < (w + 3) / 4; bx++) {
        const uint8_t *b = src + (size_t)by * pitch + (size_t)bx * (size_t)bsz;
        const uint8_t *cb = b + (fmt == F_DXT1 ? 0 : 8);
        uint32_t c0v = (uint32_t)cb[0] | ((uint32_t)cb[1] << 8), c1v = (uint32_t)cb[2] | ((uint32_t)cb[3] << 8);
        uint32_t col[4] = { c565(c0v), c565(c1v), 0, 0 };
        int punch = 0;
        if (fmt != F_DXT1 || c0v > c1v) { col[2] = mix(col[0], col[1], 2, 1, 3); col[3] = mix(col[0], col[1], 1, 2, 3); }
        else { col[2] = mix(col[0], col[1], 1, 1, 2); col[3] = 0; punch = 1; }
        uint32_t idx = (uint32_t)cb[4] | ((uint32_t)cb[5] << 8) | ((uint32_t)cb[6] << 16) | ((uint32_t)cb[7] << 24);
        uint8_t al[16];
        if (fmt == F_DXT3 || fmt == F_DXT2) {
            for (int i = 0; i < 16; i++) al[i] = (uint8_t)(((b[i / 2] >> ((i & 1) * 4)) & 15) * 17);
        } else if (fmt == F_DXT5 || fmt == F_DXT4) {
            uint8_t a0 = b[0], a1 = b[1], pal[8];
            pal[0] = a0; pal[1] = a1;
            if (a0 > a1) for (int i = 1; i < 7; i++) pal[1 + i] = (uint8_t)(((7 - i) * a0 + i * a1) / 7);
            else { for (int i = 1; i < 5; i++) pal[1 + i] = (uint8_t)(((5 - i) * a0 + i * a1) / 5); pal[6] = 0; pal[7] = 255; }
            uint64_t bits = 0;
            for (int i = 0; i < 6; i++) bits |= (uint64_t)b[2 + i] << (8 * i);
            for (int i = 0; i < 16; i++) al[i] = pal[(bits >> (3 * i)) & 7];
        } else for (int i = 0; i < 16; i++) al[i] = 255;
        for (int py = 0; py < 4; py++) for (int px = 0; px < 4; px++) {
            int x = bx * 4 + px, y = by * 4 + py;
            if (x >= w || y >= h) continue;
            int ci = (int)((idx >> (2 * (py * 4 + px))) & 3);
            uint32_t c = col[ci];
            if (punch && ci == 3) c = 0; else c = (c & 0xFFFFFFu) | ((uint32_t)al[py * 4 + px] << 24);
            dst[(size_t)y * (size_t)w + (size_t)x] = c;
        }
    }
}
