// soulrun - command line runner for SoulCore (Linux/macOS testing).
#include "../include/SoulCore.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static SoulProcess *g_p;
extern uint64_t g_interp_hist[4 * 256];
extern int g_interp_profile;
static int g_quiet_log;

static void on_output(void *ctx, int stream, const char *data, size_t len) {
    (void)ctx;
    if (stream == SOUL_STREAM_LOG && g_quiet_log) return;
    FILE *f = stream == SOUL_STREAM_STDOUT ? stdout : stderr;
    fwrite(data, 1, len, f);
    fflush(f);
}

static void *stdin_pump(void *arg) {
    (void)arg;
    char buf[4096];
    ssize_t n;
    while ((n = read(0, buf, sizeof buf)) > 0) soul_process_send_input(g_p, buf, (size_t)n);
    soul_process_close_input(g_p);
    return NULL;
}

int main(int argc, char **argv) {
    int jit = 1, verbose = 0, stats = 0;
    const char *drive = NULL;
    int i = 1;
    for (; i < argc && argv[i][0] == '-'; i++) {
        if (!strcmp(argv[i], "--no-jit")) jit = 0;
        else if (!strcmp(argv[i], "-v")) verbose = 1;
        else if (!strcmp(argv[i], "-vv")) verbose = 2;
        else if (!strcmp(argv[i], "--stats")) stats = 1;
        else if (!strcmp(argv[i], "--profile")) { stats = 1; g_interp_profile = 1; }
        else if (!strcmp(argv[i], "--quiet")) g_quiet_log = 1;
        else if (!strcmp(argv[i], "--drive") && i + 1 < argc) drive = argv[++i];
        else { fprintf(stderr, "bilinmeyen seçenek %s\n", argv[i]); return 2; }
    }
    if (i >= argc) {
        fprintf(stderr, "kullanım: soulrun [--no-jit] [-v] [--stats] [--drive DIR] program.exe [argümanlar...]\n");
        return 2;
    }
    const char *exe = argv[i++];
    char args[4096] = { 0 };
    for (; i < argc; i++) {
        strncat(args, argv[i], sizeof args - strlen(args) - 2);
        if (i + 1 < argc) strcat(args, " ");
    }
    char drivebuf[1024];
    if (!drive) {
        snprintf(drivebuf, sizeof drivebuf, "%s/.soul_drive_c", getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp");
        drive = drivebuf;
    }
    char msg[256];
    if (jit) {
        int ok = soul_jit_init(SOUL_JIT_AUTO, 64u << 20, msg, sizeof msg);
        if (verbose || stats) fprintf(stderr, "[soulrun] JIT: %s (%s)\n", ok ? "hazır" : "yok", msg);
    }
    SoulLaunchOptions o = { 0 };
    o.exe_path = exe;
    o.drive_c = drive;
    o.args = args;
    o.jit_enabled = jit;
    o.verbose = verbose;
    o.output = on_output;
    char err[512];
    if (soul_process_create(&o, &g_p, err, sizeof err) != 0) {
        fprintf(stderr, "[soulrun] hata: %s\n", err);
        return 1;
    }
    pthread_t t;
    pthread_create(&t, NULL, stdin_pump, NULL);
    pthread_detach(t);
    int code = soul_process_run(g_p);
    if (stats) {
        SoulStats s;
        soul_process_stats(g_p, &s);
        fprintf(stderr, "[soulrun] çıkış=%d süre=%.3fs jit_blok=%llu jit_komut=%llu yorum_komut=%llu api=%llu kod=%lluKB\n",
                code, s.seconds, (unsigned long long)s.jit_blocks, (unsigned long long)s.jit_insns,
                (unsigned long long)s.interp_insns, (unsigned long long)s.api_calls, (unsigned long long)(s.jit_code_bytes / 1024));
    }
    if (g_interp_profile) {
        for (int k = 0; k < 15; k++) {
            int best = -1;
            for (int m = 0; m < 4 * 256; m++) if (g_interp_hist[m] && (best < 0 || g_interp_hist[m] > g_interp_hist[best])) best = m;
            if (best < 0) break;
            fprintf(stderr, "  yorumlanan %s%02x: %llu\n", best >= 256 ? (best >= 512 ? "0f38/3a " : "0f ") : "", best & 0xFF, (unsigned long long)g_interp_hist[best]);
            g_interp_hist[best] = 0;
        }
    }
    soul_process_destroy(g_p);
    return code;
}
