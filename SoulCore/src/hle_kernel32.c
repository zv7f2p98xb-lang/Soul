// SoulCore - kernel32.dll / kernelbase.dll / ntdll.dll emulation.
#include "hle.h"
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>
#include <wctype.h>
#ifdef __APPLE__
#include <malloc/malloc.h>
#define HOST_MSIZE(p) malloc_size(p)
#else
#include <malloc.h>
#define HOST_MSIZE(p) malloc_usable_size(p)
#endif

#define CSTR(n) ((const char *)G2H(hle_arg(st, (n))))
#define WSTR(n) ((const uint16_t *)G2H(hle_arg(st, (n))))
#define FAIL(err) do { set_last_error(st, (err)); RET(0); return; } while (0)
#define INVALID_HANDLE ((uint64_t)-1)

const char *env_get_value(const char *name);
void env_set_value(const char *name, const char *value);

// ---- helpers for A/W variants ------------------------------------------------------------

static char *arg_str(X86State *st, int n, int wide) {
    uint64_t p = hle_arg(st, n);
    if (!p) return NULL;
    return wide ? w2u_dup((const uint16_t *)G2H(p)) : strdup((const char *)G2H(p));
}

// Copy a UTF-8 string into a guest A/W buffer with Win32 "returns length or required size" semantics.
static uint64_t put_str(uint64_t buf, uint64_t cap, const char *s, int wide) {
    if (wide) {
        size_t n = u2w(s, -1, NULL, 0);
        if (!buf || n + 1 > cap) return n + 1;
        u2w(s, -1, (uint16_t *)G2H(buf), cap);
        return n;
    }
    size_t n = strlen(s);
    if (!buf || n + 1 > cap) return n + 1;
    memcpy(G2H(buf), s, n + 1);
    return n;
}

static uint32_t cur_tid_k32(void) { SoulThread *t = soul_cur_thread(); return t ? t->tid : 4243; }

// ---- console / std handles ------------------------------------------------------------------

HLE(k32_GetStdHandle) {
    int64_t which = (int32_t)ARG(0);
    if (which == -10) RET(h_std(0));
    else if (which == -11) RET(h_std(1));
    else if (which == -12) RET(h_std(2));
    else RET(INVALID_HANDLE);
}
HLE(k32_SetStdHandle) { RET(1); }

static int handle_is_console(uint64_t h, int *stream) {
    HObj *o = h_get(h);
    if (!o) return 0;
    if (o->type == H_CONOUT) { *stream = 1; return 1; }
    if (o->type == H_CONERR) { *stream = 2; return 1; }
    if (o->type == H_CONIN) { *stream = 0; return 1; }
    return 0;
}

// Pipes between processes: wait for data / room without holding the global lock, so the
// program on the other end can run.
static int is_fifo(int fd) { struct stat sb; return fstat(fd, &sb) == 0 && S_ISFIFO(sb.st_mode); }
static void pipe_wait(int fd, short ev) {
    for (;;) {
        struct pollfd pf = { fd, ev, 0 };
        if (poll(&pf, 1, 0) != 0 || g_proc->terminate) return;
        SoulThread *t = soul_cur_thread();
        gil_unlock(t);
        poll(&pf, 1, 50);
        gil_lock(t);
    }
}

HLE(k32_WriteFile) {
    uint64_t h = ARG(0), buf = ARG(1), n = (uint32_t)ARG(2), written = ARG(3);
    HObj *o = h_get(h);
    if (!o) FAIL(ERROR_INVALID_HANDLE);
    int64_t w;
    if (o->type == H_CONOUT || o->type == H_CONERR) {
        con_write(o->type == H_CONOUT ? 1 : 2, (const char *)G2H(buf), n);
        w = (int64_t)n;
    } else if (o->type == H_NULLDEV) {
        w = (int64_t)n;
    } else if (o->type == H_FILE) {
        if (o->append) lseek(o->fd, 0, SEEK_END);
        if (is_fifo(o->fd)) {
            size_t done = 0;
            while (done < n) {
                pipe_wait(o->fd, POLLOUT);
                ssize_t k = write(o->fd, (const uint8_t *)G2H(buf) + done, n - done > 512 ? 512 : n - done);
                if (k <= 0) break;
                done += (size_t)k;
            }
            if (done == 0 && n) { if (written) mem_wr(written, 4, 0); FAIL(ERROR_BROKEN_PIPE); }
            w = (int64_t)done;
        } else
        w = write(o->fd, (const void *)G2H(buf), n);
        if (w < 0) { if (written) mem_wr(written, 4, 0); FAIL(errno_to_win(errno)); }
    } else FAIL(ERROR_INVALID_HANDLE);
    if (written) mem_wr(written, 4, (uint64_t)w);
    RET(1);
}

HLE(k32_ReadFile) {
    uint64_t h = ARG(0), buf = ARG(1), n = (uint32_t)ARG(2), rd = ARG(3);
    HObj *o = h_get(h);
    if (!o) FAIL(ERROR_INVALID_HANDLE);
    int64_t r;
    if (o->type == H_CONIN) {
        // raw console reads deliver CRLF line endings
        size_t want = n > 1 ? n - 1 : n;
        r = (int64_t)con_read((char *)G2H(buf), want, 1);
        char *b = (char *)G2H(buf);
        if (r > 0 && b[r - 1] == '\n' && (uint64_t)r < n) { b[r - 1] = '\r'; b[r] = '\n'; r++; }
    } else if (o->type == H_NULLDEV) {
        r = 0;
    } else if (o->type == H_FILE) {
        int fifo = is_fifo(o->fd);
        if (fifo) pipe_wait(o->fd, POLLIN);
        r = read(o->fd, G2H(buf), n);
        if (r < 0) { if (rd) mem_wr(rd, 4, 0); FAIL(errno_to_win(errno)); }
        if (fifo && r == 0 && n) { if (rd) mem_wr(rd, 4, 0); FAIL(ERROR_BROKEN_PIPE); } // every writer closed its end
    } else FAIL(ERROR_INVALID_HANDLE);
    if (rd) mem_wr(rd, 4, (uint64_t)r);
    RET(1);
}

static void write_console(X86State *st, int wide) {
    int stream;
    if (!handle_is_console(ARG(0), &stream) || stream == 0) FAIL(ERROR_INVALID_HANDLE);
    uint32_t n = (uint32_t)ARG(2);
    if (wide) {
        char *s = malloc((size_t)n * 3 + 4);
        size_t l = w2u((const uint16_t *)PTR(1), n, s, (size_t)n * 3 + 4);
        con_write(stream, s, l);
        free(s);
    } else {
        con_write(stream, (const char *)PTR(1), n);
    }
    if (ARG(3)) mem_wr(ARG(3), 4, n);
    RET(1);
}
HLE(k32_WriteConsoleA) { write_console(st, 0); }
HLE(k32_WriteConsoleW) { write_console(st, 1); }

static void read_console(X86State *st, int wide) {
    uint32_t n = (uint32_t)ARG(2);
    char tmp[4096];
    size_t max = n < sizeof tmp - 2 ? n : sizeof tmp - 2;
    if (max > 1) max--;
    size_t r = con_read(tmp, max, 1);
    if (r && tmp[r - 1] == '\n') { tmp[r - 1] = '\r'; tmp[r++] = '\n'; }
    tmp[r] = 0;
    uint32_t units;
    if (wide) units = (uint32_t)u2w(tmp, (long)r, (uint16_t *)PTR(1), n);
    else { memcpy(PTR(1), tmp, r); units = (uint32_t)r; }
    if (ARG(3)) mem_wr(ARG(3), 4, units);
    RET(1);
}
HLE(k32_ReadConsoleA) { read_console(st, 0); }
HLE(k32_ReadConsoleW) { read_console(st, 1); }

HLE(k32_ReadConsoleInputW) {
    // Deliver one KEY_EVENT (key down) per call. INPUT_RECORD is 20 bytes.
    uint8_t *rec = PTR(1);
    int c = con_getc();
    if (c < 0) { if (ARG(3)) mem_wr(ARG(3), 4, 0); RET(1); return; }
    if (c == '\n') c = '\r';
    memset(rec, 0, 20);
    uint16_t type = 1; // KEY_EVENT
    memcpy(rec, &type, 2);
    uint32_t down = 1; uint16_t rep = 1, vk = (uint16_t)toupper(c), sc = 0, ch = (uint16_t)c;
    if (c == '\r') vk = 0x0D;
    memcpy(rec + 4, &down, 4); memcpy(rec + 8, &rep, 2); memcpy(rec + 10, &vk, 2); memcpy(rec + 12, &sc, 2); memcpy(rec + 14, &ch, 2);
    if (ARG(3)) mem_wr(ARG(3), 4, 1);
    RET(1);
}
HLE(k32_PeekConsoleInputW) { if (ARG(3)) mem_wr(ARG(3), 4, 0); RET(1); }
HLE(k32_GetNumberOfConsoleInputEvents) { mem_wr(ARG(1), 4, con_has_input() ? 1 : 0); RET(1); }
HLE(k32_GetConsoleMode) {
    int stream;
    if (!handle_is_console(ARG(0), &stream)) FAIL(ERROR_INVALID_HANDLE);
    mem_wr(ARG(1), 4, stream == 0 ? 0x1F7 : 0x7);
    RET(1);
}
HLE_STUB_RET(k32_SetConsoleMode, 1)
HLE(k32_SetConsoleTitleA) { snprintf(hs()->con_title, sizeof hs()->con_title, "%s", CSTR(0)); RET(1); }
HLE(k32_SetConsoleTitleW) { w2u(WSTR(0), -1, hs()->con_title, sizeof hs()->con_title); RET(1); }
HLE(k32_GetConsoleTitleA) { RET(put_str(ARG(0), (uint32_t)ARG(1), hs()->con_title, 0)); }
HLE(k32_GetConsoleTitleW) { RET(put_str(ARG(0), (uint32_t)ARG(1), hs()->con_title, 1)); }
HLE(k32_SetConsoleTextAttribute) {
    uint16_t a = (uint16_t)ARG(1);
    static const int map[8] = { 0, 4, 2, 6, 1, 5, 3, 7 }; // BGR -> ANSI
    int fg = map[a & 7] + ((a & 8) ? 90 : 30);
    int bg = map[(a >> 4) & 7] + ((a & 0x80) ? 100 : 40);
    char seq[32];
    int n = (a == 7) ? snprintf(seq, sizeof seq, "\x1b[0m") : snprintf(seq, sizeof seq, "\x1b[%d;%dm", fg, bg);
    int stream;
    if (a != hs()->con_attr && handle_is_console(ARG(0), &stream) && stream) con_write(stream, seq, (size_t)n);
    hs()->con_attr = a;
    RET(1);
}
HLE(k32_GetConsoleScreenBufferInfo) {
    int stream;
    if (!handle_is_console(ARG(0), &stream)) FAIL(ERROR_INVALID_HANDLE);
    uint8_t *b = PTR(1);
    int16_t v[11] = { 120, 300, 0, 0, (int16_t)hs()->con_attr, 0, 0, 119, 29, 120, 300 };
    memcpy(b, v, 22);
    RET(1);
}
HLE(k32_GetConsoleScreenBufferInfoEx) { RET(0); }
HLE(k32_SetConsoleCursorPosition) {
    uint32_t c = (uint32_t)ARG(1);
    int x = (int16_t)(c & 0xFFFF), y = (int16_t)(c >> 16);
    char seq[32];
    int n = snprintf(seq, sizeof seq, "\x1b[%d;%dH", y + 1, x + 1);
    con_write(1, seq, (size_t)n);
    RET(1);
}
HLE(k32_FillConsoleOutputCharacter) {
    uint32_t len = (uint32_t)ARG(2), coord = (uint32_t)ARG(3);
    if (coord == 0 && len >= 80 * 20) con_write(1, "\x1b[2J\x1b[H", 7);
    if (ARG(4)) mem_wr(ARG(4), 4, len);
    RET(1);
}
HLE(k32_FillConsoleOutputAttribute) { if (ARG(4)) mem_wr(ARG(4), 4, (uint32_t)ARG(2)); RET(1); }
HLE(k32_GetConsoleCursorInfo) { mem_wr(ARG(1), 4, 25); mem_wr(ARG(1) + 4, 4, 1); RET(1); }
HLE_STUB_RET(k32_SetConsoleCursorInfo, 1)
HLE(k32_GetConsoleCP) { RET(65001); }
HLE_STUB_RET(k32_SetConsoleCP, 1)
HLE_STUB_RET(k32_SetConsoleCtrlHandler, 1)
HLE_STUB_RET(k32_FlushConsoleInputBuffer, 1)
HLE_STUB_RET(k32_GetConsoleWindow, 0)
HLE_STUB_RET(k32_AllocConsole, 1)
HLE_STUB_RET(k32_FreeConsole, 1)
HLE_STUB_RET(k32_AttachConsole, 1)
HLE_STUB_RET(k32_SetConsoleScreenBufferSize, 1)
HLE_STUB_RET(k32_SetConsoleWindowInfo, 1)
HLE(k32_GetLargestConsoleWindowSize) { RET((uint64_t)((300u << 16) | 120u)); }
HLE(k32_WriteConsoleOutputW) { RET(1); }
HLE(k32_GetFileType) {
    HObj *o = h_get(ARG(0));
    if (!o) { set_last_error(st, ERROR_INVALID_HANDLE); RET(0); return; }
    if (o->type == H_CONIN || o->type == H_CONOUT || o->type == H_CONERR || o->type == H_NULLDEV) RET(2);
    else RET(1);
}

// ---- files -------------------------------------------------------------------------------------

static uint64_t create_file(X86State *st, const char *path, uint32_t access, uint32_t disp, uint32_t flags) {
    if (strcasecmp(path, "CONOUT$") == 0) return h_std(1);
    if (strcasecmp(path, "CONIN$") == 0) return h_std(0);
    if (strcasecmp(path, "NUL") == 0 || strcasecmp(path, "\\\\.\\NUL") == 0) { HObj *o; return h_alloc(H_NULLDEV, &o); }
    char host[2048];
    path_w2h(path, host, sizeof host);
    struct stat sb;
    int exists = stat(host, &sb) == 0;
    if (exists && S_ISDIR(sb.st_mode)) {
        if (!(flags & 0x02000000)) { set_last_error(st, ERROR_ACCESS_DENIED); return INVALID_HANDLE; } // FILE_FLAG_BACKUP_SEMANTICS
        HObj *o;
        uint64_t h = h_alloc(H_DIR, &o);
        snprintf(o->path, sizeof o->path, "%s", host);
        return h;
    }
    int fl;
    int wr = (access & 0x40000000) || (access & 0x10000000) || (access & 0x2) || (access & 0x4); // GENERIC_WRITE / ALL / FILE_WRITE_DATA / APPEND
    int rd = (access & 0x80000000) || (access & 0x10000000) || (access & 0x1) || !wr;
    fl = (rd && wr) ? O_RDWR : wr ? O_WRONLY : O_RDONLY;
    switch (disp) {
    case 1: if (exists) { set_last_error(st, ERROR_FILE_EXISTS); return INVALID_HANDLE; } fl |= O_CREAT; break; // CREATE_NEW
    case 2: fl |= O_CREAT | O_TRUNC; break;                                                                  // CREATE_ALWAYS
    case 3: if (!exists) { set_last_error(st, ERROR_FILE_NOT_FOUND); return INVALID_HANDLE; } break;          // OPEN_EXISTING
    case 4: fl |= O_CREAT; break;                                                                            // OPEN_ALWAYS
    case 5: if (!exists) { set_last_error(st, ERROR_FILE_NOT_FOUND); return INVALID_HANDLE; } fl |= O_TRUNC; break;
    default: set_last_error(st, ERROR_INVALID_PARAMETER); return INVALID_HANDLE;
    }
    if ((fl & O_TRUNC) && !(fl & (O_WRONLY | O_RDWR))) fl = (fl & ~O_RDONLY) | O_RDWR;
    int fd = open(host, fl, 0644);
    if (fd < 0) {
        uint32_t e = errno_to_win(errno);
        if (errno == ENOENT) {
            // distinguish missing directory
            char dir[2048];
            snprintf(dir, sizeof dir, "%s", host);
            char *sl = strrchr(dir, '/');
            if (sl) { *sl = 0; if (stat(dir, &sb) != 0) e = ERROR_PATH_NOT_FOUND; }
        }
        set_last_error(st, e);
        return INVALID_HANDLE;
    }
    HObj *o;
    uint64_t h = h_alloc(H_FILE, &o);
    if (!h) { close(fd); set_last_error(st, ERROR_NOT_ENOUGH_MEMORY); return INVALID_HANDLE; }
    o->fd = fd;
    o->append = (access == 0x4);
    snprintf(o->path, sizeof o->path, "%s", host);
    set_last_error(st, (exists && (disp == 2 || disp == 4)) ? ERROR_ALREADY_EXISTS : 0);
    return h;
}
HLE(k32_CreateFileA) { char *p = arg_str(st, 0, 0); RET(create_file(st, p ? p : "", (uint32_t)ARG(1), (uint32_t)ARG(4), (uint32_t)ARG(5))); free(p); }
HLE(k32_CreateFileW) { char *p = arg_str(st, 0, 1); RET(create_file(st, p ? p : "", (uint32_t)ARG(1), (uint32_t)ARG(4), (uint32_t)ARG(5))); free(p); }
HLE(k32_CreateFile2) {
    char *p = arg_str(st, 0, 1);
    uint32_t flags = 0;
    if (ARG(4)) flags = (uint32_t)mem_rd(ARG(4) + 8, 4);
    RET(create_file(st, p ? p : "", (uint32_t)ARG(1), (uint32_t)ARG(3), flags));
    free(p);
}
HLE(k32_CloseHandle) {
    uint64_t h = ARG(0);
    if (h == (uint64_t)-1 || h == (uint64_t)-2) { RET(1); return; }
    if (!h_get(h)) FAIL(ERROR_INVALID_HANDLE);
    h_free(h);
    RET(1);
}
HLE(k32_GetFileSize) {
    HObj *o = h_get(ARG(0));
    struct stat sb;
    if (!o || o->fd < 0 || fstat(o->fd, &sb)) { set_last_error(st, ERROR_INVALID_HANDLE); RET(0xFFFFFFFF); return; }
    if (ARG(1)) mem_wr(ARG(1), 4, (uint64_t)sb.st_size >> 32);
    RET((uint32_t)sb.st_size);
}
HLE(k32_GetFileSizeEx) {
    HObj *o = h_get(ARG(0));
    struct stat sb;
    if (!o || o->fd < 0 || fstat(o->fd, &sb)) FAIL(ERROR_INVALID_HANDLE);
    mem_wr(ARG(1), 8, (uint64_t)sb.st_size);
    RET(1);
}
HLE(k32_SetFilePointer) {
    HObj *o = h_get(ARG(0));
    if (!o || o->fd < 0) { set_last_error(st, ERROR_INVALID_HANDLE); RET(0xFFFFFFFF); return; }
    int64_t off = (int32_t)ARG(1);
    if (ARG(2)) off = (int64_t)(((uint64_t)mem_rd(ARG(2), 4) << 32) | (uint32_t)ARG(1));
    off_t r = lseek(o->fd, (off_t)off, (int)ARG(3));
    if (r < 0) { set_last_error(st, ERROR_INVALID_PARAMETER); RET(0xFFFFFFFF); return; }
    if (ARG(2)) mem_wr(ARG(2), 4, (uint64_t)r >> 32);
    set_last_error(st, 0);
    RET((uint32_t)r);
}
HLE(k32_SetFilePointerEx) {
    HObj *o = h_get(ARG(0));
    if (!o || o->fd < 0) FAIL(ERROR_INVALID_HANDLE);
    off_t r = lseek(o->fd, (off_t)(int64_t)ARG(1), (int)ARG(3));
    if (r < 0) FAIL(ERROR_INVALID_PARAMETER);
    if (ARG(2)) mem_wr(ARG(2), 8, (uint64_t)r);
    RET(1);
}
HLE(k32_SetEndOfFile) {
    HObj *o = h_get(ARG(0));
    if (!o || o->fd < 0) FAIL(ERROR_INVALID_HANDLE);
    off_t pos = lseek(o->fd, 0, SEEK_CUR);
    RET(ftruncate(o->fd, pos) == 0);
}
HLE(k32_FlushFileBuffers) { HObj *o = h_get(ARG(0)); if (o && (o->type == H_CONOUT || o->type == H_CONERR)) con_flush(); RET(1); }
HLE(k32_GetFileInformationByHandle) {
    HObj *o = h_get(ARG(0));
    struct stat sb;
    if (!o || o->fd < 0 || fstat(o->fd, &sb)) FAIL(ERROR_INVALID_HANDLE);
    uint8_t *b = PTR(1);
    memset(b, 0, 52);
    uint32_t attr = S_ISDIR(sb.st_mode) ? 0x10 : 0x20;
    uint64_t ft = filetime_from_unix((double)sb.st_mtime);
    memcpy(b, &attr, 4); memcpy(b + 4, &ft, 8); memcpy(b + 12, &ft, 8); memcpy(b + 20, &ft, 8);
    uint32_t serial = 0x50554C53, hi = (uint32_t)((uint64_t)sb.st_size >> 32), lo = (uint32_t)sb.st_size, links = 1;
    uint32_t ihi = (uint32_t)((uint64_t)sb.st_ino >> 32), ilo = (uint32_t)sb.st_ino;
    memcpy(b + 28, &serial, 4); memcpy(b + 32, &hi, 4); memcpy(b + 36, &lo, 4); memcpy(b + 40, &links, 4);
    memcpy(b + 44, &ihi, 4); memcpy(b + 48, &ilo, 4);
    RET(1);
}
HLE(k32_GetFileTime) {
    HObj *o = h_get(ARG(0));
    struct stat sb;
    if (!o || o->fd < 0 || fstat(o->fd, &sb)) FAIL(ERROR_INVALID_HANDLE);
    uint64_t ft = filetime_from_unix((double)sb.st_mtime);
    for (int i = 1; i <= 3; i++) if (ARG(i)) mem_wr(ARG(i), 8, ft);
    RET(1);
}
HLE_STUB_RET(k32_SetFileTime, 1)
HLE(k32_CompareFileTime) { uint64_t a = mem_rd(ARG(0), 8), b = mem_rd(ARG(1), 8); RET(a < b ? (uint64_t)-1 : a > b ? 1 : 0); }

static void path_op(X86State *st, int wide, int op) {
    char *p = arg_str(st, 0, wide);
    char h[2048];
    path_w2h(p ? p : "", h, sizeof h);
    int r;
    if (op == 0) r = unlink(h);
    else if (op == 1) r = mkdir(h, 0755);
    else r = rmdir(h);
    free(p);
    if (r) FAIL(errno == EEXIST ? ERROR_ALREADY_EXISTS : errno_to_win(errno));
    RET(1);
}
HLE(k32_DeleteFileA) { path_op(st, 0, 0); }
HLE(k32_DeleteFileW) { path_op(st, 1, 0); }
HLE(k32_CreateDirectoryA) { path_op(st, 0, 1); }
HLE(k32_CreateDirectoryW) { path_op(st, 1, 1); }
HLE(k32_RemoveDirectoryA) { path_op(st, 0, 2); }
HLE(k32_RemoveDirectoryW) { path_op(st, 1, 2); }

static void move_file(X86State *st, int wide, uint32_t flags) {
    char *a = arg_str(st, 0, wide), *b = arg_str(st, 1, wide);
    char ha[2048], hb[2048];
    path_w2h(a ? a : "", ha, sizeof ha);
    path_w2h(b ? b : "", hb, sizeof hb);
    free(a); free(b);
    struct stat sb;
    if (!(flags & 1) && stat(hb, &sb) == 0) FAIL(ERROR_ALREADY_EXISTS);
    if (rename(ha, hb)) FAIL(errno_to_win(errno));
    RET(1);
}
HLE(k32_MoveFileA) { move_file(st, 0, 0); }
HLE(k32_MoveFileW) { move_file(st, 1, 0); }
HLE(k32_MoveFileExA) { move_file(st, 0, (uint32_t)ARG(2)); }
HLE(k32_MoveFileExW) { move_file(st, 1, (uint32_t)ARG(2)); }

static void copy_file(X86State *st, int wide) {
    char *a = arg_str(st, 0, wide), *b = arg_str(st, 1, wide);
    char ha[2048], hb[2048];
    path_w2h(a ? a : "", ha, sizeof ha);
    path_w2h(b ? b : "", hb, sizeof hb);
    free(a); free(b);
    struct stat sb;
    if (ARG(2) && stat(hb, &sb) == 0) FAIL(ERROR_FILE_EXISTS);
    FILE *in = fopen(ha, "rb");
    if (!in) FAIL(ERROR_FILE_NOT_FOUND);
    FILE *out = fopen(hb, "wb");
    if (!out) { fclose(in); FAIL(ERROR_ACCESS_DENIED); }
    char buf[65536];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, in)) > 0) fwrite(buf, 1, n, out);
    fclose(in); fclose(out);
    RET(1);
}
HLE(k32_CopyFileA) { copy_file(st, 0); }
HLE(k32_CopyFileW) { copy_file(st, 1); }

static uint32_t host_attrs(const char *host) {
    struct stat sb;
    if (stat(host, &sb)) return 0xFFFFFFFF;
    uint32_t a = S_ISDIR(sb.st_mode) ? 0x10 : 0x20;
    if (!(sb.st_mode & S_IWUSR)) a |= 1;
    return a;
}
static void get_attrs(X86State *st, int wide) {
    char *p = arg_str(st, 0, wide);
    char h[2048];
    path_w2h(p ? p : "", h, sizeof h);
    free(p);
    uint32_t a = host_attrs(h);
    if (a == 0xFFFFFFFF) set_last_error(st, ERROR_FILE_NOT_FOUND);
    RET(a);
}
HLE(k32_GetFileAttributesA) { get_attrs(st, 0); }
HLE(k32_GetFileAttributesW) { get_attrs(st, 1); }
static void get_attrs_ex(X86State *st, int wide) {
    char *p = arg_str(st, 0, wide);
    char h[2048];
    path_w2h(p ? p : "", h, sizeof h);
    free(p);
    struct stat sb;
    if (stat(h, &sb)) FAIL(ERROR_FILE_NOT_FOUND);
    uint8_t *b = PTR(2);
    uint32_t a = host_attrs(h);
    uint64_t ft = filetime_from_unix((double)sb.st_mtime);
    uint32_t hi = (uint32_t)((uint64_t)sb.st_size >> 32), lo = (uint32_t)sb.st_size;
    if (S_ISDIR(sb.st_mode)) hi = lo = 0;
    memcpy(b, &a, 4); memcpy(b + 4, &ft, 8); memcpy(b + 12, &ft, 8); memcpy(b + 20, &ft, 8);
    memcpy(b + 28, &hi, 4); memcpy(b + 32, &lo, 4);
    RET(1);
}
HLE(k32_GetFileAttributesExA) { get_attrs_ex(st, 0); }
HLE(k32_GetFileAttributesExW) { get_attrs_ex(st, 1); }
HLE_STUB_RET(k32_SetFileAttributesA, 1)
HLE_STUB_RET(k32_SetFileAttributesW, 1)

static int find_next(HObj *o, char *name, size_t nlen, char *host, size_t hlen) {
    struct dirent *e;
    while ((e = readdir(o->dir))) {
        if (wild_match(o->pattern, e->d_name)) {
            snprintf(name, nlen, "%s", e->d_name);
            snprintf(host, hlen, "%s/%s", o->path, e->d_name);
            return 1;
        }
    }
    return 0;
}
static void find_first(X86State *st, int wide, int data_arg) {
    char *p = arg_str(st, 0, wide);
    char norm[1200];
    path_normalize_win(p ? p : "", norm, sizeof norm);
    free(p);
    char *bs = strrchr(norm, '\\');
    char pattern[260], dir[1200];
    if (bs) { snprintf(pattern, sizeof pattern, "%s", bs + 1); *bs = 0; snprintf(dir, sizeof dir, "%s%s", norm, strlen(norm) == 2 ? "\\" : ""); }
    else { snprintf(pattern, sizeof pattern, "%s", norm); snprintf(dir, sizeof dir, "%s", g_proc->cwd_win); }
    char hdir[2048];
    path_w2h(dir, hdir, sizeof hdir);
    DIR *d = opendir(hdir);
    if (!d) { set_last_error(st, ERROR_PATH_NOT_FOUND); RET(INVALID_HANDLE); return; }
    HObj *o;
    uint64_t h = h_alloc(H_FIND, &o);
    o->dir = d;
    snprintf(o->pattern, sizeof o->pattern, "%s", pattern);
    snprintf(o->path, sizeof o->path, "%s", hdir);
    char name[512], host[2600];
    if (!find_next(o, name, sizeof name, host, sizeof host)) {
        h_free(h);
        set_last_error(st, ERROR_FILE_NOT_FOUND);
        RET(INVALID_HANDLE);
        return;
    }
    fill_find_data(PTR(data_arg), wide, name, host);
    RET(h);
}
HLE(k32_FindFirstFileA) { find_first(st, 0, 1); }
HLE(k32_FindFirstFileW) { find_first(st, 1, 1); }
HLE(k32_FindFirstFileExA) { find_first(st, 0, 2); }
HLE(k32_FindFirstFileExW) { find_first(st, 1, 2); }
static void find_next_impl(X86State *st, int wide) {
    HObj *o = h_get(ARG(0));
    if (!o || o->type != H_FIND) FAIL(ERROR_INVALID_HANDLE);
    char name[512], host[2600];
    if (!find_next(o, name, sizeof name, host, sizeof host)) FAIL(ERROR_NO_MORE_FILES);
    fill_find_data(PTR(1), wide, name, host);
    RET(1);
}
HLE(k32_FindNextFileA) { find_next_impl(st, 0); }
HLE(k32_FindNextFileW) { find_next_impl(st, 1); }
HLE(k32_FindClose) { h_free(ARG(0)); RET(1); }

HLE(k32_GetCurrentDirectoryA) { RET(put_str(ARG(1), (uint32_t)ARG(0), g_proc->cwd_win, 0)); }
HLE(k32_GetCurrentDirectoryW) { RET(put_str(ARG(1), (uint32_t)ARG(0), g_proc->cwd_win, 1)); }
static void set_cwd(X86State *st, int wide) {
    char *p = arg_str(st, 0, wide);
    char norm[1200];
    path_normalize_win(p ? p : "", norm, sizeof norm);
    free(p);
    char h[2048];
    path_w2h(norm, h, sizeof h);
    struct stat sb;
    if (stat(h, &sb) || !S_ISDIR(sb.st_mode)) FAIL(ERROR_PATH_NOT_FOUND);
    snprintf(g_proc->cwd_win, sizeof g_proc->cwd_win, "%s", norm);
    RET(1);
}
HLE(k32_SetCurrentDirectoryA) { set_cwd(st, 0); }
HLE(k32_SetCurrentDirectoryW) { set_cwd(st, 1); }
static void full_path(X86State *st, int wide) {
    char *p = arg_str(st, 0, wide);
    char norm[1200];
    path_normalize_win(p ? p : "", norm, sizeof norm);
    free(p);
    uint64_t r = put_str(ARG(2), (uint32_t)ARG(1), norm, wide);
    if (ARG(3) && r < (uint32_t)ARG(1)) {
        uint64_t base = ARG(2);
        char *bs = strrchr(norm, '\\');
        size_t off = bs ? (size_t)(bs + 1 - norm) : 0;
        if (wide) off = u2w(norm, (long)off, NULL, 0);
        wr_ptr(ARG(3), base + off * (wide ? 2 : 1));
    }
    RET(r);
}
HLE(k32_GetFullPathNameA) { full_path(st, 0); }
HLE(k32_GetFullPathNameW) { full_path(st, 1); }
HLE(k32_GetLongPathNameA) { RET(put_str(ARG(1), (uint32_t)ARG(2), CSTR(0), 0)); }
HLE(k32_GetLongPathNameW) { char *s = w2u_dup(WSTR(0)); RET(put_str(ARG(1), (uint32_t)ARG(2), s, 1)); free(s); }
HLE(k32_GetTempPathA) { RET(put_str(ARG(1), (uint32_t)ARG(0), "C:\\users\\soul\\Temp\\", 0)); }
HLE(k32_GetTempPathW) { RET(put_str(ARG(1), (uint32_t)ARG(0), "C:\\users\\soul\\Temp\\", 1)); }
static void temp_file_name(X86State *st, int wide) {
    char *dir = arg_str(st, 0, wide), *pre = arg_str(st, 1, wide);
    static unsigned counter = 0x1000;
    unsigned u = (unsigned)ARG(2);
    if (!u) u = ++counter;
    char name[1200];
    snprintf(name, sizeof name, "%s\\%.3s%X.tmp", dir ? dir : "C:\\users\\soul\\Temp", pre ? pre : "", u);
    char norm[1200];
    path_normalize_win(name, norm, sizeof norm);
    if (!ARG(2)) { char h[2048]; path_w2h(norm, h, sizeof h); FILE *f = fopen(h, "wb"); if (f) fclose(f); }
    put_str(ARG(3), 260, norm, wide);
    free(dir); free(pre);
    RET(u);
}
HLE(k32_GetTempFileNameA) { temp_file_name(st, 0); }
HLE(k32_GetTempFileNameW) { temp_file_name(st, 1); }
HLE(k32_GetDriveTypeA) { RET(3); }
HLE(k32_GetLogicalDrives) { RET(1u << 2); }
HLE(k32_GetLogicalDriveStringsA) { RET(put_str(ARG(1), (uint32_t)ARG(0), "C:\\", 0) + 1); }
HLE(k32_GetVolumeInformationW) {
    if (ARG(1)) put_str(ARG(1), (uint32_t)ARG(2), "SOUL", 1);
    if (ARG(3)) mem_wr(ARG(3), 4, 0x50554C53);
    if (ARG(4)) mem_wr(ARG(4), 4, 255);
    if (ARG(5)) mem_wr(ARG(5), 4, 0x3);
    if (hle_arg(st, 6)) put_str(hle_arg(st, 6), (uint32_t)hle_arg(st, 7), "NTFS", 1);
    RET(1);
}

// ---- modules ----------------------------------------------------------------------------------

typedef struct FakeMod { char name[64]; uint8_t *mem; struct FakeMod *next; } FakeMod;
static FakeMod *g_fake_mods;

static uint64_t fake_module(const char *dll) {
    char key[64];
    size_t i = 0;
    for (; dll[i] && i < sizeof key - 1; i++) key[i] = (char)tolower((unsigned char)dll[i]);
    key[i] = 0;
    if (!strchr(key, '.')) strncat(key, ".dll", sizeof key - strlen(key) - 1);
    for (FakeMod *m = g_fake_mods; m; m = m->next) if (strcmp(m->name, key) == 0) return H2G(m->mem);
    FakeMod *m = calloc(1, sizeof *m);
    snprintf(m->name, sizeof m->name, "%s", key);
    m->mem = g_pages(4096);
    m->mem[0] = 'M'; m->mem[1] = 'Z';
    m->next = g_fake_mods;
    g_fake_mods = m;
    return H2G(m->mem);
}
static const char *fake_module_name(uint64_t h) {
    for (FakeMod *m = g_fake_mods; m; m = m->next) if (H2G(m->mem) == h) return m->name;
    return NULL;
}
static uint64_t g_unhandled_filter; // SetUnhandledExceptionFilter (32-bit SEH)
// Per-process kernel32 state: fake system modules and cached command lines.
static char *g_cmdline_a;
static uint16_t *g_cmdline_w;
SOUL_PROCESS_VAR(g_cmdline_a)
SOUL_PROCESS_VAR(g_cmdline_w)
SOUL_PROCESS_VAR(g_fake_mods)
SOUL_PROCESS_VAR(g_unhandled_filter)
void k32_shutdown(void) {
    g_unhandled_filter = 0;
    while (g_fake_mods) { FakeMod *n = g_fake_mods->next; g_pages_free(g_fake_mods->mem, 4096); free(g_fake_mods); g_fake_mods = n; }
    g_free(g_cmdline_a); g_cmdline_a = NULL;
    g_free(g_cmdline_w); g_cmdline_w = NULL;
}
static PEModule *module_by_handle(uint64_t h) {
    for (PEModule *m = g_proc->modules; m; m = m->next) if (m->base == h) return m;
    return NULL;
}
static PEModule *module_by_addr(uint64_t a) {
    for (PEModule *m = g_proc->modules; m; m = m->next) if (a >= m->base && a < m->base + m->size) return m;
    return NULL;
}

static uint64_t load_library(X86State *st, const char *name) {
    if (!name || !*name) { set_last_error(st, ERROR_INVALID_PARAMETER); return 0; }
    const char *base = name;
    for (const char *q = name; *q; q++) if (*q == '\\' || *q == '/') base = q + 1;
    PEModule *m = base == name ? pe_find_loaded(g_proc, base) : NULL;
    if (m) return m->base;
    if (hle_is_system_dll(base)) return fake_module(base);
    char err[256];
    m = pe_load_dependency(g_proc, name, err, sizeof err);
    if (!m) {
        if (g_proc->verbose) soul_log("[yükleyici] LoadLibrary(%s) başarısız: %s\n", name, err);
        set_last_error(st, ERROR_MOD_NOT_FOUND);
        return 0;
    }
    pe_run_initializers(g_proc, st);
    return m->base;
}
HLE(k32_LoadLibraryA) { RET(load_library(st, CSTR(0))); }
HLE(k32_LoadLibraryW) { char *s = w2u_dup(WSTR(0)); RET(load_library(st, s)); free(s); }
HLE(k32_LoadLibraryExA) { RET(load_library(st, CSTR(0))); }
HLE(k32_LoadLibraryExW) { char *s = w2u_dup(WSTR(0)); RET(load_library(st, s)); free(s); }
HLE_STUB_RET(k32_FreeLibrary, 1)
extern char g_dll_dir[600];
HLE(k32_SetDllDirectoryA) { if (ARG(0)) snprintf(g_dll_dir, sizeof g_dll_dir, "%s", CSTR(0)); else g_dll_dir[0] = 0; RET(1); }
HLE(k32_SetDllDirectoryW) { if (ARG(0)) { char *s = w2u_dup(WSTR(0)); snprintf(g_dll_dir, sizeof g_dll_dir, "%s", s); free(s); } else g_dll_dir[0] = 0; RET(1); }
HLE(k32_AddDllDirectory) { if (ARG(0)) { char *s = w2u_dup(WSTR(0)); snprintf(g_dll_dir, sizeof g_dll_dir, "%s", s); free(s); } RET(0x5000); }
HLE_STUB_RET(k32_SetDefaultDllDirectories, 1)
HLE_STUB_RET(k32_RemoveDllDirectory, 1)

static uint64_t get_module_handle(X86State *st, const char *name) {
    if (!name) return g_proc->main_module->base;
    const char *base = name;
    for (const char *q = name; *q; q++) if (*q == '\\' || *q == '/') base = q + 1;
    PEModule *m = base == name ? pe_find_loaded(g_proc, base) : NULL;
    if (m) return m->base;
    if (hle_is_system_dll(base)) return fake_module(base);
    set_last_error(st, ERROR_MOD_NOT_FOUND);
    return 0;
}
HLE(k32_GetModuleHandleA) { RET(get_module_handle(st, ARG(0) ? CSTR(0) : NULL)); }
HLE(k32_GetModuleHandleW) { char *s = ARG(0) ? w2u_dup(WSTR(0)) : NULL; RET(get_module_handle(st, s)); free(s); }
static void get_module_handle_ex(X86State *st, int wide) {
    uint32_t flags = (uint32_t)ARG(0);
    uint64_t r = 0;
    if (flags & 4) { // FROM_ADDRESS
        PEModule *m = module_by_addr(ARG(1));
        r = m ? m->base : 0;
    } else {
        char *s = ARG(1) ? arg_str(st, 1, wide) : NULL;
        r = get_module_handle(st, s);
        free(s);
    }
    if (ARG(2)) wr_ptr(ARG(2), r);
    if (!r) FAIL(ERROR_MOD_NOT_FOUND);
    RET(1);
}
HLE(k32_GetModuleHandleExA) { get_module_handle_ex(st, 0); }
HLE(k32_GetModuleHandleExW) { get_module_handle_ex(st, 1); }
static void module_file_name(X86State *st, int wide) {
    uint64_t h = ARG(0);
    const char *path = g_proc->exe_win;
    char tmp[600];
    if (h && h != g_proc->main_module->base) {
        PEModule *m = module_by_handle(h);
        if (m) path = m->win_path;
        else {
            const char *n = fake_module_name(h);
            if (!n) FAIL(ERROR_MOD_NOT_FOUND);
            snprintf(tmp, sizeof tmp, "C:\\windows\\system32\\%s", n);
            path = tmp;
        }
    }
    uint32_t cap = (uint32_t)ARG(2);
    uint64_t r = put_str(ARG(1), cap, path, wide);
    if (r >= cap) { // truncated
        if (cap) {
            if (wide) { uint16_t *w = u2w_dup(path); memcpy(PTR(1), w, (cap - 1) * 2u); ((uint16_t *)PTR(1))[cap - 1] = 0; free(w); }
            else { memcpy(PTR(1), path, cap - 1); ((char *)PTR(1))[cap - 1] = 0; }
        }
        set_last_error(st, ERROR_INSUFFICIENT_BUFFER);
        RET(cap);
        return;
    }
    RET(r);
}
HLE(k32_GetModuleFileNameA) { module_file_name(st, 0); }
HLE(k32_GetModuleFileNameW) { module_file_name(st, 1); }
HLE(k32_GetProcAddress) {
    uint64_t h = ARG(0), np = ARG(1);
    int ordinal = np < 0x10000 ? (int)np : 0;
    const char *name = ordinal ? NULL : (const char *)G2H(np);
    PEModule *m = module_by_handle(h);
    uint64_t r = 0;
    if (m) r = pe_find_export(m, name, ordinal);
    else {
        const char *dll = fake_module_name(h);
        if (dll && !name) name = hle_ordinal_name(dll, ordinal); // system DLL export known only by number
        if (dll && name) r = hle_get_proc(g_proc, dll, name);
    }
    if (!r) {
        if (g_proc->verbose) {
            if (name) soul_log("[api] GetProcAddress: %s bulunamadı\n", name);
            else soul_log("[api] GetProcAddress: #%d bulunamadı\n", ordinal);
        }
        set_last_error(st, ERROR_PROC_NOT_FOUND);
    }
    RET(r);
}
HLE_STUB_RET(k32_DisableThreadLibraryCalls, 1)

// ---- process / environment -----------------------------------------------------------------

HLE(k32_GetCommandLineA) { if (!g_cmdline_a) g_cmdline_a = g_strdup(g_proc->cmdline); RET(g_cmdline_a); }
HLE(k32_GetCommandLineW) { if (!g_cmdline_w) g_cmdline_w = u2w_gdup(g_proc->cmdline); RET(g_cmdline_w); }
static void get_env_var(X86State *st, int wide) {
    char *n = arg_str(st, 0, wide);
    const char *v = n ? env_get_value(n) : NULL;
    free(n);
    if (!v) FAIL(ERROR_ENVVAR_NOT_FOUND);
    set_last_error(st, 0);
    RET(put_str(ARG(1), (uint32_t)ARG(2), v, wide));
}
HLE(k32_GetEnvironmentVariableA) { get_env_var(st, 0); }
HLE(k32_GetEnvironmentVariableW) { get_env_var(st, 1); }
static void set_env_var(X86State *st, int wide) {
    char *n = arg_str(st, 0, wide), *v = arg_str(st, 1, wide);
    if (n) env_set_value(n, v);
    free(n); free(v);
    RET(1);
}
HLE(k32_SetEnvironmentVariableA) { set_env_var(st, 0); }
HLE(k32_SetEnvironmentVariableW) { set_env_var(st, 1); }
static void env_block(X86State *st, int wide) {
    HleState *h = hs();
    size_t total = 0;
    for (int i = 0; i < h->env_count; i++) total += (wide ? u2w(h->env[i], -1, NULL, 0) : strlen(h->env[i])) + 1;
    total += 1;
    if (wide) {
        uint16_t *b = g_malloc(total * 2), *p = b;
        for (int i = 0; i < h->env_count; i++) { size_t n = u2w(h->env[i], -1, p, total); p += n + 1; }
        *p = 0;
        RET(b);
    } else {
        char *b = g_malloc(total), *p = b;
        for (int i = 0; i < h->env_count; i++) { size_t n = strlen(h->env[i]); memcpy(p, h->env[i], n + 1); p += n + 1; }
        *p = 0;
        RET(b);
    }
}
HLE(k32_GetEnvironmentStringsA) { env_block(st, 0); }
HLE(k32_GetEnvironmentStringsW) { env_block(st, 1); }
HLE(k32_FreeEnvironmentStrings) { g_free(PTR(0)); RET(1); }
static void expand_env(X86State *st, int wide) {
    char *s = arg_str(st, 0, wide);
    char out[4096];
    size_t o = 0;
    for (const char *p = s ? s : ""; *p && o < sizeof out - 1;) {
        if (*p == '%') {
            const char *e = strchr(p + 1, '%');
            if (e) {
                char name[256];
                snprintf(name, sizeof name, "%.*s", (int)(e - p - 1), p + 1);
                const char *v = env_get_value(name);
                if (v) { o += (size_t)snprintf(out + o, sizeof out - o, "%s", v); p = e + 1; continue; }
            }
        }
        out[o++] = *p++;
    }
    out[o] = 0;
    free(s);
    uint64_t r = put_str(ARG(1), (uint32_t)ARG(2), out, wide);
    RET(r < (uint32_t)ARG(2) ? r + 1 : r);
}
HLE(k32_ExpandEnvironmentStringsA) { expand_env(st, 0); }
HLE(k32_ExpandEnvironmentStringsW) { expand_env(st, 1); }
static void startup_info(X86State *st, int wide) {
    (void)wide;
    uint8_t *b = PTR(0);
    uint64_t hin = h_std(0), hout = h_std(1), herr = h_std(2);
    if (g_mode32) { // STARTUPINFO32: 68 bytes, std handles at 56/60/64
        memset(b, 0, 68);
        uint32_t cb = 68;
        memcpy(b, &cb, 4);
        memcpy(b + 56, &hin, 4); memcpy(b + 60, &hout, 4); memcpy(b + 64, &herr, 4);
        RET(0);
        return;
    }
    memset(b, 0, 104);
    uint32_t cb = 104;
    memcpy(b, &cb, 4);
    memcpy(b + 80, &hin, 8); memcpy(b + 88, &hout, 8); memcpy(b + 96, &herr, 8);
    RET(0);
}
HLE(k32_GetStartupInfoA) { startup_info(st, 0); }
HLE(k32_GetStartupInfoW) { startup_info(st, 1); }
HLE(k32_ExitProcess) { int code = (int)ARG(0); crt_run_atexit(st); soul_exit_process(st, code); }
HLE(k32_TerminateProcess) {
    if (ARG(0) == (uint64_t)-1) { crt_flush_all(); soul_exit_process(st, (int)ARG(1)); }
    RET(0);
}
HLE(k32_GetCurrentProcess) { RET((uint64_t)-1); }
HLE(k32_GetCurrentThread) { RET((uint64_t)-2); }
HLE(k32_GetCurrentProcessId) { RET(4242); }
HLE(k32_GetCurrentThreadId) { RET(cur_tid_k32()); }
HLE(k32_GetProcessId) { RET(4242); }
HLE(k32_GetThreadId) {
    if (ARG(0) == (uint64_t)-2) { RET(cur_tid_k32()); return; }
    HObj *o = h_get(ARG(0));
    RET(o && o->type == H_THREAD && o->ptr ? ((SoulThread *)o->ptr)->tid : 0);
}
HLE(k32_GetLastError) { RET(get_last_error(st)); }
HLE(k32_SetLastError) { set_last_error(st, (uint32_t)ARG(0)); RET(0); }
HLE(k32_GetExitCodeProcess) { mem_wr(ARG(1), 4, 259); RET(1); }
HLE(k32_GetExitCodeThread) {
    HObj *o = h_get(ARG(0));
    SoulThread *t = o && o->type == H_THREAD ? o->ptr : NULL;
    mem_wr(ARG(1), 4, t && t->finished ? t->exit_code : 259);
    RET(1);
}
HLE(k32_GetVersion) { RET((19045u << 16) | (0u << 8) | 10u); }
static void version_ex(X86State *st) {
    uint8_t *b = PTR(0);
    uint32_t v[5] = { 0, 10, 0, 19045, 2 };
    uint32_t size; memcpy(&size, b, 4);
    memcpy(b + 4, &v[1], 16);
    (void)size;
    RET(1);
}
HLE(k32_GetVersionExA) { version_ex(st); }
HLE(k32_GetVersionExW) { version_ex(st); }
HLE(ntdll_RtlGetVersion) { version_ex(st); RET(0); }
HLE(k32_VerifyVersionInfoW) { RET(1); }
HLE(k32_VerSetConditionMask) { RET(ARG(0)); }
static void system_info(X86State *st, int native) {
    uint8_t *b = PTR(0);
    if (g_mode32) { // SYSTEM_INFO32 (36 bytes); a 32-bit program sees an x86 CPU unless it asks for the native one
        memset(b, 0, 36);
        uint16_t arch = native ? 9 : 0, level = 6, rev = 0x3C03;
        uint32_t page = 4096, minaddr = 0x10000, maxaddr = 0x7FFEFFFF, mask = 1, ncpu = 1, ptype = native ? 8664 : 586, gran = 65536;
        memcpy(b, &arch, 2); memcpy(b + 4, &page, 4); memcpy(b + 8, &minaddr, 4); memcpy(b + 12, &maxaddr, 4);
        memcpy(b + 16, &mask, 4); memcpy(b + 20, &ncpu, 4); memcpy(b + 24, &ptype, 4); memcpy(b + 28, &gran, 4);
        memcpy(b + 32, &level, 2); memcpy(b + 34, &rev, 2);
        RET(0);
        return;
    }
    (void)native;
    memset(b, 0, 48);
    uint16_t arch = 9; // PROCESSOR_ARCHITECTURE_AMD64
    uint32_t page = 4096, ncpu = 1, ptype = 8664, gran = 65536;
    uint16_t level = 6, rev = 0x3C03;
    uint64_t minaddr = 0x10000, maxaddr = 0x7FFFFFFEFFFFull, mask = 1;
    memcpy(b, &arch, 2); memcpy(b + 4, &page, 4); memcpy(b + 8, &minaddr, 8); memcpy(b + 16, &maxaddr, 8);
    memcpy(b + 24, &mask, 8); memcpy(b + 32, &ncpu, 4); memcpy(b + 36, &ptype, 4); memcpy(b + 40, &gran, 4);
    memcpy(b + 44, &level, 2); memcpy(b + 46, &rev, 2);
    RET(0);
}
HLE(k32_GetSystemInfo) { system_info(st, 0); }
HLE(k32_GetNativeSystemInfo) { system_info(st, 1); }
HLE(k32_IsProcessorFeaturePresent) {
    uint32_t f = (uint32_t)ARG(0);
    // PF_XMMI_INSTRUCTIONS_AVAILABLE(6), PF_XMMI64(10), PF_NX(12), PF_COMPARE_EXCHANGE128(14), PF_FASTFAIL(23), PF_RDTSC(8)
    RET(f == 6 || f == 10 || f == 12 || f == 14 || f == 8 || f == 23 || f == 2 || f == 3);
}
HLE_STUB_RET(k32_IsDebuggerPresent, 0)
HLE(k32_CheckRemoteDebuggerPresent) { if (ARG(1)) mem_wr(ARG(1), 4, 0); RET(1); }
HLE(k32_OutputDebugStringA) { soul_log("[debug] %s", CSTR(0)); RET(0); }
HLE(k32_OutputDebugStringW) { char *s = w2u_dup(WSTR(0)); soul_log("[debug] %s", s); free(s); RET(0); }
HLE(k32_DebugBreak) { soul_fatal(st, "Program DebugBreak() çağırdı"); }
// ---- structured exception handling (32-bit programs) -----------------------------------------------
// Exceptions are dispatched like Windows does: handlers registered in the fs:[0] chain are called
// one after the other with (EXCEPTION_RECORD*, frame, CONTEXT*, dispatcher context). The handler
// runs as ordinary guest code and returns to a Soul thunk, so a handler that never returns (Delphi
// and MSVC handlers unwind and jump straight into the except block) leaves nothing behind.
// The dispatch block lives on the guest stack: [magic, current frame, 0, 0][EXCEPTION_RECORD][CONTEXT].
#define SEH_MAGIC 0x53454844u
#define SEH_REC 16
#define SEH_CTX (16 + 80)
#define SEH_BLOCK (16 + 80 + 0x2CC)

static void ctx_capture32(X86State *st, uint64_t ctx, uint32_t eip, uint32_t esp) {
    memset(G2H(ctx), 0, 0x2CC);
    mem_wr(ctx + 0x00, 4, 0x1003F); // CONTEXT_ALL (x86)
    mem_wr(ctx + 0x9C, 4, st->gpr[RDI]); mem_wr(ctx + 0xA0, 4, st->gpr[RSI]); mem_wr(ctx + 0xA4, 4, st->gpr[RBX]);
    mem_wr(ctx + 0xA8, 4, st->gpr[RDX]); mem_wr(ctx + 0xAC, 4, st->gpr[RCX]); mem_wr(ctx + 0xB0, 4, st->gpr[RAX]);
    mem_wr(ctx + 0xB4, 4, st->gpr[RBP]); mem_wr(ctx + 0xB8, 4, eip); mem_wr(ctx + 0xBC, 4, 0x23);
    mem_wr(ctx + 0xC0, 4, x86_flags(st)); mem_wr(ctx + 0xC4, 4, esp); mem_wr(ctx + 0xC8, 4, 0x2B);
    mem_wr(ctx + 0x8C, 4, 0x2B); mem_wr(ctx + 0x90, 4, 0x53); mem_wr(ctx + 0x94, 4, 0x2B); mem_wr(ctx + 0x98, 4, 0x2B);
}
static void ctx_restore32(X86State *st, uint64_t ctx) {
    st->gpr[RDI] = mem_rd(ctx + 0x9C, 4); st->gpr[RSI] = mem_rd(ctx + 0xA0, 4); st->gpr[RBX] = mem_rd(ctx + 0xA4, 4);
    st->gpr[RDX] = mem_rd(ctx + 0xA8, 4); st->gpr[RCX] = mem_rd(ctx + 0xAC, 4); st->gpr[RAX] = mem_rd(ctx + 0xB0, 4);
    st->gpr[RBP] = mem_rd(ctx + 0xB4, 4);
    st->eflags = (st->eflags & ~(uint64_t)0xFFFFFFFFu) | (mem_rd(ctx + 0xC0, 4) | 2);
    st->lf_op = LF_NONE;
    st->rip = mem_rd(ctx + 0xB8, 4);
    st->gpr[RSP] = mem_rd(ctx + 0xC4, 4);
    st->thread->skip_ret = 1;
}
static uint64_t seh_ret_thunk(void);

static void seh_unhandled(X86State *st, uint64_t blk) {
    uint64_t rec = blk + SEH_REC, ctx = blk + SEH_CTX;
    uint32_t code = (uint32_t)mem_rd(rec, 4);
    mem_wr(blk, 4, 0);
    if (g_unhandled_filter) {
        uint32_t *ptrs = g_malloc(8);
        ptrs[0] = (uint32_t)rec; ptrs[1] = (uint32_t)ctx;
        uint64_t a[1] = { H2G(ptrs) };
        int32_t r = (int32_t)soul_guest_call(st, g_unhandled_filter, 1, a);
        g_free(ptrs);
        if (r == -1) { ctx_restore32(st, ctx); return; }        // EXCEPTION_CONTINUE_EXECUTION
        if (r == 1) { crt_flush_all(); soul_exit_process(st, (int)code); } // EXCEPTION_EXECUTE_HANDLER
    }
    if (code == 0xE06D7363) soul_fatal(st, "Program yakalanmamış bir C++ istisnası fırlattı");
    if (code == 0xC0000094) soul_fatal(st, "Tamsayı sıfıra bölme (yakalanmadı) @%x", (unsigned)mem_rd(rec + 12, 4));
    soul_fatal(st, "Program yakalanmamış bir Windows istisnası oluşturdu (kod 0x%08X)", code);
}
// Calls the handler of the next registration frame (or gives up when the chain ends).
static void seh_next(X86State *st, uint64_t blk, uint32_t frame) {
    uint32_t sp = (uint32_t)st->gpr[RSP];
    if (frame == 0xFFFFFFFFu || frame == 0 || frame < sp - 0x1000 || frame & 3) { seh_unhandled(st, blk); return; }
    uint32_t handler = (uint32_t)mem_rd(frame + 4, 4);
    if (!handler) { seh_unhandled(st, blk); return; }
    mem_wr(blk + 4, 4, frame);
    uint32_t a = (uint32_t)blk - 20;
    mem_wr(a, 4, seh_ret_thunk());
    mem_wr(a + 4, 4, blk + SEH_REC);
    mem_wr(a + 8, 4, frame);
    mem_wr(a + 12, 4, blk + SEH_CTX);
    mem_wr(a + 16, 4, 0);
    st->gpr[RSP] = a;
    st->rip = handler;
    st->thread->skip_ret = 1;
    SoulThread *t = st->thread;
    if (t->seh_n == 16) { memmove(t->seh_disp, t->seh_disp + 1, sizeof(uint64_t) * 15); t->seh_n = 15; }
    t->seh_disp[t->seh_n++] = blk;
}
// The exception handler returned (cdecl: its arguments are still on the stack).
HLE(k32_SehReturn) {
    SoulThread *t = st->thread;
    uint64_t esp = (uint32_t)st->gpr[RSP], blk = 0;
    while (t->seh_n > 0) {
        uint64_t b = t->seh_disp[--t->seh_n];
        if (b >= esp && mem_rd(b, 4) == SEH_MAGIC) { blk = b; break; }
    }
    if (!blk) soul_fatal(st, "İstisna işleyicisinden beklenmeyen dönüş");
    uint32_t disp = (uint32_t)st->gpr[RAX];
    st->gpr[RSP] = (uint32_t)blk - 64; // keep the dispatch block above the stack pointer
    if (disp == 0) { // ExceptionContinueExecution
        if (mem_rd(blk + SEH_REC + 4, 4) & 1) soul_fatal(st, "Sürdürülemez istisnadan sonra devam edilmek istendi");
        mem_wr(blk, 4, 0);
        ctx_restore32(st, blk + SEH_CTX);
        return;
    }
    uint32_t frame = (uint32_t)mem_rd(blk + 4, 4);
    seh_next(st, blk, (uint32_t)mem_rd(frame, 4)); // ExceptionContinueSearch (nested/collided: keep searching)
}
static uint64_t seh_ret_thunk(void) {
    uint32_t id = hle_thunk_for(g_proc, k32_SehReturn, "soul", "SehReturn");
    return g_proc->thunk_base + (uint64_t)id * SOUL_THUNK_STRIDE;
}

int soul_seh_raise(X86State *st, uint32_t code, uint32_t flags, uint64_t eip, uint64_t esp, int nparams, const uint32_t *params) {
    if (!g_mode32) return 0;
    uint64_t blk = (((uint32_t)st->gpr[RSP] - SEH_BLOCK - 64) & ~15u);
    mem_wr(blk, 4, SEH_MAGIC); mem_wr(blk + 4, 4, 0); mem_wr(blk + 8, 8, 0);
    uint64_t rec = blk + SEH_REC;
    memset(G2H(rec), 0, 80);
    mem_wr(rec, 4, code); mem_wr(rec + 4, 4, flags & 1); mem_wr(rec + 12, 4, eip);
    if (nparams > 15) nparams = 15;
    mem_wr(rec + 16, 4, (uint32_t)nparams);
    for (int i = 0; i < nparams; i++) mem_wr(rec + 20 + 4 * (uint64_t)i, 4, params[i]);
    ctx_capture32(st, blk + SEH_CTX, (uint32_t)eip, (uint32_t)esp);
    if (g_proc->verbose) soul_log("[seh] istisna 0x%08X @%x\n", code, (unsigned)eip);
    st->gpr[RSP] = (uint32_t)blk - 64;
    seh_next(st, blk, (uint32_t)mem_rd(teb_of(st), 4));
    return 1;
}

HLE(k32_RaiseException) {
    uint32_t code = (uint32_t)ARG(0);
    if (code == 0x406D1388) { RET(0); return; } // MS_VC_EXCEPTION (thread naming)
    if (g_mode32) {
        uint32_t esp = (uint32_t)st->gpr[RSP], ret = (uint32_t)mem_rd(esp, 4);
        uint32_t n = (uint32_t)ARG(2), params[15] = { 0 };
        if (n > 15) n = 15;
        for (uint32_t i = 0; i < n && ARG(3); i++) params[i] = (uint32_t)mem_rd(ARG(3) + 4 * (uint64_t)i, 4);
        soul_seh_raise(st, code, (uint32_t)ARG(1), ret, esp + 4 + 16, (int)n, params); // continuing = returning from RaiseException
        return;
    }
    if (code == 0xE06D7363) soul_fatal(st, "Program bir C++ istisnası fırlattı; 64-bit C++ istisnaları henüz desteklenmiyor");
    soul_fatal(st, "Program bir Windows istisnası oluşturdu (kod 0x%08X)", code);
}
// RtlUnwind(TargetFrame, TargetIp, ExceptionRecord, ReturnValue): runs the handlers of the frames
// above TargetFrame in unwind mode (finally blocks, destructors) and removes them from the chain.
static void rtl_unwind32(X86State *st) {
    uint32_t target = (uint32_t)ARG(0), tip = (uint32_t)ARG(1), recp = (uint32_t)ARG(2), retval = (uint32_t)ARG(3);
    uint32_t esp = (uint32_t)st->gpr[RSP], after = esp + 4 + 16;
    uint8_t *rec = g_calloc(1, 80), *ctx = g_calloc(1, 0x2CC);
    if (recp) memcpy(rec, G2H(recp), 80);
    else { uint32_t c = 0xC0000027; memcpy(rec, &c, 4); } // STATUS_UNWIND
    uint32_t fl; memcpy(&fl, rec + 4, 4);
    fl |= 2 | (target ? 0 : 4); // EXCEPTION_UNWINDING (| EXCEPTION_EXIT_UNWIND)
    memcpy(rec + 4, &fl, 4);
    ctx_capture32(st, H2G(ctx), (uint32_t)mem_rd(esp, 4), after);
    uint64_t teb = teb_of(st);
    uint32_t frame = (uint32_t)mem_rd(teb, 4);
    while (frame != 0xFFFFFFFFu && frame && frame != target) {
        uint32_t handler = (uint32_t)mem_rd(frame + 4, 4);
        if (handler) { uint64_t a[4] = { H2G(rec), frame, H2G(ctx), 0 }; soul_guest_call(st, handler, 4, a); }
        frame = (uint32_t)mem_rd(frame, 4);
        mem_wr(teb, 4, frame);
    }
    g_free(rec); g_free(ctx);
    st->gpr[RAX] = retval;
    if (tip && tip != mem_rd(esp, 4)) { st->rip = tip; st->gpr[RSP] = after; st->thread->skip_ret = 1; return; }
    RET(retval);
}
HLE(k32_RtlUnwind) {
    if (!g_mode32) soul_fatal(st, "Yapılandırılmış istisna işleme (SEH unwind) desteklenmiyor");
    rtl_unwind32(st);
}
// NtContinue(CONTEXT*, alert) / RtlRaiseException(EXCEPTION_RECORD*)
HLE(k32_NtContinue) {
    if (!g_mode32) soul_fatal(st, "NtContinue desteklenmiyor");
    ctx_restore32(st, ARG(0));
}
HLE(k32_RtlRaiseException) {
    if (!g_mode32) soul_fatal(st, "RtlRaiseException desteklenmiyor");
    uint64_t r = ARG(0);
    uint32_t n = (uint32_t)mem_rd(r + 16, 4), params[15] = { 0 };
    if (n > 15) n = 15;
    for (uint32_t i = 0; i < n; i++) params[i] = (uint32_t)mem_rd(r + 20 + 4 * (uint64_t)i, 4);
    uint32_t esp = (uint32_t)st->gpr[RSP];
    soul_seh_raise(st, (uint32_t)mem_rd(r, 4), (uint32_t)mem_rd(r + 4, 4), mem_rd(esp, 4), esp + 8, (int)n, params);
}
HLE(k32_SetUnhandledExceptionFilter) { uint64_t old = g_unhandled_filter; g_unhandled_filter = ARG(0); RET(old); }
HLE(k32_UnhandledExceptionFilter) { RET(1); }
HLE(k32_AddVectoredExceptionHandler) { RET(0x5EC0); }
HLE(k32_RemoveVectoredExceptionHandler) { RET(1); }
HLE(k32_RtlCaptureContext) {
    uint8_t *c = PTR(0);
    if (g_mode32) { // CONTEXT (x86): flags at 0, Ebp/Eip/SegCs/EFlags/Esp/SegSs from 0xB4
        memset(c, 0, 0x2CC);
        uint32_t v = 0x10007;
        memcpy(c, &v, 4);
        static const int off[8] = { 0xB0, 0xAC, 0xA8, 0xA4, -1, 0xB4, 0xA0, 0x9C }; // eax ecx edx ebx (esp) ebp esi edi
        for (int r = 0; r < 8; r++) if (off[r] >= 0) { v = (uint32_t)st->gpr[r]; memcpy(c + off[r], &v, 4); }
        v = (uint32_t)mem_rd(st->gpr[RSP], 4); memcpy(c + 0xB8, &v, 4);
        v = (uint32_t)(st->gpr[RSP] + 4); memcpy(c + 0xC4, &v, 4);
        RET(0);
        return;
    }
    memset(c, 0, 1232);
    uint32_t flags = 0x10000B; // CONTEXT_FULL
    memcpy(c + 0x30, &flags, 4);
    memcpy(c + 0x78, &st->gpr[0], 8 * 16);
    uint64_t rip = mem_rd(st->gpr[RSP], 8), rsp = st->gpr[RSP] + 8;
    memcpy(c + 0x98, &rsp, 8);
    memcpy(c + 0xF8, &rip, 8);
    RET(0);
}
HLE(k32_RtlLookupFunctionEntry) { RET(0); }
HLE(k32_RtlVirtualUnwind) { RET(0); }
HLE(k32_RtlUnwindEx) { soul_fatal(st, "Yapılandırılmış istisna işleme (SEH unwind) desteklenmiyor"); }
HLE(k32_RtlPcToFileHeader) {
    PEModule *m = module_by_addr(ARG(0));
    wr_ptr(ARG(1), m ? m->base : 0);
    RET(m ? m->base : 0);
}
HLE(k32_EncodePointer) { RET(ARG(0)); }
HLE_STUB_RET(k32_SetErrorMode, 0)
HLE_STUB_RET(k32_GetErrorMode, 0)
HLE(k32_SetProcessAffinityMask) { RET(1); }
HLE(k32_GetProcessAffinityMask) { if (ARG(1)) wr_ptr(ARG(1), 1); if (ARG(2)) wr_ptr(ARG(2), 1); RET(1); }
HLE_STUB_RET(k32_SetThreadPriority, 1)
HLE_STUB_RET(k32_GetThreadPriority, 0)
HLE_STUB_RET(k32_SetPriorityClass, 1)
HLE(k32_GetPriorityClass) { RET(0x20); }
HLE_STUB_RET(k32_GetCurrentProcessorNumber, 0)
HLE(k32_IsWow64Process) { if (ARG(1)) mem_wr(ARG(1), 4, g_mode32 ? 1 : 0); RET(1); } // 32-bit programs run "under WOW64"
HLE_STUB_RET(k32_SetThreadDescription, 0)
HLE(k32_GetProcessTimes) {
    uint64_t now = filetime_from_unix((double)time(NULL));
    if (ARG(1)) mem_wr(ARG(1), 8, now);
    if (ARG(2)) mem_wr(ARG(2), 8, 0);
    uint64_t cpu = (uint64_t)(soul_now() - g_proc->start_time) * 10000000ull;
    if (ARG(3)) mem_wr(ARG(3), 8, 0);
    if (hle_arg(st, 4)) mem_wr(hle_arg(st, 4), 8, cpu);
    RET(1);
}
HLE(k32_CreateProcessA) { soul_log("[api] Yeni süreç başlatma (CreateProcess) desteklenmiyor\n"); FAIL(ERROR_NOT_SUPPORTED); }

// ---- time ------------------------------------------------------------------------------------

static double unix_now(void) { struct timeval tv; gettimeofday(&tv, NULL); return (double)tv.tv_sec + tv.tv_usec / 1e6; }
static void sleep_ms(uint32_t ms) {
    con_flush();
    if (ms == 0) { soul_yield(); return; }
    double deadline = ms == 0xFFFFFFFF ? -1.0 : soul_now() + ms / 1000.0;
    while (!g_proc->terminate) {
        double now = soul_now();
        if (deadline >= 0 && now >= deadline) break;
        int64_t left = deadline < 0 ? 1000 : (int64_t)((deadline - now) * 1000.0) + 1;
        gil_wait(left);
    }
}
HLE(k32_Sleep) { sleep_ms((uint32_t)ARG(0)); RET(0); }
HLE(k32_SleepEx) { sleep_ms((uint32_t)ARG(0)); RET(0); }
HLE(k32_SwitchToThread) { soul_yield(); RET(1); }
HLE(k32_GetTickCount) { RET((uint32_t)(soul_now() * 1000.0)); }
HLE(k32_GetTickCount64) { RET((uint64_t)(soul_now() * 1000.0)); }
HLE(k32_QueryPerformanceCounter) { mem_wr(ARG(0), 8, (uint64_t)(soul_now() * 10000000.0)); RET(1); }
HLE(k32_QueryPerformanceFrequency) { mem_wr(ARG(0), 8, 10000000); RET(1); }
HLE(k32_GetSystemTimeAsFileTime) { mem_wr(ARG(0), 8, filetime_from_unix(unix_now())); RET(0); }
static void ft_to_st(uint64_t ft, uint8_t *out, int local) {
    double unixt = (double)ft / 10000000.0 - 11644473600.0;
    time_t t = (time_t)unixt;
    struct tm r;
    if (local) localtime_r(&t, &r); else gmtime_r(&t, &r);
    uint16_t v[8] = { (uint16_t)(r.tm_year + 1900), (uint16_t)(r.tm_mon + 1), (uint16_t)r.tm_wday, (uint16_t)r.tm_mday,
                      (uint16_t)r.tm_hour, (uint16_t)r.tm_min, (uint16_t)r.tm_sec, (uint16_t)((ft / 10000) % 1000) };
    memcpy(out, v, 16);
}
HLE(k32_GetSystemTime) { ft_to_st(filetime_from_unix(unix_now()), PTR(0), 0); RET(0); }
HLE(k32_GetLocalTime) { ft_to_st(filetime_from_unix(unix_now()), PTR(0), 1); RET(0); }
HLE(k32_FileTimeToSystemTime) { ft_to_st(mem_rd(ARG(0), 8), PTR(1), 0); RET(1); }
HLE(k32_FileTimeToLocalFileTime) { mem_wr(ARG(1), 8, mem_rd(ARG(0), 8)); RET(1); }
HLE(k32_SystemTimeToFileTime) {
    uint16_t v[8];
    memcpy(v, PTR(0), 16);
    struct tm r = { 0 };
    r.tm_year = v[0] - 1900; r.tm_mon = v[1] - 1; r.tm_mday = v[3]; r.tm_hour = v[4]; r.tm_min = v[5]; r.tm_sec = v[6];
    time_t t = timegm(&r);
    mem_wr(ARG(1), 8, filetime_from_unix((double)t) + (uint64_t)v[7] * 10000);
    RET(1);
}
HLE(k32_GetTimeZoneInformation) { memset(PTR(0), 0, 172); RET(0); }
HLE(k32_GetDynamicTimeZoneInformation) { memset(PTR(0), 0, 432); RET(0); }

// ---- synchronisation ----------------------------------------------------------------------------
// Guest threads run one at a time under the GIL, so these objects only need to
// block (gil_wait releases the GIL) and to be woken up (gil_notify).

static uint32_t cur_tid(void) { SoulThread *t = soul_cur_thread(); return t ? t->tid : 4243; }

// Waits until notified or the deadline (<0: infinite) passes. Returns 1 on timeout.
static int wait_until(double deadline, int poll_input) {
    if (g_proc->terminate) return 1;
    if (deadline < 0 && !poll_input) { gil_wait(-1); return 0; }
    double now = soul_now();
    if (deadline >= 0 && now >= deadline) return 1;
    int64_t ms = deadline < 0 ? 20 : (int64_t)((deadline - now) * 1000.0) + 1;
    if (poll_input && ms > 20) ms = 20;
    gil_wait(ms);
    return deadline >= 0 && soul_now() >= deadline;
}
static double deadline_for(uint32_t ms) { return ms == 0xFFFFFFFF ? -1.0 : soul_now() + ms / 1000.0; }

static void deadlock(X86State *st, const char *what) {
    soul_fatal(st, "Kilitlenme: program, başka iş parçacığı yokken hiçbir zaman serbest kalmayacak bir %s bekliyor", what);
}

// critical sections: RTL_CRITICAL_SECTION { DebugInfo, LockCount, RecursionCount, OwningThread, LockSemaphore, SpinCount }
#define CS_REC (g_mode32 ? 8 : 12)
#define CS_OWNER (g_mode32 ? 12 : 16)
#define CS_SPIN (g_mode32 ? 20 : 32)
#define CS_SIZE (g_mode32 ? 24u : 40u)
static void cs_enter(X86State *st, uint64_t cs) {
    uint32_t me = cur_tid();
    for (;;) {
        uint64_t owner = rd_ptr(cs + CS_OWNER);
        if (owner == 0 || owner == me) {
            wr_ptr(cs + CS_OWNER, me);
            mem_wr(cs + CS_REC, 4, mem_rd(cs + CS_REC, 4) + 1);
            return;
        }
        if (g_proc->terminate) return;
        if (!g_proc->threads) deadlock(st, "kritik bölge");
        wait_until(-1, 0);
    }
}
static void cs_leave(uint64_t cs) {
    int32_t rec = (int32_t)mem_rd(cs + CS_REC, 4) - 1;
    if (rec <= 0) { mem_wr(cs + CS_REC, 4, 0); wr_ptr(cs + CS_OWNER, 0); gil_notify(); }
    else mem_wr(cs + CS_REC, 4, (uint32_t)rec);
}
HLE(k32_InitializeCriticalSection) { memset(PTR(0), 0, CS_SIZE); RET(0); }
HLE(k32_InitializeCriticalSectionEx) { memset(PTR(0), 0, CS_SIZE); wr_ptr(ARG(0) + CS_SPIN, (uint32_t)ARG(1)); RET(1); }
HLE(k32_EnterCriticalSection) { cs_enter(st, ARG(0)); RET(0); }
HLE(k32_LeaveCriticalSection) { cs_leave(ARG(0)); RET(0); }
HLE(k32_TryEnterCriticalSection) {
    uint64_t cs = ARG(0), owner = rd_ptr(cs + CS_OWNER);
    uint32_t me = cur_tid();
    if (owner && owner != me) { RET(0); return; }
    wr_ptr(cs + CS_OWNER, me);
    mem_wr(cs + CS_REC, 4, mem_rd(cs + CS_REC, 4) + 1);
    RET(1);
}
HLE_STUB_RET(k32_DeleteCriticalSection, 0)

// slim reader/writer locks: 0 = free, 1 = exclusive, 2*n = n shared owners
static void srw_acquire(X86State *st, uint64_t l, int shared) {
    for (;;) {
        uint64_t v = rd_ptr(l);
        if (shared ? v != 1 : v == 0) { wr_ptr(l, shared ? v + 2 : 1); return; }
        if (g_proc->terminate) return;
        if (!g_proc->threads) deadlock(st, "SRW kilidi");
        wait_until(-1, 0);
    }
}
static void srw_release(uint64_t l, int shared) {
    uint64_t v = rd_ptr(l);
    if (shared) v = v >= 2 ? v - 2 : 0; else v = 0;
    wr_ptr(l, v);
    if (v == 0) gil_notify();
}
HLE(k32_InitializeSRWLock) { wr_ptr(ARG(0), 0); RET(0); }
HLE(k32_AcquireSRWLockExclusive) { srw_acquire(st, ARG(0), 0); RET(0); }
HLE(k32_AcquireSRWLockShared) { srw_acquire(st, ARG(0), 1); RET(0); }
HLE(k32_ReleaseSRWLockExclusive) { srw_release(ARG(0), 0); RET(0); }
HLE(k32_ReleaseSRWLockShared) { srw_release(ARG(0), 1); RET(0); }
HLE(k32_TryAcquireSRWLockExclusive) { uint64_t v = rd_ptr(ARG(0)); if (v) { RET(0); return; } wr_ptr(ARG(0), 1); RET(1); }
HLE(k32_TryAcquireSRWLockShared) { uint64_t v = rd_ptr(ARG(0)); if (v == 1) { RET(0); return; } wr_ptr(ARG(0), v + 2); RET(1); }

// condition variables: the guest word is a wake-up generation counter
HLE(k32_InitializeConditionVariable) { wr_ptr(ARG(0), 0); RET(0); }
HLE(k32_WakeConditionVariable) { wr_ptr(ARG(0), rd_ptr(ARG(0)) + 1); gil_notify(); RET(0); }
static int cv_sleep(X86State *st, uint64_t cv, uint32_t ms) {
    uint64_t gen = rd_ptr(cv);
    double dl = deadline_for(ms);
    int timeout = 0;
    while (rd_ptr(cv) == gen) {
        if (g_proc->terminate) { timeout = 1; break; }
        if (ms == 0xFFFFFFFF && !g_proc->threads) deadlock(st, "koşul değişkeni");
        if (wait_until(dl, 0)) { timeout = 1; break; }
    }
    return timeout;
}
HLE(k32_SleepConditionVariableCS) {
    uint64_t cv = ARG(0), cs = ARG(1);
    uint32_t rec = (uint32_t)mem_rd(cs + CS_REC, 4);
    mem_wr(cs + CS_REC, 4, 0); wr_ptr(cs + CS_OWNER, 0);
    gil_notify();
    int timeout = cv_sleep(st, cv, (uint32_t)ARG(2));
    cs_enter(st, cs);
    mem_wr(cs + CS_REC, 4, rec ? rec : 1);
    if (timeout) { set_last_error(st, 1460); RET(0); return; }
    RET(1);
}
HLE(k32_SleepConditionVariableSRW) {
    uint64_t cv = ARG(0), l = ARG(1);
    int shared = ((uint32_t)ARG(3) & 1) != 0;
    srw_release(l, shared);
    int timeout = cv_sleep(st, cv, (uint32_t)ARG(2));
    srw_acquire(st, l, shared);
    if (timeout) { set_last_error(st, 1460); RET(0); return; }
    RET(1);
}
HLE(k32_WaitOnAddress) {
    uint64_t addr = ARG(0), cmp = ARG(1), size = ARG(2);
    double dl = deadline_for((uint32_t)ARG(3));
    while (memcmp(PTR(0), PTR(1), size) == 0) {
        if (g_proc->terminate) break;
        if ((uint32_t)ARG(3) == 0xFFFFFFFF && !g_proc->threads) deadlock(st, "adres değişikliği");
        if (wait_until(dl, 0)) { set_last_error(st, 1460); RET(0); return; }
    }
    (void)addr; (void)cmp;
    RET(1);
}
HLE(k32_WakeByAddress) { gil_notify(); RET(0); }

HLE(k32_InitOnceExecuteOnce) {
    uint64_t once = ARG(0);
    if (rd_ptr(once) == 0) {
        wr_ptr(once, 1);
        uint64_t args[3] = { once, ARG(2), ARG(3) };
        uint64_t r = soul_guest_call(st, ARG(1), 3, args) & 0xFFFFFFFF;
        if (!r) { wr_ptr(once, 0); gil_notify(); RET(0); return; }
        wr_ptr(once, 2);
        gil_notify();
    }
    while (rd_ptr(once) == 1 && !g_proc->terminate) wait_until(-1, 0);
    RET(1);
}
HLE(k32_InitOnceBeginInitialize) {
    uint64_t once = ARG(0);
    while (rd_ptr(once) == 1 && !g_proc->terminate) wait_until(-1, 0);
    int done = rd_ptr(once) == 2;
    if (!done) wr_ptr(once, 1);
    if (ARG(2)) mem_wr(ARG(2), 4, done ? 0 : 1);
    if (ARG(3)) wr_ptr(ARG(3), 0);
    RET(1);
}
HLE(k32_InitOnceComplete) { wr_ptr(ARG(0), ((uint32_t)ARG(1) & 4) ? 0 : 2); gil_notify(); RET(1); }

// Named events, mutexes and semaphores: a second Create returns the existing object with
// ERROR_ALREADY_EXISTS, Open finds it (names are kept per process).
static uint64_t g_sync_name_arg; // name argument of the Create call being handled (0 = none)
static int g_sync_name_wide;
static uint64_t find_named(HType t, const char *name) {
    HleState *h = hs();
    for (int i = 0; i < MAX_HANDLES; i++) {
        HObj *o = &h->handles[i];
        if (o->type == t && o->path[0] == '\x01' && !strcmp(o->path + 1, name)) return HANDLE_BASE + (uint64_t)i * 4;
    }
    return 0;
}
static char *sync_name(void) {
    uint64_t p = g_sync_name_arg;
    g_sync_name_arg = 0;
    if (!p) return NULL;
    char *s = g_sync_name_wide ? w2u_dup((const uint16_t *)G2H(p)) : strdup((const char *)G2H(p));
    if (s && !*s) { free(s); return NULL; }
    return s;
}
static uint64_t make_sync(HType t, int signaled, int manual, long count) {
    char *name = sync_name();
    X86State *st = hle_state_of();
    if (st && soul_cur_thread()) st = soul_cur_thread()->st;
    if (name) {
        uint64_t h = find_named(t, name);
        if (h) { free(name); if (st) set_last_error(st, ERROR_ALREADY_EXISTS); return h; }
    }
    HObj *o;
    uint64_t h = h_alloc(t, &o);
    if (o) {
        o->signaled = signaled; o->manual = manual; o->count = count;
        if (name) { o->path[0] = 1; snprintf(o->path + 1, sizeof o->path - 1, "%s", name); }
    }
    free(name);
    if (st) set_last_error(st, 0);
    return h;
}
static void open_named(X86State *st, HType t, int wide) {
    char *name = ARG(2) ? (wide ? w2u_dup(WSTR(2)) : strdup(CSTR(2))) : NULL;
    uint64_t h = name ? find_named(t, name) : 0;
    free(name);
    if (!h) FAIL(ERROR_FILE_NOT_FOUND);
    RET(h);
}
HLE(k32_OpenEventA) { open_named(st, H_EVENT, 0); }
HLE(k32_OpenEventW) { open_named(st, H_EVENT, 1); }
HLE(k32_OpenMutexA) { open_named(st, H_MUTEX, 0); }
HLE(k32_OpenMutexW) { open_named(st, H_MUTEX, 1); }
HLE(k32_OpenSemaphoreA) { open_named(st, H_SEMAPHORE, 0); }
HLE(k32_OpenSemaphoreW) { open_named(st, H_SEMAPHORE, 1); }
#define NAMED(arg, wide) (g_sync_name_arg = ARG(arg), g_sync_name_wide = (wide))
HLE(k32_CreateEventA) { NAMED(3, 0); RET(make_sync(H_EVENT, (int)ARG(2), (int)ARG(1), 0)); }
HLE(k32_CreateEventW) { NAMED(3, 1); RET(make_sync(H_EVENT, (int)ARG(2), (int)ARG(1), 0)); }
HLE(k32_CreateEventExW) { NAMED(1, 1); uint32_t f = (uint32_t)ARG(2); RET(make_sync(H_EVENT, (f & 2) != 0, (f & 1) != 0, 0)); }
HLE(k32_SetEvent) { HObj *o = h_get(ARG(0)); if (o) { o->signaled = 1; gil_notify(); } RET(o != NULL); }
HLE(k32_ResetEvent) { HObj *o = h_get(ARG(0)); if (o) o->signaled = 0; RET(o != NULL); }
static uint64_t make_mutex(int owned) {
    uint64_t h = make_sync(H_MUTEX, 0, 0, 0);
    HObj *o = h_get(h);
    if (o && owned && o->owner == 0) { o->owner = cur_tid(); o->recursion = 1; }
    return h;
}
HLE(k32_CreateMutexA) { NAMED(2, 0); RET(make_mutex(ARG(1) != 0)); }
HLE(k32_CreateMutexW) { NAMED(2, 1); RET(make_mutex(ARG(1) != 0)); }
HLE(k32_CreateMutexExW) { NAMED(1, 1); RET(make_mutex(((uint32_t)ARG(2) & 1) != 0)); }
HLE(k32_ReleaseMutex) {
    HObj *o = h_get(ARG(0));
    if (!o || o->type != H_MUTEX || o->owner != cur_tid()) FAIL(288); // ERROR_NOT_OWNER
    if (--o->recursion == 0) { o->owner = 0; gil_notify(); }
    RET(1);
}
HLE(k32_CreateSemaphoreA) { NAMED(3, 0); RET(make_sync(H_SEMAPHORE, ARG(1) > 0, 0, (long)(int32_t)ARG(1))); }
HLE(k32_CreateSemaphoreW) { NAMED(3, 1); RET(make_sync(H_SEMAPHORE, ARG(1) > 0, 0, (long)(int32_t)ARG(1))); }
HLE(k32_CreateSemaphoreExW) { RET(make_sync(H_SEMAPHORE, ARG(1) > 0, 0, (long)(int32_t)ARG(1))); }
HLE(k32_ReleaseSemaphore) {
    HObj *o = h_get(ARG(0));
    if (!o) FAIL(ERROR_INVALID_HANDLE);
    if (ARG(2)) mem_wr(ARG(2), 4, (uint64_t)o->count);
    o->count += (long)(int32_t)ARG(1);
    o->signaled = o->count > 0;
    gil_notify();
    RET(1);
}

// 1 if the object is signalled (does not consume it)
static int obj_ready(HObj *o) {
    switch (o->type) {
    case H_EVENT: return o->signaled;
    case H_MUTEX: return o->owner == 0 || o->owner == cur_tid();
    case H_SEMAPHORE: return o->count > 0;
    case H_THREAD: return o->ptr && ((SoulThread *)o->ptr)->finished;
    case H_PROCESS: return o->ptr && ((SoulProcess *)o->ptr)->exited;
    case H_CONIN: return con_has_input();
    default: return 1;
    }
}
static void obj_consume(HObj *o) {
    switch (o->type) {
    case H_EVENT: if (!o->manual) o->signaled = 0; break;
    case H_MUTEX: o->owner = cur_tid(); o->recursion++; break;
    case H_SEMAPHORE: o->count--; o->signaled = o->count > 0; break;
    default: break;
    }
}
static int can_be_signalled_later(HObj *o) {
    // with a single guest thread only console input (from the UI) can still arrive
    return g_proc->threads != NULL || o->type == H_CONIN || o->type == H_PROCESS;
}

static uint32_t wait_objects(X86State *st, uint32_t n, const uint64_t *hs_, int all, uint32_t ms) {
    HObj *objs[64];
    if (n == 0 || n > 64) { set_last_error(st, ERROR_INVALID_PARAMETER); return 0xFFFFFFFF; }
    int poll = 0, later = 0;
    for (uint32_t i = 0; i < n; i++) {
        if (hs_[i] == (uint64_t)-1) { objs[i] = NULL; continue; } // the current process: never signalled while running
        if (hs_[i] == (uint64_t)-2) { objs[i] = NULL; continue; }
        objs[i] = h_get(hs_[i]);
        if (!objs[i]) { set_last_error(st, ERROR_INVALID_HANDLE); return 0xFFFFFFFF; }
        if (objs[i]->type == H_CONIN) poll = 1;
        if (can_be_signalled_later(objs[i])) later = 1;
    }
    double dl = deadline_for(ms);
    for (;;) {
        if (all) {
            int ok = 1;
            for (uint32_t i = 0; i < n; i++) if (!objs[i] || !obj_ready(objs[i])) { ok = 0; break; }
            if (ok) { for (uint32_t i = 0; i < n; i++) obj_consume(objs[i]); return 0; }
        } else {
            for (uint32_t i = 0; i < n; i++) if (objs[i] && obj_ready(objs[i])) { obj_consume(objs[i]); return i; }
        }
        if (g_proc->terminate) return 0xFFFFFFFF;
        if (ms == 0) return 0x102;
        if (ms == 0xFFFFFFFF && !later) deadlock(st, "nesne");
        if (wait_until(dl, poll)) return 0x102; // WAIT_TIMEOUT
    }
}
HLE(k32_WaitForSingleObject) { uint64_t h = ARG(0); RET(wait_objects(st, 1, &h, 0, (uint32_t)ARG(1))); }
HLE(k32_WaitForSingleObjectEx) { uint64_t h = ARG(0); RET(wait_objects(st, 1, &h, 0, (uint32_t)ARG(1))); }
HLE(k32_WaitForMultipleObjects) {
    uint32_t n = (uint32_t)ARG(0);
    uint64_t hs_[64];
    for (uint32_t i = 0; i < n && i < 64; i++) hs_[i] = rd_ptr(ARG(1) + (uint64_t)PSZ * i);
    RET(wait_objects(st, n, hs_, ARG(2) != 0, (uint32_t)ARG(3)));
}
HLE(k32_DuplicateHandle) {
    uint64_t h = ARG(1);
    if (h == (uint64_t)-2) { // GetCurrentThread(): make a real handle
        HObj *o;
        uint64_t nh = h_alloc(H_THREAD, &o);
        o->ptr = soul_cur_thread();
        h = nh;
    } else {
        HObj *src = h_get(h), *o;
        if (src && (src->type == H_FILE || src->type == H_PROCESS)) { // a real copy (pipes are duplicated this way)
            HObj copy = *src;
            uint64_t nh = h_alloc(src->type, &o);
            if (o) {
                int keep_fd = o->fd;
                *o = copy;
                o->fd = copy.fd >= 0 ? dup(copy.fd) : keep_fd;
                o->dir = NULL;
                if ((uint32_t)hle_arg(st, 6) & 1) h_free(h); // DUPLICATE_CLOSE_SOURCE
                h = nh;
            }
        }
    }
    if (ARG(3)) wr_ptr(ARG(3), h);
    RET(1);
}

// threads
uint64_t k32_create_thread(X86State *st, uint64_t stack, uint64_t fn, uint64_t arg, uint32_t flags, uint64_t tid_out) {
    SoulThread *t = soul_thread_create(st, fn, arg, (size_t)stack, (flags & 4) != 0);
    if (!t) { set_last_error(st, ERROR_NOT_ENOUGH_MEMORY); return 0; }
    HObj *o;
    uint64_t h = h_alloc(H_THREAD, &o);
    o->ptr = t;
    if (tid_out) mem_wr(tid_out, 4, t->tid);
    if (g_proc->verbose) soul_log("[iş parçacığı] #%u oluşturuldu (başlangıç 0x%llx)\n", t->tid, (unsigned long long)fn);
    return h;
}
HLE(k32_CreateThread) { RET(k32_create_thread(st, ARG(1), ARG(2), ARG(3), (uint32_t)hle_arg(st, 4), hle_arg(st, 5))); }
HLE(k32_CreateRemoteThread) { RET(k32_create_thread(st, ARG(2), ARG(3), hle_arg(st, 4), (uint32_t)hle_arg(st, 5), hle_arg(st, 6))); }
HLE(k32_ResumeThread) {
    HObj *o = h_get(ARG(0));
    if (!o || o->type != H_THREAD || !o->ptr) FAIL(ERROR_INVALID_HANDLE);
    SoulThread *t = o->ptr;
    uint32_t prev = t->suspended ? 1 : 0;
    soul_thread_resume(t);
    RET(prev);
}
HLE_STUB_RET(k32_SuspendThread, 0)
HLE(k32_TerminateThread) { FAIL(ERROR_NOT_SUPPORTED); }
HLE(k32_ExitThread) {
    int code = (int)ARG(0);
    SoulThread *t = soul_cur_thread();
    if (!t || t->is_main) { crt_run_atexit(st); soul_exit_process(st, code); }
    soul_exit_thread(st, (uint32_t)code);
}

// TLS: the TEB holds 64 slots (TEB64+0x1480, TEB32+0xE10)
HLE(k32_TlsAlloc) {
    HleState *h = hs();
    if (h->tls_next >= 64) { RET(0xFFFFFFFF); return; }
    uint64_t idx = h->tls_next++;
    wr_ptr(teb_of(st) + TEB_TLSSLOTS + idx * PSZ, 0);
    RET(idx);
}
HLE_STUB_RET(k32_TlsFree, 1)
HLE(k32_TlsGetValue) { uint64_t i = (uint32_t)ARG(0); set_last_error(st, 0); RET(i < 64 ? rd_ptr(teb_of(st) + TEB_TLSSLOTS + i * PSZ) : 0); }
HLE(k32_TlsSetValue) { uint64_t i = (uint32_t)ARG(0); if (i >= 64) FAIL(ERROR_INVALID_PARAMETER); wr_ptr(teb_of(st) + TEB_TLSSLOTS + i * PSZ, ARG(1)); RET(1); }
HLE(k32_FlsAlloc) { HleState *h = hs(); if (h->fls_next >= 128) { RET(0xFFFFFFFF); return; } RET(h->fls_next++); }
HLE_STUB_RET(k32_FlsFree, 1)
HLE(k32_FlsGetValue) { uint64_t i = (uint32_t)ARG(0); RET(i < 128 ? st->thread->fls[i] : 0); }
HLE(k32_FlsSetValue) { uint64_t i = (uint32_t)ARG(0); if (i >= 128) FAIL(ERROR_INVALID_PARAMETER); st->thread->fls[i] = ARG(1); RET(1); }

// Interlocked singly linked lists
#define SL_NEXT (g_mode32 ? 0 : 8)
#define SL_DEPTH (g_mode32 ? 4 : 0)
HLE(k32_InitializeSListHead) { memset(PTR(0), 0, g_mode32 ? 8 : 16); RET(0); }
HLE(k32_InterlockedPushEntrySList) {
    uint64_t head = ARG(0), e = ARG(1);
    uint64_t first = rd_ptr(head + SL_NEXT);
    wr_ptr(e, first);
    wr_ptr(head + SL_NEXT, e);
    mem_wr(head + SL_DEPTH, 2, mem_rd(head + SL_DEPTH, 2) + 1);
    RET(first);
}
HLE(k32_InterlockedPopEntrySList) {
    uint64_t head = ARG(0);
    uint64_t first = rd_ptr(head + SL_NEXT);
    if (first) { wr_ptr(head + SL_NEXT, rd_ptr(first)); mem_wr(head + SL_DEPTH, 2, mem_rd(head + SL_DEPTH, 2) - 1); }
    RET(first);
}
HLE(k32_InterlockedFlushSList) { uint64_t head = ARG(0); uint64_t first = rd_ptr(head + SL_NEXT); wr_ptr(head + SL_NEXT, 0); mem_wr(head + SL_DEPTH, 2, 0); RET(first); }
HLE(k32_QueryDepthSList) { RET(mem_rd(ARG(0) + SL_DEPTH, 2)); }

// ---- memory --------------------------------------------------------------------------------------

HLE(k32_GetProcessHeap) { RET(hs()->process_heap); }
HLE(k32_GetProcessHeaps) { if (ARG(0) >= 1 && ARG(1)) wr_ptr(ARG(1), hs()->process_heap); RET(1); }
HLE(k32_HeapCreate) { RET(hs()->process_heap + 0x10000 + (hs()->api_calls & 0xFFF0)); }
HLE_STUB_RET(k32_HeapDestroy, 1)
HLE(k32_HeapAlloc) {
    size_t n = ARG(2) ? ARG(2) : 1;
    void *p = ((uint32_t)ARG(1) & 8) ? g_calloc(1, n) : g_malloc(n);
    if (!p) set_last_error(st, ERROR_NOT_ENOUGH_MEMORY);
    RET(p);
}
HLE(k32_HeapFree) { g_free(PTR(2)); RET(1); }
HLE(k32_HeapReAlloc) {
    void *old = PTR(2);
    size_t n = ARG(3) ? ARG(3) : 1;
    size_t os = old ? g_msize(old) : 0;
    if ((uint32_t)ARG(1) & 0x10) { RET(n <= os ? old : NULL); return; } // HEAP_REALLOC_IN_PLACE_ONLY
    void *p = g_realloc(old, n);
    if (p && ((uint32_t)ARG(1) & 8) && n > os) memset((char *)p + os, 0, n - os);
    RET(p);
}
HLE(k32_HeapSize) { RET(ARG(2) ? g_msize(PTR(2)) : (uint64_t)-1); }
HLE_STUB_RET(k32_HeapValidate, 1)
HLE_STUB_RET(k32_HeapSetInformation, 1)
HLE(k32_HeapQueryInformation) { if (ARG(2)) mem_wr(ARG(2), 4, 2); RET(1); }
HLE_STUB_RET(k32_HeapLock, 1)
HLE(k32_LocalAlloc) {
    size_t n = ARG(1) ? ARG(1) : 1;
    RET(((uint32_t)ARG(0) & 0x40) ? g_calloc(1, n) : g_malloc(n));
}
HLE(k32_LocalFree) { g_free(PTR(0)); RET(0); }
HLE(k32_LocalReAlloc) { RET(g_realloc(PTR(0), ARG(1) ? ARG(1) : 1)); }
HLE(k32_GlobalLock) { RET(ARG(0)); }
HLE_STUB_RET(k32_GlobalUnlock, 1)
HLE(k32_GlobalSize) { RET(ARG(0) ? g_msize(PTR(0)) : 0); }
HLE(k32_GlobalMemoryStatusEx) {
    uint8_t *b = PTR(0);
    uint32_t len = 64, load = 30;
    uint64_t total = 4ull << 30, avail = 2ull << 30, vtotal = 128ull << 40;
    memcpy(b, &len, 4); memcpy(b + 4, &load, 4); memcpy(b + 8, &total, 8); memcpy(b + 16, &avail, 8);
    memcpy(b + 24, &total, 8); memcpy(b + 32, &avail, 8); memcpy(b + 40, &vtotal, 8); memcpy(b + 48, &vtotal, 8);
    memset(b + 56, 0, 8);
    RET(1);
}
HLE(k32_VirtualAlloc) {
    uint64_t addr = ARG(0);
    size_t size = ARG(1);
    void *p = vm_alloc(addr, size, NULL);
    if (!p) FAIL(ERROR_NOT_ENOUGH_MEMORY);
    if ((uint32_t)ARG(3) & 0xF0) soul_flush_code_cache();
    RET(p);
}
HLE(k32_VirtualFree) {
    uint32_t type = (uint32_t)ARG(2);
    if (type & 0x8000) vm_free(ARG(0)); // MEM_RELEASE
    RET(1);
}
HLE(k32_VirtualProtect) {
    if (ARG(3)) mem_wr(ARG(3), 4, 0x40); // pretend PAGE_EXECUTE_READWRITE
    if ((uint32_t)ARG(2) & 0xF0) soul_flush_code_cache();
    RET(1);
}
uint64_t k32_virtual_query(uint64_t addr, uint8_t *b) {
    uint64_t base, abase, size;
    int type;
    if (g_mode32) { // MEMORY_BASIC_INFORMATION32: 7 DWORDs
        uint32_t v[7] = { 0 };
        if (!vm_query(addr, &base, &abase, &size, &type)) {
            v[0] = (uint32_t)(addr & ~0xFFFull); v[3] = 0x1000; v[4] = 0x10000; v[5] = 1;
        } else {
            v[0] = (uint32_t)base; v[1] = (uint32_t)abase; v[2] = 0x40; v[3] = (uint32_t)size; v[4] = 0x1000; v[5] = 0x40; v[6] = (uint32_t)type;
        }
        memcpy(b, v, 28);
        return 28;
    }
    memset(b, 0, 48);
    if (!vm_query(addr, &base, &abase, &size, &type)) {
        uint64_t page = addr & ~0xFFFull;
        uint32_t state = 0x10000; // MEM_FREE
        memcpy(b, &page, 8);
        uint64_t sz = 0x1000;
        memcpy(b + 24, &sz, 8);
        memcpy(b + 32, &state, 4);
        uint32_t prot = 1;
        memcpy(b + 36, &prot, 4);
        return 48;
    }
    uint32_t prot = 0x40, state = 0x1000;
    memcpy(b, &base, 8); memcpy(b + 8, &abase, 8); memcpy(b + 16, &prot, 4);
    memcpy(b + 24, &size, 8); memcpy(b + 32, &state, 4); memcpy(b + 36, &prot, 4); memcpy(b + 40, &type, 4);
    return 48;
}
HLE(k32_VirtualQuery) { RET(k32_virtual_query(ARG(0), PTR(1))); }
HLE(k32_FlushInstructionCache) { soul_flush_code_cache(); RET(1); }
HLE(k32_IsBadReadPtr) { RET(ARG(0) == 0); }

// ---- strings / locale --------------------------------------------------------------------------

HLE(k32_MultiByteToWideChar) {
    // (cp, flags, src, srclen, dst, dstlen)
    const char *src = CSTR(2);
    int slen = (int)(int32_t)ARG(3);
    uint64_t dst = ARG(4);
    int dlen = (int)(int32_t)hle_arg(st, 5);
    if (!src) FAIL(ERROR_INVALID_PARAMETER);
    long l = slen < 0 ? (long)strlen(src) + 1 : slen;
    size_t need = u2w(src, l, NULL, 0);
    if (!dst || dlen == 0) { RET(need); return; }
    if (need > (size_t)dlen) FAIL(ERROR_INSUFFICIENT_BUFFER);
    uint16_t *tmp = malloc((need + 1) * 2);
    u2w(src, l, tmp, need + 1);
    memcpy(G2H(dst), tmp, need * 2);
    free(tmp);
    RET(need);
}
HLE(k32_WideCharToMultiByte) {
    // (cp, flags, src, srclen, dst, dstlen, defchar, useddef)
    const uint16_t *src = WSTR(2);
    int slen = (int)(int32_t)ARG(3);
    uint64_t dst = hle_arg(st, 4);
    int dlen = (int)(int32_t)hle_arg(st, 5);
    if (hle_arg(st, 7)) mem_wr(hle_arg(st, 7), 4, 0);
    if (!src) FAIL(ERROR_INVALID_PARAMETER);
    long l = slen < 0 ? (long)wstrlen(src) + 1 : slen;
    size_t need = w2u(src, l, NULL, 0);
    if (!dst || dlen == 0) { RET(need); return; }
    if (need > (size_t)dlen) FAIL(ERROR_INSUFFICIENT_BUFFER);
    char *tmp = malloc(need + 1);
    w2u(src, l, tmp, need + 1);
    memcpy(G2H(dst), tmp, need);
    free(tmp);
    RET(need);
}
HLE(k32_GetACP) { RET(65001); }
HLE(k32_GetOEMCP) { RET(65001); }
HLE(k32_GetCPInfo) {
    uint8_t *b = PTR(1);
    memset(b, 0, 20);
    uint32_t maxlen = 4;
    memcpy(b, &maxlen, 4);
    b[4] = '?';
    RET(1);
}
HLE(k32_GetCPInfoExW) { uint8_t *b = PTR(2); memset(b, 0, 544); uint32_t m = 4; memcpy(b, &m, 4); b[4] = '?'; uint32_t cp = 65001; memcpy(b + 28, &cp, 4); RET(1); }
HLE_STUB_RET(k32_IsValidCodePage, 1)
HLE_STUB_RET(k32_IsDBCSLeadByte, 0)
HLE_STUB_RET(k32_IsDBCSLeadByteEx, 0)
HLE(k32_GetStringTypeW) {
    const uint16_t *s = WSTR(1);
    int n = (int)(int32_t)ARG(2);
    if (n < 0) n = (int)wstrlen(s);
    uint16_t *out = PTR(3);
    for (int i = 0; i < n; i++) {
        wint_t c = s[i];
        uint16_t f = 0;
        if (iswupper(c)) f |= 0x1;
        if (iswlower(c)) f |= 0x2;
        if (iswdigit(c)) f |= 0x4;
        if (iswspace(c)) f |= 0x8;
        if (iswpunct(c)) f |= 0x10;
        if (iswcntrl(c)) f |= 0x20;
        if (c == ' ' || c == '\t') f |= 0x40;
        if (iswxdigit(c)) f |= 0x80;
        if (iswalpha(c)) f |= 0x100;
        if ((uint32_t)ARG(0) != 1) f = 0;
        out[i] = f;
    }
    RET(1);
}
HLE(k32_GetStringTypeExW) {
    // (locale, type, src, len, out) -> same as GetStringTypeW(type, src, len, out)
    uint64_t save[4] = { st->gpr[RCX], st->gpr[RDX], st->gpr[R8], st->gpr[R9] };
    st->gpr[RCX] = save[1]; st->gpr[RDX] = save[2]; st->gpr[R8] = save[3]; st->gpr[R9] = hle_arg(st, 4);
    k32_GetStringTypeW(st);
    st->gpr[RCX] = save[0]; st->gpr[RDX] = save[1]; st->gpr[R8] = save[2]; st->gpr[R9] = save[3];
}
static void lcmap(X86State *st, int argbase) {
    uint32_t flags = (uint32_t)ARG(argbase);
    const uint16_t *src = (const uint16_t *)G2H(hle_arg(st, argbase + 1));
    int n = (int)(int32_t)hle_arg(st, argbase + 2);
    uint64_t dst = hle_arg(st, argbase + 3);
    int dn = (int)(int32_t)hle_arg(st, argbase + 4);
    if (n < 0) n = (int)wstrlen(src) + 1;
    if (!dst || !dn) { RET(n); return; }
    if (dn < n) FAIL(ERROR_INSUFFICIENT_BUFFER);
    uint16_t *d = (uint16_t *)G2H(dst);
    for (int i = 0; i < n; i++) {
        wint_t c = src[i];
        if (flags & 0x100) c = uc_lower(c);
        if (flags & 0x200) c = uc_upper(c);
        d[i] = (uint16_t)c;
    }
    RET(n);
}
HLE(k32_LCMapStringW) { lcmap(st, 1); }
HLE(k32_LCMapStringEx) { lcmap(st, 1); }
static int cmp_w(const uint16_t *a, int na, const uint16_t *b, int nb, int icase) {
    if (na < 0) na = (int)wstrlen(a);
    if (nb < 0) nb = (int)wstrlen(b);
    for (int i = 0; i < na && i < nb; i++) {
        wint_t x = a[i], y = b[i];
        if (icase) { x = uc_lower(x); y = uc_lower(y); }
        if (x != y) return x < y ? 1 : 3;
    }
    return na == nb ? 2 : na < nb ? 1 : 3;
}
HLE(k32_CompareStringW) { RET(cmp_w((const uint16_t *)PTR(2), (int)(int32_t)ARG(3), (const uint16_t *)G2H(hle_arg(st, 4)), (int)(int32_t)hle_arg(st, 5), ((uint32_t)ARG(1) & 1) != 0)); }
HLE(k32_CompareStringEx) { RET(cmp_w((const uint16_t *)PTR(2), (int)(int32_t)ARG(3), (const uint16_t *)G2H(hle_arg(st, 4)), (int)(int32_t)hle_arg(st, 5), ((uint32_t)ARG(1) & 1) != 0)); }
HLE(k32_CompareStringOrdinal) { RET(cmp_w(WSTR(0), (int)(int32_t)ARG(1), WSTR(2), (int)(int32_t)ARG(3), ARG(4) != 0)); }
HLE(k32_CompareStringA) {
    const char *a = (const char *)PTR(2), *b = (const char *)G2H(hle_arg(st, 4));
    int na = (int)(int32_t)ARG(3), nb = (int)(int32_t)hle_arg(st, 5);
    if (na < 0) na = (int)strlen(a);
    if (nb < 0) nb = (int)strlen(b);
    int c = ((uint32_t)ARG(1) & 1) ? strncasecmp(a, b, (size_t)(na < nb ? na : nb)) : strncmp(a, b, (size_t)(na < nb ? na : nb));
    if (!c) c = na - nb;
    RET(c < 0 ? 1 : c > 0 ? 3 : 2);
}
static void locale_info(X86State *st, int wide, int lcarg) {
    uint32_t type = (uint32_t)ARG(lcarg) & 0xFFFF;
    const char *v = "";
    char tmp[32];
    switch (type) {
    case 0x1: v = "0409"; break;        // LOCALE_ILANGUAGE
    case 0x2: case 0x1001: v = "English"; break;
    case 0x3: v = "ENU"; break;
    case 0x6: case 0x1002: v = "United States"; break;
    case 0x7: v = "USA"; break;
    case 0xB: v = "437"; break;         // LOCALE_IDEFAULTCODEPAGE
    case 0x1004: v = "65001"; break;     // LOCALE_IDEFAULTANSICODEPAGE
    case 0xE: v = "."; break;           // LOCALE_SDECIMAL
    case 0xF: v = ","; break;           // LOCALE_STHOUSAND
    case 0x10: v = "3;0"; break;
    case 0x14: v = "$"; break;
    case 0x1D: v = "/"; break;
    case 0x1E: v = ":"; break;
    case 0x1F: v = "M/d/yyyy"; break;
    case 0x20: v = "dddd, MMMM d, yyyy"; break;
    case 0x59: v = "en"; break;
    case 0x5A: v = "US"; break;
    case 0x5C: v = "en-US"; break;
    case 0x1009: v = "6"; break;
    case 0x100C: v = "0"; break;
    default: snprintf(tmp, sizeof tmp, "%s", ""); v = tmp; break;
    }
    if ((uint32_t)ARG(lcarg) & 0x20000000) { // LOCALE_RETURN_NUMBER
        uint32_t n = (uint32_t)strtoul(v, NULL, 10);
        if (type == 0x1) n = 0x409;
        if (ARG(lcarg + 1)) mem_wr(ARG(lcarg + 1), 4, n);
        RET(wide ? 2 : 4);
        return;
    }
    uint64_t r = put_str(ARG(lcarg + 1), (uint32_t)ARG(lcarg + 2), v, wide);
    RET((uint32_t)ARG(lcarg + 2) == 0 ? r : r + 1);
}
HLE(k32_GetLocaleInfoA) { locale_info(st, 0, 1); }
HLE(k32_GetLocaleInfoW) { locale_info(st, 1, 1); }
HLE(k32_GetLocaleInfoEx) { locale_info(st, 1, 1); }
HLE(k32_GetUserDefaultLCID) { RET(0x409); }
HLE(k32_GetUserDefaultLocaleName) { RET(put_str(ARG(0), (uint32_t)ARG(1), "en-US", 1) + 1); }
HLE(k32_GetSystemDefaultLocaleName) { RET(put_str(ARG(0), (uint32_t)ARG(1), "en-US", 1) + 1); }
HLE(k32_GetUserDefaultLangID) { RET(0x409); }
HLE(k32_GetUserDefaultUILanguage) { RET(0x409); }
HLE(k32_IsValidLocale) { RET(1); }
HLE(k32_IsValidLocaleName) { RET(1); }
HLE(k32_LocaleNameToLCID) { RET(0x409); }
HLE(k32_LCIDToLocaleName) { RET(put_str(ARG(1), (uint32_t)ARG(2), "en-US", 1) + 1); }
HLE(k32_EnumSystemLocalesW) { RET(1); }
static void date_time_format(X86State *st, int wide, int is_time) {
    // (lcid, flags, SYSTEMTIME*, fmt, buf, cch)
    struct tm r = { 0 };
    if (ARG(2)) {
        uint16_t v[8];
        memcpy(v, PTR(2), 16);
        r.tm_year = v[0] - 1900; r.tm_mon = v[1] - 1; r.tm_wday = v[2]; r.tm_mday = v[3]; r.tm_hour = v[4]; r.tm_min = v[5]; r.tm_sec = v[6];
    } else {
        time_t t = time(NULL);
        localtime_r(&t, &r);
    }
    char buf[128];
    strftime(buf, sizeof buf, is_time ? "%I:%M:%S %p" : "%m/%d/%Y", &r);
    uint64_t res = put_str(hle_arg(st, 4), (uint32_t)hle_arg(st, 5), buf, wide);
    RET((uint32_t)hle_arg(st, 5) == 0 ? res : res + 1);
}
HLE(k32_GetDateFormatA) { date_time_format(st, 0, 0); }
HLE(k32_GetDateFormatW) { date_time_format(st, 1, 0); }
HLE(k32_GetTimeFormatA) { date_time_format(st, 0, 1); }
HLE(k32_GetTimeFormatW) { date_time_format(st, 1, 1); }
HLE(k32_lstrlenA) { RET(ARG(0) ? strlen(CSTR(0)) : 0); }
HLE(k32_lstrlenW) { RET(ARG(0) ? wstrlen(WSTR(0)) : 0); }
HLE(k32_lstrcpyA) { strcpy(PTR(0), CSTR(1)); RET(ARG(0)); }
HLE(k32_lstrcpyW) { const uint16_t *s = WSTR(1); memmove(PTR(0), s, (wstrlen(s) + 1) * 2); RET(ARG(0)); }
HLE(k32_lstrcpynA) { size_t n = (size_t)(int32_t)ARG(2); if (n) { strncpy(PTR(0), CSTR(1), n - 1); ((char *)PTR(0))[n - 1] = 0; } RET(ARG(0)); }
HLE(k32_lstrcpynW) { size_t n = (size_t)(int32_t)ARG(2); const uint16_t *s = WSTR(1); uint16_t *d = PTR(0); size_t i = 0; for (; i + 1 < n && s[i]; i++) d[i] = s[i]; if (n) d[i] = 0; RET(ARG(0)); }
HLE(k32_lstrcatA) { strcat(PTR(0), CSTR(1)); RET(ARG(0)); }
HLE(k32_lstrcatW) { uint16_t *d = PTR(0); const uint16_t *s = WSTR(1); memmove(d + wstrlen(d), s, (wstrlen(s) + 1) * 2); RET(ARG(0)); }
HLE(k32_lstrcmpA) { int c = strcmp(CSTR(0), CSTR(1)); RET((uint64_t)(int64_t)(c < 0 ? -1 : c > 0)); }
HLE(k32_lstrcmpiA) { int c = strcasecmp(CSTR(0), CSTR(1)); RET((uint64_t)(int64_t)(c < 0 ? -1 : c > 0)); }
HLE(k32_lstrcmpW) { int c = cmp_w(WSTR(0), -1, WSTR(1), -1, 0); RET((uint64_t)(int64_t)(c - 2)); }
HLE(k32_lstrcmpiW) { int c = cmp_w(WSTR(0), -1, WSTR(1), -1, 1); RET((uint64_t)(int64_t)(c - 2)); }

static void format_message(X86State *st, int wide) {
    // (flags, source, msgid, langid, buffer, size, args)
    uint32_t flags = (uint32_t)ARG(0);
    char msg[512];
    if (flags & 0x400) { // FROM_STRING
        char *s = arg_str(st, 1, wide);
        snprintf(msg, sizeof msg, "%s", s ? s : "");
        free(s);
    } else {
        uint32_t id = (uint32_t)ARG(2);
        const char *m;
        switch (id) {
        case 0: m = "The operation completed successfully."; break;
        case 2: m = "The system cannot find the file specified."; break;
        case 3: m = "The system cannot find the path specified."; break;
        case 5: m = "Access is denied."; break;
        case 6: m = "The handle is invalid."; break;
        case 8: m = "Not enough memory resources are available to process this command."; break;
        case 87: m = "The parameter is incorrect."; break;
        case 120: m = "This function is not supported on this system."; break;
        default: snprintf(msg, sizeof msg, "Error %u.", id); m = NULL; break;
        }
        if (m) snprintf(msg, sizeof msg, "%s", m);
        if (!(flags & 0x200)) strncat(msg, "\r\n", sizeof msg - strlen(msg) - 1);
    }
    uint64_t buf = hle_arg(st, 4);
    uint32_t size = (uint32_t)hle_arg(st, 5);
    if (flags & 0x100) { // ALLOCATE_BUFFER
        size_t n = wide ? (u2w(msg, -1, NULL, 0) + 1) * 2 : strlen(msg) + 1;
        void *p = g_malloc(n);
        if (wide) u2w(msg, -1, p, n / 2); else memcpy(p, msg, n);
        wr_ptr(buf, H2G(p));
        RET(wide ? n / 2 - 1 : n - 1);
        return;
    }
    uint64_t r = put_str(buf, size, msg, wide);
    RET(r < size ? r : 0);
}
HLE(k32_FormatMessageA) { format_message(st, 0); }
HLE(k32_FormatMessageW) { format_message(st, 1); }
HLE(k32_Beep) { con_write(1, "\a", 1); RET(1); }
static void sysdir(X86State *st, int wide, const char *d) { RET(put_str(ARG(0), (uint32_t)ARG(1), d, wide)); }
HLE(k32_GetSystemDirectoryA) { sysdir(st, 0, "C:\\windows\\system32"); }
HLE(k32_GetSystemDirectoryW) { sysdir(st, 1, "C:\\windows\\system32"); }
HLE(k32_GetWindowsDirectoryA) { sysdir(st, 0, "C:\\windows"); }
HLE(k32_GetWindowsDirectoryW) { sysdir(st, 1, "C:\\windows"); }
static void computer_name(X86State *st, int wide) {
    uint32_t cap = (uint32_t)mem_rd(ARG(1), 4);
    uint64_t r = put_str(ARG(0), cap, "IPAD", wide);
    if (r >= cap) { mem_wr(ARG(1), 4, r); FAIL(111); }
    mem_wr(ARG(1), 4, r);
    RET(1);
}
HLE(k32_GetComputerNameA) { computer_name(st, 0); }
HLE(k32_GetComputerNameW) { computer_name(st, 1); }
HLE(k32_GetComputerNameExW) { uint32_t cap = (uint32_t)mem_rd(ARG(2), 4); uint64_t r = put_str(ARG(1), cap, "IPAD", 1); mem_wr(ARG(2), 4, r); RET(r < cap); }

// file mapping (read-only snapshot / anonymous)
HLE(k32_CreateFileMappingW) {
    HObj *f = ARG(0) != (uint64_t)-1 ? h_get(ARG(0)) : NULL;
    uint64_t size = ((uint64_t)(uint32_t)ARG(3) << 32) | (uint32_t)hle_arg(st, 4);
    if (f && f->fd >= 0 && !size) { struct stat sb; fstat(f->fd, &sb); size = (uint64_t)sb.st_size; }
    if (!size) FAIL(ERROR_INVALID_PARAMETER);
    HObj *o;
    uint64_t h = h_alloc(H_MAPPING, &o);
    o->ptr = g_calloc(1, size);
    o->len = size;
    if (f && f->fd >= 0) { off_t pos = lseek(f->fd, 0, SEEK_CUR); lseek(f->fd, 0, SEEK_SET); ssize_t rr = read(f->fd, o->ptr, size); (void)rr; lseek(f->fd, pos, SEEK_SET); }
    RET(h);
}
HLE(k32_MapViewOfFile) {
    HObj *o = h_get(ARG(0));
    if (!o || o->type != H_MAPPING) FAIL(ERROR_INVALID_HANDLE);
    uint64_t off = ((uint64_t)(uint32_t)ARG(2) << 32) | (uint32_t)ARG(3);
    RET((uint8_t *)o->ptr + off);
}
HLE_STUB_RET(k32_UnmapViewOfFile, 1)

// ---- table --------------------------------------------------------------------------------------

const HleEntry hle_kernel32_table[] = {
    { "GetStdHandle", k32_GetStdHandle, NULL }, { "SetStdHandle", k32_SetStdHandle, NULL }, { "WriteFile", k32_WriteFile, NULL },
    { "ReadFile", k32_ReadFile, NULL }, { "WriteConsoleA", k32_WriteConsoleA, NULL }, { "WriteConsoleW", k32_WriteConsoleW, NULL },
    { "ReadConsoleA", k32_ReadConsoleA, NULL }, { "ReadConsoleW", k32_ReadConsoleW, NULL }, { "ReadConsoleInputW", k32_ReadConsoleInputW, NULL },
    { "ReadConsoleInputA", k32_ReadConsoleInputW, NULL }, { "PeekConsoleInputW", k32_PeekConsoleInputW, NULL }, { "PeekConsoleInputA", k32_PeekConsoleInputW, NULL },
    { "GetNumberOfConsoleInputEvents", k32_GetNumberOfConsoleInputEvents, NULL }, { "GetConsoleMode", k32_GetConsoleMode, NULL },
    { "SetConsoleMode", k32_SetConsoleMode, NULL }, { "SetConsoleTitleA", k32_SetConsoleTitleA, NULL }, { "SetConsoleTitleW", k32_SetConsoleTitleW, NULL },
    { "GetConsoleTitleA", k32_GetConsoleTitleA, NULL }, { "GetConsoleTitleW", k32_GetConsoleTitleW, NULL },
    { "SetConsoleTextAttribute", k32_SetConsoleTextAttribute, NULL }, { "GetConsoleScreenBufferInfo", k32_GetConsoleScreenBufferInfo, NULL },
    { "GetConsoleScreenBufferInfoEx", k32_GetConsoleScreenBufferInfoEx, NULL }, { "SetConsoleCursorPosition", k32_SetConsoleCursorPosition, NULL },
    { "FillConsoleOutputCharacterA", k32_FillConsoleOutputCharacter, NULL }, { "FillConsoleOutputCharacterW", k32_FillConsoleOutputCharacter, NULL },
    { "FillConsoleOutputAttribute", k32_FillConsoleOutputAttribute, NULL }, { "GetConsoleCursorInfo", k32_GetConsoleCursorInfo, NULL },
    { "SetConsoleCursorInfo", k32_SetConsoleCursorInfo, NULL }, { "GetConsoleCP", k32_GetConsoleCP, NULL }, { "GetConsoleOutputCP", k32_GetConsoleCP, NULL },
    { "SetConsoleCP", k32_SetConsoleCP, NULL }, { "SetConsoleOutputCP", k32_SetConsoleCP, NULL }, { "SetConsoleCtrlHandler", k32_SetConsoleCtrlHandler, NULL },
    { "FlushConsoleInputBuffer", k32_FlushConsoleInputBuffer, NULL }, { "GetConsoleWindow", k32_GetConsoleWindow, NULL }, { "AllocConsole", k32_AllocConsole, NULL },
    { "FreeConsole", k32_FreeConsole, NULL }, { "AttachConsole", k32_AttachConsole, NULL }, { "SetConsoleScreenBufferSize", k32_SetConsoleScreenBufferSize, NULL },
    { "SetConsoleWindowInfo", k32_SetConsoleWindowInfo, NULL }, { "GetLargestConsoleWindowSize", k32_GetLargestConsoleWindowSize, NULL },
    { "WriteConsoleOutputW", k32_WriteConsoleOutputW, NULL }, { "WriteConsoleOutputA", k32_WriteConsoleOutputW, NULL }, { "GetFileType", k32_GetFileType, NULL },
    { "CreateFileA", k32_CreateFileA, NULL }, { "CreateFileW", k32_CreateFileW, NULL }, { "CreateFile2", k32_CreateFile2, NULL },
    { "CloseHandle", k32_CloseHandle, NULL }, { "GetFileSize", k32_GetFileSize, NULL }, { "GetFileSizeEx", k32_GetFileSizeEx, NULL },
    { "SetFilePointer", k32_SetFilePointer, NULL }, { "SetFilePointerEx", k32_SetFilePointerEx, NULL }, { "SetEndOfFile", k32_SetEndOfFile, NULL },
    { "FlushFileBuffers", k32_FlushFileBuffers, NULL }, { "GetFileInformationByHandle", k32_GetFileInformationByHandle, NULL },
    { "GetFileTime", k32_GetFileTime, NULL }, { "SetFileTime", k32_SetFileTime, NULL }, { "CompareFileTime", k32_CompareFileTime, NULL },
    { "DeleteFileA", k32_DeleteFileA, NULL }, { "DeleteFileW", k32_DeleteFileW, NULL }, { "CreateDirectoryA", k32_CreateDirectoryA, NULL },
    { "CreateDirectoryW", k32_CreateDirectoryW, NULL }, { "RemoveDirectoryA", k32_RemoveDirectoryA, NULL }, { "RemoveDirectoryW", k32_RemoveDirectoryW, NULL },
    { "MoveFileA", k32_MoveFileA, NULL }, { "MoveFileW", k32_MoveFileW, NULL }, { "MoveFileExA", k32_MoveFileExA, NULL }, { "MoveFileExW", k32_MoveFileExW, NULL },
    { "CopyFileA", k32_CopyFileA, NULL }, { "CopyFileW", k32_CopyFileW, NULL }, { "GetFileAttributesA", k32_GetFileAttributesA, NULL },
    { "GetFileAttributesW", k32_GetFileAttributesW, NULL }, { "GetFileAttributesExA", k32_GetFileAttributesExA, NULL },
    { "GetFileAttributesExW", k32_GetFileAttributesExW, NULL }, { "SetFileAttributesA", k32_SetFileAttributesA, NULL },
    { "SetFileAttributesW", k32_SetFileAttributesW, NULL }, { "FindFirstFileA", k32_FindFirstFileA, NULL }, { "FindFirstFileW", k32_FindFirstFileW, NULL },
    { "FindFirstFileExA", k32_FindFirstFileExA, NULL }, { "FindFirstFileExW", k32_FindFirstFileExW, NULL }, { "FindNextFileA", k32_FindNextFileA, NULL },
    { "FindNextFileW", k32_FindNextFileW, NULL }, { "FindClose", k32_FindClose, NULL }, { "GetCurrentDirectoryA", k32_GetCurrentDirectoryA, NULL },
    { "GetCurrentDirectoryW", k32_GetCurrentDirectoryW, NULL }, { "SetCurrentDirectoryA", k32_SetCurrentDirectoryA, NULL },
    { "SetCurrentDirectoryW", k32_SetCurrentDirectoryW, NULL }, { "GetFullPathNameA", k32_GetFullPathNameA, NULL }, { "GetFullPathNameW", k32_GetFullPathNameW, NULL },
    { "GetLongPathNameA", k32_GetLongPathNameA, NULL }, { "GetLongPathNameW", k32_GetLongPathNameW, NULL }, { "GetShortPathNameW", k32_GetLongPathNameW, NULL },
    { "GetTempPathA", k32_GetTempPathA, NULL }, { "GetTempPathW", k32_GetTempPathW, NULL }, { "GetTempPath2W", k32_GetTempPathW, NULL },
    { "GetTempFileNameA", k32_GetTempFileNameA, NULL }, { "GetTempFileNameW", k32_GetTempFileNameW, NULL }, { "GetDriveTypeA", k32_GetDriveTypeA, NULL },
    { "GetDriveTypeW", k32_GetDriveTypeA, NULL }, { "GetLogicalDrives", k32_GetLogicalDrives, NULL }, { "GetLogicalDriveStringsA", k32_GetLogicalDriveStringsA, NULL },
   
    { "GetVolumeInformationW", k32_GetVolumeInformationW, NULL },
    { "LoadLibraryA", k32_LoadLibraryA, NULL }, { "LoadLibraryW", k32_LoadLibraryW, NULL }, { "LoadLibraryExA", k32_LoadLibraryExA, NULL },
    { "LoadLibraryExW", k32_LoadLibraryExW, NULL }, { "FreeLibrary", k32_FreeLibrary, NULL }, { "SetDllDirectoryA", k32_SetDllDirectoryA, NULL }, { "SetDllDirectoryW", k32_SetDllDirectoryW, NULL },
    { "AddDllDirectory", k32_AddDllDirectory, NULL }, { "SetDefaultDllDirectories", k32_SetDefaultDllDirectories, NULL }, { "RemoveDllDirectory", k32_RemoveDllDirectory, NULL }, { "GetModuleHandleA", k32_GetModuleHandleA, NULL },
    { "GetModuleHandleW", k32_GetModuleHandleW, NULL }, { "GetModuleHandleExA", k32_GetModuleHandleExA, NULL }, { "GetModuleHandleExW", k32_GetModuleHandleExW, NULL },
    { "GetModuleFileNameA", k32_GetModuleFileNameA, NULL }, { "GetModuleFileNameW", k32_GetModuleFileNameW, NULL }, { "GetProcAddress", k32_GetProcAddress, NULL },
    { "DisableThreadLibraryCalls", k32_DisableThreadLibraryCalls, NULL },
    { "GetCommandLineA", k32_GetCommandLineA, NULL }, { "GetCommandLineW", k32_GetCommandLineW, NULL },
    { "GetEnvironmentVariableA", k32_GetEnvironmentVariableA, NULL }, { "GetEnvironmentVariableW", k32_GetEnvironmentVariableW, NULL },
    { "SetEnvironmentVariableA", k32_SetEnvironmentVariableA, NULL }, { "SetEnvironmentVariableW", k32_SetEnvironmentVariableW, NULL },
    { "GetEnvironmentStrings", k32_GetEnvironmentStringsA, NULL }, { "GetEnvironmentStringsA", k32_GetEnvironmentStringsA, NULL },
    { "GetEnvironmentStringsW", k32_GetEnvironmentStringsW, NULL }, { "FreeEnvironmentStringsA", k32_FreeEnvironmentStrings, NULL },
    { "FreeEnvironmentStringsW", k32_FreeEnvironmentStrings, NULL }, { "ExpandEnvironmentStringsA", k32_ExpandEnvironmentStringsA, NULL },
    { "ExpandEnvironmentStringsW", k32_ExpandEnvironmentStringsW, NULL }, { "GetStartupInfoA", k32_GetStartupInfoA, NULL },
    { "GetStartupInfoW", k32_GetStartupInfoW, NULL }, { "ExitProcess", k32_ExitProcess, NULL }, { "TerminateProcess", k32_TerminateProcess, NULL },
    { "GetCurrentProcess", k32_GetCurrentProcess, NULL }, { "GetCurrentThread", k32_GetCurrentThread, NULL }, { "GetCurrentProcessId", k32_GetCurrentProcessId, NULL },
    { "GetCurrentThreadId", k32_GetCurrentThreadId, NULL }, { "GetProcessId", k32_GetProcessId, NULL }, { "GetThreadId", k32_GetThreadId, NULL },
    { "GetLastError", k32_GetLastError, NULL }, { "SetLastError", k32_SetLastError, NULL }, { "RtlSetLastWin32Error", k32_SetLastError, NULL },
    { "GetExitCodeProcess", k32_GetExitCodeProcess, NULL }, { "GetExitCodeThread", k32_GetExitCodeThread, NULL }, { "GetVersion", k32_GetVersion, NULL },
    { "GetVersionExA", k32_GetVersionExA, NULL }, { "GetVersionExW", k32_GetVersionExW, NULL }, { "RtlGetVersion", ntdll_RtlGetVersion, NULL },
    { "VerifyVersionInfoW", k32_VerifyVersionInfoW, NULL }, { "VerSetConditionMask", k32_VerSetConditionMask, NULL },
    { "GetSystemInfo", k32_GetSystemInfo, NULL }, { "GetNativeSystemInfo", k32_GetNativeSystemInfo, NULL }, { "IsProcessorFeaturePresent", k32_IsProcessorFeaturePresent, NULL },
    { "IsDebuggerPresent", k32_IsDebuggerPresent, NULL }, { "CheckRemoteDebuggerPresent", k32_CheckRemoteDebuggerPresent, NULL },
    { "OutputDebugStringA", k32_OutputDebugStringA, NULL }, { "OutputDebugStringW", k32_OutputDebugStringW, NULL }, { "DebugBreak", k32_DebugBreak, NULL },
    { "RaiseException", k32_RaiseException, NULL }, { "SetUnhandledExceptionFilter", k32_SetUnhandledExceptionFilter, NULL },
    { "UnhandledExceptionFilter", k32_UnhandledExceptionFilter, NULL }, { "AddVectoredExceptionHandler", k32_AddVectoredExceptionHandler, NULL },
    { "RemoveVectoredExceptionHandler", k32_RemoveVectoredExceptionHandler, NULL }, { "AddVectoredContinueHandler", k32_AddVectoredExceptionHandler, NULL },
    { "RtlCaptureContext", k32_RtlCaptureContext, NULL }, { "RtlLookupFunctionEntry", k32_RtlLookupFunctionEntry, NULL },
    { "RtlVirtualUnwind", k32_RtlVirtualUnwind, NULL }, { "RtlUnwindEx", k32_RtlUnwindEx, NULL }, { "RtlUnwind", k32_RtlUnwind, NULL },
    { "NtContinue", k32_NtContinue, NULL }, { "ZwContinue", k32_NtContinue, NULL }, { "RtlRaiseException", k32_RtlRaiseException, NULL },
    { "RtlPcToFileHeader", k32_RtlPcToFileHeader, NULL }, { "EncodePointer", k32_EncodePointer, NULL }, { "DecodePointer", k32_EncodePointer, NULL },
    { "RtlEncodePointer", k32_EncodePointer, NULL }, { "RtlDecodePointer", k32_EncodePointer, NULL },
    { "SetErrorMode", k32_SetErrorMode, NULL }, { "GetErrorMode", k32_GetErrorMode, NULL }, { "SetThreadErrorMode", k32_SetErrorMode, NULL },
    { "SetProcessAffinityMask", k32_SetProcessAffinityMask, NULL }, { "GetProcessAffinityMask", k32_GetProcessAffinityMask, NULL },
    { "SetThreadPriority", k32_SetThreadPriority, NULL }, { "GetThreadPriority", k32_GetThreadPriority, NULL }, { "SetPriorityClass", k32_SetPriorityClass, NULL },
    { "GetPriorityClass", k32_GetPriorityClass, NULL }, { "GetCurrentProcessorNumber", k32_GetCurrentProcessorNumber, NULL }, { "IsWow64Process", k32_IsWow64Process, NULL },
    { "SetThreadDescription", k32_SetThreadDescription, NULL }, { "GetProcessTimes", k32_GetProcessTimes, NULL }, { "GetThreadTimes", k32_GetProcessTimes, NULL },
    { "CreateProcessA", k32_CreateProcessA, NULL }, { "CreateProcessW", k32_CreateProcessA, NULL },
    { "Sleep", k32_Sleep, NULL }, { "SleepEx", k32_SleepEx, NULL }, { "SwitchToThread", k32_SwitchToThread, NULL }, { "GetTickCount", k32_GetTickCount, NULL },
    { "GetTickCount64", k32_GetTickCount64, NULL }, { "QueryPerformanceCounter", k32_QueryPerformanceCounter, NULL },
    { "QueryPerformanceFrequency", k32_QueryPerformanceFrequency, NULL }, { "GetSystemTimeAsFileTime", k32_GetSystemTimeAsFileTime, NULL },
    { "GetSystemTimePreciseAsFileTime", k32_GetSystemTimeAsFileTime, NULL }, { "GetSystemTime", k32_GetSystemTime, NULL }, { "GetLocalTime", k32_GetLocalTime, NULL },
    { "FileTimeToSystemTime", k32_FileTimeToSystemTime, NULL }, { "FileTimeToLocalFileTime", k32_FileTimeToLocalFileTime, NULL },
    { "LocalFileTimeToFileTime", k32_FileTimeToLocalFileTime, NULL }, { "SystemTimeToFileTime", k32_SystemTimeToFileTime, NULL },
    { "SystemTimeToTzSpecificLocalTime", k32_FileTimeToLocalFileTime, NULL },
    { "GetTimeZoneInformation", k32_GetTimeZoneInformation, NULL }, { "GetDynamicTimeZoneInformation", k32_GetDynamicTimeZoneInformation, NULL },
    { "InitializeCriticalSection", k32_InitializeCriticalSection, NULL }, { "InitializeCriticalSectionAndSpinCount", k32_InitializeCriticalSectionEx, NULL },
    { "InitializeCriticalSectionEx", k32_InitializeCriticalSectionEx, NULL }, { "EnterCriticalSection", k32_EnterCriticalSection, NULL },
    { "LeaveCriticalSection", k32_LeaveCriticalSection, NULL }, { "TryEnterCriticalSection", k32_TryEnterCriticalSection, NULL },
    { "DeleteCriticalSection", k32_DeleteCriticalSection, NULL }, { "InitializeSRWLock", k32_InitializeSRWLock, NULL },
    { "AcquireSRWLockExclusive", k32_AcquireSRWLockExclusive, NULL }, { "AcquireSRWLockShared", k32_AcquireSRWLockShared, NULL },
    { "ReleaseSRWLockExclusive", k32_ReleaseSRWLockExclusive, NULL }, { "ReleaseSRWLockShared", k32_ReleaseSRWLockShared, NULL },
    { "TryAcquireSRWLockExclusive", k32_TryAcquireSRWLockExclusive, NULL }, { "TryAcquireSRWLockShared", k32_TryAcquireSRWLockShared, NULL },
    { "WaitOnAddress", k32_WaitOnAddress, NULL }, { "WakeByAddressSingle", k32_WakeByAddress, NULL }, { "WakeByAddressAll", k32_WakeByAddress, NULL },
    { "ResumeThread", k32_ResumeThread, NULL }, { "SuspendThread", k32_SuspendThread, NULL }, { "TerminateThread", k32_TerminateThread, NULL },
    { "CreateRemoteThread", k32_CreateRemoteThread, NULL },
    { "InitializeConditionVariable", k32_InitializeConditionVariable, NULL }, { "WakeConditionVariable", k32_WakeConditionVariable, NULL },
    { "WakeAllConditionVariable", k32_WakeConditionVariable, NULL }, { "SleepConditionVariableCS", k32_SleepConditionVariableCS, NULL },
    { "SleepConditionVariableSRW", k32_SleepConditionVariableSRW, NULL }, { "InitOnceExecuteOnce", k32_InitOnceExecuteOnce, NULL },
    { "InitOnceBeginInitialize", k32_InitOnceBeginInitialize, NULL }, { "InitOnceComplete", k32_InitOnceComplete, NULL },
    { "CreateEventA", k32_CreateEventA, NULL }, { "CreateEventW", k32_CreateEventW, NULL }, { "CreateEventExW", k32_CreateEventExW, NULL },
    { "SetEvent", k32_SetEvent, NULL }, { "ResetEvent", k32_ResetEvent, NULL }, { "OpenEventA", k32_OpenEventA, NULL }, { "OpenEventW", k32_OpenEventW, NULL }, { "OpenMutexA", k32_OpenMutexA, NULL },
    { "OpenMutexW", k32_OpenMutexW, NULL }, { "OpenSemaphoreA", k32_OpenSemaphoreA, NULL }, { "OpenSemaphoreW", k32_OpenSemaphoreW, NULL },
    { "CreateMutexA", k32_CreateMutexA, NULL }, { "CreateMutexW", k32_CreateMutexW, NULL },
    { "CreateMutexExW", k32_CreateMutexExW, NULL }, { "ReleaseMutex", k32_ReleaseMutex, NULL }, { "CreateSemaphoreA", k32_CreateSemaphoreA, NULL },
    { "CreateSemaphoreW", k32_CreateSemaphoreW, NULL }, { "CreateSemaphoreExW", k32_CreateSemaphoreExW, NULL }, { "ReleaseSemaphore", k32_ReleaseSemaphore, NULL },
    { "WaitForSingleObject", k32_WaitForSingleObject, NULL }, { "WaitForSingleObjectEx", k32_WaitForSingleObjectEx, NULL },
    { "WaitForMultipleObjects", k32_WaitForMultipleObjects, NULL }, { "WaitForMultipleObjectsEx", k32_WaitForMultipleObjects, NULL },
    { "DuplicateHandle", k32_DuplicateHandle, NULL }, { "CreateThread", k32_CreateThread, NULL }, { "ExitThread", k32_ExitThread, NULL },
    { "TlsAlloc", k32_TlsAlloc, NULL }, { "TlsFree", k32_TlsFree, NULL }, { "TlsGetValue", k32_TlsGetValue, NULL }, { "TlsSetValue", k32_TlsSetValue, NULL },
    { "FlsAlloc", k32_FlsAlloc, NULL }, { "FlsFree", k32_FlsFree, NULL }, { "FlsGetValue", k32_FlsGetValue, NULL }, { "FlsSetValue", k32_FlsSetValue, NULL },
    { "InitializeSListHead", k32_InitializeSListHead, NULL }, { "InterlockedPushEntrySList", k32_InterlockedPushEntrySList, NULL },
    { "InterlockedPopEntrySList", k32_InterlockedPopEntrySList, NULL }, { "InterlockedFlushSList", k32_InterlockedFlushSList, NULL },
    { "QueryDepthSList", k32_QueryDepthSList, NULL }, { "RtlInitializeSListHead", k32_InitializeSListHead, NULL },
    { "GetProcessHeap", k32_GetProcessHeap, NULL }, { "GetProcessHeaps", k32_GetProcessHeaps, NULL }, { "HeapCreate", k32_HeapCreate, NULL },
    { "HeapDestroy", k32_HeapDestroy, NULL }, { "HeapAlloc", k32_HeapAlloc, NULL }, { "RtlAllocateHeap", k32_HeapAlloc, NULL }, { "HeapFree", k32_HeapFree, NULL },
    { "RtlFreeHeap", k32_HeapFree, NULL }, { "HeapReAlloc", k32_HeapReAlloc, NULL }, { "RtlReAllocateHeap", k32_HeapReAlloc, NULL }, { "HeapSize", k32_HeapSize, NULL },
    { "RtlSizeHeap", k32_HeapSize, NULL }, { "HeapValidate", k32_HeapValidate, NULL }, { "HeapSetInformation", k32_HeapSetInformation, NULL },
    { "HeapQueryInformation", k32_HeapQueryInformation, NULL }, { "HeapLock", k32_HeapLock, NULL }, { "HeapUnlock", k32_HeapLock, NULL }, { "HeapCompact", k32_HeapLock, NULL },
    { "LocalAlloc", k32_LocalAlloc, NULL }, { "LocalFree", k32_LocalFree, NULL }, { "LocalReAlloc", k32_LocalReAlloc, NULL },
    { "GlobalAlloc", k32_LocalAlloc, NULL }, { "GlobalFree", k32_LocalFree, NULL }, { "GlobalReAlloc", k32_LocalReAlloc, NULL }, { "GlobalLock", k32_GlobalLock, NULL },
    { "GlobalUnlock", k32_GlobalUnlock, NULL }, { "GlobalSize", k32_GlobalSize, NULL }, { "GlobalMemoryStatusEx", k32_GlobalMemoryStatusEx, NULL },
    { "VirtualAlloc", k32_VirtualAlloc, NULL }, { "VirtualFree", k32_VirtualFree, NULL }, { "VirtualProtect", k32_VirtualProtect, NULL },
    { "VirtualQuery", k32_VirtualQuery, NULL }, { "FlushInstructionCache", k32_FlushInstructionCache, NULL }, { "IsBadReadPtr", k32_IsBadReadPtr, NULL },
    { "IsBadWritePtr", k32_IsBadReadPtr, NULL }, { "IsBadCodePtr", k32_IsBadReadPtr, NULL },
    { "MultiByteToWideChar", k32_MultiByteToWideChar, NULL }, { "WideCharToMultiByte", k32_WideCharToMultiByte, NULL }, { "GetACP", k32_GetACP, NULL },
    { "GetOEMCP", k32_GetOEMCP, NULL }, { "GetCPInfo", k32_GetCPInfo, NULL }, { "GetCPInfoExW", k32_GetCPInfoExW, NULL }, { "IsValidCodePage", k32_IsValidCodePage, NULL },
    { "IsDBCSLeadByte", k32_IsDBCSLeadByte, NULL }, { "IsDBCSLeadByteEx", k32_IsDBCSLeadByteEx, NULL }, { "GetStringTypeW", k32_GetStringTypeW, NULL },
    { "GetStringTypeExW", k32_GetStringTypeExW, NULL }, { "LCMapStringW", k32_LCMapStringW, NULL }, { "LCMapStringEx", k32_LCMapStringEx, NULL },
    { "CompareStringW", k32_CompareStringW, NULL }, { "CompareStringEx", k32_CompareStringEx, NULL }, { "CompareStringOrdinal", k32_CompareStringOrdinal, NULL },
    { "CompareStringA", k32_CompareStringA, NULL }, { "GetLocaleInfoA", k32_GetLocaleInfoA, NULL }, { "GetLocaleInfoW", k32_GetLocaleInfoW, NULL },
    { "GetLocaleInfoEx", k32_GetLocaleInfoEx, NULL }, { "GetUserDefaultLCID", k32_GetUserDefaultLCID, NULL }, { "GetThreadLocale", k32_GetUserDefaultLCID, NULL },
    { "GetSystemDefaultLCID", k32_GetUserDefaultLCID, NULL }, { "GetUserDefaultLocaleName", k32_GetUserDefaultLocaleName, NULL },
    { "GetSystemDefaultLocaleName", k32_GetSystemDefaultLocaleName, NULL }, { "GetUserDefaultLangID", k32_GetUserDefaultLangID, NULL },
    { "GetSystemDefaultLangID", k32_GetUserDefaultLangID, NULL }, { "GetUserDefaultUILanguage", k32_GetUserDefaultUILanguage, NULL },
    { "GetSystemDefaultUILanguage", k32_GetUserDefaultUILanguage, NULL }, { "IsValidLocale", k32_IsValidLocale, NULL }, { "IsValidLocaleName", k32_IsValidLocaleName, NULL },
    { "LocaleNameToLCID", k32_LocaleNameToLCID, NULL }, { "LCIDToLocaleName", k32_LCIDToLocaleName, NULL }, { "EnumSystemLocalesW", k32_EnumSystemLocalesW, NULL },
    { "EnumSystemLocalesEx", k32_EnumSystemLocalesW, NULL }, { "GetDateFormatA", k32_GetDateFormatA, NULL }, { "GetDateFormatW", k32_GetDateFormatW, NULL },
    { "GetTimeFormatA", k32_GetTimeFormatA, NULL }, { "GetTimeFormatW", k32_GetTimeFormatW, NULL }, { "GetDateFormatEx", k32_GetDateFormatW, NULL },
    { "GetTimeFormatEx", k32_GetTimeFormatW, NULL },
    { "lstrlenA", k32_lstrlenA, NULL }, { "lstrlenW", k32_lstrlenW, NULL }, { "lstrlen", k32_lstrlenA, NULL }, { "lstrcpyA", k32_lstrcpyA, NULL },
    { "lstrcpyW", k32_lstrcpyW, NULL }, { "lstrcpynA", k32_lstrcpynA, NULL }, { "lstrcpynW", k32_lstrcpynW, NULL }, { "lstrcatA", k32_lstrcatA, NULL },
    { "lstrcatW", k32_lstrcatW, NULL }, { "lstrcmpA", k32_lstrcmpA, NULL }, { "lstrcmpiA", k32_lstrcmpiA, NULL }, { "lstrcmpW", k32_lstrcmpW, NULL },
    { "lstrcmpiW", k32_lstrcmpiW, NULL }, { "FormatMessageA", k32_FormatMessageA, NULL }, { "FormatMessageW", k32_FormatMessageW, NULL }, { "Beep", k32_Beep, NULL },
    { "GetSystemDirectoryA", k32_GetSystemDirectoryA, NULL }, { "GetSystemDirectoryW", k32_GetSystemDirectoryW, NULL },
    { "GetWindowsDirectoryA", k32_GetWindowsDirectoryA, NULL }, { "GetWindowsDirectoryW", k32_GetWindowsDirectoryW, NULL },
    { "GetSystemWindowsDirectoryW", k32_GetWindowsDirectoryW, NULL }, { "GetComputerNameA", k32_GetComputerNameA, NULL },
    { "GetComputerNameW", k32_GetComputerNameW, NULL }, { "GetComputerNameExW", k32_GetComputerNameExW, NULL },
    { "CreateFileMappingW", k32_CreateFileMappingW, NULL }, { "CreateFileMappingA", k32_CreateFileMappingW, NULL }, { "MapViewOfFile", k32_MapViewOfFile, NULL },
    { "UnmapViewOfFile", k32_UnmapViewOfFile, NULL },
    { NULL, NULL, NULL }
};

// Used by user32's MsgWaitForMultipleObjects (polls with a 0 timeout).
uint32_t k32_wait_objects(X86State *st, uint32_t n, const uint64_t *h, int all, uint32_t ms) { return wait_objects(st, n, h, all, ms); }
