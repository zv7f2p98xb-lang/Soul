#include <stdio.h>
#include <string.h>
#include <stdlib.h>
int main(void) {
    FILE *f = fopen("soul_test.txt", "w");
    if (!f) { printf("open failed\n"); return 1; }
    for (int i = 0; i < 5; i++) fprintf(f, "line %d: %s\n", i, i % 2 ? "odd" : "even");
    fclose(f);
    f = fopen("soul_test.txt", "r");
    char line[128];
    int n = 0;
    while (fgets(line, sizeof line, f)) { n++; line[strcspn(line, "\n")] = 0; printf("[%s]\n", line); }
    fclose(f);
    f = fopen("soul_test.bin", "wb");
    int data[100];
    for (int i = 0; i < 100; i++) data[i] = i * i;
    fwrite(data, sizeof(int), 100, f);
    fclose(f);
    f = fopen("soul_test.bin", "rb");
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 40 * sizeof(int), SEEK_SET);
    int v; fread(&v, sizeof v, 1, f);
    fclose(f);
    printf("lines=%d size=%ld v=%d\n", n, size, v);
    f = fopen("soul_test.txt", "a"); fputs("appended\n", f); fclose(f);
    f = fopen("soul_test.txt", "r"); int lines = 0, c; while ((c = fgetc(f)) != EOF) if (c == '\n') lines++; fclose(f);
    printf("after append=%d\n", lines);
    printf("remove=%d %d\n", remove("soul_test.txt"), remove("soul_test.bin"));
    printf("missing=%d\n", fopen("does_not_exist.txt", "r") == NULL);
    return 0;
}
