// Soul örnek programı: renkli konsol çıktısı (Win32 SetConsoleTextAttribute)
#include <windows.h>
#include <stdio.h>
static void color(WORD c) { SetConsoleTextAttribute(GetStdHandle(STD_OUTPUT_HANDLE), c); }
int main(void) {
    SetConsoleOutputCP(CP_UTF8);
    color(FOREGROUND_GREEN | FOREGROUND_INTENSITY);
    printf("Merhaba! Bu program bir Windows x64 .exe dosyası.\n");
    color(FOREGROUND_BLUE | FOREGROUND_GREEN | FOREGROUND_INTENSITY);
    printf("Şu an iPad'in Apple Silicon (ARM64) işlemcisinde Soul ile çalışıyor.\n");
    color(FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_INTENSITY);
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    printf("Windows'a göre işlemci mimarisi: %s\n", si.wProcessorArchitecture == PROCESSOR_ARCHITECTURE_AMD64 ? "x64 (AMD64)" : "bilinmiyor");
    color(7);
    char path[MAX_PATH];
    GetModuleFileNameA(NULL, path, MAX_PATH);
    printf("Programın Windows yolu: %s\n", path);
    for (int i = 1; i <= 15; i++) { color((WORD)i); printf("█"); }
    color(7);
    printf("\n");
    return 0;
}
