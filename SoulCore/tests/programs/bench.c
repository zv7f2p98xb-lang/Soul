#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
static uint32_t lcg(uint32_t *s) { *s = *s * 1664525u + 1013904223u; return *s; }
int main(void) {
    const int N = 2000000;
    char *sieve = calloc(N + 1, 1);
    int primes = 0;
    for (int i = 2; i <= N; i++) {
        if (!sieve[i]) { primes++; for (long long j = (long long)i * i; j <= N; j += i) sieve[j] = 1; }
    }
    printf("primes<=%d: %d\n", N, primes);
    free(sieve);
    int M = 120;
    double *A = malloc(sizeof(double) * M * M), *B = malloc(sizeof(double) * M * M), *C = calloc(M * M, sizeof(double));
    uint32_t s = 1;
    for (int i = 0; i < M * M; i++) { A[i] = (lcg(&s) % 1000) / 100.0; B[i] = (lcg(&s) % 1000) / 100.0; }
    for (int i = 0; i < M; i++) for (int k = 0; k < M; k++) { double a = A[i * M + k]; for (int j = 0; j < M; j++) C[i * M + j] += a * B[k * M + j]; }
    double tr = 0; for (int i = 0; i < M; i++) tr += C[i * M + i];
    printf("trace=%.2f\n", tr);
    uint64_t h = 1469598103934665603ULL;
    for (int r = 0; r < 300; r++) for (int i = 0; i < 10000; i++) { h ^= (uint64_t)(i * r); h *= 1099511628211ULL; }
    printf("fnv=%016llx\n", (unsigned long long)h);
    int arr[50000];
    for (int i = 0; i < 50000; i++) arr[i] = (int)(lcg(&s) % 100000);
    for (int gap = 25000; gap > 0; gap /= 2) for (int i = gap; i < 50000; i++) { int t = arr[i], j = i; while (j >= gap && arr[j - gap] > t) { arr[j] = arr[j - gap]; j -= gap; } arr[j] = t; }
    int ok = 1; for (int i = 1; i < 50000; i++) if (arr[i - 1] > arr[i]) ok = 0;
    printf("sorted=%d min=%d max=%d\n", ok, arr[0], arr[49999]);
    return 0;
}
