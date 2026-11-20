// Soul örnek programı: renkli ASCII Mandelbrot (kayan nokta yoğun)
#include <windows.h>
#include <stdio.h>
int main(void) {
    HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
    const char *shades = " .:-=+*#%@";
    const WORD colors[] = { 1, 9, 3, 11, 2, 10, 14, 6, 12, 4 };
    LARGE_INTEGER f, t0, t1;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&t0);
    long long total = 0;
    for (int y = 0; y < 32; y++) {
        for (int x = 0; x < 78; x++) {
            double cr = -2.2 + x * 3.0 / 78, ci = -1.2 + y * 2.4 / 32, zr = 0, zi = 0;
            int it = 0;
            while (it < 500 && zr * zr + zi * zi < 4.0) { double t = zr * zr - zi * zi + cr; zi = 2 * zr * zi + ci; zr = t; it++; }
            total += it;
            int idx = it >= 500 ? 9 : it % 9;
            SetConsoleTextAttribute(out, colors[idx]);
            putchar(it >= 500 ? '@' : shades[idx]);
        }
        SetConsoleTextAttribute(out, 7);
        putchar('\n');
    }
    QueryPerformanceCounter(&t1);
    printf("Toplam iterasyon: %lld, süre: %.1f ms\n", total, (t1.QuadPart - t0.QuadPart) * 1000.0 / f.QuadPart);
    return 0;
}
