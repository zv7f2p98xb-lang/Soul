// Program started by a type-34 custom action of ca.msi: writes ca_exe.txt (arguments) into its working directory.
#include <windows.h>
int main(int argc, char **argv) {
    char buf[300] = "exe:";
    if (argc > 1) lstrcatA(buf, argv[1]);
    HANDLE f = CreateFileA("ca_exe.txt", GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE) return 3;
    DWORD w;
    WriteFile(f, buf, lstrlenA(buf), &w, NULL);
    CloseHandle(f);
    return 0;
}
