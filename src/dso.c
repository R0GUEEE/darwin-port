/* dso.c - multi-image loader: maps a Mach-O executable and the dylibs it
 * names, binds two-level-namespace imports against the right image, and
 * falls back to the libSystem shim for anything the host must provide. */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dlfcn.h>
#include <unistd.h>
#include "macho.h"
#include "plat.h"
#include "dso.h"
#include "shim.h"

#define MAX_DSO 96

struct dso {
    image_t  im;
    char     path[512];       /* file we loaded                    */
    char     install[512];    /* LC_ID_DYLIB / the path as imported */
    int      is_main;
    int      bound;
};

static struct dso g_dsos[MAX_DSO];
static int g_ndso;

static int file_exists(const char *p)
{
    FILE *f = fopen(p, "rb");
    if (!f) return 0;
    fclose(f);
    return 1;
}

static const char *base_of(const char *p)
{
    const char *s = strrchr(p, '/');
    return s ? s + 1 : p;
}

/* Turn an install name into a path we can open. */
static int resolve_path(const char *iname, const char *loader_dir, char *out, size_t n)
{
    if (!strncmp(iname, "@loader_path/", 13))
        snprintf(out, n, "%s/%s", loader_dir, iname + 13);
    else if (!strncmp(iname, "@executable_path/", 17))
        snprintf(out, n, "%s/%s", loader_dir, iname + 17);
    else if (!strncmp(iname, "@rpath/", 7))
        snprintf(out, n, "%s", iname + 7);
    else
        snprintf(out, n, "%s", iname);

    if (file_exists(out)) return 0;

    const char *root = getenv("DS_SYSROOT");
    if (root && iname[0] == '/') {
        snprintf(out, n, "%s%s", root, iname);
        if (file_exists(out)) return 0;
    }

    const char *sp = getenv("DS_LIB_PATH");
    while (sp && *sp) {
        const char *colon = strchr(sp, ':');
        size_t l = colon ? (size_t)(colon - sp) : strlen(sp);
        if (l && l + strlen(base_of(iname)) + 2 < n) {
            memcpy(out, sp, l);
            out[l] = '/';
            strcpy(out + l + 1, base_of(iname));
            if (file_exists(out)) return 0;
        }
        sp = colon ? colon + 1 : NULL;
    }
    /* last resort: next to the loading image */
    if (loader_dir && *loader_dir) {
        snprintf(out, n, "%s/%s", loader_dir, base_of(iname));
        if (file_exists(out)) return 0;
    }
    return -1;
}

static struct dso *find_by_path(const char *p)
{
    for (int i = 0; i < g_ndso; i++)
        if (!strcmp(g_dsos[i].path, p)) return &g_dsos[i];
    return NULL;
}

static struct dso *find_by_install(const char *n)
{
    for (int i = 0; i < g_ndso; i++)
        if (g_dsos[i].install[0] && !strcmp(g_dsos[i].install, n)) return &g_dsos[i];
    for (int i = 0; i < g_ndso; i++)
        if (!strcmp(base_of(g_dsos[i].path), base_of(n))) return &g_dsos[i];
    return NULL;
}

static struct dso *load(const char *path, int depth)
{
    if (depth > 12 || g_ndso >= MAX_DSO) return NULL;
    struct dso *have = find_by_path(path);
    if (have) return have;

    struct dso *d = &g_dsos[g_ndso];
    if (macho_load(path, &d->im) != 0) return NULL;
    if (macho_map(&d->im, NULL) != 0) {
        fprintf(stderr, "darwinld: cannot map %s\n", path);
        return NULL;
    }
    snprintf(d->path, sizeof d->path, "%s", path);
    g_ndso++;
    shim_notify_image_added(&d->im);   /* dyld: new image → registered callbacks */

    /* install name: LC_ID_DYLIB appears as the first recorded dylib for a
     * dylib image; the loader stores it explicitly in im->install_name. */
    snprintf(d->install, sizeof d->install, "%s", d->im.install_name);

    char dir[512];
    snprintf(dir, sizeof dir, "%s", path);
    char *slash = strrchr(dir, '/');
    if (slash) *slash = 0;

    for (int i = 0; i < d->im.ndylib; i++) {
        char rp[512];
        if (resolve_path(d->im.dylib[i].path, dir, rp, sizeof rp) == 0) {
            struct dso *dep = load(rp, depth + 1);
            if (!dep && getenv("DS_TRACE"))
                fprintf(stderr, "  [dso] MISSING dylib %s\n", d->im.dylib[i].path);
        } else if (getenv("DS_TRACE")) {
            fprintf(stderr, "  [dso] not found: %s\n", d->im.dylib[i].path);
        }
    }
    return d;
}

/* exports of one image; returns NULL when the symbol is not there */
static void *lookup_in(struct dso *d, const char *name)
{
    uint64_t addr = 0;
    uint32_t flags = 0;
    int re_lib = 0;
    const char *fb = NULL;
    int rc = macho_export_lookup(&d->im, name, &addr, &flags, &re_lib, &fb);

    if (rc == 0) return (void *)(uintptr_t)(addr + d->im.slide);

    if (rc == 1) {                      /* reexport: follow the ordinal */
        if (re_lib > 0 && re_lib <= d->im.ndylib) {
            char rp[512];
            char dir[512];
            snprintf(dir, sizeof dir, "%s", d->path);
            char *slash = strrchr(dir, '/');
            if (slash) *slash = 0;
            if (resolve_path(d->im.dylib[re_lib - 1].path, dir, rp, sizeof rp) == 0) {
                struct dso *t = find_by_path(rp);
                if (t) return lookup_in(t, fb ? fb : name);
            }
        }
        return NULL;
    }
    /* fall back to the classic symbol table for images with no trie */
    if (d->im.nsyms && d->im.stroff) {
        const uint8_t *data = d->im.data;
        const char *strs = (const char *)(data + d->im.stroff);
        for (uint32_t i = 0; i < d->im.nsyms; i++) {
            const uint8_t *e = data + d->im.symoff + (size_t)i * 16;
            uint32_t strx = 0; memcpy(&strx, e, 4);
            uint8_t type = e[4];
            uint64_t value = 0; memcpy(&value, e + 8, 8);
            if (!(type & 0x01)) continue;               /* N_EXT */
            int kind = type & 0x0e;
            /* N_SECT values are vmaddrs (slide applies); N_ABS values are
             * absolute and must not be slid — generated stub libraries use
             * them to declare exports without a trie. */
            if (kind != 0x0e && kind != 0x02) continue;
            if (!strcmp(strs + strx, name))
                return (void *)(uintptr_t)(kind == 0x02 ? value : value + d->im.slide);
        }
    }
    return NULL;
}

static void *resolve_sym(const char *name, int lib, void *ctx)
{
    struct dso *self = (struct dso *)ctx;

    if (lib > 0 && lib <= self->im.ndylib) {
        char dir[512], rp[512];
        snprintf(dir, sizeof dir, "%s", self->path);
        char *slash = strrchr(dir, '/');
        if (slash) *slash = 0;
        if (resolve_path(self->im.dylib[lib - 1].path, dir, rp, sizeof rp) == 0) {
            struct dso *t = find_by_path(rp);
            if (t) { void *a = lookup_in(t, name); if (a) return a; }
        }
    }
    if (lib == 0) {
        void *a = lookup_in(self, name);
        if (a) return a;
    }
    /* Flat namespace imports and unresolved two-level imports may be supplied
     * by another mapped guest image, then by explicitly loaded host runtimes. */
    for (int i = 0; i < g_ndso; i++) {
        void *a = lookup_in(&g_dsos[i], name);
        if (a) return a;
    }
    void *a = dlsym(RTLD_DEFAULT, name[0] == '_' ? name + 1 : name);
    if (a) return a;
    return dshim_lookup(name);
}

struct dso *dso_load_main(const char *path)
{
    /* Load optional host runtimes before mapping/binding guest images. */
    const char *hostlibs = getenv("DS_HOST_LIBS");
    if (hostlibs && *hostlibs) {
        char *copy = strdup(hostlibs);
        if (copy) {
            for (char *save = NULL, *p = strtok_r(copy, ":", &save); p;
                 p = strtok_r(NULL, ":", &save)) {
                if (!dlopen(p, RTLD_NOW | RTLD_GLOBAL) && getenv("DS_TRACE"))
                    fprintf(stderr, "[dso] host runtime %s: %s\n", p, dlerror());
            }
            free(copy);
        }
    }
    struct dso *d = load(path, 0);
    if (d) d->is_main = 1;
    return d;
}

int dso_bind_all(int verbose)
{
    int total = 0;
    for (int i = 0; i < g_ndso; i++) {
        if (g_dsos[i].bound) continue;
        int miss = macho_bind(&g_dsos[i].im, resolve_sym, &g_dsos[i], verbose && g_dsos[i].is_main);
        g_dsos[i].bound = 1;
        if (g_dsos[i].is_main) total = miss;
    }
    return total;
}

void dso_run_init_funcs(void)
{
    for (int i = 0; i < g_ndso; i++) {
        image_t *im = &g_dsos[i].im;
        for (int s = 0; s < im->nseg; s++) {
            seg64_t *seg = &im->seg[s];
            if (!seg->nsects) continue;
            sect64_t *sec = (sect64_t *)((uint8_t *)seg + sizeof(seg64_t));
            for (uint32_t j = 0; j < seg->nsects; j++) {
                if (strcmp(sec[j].sectname, "__mod_init_func") || !sec[j].size) continue;
                void **p = (void **)(sec[j].addr + im->slide);
                size_t n = sec[j].size / sizeof(void *);
                for (size_t k = 0; k < n; k++) {
                    void (*fn)(void) = (void (*)(void))p[k];
                    if (fn) fn();
                }
            }
        }
    }
}

int dso_count(void) { return g_ndso; }
void dso_list(void)
{
    for (int i = 0; i < g_ndso; i++)
        printf("  %s%s\n", g_dsos[i].path, g_dsos[i].is_main ? "  (main)" : "");
}
image_t *dso_main_image(void)
{
    for (int i = 0; i < g_ndso; i++) if (g_dsos[i].is_main) return &g_dsos[i].im;
    return NULL;
}

const char *dso_path_at(int i)
{
    if (i < 0 || i >= g_ndso) return NULL;
    return g_dsos[i].path;
}

image_t *dso_image_at(int i)
{
    if (i < 0 || i >= g_ndso) return NULL;
    return &g_dsos[i].im;
}
