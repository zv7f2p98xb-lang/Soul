// HTTP clients: WinINet, WinHTTP and URLDownloadToFile against a small server on a thread.
#include <winsock2.h>
#include <windows.h>
#include <wininet.h>
#include <urlmon.h>
#include <stdio.h>
#include <string.h>

static SOCKET g_srv;
static int g_port;

static void reply(SOCKET c, const char *status, const char *extra, const char *body, int chunked) {
    char head[512];
    if (chunked) {
        int n = snprintf(head, sizeof head, "HTTP/1.1 %s\r\nTransfer-Encoding: chunked\r\n%s\r\n", status, extra);
        send(c, head, n, 0);
        size_t len = strlen(body), half = len / 2;
        char ch[64];
        n = snprintf(ch, sizeof ch, "%zx\r\n", half); send(c, ch, n, 0); send(c, body, (int)half, 0); send(c, "\r\n", 2, 0);
        n = snprintf(ch, sizeof ch, "%zx\r\n", len - half); send(c, ch, n, 0); send(c, body + half, (int)(len - half), 0); send(c, "\r\n", 2, 0);
        send(c, "0\r\n\r\n", 5, 0);
        return;
    }
    int n = snprintf(head, sizeof head, "HTTP/1.1 %s\r\nContent-Length: %zu\r\nContent-Type: text/plain\r\n%s\r\n", status, strlen(body), extra);
    send(c, head, n, 0);
    send(c, body, (int)strlen(body), 0);
}

static DWORD WINAPI server(LPVOID arg) {
    (void)arg;
    for (;;) {
        SOCKET c = accept(g_srv, NULL, NULL);
        if (c == INVALID_SOCKET) return 0;
        char req[4096];
        int n = 0, r;
        while ((r = recv(c, req + n, sizeof req - 1 - n, 0)) > 0) {
            n += r;
            req[n] = 0;
            char *end = strstr(req, "\r\n\r\n");
            if (!end) continue;
            char *cl = strstr(req, "Content-Length:");
            int want = cl ? atoi(cl + 15) : 0;
            if (n - (int)(end + 4 - req) >= want) break;
        }
        req[n] = 0;
        char path[256] = "";
        sscanf(req, "%*s %255s", path);
        if (!strcmp(path, "/hello")) reply(c, "200 OK", "X-Soul: 42\r\n", "Merhaba HTTP", 0);
        else if (!strcmp(path, "/chunked")) reply(c, "200 OK", "", "parca parca gelen govde", 1);
        else if (!strcmp(path, "/redirect")) reply(c, "302 Found", "Location: /hello\r\n", "", 0);
        else if (!strcmp(path, "/echo")) { char *b = strstr(req, "\r\n\r\n"); reply(c, "200 OK", "", b ? b + 4 : "", 0); }
        else if (!strcmp(path, "/quit")) { reply(c, "200 OK", "", "bye", 0); closesocket(c); return 0; }
        else reply(c, "404 Not Found", "", "yok", 0);
        closesocket(c);
    }
}

static void wininet(void) {
    HINTERNET s = InternetOpenA("SoulTest", INTERNET_OPEN_TYPE_DIRECT, NULL, NULL, 0);
    HINTERNET c = InternetConnectA(s, "127.0.0.1", (INTERNET_PORT)g_port, NULL, NULL, INTERNET_SERVICE_HTTP, 0, 0);
    HINTERNET r = HttpOpenRequestA(c, "GET", "/hello", NULL, NULL, NULL, INTERNET_FLAG_RELOAD, 0);
    BOOL ok = HttpSendRequestA(r, NULL, 0, NULL, 0);
    DWORD status = 0, len = sizeof status;
    HttpQueryInfoA(r, HTTP_QUERY_STATUS_CODE | HTTP_QUERY_FLAG_NUMBER, &status, &len, NULL);
    char clen[32] = "";
    len = sizeof clen;
    HttpQueryInfoA(r, HTTP_QUERY_CONTENT_LENGTH, clen, &len, NULL);
    char custom[64] = "X-Soul";
    len = sizeof custom;
    BOOL hc = HttpQueryInfoA(r, HTTP_QUERY_CUSTOM, custom, &len, NULL);
    char body[128];
    DWORD got = 0, total = 0;
    while (InternetReadFile(r, body + total, sizeof body - 1 - total, &got) && got) total += got;
    body[total] = 0;
    printf("wininet: send=%d status=%lu length=%s x-soul=%s body=%s\n", ok, status, clen, hc ? custom : "-", body);
    InternetCloseHandle(r);

    // POST
    r = HttpOpenRequestA(c, "POST", "/echo", NULL, NULL, NULL, 0, 0);
    const char *hdr = "Content-Type: application/x-www-form-urlencoded\r\n";
    ok = HttpSendRequestA(r, hdr, (DWORD)strlen(hdr), (void *)"ad=soul&deger=1", 15);
    total = 0;
    while (InternetReadFile(r, body + total, sizeof body - 1 - total, &got) && got) total += got;
    body[total] = 0;
    printf("wininet post: %d %s\n", ok, body);
    InternetCloseHandle(r);
    InternetCloseHandle(c);

    // InternetOpenUrl follows the redirect
    char url[128];
    snprintf(url, sizeof url, "http://127.0.0.1:%d/redirect", g_port);
    r = InternetOpenUrlA(s, url, NULL, 0, INTERNET_FLAG_RELOAD, 0);
    total = 0;
    while (r && InternetReadFile(r, body + total, sizeof body - 1 - total, &got) && got) total += got;
    body[total] = 0;
    status = 0; len = sizeof status;
    if (r) HttpQueryInfoA(r, HTTP_QUERY_STATUS_CODE | HTTP_QUERY_FLAG_NUMBER, &status, &len, NULL);
    printf("openurl redirect: status=%lu body=%s\n", status, body);
    if (r) InternetCloseHandle(r);
    snprintf(url, sizeof url, "http://127.0.0.1:%d/chunked", g_port);
    r = InternetOpenUrlA(s, url, NULL, 0, 0, 0);
    total = 0;
    while (r && InternetReadFile(r, body + total, sizeof body - 1 - total, &got) && got) total += got;
    body[total] = 0;
    printf("chunked: %s\n", body);
    if (r) InternetCloseHandle(r);
    snprintf(url, sizeof url, "http://127.0.0.1:%d/none", g_port);
    r = InternetOpenUrlA(s, url, NULL, 0, 0, 0);
    status = 0; len = sizeof status;
    if (r) HttpQueryInfoA(r, HTTP_QUERY_STATUS_CODE | HTTP_QUERY_FLAG_NUMBER, &status, &len, NULL);
    printf("missing page: status=%lu\n", status);
    if (r) InternetCloseHandle(r);
    InternetCloseHandle(s);

    URL_COMPONENTSA uc = { sizeof uc };
    char host[64], path[128];
    uc.lpszHostName = host; uc.dwHostNameLength = sizeof host;
    uc.lpszUrlPath = path; uc.dwUrlPathLength = sizeof path;
    InternetCrackUrlA("https://ornek.com:8443/a/b.txt?x=1", 0, 0, &uc);
    printf("crack: host=%s port=%d path=%s scheme=%d\n", host, uc.nPort, path, uc.nScheme);
}

void winhttp(int port); // http_winhttp.c (winhttp.h and wininet.h cannot be included together)

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    WSADATA wd;
    WSAStartup(MAKEWORD(2, 2), &wd);
    g_srv = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a = { 0 };
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = inet_addr("127.0.0.1");
    bind(g_srv, (struct sockaddr *)&a, sizeof a);
    listen(g_srv, 8);
    int l = sizeof a;
    getsockname(g_srv, (struct sockaddr *)&a, &l);
    g_port = ntohs(a.sin_port);
    HANDLE th = CreateThread(NULL, 0, server, NULL, 0, NULL);

    wininet();
    winhttp(g_port);
    char url[128];
    snprintf(url, sizeof url, "http://127.0.0.1:%d/hello", g_port);
    HRESULT hr = URLDownloadToFileA(NULL, url, "C:\\users\\soul\\Temp\\indirilen.txt", 0, NULL);
    FILE *f = fopen("C:\\users\\soul\\Temp\\indirilen.txt", "rb");
    char buf[64] = "";
    if (f) { size_t n = fread(buf, 1, sizeof buf - 1, f); buf[n] = 0; fclose(f); }
    printf("URLDownloadToFile=%08lx file=%s\n", (unsigned long)hr, buf);
    DeleteFileA("C:\\users\\soul\\Temp\\indirilen.txt");
    DWORD flags = 0;
    printf("connected=%d\n", InternetGetConnectedState(&flags, 0));

    snprintf(url, sizeof url, "http://127.0.0.1:%d/quit", g_port);
    HINTERNET s = InternetOpenA("q", 0, NULL, NULL, 0);
    HINTERNET r = InternetOpenUrlA(s, url, NULL, 0, 0, 0);
    InternetCloseHandle(r);
    InternetCloseHandle(s);
    WaitForSingleObject(th, 5000);
    closesocket(g_srv);
    printf("done\n");
    return 0;
}
