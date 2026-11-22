// Winsock: TCP server/client across threads, UDP, select, non-blocking connect,
// WSAEventSelect, WSAAsyncSelect, name lookups and address conversions.
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <ctype.h>

static SOCKET g_listener;

static DWORD WINAPI server(LPVOID arg) {
    (void)arg;
    for (int k = 0; k < 2; k++) {
        SOCKET c = accept(g_listener, NULL, NULL);
        if (c == INVALID_SOCKET) { printf("accept failed %d\n", WSAGetLastError()); return 1; }
        char buf[128];
        int n = recv(c, buf, sizeof buf, 0);
        for (int i = 0; i < n; i++) buf[i] = (char)toupper((unsigned char)buf[i]);
        if (n > 0) send(c, buf, n, 0);
        closesocket(c);
    }
    return 0;
}

static LRESULT CALLBACK wndproc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_USER + 1) {
        printf("async: event=%d error=%d\n", (int)WSAGETSELECTEVENT(l), (int)WSAGETSELECTERROR(l));
        if (WSAGETSELECTEVENT(l) == FD_READ) {
            char b[64];
            int n = recv((SOCKET)w, b, sizeof b - 1, 0);
            b[n > 0 ? n : 0] = 0;
            printf("async read: %s\n", b);
            PostQuitMessage(0);
        }
        return 0;
    }
    return DefWindowProcA(h, m, w, l);
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    WSADATA wd;
    int ws = WSAStartup(MAKEWORD(2, 2), &wd);
    printf("WSAStartup=%d version=%x\n", ws, wd.wVersion);

    // listener on 127.0.0.1, port chosen by the system
    g_listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    struct sockaddr_in a = { 0 };
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = inet_addr("127.0.0.1");
    a.sin_port = 0;
    int b = bind(g_listener, (struct sockaddr *)&a, sizeof a);
    int l = listen(g_listener, 4);
    printf("bind=%d listen=%d\n", b, l);
    int len = sizeof a;
    getsockname(g_listener, (struct sockaddr *)&a, &len);
    printf("port assigned=%d family=%d len=%d\n", ntohs(a.sin_port) != 0, a.sin_family, len);
    HANDLE th = CreateThread(NULL, 0, server, NULL, 0, NULL);

    // blocking client
    SOCKET c = socket(AF_INET, SOCK_STREAM, 0);
    printf("connect=%d\n", connect(c, (struct sockaddr *)&a, sizeof a));
    send(c, "merhaba dünya", (int)strlen("merhaba dünya"), 0);
    char buf[128];
    int n = recv(c, buf, sizeof buf - 1, 0);
    buf[n > 0 ? n : 0] = 0;
    printf("echo: %s\n", buf);
    printf("recv after close=%d\n", recv(c, buf, sizeof buf, 0));
    closesocket(c);

    // non-blocking connect completed with select
    c = socket(AF_INET, SOCK_STREAM, 0);
    u_long on = 1;
    ioctlsocket(c, FIONBIO, &on);
    int r = connect(c, (struct sockaddr *)&a, sizeof a);
    printf("nb connect=%d err=%d\n", r, r ? WSAGetLastError() : 0);
    fd_set wr;
    FD_ZERO(&wr);
    FD_SET(c, &wr);
    struct timeval tv = { 5, 0 };
    printf("select writable=%d isset=%d\n", select(0, NULL, &wr, NULL, &tv), FD_ISSET(c, &wr));
    // nothing to read yet: would block
    r = recv(c, buf, sizeof buf, 0);
    printf("nb recv=%d err=%d\n", r, WSAGetLastError());
    send(c, "abc", 3, 0);
    fd_set rd;
    FD_ZERO(&rd);
    FD_SET(c, &rd);
    printf("select readable=%d\n", select(0, &rd, NULL, NULL, &tv));
    n = recv(c, buf, sizeof buf - 1, 0);
    buf[n > 0 ? n : 0] = 0;
    printf("nb echo: %s\n", buf);
    closesocket(c);
    WaitForSingleObject(th, 5000);

    // select timeout on an idle socket
    FD_ZERO(&rd);
    FD_SET(g_listener, &rd);
    struct timeval quick = { 0, 100000 };
    printf("select timeout=%d\n", select(0, &rd, NULL, NULL, &quick));

    // UDP to ourselves
    SOCKET u1 = socket(AF_INET, SOCK_DGRAM, 0), u2 = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in ua = { 0 };
    ua.sin_family = AF_INET;
    ua.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    bind(u1, (struct sockaddr *)&ua, sizeof ua);
    len = sizeof ua;
    getsockname(u1, (struct sockaddr *)&ua, &len);
    sendto(u2, "udp!", 4, 0, (struct sockaddr *)&ua, sizeof ua);
    struct sockaddr_in from;
    int fl = sizeof from;
    n = recvfrom(u1, buf, sizeof buf - 1, 0, (struct sockaddr *)&from, &fl);
    buf[n > 0 ? n : 0] = 0;
    printf("udp: %s from=%s\n", buf, inet_ntoa(from.sin_addr));
    closesocket(u1);
    closesocket(u2);

    // WSAEventSelect
    SOCKET p1 = socket(AF_INET, SOCK_STREAM, 0);
    th = CreateThread(NULL, 0, server, NULL, 0, NULL);
    connect(p1, (struct sockaddr *)&a, sizeof a);
    WSAEVENT ev = WSACreateEvent();
    WSAEventSelect(p1, ev, FD_READ | FD_CLOSE);
    send(p1, "olay", 4, 0);
    DWORD w = WSAWaitForMultipleEvents(1, &ev, FALSE, 5000, FALSE);
    WSANETWORKEVENTS ne;
    WSAEnumNetworkEvents(p1, ev, &ne);
    n = recv(p1, buf, sizeof buf - 1, 0);
    buf[n > 0 ? n : 0] = 0;
    printf("event wait=%lu read=%d data=%s\n", w, (ne.lNetworkEvents & FD_READ) != 0, buf);
    WSACloseEvent(ev);
    closesocket(p1);
    WaitForSingleObject(th, 5000);

    // WSAAsyncSelect: a datagram arrives as a window message
    WNDCLASSA wc = { 0 };
    wc.lpfnWndProc = wndproc;
    wc.hInstance = GetModuleHandleA(NULL);
    wc.lpszClassName = "NetTest";
    RegisterClassA(&wc);
    HWND hw = CreateWindowA("NetTest", "", 0, 0, 0, 0, 0, HWND_MESSAGE, NULL, wc.hInstance, NULL);
    u1 = socket(AF_INET, SOCK_DGRAM, 0);
    u2 = socket(AF_INET, SOCK_DGRAM, 0);
    bind(u1, (struct sockaddr *)&ua, sizeof ua);
    len = sizeof ua;
    getsockname(u1, (struct sockaddr *)&ua, &len);
    WSAAsyncSelect(u1, hw, WM_USER + 1, FD_READ);
    sendto(u2, "mesaj", 5, 0, (struct sockaddr *)&ua, sizeof ua);
    MSG msg;
    SetTimer(hw, 1, 5000, NULL);
    while (GetMessageA(&msg, NULL, 0, 0) > 0) { if (msg.message == WM_TIMER) { printf("async timeout\n"); break; } DispatchMessageA(&msg); }
    closesocket(u1);
    closesocket(u2);

    // names and addresses
    struct hostent *he = gethostbyname("localhost");
    printf("gethostbyname: %s len=%d\n", he ? inet_ntoa(*(struct in_addr *)he->h_addr_list[0]) : "(null)", he ? he->h_length : 0);
    struct addrinfo hints = { 0 }, *res = NULL;
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    int g = getaddrinfo("127.0.0.1", "8080", &hints, &res);
    if (g == 0 && res) {
        struct sockaddr_in *sa = (struct sockaddr_in *)res->ai_addr;
        char ip[64];
        inet_ntop(AF_INET, &sa->sin_addr, ip, sizeof ip);
        printf("getaddrinfo=%d %s:%d family=%d socktype=%d\n", g, ip, ntohs(sa->sin_port), res->ai_family, res->ai_socktype);
        freeaddrinfo(res);
    } else printf("getaddrinfo failed %d\n", g);
    struct in6_addr a6;
    char s6[64];
    printf("inet_pton6=%d ", inet_pton(AF_INET6, "2001:db8::1", &a6));
    inet_ntop(AF_INET6, &a6, s6, sizeof s6);
    printf("ntop6=%s\n", s6);
    he = gethostbyname("bu-alan-adi-yok.invalid");
    int e = WSAGetLastError();
    printf("bad host=%d err=%d\n", he == NULL, e == WSAHOST_NOT_FOUND || e == WSATRY_AGAIN || e == WSANO_DATA);
    closesocket(g_listener);
    WSACleanup();
    printf("done\n");
    return 0;
}
