// WinHTTP half of the HTTP test.
#include <windows.h>
#include <winhttp.h>
#include <stdio.h>

void winhttp(int g_port) {
    HINTERNET s = WinHttpOpen(L"SoulTest", WINHTTP_ACCESS_TYPE_NO_PROXY, NULL, NULL, 0);
    HINTERNET c = WinHttpConnect(s, L"127.0.0.1", (INTERNET_PORT)g_port, 0);
    HINTERNET r = WinHttpOpenRequest(c, L"GET", L"/hello", NULL, NULL, NULL, 0);
    BOOL ok = WinHttpSendRequest(r, NULL, 0, NULL, 0, 0, 0) && WinHttpReceiveResponse(r, NULL);
    DWORD status = 0, len = sizeof status;
    WinHttpQueryHeaders(r, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, NULL, &status, &len, NULL);
    WCHAR xs[32];
    len = sizeof xs;
    BOOL hc = WinHttpQueryHeaders(r, WINHTTP_QUERY_CUSTOM, L"X-Soul", xs, &len, NULL);
    DWORD avail = 0;
    WinHttpQueryDataAvailable(r, &avail);
    char body[128];
    DWORD got = 0;
    WinHttpReadData(r, body, sizeof body - 1, &got);
    body[got] = 0;
    printf("winhttp: ok=%d status=%lu x-soul=%ls avail=%lu body=%s\n", ok, status, hc ? xs : L"-", avail, body);
    WinHttpCloseHandle(r);
    WinHttpCloseHandle(c);
    WinHttpCloseHandle(s);
}

