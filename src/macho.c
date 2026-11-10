/* macho.c - Mach-O parsing, mapping and binding (arm64, PIE + classic) */
#define _GNU_SOURCE
#include "macho.h"
#include "plat.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/stat.h>

#define ROUND_UP(x, a) (((x) + ((a) - 1)) & ~((uint64_t)(a) - 1))

static uint32_t rd32(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }
static uint64_t rd64(const uint8_t *p) { uint64_t v; memcpy(&v, p, 8); return v; }
static void    wr64(uint8_t *p, uint64_t v) { memcpy(p, &v, 8); }

static int map_file(const char *path, uint8_t **out, size_t *sz)
{
    void *b = NULL;
    if (plat_read_file(path, &b, sz) < 0) { perror(path); return -1; }
    *out = b;
    return 0;
}

int macho_load(const char *path, image_t *im)
{
    memset(im, 0, sizeof *im);
    uint8_t *file; size_t fsz;
    if (map_file(path, &file, &fsz) < 0) { perror(path); return -1; }

    size_t off = 0, len = fsz;
    uint32_t magic = rd32(file);
    if (magic == FAT_MAGIC || magic == FAT_CIGAM) {
        uint32_t n = (magic == FAT_MAGIC) ? __builtin_bswap32(rd32(file + 4))
                                          : rd32(file + 4);
        int found = 0;
        for (uint32_t i = 0; i < n; i++) {
            uint8_t *a = file + 8 + i * 20;
            uint32_t cpu = rd32(a), sub = rd32(a + 4), o = rd32(a + 8), s = rd32(a + 12);
            if (magic == FAT_CIGAM) { cpu = __builtin_bswap32(cpu); sub = __builtin_bswap32(sub);
                                      o = __builtin_bswap32(o); s = __builtin_bswap32(s); }
            if (cpu == CPU_TYPE_ARM64) { off = o; len = s; found = 1; break; }
        }
        if (!found) { fprintf(stderr, "%s: no arm64 slice in fat binary\n", path); free(file); return -2; }
    }

    uint8_t *d = file + off;
    if (rd32(d) != MH_MAGIC_64) {
        fprintf(stderr, "%s: not a 64-bit Mach-O (magic %08x)\n", path, rd32(d));
        if (rd32(d) == 0xfeedface) fprintf(stderr, "  (32-bit i386/arm binary)\n");
        free(file); return -2;
    }
    im->data = d; im->size = len; im->path = path; im->file = file;

    uint32_t sizeofcmds = rd32(d + 20);
    uint32_t p = 32, end = 32 + sizeofcmds;
    im->seg = calloc(64, sizeof(seg64_t));
    im->dylib = calloc(256, sizeof(dylibref_t));

    while (p + 8 <= end && p + 8 <= len) {
        lc_t *lc = (lc_t *)(d + p);
        if (lc->cmdsize < 8) break;
        switch (lc->cmd) {
        case LC_SEGMENT_64: {
            seg64_t *s = (seg64_t *)(d + p);
            im->seg[im->nseg++] = *s;
            if (!strcmp(s->segname, "__TEXT")) { im->base = s->vmaddr; im->vmtext = s->vmaddr; }
            break; }
        case LC_LOAD_DYLIB: case LC_LOAD_WEAK_DYLIB: case LC_REEXPORT_DYLIB:
        case LC_LOAD_UPWARD_DYLIB: case LC_LAZY_LOAD_DYLIB: {
            dylib_t *dl = (dylib_t *)(d + p);
            dylibref_t *r = &im->dylib[im->ndylib++];
            r->off = dl->name;
            snprintf(r->path, sizeof r->path, "%s", (char *)(d + p + dl->name));
            break; }
        case LC_SYMTAB: {
            symtab_t *st = (symtab_t *)(d + p);
            im->symoff = st->symoff; im->nsyms = st->nsyms;
            im->stroff = st->stroff; im->strsize = st->strsize;
            break; }
        case LC_DYLD_INFO: case LC_DYLD_INFO_ONLY: {
            uint32_t *v = (uint32_t *)(d + p + 8);
            im->rebase_off = v[0]; im->rebase_size = v[1];
            im->bind_off = v[2];   im->bind_size = v[3];
            im->weak_off = v[4];  im->weak_size = v[5];
            /* v[6],v[7] lazy */
            im->lazy_off = v[6];   im->lazy_size = v[7];
            im->export_off = v[8]; im->export_size = v[9];
            break; }
        case LC_ID_DYLIB: {
            dylib_t *dl = (dylib_t *)(d + p);
            snprintf(im->install_name, sizeof im->install_name, "%s", (char *)(d + p + dl->name));
            break; }
        case LC_DYLD_EXPORTS_TRIE: {
            uint32_t *v = (uint32_t *)(d + p + 8);
            im->export_off = v[0]; im->export_size = v[1];
            break; }
        case LC_DYLD_CHAINED_FIXUPS: {
            uint32_t *v = (uint32_t *)(d + p + 8);
            im->chain_off = v[0]; im->chain_size = v[1];
            break; }
        case LC_MAIN: {
            entry_t *e = (entry_t *)(d + p);
            im->entry = e->entryoff;   /* file offset from __TEXT start */
            break; }
        case LC_ENCRYPTION_INFO_64: {
            im->cryptid = *(uint32_t *)(d + p + 16);
            break; }
        default: break;
        }
        p += lc->cmdsize;
    }
    if (!im->base) im->base = 0x100000000ULL;
    return 0;
}

void macho_free(image_t *im)
{
    free(im->seg); free(im->dylib);
    free(im->file); im->file = NULL;
}

void macho_list_dylibs(image_t *im)
{
    for (int i = 0; i < im->ndylib; i++)
        printf("  %s\n", im->dylib[i].path);
}

/* ---------------- mapping ---------------- */

int macho_map(image_t *im, void **mapbase)
{
    uint64_t lo = ~0ULL, hi = 0;
    for (int i = 0; i < im->nseg; i++) {
        seg64_t *s = &im->seg[i];
        if (!strcmp(s->segname, "__PAGEZERO")) continue;
        if (s->vmaddr < lo) lo = s->vmaddr;
        if (s->vmaddr + s->vmsize > hi) hi = s->vmaddr + s->vmsize;
    }
    if (lo == ~0ULL) return -1;
    uint64_t span = ROUND_UP(hi - lo, 0x4000);
    /* a dylib's __TEXT starts at 0; mmap(0) "succeeds" on some kernels and
     * then every segment lands on the null page. Force a slide instead. */
    void *m = (lo >= 0x10000) ? plat_map((void *)lo, span, PROT_R, 2) : NULL;
    if (!m) {
        m = plat_map(NULL, span, PROT_R, 0);   /* any address, then slide */
        if (!m) { perror("plat_map"); return -1; }
        plat_unmap(m, span);
        im->slide = (uint64_t)m - lo;
        m = plat_map(m, span, PROT_R, 1);
        if (!m) { perror("plat_map fixed"); return -1; }
    } else {
        im->slide = 0;
    }
    for (int i = 0; i < im->nseg; i++) {
        seg64_t *s = &im->seg[i];
        if (!strcmp(s->segname, "__PAGEZERO")) continue;
        uint64_t va = s->vmaddr + im->slide;
        uint64_t vs = ROUND_UP(s->vmsize, 0x4000);
        if (!vs) continue;
        /* one 16 KiB page may hold both __TEXT and __DATA: keep RWX while binding */
        void *r = plat_map((void *)va, vs, PROT_R | PROT_W | PROT_X, 1);
        if (!r) { perror("seg plat_map"); return -1; }
        if (s->filesize) memcpy((void *)va, im->data + s->fileoff, s->filesize);
    }
    im->base = lo + im->slide;
    if (mapbase) *mapbase = (void *)im->base;
    return 0;
}

/* ---------------- binding ---------------- */

struct bindctx {
    image_t *im;
    resolve_fn resolve;
    void *uctx;
    int missing;
    int verbose;
};

static void record_import(image_t *im, const char *name, int ok)
{
    im->imp = realloc(im->imp, sizeof(char *) * (size_t)(im->nimp + 1));
    im->imp_ok = realloc(im->imp_ok, (size_t)(im->nimp + 1));
    im->imp[im->nimp] = strdup(name);
    im->imp_ok[im->nimp] = (unsigned char)ok;
    im->nimp++;
}

static void *lookup_o(struct bindctx *b, const char *name, int ord)
{
    void *a = b->resolve(name, ord, b->uctx);
    record_import(b->im, name, a != NULL);
    if (getenv("DS_TRACE")) fprintf(stderr, "bind %-40s (lib %d) -> %p\n", name, ord, a);
    if (!a) {
        const char *mf = getenv("DS_MISSING_FILE");
        if (mf) {
            FILE *f = fopen(mf, "a");
            if (f) { fprintf(f, "%s\n", name); fclose(f); }
        } else if (b->verbose) {
            fprintf(stderr, "  MISSING %s\n", name);
        }
        b->missing++;
    }
    return a;
}

/* ---- chained fixups ---- */

#define PTR_ARM64E          1
#define PTR_64              2
#define PTR_32              3
#define PTR_32_CACHE        4
#define PTR_32_FIRMWARE     5
#define PTR_64_OFFSET       6
#define PTR_ARM64E_OFFSET   7
#define PTR_ARM64E_KERNEL   8
#define PTR_64_KERNEL_CACHE 9
#define PTR_ARM64E_USERLAND 9

static int bind_chained(struct bindctx *b)
{
    image_t *im = b->im;
    const uint8_t *hdr = im->data + im->chain_off;
    uint32_t starts_off = rd32(hdr + 4), imports_off = rd32(hdr + 8),
             symbols_off = rd32(hdr + 12), imports_count = rd32(hdr + 16),
             imports_format = rd32(hdr + 20);
    const uint8_t *imports = hdr + imports_off;
    const char *symbols = (const char *)(hdr + symbols_off);

    /* build import name table */
    const char **names = calloc(imports_count + 1, sizeof(char *));
    int64_t *addends = calloc(imports_count + 1, sizeof(int64_t));
    int *libords = calloc(imports_count + 1, sizeof(int));
    for (uint32_t i = 0; i < imports_count; i++) {
        if (imports_format == 1 || imports_format == 2) {
            /* struct dyld_chained_import { lib_ordinal:8, weak:1, name_offset:23 } */
            uint32_t v = rd32(imports + i * 4);
            names[i] = symbols + (v >> 9);
            libords[i] = (int)(v & 0xFF);
            if (imports_format == 2) {
                const int32_t *ad = (const int32_t *)(imports + imports_count * 4);
                addends[i] = ad[i];
            }
        } else if (imports_format == 3) {
            uint64_t v = rd64(imports + i * 8);
            names[i] = symbols + (uint32_t)(v >> 32);
            libords[i] = (int)(v & 0xFFFF);
            const int64_t *ad = (const int64_t *)(imports + imports_count * 8);
            addends[i] = ad[i];
        } else {
            fprintf(stderr, "unsupported imports_format %u\n", imports_format);
            return -1;
        }
    }

    const uint8_t *starts = hdr + starts_off;
    uint32_t seg_count = rd32(starts);
    for (uint32_t si = 0; si < seg_count && si < (uint32_t)im->nseg; si++) {
        uint32_t seg_info_off = rd32(starts + 4 + si * 4);
        if (!seg_info_off) continue;
        const uint8_t *s = starts + seg_info_off;
        /* Two dialects of dyld_chained_starts_in_segment exist:
         *   dyld  : size, page_size, page_format, max_valid(8), pointer_format,
         *           segment_offset(8), overflow_count, page_start[]
         *   LLVM  : size, page_size, pointer_format, segment_offset(8),
         *           max_valid(4), page_count, page_start[]
         * They are told apart by the size consistency check below. */
        uint32_t size = rd32(s);
        uint16_t page_size, page_format = 0, pointer_format, page_count;
        uint64_t segment_offset;
        const uint16_t *page_start;
        if (size == 22 + 2u * *(uint16_t *)(s + 20)) {
            page_size = *(uint16_t *)(s + 4);
            pointer_format = *(uint16_t *)(s + 6);
            segment_offset = rd64(s + 8);
            page_count = *(uint16_t *)(s + 20);
            page_start = (const uint16_t *)(s + 22);
        } else {
            page_size = *(uint16_t *)(s + 4);
            page_format = *(uint16_t *)(s + 6);
            pointer_format = *(uint16_t *)(s + 16);
            segment_offset = rd64(s + 18);
            page_count = (uint16_t)((size - 28) / 2);
            page_start = (const uint16_t *)(s + 28);
        }
        (void)page_format; (void)segment_offset;
        uint8_t *segbase = (uint8_t *)(im->seg[si].vmaddr + im->slide);
        if (!segment_offset) { /* fall back: segment content offset */ }

        for (uint16_t pg = 0; pg < page_count; pg++) {
            uint16_t start = page_start[pg];
            if (start == 0xFFFF) continue;
            uint8_t *loc = segbase + (uint64_t)pg * page_size + start;
            uint64_t stride;
            for (;;) {
                uint64_t raw = rd64(loc);
                uint64_t next;
                if (pointer_format == PTR_64_OFFSET || pointer_format == PTR_64) {
                    stride = 4;
                    int is_bind = (raw >> 63) & 1;
                    next = (raw >> 51) & 0xFFF;
                    if (is_bind) {
                        uint32_t ordinal = raw & 0xFFFFFF;
                        if (ordinal >= imports_count) { b->missing++; break; }
                        int64_t addend = (int64_t)((raw >> 24) & 0xFF);
                        if (addend & 0x80) addend -= 0x100;
                        if (addend == 0) addend = addends[ordinal];
                        void *a = lookup_o(b, names[ordinal], libords[ordinal]);
                        wr64(loc, (uint64_t)a + addend);
                    } else {
                        /* PTR_64 stores an absolute vmaddr; PTR_64_OFFSET stores
                         * a vm offset from the image base, so it must be fixed
                         * up even when the image did not slide. */
                        uint64_t target = (raw & 0xFFFFFFFFFULL) |
                                          (((raw >> 36) & 0xFF) << 56);
                        if (pointer_format == PTR_64_OFFSET)
                            wr64(loc, target + im->base);
                        else if (im->slide)
                            wr64(loc, target + im->slide);
                    }
                } else if (pointer_format == PTR_ARM64E || pointer_format == PTR_ARM64E_USERLAND) {
                    stride = 8;
                    uint64_t auth = (raw >> 63) & 1, is_bind = (raw >> 62) & 1;
                    next = (raw >> 51) & 0x7FF;
                    if (is_bind) {
                        uint32_t ordinal = raw & 0xFFFFFF;
                        if (ordinal >= imports_count) { b->missing++; break; }
                        int64_t addend = (raw >> 32) & 0x7FFFF;
                        if (addend & 0x40000) addend -= 0x80000;
                        void *a = lookup_o(b, names[ordinal], libords[ordinal]);
                        wr64(loc, (uint64_t)a + addend);
                    } else {
                        uint64_t target = raw & 0x7FFFFFFFFFFULL;
                        if (auth) target = raw & 0xFFFFFFFFULL;
                        if (pointer_format == PTR_ARM64E_USERLAND)
                            wr64(loc, target + im->base);
                        else if (im->slide)
                            wr64(loc, target + im->slide);
                    }
                } else {
                    fprintf(stderr, "unsupported pointer_format %u\n", pointer_format);
                    free(names); free(addends); free(libords);
                    return -1;
                }
                if (next == 0) break;
                loc += next * stride;
            }
        }
    }
    free(names); free(addends); free(libords);
    return 0;
}

/* ---- classic dyld info bind opcodes ---- */

static uint64_t uleb(const uint8_t **pp)
{
    const uint8_t *p = *pp; uint64_t r = 0; int shift = 0;
    for (;;) { uint8_t b = *p++; r |= (uint64_t)(b & 0x7f) << shift; shift += 7; if (!(b & 0x80)) break; }
    *pp = p; return r;
}
static int64_t sleb(const uint8_t **pp)
{
    const uint8_t *p = *pp; int64_t r = 0; int shift = 0; uint8_t b;
    do { b = *p++; r |= (int64_t)(b & 0x7f) << shift; shift += 7; } while (b & 0x80);
    if (shift < 64 && (b & 0x40)) r |= -((int64_t)1 << shift);
    *pp = p; return r;
}

static int run_bind_stream(struct bindctx *b, const uint8_t *p, const uint8_t *end,
                           int lazy)
{
    image_t *im = b->im;
    int seg = -1, lib = 0;
    uint64_t off = 0;
    const char *sym = NULL;
    int64_t addend = 0;
    while (p < end) {
        uint8_t op = *p++, imm = op & 0x0f;
        switch (op & 0xf0) {
        case 0x00:
            if (lazy) { sym = NULL; addend = 0; break; }
            return 0;
        case 0x10: lib = imm; break;            /* SET_DYLIB_ORDINAL_IMM */
        case 0x20: lib = (int)uleb(&p); break;
        case 0x30: lib = imm ? imm - 16 : 0; break;  /* SPECIAL (0xFE/0xFF) */
        case 0x40: sym = (const char *)p; p += strlen(sym) + 1; break;
        case 0x50: break;                       /* type */
        case 0x60: addend = sleb(&p); break;
        case 0x70: seg = imm; off = uleb(&p); break;
        case 0x80: off += uleb(&p); break;
        case 0x90: {                            /* DO_BIND */
            if (sym) {
                void *a = lookup_o(b, sym, lib);
                uint64_t va = im->seg[seg].vmaddr + im->slide + off;
                wr64((uint8_t *)va, (uint64_t)a + addend);
            }
            off += 8;
            if (!lazy) addend = 0;
            break; }
        case 0xa0: {
            if (sym) {
                void *a = lookup_o(b, sym, lib);
                uint64_t va = im->seg[seg].vmaddr + im->slide + off;
                wr64((uint8_t *)va, (uint64_t)a + addend);
            }
            off += 8 + uleb(&p);
            break; }
        case 0xb0: {
            if (sym) {
                void *a = lookup_o(b, sym, lib);
                uint64_t va = im->seg[seg].vmaddr + im->slide + off;
                wr64((uint8_t *)va, (uint64_t)a + addend);
            }
            off += 8 + (uint64_t)imm * 8;
            break; }
        case 0xc0: {
            uint64_t count = uleb(&p), skip = uleb(&p);
            for (uint64_t i = 0; i < count; i++) {
                if (sym) {
                    void *a = lookup_o(b, sym, lib);
                    uint64_t va = im->seg[seg].vmaddr + im->slide + off;
                    wr64((uint8_t *)va, (uint64_t)a + addend);
                }
                off += 8 + skip;
            }
            break; }
        case 0xd0: return 0;                    /* THREADED (x86_64 only) */
        default: return -1;
        }
    }
    return 0;
}


/* ---- rebase opcodes ---- */
static int run_rebase_stream(image_t *im, const uint8_t *p, const uint8_t *end)
{
    int seg = -1;
    uint64_t off = 0;
    uint64_t slide = im->slide;
    while (p < end) {
        uint8_t op = *p++, imm = op & 0x0f;
        switch (op & 0xf0) {
        case 0x00: return 0;
        case 0x10: break;                       /* SET_TYPE_IMM */
        case 0x20: seg = imm; off = uleb(&p); break;
        case 0x30: off += uleb(&p); break;
        case 0x40: off += (uint64_t)imm * 8; break;
        case 0x50: {                            /* DO_REBASE_IMM_TIMES */
            for (uint8_t i = 0; i < imm; i++) {
                uint64_t va = im->seg[seg].vmaddr + off;
                wr64((uint8_t *)va, rd64((uint8_t *)va) + slide);
                off += 8;
            }
            break; }
        case 0x60: {                            /* DO_REBASE_ULEB_TIMES */
            uint64_t n = uleb(&p);
            for (uint64_t i = 0; i < n; i++) {
                uint64_t va = im->seg[seg].vmaddr + off;
                wr64((uint8_t *)va, rd64((uint8_t *)va) + slide);
                off += 8;
            }
            break; }
        case 0x70: {                            /* DO_REBASE_ADD_ADDR_ULEB */
            uint64_t va = im->seg[seg].vmaddr + off;
            wr64((uint8_t *)va, rd64((uint8_t *)va) + slide);
            off += 8 + uleb(&p);
            break; }
        case 0x80: {                            /* ..._SKIPPING_ULEB */
            uint64_t n = uleb(&p), skip = uleb(&p);
            for (uint64_t i = 0; i < n; i++) {
                uint64_t va = im->seg[seg].vmaddr + off;
                wr64((uint8_t *)va, rd64((uint8_t *)va) + slide);
                off += 8 + skip;
            }
            break; }
        default: return -1;
        }
    }
    return 0;
}

int macho_bind(image_t *im, resolve_fn resolve, void *ctx, int verbose)
{
    struct bindctx b = { im, resolve, ctx, 0, verbose };
    if (im->rebase_size && im->slide) {
        const uint8_t *p = im->data + im->rebase_off;
        run_rebase_stream(im, p, p + im->rebase_size);
    }
    if (im->chain_off) {
        if (bind_chained(&b) < 0) return -1;
    }
    if (im->bind_size) {
        const uint8_t *p = im->data + im->bind_off;
        run_bind_stream(&b, p, p + im->bind_size, 0);
    }
    if (im->weak_size) {
        const uint8_t *p = im->data + im->weak_off;
        run_bind_stream(&b, p, p + im->weak_size, 0);
    }
    if (im->lazy_size) {
        const uint8_t *p = im->data + im->lazy_off;
        run_bind_stream(&b, p, p + im->lazy_size, 1);
    }
    return b.missing;
}

/* nearest symbol for a runtime address, for the crash reporter */
const char *macho_symbol_at(image_t *im, uint64_t addr, uint64_t *delta)
{
    if (!im->nsyms) return NULL;
    const uint8_t *d = im->data;
    const char *best = NULL;
    uint64_t bestv = 0;
    const char *strs = (const char *)(d + im->stroff);
    for (uint32_t i = 0; i < im->nsyms; i++) {
        const uint8_t *e = d + im->symoff + i * 16;   /* nlist_64 */
        uint32_t strx = rd32(e);
        uint8_t type = e[4];
        uint64_t value = rd64(e + 8);
        if ((type & 0x0e) != 0x0e) continue;           /* N_SECT only */
        if (value <= addr && value >= bestv) { bestv = value; best = strs + strx; }
    }
    if (best) *delta = addr - bestv;
    return best;
}

/* ---------------- export trie ---------------- */
static int uleb_len(const uint8_t *p)
{
    int n = 0;
    while (*p & 0x80) { p++; n++; }
    return n + 1;
}
static uint64_t uleb_at(const uint8_t **pp)
{
    const uint8_t *p = *pp; uint64_t r = 0; int shift = 0;
    for (;;) { uint8_t b = *p++; r |= (uint64_t)(b & 0x7f) << shift; shift += 7; if (!(b & 0x80)) break; }
    *pp = p; return r;
}

int macho_export_lookup(image_t *im, const char *name, uint64_t *addr,
                        uint32_t *flags_out, int *reexport_lib,
                        const char **fallback_name)
{
    if (!im->export_size || !im->export_off) return -1;
    const uint8_t *trie = im->data + im->export_off;
    const uint8_t *end = trie + im->export_size;
    const uint8_t *p = trie;
    const char *s = name;

    for (;;) {
        if (p >= end) return -1;
        const uint8_t *term = p;
        uint64_t term_size = uleb_at(&p);
        if (term_size && *s == 0) {
            const uint8_t *q = term + uleb_len(term);
            uint64_t flags = uleb_at(&q);
            if (flags & 0x08) {                       /* reexport */
                uint64_t ord = uleb_at(&q);
                const char *nm = (const char *)q;
                if (reexport_lib) *reexport_lib = (int)ord;
                if (fallback_name) *fallback_name = *nm ? nm : name;
                if (flags_out) *flags_out = (uint32_t)flags;
                return 1;
            }
            uint64_t a = uleb_at(&q);
            if (flags & 0x10) uleb_at(&q);            /* stub and resolver */
            if (addr) *addr = a;
            if (flags_out) *flags_out = (uint32_t)flags;
            if (reexport_lib) *reexport_lib = 0;
            return 0;
        }
        /* children */
        int matched = 0;
        while (p < end) {
            uint8_t elen = *p++;
            if (!elen) return -1;
            const uint8_t *edge = p;
            p += elen;
            const uint8_t *save = p;
            uint64_t child = uleb_at(&p);
            if (!matched && !memcmp(edge, s, elen)) {
                s += elen;
                p = trie + child;
                matched = 1;
                break;
            }
            (void)save;
        }
        if (!matched) return -1;
    }
}
