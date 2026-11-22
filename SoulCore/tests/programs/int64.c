// 64-bit integer arithmetic: in 32-bit builds this becomes ADC/SBB chains, one-operand MUL,
// SHLD/SHRD and library division helpers, all of which the JIT must get exactly right.
#include <stdio.h>
#include <stdint.h>
int main(void) {
    uint64_t h = 1469598103934665603ULL, acc = 0;
    int64_t s = -5;
    for (int i = 0; i < 2000; i++) {
        h ^= (uint64_t)(i * 2654435761u);
        h *= 1099511628211ULL;
        acc += h >> (i % 61);
        acc -= (h << (i % 13)) ^ (uint64_t)s;
        s = s * 3 + (int64_t)(h % 1000) - 499;
        if (s > 1000000000000LL || s < -1000000000000LL) s /= 7;
    }
    printf("h=%016llx acc=%016llx s=%lld\n", (unsigned long long)h, (unsigned long long)acc, (long long)s);
    int lt = 0, gt = 0;
    for (int64_t a = -3000000000LL; a < 3000000000LL; a += 123456789) {
        int64_t b = a * -2 + 7;
        if (a < b) lt++;
        if ((uint64_t)a > (uint64_t)b) gt++;
    }
    uint64_t q = 0xFFFFFFFFFFFFFFFFULL / 12345ULL, r = 0xFFFFFFFFFFFFFFFFULL % 12345ULL;
    int64_t sq = -9000000000000000000LL / 777, sr = -9000000000000000000LL % 777;
    uint32_t x = 0xDEADBEEF, y = 0x12345678;
    uint64_t wide = (uint64_t)x * y;
    int64_t swide = (int64_t)(int32_t)x * (int32_t)y;
    printf("cmp %d %d div %llu %llu %lld %lld mul %016llx %lld\n", lt, gt, (unsigned long long)q, (unsigned long long)r,
           (long long)sq, (long long)sr, (unsigned long long)wide, (long long)swide);
    return 0;
}
