// SoulCore - Winsock (ws2_32 / wsock32) on the host's BSD sockets.
//
// Windows' socket structures are converted to the host's (Apple's sockaddr starts with a length
// byte, and AF_INET6 has a different number everywhere). Host sockets are always non-blocking;
// a blocking call waits without the global lock, so other guest threads and processes go on.
#include "hle.h"
#include "gui.h"
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <poll.h>
#include <pthread.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>

#define CSTR(n) ((const char *)G2H(hle_arg(st, (n))))
#define WSTR(n) ((const uint16_t *)G2H(hle_arg(st, (n))))
#define ARGI(n) ((int)(int32_t)hle_arg(st, (n)))
#define INVALID_SOCK (g_mode32 ? 0xFFFFFFFFull : ~0ull)
#define SOCK_BASE 0x2000u
#define MAX_FD 8192

enum { WSAEINTR = 10004, WSAEBADF = 10009, WSAEACCES = 10013, WSAEFAULT = 10014, WSAEINVAL = 10022, WSAEMFILE = 10024,
       WSAEWOULDBLOCK = 10035, WSAEINPROGRESS = 10036, WSAEALREADY = 10037, WSAENOTSOCK = 10038, WSAEDESTADDRREQ = 10039,
       WSAEMSGSIZE = 10040, WSAEPROTONOSUPPORT = 10043, WSAEOPNOTSUPP = 10045, WSAEAFNOSUPPORT = 10047, WSAEADDRINUSE = 10048,
       WSAEADDRNOTAVAIL = 10049, WSAENETDOWN = 10050, WSAENETUNREACH = 10051, WSAECONNABORTED = 10053, WSAECONNRESET = 10054,
       WSAENOBUFS = 10055, WSAEISCONN = 10056, WSAENOTCONN = 10057, WSAESHUTDOWN = 10058, WSAETIMEDOUT = 10060,
       WSAECONNREFUSED = 10061, WSAEHOSTUNREACH = 10065, WSANOTINITIALISED = 10093, WSAHOST_NOT_FOUND = 11001,
       WSATRY_AGAIN = 11002, WSANO_DATA = 11004 };

// per-socket options the host cannot keep for us (sockets are host-wide, so this table is too)
typedef struct SockInfo { uint8_t nonblock, used; int rcv_ms, snd_ms; } SockInfo;
static SockInfo g_sock[MAX_FD];

static int wsa_err(int e) {
    switch (e) {
    case EWOULDBLOCK: return WSAEWOULDBLOCK;
#if EAGAIN != EWOULDBLOCK
    case EAGAIN: return WSAEWOULDBLOCK;
#endif
    case EINPROGRESS: return WSAEWOULDBLOCK; // a non-blocking connect is "would block" on Windows
    case EALREADY: return WSAEALREADY;
    case ENOTSOCK: return WSAENOTSOCK;
    case EBADF: return WSAENOTSOCK;
    case EINVAL: return WSAEINVAL;
    case EACCES: return WSAEACCES;
    case EADDRINUSE: return WSAEADDRINUSE;
    case EADDRNOTAVAIL: return WSAEADDRNOTAVAIL;
    case ENETDOWN: return WSAENETDOWN;
    case ENETUNREACH: return WSAENETUNREACH;
    case ECONNABORTED: return WSAECONNABORTED;
    case ECONNRESET: return WSAECONNRESET;
    case EPIPE: return WSAECONNRESET;
    case ENOBUFS: return WSAENOBUFS;
    case EISCONN: return WSAEISCONN;
    case ENOTCONN: return WSAENOTCONN;
    case ETIMEDOUT: return WSAETIMEDOUT;
    case ECONNREFUSED: return WSAECONNREFUSED;
    case EHOSTUNREACH: return WSAEHOSTUNREACH;
    case EMFILE: return WSAEMFILE;
    case EAFNOSUPPORT: return WSAEAFNOSUPPORT;
    case EPROTONOSUPPORT: return WSAEPROTONOSUPPORT;
    case EOPNOTSUPP: return WSAEOPNOTSUPP;
    case EMSGSIZE: return WSAEMSGSIZE;
    case EFAULT: return WSAEFAULT;
    case EINTR: return WSAEINTR;
    default: return WSAEINVAL;
    }
}
static void fail_errno(X86State *st) { set_last_error(st, (uint32_t)wsa_err(errno)); }

static int fd_of(uint64_t s) {
    uint64_t v = s - SOCK_BASE;
    if (s < SOCK_BASE || v >= MAX_FD || !g_sock[v].used) return -1;
    return (int)v;
}
static uint64_t sock_of(int fd) { return SOCK_BASE + (uint64_t)fd; }

// Waits for ev on fd without the global lock. timeout_ms < 0: forever. 1 = ready, 0 = timeout,
// -1 = the program is ending.
static int wait_fd(int fd, short ev, int timeout_ms) {
    SoulThread *t = soul_cur_thread();
    SoulProcess *p = t && t->proc ? t->proc : g_proc;
    struct pollfd pf = { fd, ev, 0 };
    if (poll(&pf, 1, 0) > 0) return 1;
    if (timeout_ms == 0) return 0;
    double end = timeout_ms > 0 ? soul_now() + timeout_ms / 1000.0 : 0;
    int r = 0;
    gil_unlock(t);
    for (;;) {
        if (p->terminate) { r = -1; break; }
        int slice = 100;
        if (timeout_ms > 0) { double left = (end - soul_now()) * 1000.0; if (left <= 0) break; if (left < slice) slice = (int)left + 1; }
        pf.revents = 0;
        if (poll(&pf, 1, slice) > 0) { r = 1; break; }
    }
    gil_lock(t);
    return r;
}

// ---- sockaddr conversion ------------------------------------------------------------------------------
// Windows: { u16 family; ... }; AF_INET = 2, AF_INET6 = 23.

static int sa_from_guest(uint64_t g, int glen, struct sockaddr_storage *h, socklen_t *hlen) {
    memset(h, 0, sizeof *h);
    if (!g || glen < 2) return -1;
    uint16_t fam = (uint16_t)mem_rd(g, 2);
    if (fam == 2 && glen >= 8) {
        struct sockaddr_in *a = (struct sockaddr_in *)h;
        a->sin_family = AF_INET;
        memcpy(&a->sin_port, G2H(g + 2), 2);
        memcpy(&a->sin_addr, G2H(g + 4), 4);
#ifdef __APPLE__
        a->sin_len = sizeof *a;
#endif
        *hlen = sizeof *a;
        return 0;
    }
    if (fam == 23 && glen >= 24) {
        struct sockaddr_in6 *a = (struct sockaddr_in6 *)h;
        a->sin6_family = AF_INET6;
        memcpy(&a->sin6_port, G2H(g + 2), 2);
        memcpy(&a->sin6_flowinfo, G2H(g + 4), 4);
        memcpy(&a->sin6_addr, G2H(g + 8), 16);
        if (glen >= 28) memcpy(&a->sin6_scope_id, G2H(g + 24), 4);
#ifdef __APPLE__
        a->sin6_len = sizeof *a;
#endif
        *hlen = sizeof *a;
        return 0;
    }
    if (fam == 0 && glen >= 16) { // AF_UNSPEC: treat as IPv4 any
        struct sockaddr_in *a = (struct sockaddr_in *)h;
        a->sin_family = AF_UNSPEC;
        *hlen = sizeof *a;
        return 0;
    }
    return -1;
}
// Writes a host address in Windows form; *glen_p (guest int) is updated. Returns 0 or -1 (too small).
static int sa_to_guest(const struct sockaddr *h, uint64_t g, uint64_t glen_p) {
    if (!g) return 0;
    int cap = glen_p ? (int)(int32_t)mem_rd(glen_p, 4) : 16;
    uint8_t out[28];
    int n;
    memset(out, 0, sizeof out);
    if (h->sa_family == AF_INET) {
        const struct sockaddr_in *a = (const struct sockaddr_in *)(const void *)h;
        out[0] = 2;
        memcpy(out + 2, &a->sin_port, 2);
        memcpy(out + 4, &a->sin_addr, 4);
        n = 16;
    } else if (h->sa_family == AF_INET6) {
        const struct sockaddr_in6 *a = (const struct sockaddr_in6 *)(const void *)h;
        out[0] = 23;
        memcpy(out + 2, &a->sin6_port, 2);
        memcpy(out + 4, &a->sin6_flowinfo, 4);
        memcpy(out + 8, &a->sin6_addr, 16);
        memcpy(out + 24, &a->sin6_scope_id, 4);
        n = 28;
    } else n = 16;
    if (cap < n) { if (glen_p) mem_wr(glen_p, 4, (uint32_t)n); return -1; }
    memcpy(G2H(g), out, (size_t)n);
    if (glen_p) mem_wr(glen_p, 4, (uint32_t)n);
    return 0;
}
static int host_af(int af) { return af == 2 ? AF_INET : af == 23 ? AF_INET6 : af == 0 ? AF_UNSPEC : -1; }
static int guest_af(int af) { return af == AF_INET ? 2 : af == AF_INET6 ? 23 : 0; }

// ---- startup / errors -------------------------------------------------------------------------------------

HLE(ws_WSAStartup) { // (version, WSADATA*)
    uint64_t d = ARG(1);
    if (d) {
        size_t sz = g_mode32 ? 400 : 408;
        memset(G2H(d), 0, sz);
        mem_wr(d, 2, ARG(0) & 0xFFFF ? (ARG(0) & 0xFFFF) : 0x0202);
        mem_wr(d + 2, 2, 0x0202);
        // 32-bit: szDescription follows at +4; 64-bit: iMaxSockets/iMaxUdpDg/lpVendorInfo first
        const char *desc = "WinSock 2.0 (Soul)";
        uint64_t at = g_mode32 ? d + 4 : d + 16;
        memcpy(G2H(at), desc, strlen(desc) + 1);
        memcpy(G2H(at + 257), "Running", 8);
        if (g_mode32) { mem_wr(d + 4 + 257 + 129, 2, 32767); mem_wr(d + 4 + 257 + 129 + 2, 2, 65467); }
        else { mem_wr(d + 4, 2, 32767); mem_wr(d + 6, 2, 65467); }
    }
    RET(0);
}
HLE(ws_WSACleanup) { RET(0); }
HLE(ws_WSAGetLastError) { RET(get_last_error(st)); }
HLE(ws_WSASetLastError) { set_last_error(st, (uint32_t)ARG(0)); RET(0); }

// ---- sockets ------------------------------------------------------------------------------------------------

static uint64_t make_socket(X86State *st, int af, int type, int proto) {
    int haf = host_af(af);
    if (haf < 0 || haf == AF_UNSPEC) haf = AF_INET;
    int ht = type == 1 ? SOCK_STREAM : type == 2 ? SOCK_DGRAM : type == 3 ? SOCK_RAW : -1;
    if (ht < 0) { set_last_error(st, 10044); return INVALID_SOCK; } // WSAESOCKTNOSUPPORT
    int fd = socket(haf, ht, proto == 6 || proto == 17 ? proto : 0);
    if (fd < 0) { fail_errno(st); return INVALID_SOCK; }
    if (fd >= MAX_FD) { close(fd); set_last_error(st, WSAEMFILE); return INVALID_SOCK; }
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
#ifdef SO_NOSIGPIPE
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
#endif
    memset(&g_sock[fd], 0, sizeof g_sock[fd]);
    g_sock[fd].used = 1;
    set_last_error(st, 0);
    return sock_of(fd);
}
HLE(ws_socket) { RET(make_socket(st, ARGI(0), ARGI(1), ARGI(2))); }
HLE(ws_WSASocket) { RET(make_socket(st, ARGI(0), ARGI(1), ARGI(2))); } // (af, type, proto, info, group, flags)

static void async_forget(int fd);
HLE(ws_closesocket) {
    int fd = fd_of(ARG(0));
    if (fd < 0) { set_last_error(st, WSAENOTSOCK); RET(-1); return; }
    async_forget(fd);
    g_sock[fd].used = 0;
    close(fd);
    RET(0);
}
HLE(ws_shutdown) {
    int fd = fd_of(ARG(0));
    if (fd < 0) { set_last_error(st, WSAENOTSOCK); RET(-1); return; }
    int how = ARGI(1) == 0 ? SHUT_RD : ARGI(1) == 1 ? SHUT_WR : SHUT_RDWR;
    if (shutdown(fd, how) != 0) { fail_errno(st); RET(-1); return; }
    RET(0);
}
HLE(ws_bind) {
    int fd = fd_of(ARG(0));
    struct sockaddr_storage a;
    socklen_t l;
    if (fd < 0) { set_last_error(st, WSAENOTSOCK); RET(-1); return; }
    if (sa_from_guest(ARG(1), ARGI(2), &a, &l) != 0) { set_last_error(st, WSAEFAULT); RET(-1); return; }
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one); // Windows lets a port be reused after close
    if (bind(fd, (struct sockaddr *)&a, l) != 0) { fail_errno(st); RET(-1); return; }
    RET(0);
}
HLE(ws_listen) {
    int fd = fd_of(ARG(0));
    if (fd < 0) { set_last_error(st, WSAENOTSOCK); RET(-1); return; }
    if (listen(fd, ARGI(1) > 0 ? ARGI(1) : 16) != 0) { fail_errno(st); RET(-1); return; }
    RET(0);
}
static void async_rearm(int fd, uint32_t ev);
HLE(ws_accept) {
    int fd = fd_of(ARG(0));
    if (fd < 0) { set_last_error(st, WSAENOTSOCK); RET(INVALID_SOCK); return; }
    for (;;) {
        struct sockaddr_storage a;
        socklen_t l = sizeof a;
        int c = accept(fd, (struct sockaddr *)&a, &l);
        if (c >= 0) {
            async_rearm(fd, 8);
            if (c >= MAX_FD) { close(c); set_last_error(st, WSAEMFILE); RET(INVALID_SOCK); return; }
            fcntl(c, F_SETFL, fcntl(c, F_GETFL) | O_NONBLOCK);
            memset(&g_sock[c], 0, sizeof g_sock[c]);
            g_sock[c].used = 1;
            if (ARG(1)) sa_to_guest((struct sockaddr *)&a, ARG(1), ARG(2));
            RET(sock_of(c));
            return;
        }
        if ((errno != EWOULDBLOCK && errno != EAGAIN) || g_sock[fd].nonblock) { async_rearm(fd, 8); fail_errno(st); RET(INVALID_SOCK); return; }
        if (wait_fd(fd, POLLIN, -1) < 0) { set_last_error(st, WSAEINTR); RET(INVALID_SOCK); return; }
    }
}
HLE(ws_connect) {
    int fd = fd_of(ARG(0));
    struct sockaddr_storage a;
    socklen_t l;
    if (fd < 0) { set_last_error(st, WSAENOTSOCK); RET(-1); return; }
    if (sa_from_guest(ARG(1), ARGI(2), &a, &l) != 0) { set_last_error(st, WSAEFAULT); RET(-1); return; }
    if (connect(fd, (struct sockaddr *)&a, l) == 0) { RET(0); return; }
    if (errno != EINPROGRESS || g_sock[fd].nonblock) { fail_errno(st); RET(-1); return; }
    int w = wait_fd(fd, POLLOUT, g_sock[fd].snd_ms > 0 ? g_sock[fd].snd_ms : 75000);
    if (w <= 0) { set_last_error(st, w < 0 ? WSAEINTR : WSAETIMEDOUT); RET(-1); return; }
    int err = 0;
    socklen_t el = sizeof err;
    getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &el);
    if (err) { errno = err; fail_errno(st); RET(-1); return; }
    RET(0);
}
static int64_t do_recv(X86State *st, int fd, void *buf, size_t n, int flags, struct sockaddr_storage *from, socklen_t *fl) {
    int hf = (flags & 2 ? MSG_PEEK : 0) | (flags & 1 ? MSG_OOB : 0) | (flags & 8 ? MSG_WAITALL : 0);
    for (;;) {
        ssize_t r = from ? recvfrom(fd, buf, n, hf, (struct sockaddr *)from, fl) : recv(fd, buf, n, hf);
        if (r >= 0) { async_rearm(fd, 1 | 32); return r; }
        if ((errno != EWOULDBLOCK && errno != EAGAIN) || g_sock[fd].nonblock) { async_rearm(fd, 1); fail_errno(st); return -1; }
        int w = wait_fd(fd, POLLIN, g_sock[fd].rcv_ms > 0 ? g_sock[fd].rcv_ms : -1);
        if (w <= 0) { set_last_error(st, w < 0 ? WSAEINTR : WSAETIMEDOUT); return -1; }
    }
}
static int64_t do_send(X86State *st, int fd, const uint8_t *buf, size_t n, const struct sockaddr *to, socklen_t tl) {
    size_t done = 0;
    while (done < n || n == 0) {
        ssize_t r = to ? sendto(fd, buf + done, n - done, 0, to, tl) : send(fd, buf + done, n - done, 0
#ifdef MSG_NOSIGNAL
                                                                          | MSG_NOSIGNAL
#endif
        );
        if (r >= 0) { done += (size_t)r; if (to || n == 0) break; continue; }
        if (errno != EWOULDBLOCK && errno != EAGAIN) { if (done) break; fail_errno(st); return -1; }
        if (g_sock[fd].nonblock) { if (done) break; async_rearm(fd, 2); fail_errno(st); return -1; }
        int w = wait_fd(fd, POLLOUT, g_sock[fd].snd_ms > 0 ? g_sock[fd].snd_ms : -1);
        if (w <= 0) { if (done) break; set_last_error(st, w < 0 ? WSAEINTR : WSAETIMEDOUT); return -1; }
    }
    return (int64_t)done;
}
HLE(ws_recv) {
    int fd = fd_of(ARG(0));
    if (fd < 0) { set_last_error(st, WSAENOTSOCK); RET(-1); return; }
    int64_t r = do_recv(st, fd, PTR(1), (size_t)(uint32_t)ARG(2), ARGI(3), NULL, NULL);
    RET((uint64_t)r);
}
HLE(ws_recvfrom) {
    int fd = fd_of(ARG(0));
    if (fd < 0) { set_last_error(st, WSAENOTSOCK); RET(-1); return; }
    struct sockaddr_storage a;
    socklen_t l = sizeof a;
    int64_t r = do_recv(st, fd, PTR(1), (size_t)(uint32_t)ARG(2), ARGI(3), &a, &l);
    if (r >= 0 && hle_arg(st, 4)) sa_to_guest((struct sockaddr *)&a, hle_arg(st, 4), hle_arg(st, 5));
    RET((uint64_t)r);
}
HLE(ws_send) {
    int fd = fd_of(ARG(0));
    if (fd < 0) { set_last_error(st, WSAENOTSOCK); RET(-1); return; }
    RET((uint64_t)do_send(st, fd, PTR(1), (size_t)(uint32_t)ARG(2), NULL, 0));
}
HLE(ws_sendto) {
    int fd = fd_of(ARG(0));
    if (fd < 0) { set_last_error(st, WSAENOTSOCK); RET(-1); return; }
    struct sockaddr_storage a;
    socklen_t l = 0;
    int has = hle_arg(st, 4) && sa_from_guest(hle_arg(st, 4), (int)(int32_t)hle_arg(st, 5), &a, &l) == 0;
    RET((uint64_t)do_send(st, fd, PTR(1), (size_t)(uint32_t)ARG(2), has ? (struct sockaddr *)&a : NULL, l));
}
// WSABUF: { ULONG len; CHAR *buf }
static void wsa_bufs(uint64_t arr, uint32_t n, uint64_t *ptrs, uint32_t *lens) {
    const char *L = "4P";
    for (uint32_t i = 0; i < n; i++) { uint64_t e = arr + (uint64_t)i * gs_size(L); lens[i] = (uint32_t)gs_get(e, L, 0); ptrs[i] = gs_get(e, L, 1); }
}
HLE(ws_WSASend) { // (s, bufs, count, &sent, flags, overlapped, routine)
    int fd = fd_of(ARG(0));
    if (fd < 0) { set_last_error(st, WSAENOTSOCK); RET(-1); return; }
    uint32_t n = (uint32_t)ARG(2) > 64 ? 64 : (uint32_t)ARG(2), lens[64];
    uint64_t ptrs[64], total = 0;
    wsa_bufs(ARG(1), n, ptrs, lens);
    for (uint32_t i = 0; i < n; i++) {
        int64_t r = do_send(st, fd, G2H(ptrs[i]), lens[i], NULL, 0);
        if (r < 0) { if (!total) { RET(-1); return; } break; }
        total += (uint64_t)r;
        if ((uint32_t)r < lens[i]) break;
    }
    if (ARG(3)) mem_wr(ARG(3), 4, total);
    RET(0);
}
HLE(ws_WSARecv) { // (s, bufs, count, &received, &flags, overlapped, routine)
    int fd = fd_of(ARG(0));
    if (fd < 0) { set_last_error(st, WSAENOTSOCK); RET(-1); return; }
    uint32_t lens[1];
    uint64_t ptrs[1];
    wsa_bufs(ARG(1), 1, ptrs, lens);
    int flags = hle_arg(st, 4) ? (int)mem_rd(hle_arg(st, 4), 4) : 0;
    int64_t r = do_recv(st, fd, G2H(ptrs[0]), lens[0], flags, NULL, NULL);
    if (r < 0) { RET(-1); return; }
    if (ARG(3)) mem_wr(ARG(3), 4, (uint64_t)r);
    if (hle_arg(st, 4)) mem_wr(hle_arg(st, 4), 4, 0);
    RET(0);
}

// ---- options / names ---------------------------------------------------------------------------------------

HLE(ws_ioctlsocket) { // (s, cmd, u_long*)
    int fd = fd_of(ARG(0));
    if (fd < 0) { set_last_error(st, WSAENOTSOCK); RET(-1); return; }
    uint32_t cmd = (uint32_t)ARG(1);
    if (cmd == 0x8004667E) { g_sock[fd].nonblock = mem_rd(ARG(2), 4) != 0; RET(0); return; } // FIONBIO
    if (cmd == 0x4004667F) { int n = 0; ioctl(fd, FIONREAD, &n); mem_wr(ARG(2), 4, (uint32_t)n); RET(0); return; } // FIONREAD
    if (cmd == 0x40047307) { mem_wr(ARG(2), 4, 1); RET(0); return; } // SIOCATMARK
    set_last_error(st, WSAEINVAL);
    RET(-1);
}
static int map_opt(int level, int opt, int *hl, int *ho) {
    if (level == 0xFFFF) {
        *hl = SOL_SOCKET;
        switch (opt) {
        case 0x0004: *ho = SO_REUSEADDR; return 1;
        case 0x0008: *ho = SO_KEEPALIVE; return 1;
        case 0x0020: *ho = SO_BROADCAST; return 1;
        case 0x0080: *ho = SO_LINGER; return 2;
        case 0x0100: *ho = SO_OOBINLINE; return 1;
        case 0x1001: *ho = SO_SNDBUF; return 1;
        case 0x1002: *ho = SO_RCVBUF; return 1;
        case 0x1005: *ho = SO_SNDTIMEO; return 3;
        case 0x1006: *ho = SO_RCVTIMEO; return 3;
        case 0x1007: *ho = SO_ERROR; return 1;
        case 0x1008: *ho = SO_TYPE; return 4;
        case 0x0002: *ho = SO_ACCEPTCONN; return 1;
        case (int)0xFFFFFFFB: *ho = SO_REUSEADDR; return 5; // SO_EXCLUSIVEADDRUSE: accept, nothing to do
        default: return 0;
        }
    }
    if (level == 6) { *hl = IPPROTO_TCP; if (opt == 1) { *ho = TCP_NODELAY; return 1; } return 0; }
    if (level == 0) { *hl = IPPROTO_IP; if (opt == 4) { *ho = IP_TTL; return 1; } if (opt == 10) { *ho = IP_MULTICAST_TTL; return 1; } return 0; }
    if (level == 41) { *hl = IPPROTO_IPV6; if (opt == 27) { *ho = IPV6_V6ONLY; return 1; } return 0; }
    return 0;
}
HLE(ws_setsockopt) { // (s, level, opt, value, len)
    int fd = fd_of(ARG(0));
    if (fd < 0) { set_last_error(st, WSAENOTSOCK); RET(-1); return; }
    int hl, ho, kind = map_opt(ARGI(1), ARGI(2), &hl, &ho);
    uint64_t v = ARG(3);
    if (!kind || kind == 5) { RET(0); return; } // unknown options are accepted, like most stacks do
    if (kind == 3) { // timeouts: DWORD milliseconds on Windows, kept here (the host socket is non-blocking)
        int ms = v ? (int)mem_rd(v, 4) : 0;
        if (ho == SO_RCVTIMEO) g_sock[fd].rcv_ms = ms; else g_sock[fd].snd_ms = ms;
        RET(0);
        return;
    }
    if (kind == 2) { struct linger lg = { v ? (int)mem_rd(v, 2) : 0, v ? (int)mem_rd(v + 2, 2) : 0 }; setsockopt(fd, hl, ho, &lg, sizeof lg); RET(0); return; }
    int iv = v ? (int)mem_rd(v, (int)hle_arg(st, 4) >= 4 ? 4 : 1) : 0;
    if (setsockopt(fd, hl, ho, &iv, sizeof iv) != 0) { fail_errno(st); RET(-1); return; }
    RET(0);
}
HLE(ws_getsockopt) { // (s, level, opt, value, &len)
    int fd = fd_of(ARG(0));
    if (fd < 0) { set_last_error(st, WSAENOTSOCK); RET(-1); return; }
    int hl, ho, kind = map_opt(ARGI(1), ARGI(2), &hl, &ho);
    uint64_t v = ARG(3);
    if (!v) { set_last_error(st, WSAEFAULT); RET(-1); return; }
    if (!kind || kind == 5) { mem_wr(v, 4, 0); if (hle_arg(st, 4)) mem_wr(hle_arg(st, 4), 4, 4); RET(0); return; }
    if (kind == 3) { mem_wr(v, 4, (uint32_t)(ho == SO_RCVTIMEO ? g_sock[fd].rcv_ms : g_sock[fd].snd_ms)); RET(0); return; }
    if (kind == 2) { struct linger lg; socklen_t l = sizeof lg; getsockopt(fd, hl, ho, &lg, &l); mem_wr(v, 2, (uint32_t)lg.l_onoff); mem_wr(v + 2, 2, (uint32_t)lg.l_linger); RET(0); return; }
    int iv = 0;
    socklen_t l = sizeof iv;
    if (getsockopt(fd, hl, ho, &iv, &l) != 0) { fail_errno(st); RET(-1); return; }
    if (kind == 4) iv = iv == SOCK_STREAM ? 1 : iv == SOCK_DGRAM ? 2 : 3;
    if (ho == SO_ERROR && hl == SOL_SOCKET && iv) iv = wsa_err(iv);
    mem_wr(v, 4, (uint32_t)iv);
    if (hle_arg(st, 4)) mem_wr(hle_arg(st, 4), 4, 4);
    RET(0);
}
static void sock_name(X86State *st, int peer) {
    int fd = fd_of(ARG(0));
    if (fd < 0) { set_last_error(st, WSAENOTSOCK); RET(-1); return; }
    struct sockaddr_storage a;
    socklen_t l = sizeof a;
    if ((peer ? getpeername(fd, (struct sockaddr *)&a, &l) : getsockname(fd, (struct sockaddr *)&a, &l)) != 0) { fail_errno(st); RET(-1); return; }
    if (sa_to_guest((struct sockaddr *)&a, ARG(1), ARG(2)) != 0) { set_last_error(st, WSAEFAULT); RET(-1); return; }
    RET(0);
}
HLE(ws_getsockname) { sock_name(st, 0); }
HLE(ws_getpeername) { sock_name(st, 1); }

// ---- select ------------------------------------------------------------------------------------------------
// fd_set: { u_int fd_count; SOCKET fd_array[64] } (the array starts at 8 in 64-bit programs)

#define FDS_ARR (g_mode32 ? 4u : 8u)
HLE(ws_select) { // (nfds, read, write, except, timeout)
    uint64_t sets[3] = { ARG(1), ARG(2), ARG(3) };
    struct pollfd pf[192];
    int owner[192], n = 0;
    for (int k = 0; k < 3; k++) {
        if (!sets[k]) continue;
        uint32_t c = (uint32_t)mem_rd(sets[k], 4);
        for (uint32_t i = 0; i < c && i < 64 && n < 192; i++) {
            int fd = fd_of(rd_ptr(sets[k] + FDS_ARR + (uint64_t)i * PSZ));
            if (fd < 0) { set_last_error(st, WSAENOTSOCK); RET(-1); return; }
            pf[n].fd = fd;
            pf[n].events = k == 0 ? POLLIN : k == 1 ? POLLOUT : POLLPRI;
            pf[n].revents = 0;
            owner[n++] = k;
        }
    }
    uint64_t tv = hle_arg(st, 4);
    int timeout = tv ? (int)mem_rd(tv, 4) * 1000 + (int)mem_rd(tv + 4, 4) / 1000 : -1;
    SoulThread *t = soul_cur_thread();
    SoulProcess *p = t && t->proc ? t->proc : g_proc;
    int ready = poll(pf, (nfds_t)n, 0);
    if (ready == 0 && timeout != 0) {
        double end = timeout > 0 ? soul_now() + timeout / 1000.0 : 0;
        gil_unlock(t);
        for (;;) {
            int slice = 100;
            if (timeout > 0) { double left = (end - soul_now()) * 1000.0; if (left <= 0) break; if (left < slice) slice = (int)left + 1; }
            ready = poll(pf, (nfds_t)n, slice);
            if (ready != 0 || p->terminate) break;
        }
        gil_lock(t);
    }
    if (ready < 0) { fail_errno(st); RET(-1); return; }
    // keep only the ready sockets in each set
    uint32_t cnt[3] = { 0, 0, 0 };
    int total = 0;
    uint64_t keep[3][64];
    for (int i = 0; i < n; i++) {
        short rv = pf[i].revents;
        int k = owner[i];
        int ok = k == 0 ? (rv & (POLLIN | POLLHUP | POLLERR)) != 0 : k == 1 ? (rv & (POLLOUT | POLLERR)) != 0 : (rv & POLLPRI) != 0;
        if (k == 1 && (rv & POLLERR)) ok = 1;
        if (ok) { keep[k][cnt[k]++] = sock_of(pf[i].fd); total++; }
    }
    for (int k = 0; k < 3; k++) {
        if (!sets[k]) continue;
        mem_wr(sets[k], 4, cnt[k]);
        for (uint32_t i = 0; i < cnt[k]; i++) wr_ptr(sets[k] + FDS_ARR + (uint64_t)i * PSZ, keep[k][i]);
    }
    RET((uint64_t)total);
}
HLE(ws___WSAFDIsSet) {
    uint64_t set = ARG(1);
    uint32_t c = (uint32_t)mem_rd(set, 4);
    for (uint32_t i = 0; i < c && i < 64; i++) if (rd_ptr(set + FDS_ARR + (uint64_t)i * PSZ) == (ARG(0) & (g_mode32 ? 0xFFFFFFFFull : ~0ull))) { RET(1); return; }
    RET(0);
}

// ---- byte order / addresses --------------------------------------------------------------------------------

HLE(ws_htons) { RET(htons((uint16_t)ARG(0))); }
HLE(ws_htonl) { RET(htonl((uint32_t)ARG(0))); }
HLE(ws_inet_addr) {
    struct in_addr a;
    RET(ARG(0) && inet_aton(CSTR(0), &a) ? a.s_addr : 0xFFFFFFFFu);
}
static uint64_t g_ntoa_buf;
SOUL_PROCESS_VAR(g_ntoa_buf)
HLE(ws_inet_ntoa) {
    struct in_addr a;
    a.s_addr = (uint32_t)ARG(0);
    if (!g_ntoa_buf) g_ntoa_buf = H2G(g_calloc(1, 32));
    snprintf(G2H(g_ntoa_buf), 32, "%s", inet_ntoa(a));
    RET(g_ntoa_buf);
}
HLE(ws_inet_pton) { // (family, string, buffer)
    int af = host_af(ARGI(0));
    RET(af < 0 ? (uint64_t)-1 : (uint64_t)inet_pton(af, CSTR(1), PTR(2)));
}
HLE(ws_InetPtonW) { char *s = w2u_dup(WSTR(1)); int af = host_af(ARGI(0)); int r = af < 0 ? -1 : inet_pton(af, s, PTR(2)); free(s); RET((uint64_t)(int64_t)r); }
static void ntop(X86State *st, int wide) { // (family, addr, buffer, size)
    char b[64];
    int af = host_af(ARGI(0));
    if (af < 0 || !inet_ntop(af, PTR(1), b, sizeof b)) { set_last_error(st, WSAEAFNOSUPPORT); RET(0); return; }
    size_t cap = (size_t)ARG(3);
    if (strlen(b) + 1 > cap) { set_last_error(st, WSAEINVAL); RET(0); return; }
    if (wide) u2w(b, -1, PTR(2), cap); else memcpy(PTR(2), b, strlen(b) + 1);
    RET(ARG(2));
}
HLE(ws_inet_ntop) { ntop(st, 0); }
HLE(ws_InetNtopW) { ntop(st, 1); }
HLE(ws_gethostname) { // (buffer, length)
    char h[256];
    if (gethostname(h, sizeof h) != 0 || !h[0]) snprintf(h, sizeof h, "IPAD");
    char *dot = strchr(h, '.');
    if (dot) *dot = 0;
    if ((int)strlen(h) + 1 > ARGI(1)) { set_last_error(st, WSAEFAULT); RET(-1); return; }
    memcpy(PTR(0), h, strlen(h) + 1);
    RET(0);
}

// Name lookups wait without the global lock.
static int lookup(const char *node, const char *service, const struct addrinfo *hints, struct addrinfo **res) {
    SoulThread *t = soul_cur_thread();
    gil_unlock(t);
    int r = getaddrinfo(node, service, hints, res);
    gil_lock(t);
    return r;
}
static int gai_err(int r) {
    switch (r) {
    case EAI_AGAIN: return WSATRY_AGAIN;
    case EAI_NONAME: return WSAHOST_NOT_FOUND;
#ifdef EAI_NODATA
#if EAI_NODATA != EAI_NONAME
    case EAI_NODATA: return WSANO_DATA;
#endif
#endif
    case EAI_FAMILY: return WSAEAFNOSUPPORT;
    case EAI_MEMORY: return WSAENOBUFS;
    case EAI_SERVICE: return 10109; // WSATYPE_NOT_FOUND
    default: return WSAHOST_NOT_FOUND;
    }
}

// hostent: { name, aliases, short addrtype, short length, addr_list } — one per process, like
// Windows' per-thread buffer.
static uint64_t g_hostent;
SOUL_PROCESS_VAR(g_hostent)
static uint64_t make_hostent(const char *name, const uint8_t (*addrs)[4], int n) {
    const char *L = "PP22P";
    if (!g_hostent) g_hostent = H2G(g_calloc(1, 1024));
    uint64_t he = g_hostent;
    memset(G2H(he), 0, 1024);
    uint64_t nm = he + 64, al = he + 320, list = he + 336, data = he + 600;
    snprintf(G2H(nm), 250, "%s", name);
    if (n > 16) n = 16;
    for (int i = 0; i < n; i++) { memcpy(G2H(data + 4 * (uint64_t)i), addrs[i], 4); wr_ptr(list + (uint64_t)i * PSZ, data + 4 * (uint64_t)i); }
    gs_set(he, L, 0, nm); gs_set(he, L, 1, al); gs_set(he, L, 2, 2); gs_set(he, L, 3, 4); gs_set(he, L, 4, list);
    return he;
}
HLE(ws_gethostbyname) {
    if (!ARG(0)) { RET(0); return; }
    char name[256];
    snprintf(name, sizeof name, "%s", CSTR(0));
    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_INET;
    int r = lookup(name, NULL, &hints, &res);
    if (r != 0 || !res) { set_last_error(st, (uint32_t)gai_err(r)); RET(0); return; }
    uint8_t addrs[16][4];
    int n = 0;
    for (struct addrinfo *a = res; a && n < 16; a = a->ai_next) {
        struct sockaddr_in *in = (struct sockaddr_in *)(void *)a->ai_addr;
        int dup = 0;
        for (int i = 0; i < n; i++) if (!memcmp(addrs[i], &in->sin_addr, 4)) dup = 1;
        if (!dup) memcpy(addrs[n++], &in->sin_addr, 4);
    }
    freeaddrinfo(res);
    RET(make_hostent(name, (const uint8_t (*)[4])addrs, n));
}
HLE(ws_gethostbyaddr) { // (addr, len, type)
    uint8_t a[1][4];
    memcpy(a[0], PTR(0), 4);
    char name[64];
    snprintf(name, sizeof name, "%u.%u.%u.%u", a[0][0], a[0][1], a[0][2], a[0][3]);
    RET(make_hostent(name, (const uint8_t (*)[4])a, 1));
}
static uint64_t g_servent;
SOUL_PROCESS_VAR(g_servent)
HLE(ws_getservbyname) { // (name, proto): the common services
    static const struct { const char *n; int port; } k[] = { { "http", 80 }, { "https", 443 }, { "ftp", 21 }, { "smtp", 25 }, { "pop3", 110 },
                                                             { "imap", 143 }, { "telnet", 23 }, { "ssh", 22 }, { "domain", 53 }, { NULL, 0 } };
    const char *n = ARG(0) ? CSTR(0) : "";
    for (int i = 0; k[i].n; i++) if (!strcasecmp(k[i].n, n)) {
        const char *L = "PP2P"; // servent { name, aliases, short port, proto }
        if (!g_servent) g_servent = H2G(g_calloc(1, 64));
        uint64_t sv = g_servent;
        gs_set(sv, L, 0, sv + 40); gs_set(sv, L, 1, 0); gs_set(sv, L, 2, htons((uint16_t)k[i].port)); gs_set(sv, L, 3, 0);
        snprintf(G2H(sv + 40), 20, "%s", k[i].n);
        RET(sv);
        return;
    }
    set_last_error(st, WSANO_DATA);
    RET(0);
}

// addrinfo: { int flags, family, socktype, protocol; size_t addrlen; char *canonname; sockaddr *addr; addrinfo *next }
#define L_ADDRINFO "4444PPPP"
static void get_addr_info(X86State *st, int wide) { // (node, service, hints, &result)
    char *node = ARG(0) ? (wide ? w2u_dup(WSTR(0)) : strdup(CSTR(0))) : NULL;
    char *service = ARG(1) ? (wide ? w2u_dup(WSTR(1)) : strdup(CSTR(1))) : NULL;
    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof hints);
    if (ARG(2)) {
        uint64_t h = ARG(2);
        int fl = (int)gs_get(h, L_ADDRINFO, 0);
        hints.ai_flags = (fl & 1 ? AI_PASSIVE : 0) | (fl & 2 ? AI_CANONNAME : 0) | (fl & 4 ? AI_NUMERICHOST : 0) | (fl & 0x400 ? AI_ADDRCONFIG : 0);
        int af = host_af((int)gs_get(h, L_ADDRINFO, 1));
        hints.ai_family = af < 0 ? AF_UNSPEC : af;
        int t = (int)gs_get(h, L_ADDRINFO, 2);
        hints.ai_socktype = t == 1 ? SOCK_STREAM : t == 2 ? SOCK_DGRAM : 0;
        hints.ai_protocol = (int)gs_get(h, L_ADDRINFO, 3);
    }
    int r = lookup(node, service, &hints, &res);
    free(node); free(service);
    if (r != 0) { int e = gai_err(r); set_last_error(st, (uint32_t)e); RET((uint64_t)e); return; }
    uint64_t first = 0, prev = 0;
    size_t sz = gs_size(L_ADDRINFO);
    for (struct addrinfo *a = res; a; a = a->ai_next) {
        if (a->ai_family != AF_INET && a->ai_family != AF_INET6) continue;
        uint8_t *blk = g_calloc(1, sz + 32 + 260 * 2);
        uint64_t g = H2G(blk), sa = g + sz, cn = sa + 32;
        gs_set(g, L_ADDRINFO, 0, 0);
        gs_set(g, L_ADDRINFO, 1, (uint64_t)guest_af(a->ai_family));
        gs_set(g, L_ADDRINFO, 2, a->ai_socktype == SOCK_STREAM ? 1 : a->ai_socktype == SOCK_DGRAM ? 2 : 3);
        gs_set(g, L_ADDRINFO, 3, (uint64_t)a->ai_protocol);
        uint64_t lenp = H2G(g_calloc(1, 4));
        mem_wr(lenp, 4, 28);
        sa_to_guest(a->ai_addr, sa, lenp);
        gs_set(g, L_ADDRINFO, 4, mem_rd(lenp, 4));
        g_free(G2H(lenp));
        gs_set(g, L_ADDRINFO, 6, sa);
        if (a->ai_canonname) {
            if (wide) u2w(a->ai_canonname, -1, (uint16_t *)G2H(cn), 259); else snprintf(G2H(cn), 259, "%s", a->ai_canonname);
            gs_set(g, L_ADDRINFO, 5, cn);
        }
        if (prev) gs_set(prev, L_ADDRINFO, 7, g); else first = g;
        prev = g;
    }
    freeaddrinfo(res);
    if (!first) { set_last_error(st, WSAHOST_NOT_FOUND); RET(WSAHOST_NOT_FOUND); return; }
    wr_ptr(ARG(3), first);
    RET(0);
}
HLE(ws_getaddrinfo) { get_addr_info(st, 0); }
HLE(ws_GetAddrInfoW) { get_addr_info(st, 1); }
HLE(ws_freeaddrinfo) {
    uint64_t a = ARG(0);
    while (a) { uint64_t n = gs_get(a, L_ADDRINFO, 7); g_free(G2H(a)); a = n; }
    RET(0);
}
static void get_name_info(X86State *st, int wide) { // (sa, salen, host, hostlen, serv, servlen, flags)
    struct sockaddr_storage a;
    socklen_t l;
    if (sa_from_guest(ARG(0), ARGI(1), &a, &l) != 0) { RET(WSAEAFNOSUPPORT); return; }
    char host[1025] = "", serv[32] = "";
    int fl = (int)hle_arg(st, 6);
    int hf = (fl & 1 ? NI_NOFQDN : 0) | (fl & 2 ? NI_NUMERICHOST : 0) | (fl & 4 ? NI_NAMEREQD : 0) | (fl & 8 ? NI_NUMERICSERV : 0) | (fl & 16 ? NI_DGRAM : 0);
    SoulThread *t = soul_cur_thread();
    gil_unlock(t);
    int r = getnameinfo((struct sockaddr *)&a, l, host, sizeof host, serv, sizeof serv, hf);
    gil_lock(t);
    if (r != 0) { RET((uint64_t)gai_err(r)); return; }
    if (ARG(2) && ARG(3)) { if (wide) u2w(host, -1, PTR(2), (size_t)ARG(3)); else snprintf(PTR(2), (size_t)ARG(3), "%s", host); }
    if (hle_arg(st, 4) && hle_arg(st, 5)) { uint64_t sp = hle_arg(st, 4); if (wide) u2w(serv, -1, (uint16_t *)G2H(sp), (size_t)hle_arg(st, 5)); else snprintf(G2H(sp), (size_t)hle_arg(st, 5), "%s", serv); }
    RET(0);
}
HLE(ws_getnameinfo) { get_name_info(st, 0); }
HLE(ws_GetNameInfoW) { get_name_info(st, 1); }

// ---- WSAAsyncSelect / WSAEventSelect -------------------------------------------------------------------------
// A watcher thread polls registered sockets and posts the window message or signals the event.
// FD_READ 1, FD_WRITE 2, FD_OOB 4, FD_ACCEPT 8, FD_CONNECT 16, FD_CLOSE 32

typedef struct AsyncReg {
    int fd, used;
    uint32_t events, armed, pending, errors[10];
    uint64_t hwnd;
    uint32_t msg;
    HObj *event;          // WSAEventSelect: the event object (stable host memory of the owning process)
    int connected_seen;
} AsyncReg;
static AsyncReg g_async[256];
static pthread_mutex_t g_async_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_t g_watcher;
static int g_watcher_on;

static void async_forget(int fd) {
    pthread_mutex_lock(&g_async_mu);
    for (int i = 0; i < 256; i++) if (g_async[i].used && g_async[i].fd == fd) g_async[i].used = 0;
    pthread_mutex_unlock(&g_async_mu);
}
static void async_rearm(int fd, uint32_t ev) {
    pthread_mutex_lock(&g_async_mu);
    for (int i = 0; i < 256; i++) if (g_async[i].used && g_async[i].fd == fd) g_async[i].armed |= ev & g_async[i].events;
    pthread_mutex_unlock(&g_async_mu);
}
static void async_fire(AsyncReg *r, uint32_t ev, int err) {
    if (r->event) {
        r->pending |= ev;
        int bit = 0;
        while (bit < 10 && !((ev >> bit) & 1)) bit++;
        if (bit < 10) r->errors[bit] = (uint32_t)err;
        r->event->signaled = 1;
    }
    if (r->hwnd) win_post(r->hwnd, r->msg, sock_of(r->fd), ((uint64_t)(uint16_t)err << 16) | ev);
}
static void *watcher_main(void *arg) {
    (void)arg;
    for (;;) {
        struct pollfd pf[256];
        int idx[256], n = 0;
        pthread_mutex_lock(&g_async_mu);
        for (int i = 0; i < 256; i++) {
            AsyncReg *r = &g_async[i];
            if (!r->used || !r->armed) continue;
            short ev = 0;
            if (r->armed & (1 | 8 | 32)) ev |= POLLIN;
            if (r->armed & (2 | 16)) ev |= POLLOUT;
            if (r->armed & 4) ev |= POLLPRI;
            pf[n].fd = r->fd; pf[n].events = ev; pf[n].revents = 0; idx[n++] = i;
        }
        pthread_mutex_unlock(&g_async_mu);
        if (!n) { usleep(50000); if (!g_watcher_on) break; continue; }
        if (poll(pf, (nfds_t)n, 50) <= 0) continue;
        gil_lock(NULL);
        pthread_mutex_lock(&g_async_mu);
        for (int k = 0; k < n; k++) {
            AsyncReg *r = &g_async[idx[k]];
            short rv = pf[k].revents;
            if (!r->used || !rv) continue;
            int err = 0;
            if (rv & POLLERR) { int e = 0; socklen_t l = sizeof e; getsockopt(r->fd, SOL_SOCKET, SO_ERROR, &e, &l); err = e ? wsa_err(e) : WSAECONNRESET; }
            if ((rv & POLLOUT) && (r->armed & 16)) { r->armed &= ~16u; async_fire(r, 16, err); if (!err) { r->armed &= ~2u; async_fire(r, 2, 0); } continue; }
            if ((rv & POLLOUT) && (r->armed & 2)) { r->armed &= ~2u; async_fire(r, 2, err); }
            if (rv & (POLLIN | POLLHUP)) {
                int avail = 0;
                ioctl(r->fd, FIONREAD, &avail);
                int listening = 0;
                socklen_t l = sizeof listening;
                getsockopt(r->fd, SOL_SOCKET, SO_ACCEPTCONN, &listening, &l);
                if (listening) { if (r->armed & 8) { r->armed &= ~8u; async_fire(r, 8, err); } }
                else if (avail > 0) { if (r->armed & 1) { r->armed &= ~1u; async_fire(r, 1, err); } }
                else if (r->armed & 32) { r->armed &= ~(32u | 1u); async_fire(r, 32, err); }
            }
            if ((rv & POLLPRI) && (r->armed & 4)) { r->armed &= ~4u; async_fire(r, 4, err); }
        }
        pthread_mutex_unlock(&g_async_mu);
        gil_notify();
        gil_unlock(NULL);
    }
    return NULL;
}
static int async_register(int fd, uint32_t events, uint64_t hwnd, uint32_t msg, HObj *ev) {
    pthread_mutex_lock(&g_async_mu);
    AsyncReg *r = NULL;
    for (int i = 0; i < 256 && !r; i++) if (g_async[i].used && g_async[i].fd == fd) r = &g_async[i];
    for (int i = 0; i < 256 && !r; i++) if (!g_async[i].used) r = &g_async[i];
    if (!r) { pthread_mutex_unlock(&g_async_mu); return -1; }
    memset(r, 0, sizeof *r);
    if (events) {
        r->used = 1; r->fd = fd; r->events = events; r->armed = events; r->hwnd = hwnd; r->msg = msg; r->event = ev;
        g_sock[fd].nonblock = 1; // both calls make the socket non-blocking
    }
    pthread_mutex_unlock(&g_async_mu);
    if (!g_watcher_on) { g_watcher_on = 1; pthread_create(&g_watcher, NULL, watcher_main, NULL); pthread_detach(g_watcher); }
    return 0;
}
HLE(ws_WSAAsyncSelect) { // (s, hwnd, msg, events)
    int fd = fd_of(ARG(0));
    if (fd < 0) { set_last_error(st, WSAENOTSOCK); RET(-1); return; }
    RET(async_register(fd, (uint32_t)hle_arg(st, 3), ARG(1), (uint32_t)ARG(2), NULL) == 0 ? 0 : (uint64_t)-1);
}
HLE(ws_WSAEventSelect) { // (s, event, events)
    int fd = fd_of(ARG(0));
    if (fd < 0) { set_last_error(st, WSAENOTSOCK); RET(-1); return; }
    HObj *o = h_get(ARG(1));
    RET(async_register(fd, (uint32_t)ARG(2), 0, 0, o) == 0 ? 0 : (uint64_t)-1);
}
HLE(ws_WSAEnumNetworkEvents) { // (s, event, WSANETWORKEVENTS*): { long events; int errors[10] }
    int fd = fd_of(ARG(0));
    if (fd < 0) { set_last_error(st, WSAENOTSOCK); RET(-1); return; }
    uint32_t ev = 0, errs[10] = { 0 };
    pthread_mutex_lock(&g_async_mu);
    for (int i = 0; i < 256; i++) if (g_async[i].used && g_async[i].fd == fd) { ev = g_async[i].pending; memcpy(errs, g_async[i].errors, sizeof errs); g_async[i].pending = 0; }
    pthread_mutex_unlock(&g_async_mu);
    if (ARG(1)) { HObj *o = h_get(ARG(1)); if (o) o->signaled = 0; }
    if (ARG(2)) { mem_wr(ARG(2), 4, ev); for (int i = 0; i < 10; i++) mem_wr(ARG(2) + 4 + 4 * (uint64_t)i, 4, errs[i]); }
    RET(0);
}
HLE(ws_WSACreateEvent) { HObj *o; uint64_t h = h_alloc(H_EVENT, &o); if (o) { o->manual = 1; o->signaled = 0; } RET(h); }
HLE(ws_WSACloseEvent) { if (h_get(ARG(0))) h_free(ARG(0)); RET(1); }
HLE(ws_WSASetEvent) { HObj *o = h_get(ARG(0)); if (o) { o->signaled = 1; gil_notify(); } RET(o != NULL); }
HLE(ws_WSAResetEvent) { HObj *o = h_get(ARG(0)); if (o) o->signaled = 0; RET(o != NULL); }
HLE(ws_WSAWaitForMultipleEvents) { // (count, events, waitAll, timeout, alertable)
    uint32_t n = (uint32_t)ARG(0);
    uint64_t hs_[64];
    for (uint32_t i = 0; i < n && i < 64; i++) hs_[i] = rd_ptr(ARG(1) + (uint64_t)PSZ * i);
    uint32_t r = k32_wait_objects(st, n > 64 ? 64 : n, hs_, ARG(2) != 0, (uint32_t)ARG(3));
    RET(r);
}
HLE(ws_WSAIoctl) { set_last_error(st, WSAEOPNOTSUPP); RET(-1); }
HLE(ws_WSAGetOverlappedResult) { set_last_error(st, WSAEINVAL); RET(0); }
HLE(ws_WSAAddressToStringA) { // (addr, len, info, buffer, &length)
    struct sockaddr_storage a;
    socklen_t l;
    if (sa_from_guest(ARG(0), ARGI(1), &a, &l) != 0) { set_last_error(st, WSAEINVAL); RET(-1); return; }
    char b[80], ip[64];
    if (a.ss_family == AF_INET) { struct sockaddr_in *in = (struct sockaddr_in *)&a; inet_ntop(AF_INET, &in->sin_addr, ip, sizeof ip); if (in->sin_port) snprintf(b, sizeof b, "%s:%u", ip, ntohs(in->sin_port)); else snprintf(b, sizeof b, "%s", ip); }
    else { struct sockaddr_in6 *in = (struct sockaddr_in6 *)&a; inet_ntop(AF_INET6, &in->sin6_addr, ip, sizeof ip); if (in->sin6_port) snprintf(b, sizeof b, "[%s]:%u", ip, ntohs(in->sin6_port)); else snprintf(b, sizeof b, "%s", ip); }
    uint32_t cap = (uint32_t)mem_rd(hle_arg(st, 4), 4);
    mem_wr(hle_arg(st, 4), 4, (uint32_t)strlen(b) + 1);
    if (cap < strlen(b) + 1) { set_last_error(st, WSAEFAULT); RET(-1); return; }
    memcpy(PTR(3), b, strlen(b) + 1);
    RET(0);
}

const HleEntry hle_winsock_table[] = {
    { "WSAStartup", ws_WSAStartup, NULL }, { "WSACleanup", ws_WSACleanup, NULL }, { "WSAGetLastError", ws_WSAGetLastError, NULL },
    { "WSASetLastError", ws_WSASetLastError, NULL }, { "socket", ws_socket, NULL }, { "WSASocketA", ws_WSASocket, NULL },
    { "WSASocketW", ws_WSASocket, NULL }, { "closesocket", ws_closesocket, NULL }, { "shutdown", ws_shutdown, NULL },
    { "bind", ws_bind, NULL }, { "listen", ws_listen, NULL }, { "accept", ws_accept, NULL }, { "connect", ws_connect, NULL },
    { "WSAConnect", ws_connect, NULL }, { "recv", ws_recv, NULL }, { "recvfrom", ws_recvfrom, NULL }, { "send", ws_send, NULL },
    { "sendto", ws_sendto, NULL }, { "WSASend", ws_WSASend, NULL }, { "WSARecv", ws_WSARecv, NULL },
    { "ioctlsocket", ws_ioctlsocket, NULL }, { "setsockopt", ws_setsockopt, NULL }, { "getsockopt", ws_getsockopt, NULL },
    { "getsockname", ws_getsockname, NULL }, { "getpeername", ws_getpeername, NULL }, { "select", ws_select, NULL },
    { "__WSAFDIsSet", ws___WSAFDIsSet, NULL }, { "htons", ws_htons, NULL }, { "ntohs", ws_htons, NULL }, { "htonl", ws_htonl, NULL },
    { "ntohl", ws_htonl, NULL }, { "inet_addr", ws_inet_addr, NULL }, { "inet_ntoa", ws_inet_ntoa, NULL },
    { "inet_pton", ws_inet_pton, NULL }, { "InetPtonA", ws_inet_pton, NULL }, { "InetPtonW", ws_InetPtonW, NULL },
    { "inet_ntop", ws_inet_ntop, NULL }, { "InetNtopA", ws_inet_ntop, NULL }, { "InetNtopW", ws_InetNtopW, NULL },
    { "gethostname", ws_gethostname, NULL }, { "gethostbyname", ws_gethostbyname, NULL }, { "gethostbyaddr", ws_gethostbyaddr, NULL },
    { "getservbyname", ws_getservbyname, NULL }, { "getaddrinfo", ws_getaddrinfo, NULL }, { "GetAddrInfoA", ws_getaddrinfo, NULL },
    { "GetAddrInfoW", ws_GetAddrInfoW, NULL }, { "freeaddrinfo", ws_freeaddrinfo, NULL }, { "FreeAddrInfoW", ws_freeaddrinfo, NULL },
    { "getnameinfo", ws_getnameinfo, NULL }, { "GetNameInfoW", ws_GetNameInfoW, NULL },
    { "WSAAsyncSelect", ws_WSAAsyncSelect, NULL }, { "WSAEventSelect", ws_WSAEventSelect, NULL },
    { "WSAEnumNetworkEvents", ws_WSAEnumNetworkEvents, NULL }, { "WSACreateEvent", ws_WSACreateEvent, NULL },
    { "WSACloseEvent", ws_WSACloseEvent, NULL }, { "WSASetEvent", ws_WSASetEvent, NULL }, { "WSAResetEvent", ws_WSAResetEvent, NULL },
    { "WSAWaitForMultipleEvents", ws_WSAWaitForMultipleEvents, NULL }, { "WSAIoctl", ws_WSAIoctl, NULL },
    { "WSAGetOverlappedResult", ws_WSAGetOverlappedResult, NULL }, { "WSAAddressToStringA", ws_WSAAddressToStringA, NULL },
    { NULL, NULL, NULL }
};
