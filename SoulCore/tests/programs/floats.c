#include <stdio.h>
#include <math.h>
#include <stdlib.h>
int main(void) {
    double x = 0.0;
    for (int i = 1; i <= 1000; i++) x += 1.0 / ((double)i * i);
    printf("basel=%.12f pi2/6=%.12f\n", x, M_PI * M_PI / 6);
    float f = 1.0f;
    for (int i = 0; i < 20; i++) f = f * 1.1f + 0.5f;
    printf("f=%.6f %g %e\n", f, f, (double)f);
    printf("sqrt=%.10f sin=%.10f cos=%.10f exp=%.10f log=%.10f pow=%.6f\n", sqrt(2.0), sin(1.0), cos(1.0), exp(1.5), log(10.0), pow(2.5, 3.3));
    printf("floor=%.1f ceil=%.1f round=%.1f fmod=%.3f atan2=%.6f\n", floor(-2.5), ceil(-2.5), round(2.5), fmod(10.3, 3.0), atan2(1.0, -1.0));
    double vals[] = { 3.75, -2.25, 1e10, -1e-3, 123456.789 };
    for (int i = 0; i < 5; i++) printf("%d:%lld:%d ", i, (long long)vals[i], (int)vals[i]);
    printf("\n");
    long long n = 9007199254740993LL;
    printf("conv %.1f %.1f %u\n", (double)n, (double)(float)n, (unsigned)(3.99e9));
    double a = 0.1, b = 0.2;
    printf("cmp %d %d %d %d\n", a + b == 0.3, a + b > 0.3, a < b, (a + b) != (b + a));
    double nan = sqrt(-1.0);
    printf("nan %d %d %d\n", nan == nan, nan < 1.0, isnan(nan) != 0);
    double m[3][3] = { { 2, 1, 0 }, { 1, 3, 1 }, { 0, 1, 4 } }, r[3][3] = { { 0 } };
    for (int i = 0; i < 3; i++) for (int j = 0; j < 3; j++) for (int k = 0; k < 3; k++) r[i][j] += m[i][k] * m[k][j];
    printf("mat %.0f %.0f %.0f %.0f\n", r[0][0], r[1][1], r[2][2], r[0][2]);
    printf("strtod %.4f atof %.4f\n", strtod("3.14159xyz", NULL), atof("-2.5e2"));
    float fa[8]; for (int i = 0; i < 8; i++) fa[i] = i * 0.5f;
    float s = 0; for (int i = 0; i < 8; i++) s += fa[i] * fa[i];
    printf("dot=%.3f\n", s);
    return 0;
}
