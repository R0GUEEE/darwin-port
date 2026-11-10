/* main.c - darwinld: load a Darwin arm64 image and its dylibs, bind, run. */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <ucontext.h>
#include <dlfcn.h>
#include "macho.h"
#include "plat.h"
#include "shim.h"
#include "dso.h"

extern char **environ;
static image_t *g_im;

static void crash(int sig, siginfo_t *si, void *uc_)
{
    ucontext_t *uc = (ucontext_t *)uc_;
    unsigned long pc = uc->uc_mcontext.pc;
    uint64_t d = 0;
    const char *sym = g_im ? macho_symbol_at(g_im, pc, &d) : NULL;
    char buf[1024];
    int n = snprintf(buf, sizeof buf,
        "\n[darwinld] signal %d at pc=0x%lx (+0x%lx from image base) in %s\n"
        "   fault=0x%lx x0=0x%lx x1=0x%lx x2=0x%lx\n",
        sig, pc, pc - (unsigned long)(g_im ? g_im->base : 0), sym ? sym : "outside-image",
        (unsigned long)si->si_addr, (unsigned long)uc->uc_mcontext.regs[0],
        (unsigned long)uc->uc_mcontext.regs[1], (unsigned long)uc->uc_mcontext.regs[2]);
    write(2, buf, (size_t)n);
    unsigned long fp = uc->uc_mcontext.regs[29];
    for (int i = 0; i < 10 && fp; i++) {
        unsigned long *f = (unsigned long *)fp;
        unsigned long saved_fp = f[0], ret = f[1];
        uint64_t d2 = 0;
        const char *sym2 = g_im ? macho_symbol_at(g_im, ret, &d2) : NULL;
        int k = snprintf(buf, sizeof buf, "   #%d 0x%lx %s+0x%lx\n", i,
                         ret - (unsigned long)(g_im ? g_im->base : 0),
                         sym2 ? sym2 : "?", (unsigned long)d2);
        write(2, buf, (size_t)k);
        if (saved_fp <= fp) break;
        fp = saved_fp;
    }
    _exit(128 + sig);
}

static void arm_reporter(image_t *im)
{
    g_im = im;
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = crash;
    sa.sa_flags = SA_SIGINFO;
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGBUS, &sa, NULL);
    sigaction(SIGILL, &sa, NULL);
    sigaction(SIGFPE, &sa, NULL);
}

static void info(image_t *im)
{
    printf("path      : %s\n", im->path);
    if (im->install_name[0]) printf("install   : %s\n", im->install_name);
    printf("image base: 0x%llx   entry off: 0x%llx   cryptid: %u\n",
           (unsigned long long)im->base, (unsigned long long)im->entry, im->cryptid);
    printf("segments  : %d\n", im->nseg);
    for (int i = 0; i < im->nseg; i++) {
        seg64_t *s = &im->seg[i];
        printf("  %-12s vm 0x%-12llx size 0x%-10llx file 0x%-8llx/0x%-8llx prot %d/%d\n",
               s->segname, (unsigned long long)s->vmaddr, (unsigned long long)s->vmsize,
               (unsigned long long)s->fileoff, (unsigned long long)s->filesize,
               s->initprot, s->maxprot);
    }
    printf("dylibs    : %d\n", im->ndylib);
    for (int i = 0; i < im->ndylib; i++) printf("  %s\n", im->dylib[i].path);
    printf("fixups    : %s   exports: %u bytes\n",
           im->chain_off ? "chained" : (im->bind_size ? "dyld_info" : "none"), im->export_size);
}

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    arm_reporter(NULL);
    if (argc < 3) {
        fprintf(stderr,
            "usage: %s <run|check|report|info|dylibs> <macho> [args...]\n"
            "  info    segments, dylib deps, fixup kind\n"
            "  dylibs  resolve and map the dependency graph, then stop\n"
            "  check   bind everything, report unresolved imports\n"
            "  report  same as check, as JSON (per image, per symbol)\n"
            "  run     bind and execute\n", argv[0]);
        return 2;
    }
    const char *mode = argv[1];
    const char *path = argv[2];

    if (!strcmp(mode, "info")) {
        image_t im;
        if (macho_load(path, &im) != 0) return 1;
        info(&im);
        return 0;
    }

    struct dso *main_dso = dso_load_main(path);
    if (!main_dso) { fprintf(stderr, "darwinld: cannot load %s\n", path); return 1; }
    image_t *im = dso_main_image();
    if (im->cryptid) {
        fprintf(stderr, "refusing: FairPlay-encrypted (cryptid=%u)\n", im->cryptid);
        return 3;
    }
    if (!strcmp(mode, "dylibs")) {
        printf("%d images mapped:\n", dso_count());
        dso_list();
        return 0;
    }

    int missing = dso_bind_all(!strcmp(mode, "check"));
    /* diagnostics go to stderr: the guest owns stdout */
    fprintf(stderr, "[darwinld] images %d, unresolved %d, base 0x%llx\n",
            dso_count(), missing, (unsigned long long)im->base);
    if (!strcmp(mode, "report")) {          /* machine-readable import census */
        printf("{\n\"tool\": \"%s\",\n\"unresolved_total\": %d,\n\"images\": [\n", path, missing);
        for (int i = 0; i < dso_count(); i++) {
            image_t *x = dso_image_at(i);
            const char *ip = dso_path_at(i);
            int u = 0;
            for (int k = 0; k < x->nimp; k++) if (!x->imp_ok[k]) u++;
            printf(" {\"path\": \"%s\", \"imports\": %d, \"unresolved\": %d%s%s%s", ip, x->nimp, u,
                   u ? ", \"missing\": [" : "", u ? "" : "", u ? "" : "");
            if (u) {
                int first = 1;
                for (int k = 0; k < x->nimp; k++)
                    if (!x->imp_ok[k]) { printf("%s\"%s\"", first ? "" : ", ", x->imp[k]); first = 0; }
                printf("]");
            }
            printf("}%s\n", i + 1 < dso_count() ? "," : "");
        }
        printf("]\n}\n");
        return missing ? 1 : 0;
    }
    if (!strcmp(mode, "check")) return missing ? 1 : 0;
    if (missing) {
        fprintf(stderr, "cannot run: %d imports unresolved\n", missing);
        return 4;
    }

    arm_reporter(im);

    int g_argc = argc - 2;
    char **g_argv = argv + 2;
    shim_set_args(g_argc, g_argv, environ);
    dso_run_init_funcs();

    uint64_t entry = im->base - (im->base - im->vmtext) + im->entry;
    (void)entry;
    entry = im->vmtext + im->slide + im->entry;

    typedef int (*main_fn)(int, char **, char **, void *);
    int rc = ((main_fn)(void *)entry)(g_argc, g_argv, environ, NULL);
    fflush(stdout);
    return rc;
}
