// Soul örnek programı: Soul'un taklit ettiği Windows ortamı hakkında bilgi
#include <windows.h>
#include <stdio.h>
#include <intrin.h>
int main(void) {
    int regs[4];
    char brand[49] = { 0 };
    __cpuid(regs, 0x80000000);
    if ((unsigned)regs[0] >= 0x80000004) {
        __cpuid((int *)(brand), 0x80000002);
        __cpuid((int *)(brand + 16), 0x80000003);
        __cpuid((int *)(brand + 32), 0x80000004);
    }
    printf("CPU (CPUID): %s\n", brand);
    OSVERSIONINFOA v = { sizeof v };
    GetVersionExA(&v);
    printf("Windows sürümü: %lu.%lu (derleme %lu)\n", v.dwMajorVersion, v.dwMinorVersion, v.dwBuildNumber);
    char buf[MAX_PATH];
    DWORD n = sizeof buf;
    GetComputerNameA(buf, &n);
    printf("Bilgisayar adı: %s\n", buf);
    GetWindowsDirectoryA(buf, MAX_PATH);
    printf("Windows klasörü: %s\n", buf);
    GetTempPathA(MAX_PATH, buf);
    printf("Geçici klasör: %s\n", buf);
    printf("Kullanıcı profili: %s\n", getenv("USERPROFILE"));
    MEMORYSTATUSEX ms = { sizeof ms };
    GlobalMemoryStatusEx(&ms);
    printf("Bellek: %llu MB\n", (unsigned long long)(ms.ullTotalPhys >> 20));
    SYSTEMTIME st;
    GetLocalTime(&st);
    printf("Yerel saat: %02d.%02d.%04d %02d:%02d:%02d\n", st.wDay, st.wMonth, st.wYear, st.wHour, st.wMinute, st.wSecond);
    printf("Komut satırı: %s\n", GetCommandLineA());
    return 0;
}
