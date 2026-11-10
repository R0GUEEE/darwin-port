#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static int cmp(const void *a, const void *b) { return *(const int *)a - *(const int *)b; }

int main(int argc, char **argv)
{
    int v[] = { 9, 2, 7, 4 };
    qsort(v, 4, sizeof v[0], cmp);
    printf("built by pxcodebuild: argc=%d v=%d%d%d%d\n", argc, v[0], v[1], v[2], v[3]);
    return 0;
}
