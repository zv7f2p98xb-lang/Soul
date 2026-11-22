// x87 forms whose AT&T spelling is easy to get backwards; expected output comes from real x87 hardware.
#include <stdio.h>
// Exercises x87 register forms directly (32-bit builds): results must match the interpreter.
static double op_dc(int which, double a, double b) {
    double r = a;
    switch (which) { // ST(1) = ST(1) op ST(0) with pop (DE forms) and DC forms
    case 0: __asm__("fldl %2\n\tfldl %1\n\tfsubrp %%st, %%st(1)\n\tfstpl %0" : "=m"(r) : "m"(a), "m"(b)); break;
    case 1: __asm__("fldl %2\n\tfldl %1\n\tfsubp %%st, %%st(1)\n\tfstpl %0" : "=m"(r) : "m"(a), "m"(b)); break;
    case 2: __asm__("fldl %2\n\tfldl %1\n\tfdivrp %%st, %%st(1)\n\tfstpl %0" : "=m"(r) : "m"(a), "m"(b)); break;
    case 3: __asm__("fldl %2\n\tfldl %1\n\tfdivp %%st, %%st(1)\n\tfstpl %0" : "=m"(r) : "m"(a), "m"(b)); break;
    case 4: __asm__("fldl %2\n\tfldl %1\n\tfsub %%st, %%st(1)\n\tfstp %%st(0)\n\tfstpl %0" : "=m"(r) : "m"(a), "m"(b)); break;
    case 5: __asm__("fldl %2\n\tfldl %1\n\tfdiv %%st, %%st(1)\n\tfstp %%st(0)\n\tfstpl %0" : "=m"(r) : "m"(a), "m"(b)); break;
    case 6: __asm__("fldl %2\n\tfldl %1\n\tfsub %%st(1), %%st\n\tfstpl %0\n\tfstp %%st(0)" : "=m"(r) : "m"(a), "m"(b)); break;
    case 7: __asm__("fldl %2\n\tfldl %1\n\tfdivr %%st(1), %%st\n\tfstpl %0\n\tfstp %%st(0)" : "=m"(r) : "m"(a), "m"(b)); break;
    case 8: __asm__("fldl %1\n\tfldl %2\n\tfxch %%st(1)\n\tfsubl %2\n\tfstpl %0\n\tfstp %%st(0)" : "=m"(r) : "m"(a), "m"(b)); break;
    case 9: __asm__("fldl %1\n\tfchs\n\tfabs\n\tfsqrt\n\tfld1\n\tfaddp\n\tfldz\n\tfaddp\n\tfstpl %0" : "=m"(r) : "m"(a)); break;
    }
    return r;
}
int main(void) {
    double a = 7.5, b = 2.0;
    for (int k = 0; k < 10; k++) printf("op%d=%.6f\n", k, op_dc(k, a, b));
    int lt = 0, eq = 0, gt = 0;
    for (int i = 0; i < 300; i++) { double x = i * 0.5, y = 75.0; if (x < y) lt++; else if (x == y) eq++; else gt++; }
    short s16 = -1234; long long i64 = 123456789012345LL; int i32 = -42;
    double f16 = s16, f64 = (double)i64, f32 = i32;
    float fl = 1.0f / 3.0f;
    printf("cmp %d %d %d fild %.1f %.1f %.1f fl %.7f\n", lt, eq, gt, f16, f64, f32, fl);
    return 0;
}
