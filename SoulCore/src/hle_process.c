// SoulCore - starting other programs: CreateProcess, ShellExecute for programs and documents,
// process handles (wait, exit code, terminate, OpenProcess).
#include "hle.h"
#include <ctype.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>
#include <poll.h>
#include <sys/ioctl.h>

#define CSTR(n) ((const char *)G2H(hle_arg(st, (n))))
#define WSTR(n) ((const uint16_t *)G2H(hle_arg(st, (n))))
#define FAIL(err) do { set_last_error(st, (err)); RET(0); return; } while (0)
#define ERROR_BAD_EXE_FORMAT 193

const char *env_get_value(const char *name); // hle_kernel32.c

static char *gstr(uint64_t p, int wide) {
    if (!p) return NULL;
    return wide ? w2u_dup((const uint16_t *)G2H(p)) : strdup((const char *)G2H(p));
}

static int host_file(const char *win, char *host, size_t cap) {
    char norm[1200];
    path_normalize_win(win, norm, sizeof norm);
    path_w2h(norm, host, cap);
    struct stat sb;
    return stat(host, &sb) == 0 && S_ISREG(sb.st_mode);
}

// Finds a program like CreateProcess does: a path is taken relative to the current folder;
// a bare name is searched in the program's folder, the current folder, system32, the Windows
// folder and PATH. ".exe" is added when the name has no extension.
int proc_resolve_exe(const char *name, char *host, size_t hcap, char *win, size_t wcap) {
    char n[1200];
    snprintf(n, sizeof n, "%s", name);
    const char *bs = strrchr(n, '\\');
    const char *base = bs ? bs + 1 : n;
    if (!strchr(base, '.')) strncat(n, ".exe", sizeof n - strlen(n) - 1);
    if (strchr(n, '\\') || strchr(n, '/') || strchr(n, ':')) {
        if (!host_file(n, host, hcap)) return 0;
        path_normalize_win(n, win, wcap);
        return 1;
    }
    char dirs[4096], exe_dir[600];
    snprintf(exe_dir, sizeof exe_dir, "%s", g_proc->exe_win);
    char *b = strrchr(exe_dir, '\\');
    if (b) *b = 0;
    const char *path = env_get_value("PATH");
    snprintf(dirs, sizeof dirs, "%s;%s;C:\\windows\\system32;C:\\windows;%s", exe_dir, g_proc->cwd_win, path ? path : "");
    for (char *d = strtok(dirs, ";"); d; d = strtok(NULL, ";")) {
        if (!*d) continue;
        char cand[1400];
        snprintf(cand, sizeof cand, "%s\\%s", d, n);
        if (host_file(cand, host, hcap)) { path_normalize_win(cand, win, wcap); return 1; }
    }
    return 0;
}

// Windows tools Soul does not have as files: they are accepted and finish at once.
static int builtin_tool(const char *name) {
    const char *bs = strrchr(name, '\\');
    const char *b = bs ? bs + 1 : name;
    static const char *const tools[] = { "explorer", "regsvr32", "rundll32", "cmd", "taskkill", "schtasks", "sc",
                                         "netsh", "reg", "attrib", "icacls", "takeown", "ngen", "regasm", "wevtutil", "ie4uinit", NULL };
    for (int i = 0; tools[i]; i++) {
        size_t l = strlen(tools[i]);
        if (!strncasecmp(b, tools[i], l) && (b[l] == 0 || !strcasecmp(b + l, ".exe"))) return 1;
    }
    return 0;
}

// The program named at the start of a command line (quoted, or the longest prefix of unquoted
// words that names an existing program, like Windows' search for "C:\Program Files\x.exe").
static int program_of_cmdline(const char *cmd, char *host, size_t hcap, char *win, size_t wcap, char *name, size_t ncap) {
    while (*cmd == ' ' || *cmd == '\t') cmd++;
    if (*cmd == '"') {
        const char *e = strchr(cmd + 1, '"');
        size_t l = e ? (size_t)(e - cmd - 1) : strlen(cmd + 1);
        snprintf(name, ncap, "%.*s", (int)l, cmd + 1);
        return proc_resolve_exe(name, host, hcap, win, wcap);
    }
    size_t len = strlen(cmd);
    for (size_t i = 0; i <= len; i++) {
        if (i < len && cmd[i] != ' ' && cmd[i] != '\t') continue;
        snprintf(name, ncap, "%.*s", (int)i, cmd);
        if (proc_resolve_exe(name, host, hcap, win, wcap)) return 1;
    }
    size_t l = strcspn(cmd, " \t");
    snprintf(name, ncap, "%.*s", (int)l, cmd);
    return 0;
}

// A process object for a tool that "ran" without doing anything.
static SoulProcess *finished_process(void) {
    static SoulProcess done;
    done.exited = 1;
    done.exit_code = 0;
    done.main_thread.finished = 1;
    done.pid = 4;
    return &done;
}

// Starts a program. Returns the new process, or NULL with the Win32 error in *err.
static SoulProcess *start_process(const char *app, const char *cmdline, const char *cwd, char **env, int suspended,
                                  const int fds[3], uint32_t *err) {
    char host[2048], win[1200], name[1200];
    int found;
    if (app && *app) { snprintf(name, sizeof name, "%s", app); found = proc_resolve_exe(app, host, sizeof host, win, sizeof win); }
    else found = program_of_cmdline(cmdline ? cmdline : "", host, sizeof host, win, sizeof win, name, sizeof name);
    if (!found) {
        if (builtin_tool(name)) {
            soul_log("[Soul] '%s' çalıştırılmadı (Soul'da gerekmiyor): %s\n", name, cmdline ? cmdline : "");
            for (int i = 0; i < 3; i++) if (fds[i] >= 0) close(fds[i]);
            return finished_process();
        }
        soul_log("[Soul] Program bulunamadı: %s\n", name);
        *err = ERROR_FILE_NOT_FOUND;
        for (int i = 0; i < 3; i++) if (fds[i] >= 0) close(fds[i]);
        return NULL;
    }
    int m = pe_machine(host);
    if (m != 0x14C && m != 0x8664) {
        soul_log("[Soul] '%s' bir Windows programı değil (veya desteklenmeyen bir mimari)\n", win);
        *err = ERROR_BAD_EXE_FORMAT;
        for (int i = 0; i < 3; i++) if (fds[i] >= 0) close(fds[i]);
        return NULL;
    }
    char cmd[2048];
    if (cmdline && *cmdline) snprintf(cmd, sizeof cmd, "%s", cmdline);
    else snprintf(cmd, sizeof cmd, "\"%s\"", win);
    char cwd_norm[1200] = "";
    if (cwd && *cwd) path_normalize_win(cwd, cwd_norm, sizeof cwd_norm);
    char **e = env;
    char **own = NULL;
    if (!e) { // the parent's environment
        HleState *h = hs();
        own = calloc((size_t)h->env_count + 1, sizeof(char *));
        for (int i = 0; i < h->env_count; i++) own[i] = strdup(h->env[i]);
        e = own;
    }
    SoulSpawn s = { host, win, cmd, cwd_norm, e, suspended, { fds[0], fds[1], fds[2] } };
    char msg[512];
    SoulProcess *p = soul_process_spawn(&s, msg, sizeof msg);
    if (own) { for (int i = 0; own[i]; i++) free(own[i]); free(own); }
    if (!p) { soul_log("[Soul] %s başlatılamadı: %s\n", win, msg); *err = ERROR_BAD_EXE_FORMAT; return NULL; }
    return p;
}

static uint64_t process_handle(SoulProcess *p) {
    HObj *o;
    uint64_t h = h_alloc(H_PROCESS, &o);
    if (o) o->ptr = p;
    return h;
}

// ---- CreateProcess ------------------------------------------------------------------------------------

// "NAME=value\0...\0\0" in ANSI or UTF-16
static char **env_block(uint64_t p, int wide) {
    if (!p) return NULL;
    int n = 0, cap = 32;
    char **v = calloc((size_t)cap + 1, sizeof(char *));
    for (;;) {
        char *s = gstr(p, wide);
        if (!s || !*s) { free(s); break; }
        if (n == cap) { cap *= 2; v = realloc(v, sizeof(char *) * ((size_t)cap + 1)); }
        v[n++] = s;
        p += wide ? (wstrlen((const uint16_t *)G2H(p)) + 1) * 2 : strlen((const char *)G2H(p)) + 1;
    }
    v[n] = NULL;
    return v;
}
static int dup_handle_fd(uint64_t h) {
    HObj *o = h_get(h);
    if (!o || o->type != H_FILE || o->fd < 0) return -1;
    return dup(o->fd);
}

#define L_STARTUPINFO "4PPP4444444422PPPP"
#define L_PROCINFO "PP44"
static void create_process(X86State *st, int wide) {
    char *app = gstr(ARG(0), wide), *cmd = gstr(ARG(1), wide);
    uint32_t flags = (uint32_t)hle_arg(st, 5);
    uint64_t envp = hle_arg(st, 6), si = hle_arg(st, 8), pi = hle_arg(st, 9);
    char *cwd = gstr(hle_arg(st, 7), wide);
    char **env = env_block(envp, (flags & 0x400) != 0 || (wide && envp && mem_rd(envp + 1, 1) == 0));
    int fds[3] = { -1, -1, -1 };
    if (si && (gs_get(si, L_STARTUPINFO, 11) & 0x100)) // STARTF_USESTDHANDLES
        for (int i = 0; i < 3; i++) fds[i] = dup_handle_fd(gs_get(si, L_STARTUPINFO, 15 + i));
    uint32_t err = 0;
    SoulProcess *p = start_process(app, cmd, cwd, env, (flags & 4) != 0, fds, &err);
    if (env) { for (int i = 0; env[i]; i++) free(env[i]); free(env); }
    free(app); free(cmd); free(cwd);
    if (!p) FAIL(err);
    if (pi) {
        gs_set(pi, L_PROCINFO, 0, process_handle(p));
        HObj *o;
        uint64_t th = h_alloc(H_THREAD, &o);
        if (o) o->ptr = &p->main_thread;
        gs_set(pi, L_PROCINFO, 1, th);
        gs_set(pi, L_PROCINFO, 2, p->pid);
        gs_set(pi, L_PROCINFO, 3, p->main_thread.tid);
    }
    RET(1);
}
HLE(k32_CreateProcessA) { create_process(st, 0); }
HLE(k32_CreateProcessW) { create_process(st, 1); }
HLE(k32_CreateProcessAsUserW) { // (token, app, cmd, ...): the same without the token
    char *app = gstr(ARG(1), 1), *cmd = gstr(ARG(2), 1), *cwd = gstr(hle_arg(st, 8), 1);
    uint32_t flags = (uint32_t)hle_arg(st, 6);
    int fds[3] = { -1, -1, -1 };
    uint32_t err = 0;
    SoulProcess *p = start_process(app, cmd, cwd, NULL, (flags & 4) != 0, fds, &err);
    free(app); free(cmd); free(cwd);
    if (!p) FAIL(err);
    uint64_t pi = hle_arg(st, 10);
    if (pi) {
        gs_set(pi, L_PROCINFO, 0, process_handle(p));
        HObj *o;
        uint64_t th = h_alloc(H_THREAD, &o);
        if (o) o->ptr = &p->main_thread;
        gs_set(pi, L_PROCINFO, 1, th);
        gs_set(pi, L_PROCINFO, 2, p->pid);
        gs_set(pi, L_PROCINFO, 3, p->main_thread.tid);
    }
    RET(1);
}
HLE(k32_WinExec) {
    int fds[3] = { -1, -1, -1 };
    uint32_t err = 0;
    SoulProcess *p = start_process(NULL, CSTR(0), NULL, NULL, 0, fds, &err);
    RET(p ? 33 : 2);
}

// ---- process handles ---------------------------------------------------------------------------------

static SoulProcess *proc_of(uint64_t h) {
    if (h == (uint64_t)-1) return g_proc;
    HObj *o = h_get(h);
    return o && o->type == H_PROCESS ? o->ptr : NULL;
}
HLE(k32_GetExitCodeProcess) {
    SoulProcess *p = proc_of(ARG(0));
    if (!p) FAIL(ERROR_INVALID_HANDLE);
    if (ARG(1)) mem_wr(ARG(1), 4, p != g_proc && p->exited ? (uint32_t)p->exit_code : 259);
    RET(1);
}
HLE(k32_GetCurrentProcessId) { RET(g_proc->pid); }
HLE(k32_GetProcessId) { SoulProcess *p = proc_of(ARG(0)); RET(p ? p->pid : 0); }
HLE(k32_TerminateProcess) {
    SoulProcess *p = proc_of(ARG(0));
    if (!p) FAIL(ERROR_INVALID_HANDLE);
    if (p == g_proc) { crt_flush_all(); soul_exit_process(st, (int)ARG(1)); }
    soul_process_kill(p, (int)ARG(1));
    RET(1);
}
HLE(k32_OpenProcess) {
    uint32_t pid = (uint32_t)ARG(2);
    SoulProcess *p = pid == g_proc->pid ? g_proc : soul_process_by_pid(pid);
    if (!p || (p != g_proc && p->exited && p->released)) FAIL(ERROR_INVALID_PARAMETER);
    RET(process_handle(p));
}
HLE(u_WaitForInputIdle) { RET(0); }
static void image_name(X86State *st, int wide, uint64_t buf, uint64_t cap_ptr, uint32_t cap_val, int by_ptr) {
    SoulProcess *p = proc_of(ARG(0));
    if (!p) FAIL(ERROR_INVALID_HANDLE);
    uint32_t cap = by_ptr ? (uint32_t)mem_rd(cap_ptr, 4) : cap_val;
    const char *s = p->exe_win;
    size_t n = wide ? u2w(s, -1, NULL, 0) : strlen(s);
    if (n + 1 > cap) FAIL(ERROR_INSUFFICIENT_BUFFER);
    if (wide) u2w(s, -1, (uint16_t *)G2H(buf), cap); else memcpy(G2H(buf), s, n + 1);
    if (by_ptr) mem_wr(cap_ptr, 4, n);
    RET(by_ptr ? 1 : n);
}
HLE(k32_QueryFullProcessImageNameA) { image_name(st, 0, ARG(2), ARG(3), 0, 1); }
HLE(k32_QueryFullProcessImageNameW) { image_name(st, 1, ARG(2), ARG(3), 0, 1); }
HLE(k32_GetProcessImageFileNameA) { image_name(st, 0, ARG(1), 0, (uint32_t)ARG(2), 0); }
HLE(k32_GetProcessImageFileNameW) { image_name(st, 1, ARG(1), 0, (uint32_t)ARG(2), 0); }
HLE(k32_GetModuleFileNameExW) { // (process, module, buf, size)
    SoulProcess *p = proc_of(ARG(0));
    if (!p) FAIL(ERROR_INVALID_HANDLE);
    if (p == g_proc && ARG(1)) FAIL(ERROR_INVALID_HANDLE);
    RET(u2w(p->exe_win, -1, (uint16_t *)PTR(2), (uint32_t)ARG(3)));
}

// ---- ShellExecute ------------------------------------------------------------------------------------

int soul_lnk_read(const char *host_path, SoulLnkInfo *out); // hle_com.c

// Opens a file the way Explorer would. Returns > 32 on success (like ShellExecute) and the
// started process in *pp (NULL when nothing runs in Soul).
uint64_t shell_execute_file(const char *file, const char *params, const char *dir, SoulProcess **pp) {
    *pp = NULL;
    if (!file || !*file) return 2;
    if (!strncasecmp(file, "http://", 7) || !strncasecmp(file, "https://", 8) || !strncasecmp(file, "mailto:", 7)) {
        if (g_proc->opts.open_url) g_proc->opts.open_url(g_proc->opts.ctx, file);
        else soul_log("[Soul] Bağlantı: %s\n", file);
        return 42;
    }
    char path[1200];
    snprintf(path, sizeof path, "%s", file);
    if (dir && *dir && !strchr(file, ':') && file[0] != '\\') snprintf(path, sizeof path, "%s\\%s", dir, file);
    char host[2048], win[1200];
    const char *dot = strrchr(path, '.');
    const char *bs = strrchr(path, '\\');
    if (bs && dot && dot < bs) dot = NULL;
    int fds[3] = { -1, -1, -1 };
    uint32_t err = 0;
    if (!dot || !strcasecmp(dot, ".exe") || !strcasecmp(dot, ".com")) {
        if (proc_resolve_exe(path, host, sizeof host, win, sizeof win) || builtin_tool(path)) {
            char cmd[2400];
            snprintf(cmd, sizeof cmd, "\"%s\"%s%s", proc_resolve_exe(path, host, sizeof host, win, sizeof win) ? win : path,
                     params && *params ? " " : "", params ? params : "");
            SoulProcess *p = start_process(NULL, cmd, dir, NULL, 0, fds, &err);
            if (!p) return err == ERROR_FILE_NOT_FOUND ? 2 : 11;
            *pp = p;
            return 42;
        }
    }
    char norm[1200];
    path_normalize_win(path, norm, sizeof norm);
    path_w2h(norm, host, sizeof host);
    struct stat sb;
    if (stat(host, &sb) != 0) { soul_log("[api] ShellExecute: '%s' bulunamadı\n", path); return 2; }
    if (S_ISDIR(sb.st_mode)) { soul_log("[Soul] Klasör açılmak istendi: %s\n", norm); return 42; }
    if (dot && !strcasecmp(dot, ".msi")) { // Windows Installer packages: Soul's msiexec wizard
        char cmd[2400], pw[1300];
        snprintf(pw, sizeof pw, "%s", norm);
        snprintf(cmd, sizeof cmd, "\"C:\\windows\\system32\\msiexec.exe\" /i \"%s\"%s%s", pw, params && *params ? " " : "", params ? params : "");
        SoulProcess *p = start_process(NULL, cmd, dir, NULL, 0, fds, &err);
        if (!p) return err == ERROR_FILE_NOT_FOUND ? 2 : 11;
        *pp = p;
        return 42;
    }
    if (dot && !strcasecmp(dot, ".lnk")) {
        SoulLnkInfo li;
        if (soul_lnk_read(host, &li) == 0 && li.target[0]) return shell_execute_file(li.target, li.arguments, li.workdir, pp);
        return 2;
    }
    // documents: the iPad shows them (Quick Look / the browser)
    if (g_proc->opts.open_url) {
        char url[2200];
        snprintf(url, sizeof url, "file://%s", host);
        g_proc->opts.open_url(g_proc->opts.ctx, url);
        return 42;
    }
    soul_log("[Soul] Belge: %s\n", norm);
    return 42;
}

// ---- anonymous pipes ------------------------------------------------------------------------------------

HLE(k32_CreatePipe) {
    int fds[2];
    if (pipe(fds) != 0) FAIL(ERROR_NOT_ENOUGH_MEMORY);
    HObj *o;
    uint64_t r = h_alloc(H_FILE, &o);
    if (o) o->fd = fds[0];
    uint64_t w = h_alloc(H_FILE, &o);
    if (o) o->fd = fds[1];
    wr_ptr(ARG(0), r);
    wr_ptr(ARG(1), w);
    RET(1);
}
HLE(k32_PeekNamedPipe) { // (pipe, buffer, size, read, available, left): the data stays in the pipe
    HObj *o = h_get(ARG(0));
    if (!o || o->type != H_FILE || o->fd < 0) FAIL(ERROR_INVALID_HANDLE);
    int n = 0;
    ioctl(o->fd, FIONREAD, &n);
    if (n == 0) { // no data: tell a closed pipe apart from an empty one
        struct pollfd pf = { o->fd, POLLIN, 0 };
        if (poll(&pf, 1, 0) > 0 && (pf.revents & POLLHUP)) FAIL(ERROR_BROKEN_PIPE);
    }
    if (ARG(3)) mem_wr(ARG(3), 4, 0);
    if (hle_arg(st, 4)) mem_wr(hle_arg(st, 4), 4, (uint32_t)n);
    if (hle_arg(st, 5)) mem_wr(hle_arg(st, 5), 4, 0);
    RET(1);
}
HLE(k32_SetHandleInformation) { RET(1); }

const HleEntry hle_process_table[] = {
    { "CreatePipe", k32_CreatePipe, NULL }, { "PeekNamedPipe", k32_PeekNamedPipe, NULL }, { "SetHandleInformation", k32_SetHandleInformation, NULL },
    { "CreateProcessA", k32_CreateProcessA, NULL }, { "CreateProcessW", k32_CreateProcessW, NULL },
    { "CreateProcessAsUserW", k32_CreateProcessAsUserW, NULL }, { "WinExec", k32_WinExec, NULL },
    { "GetExitCodeProcess", k32_GetExitCodeProcess, NULL }, { "GetCurrentProcessId", k32_GetCurrentProcessId, NULL },
    { "GetProcessId", k32_GetProcessId, NULL }, { "TerminateProcess", k32_TerminateProcess, NULL }, { "OpenProcess", k32_OpenProcess, NULL },
    { "WaitForInputIdle", u_WaitForInputIdle, NULL },
    { "QueryFullProcessImageNameA", k32_QueryFullProcessImageNameA, NULL }, { "QueryFullProcessImageNameW", k32_QueryFullProcessImageNameW, NULL },
    { "GetProcessImageFileNameA", k32_GetProcessImageFileNameA, NULL }, { "GetProcessImageFileNameW", k32_GetProcessImageFileNameW, NULL },
    { "K32GetProcessImageFileNameA", k32_GetProcessImageFileNameA, NULL }, { "K32GetProcessImageFileNameW", k32_GetProcessImageFileNameW, NULL },
    { "GetModuleFileNameExW", k32_GetModuleFileNameExW, NULL }, { "K32GetModuleFileNameExW", k32_GetModuleFileNameExW, NULL },
    { NULL, NULL, NULL }
};
