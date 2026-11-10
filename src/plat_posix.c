/* plat_posix.c - Linux/macOS backend for the Darwin loader. */
#define _GNU_SOURCE
#include "plat.h"
#include <sys/mman.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

static int mflags(int prot)
{
    int f = 0;
    if (prot & PROT_R) f |= PROT_READ;
    if (prot & PROT_W) f |= PROT_WRITE;
    if (prot & PROT_X) f |= PROT_EXEC;
    return f ? f : PROT_NONE;
}

void *plat_map(void *addr, size_t len, int prot, int fixed)
{
    int flags = MAP_PRIVATE | MAP_ANONYMOUS;
    if (fixed == 2) flags |= MAP_FIXED_NOREPLACE;   /* reserve, fail if taken */
    else if (fixed == 1) flags |= MAP_FIXED;        /* place exactly */
    else if (addr) flags |= MAP_FIXED;
    void *r = mmap(addr, len, mflags(prot), flags, -1, 0);
    return r == MAP_FAILED ? NULL : r;
}

int plat_unmap(void *a, size_t l) { return munmap(a, l); }
int plat_protect(void *a, size_t l, int p) { return mprotect(a, l, mflags(p)); }

void *plat_sym(const char *name) { return dlsym(RTLD_DEFAULT, name); }

int plat_read_file(const char *path, void **buf, size_t *len)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0) return -1;
    struct stat st;
    if (fstat(fd, &st) < 0) { close(fd); return -1; }
    void *b = malloc((size_t)st.st_size);
    size_t got = 0;
    while (got < (size_t)st.st_size) {
        ssize_t n = read(fd, (char *)b + got, (size_t)st.st_size - got);
        if (n <= 0) break;
        got += (size_t)n;
    }
    close(fd);
    *buf = b; *len = got;
    return 0;
}

const char *plat_name(void) { return "linux"; }
