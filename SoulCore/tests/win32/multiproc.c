// Several processes: CreateProcess (64/32-bit children), exit codes, inherited environment,
// redirected output through a pipe, WM_COPYDATA between processes, ShellExecuteEx, shell links.
#define COBJMACROS
#define INITGUID
#include <windows.h>
#include <shlobj.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static char g_dir[MAX_PATH];

static int child(int argc, char **argv) {
    int n = atoi(argv[2]);
    char cwd[MAX_PATH];
    GetCurrentDirectoryA(sizeof cwd, cwd);
    printf("child %d (%d-bit): env=%s\n", n, (int)sizeof(void *) * 8, getenv("SOUL_TEST") ? getenv("SOUL_TEST") : "(none)");
    fflush(stdout);
    if (argc > 3 && !strcmp(argv[3], "copydata")) {
        HWND h = FindWindowA("SoulParentWnd", NULL);
        COPYDATASTRUCT cds = { 7, 6, (void *)"hello" };
        LRESULT r = SendMessageA(h, WM_COPYDATA, 0, (LPARAM)&cds);
        char title[64] = "";
        SendMessageA(h, WM_GETTEXT, sizeof title, (LPARAM)title);
        printf("child %d: found=%d reply=%ld title=%s\n", n, h != NULL, (long)r, title);
        fflush(stdout);
    }
    return 40 + n;
}

static LRESULT CALLBACK wndproc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_COPYDATA) {
        COPYDATASTRUCT *c = (COPYDATASTRUCT *)l;
        printf("parent: WM_COPYDATA tag=%d size=%d data=%s\n", (int)c->dwData, (int)c->cbData, (const char *)c->lpData);
        fflush(stdout);
        return 99;
    }
    return DefWindowProcA(h, m, w, l);
}

static DWORD run(const char *exe, const char *args, HANDLE out) {
    char cmd[1024];
    snprintf(cmd, sizeof cmd, "\"%s\\%s\" %s", g_dir, exe, args);
    STARTUPINFOA si = { sizeof si };
    PROCESS_INFORMATION pi;
    if (out) { si.dwFlags = STARTF_USESTDHANDLES; si.hStdOutput = out; si.hStdError = out; si.hStdInput = GetStdHandle(STD_INPUT_HANDLE); }
    if (!CreateProcessA(NULL, cmd, NULL, NULL, out != NULL, 0, NULL, NULL, &si, &pi)) { printf("CreateProcess failed %lu\n", GetLastError()); return 0; }
    if (out) CloseHandle(out);
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 0;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return code;
}

int main(int argc, char **argv) {
    if (argc > 2 && !strcmp(argv[1], "child")) return child(argc, argv);
    GetModuleFileNameA(NULL, g_dir, sizeof g_dir);
    *strrchr(g_dir, '\\') = 0;
    const char *self = sizeof(void *) == 8 ? "multiproc.exe" : "multiproc32.exe";
    const char *other = sizeof(void *) == 8 ? "multiproc32.exe" : "multiproc.exe";
    setvbuf(stdout, NULL, _IONBF, 0);

    // 1. a child of the same kind, environment inherited
    SetEnvironmentVariableA("SOUL_TEST", "inherited");
    printf("exit=%lu\n", run(self, "child 1", NULL));
    // 2. the other architecture
    printf("exit=%lu\n", run(other, "child 2", NULL));

    // 3. output through a pipe
    SECURITY_ATTRIBUTES sa = { sizeof sa, NULL, TRUE };
    HANDLE rd, wr;
    CreatePipe(&rd, &wr, &sa, 0);
    HANDLE rd2;
    DuplicateHandle(GetCurrentProcess(), rd, GetCurrentProcess(), &rd2, 0, FALSE, DUPLICATE_SAME_ACCESS | DUPLICATE_CLOSE_SOURCE);
    char cmd[1024];
    snprintf(cmd, sizeof cmd, "\"%s\\%s\" child 3", g_dir, self);
    STARTUPINFOA si = { sizeof si };
    si.dwFlags = STARTF_USESTDHANDLES; si.hStdOutput = wr; si.hStdError = wr;
    PROCESS_INFORMATION pi;
    CreateProcessA(NULL, cmd, NULL, NULL, TRUE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi);
    CloseHandle(wr);
    char buf[512];
    DWORD got, total = 0;
    while (ReadFile(rd2, buf + total, sizeof buf - 1 - total, &got, NULL) && got) total += got;
    buf[total] = 0;
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code;
    GetExitCodeProcess(pi.hProcess, &code);
    printf("piped (%lu bytes): %s", total, buf);
    printf("exit=%lu\n", code);

    // 4. a window of this process gets WM_COPYDATA from the child
    WNDCLASSA wc = { 0 };
    wc.lpfnWndProc = wndproc;
    wc.hInstance = GetModuleHandleA(NULL);
    wc.lpszClassName = "SoulParentWnd";
    RegisterClassA(&wc);
    HWND hw = CreateWindowA("SoulParentWnd", "Ebeveyn", 0, 0, 0, 10, 10, NULL, NULL, wc.hInstance, NULL);
    snprintf(cmd, sizeof cmd, "\"%s\\%s\" child 4 copydata", g_dir, other);
    STARTUPINFOA si2 = { sizeof si2 };
    CreateProcessA(NULL, cmd, NULL, NULL, FALSE, 0, NULL, NULL, &si2, &pi);
    for (;;) {
        DWORD r = MsgWaitForMultipleObjects(1, &pi.hProcess, FALSE, 5000, QS_ALLINPUT);
        MSG msg;
        while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) DispatchMessageA(&msg);
        if (r == WAIT_OBJECT_0 || r == WAIT_TIMEOUT) break;
    }
    GetExitCodeProcess(pi.hProcess, &code);
    printf("exit=%lu\n", code);
    DestroyWindow(hw);

    // 5. ShellExecuteEx with a process handle
    SHELLEXECUTEINFOA sei = { sizeof sei };
    char file[MAX_PATH];
    snprintf(file, sizeof file, "%s\\%s", g_dir, self);
    sei.fMask = SEE_MASK_NOCLOSEPROCESS;
    sei.lpFile = file;
    sei.lpParameters = "child 5";
    sei.nShow = SW_SHOW;
    BOOL ok = ShellExecuteExA(&sei);
    WaitForSingleObject(sei.hProcess, INFINITE);
    GetExitCodeProcess(sei.hProcess, &code);
    printf("ShellExecuteEx=%d exit=%lu\n", ok, code);

    // 6. a shell link written and read back
    CoInitialize(NULL);
    IShellLinkW *link;
    HRESULT hr = CoCreateInstance(&CLSID_ShellLink, NULL, CLSCTX_INPROC_SERVER, &IID_IShellLinkW, (void **)&link);
    printf("CoCreateInstance=%08lx\n", (unsigned long)hr);
    if (SUCCEEDED(hr)) {
        IShellLinkW_SetPath(link, L"C:\\Program Files\\Soul Test\\uygulama.exe");
        IShellLinkW_SetArguments(link, L"--hızlı");
        IShellLinkW_SetDescription(link, L"Deneme kısayolu");
        IShellLinkW_SetWorkingDirectory(link, L"C:\\Program Files\\Soul Test");
        IPersistFile *pf;
        IShellLinkW_QueryInterface(link, &IID_IPersistFile, (void **)&pf);
        hr = IPersistFile_Save(pf, L"C:\\users\\soul\\Desktop\\Soul Test.lnk", TRUE);
        printf("Save=%08lx\n", (unsigned long)hr);
        IPersistFile_Release(pf);
        IShellLinkW_Release(link);
        IShellLinkW *l2;
        CoCreateInstance(&CLSID_ShellLink, NULL, CLSCTX_INPROC_SERVER, &IID_IShellLinkW, (void **)&l2);
        IShellLinkW_QueryInterface(l2, &IID_IPersistFile, (void **)&pf);
        hr = IPersistFile_Load(pf, L"C:\\users\\soul\\Desktop\\Soul Test.lnk", STGM_READ);
        WCHAR path[MAX_PATH], args[MAX_PATH], desc[MAX_PATH];
        IShellLinkW_GetPath(l2, path, MAX_PATH, NULL, 0);
        IShellLinkW_GetArguments(l2, args, MAX_PATH);
        IShellLinkW_GetDescription(l2, desc, MAX_PATH);
        char u[3][MAX_PATH * 3];
        WideCharToMultiByte(CP_UTF8, 0, path, -1, u[0], sizeof u[0], NULL, NULL);
        WideCharToMultiByte(CP_UTF8, 0, args, -1, u[1], sizeof u[1], NULL, NULL);
        WideCharToMultiByte(CP_UTF8, 0, desc, -1, u[2], sizeof u[2], NULL, NULL);
        printf("Load=%08lx path=%s args=%s desc=%s\n", (unsigned long)hr, u[0], u[1], u[2]);
        IPersistFile_Release(pf);
        IShellLinkW_Release(l2);
        DeleteFileA("C:\\users\\soul\\Desktop\\Soul Test.lnk");
    }
    CoUninitialize();
    printf("done\n");
    return 0;
}
