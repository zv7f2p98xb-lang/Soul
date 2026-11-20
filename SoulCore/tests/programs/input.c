#include <stdio.h>
int main(void) {
    int a, b;
    char name[64];
    printf("Adin? ");
    if (scanf("%63s", name) != 1) return 1;
    printf("Iki sayi: ");
    if (scanf("%d %d", &a, &b) != 2) return 2;
    printf("Merhaba %s! %d + %d = %d\n", name, a, b, a + b);
    int c, lines = 0;
    while ((c = getchar()) != EOF) if (c == '\n') lines++;
    printf("kalan satir=%d\n", lines);
    return 0;
}
