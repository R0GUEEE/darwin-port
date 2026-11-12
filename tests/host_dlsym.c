#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <dlfcn.h>

int main(int argc, char **argv)
{
    if (argc != 3) return 2;
    void *h = dlopen(argv[1], RTLD_NOW | RTLD_GLOBAL);
    if (!h) { fprintf(stderr, "dlopen: %s\n", dlerror()); return 1; }
    const char *name = argv[2];
    void *p = dlsym(h, name);
    if (!p && name[0] == '_') p = dlsym(h, name + 1);
    printf("%p\n", p);
    return p ? 0 : 1;
}
