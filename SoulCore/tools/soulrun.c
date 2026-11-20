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

// ---- GUI scripting (tests): drives windows like the iPad UI would -------------------------------
static const char *g_script;
static int g_script_failed;

static int top_windows(SoulWindowInfo *w, int max) { return soul_gui_windows(g_p, w, max); }

static int pick(SoulWindowInfo *w, int n, const char *sel) { // window by index ("-1" = front) or title prefix
    if (n <= 0) return -1;
    if (!sel || !*sel) return n - 1;
    char *end;
    long i = strtol(sel, &end, 10);
    if (*end == 0) return i < 0 ? (int)(n + i) : (i < n ? (int)i : -1);
    for (int k = n - 1; k >= 0; k--) if (strncmp(w[k].title, sel, strlen(sel)) == 0) return k;
    return -1;
}

static void dump_desktop(const char *path) {
    SoulWindowInfo w[32];
    int n = top_windows(w, 32);
    if (n <= 0) { fprintf(stderr, "[script] dump: pencere yok\n"); return; }
    int x0 = w[0].x, y0 = w[0].y, x1 = w[0].x + w[0].width, y1 = w[0].y + w[0].height;
    for (int i = 1; i < n; i++) {
        if (w[i].x < x0) x0 = w[i].x;
        if (w[i].y < y0) y0 = w[i].y;
        if (w[i].x + w[i].width > x1) x1 = w[i].x + w[i].width;
        if (w[i].y + w[i].height > y1) y1 = w[i].y + w[i].height;
    }
    int W = x1 - x0 + 16, H = y1 - y0 + 16;
    uint32_t *img = malloc((size_t)W * (size_t)H * 4);
    for (int i = 0; i < W * H; i++) img[i] = 0xFF3A4A5Au;
    for (int i = 0; i < n; i++) {
        uint32_t *px = malloc((size_t)w[i].width * (size_t)w[i].height * 4);
        uint64_t ver = (uint64_t)-1;
        soul_gui_copy(g_p, w[i].hwnd, px, (size_t)w[i].width * 4, w[i].width, w[i].height, &ver);
        for (int y = 0; y < w[i].height; y++) for (int x = 0; x < w[i].width; x++) {
            int dx = w[i].x - x0 + 8 + x, dy = w[i].y - y0 + 8 + y;
            if (dx >= 0 && dy >= 0 && dx < W && dy < H) img[(size_t)dy * W + dx] = px[(size_t)y * w[i].width + x];
        }
        free(px);
    }
    FILE *f = fopen(path, "wb");
    if (f) {
        fprintf(f, "P6\n%d %d\n255\n", W, H);
        for (int i = 0; i < W * H; i++) { unsigned char c[3] = { (unsigned char)(img[i] >> 16), (unsigned char)(img[i] >> 8), (unsigned char)img[i] }; fwrite(c, 1, 3, f); }
        fclose(f);
    }
    free(img);
    fprintf(stderr, "[script] %s yazıldı (%dx%d)\n", path, W, H);
}

// waits until the program has handled all input and repainted (or 10 s pass)
static void settle(void) {
    int calm = 0;
    for (int k = 0; k < 500 && calm < 3; k++) {
        usleep(10000);
        calm = soul_gui_idle(g_p) ? calm + 1 : 0;
    }
}

static void *script_main(void *arg) {
    (void)arg;
    FILE *f = fopen(g_script, "r");
    if (!f) { fprintf(stderr, "[script] açılamadı: %s\n", g_script); soul_process_terminate(g_p); return NULL; }
    char line[1024];
    SoulWindowInfo w[32];
    while (fgets(line, sizeof line, f)) {
        line[strcspn(line, "\r\n")] = 0;
        char cmd[32] = "", a[512] = "", b[64] = "", c[64] = "", d[64] = "", e[64] = "";
        if (sscanf(line, "%31s", cmd) != 1 || cmd[0] == '#') continue;
        const char *rest = line + strlen(cmd);
        while (*rest == ' ') rest++;
        sscanf(rest, "%511s %63s %63s %63s %63s", a, b, c, d, e);
        int n = top_windows(w, 32);
        if (!strcmp(cmd, "wait")) { usleep((useconds_t)atoi(a) * 1000); continue; }
        if (!strcmp(cmd, "waitwin")) {
            int want = atoi(a);
            for (int k = 0; k < 2000 && top_windows(w, 32) < want; k++) usleep(10000);
            settle();
            if (top_windows(w, 32) < want) { fprintf(stderr, "[script] HATA: %d pencere beklendi\n", want); g_script_failed = 1; }
            continue;
        }
        if (!strcmp(cmd, "print")) {
            for (int i = 0; i < n; i++) fprintf(stderr, "[script] pencere %d: \"%s\" %dx%d @%d,%d bayrak=%x\n", i, w[i].title, w[i].width, w[i].height, w[i].x, w[i].y, w[i].flags);
            continue;
        }
        if (!strcmp(cmd, "dump")) { settle(); dump_desktop(a); continue; }
        if (!strcmp(cmd, "settle")) { settle(); continue; }
        if (!strcmp(cmd, "quit")) { soul_process_terminate(g_p); break; }
        if (!strcmp(cmd, "popup")) {
            for (int k = 0; k < 1000 && !soul_gui_popup_active(g_p); k++) usleep(10000);
            soul_gui_event(g_p, 0, SOUL_GUI_POPUP_RESULT, 0, 0, atoi(a), 0, NULL);
            settle();
            continue;
        }
        if (!strcmp(cmd, "expect")) { // expect <window-selector> <title prefix>
            int i = pick(w, n, a);
            if (i < 0 || strncmp(w[i].title, rest + strlen(a) + 1, strlen(rest + strlen(a) + 1)) != 0) { fprintf(stderr, "[script] HATA: beklenen başlık \"%s\"\n", rest + strlen(a) + 1); g_script_failed = 1; }
            continue;
        }
        // commands that target a window: last argument may select it
        if (!strcmp(cmd, "click") || !strcmp(cmd, "rclick") || !strcmp(cmd, "dblclick")) {
            int i = pick(w, n, c);
            if (i < 0) { fprintf(stderr, "[script] HATA: pencere yok\n"); g_script_failed = 1; continue; }
            int x = atoi(a), y = atoi(b), btn = cmd[0] == 'r';
            int times = cmd[0] == 'd' ? 2 : 1;
            for (int k = 0; k < times; k++) {
                soul_gui_event(g_p, w[i].hwnd, SOUL_GUI_MOUSE_DOWN, x, y, btn, 0, NULL);
                soul_gui_event(g_p, w[i].hwnd, SOUL_GUI_MOUSE_UP, x, y, btn, 0, NULL);
            }
            settle();
            continue;
        }
        if (!strcmp(cmd, "drag")) { // drag x1 y1 x2 y2 [win]
            int i = pick(w, n, e);
            if (i < 0) continue;
            int x1 = atoi(a), y1 = atoi(b), x2 = atoi(c), y2 = atoi(d);
            soul_gui_event(g_p, w[i].hwnd, SOUL_GUI_MOUSE_DOWN, x1, y1, 0, 0, NULL);
            for (int k = 1; k <= 20; k++) { soul_gui_event(g_p, w[i].hwnd, SOUL_GUI_MOUSE_MOVE, x1 + (x2 - x1) * k / 20, y1 + (y2 - y1) * k / 20, 0, 0, NULL); usleep(5000); }
            soul_gui_event(g_p, w[i].hwnd, SOUL_GUI_MOUSE_UP, x2, y2, 0, 0, NULL);
            settle();
            continue;
        }
        if (!strcmp(cmd, "key")) { // key VK [mods] [win]
            int i = pick(w, n, c);
            if (i < 0) continue;
            int vk = (int)strtol(a, NULL, 0), mods = (int)strtol(b[0] ? b : "0", NULL, 0);
            soul_gui_event(g_p, w[i].hwnd, SOUL_GUI_KEY, 1, mods, vk, 0, NULL);
            soul_gui_event(g_p, w[i].hwnd, SOUL_GUI_KEY, 0, mods, vk, 0, NULL);
            settle();
            continue;
        }
        if (!strcmp(cmd, "text")) {
            int i = pick(w, n, "");
            if (i >= 0) soul_gui_event(g_p, w[i].hwnd, SOUL_GUI_TEXT, 0, 0, 0, 0, rest);
            settle();
            continue;
        }
        if (!strcmp(cmd, "menu")) { int i = pick(w, n, b); if (i >= 0) soul_gui_event(g_p, w[i].hwnd, SOUL_GUI_COMMAND, 0, 0, atoi(a), 0, NULL); settle(); continue; }
        if (!strcmp(cmd, "close")) { int i = pick(w, n, a); if (i >= 0) soul_gui_event(g_p, w[i].hwnd, SOUL_GUI_CLOSE, 0, 0, 0, 0, NULL); settle(); continue; }
        if (!strcmp(cmd, "resize")) { int i = pick(w, n, c); if (i >= 0) soul_gui_event(g_p, w[i].hwnd, SOUL_GUI_RESIZE, atoi(a), atoi(b), 0, 0, NULL); settle(); continue; }
        if (!strcmp(cmd, "menus")) {
            int i = pick(w, n, a);
            SoulMenuEntry m[128];
            int k = i >= 0 ? soul_gui_menu(g_p, w[i].hwnd, m, 128) : 0;
            for (int j = 0; j < k; j++) fprintf(stderr, "[script] menü %*s%s (id=%u, bayrak=%x)\n", m[j].depth * 2, "", m[j].text, m[j].id, m[j].flags);
            continue;
        }
        if (!strcmp(cmd, "pixel")) { // pixel x y RRGGBB [win]
            settle();
            n = top_windows(w, 32);
            int i = pick(w, n, d);
            if (i < 0) { g_script_failed = 1; continue; }
            uint32_t *px = malloc((size_t)w[i].width * (size_t)w[i].height * 4);
            uint64_t ver = (uint64_t)-1;
            soul_gui_copy(g_p, w[i].hwnd, px, (size_t)w[i].width * 4, w[i].width, w[i].height, &ver);
            int x = atoi(a), y = atoi(b);
            uint32_t want = (uint32_t)strtoul(c, NULL, 16), got = x < w[i].width && y < w[i].height ? px[(size_t)y * w[i].width + x] & 0xFFFFFF : 0xFFFFFFFF;
            if (got != want) { fprintf(stderr, "[script] HATA: piksel (%d,%d) = %06X, beklenen %06X\n", x, y, got, want); g_script_failed = 1; }
            free(px);
            continue;
        }
        fprintf(stderr, "[script] bilinmeyen komut: %s\n", cmd);
    }
    fclose(f);
    usleep(100000);
    soul_process_terminate(g_p); // the script is over: end the program (GUI loops never end by themselves)
    return NULL;
}

int main(int argc, char **argv) {
    int jit = 1, verbose = 0, stats = 0, screen_w = 0, screen_h = 0;
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
        else if (!strcmp(argv[i], "--gui-script") && i + 1 < argc) g_script = argv[++i];
        else if (!strcmp(argv[i], "--screen") && i + 1 < argc) { if (sscanf(argv[++i], "%dx%d", &screen_w, &screen_h) != 2) return 2; }
        else { fprintf(stderr, "bilinmeyen seçenek %s\n", argv[i]); return 2; }
    }
    if (i >= argc) {
        fprintf(stderr, "kullanım: soulrun [--no-jit] [-v] [--stats] [--drive DIR] [--gui-script FILE] [--screen WxH] program.exe [argümanlar...]\n");
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
    o.gui_enabled = g_script != NULL;
    char err[512];
    if (soul_process_create(&o, &g_p, err, sizeof err) != 0) {
        fprintf(stderr, "[soulrun] hata: %s\n", err);
        return 1;
    }
    if (screen_w) soul_gui_set_screen(g_p, screen_w, screen_h);
    pthread_t t, script;
    pthread_create(&t, NULL, stdin_pump, NULL);
    pthread_detach(t);
    if (g_script) pthread_create(&script, NULL, script_main, NULL);
    int code = soul_process_run(g_p);
    if (g_script) {
        pthread_join(script, NULL);
        if (g_script_failed) { fprintf(stderr, "[script] BAŞARISIZ\n"); code = 99; }
    }
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
