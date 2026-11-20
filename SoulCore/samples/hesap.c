// Soul örnek programı: etkileşimli hesap makinesi (stdin okuma)
#include <stdio.h>
#include <math.h>
#include <string.h>
int main(void) {
    printf("Soul Hesap Makinesi\n");
    printf("Örnek: 12.5 * 4   |  2 ^ 10  |  sqrt 2  |  çıkmak için: q\n\n");
    char line[256];
    for (;;) {
        printf("> ");
        fflush(stdout);
        if (!fgets(line, sizeof line, stdin)) break;
        if (line[0] == 'q' || line[0] == 'Q') break;
        double a, b;
        char op[16];
        if (sscanf(line, "sqrt %lf", &a) == 1) { printf("= %.10g\n", sqrt(a)); continue; }
        if (sscanf(line, "%lf %15s %lf", &a, op, &b) == 3) {
            double r;
            switch (op[0]) {
            case '+': r = a + b; break;
            case '-': r = a - b; break;
            case '*': case 'x': r = a * b; break;
            case '/': if (b == 0) { printf("Hata: sıfıra bölme\n"); continue; } r = a / b; break;
            case '^': r = pow(a, b); break;
            case '%': r = fmod(a, b); break;
            default: printf("Bilinmeyen işlem: %s\n", op); continue;
            }
            printf("= %.10g\n", r);
        } else if (strlen(line) > 1) {
            printf("Anlaşılmadı. Biçim: sayı işlem sayı\n");
        }
    }
    printf("Güle güle!\n");
    return 0;
}
