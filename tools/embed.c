/* embed <identifier> <input-file> — writes a C header on stdout containing
 * the file as a byte array. Built and run by the Makefile; not shipped. */
#include <stdio.h>
#include <stdlib.h>

int main(int argc, char **argv) {
    if (argc != 3) {
        fprintf(stderr, "usage: embed <identifier> <file>\n");
        return 2;
    }
    const char *name = argv[1];
    FILE *f = fopen(argv[2], "rb");
    if (!f) {
        perror(argv[2]);
        return 1;
    }
    printf("/* generated from %s — do not edit */\n", argv[2]);
    printf("static const unsigned char %s[] = {\n", name);
    int c, n = 0;
    long total = 0;
    while ((c = fgetc(f)) != EOF) {
        printf("%d,", c);
        total++;
        if (++n == 20) {
            putchar('\n');
            n = 0;
        }
    }
    /* NUL terminator so the array can be used as a C string */
    printf("0};\n");
    printf("static const unsigned long %s_len = %ld;\n", name, total);
    fclose(f);
    return 0;
}
