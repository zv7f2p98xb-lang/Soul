// Soul örnek programı: JIT performans testi
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
static double now_ms(void) { LARGE_INTEGER f, c; QueryPerformanceFrequency(&f); QueryPerformanceCounter(&c); return c.QuadPart * 1000.0 / f.QuadPart; }
int main(void) {
    printf("Soul JIT Performans Testi\n=========================\n");
    double t = now_ms();
    const int N = 5000000;
    char *s = calloc(N + 1, 1);
    int primes = 0;
    for (int i = 2; i <= N; i++) if (!s[i]) { primes++; for (long long j = (long long)i * i; j <= N; j += i) s[j] = 1; }
    free(s);
    printf("Eratosthenes eleği (5M): %d asal, %.1f ms\n", primes, now_ms() - t);
    t = now_ms();
    uint64_t h = 1469598103934665603ULL;
    for (int i = 0; i < 20000000; i++) { h ^= (uint64_t)i; h *= 1099511628211ULL; }
    printf("FNV-1a karma (20M): %016llx, %.1f ms\n", (unsigned long long)h, now_ms() - t);
    t = now_ms();
    int M = 200;
    double *A = malloc(sizeof(double) * M * M), *B = malloc(sizeof(double) * M * M), *C = calloc((size_t)M * M, sizeof(double));
    for (int i = 0; i < M * M; i++) { A[i] = (i % 17) * 0.25; B[i] = (i % 13) * 0.5; }
    for (int i = 0; i < M; i++) for (int k = 0; k < M; k++) { double a = A[i * M + k]; for (int j = 0; j < M; j++) C[i * M + j] += a * B[k * M + j]; }
    double tr = 0; for (int i = 0; i < M; i++) tr += C[i * M + i];
    printf("Matris çarpımı (200x200): iz=%.1f, %.1f ms\n", tr, now_ms() - t);
    t = now_ms();
    long long fib[90]; fib[0] = 0; fib[1] = 1;
    long long acc = 0;
    for (int r = 0; r < 200000; r++) { for (int i = 2; i < 90; i++) fib[i] = fib[i - 1] + fib[i - 2]; acc += fib[89] & 0xFF; }
    printf("Fibonacci döngüsü: %lld, %.1f ms\n", acc, now_ms() - t);
    printf("\nİpucu: Aynı testi Ayarlar'dan JIT kapalıyken çalıştırıp farkı görebilirsin.\n");
    return 0;
}
