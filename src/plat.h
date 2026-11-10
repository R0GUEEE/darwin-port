/* plat.h - the only three things the loader needs from a host OS. */
#ifndef PLAT_H
#define PLAT_H
#include <stddef.h>
#include <stdint.h>

#define PROT_R 1
#define PROT_W 2
#define PROT_X 4

/* fixed: 0=anywhere, 1=exactly here, 2=here or fail */
void *plat_map(void *addr, size_t len, int prot, int fixed);
int   plat_unmap(void *addr, size_t len);
int   plat_protect(void *addr, size_t len, int prot);
void *plat_sym(const char *name);            /* host dynamic lookup, stripped name */
int   plat_read_file(const char *path, void **buf, size_t *len);
const char *plat_name(void);
#endif
