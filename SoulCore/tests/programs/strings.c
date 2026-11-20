#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
static int cmp_int(const void *a, const void *b) { int x = *(const int *)a, y = *(const int *)b; return (x > y) - (x < y); }
static int cmp_str(const void *a, const void *b) { return strcmp(*(const char *const *)a, *(const char *const *)b); }
int main(void) {
    int v[20];
    for (int i = 0; i < 20; i++) v[i] = (i * 7919) % 101 - 50;
    qsort(v, 20, sizeof(int), cmp_int);
    for (int i = 0; i < 20; i++) printf("%d ", v[i]);
    printf("\n");
    const char *words[] = { "pear", "apple", "fig", "banana", "kiwi", "cherry" };
    qsort(words, 6, sizeof(char *), cmp_str);
    for (int i = 0; i < 6; i++) printf("%s ", words[i]);
    printf("\n");
    char buf[128];
    int n = sprintf(buf, "[%5d|%-5d|%05.1f|%x|%X|%o|%c|%10s|%-6s|%.3s]", 42, -7, 3.14159, 255, 3054, 8, 'Z', "right", "left", "truncate");
    printf("%s (%d)\n", buf, n);
    snprintf(buf, 10, "%s", "0123456789abcdef");
    printf("snprintf=%s\n", buf);
    char text[] = "the quick brown fox jumps over the lazy dog";
    int count = 0;
    for (char *t = strtok(text, " "); t; t = strtok(NULL, " ")) count += (int)strlen(t);
    printf("letters=%d\n", count);
    int a, b; char w[32]; double d;
    int got = sscanf("17 -4 hello 2.5", "%d %d %31s %lf", &a, &b, w, &d);
    printf("sscanf=%d %d %d %s %.2f\n", got, a, b, w, d);
    char *dup = strdup("MiXeD CaSe");
    for (char *p = dup; *p; p++) *p = (char)toupper((unsigned char)*p);
    printf("%s %s %d\n", dup, strstr(dup, "CASE") ? "found" : "none", (int)(strchr(dup, 'X') - dup));
    free(dup);
    char *big = malloc(100000);
    for (int i = 0; i < 100000; i++) big[i] = (char)('a' + i % 26);
    big[99999] = 0;
    size_t total = 0;
    for (int k = 0; k < 10; k++) total += strlen(big + k * 1000);
    printf("total=%zu\n", total);
    char *grow = NULL; size_t cap = 0;
    for (int i = 0; i < 1000; i++) { grow = realloc(grow, cap + 16); cap += 16; grow[cap - 1] = (char)i; }
    printf("grow=%zu %d\n", cap, grow[cap - 1]);
    free(grow); free(big);
    printf("strtol %ld %lu %lld\n", strtol("-0x1f", NULL, 16), strtoul("777", NULL, 8), strtoll("123456789012", NULL, 10));
    printf("cmp %d %d %d\n", strcmp("abc", "abd") < 0, strncmp("hello", "help", 3), memcmp("xyz", "xyy", 3) > 0);
    return 0;
}
