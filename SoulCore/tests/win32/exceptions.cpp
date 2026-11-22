// C++ exceptions (32-bit MinGW builds unwind with DWARF tables inside the program itself).
#include <cstdio>
#include <stdexcept>
#include <string>
int f(int x) { if (x > 2) throw std::runtime_error("çok büyük: " + std::to_string(x)); return x * 2; }
int main() {
    int ok = 0;
    for (int i = 0; i < 5; i++) {
        try { ok += f(i); } catch (const std::exception &e) { std::printf("yakalandı: %s\n", e.what()); }
    }
    std::printf("toplam %d\n", ok);
}
