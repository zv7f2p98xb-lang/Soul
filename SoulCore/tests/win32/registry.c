// Registry: values of every common type, enumeration, and persistence across runs.
#include <windows.h>
#include <stdio.h>
#include <string.h>

static void dump(HKEY k) {
    DWORD subkeys = 0, values = 0;
    RegQueryInfoKeyA(k, NULL, NULL, NULL, &subkeys, NULL, NULL, &values, NULL, NULL, NULL, NULL);
    printf("alt anahtar=%lu deger=%lu\n", subkeys, values);
    for (DWORD i = 0;; i++) {
        char name[64]; DWORD n = sizeof name;
        if (RegEnumKeyExA(k, i, name, &n, NULL, NULL, NULL, NULL) != ERROR_SUCCESS) break;
        printf("  anahtar %s\n", name);
    }
    for (DWORD i = 0;; i++) {
        char name[64]; DWORD n = sizeof name, type = 0, cb = 0;
        BYTE data[256];
        cb = sizeof data;
        if (RegEnumValueA(k, i, name, &n, NULL, &type, data, &cb) != ERROR_SUCCESS) break;
        if (type == REG_DWORD) printf("  %s = DWORD %lu\n", name, *(DWORD *)data);
        else if (type == REG_SZ) printf("  %s = \"%s\"\n", name, (char *)data);
        else printf("  %s = tür %lu, %lu bayt\n", name, type, cb);
    }
}

int main(int argc, char **argv) {
    int write = argc > 1 && !strcmp(argv[1], "yaz");
    HKEY k;
    if (write) {
        RegDeleteTreeA(HKEY_CURRENT_USER, "Software\\SoulTest");
        DWORD disp = 0;
        if (RegCreateKeyExA(HKEY_CURRENT_USER, "Software\\SoulTest\\Ayarlar", 0, NULL, 0, KEY_ALL_ACCESS, NULL, &k, &disp) != ERROR_SUCCESS) return 1;
        printf("oluşturuldu (yeni=%d)\n", disp == REG_CREATED_NEW_KEY);
        DWORD runs = 1;
        RegSetValueExA(k, "Calistirma", 0, REG_DWORD, (BYTE *)&runs, 4);
        RegSetValueExA(k, "Ad", 0, REG_SZ, (const BYTE *)"Soul kullanıcısı", (DWORD)strlen("Soul kullanıcısı") + 1);
        RegSetValueExW(k, L"Dil", 0, REG_SZ, (const BYTE *)L"Türkçe", (DWORD)(wcslen(L"Türkçe") + 1) * 2);
        BYTE bin[4] = { 1, 2, 3, 4 };
        RegSetValueExA(k, "Ikili", 0, REG_BINARY, bin, 4);
        RegCloseKey(k);
        HKEY sub;
        RegCreateKeyA(HKEY_CURRENT_USER, "Software\\SoulTest\\Ayarlar\\Pencere", &sub);
        DWORD w = 800;
        RegSetValueExA(sub, "Genislik", 0, REG_DWORD, (BYTE *)&w, 4);
        RegCloseKey(sub);
    }
    if (RegOpenKeyExA(HKEY_CURRENT_USER, "Software\\SoulTest\\Ayarlar", 0, KEY_READ, &k) != ERROR_SUCCESS) { printf("anahtar yok\n"); return 2; }
    DWORD runs = 0, cb = 4, type = 0;
    RegQueryValueExA(k, "Calistirma", NULL, &type, (BYTE *)&runs, &cb);
    printf("calistirma=%lu tur=%lu\n", runs, type);
    if (!write) { runs++; RegSetValueExA(k, "Calistirma", 0, REG_DWORD, (BYTE *)&runs, 4); }
    wchar_t lang[32]; cb = sizeof lang;
    RegQueryValueExW(k, L"Dil", NULL, NULL, (BYTE *)lang, &cb);
    printf("dil uzunlugu=%u bayt=%lu\n", (unsigned)wcslen(lang), cb);
    char small[4]; cb = sizeof small;
    LONG r = RegQueryValueExA(k, "Ad", NULL, NULL, (BYTE *)small, &cb);
    printf("kucuk tampon: %ld gereken=%lu\n", r, cb);
    DWORD width = 0; cb = 4;
    r = RegGetValueA(HKEY_CURRENT_USER, "Software\\SoulTest\\Ayarlar\\Pencere", "Genislik", RRF_RT_REG_DWORD, NULL, &width, &cb);
    printf("RegGetValue: %ld genislik=%lu\n", r, width);
    printf("olmayan: %ld\n", RegQueryValueExA(k, "Yok", NULL, NULL, NULL, NULL));
    dump(k);
    RegCloseKey(k);
    char product[64]; cb = sizeof product;
    if (RegGetValueA(HKEY_LOCAL_MACHINE, "SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion", "ProductName", RRF_RT_REG_SZ, NULL, product, &cb) == ERROR_SUCCESS)
        printf("urun: %s\n", product);
    return 0;
}
