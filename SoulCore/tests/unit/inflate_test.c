// Unit test for src/inflate.c. Usage: inflate_test <vector dir> (vectors from inflate_vectors.py).
#include "inflate.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_pass, g_fail;
static void report(const char *name, int ok) {
    printf("%s %s\n", ok ? "PASS" : "FAIL", name);
    if (ok) g_pass++; else g_fail++;
}
static uint8_t *slurp(const char *dir, const char *name, size_t *n) {
    char p[1024];
    snprintf(p, sizeof p, "%s/%s", dir, name);
    FILE *f = fopen(p, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *b = malloc((size_t)sz + 1);
    *n = fread(b, 1, (size_t)sz, f);
    fclose(f);
    return b;
}

int main(int argc, char **argv) {
    const char *dir = argc > 1 ? argv[1] : ".";
    size_t mn;
    char *man = (char *)slurp(dir, "inflate_manifest.txt", &mn);
    if (!man) { printf("FAIL manifest\n"); return 1; }
    man[mn] = 0;
    char *save = NULL;
    for (char *line = strtok_r(man, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
        char name[128], kind[16], dn[128];
        if (sscanf(line, "%127s %15s %127s", name, kind, dn) != 3) continue;
        char f1[160], f2[160];
        snprintf(f1, sizeof f1, "%s.comp", name);
        snprintf(f2, sizeof f2, "%s.orig", name);
        size_t cn, on, dl = 0;
        uint8_t *comp = slurp(dir, f1, &cn), *orig = slurp(dir, f2, &on), *dict = strcmp(dn, "-") ? slurp(dir, dn, &dl) : NULL;
        if (!comp || !orig) { report(name, 0); continue; }
        uint8_t *out = malloc(on + 1);
        long r = !strcmp(kind, "zlib") ? soul_zlib_inflate(comp, cn, out, on) : soul_inflate(comp, cn, out, on, dict, dl);
        int ok = r == (long)on && (on == 0 || memcmp(out, orig, on) == 0);
        report(name, ok);
        // exact-size buffer was used above; one byte too small must fail cleanly
        if (on > 0) {
            uint8_t *small = malloc(on);
            long r2 = !strcmp(kind, "zlib") ? soul_zlib_inflate(comp, cn, small, on - 1) : soul_inflate(comp, cn, small, on - 1, dict, dl);
            char tn[160];
            snprintf(tn, sizeof tn, "%s_small_out", name);
            report(tn, r2 == -1);
            free(small);
        }
        // truncated input must fail (or at least not crash) - only checked for streams of real size
        if (cn > 16 && strcmp(kind, "zlib")) {
            long r3 = soul_inflate(comp, cn / 2, out, on, dict, dl);
            char tn[160];
            snprintf(tn, sizeof tn, "%s_truncated", name);
            report(tn, r3 == -1);
        }
        free(comp); free(orig); free(out); free(dict);
    }
    // garbage input: must not crash
    {
        uint8_t junk[512], out[4096];
        unsigned s = 99;
        int ok = 1;
        for (int t = 0; t < 2000; t++) {
            for (int i = 0; i < 512; i++) { s = s * 1103515245u + 12345u; junk[i] = (uint8_t)(s >> 16); }
            long r = soul_inflate(junk, 1 + (size_t)(t % 511), out, sizeof out, NULL, 0);
            if (r > (long)sizeof out) ok = 0;
        }
        report("garbage_no_crash", ok);
    }
    // bad zlib header / FDICT
    {
        uint8_t out[16], bad[4] = { 0x78, 0x01 + 1, 0, 0 }, fd[8] = { 0x78, 0xBB, 0, 0, 0, 0, 3, 0 };
        report("zlib_bad_header", soul_zlib_inflate(bad, 4, out, sizeof out) == -1);
        report("zlib_fdict_rejected", soul_zlib_inflate(fd, 8, out, sizeof out) == -1);
    }
    printf("passed=%d failed=%d\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
