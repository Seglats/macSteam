/* Host test tool: dxil_rewrite_tool IN OUT. Exit 0 rewritten, 1 unchanged, 2 error. */
#include <stdio.h>
#include <stdlib.h>

#include "dxil_rewrite.h"

int main(int argc, char **argv)
{
    FILE *f;
    long size;
    void *in, *out;
    size_t out_size;
    unsigned n, cb;

    if (argc != 3 || !(f = fopen(argv[1], "rb"))) return 2;
    fseek(f, 0, SEEK_END); size = ftell(f); fseek(f, 0, SEEK_SET);
    if (size <= 0 || !(in = malloc(size)) || fread(in, 1, size, f) != (size_t)size) return 2;
    fclose(f);
    if (!(n = dxil_rewrite_container(in, size, &out, &out_size, &cb))) return 1;
    if (!(f = fopen(argv[2], "wb")) || fwrite(out, 1, out_size, f) != out_size) return 2;
    fclose(f);
    printf("%u %u\n", n, cb);
    return 0;
}
