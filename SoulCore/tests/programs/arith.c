#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>

static int64_t fib(int n) { return n < 2 ? n : fib(n - 1) + fib(n - 2); }
static uint64_t mix(uint64_t x) { x ^= x >> 33; x *= 0xff51afd7ed558ccdULL; x ^= x >> 33; x *= 0xc4ceb9fe1a85ec53ULL; x ^= x >> 33; return x; }
typedef int (*binop)(int, int);
static int add(int a, int b) { return a + b; }
static int sub(int a, int b) { return a - b; }
static int mul(int a, int b) { return a * b; }
static const char *name_of(int k) {
    switch (k) {
    case 0: return "zero"; case 1: return "one"; case 2: return "two"; case 3: return "three";
    case 4: return "four"; case 5: return "five"; case 6: return "six"; case 7: return "seven";
    default: return "many";
    }
}
int main(void) {
    printf("fib(25)=%lld\n", (long long)fib(25));
    uint64_t h = 0;
    for (uint64_t i = 0; i < 100000; i++) h += mix(i);
    printf("hash=%016llx\n", (unsigned long long)h);
    int64_t a = -1234567890123LL, b = 98765;
    printf("div=%lld mod=%lld udiv=%llu\n", (long long)(a / b), (long long)(a % b), (unsigned long long)((uint64_t)a / (uint64_t)b));
    int32_t c = -77777, d = 13;
    printf("i32 %d %d %u %d\n", c / d, c % d, (uint32_t)c / 7u, c >> 3);
    unsigned __int128 big = (unsigned __int128)0xFFFFFFFFFFFFFFFFULL * 0xFFFFFFFFULL;
    printf("u128 hi=%llx lo=%llx\n", (unsigned long long)(big >> 64), (unsigned long long)big);
    binop ops[3] = { add, sub, mul };
    int acc = 7;
    for (int i = 0; i < 30; i++) acc = ops[i % 3](acc, i + 1) & 0xFFFFF;
    printf("acc=%d\n", acc);
    for (int k = 0; k < 10; k++) printf("%s%s", name_of(k), k < 9 ? "," : "\n");
    uint32_t bits = 0x00F0F001;
    printf("pop=%d clz=%d ctz=%d rot=%08x bswap=%08x\n", __builtin_popcount(bits), __builtin_clz(bits), __builtin_ctz(bits),
           (bits << 7) | (bits >> 25), __builtin_bswap32(bits));
    int8_t s8 = -100; uint8_t u8 = 200; int16_t s16 = -30000;
    printf("ext %d %d %d %u\n", s8 * 3, u8 + s8, s16 / 7, (uint16_t)s16);
    int arr[64];
    for (int i = 0; i < 64; i++) arr[i] = (i * 37 + 11) % 64;
    int best = 0;
    for (int i = 0; i < 64; i++) for (int j = i + 1; j < 64; j++) if (arr[i] > arr[j]) best++;
    printf("inversions=%d\n", best);
    char buf[64];
    memset(buf, 'x', sizeof buf);
    memcpy(buf + 5, "soul", 4);
    buf[20] = 0;
    printf("buf=%s len=%zu\n", buf, strlen(buf));
    long long cmpres = 0;
    for (long long i = -50; i < 50; i++) { if (i < 3) cmpres += i; if ((unsigned long long)i > 10ULL) cmpres ^= i; if (i >= -7 && i <= 7) cmpres -= 2; }
    printf("cmp=%lld\n", cmpres);
    return 0;
}
