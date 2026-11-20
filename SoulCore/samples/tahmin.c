// Soul örnek programı: sayı tahmin oyunu
#include <stdio.h>
#include <stdlib.h>
#include <windows.h>
int main(void) {
    srand(GetTickCount());
    int secret = rand() % 100 + 1, tries = 0, guess = 0;
    printf("1 ile 100 arasında bir sayı tuttum. Tahmin et!\n");
    while (guess != secret) {
        printf("Tahminin: ");
        fflush(stdout);
        if (scanf("%d", &guess) != 1) { int c; while ((c = getchar()) != '\n' && c != EOF) {} if (c == EOF) return 0; printf("Lütfen bir sayı yaz.\n"); continue; }
        tries++;
        if (guess < secret) printf("Daha BÜYÜK bir sayı dene.\n");
        else if (guess > secret) printf("Daha KÜÇÜK bir sayı dene.\n");
    }
    printf("Tebrikler! %d denemede buldun.\n", tries);
    MessageBoxA(NULL, "Kazandın! 🎉", "Sayı Tahmin", MB_OK | MB_ICONINFORMATION);
    return 0;
}
