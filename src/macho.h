/* macho.h - minimal Mach-O reader for arm64 images, no dyld, no frameworks. */
#ifndef MACHO_H
#define MACHO_H
#include <stdint.h>
#include <stddef.h>

#define MH_MAGIC_64   0xfeedfacfu
#define MH_CIGAM_64   0xcffaedfeu
#define FAT_MAGIC     0xcafebabeu
#define FAT_CIGAM     0xbebafecau
#define CPU_TYPE_ARM64 0x0100000cu

#define LC_SEGMENT_64            0x19
#define LC_SYMTAB                0x02
#define LC_DYSYMTAB              0x0b
#define LC_LOAD_DYLIB            0x0c
#define LC_ID_DYLIB              0x0d
#define LC_LOAD_WEAK_DYLIB       0x80000018
#define LC_REEXPORT_DYLIB        0x8000001f
#define LC_LOAD_UPWARD_DYLIB     0x80000023
#define LC_LAZY_LOAD_DYLIB       0x20
#define LC_MAIN                  0x80000028
#define LC_UNIXTHREAD            0x05
#define LC_DYLD_INFO             0x22
#define LC_DYLD_INFO_ONLY        0x80000022
#define LC_DYLD_EXPORTS_TRIE     0x80000033
#define LC_DYLD_CHAINED_FIXUPS   0x80000034
#define LC_ENCRYPTION_INFO_64    0x2c
#define LC_CODE_SIGNATURE        0x1d

#define S_ZEROFILL        0x1
#define S_THREAD_LOCAL_ZEROFILL 0x12
#define S_THREAD_LOCAL_VARIABLES 0x13
#define S_THREAD_LOCAL_VAR_PTRS  0x14

typedef struct { uint32_t cmd, cmdsize; } lc_t;

typedef struct {
    uint32_t cmd, cmdsize;
    char     segname[16];
    uint64_t vmaddr, vmsize, fileoff, filesize;
    uint32_t maxprot, initprot, nsects, flags;
} seg64_t;

typedef struct {
    char sectname[16], segname[16];
    uint64_t addr, size;
    uint32_t offset, align, reloff, nreloc, flags;
    uint32_t reserved1, reserved2, reserved3;
} sect64_t;

typedef struct {
    uint32_t cmd, cmdsize;
    uint32_t symoff, nsyms, stroff, strsize;
} symtab_t;

typedef struct {
    uint32_t cmd, cmdsize;
    uint32_t name;               /* lc_str offset */
    uint32_t timestamp, current_version, compat_version;
} dylib_t;

typedef struct { uint32_t cmd, cmdsize; uint64_t entryoff, stacksize; } entry_t;

typedef struct {
    uint32_t cmd, cmdsize;
    uint32_t vmaddr, vmsize, fileoff, filesize;
    uint32_t maxprot, initprot, nsects, flags;
} seg32_t;

typedef struct {
    char     path[512];          /* install name */
    uint32_t off;                /* lc_str offset inside the image */
} dylibref_t;

typedef struct {
    uint8_t *data; size_t size;  /* slice bytes (inside file) */
    uint8_t *file;               /* whole mapped file */
    const char *path;
    uint64_t base;               /* vmaddr of __TEXT */
    uint64_t vmtext;             /* __TEXT vmaddr, before slide */
    uint64_t slide;
    uint64_t entry;              /* LC_MAIN entryoff, file offset from __TEXT */
    /* segments */
    int nseg; seg64_t *seg;
    /* dylibs */
    int ndylib; dylibref_t *dylib;
    /* symtab */
    uint32_t symoff, nsyms, stroff, strsize;
    /* dyld info */
    uint32_t bind_off, bind_size, lazy_off, lazy_size;
    uint32_t rebase_off, rebase_size, export_off, export_size;
    uint32_t weak_off, weak_size;
    /* chained */
    uint32_t chain_off, chain_size;
    /* code signature presence (encryption check) */
    uint32_t cryptid;
    char     install_name[512];  /* LC_ID_DYLIB */
    /* import census, filled while binding (used by `darwinld report`) */
    int      nimp;
    char   **imp;
    unsigned char *imp_ok;
} image_t;

int  macho_load(const char *path, image_t *im);
void macho_free(image_t *im);
void macho_list_dylibs(image_t *im);
const char *macho_symbol_at(image_t *im, uint64_t addr, uint64_t *delta);

/* export trie: returns 0 on success, fills the address and flags */
int  macho_export_lookup(image_t *im, const char *name,
                         uint64_t *addr, uint32_t *flags,
                         int *reexport_lib, const char **fallback_name);

/* callbacks used by the binder */
typedef void *(*resolve_fn)(const char *name, int lib_ordinal, void *ctx);

/* returns number of missing symbols, prints them if verbose */
int macho_bind(image_t *im, resolve_fn resolve, void *ctx, int verbose);

/* map the image into memory at its preferred address; returns slide */
int macho_map(image_t *im, void **mapbase);

#endif
