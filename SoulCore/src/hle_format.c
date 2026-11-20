// SoulCore - printf / scanf engine with Windows (LLP64) argument semantics.
#include "hle.h"
#include <ctype.h>
#include <math.h>

typedef struct { char *p; size_t len, cap; } SBuf;
static void sb_put(SBuf *b, const char *s, size_t n) {
    if (b->len + n + 1 > b->cap) {
        size_t nc = b->cap ? b->cap * 2 : 256;
        while (nc < b->len + n + 1) nc *= 2;
        b->p = realloc(b->p, nc);
        b->cap = nc;
    }
    memcpy(b->p + b->len, s, n);
    b->len += n;
    b->p[b->len] = 0;
}
static void sb_pad(SBuf *b, char c, int n) { for (int i = 0; i < n; i++) sb_put(b, &c, 1); }

static size_t utf8_count_chars(const char *s, size_t n) {
    size_t c = 0;
    for (size_t i = 0; i < n; i++) if (((unsigned char)s[i] & 0xC0) != 0x80) c++;
    return c;
}
static size_t utf8_prefix_bytes(const char *s, size_t n, size_t maxchars) {
    size_t c = 0, i = 0;
    for (; i < n; i++) {
        if (((unsigned char)s[i] & 0xC0) != 0x80) {
            if (c == maxchars) break;
            c++;
        }
    }
    return i;
}

char *fmt_vformat(const char *fmt, ArgSrc *a, int wide_fmt, size_t *outlen) {
    SBuf b = { 0 };
    sb_put(&b, "", 0);
    const char *f = fmt;
    while (*f) {
        if (*f != '%') {
            const char *s = f;
            while (*f && *f != '%') f++;
            sb_put(&b, s, (size_t)(f - s));
            continue;
        }
        const char *spec_start = f++;
        if (*f == '%') { sb_put(&b, "%", 1); f++; continue; }
        char flags[8]; int nf = 0;
        while (*f && strchr("-+ #0", *f) && nf < 7) flags[nf++] = *f++;
        flags[nf] = 0;
        int width = -1, prec = -1;
        if (*f == '*') { width = (int)(int32_t)arg_next(a); f++; if (width < 0) { width = -width; if (nf < 7) { flags[nf++] = '-'; flags[nf] = 0; } } }
        else if (isdigit((unsigned char)*f)) { width = 0; while (isdigit((unsigned char)*f)) width = width * 10 + (*f++ - '0'); }
        if (*f == '.') {
            f++;
            prec = 0;
            if (*f == '*') { prec = (int)(int32_t)arg_next(a); f++; if (prec < 0) prec = -1; }
            else while (isdigit((unsigned char)*f)) prec = prec * 10 + (*f++ - '0');
        }
        int lsize = 0; // 0 default(int), 1 hh, 2 h, 3 l, 4 ll/64, 5 L
        int wideflag = 0, narrowflag = 0;
        for (;;) {
            if (*f == 'h') { f++; if (*f == 'h') { f++; lsize = 1; } else { lsize = 2; narrowflag = 1; } }
            else if (*f == 'l') { f++; if (*f == 'l') { f++; lsize = 4; } else { lsize = 3; wideflag = 1; } }
            else if (*f == 'w') { f++; wideflag = 1; lsize = 3; }
            else if (*f == 'L') { f++; lsize = 5; }
            else if (*f == 'I') {
                f++;
                if (f[0] == '6' && f[1] == '4') { f += 2; lsize = 4; }
                else if (f[0] == '3' && f[1] == '2') { f += 2; lsize = 0; }
                else lsize = 4;
            }
            else if (*f == 'z' || *f == 'j' || *f == 't') { f++; lsize = 4; }
            else break;
        }
        char conv = *f ? *f++ : 0;
        char spec[64];
        char tmp[512];
        switch (conv) {
        case 'd': case 'i': case 'u': case 'o': case 'x': case 'X': {
            uint64_t raw = arg_next(a);
            int is_signed = (conv == 'd' || conv == 'i');
            long long sv; unsigned long long uv;
            switch (lsize) {
            case 1: sv = (int8_t)raw; uv = (uint8_t)raw; break;
            case 2: sv = (int16_t)raw; uv = (uint16_t)raw; break;
            case 4: sv = (int64_t)raw; uv = raw; break;
            default: sv = (int32_t)raw; uv = (uint32_t)raw; break;
            }
            char *sp = spec;
            *sp++ = '%';
            for (int k = 0; flags[k]; k++) *sp++ = flags[k];
            if (width >= 0) sp += sprintf(sp, "%d", width);
            if (prec >= 0) sp += sprintf(sp, ".%d", prec);
            *sp++ = 'l'; *sp++ = 'l'; *sp++ = conv; *sp = 0;
            int n = is_signed ? snprintf(tmp, sizeof tmp, spec, sv) : snprintf(tmp, sizeof tmp, spec, uv);
            sb_put(&b, tmp, (size_t)(n < (int)sizeof tmp ? n : (int)sizeof tmp - 1));
            break;
        }
        case 'p': {
            uint64_t v = arg_next(a);
            int n = snprintf(tmp, sizeof tmp, "%016llX", (unsigned long long)v);
            int pad = width > n ? width - n : 0;
            int left = strchr(flags, '-') != NULL;
            if (!left) sb_pad(&b, ' ', pad);
            sb_put(&b, tmp, (size_t)n);
            if (left) sb_pad(&b, ' ', pad);
            break;
        }
        case 'f': case 'F': case 'e': case 'E': case 'g': case 'G': case 'a': case 'A': {
            uint64_t raw = arg_next(a);
            double v; memcpy(&v, &raw, 8);
            char *sp = spec;
            *sp++ = '%';
            for (int k = 0; flags[k]; k++) *sp++ = flags[k];
            if (width >= 0) sp += sprintf(sp, "%d", width);
            if (prec >= 0) sp += sprintf(sp, ".%d", prec);
            *sp++ = conv; *sp = 0;
            int n = snprintf(tmp, sizeof tmp, spec, v);
            if (n >= (int)sizeof tmp) {
                char *big = malloc((size_t)n + 1);
                snprintf(big, (size_t)n + 1, spec, v);
                sb_put(&b, big, (size_t)n);
                free(big);
            } else {
                sb_put(&b, tmp, (size_t)n);
            }
            break;
        }
        case 'c': case 'C': {
            uint64_t raw = arg_next(a);
            int wide = (conv == 'C') ^ wide_fmt;
            if (wideflag) wide = 1;
            if (narrowflag) wide = 0;
            char u[8];
            size_t n;
            if (wide) { uint16_t w[2] = { (uint16_t)raw, 0 }; n = w2u(w, 1, u, sizeof u); }
            else { u[0] = (char)raw; n = 1; }
            int pad = width > 1 ? width - 1 : 0;
            int left = strchr(flags, '-') != NULL;
            if (!left) sb_pad(&b, ' ', pad);
            sb_put(&b, u, n);
            if (left) sb_pad(&b, ' ', pad);
            break;
        }
        case 's': case 'S': case 'Z': {
            uint64_t ptr = arg_next(a);
            int wide = (conv == 'S') ^ wide_fmt;
            if (wideflag) wide = 1;
            if (narrowflag) wide = 0;
            char *str = NULL;
            const char *s;
            size_t n;
            if (conv == 'Z') { // ANSI_STRING / UNICODE_STRING
                if (ptr) {
                    uint16_t len; memcpy(&len, (void *)(uintptr_t)ptr, 2);
                    uint64_t buf = mem_rd(ptr + 8, 8);
                    if (wide) { str = malloc(len * 2u + 4); w2u((const uint16_t *)(uintptr_t)buf, len / 2, str, len * 2u + 4); }
                    else { str = malloc(len + 1u); memcpy(str, (void *)(uintptr_t)buf, len); str[len] = 0; }
                }
                s = str ? str : "(null)";
            } else if (!ptr) {
                s = "(null)";
            } else if (wide) {
                str = w2u_dup((const uint16_t *)(uintptr_t)ptr);
                s = str;
            } else {
                s = (const char *)(uintptr_t)ptr;
            }
            n = strlen(s);
            if (prec >= 0) n = utf8_prefix_bytes(s, n, (size_t)prec);
            int chars = (int)utf8_count_chars(s, n);
            int pad = width > chars ? width - chars : 0;
            int left = strchr(flags, '-') != NULL;
            if (!left) sb_pad(&b, ' ', pad);
            sb_put(&b, s, n);
            if (left) sb_pad(&b, ' ', pad);
            free(str);
            break;
        }
        case 'n': {
            uint64_t ptr = arg_next(a);
            if (ptr) mem_wr(ptr, lsize == 4 ? 8 : lsize == 2 ? 2 : 4, b.len);
            break;
        }
        default:
            // unknown conversion: print verbatim
            sb_put(&b, spec_start, (size_t)(f - spec_start));
            break;
        }
    }
    *outlen = b.len;
    return b.p;
}

size_t fmt_format(OutSink *out, const char *fmt, ArgSrc *args, int wide_fmt) {
    size_t n;
    char *s = fmt_vformat(fmt, args, wide_fmt, &n);
    size_t units;
    if (out->flush) {
        out->flush(out, s, n);
        units = out->wide ? u2w(s, (long)n, NULL, 0) : n;
    } else if (out->wide) {
        units = u2w(s, (long)n, NULL, 0);
        if (out->buf && out->cap) u2w(s, (long)n, (uint16_t *)out->buf, out->cap);
    } else {
        units = n;
        if (out->buf && out->cap) {
            size_t c = n < out->cap - 1 ? n : out->cap - 1;
            memcpy(out->buf, s, c);
            out->buf[c] = 0;
        }
    }
    out->len = units;
    free(s);
    return units;
}

// ---- scanf ---------------------------------------------------------------------

static int in_get(InSrc *in) {
    int c = in->getc(in);
    if (c >= 0) in->consumed++;
    return c;
}
static void in_unget(InSrc *in, int c) {
    if (c >= 0) { in->ungetc(in, c); in->consumed--; }
}

int fmt_scan(InSrc *in, const char *fmt, ArgSrc *a, int wide_fmt) {
    int assigned = 0;
    int any_input = 0;
    const char *f = fmt;
    while (*f) {
        if (isspace((unsigned char)*f)) {
            int c;
            while ((c = in_get(in)) >= 0 && isspace(c)) {}
            in_unget(in, c);
            while (isspace((unsigned char)*f)) f++;
            continue;
        }
        if (*f != '%' || f[1] == '%') {
            char want = *f == '%' ? '%' : *f;
            f += (*f == '%') ? 2 : 1;
            int c = in_get(in);
            if (c != (unsigned char)want) { in_unget(in, c); if (c < 0 && !any_input) return -1; return assigned; }
            continue;
        }
        f++;
        int suppress = 0;
        if (*f == '*') { suppress = 1; f++; }
        int width = 0;
        while (isdigit((unsigned char)*f)) width = width * 10 + (*f++ - '0');
        int lsize = 0, wide = 0;
        for (;;) {
            if (*f == 'h') { f++; if (*f == 'h') { f++; lsize = 1; } else lsize = 2; }
            else if (*f == 'l') { f++; if (*f == 'l') { f++; lsize = 4; } else { lsize = 3; wide = 1; } }
            else if (*f == 'L') { f++; lsize = 5; }
            else if (*f == 'I') { f++; if (f[0] == '6' && f[1] == '4') { f += 2; lsize = 4; } else if (f[0] == '3' && f[1] == '2') { f += 2; lsize = 0; } else lsize = 4; }
            else if (*f == 'z' || *f == 'j' || *f == 't') { f++; lsize = 4; }
            else if (*f == 'w') { f++; wide = 1; }
            else break;
        }
        char conv = *f ? *f++ : 0;
        if (conv == 'n') {
            if (!suppress) { uint64_t p = arg_next(a); mem_wr(p, 4, (uint64_t)in->consumed); }
            continue;
        }
        if (conv != 'c' && conv != '[') {
            int c;
            while ((c = in_get(in)) >= 0 && isspace(c)) {}
            if (c < 0) return any_input || assigned ? assigned : -1;
            in_unget(in, c);
        }
        char buf[512];
        int n = 0;
        int maxw = width ? width : (int)sizeof(buf) - 1;
        if (maxw > (int)sizeof(buf) - 1) maxw = (int)sizeof(buf) - 1;
        if (conv == 'c') {
            int cnt = width ? width : 1;
            uint64_t p = suppress ? 0 : arg_next(a);
            if (p && in->secure) { uint64_t cap = arg_next(a) & 0xFFFFFFFF; if ((uint64_t)cnt > cap) return assigned; }
            for (int i = 0; i < cnt; i++) {
                int c = in_get(in);
                if (c < 0) return any_input ? assigned : -1;
                any_input = 1;
                if (p) { if (wide ^ wide_fmt) mem_wr(p + 2u * (unsigned)i, 2, (uint64_t)c); else mem_wr(p + (unsigned)i, 1, (uint64_t)c); }
            }
            if (!suppress) assigned++;
            continue;
        }
        if (conv == 's' || conv == 'S' || conv == '[') {
            char set[256] = { 0 };
            int negate = 0;
            if (conv == '[') {
                if (*f == '^') { negate = 1; f++; }
                if (*f == ']') { set[(unsigned char)']'] = 1; f++; }
                while (*f && *f != ']') {
                    if (f[1] == '-' && f[2] && f[2] != ']') {
                        for (int ch = (unsigned char)f[0]; ch <= (unsigned char)f[2]; ch++) set[ch] = 1;
                        f += 3;
                    } else set[(unsigned char)*f++] = 1;
                }
                if (*f == ']') f++;
            }
            int c;
            while (n < maxw && (c = in_get(in)) >= 0) {
                int ok = (conv == '[') ? (set[c] ^ negate) : !isspace(c);
                if (!ok) { in_unget(in, c); break; }
                buf[n++] = (char)c;
            }
            buf[n] = 0;
            if (n == 0) return any_input || assigned ? assigned : -1;
            any_input = 1;
            if (!suppress) {
                uint64_t p = arg_next(a);
                if (in->secure) {
                    uint64_t cap = arg_next(a) & 0xFFFFFFFF;
                    if ((uint64_t)n + 1 > cap) { if (cap) mem_wr(p, 1, 0); return assigned; }
                }
                int w = (conv == 'S') ? !wide_fmt : (wide || wide_fmt);
                if (conv == 'S' && wide) w = 1;
                if (w) u2w(buf, -1, (uint16_t *)(uintptr_t)p, (size_t)n + 1);
                else memcpy((void *)(uintptr_t)p, buf, (size_t)n + 1);
                assigned++;
            }
            continue;
        }
        if (strchr("diouxXp", conv)) {
            int base = conv == 'o' ? 8 : (conv == 'x' || conv == 'X' || conv == 'p') ? 16 : conv == 'i' ? 0 : 10;
            int c = in_get(in);
            if (c == '+' || c == '-') { buf[n++] = (char)c; c = in_get(in); }
            if ((base == 16 || base == 0) && c == '0') {
                buf[n++] = '0';
                c = in_get(in);
                if (c == 'x' || c == 'X') { buf[n++] = (char)c; base = 16; c = in_get(in); }
                else if (base == 0) base = 8;
            }
            if (base == 0) base = 10;
            while (c >= 0 && n < maxw) {
                int ok = (base == 16) ? isxdigit(c) : (base == 8) ? (c >= '0' && c <= '7') : isdigit(c);
                if (!ok) break;
                buf[n++] = (char)c;
                c = in_get(in);
            }
            in_unget(in, c);
            buf[n] = 0;
            if (n == 0 || (n == 1 && (buf[0] == '+' || buf[0] == '-'))) return any_input || assigned ? assigned : -1;
            any_input = 1;
            if (!suppress) {
                uint64_t v = (conv == 'd' || conv == 'i') ? (uint64_t)strtoll(buf, NULL, base) : strtoull(buf, NULL, base);
                uint64_t p = arg_next(a);
                int sz = conv == 'p' ? 8 : lsize == 1 ? 1 : lsize == 2 ? 2 : lsize == 4 ? 8 : 4;
                mem_wr(p, sz, v);
                assigned++;
            }
            continue;
        }
        if (strchr("eEfFgGaA", conv)) {
            int c = in_get(in);
            while (c >= 0 && n < maxw && (isdigit(c) || strchr("+-.eExXabcdefABCDEFinftyINFTYnN", c))) {
                buf[n++] = (char)c;
                c = in_get(in);
            }
            in_unget(in, c);
            buf[n] = 0;
            char *end;
            double v = strtod(buf, &end);
            // give back characters strtod did not use
            for (char *q = buf + n - 1; q >= end; q--) in_unget(in, (unsigned char)*q);
            if (end == buf) return any_input || assigned ? assigned : -1;
            any_input = 1;
            if (!suppress) {
                uint64_t p = arg_next(a);
                if (lsize == 3 || lsize == 5) { memcpy((void *)(uintptr_t)p, &v, 8); }
                else { float fv = (float)v; memcpy((void *)(uintptr_t)p, &fv, 4); }
                assigned++;
            }
            continue;
        }
        break;
    }
    return assigned;
}
