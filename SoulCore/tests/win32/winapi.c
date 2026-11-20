#include <windows.h>
#include <stdio.h>
#include <setjmp.h>
#include <wchar.h>
static jmp_buf jb;
static void deep(int n) { if (n == 0) longjmp(jb, 42); deep(n - 1); }
__thread int tls_counter = 5;
int main(int argc, char **argv) {
    char path[MAX_PATH];
    GetModuleFileNameA(NULL, path, MAX_PATH);
    printf("module=%s\n", path);
    char cwd[MAX_PATH];
    GetCurrentDirectoryA(MAX_PATH, cwd);
    printf("cwd=%s\n", cwd);
    HANDLE h = CreateFileA("w32test.dat", GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    DWORD written = 0;
    WriteFile(h, "Soul Win32\r\n", 12, &written, NULL);
    CloseHandle(h);
    h = CreateFileA("w32test.dat", GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    char buf[64] = { 0 };
    DWORD rd = 0;
    ReadFile(h, buf, sizeof buf - 1, &rd, NULL);
    printf("read %lu bytes: %s", (unsigned long)rd, buf);
    printf("size=%lu\n", (unsigned long)GetFileSize(h, NULL));
    CloseHandle(h);
    WIN32_FIND_DATAA fd;
    HANDLE f = FindFirstFileA("w32*.dat", &fd);
    printf("find=%s %s\n", f != INVALID_HANDLE_VALUE ? "ok" : "fail", fd.cFileName);
    FindClose(f);
    DeleteFileA("w32test.dat");
    printf("attr after delete=%lx err=%lu\n", (unsigned long)GetFileAttributesA("w32test.dat"), (unsigned long)GetLastError());
    char env[128];
    SetEnvironmentVariableA("SOUL_VAR", "merhaba");
    GetEnvironmentVariableA("SOUL_VAR", env, sizeof env);
    printf("env=%s windir=%s\n", env, getenv("windir"));
    wchar_t wbuf[64];
    int wn = MultiByteToWideChar(CP_UTF8, 0, "Çay şeker İstanbul", -1, wbuf, 64);
    char back[128];
    int bn = WideCharToMultiByte(CP_UTF8, 0, wbuf, -1, back, sizeof back, NULL, NULL);
    printf("utf: %d %d %s\n", wn, bn, back);
    HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
    const wchar_t *msg = L"WriteConsoleW: ğüşöç\n";
    DWORD cw;
    WriteConsoleW(out, msg, (DWORD)wcslen(msg), &cw, NULL);
    int r = setjmp(jb);
    if (r == 0) deep(50);
    printf("longjmp returned %d\n", r);
    tls_counter += 10;
    printf("tls=%d\n", tls_counter);
    DWORD t0 = GetTickCount();
    Sleep(20);
    printf("slept=%d\n", GetTickCount() - t0 >= 15);
    LARGE_INTEGER freq, c1;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&c1);
    printf("qpc=%d\n", freq.QuadPart > 0 && c1.QuadPart > 0);
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    printf("arch=%u page=%lu\n", si.wProcessorArchitecture, (unsigned long)si.dwPageSize);
    void *mem = VirtualAlloc(NULL, 1 << 20, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    memset(mem, 7, 1 << 20);
    printf("virtualalloc=%d\n", ((unsigned char *)mem)[12345]);
    VirtualFree(mem, 0, MEM_RELEASE);
    HANDLE heap = GetProcessHeap();
    int *arr = HeapAlloc(heap, HEAP_ZERO_MEMORY, 100 * sizeof(int));
    arr[99] = 9;
    arr = HeapReAlloc(heap, 0, arr, 200 * sizeof(int));
    printf("heap=%d %d\n", arr[0], arr[99]);
    HeapFree(heap, 0, arr);
    char numbuf[32];
    wsprintfA(numbuf, "%d-%s-%x", 12, "ab", 255);
    printf("wsprintf=%s\n", numbuf);
    printf("argc=%d argv1=%s\n", argc, argc > 1 ? argv[1] : "-");
    MessageBoxA(NULL, "Merhaba iPad!", "Soul", MB_OK);
    return 7;
}
