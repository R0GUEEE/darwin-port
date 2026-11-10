/* shim.c - Darwin libSystem/libc entry points implemented over the host libc.
 *
 * Every Mach-O symbol name carries one leading underscore; strip it and the
 * name is usually a plain POSIX name the host already provides.  What is left
 * is the part of libSystem that is not POSIX: dyld services, mach time,
 * malloc zones, os_log, the _NSGet* accessors and Apple's fortified helpers.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <time.h>
#include <stdarg.h>
#include <pthread.h>
#include <sys/time.h>
#include <sys/stat.h>
#include <signal.h>
#include <ctype.h>
#include <dlfcn.h>
#include "plat.h"
#include "shim.h"
#include "macho.h"

/* assembly front ends (fmt_asm.S) - they capture sp for Darwin varargs */
int ds_printf_stub(void), ds_fprintf_stub(void), ds_sprintf_stub(void);
int ds_snprintf_stub(void), ds_asprintf_stub(void), ds_dprintf_stub(void);
int ds___sprintf_chk_stub(void), ds___snprintf_chk_stub(void);
void ds_err_stub(void), ds_warn_stub(void), ds_warnx_stub(void);
int ds_vfprintf(FILE *, const char *, void *);
int ds_vsprintf(char *, const char *, void *);
int ds_vsnprintf(char *, size_t, const char *, void *);
int ds_vasprintf(char **, const char *, void *);
int ds_vsnprintf_chk(char *, size_t, int, size_t, const char *, void *);
void ds_dprintf_c(int, const char *, const void *);
int  ds_setjmp(void *);
void ds_longjmp(void *, int);
int  ds_sigsetjmp(void *, int);
void ds_siglongjmp(void *, int);

/* program name used by err/warn/errx/warnx */
const char *ds_progname = "guest";

/* Darwin C++ is std::__1, which is exactly libc++'s namespace everywhere, so
 * a host libc++ can satisfy many imports - if it is loaded. */
__attribute__((constructor)) static void shim_load_cxx(void)
{
    dlopen("libc++.so.1", RTLD_NOW | RTLD_GLOBAL);
    dlopen("libc++abi.so.1", RTLD_NOW | RTLD_GLOBAL);
    dlopen("libz.so.1", RTLD_NOW | RTLD_GLOBAL);      /* uncompress/compress */
    dlopen("libbz2.so.1", RTLD_NOW | RTLD_GLOBAL);
    dlopen("libresolv.so.2", RTLD_NOW | RTLD_GLOBAL);
}

/* ---- process context the guest may ask about ---- */
static int    g_argc;
static char **g_argv;
static char **g_envp;

extern const char *ds_progname;
void shim_set_args(int argc, char **argv, char **envp)
{
    g_argc = argc; g_argv = argv; g_envp = envp;
    if (argc && argv && argv[0]) {
        const char *slash = strrchr(argv[0], '/');
        ds_progname = slash ? slash + 1 : argv[0];
    }
}

/* ---- dyld ---- */
static void dyld_stub_binder(void) { /* lazy stubs are resolved eagerly */ }

/* ---- errno ---- */
static int *__error(void) { return &errno; }

/* ---- _NSGet* ---- */
static int * _NSGetArgc(void) { return &g_argc; }
static char *** _NSGetArgv(void) { return &g_argv; }
static char *** _NSGetEnviron(void) { return &g_envp; }
static int _NSGetExecutablePath(char *buf, unsigned int *sz)
{
    const char *p = g_argv && g_argv[0] ? g_argv[0] : "/";
    unsigned n = (unsigned)strlen(p) + 1;
    if (*sz < n) { *sz = n; return -1; }
    memcpy(buf, p, n);
    *sz = n;
    return 0;
}

/* ---- mach time ---- */
static uint64_t mach_absolute_time(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + ts.tv_nsec;
}
static uint64_t mach_continuous_time(void) { return mach_absolute_time(); }
struct mach_timebase_info { uint32_t numer, denom; };
static int mach_timebase_info(struct mach_timebase_info *i) { i->numer = 1; i->denom = 1; return 0; }
static uint64_t clock_gettime_nsec_np(int clk)
{
    struct timespec ts;
    if (clock_gettime((clockid_t)clk, &ts) != 0) return 0;
    return (uint64_t)ts.tv_sec * 1000000000ull + ts.tv_nsec;
}

/* ---- malloc zones ---- */
static void *malloc_default_zone(void) { return (void *)1; }
static void *malloc_zone_malloc(void *z, size_t n) { (void)z; return malloc(n); }
static void *malloc_zone_calloc(void *z, size_t n, size_t s) { (void)z; return calloc(n, s); }
static void *malloc_zone_realloc(void *z, void *p, size_t n) { (void)z; return realloc(p, n); }
static void  malloc_zone_free(void *z, void *p) { (void)z; free(p); }
static size_t malloc_size(const void *p) { (void)p; return 0; }
static size_t malloc_good_size(size_t n) { return n; }

/* ---- os_log: no unified logging on this host ---- */
static void os_log_impl(void *dso, void *cat, int type, const char *fmt, ...)
{ (void)dso; (void)cat; (void)type; (void)fmt; }
static void *os_log_create(const char *sub, const char *cat) { (void)sub; (void)cat; return (void *)1; }
static int  os_log_type_enabled(void *dso, int type) { (void)dso; (void)type; return 0; }

/* ---- sysctl ---- */
static int sysctlbyname(const char *name, void *oldp, size_t *oldlenp,
                        void *newp, size_t newlen)
{
    (void)newp; (void)newlen;
    if (!strcmp(name, "hw.ncpu")) { int v = 1; if (oldp && oldlenp && *oldlenp >= 4) { memcpy(oldp, &v, 4); *oldlenp = 4; return 0; } }
    if (!strcmp(name, "hw.memsize")) { long v = 1L << 30; if (oldp && oldlenp && *oldlenp >= 8) { memcpy(oldp, &v, 8); *oldlenp = 8; return 0; } }
    errno = ENOTSUP;
    return -1;
}

/* ---- misc libSystem ---- */
static const char *getprogname(void) { return (g_argv && g_argv[0]) ? g_argv[0] : "guest"; }
static void setprogname(const char *p) { (void)p; }

static size_t shim_strlcpy(char *d, const char *s, size_t n)
{
    size_t l = strlen(s);
    if (n) { size_t c = l < n - 1 ? l : n - 1; memcpy(d, s, c); d[c] = 0; }
    return l;
}
static size_t shim_strlcat(char *d, const char *s, size_t n)
{
    size_t dl = strnlen(d, n);
    if (dl == n) return n + strlen(s);
    return dl + shim_strlcpy(d + dl, s, n - dl);
}

/* Apple's optimised entry points */
static void *p_memcpy(void *d, const void *s, size_t n) { return memcpy(d, s, n); }
static void *p_memmove(void *d, const void *s, size_t n) { return memmove(d, s, n); }
static void *p_memset(void *d, int c, size_t n) { return memset(d, c, n); }
static int   p_memcmp(const void *a, const void *b, size_t n) { return memcmp(a, b, n); }
static size_t p_strlen(const char *s) { return strlen(s); }

static int shim_pthread_setname_np(const char *name) { (void)name; return 0; }
static int pthread_threadid_np(void *tid, uint64_t *out)
{ (void)tid; if (out) *out = (uint64_t)getpid(); return 0; }

/* ---- atexit bridge for guest constructors ---- */
#define MAX_ATE 256
static struct { void (*fn)(void *); void *arg; } g_ate[MAX_ATE];
static int g_nate;
static void run_ate(void) { while (g_nate > 0) { g_nate--; if (g_ate[g_nate].fn) g_ate[g_nate].fn(g_ate[g_nate].arg); } }
static int __cxa_atexit(void (*fn)(void *), void *arg, void *dso)
{
    (void)dso;
    if (g_nate >= MAX_ATE) return -1;
    if (g_nate == 0) atexit(run_ate);
    g_ate[g_nate].fn = fn; g_ate[g_nate].arg = arg; g_nate++;
    return 0;
}
static void __cxa_finalize(void *dso) { (void)dso; }

static void __assert_rtn(const char *fn, const char *file, int line, const char *expr)
{
    fprintf(stderr, "Assertion failed: (%s), function %s, file %s, line %d.\n",
            expr, fn ? fn : "?", file ? file : "?", line);
    abort();
}


/* ---- fortified + BSD helpers used by every clang-built tool ---- */
static int __sprintf_chk(char *dst, int flag, size_t dstlen, const char *fmt, ...)
{
    (void)flag;
    va_list ap; va_start(ap, fmt);
    int n = vsnprintf(dst, dstlen, fmt, ap);
    va_end(ap);
    return n;
}
static int __snprintf_chk(char *dst, size_t n, int flag, size_t dstlen, const char *fmt, ...)
{
    (void)flag;
    if (n > dstlen) n = dstlen;
    va_list ap; va_start(ap, fmt);
    int r = vsnprintf(dst, n, fmt, ap);
    va_end(ap);
    return r;
}
static int __vsnprintf_chk(char *dst, size_t n, int flag, size_t dstlen, const char *fmt, va_list ap)
{ (void)flag; if (n > dstlen) n = dstlen; return vsnprintf(dst, n, fmt, ap); }
static char *__strcpy_chk(char *d, const char *s, size_t n) { (void)n; return strcpy(d, s); }
static char *__strcat_chk(char *d, const char *s, size_t n) { (void)n; return strcat(d, s); }
static void *__memcpy_chk(void *d, const void *s, size_t n, size_t dn) { (void)dn; return memcpy(d, s, n); }
static void *__memset_chk(void *d, int c, size_t n, size_t dn) { (void)dn; return memset(d, c, n); }

/* Stack probing on Darwin: the caller has already allocated the frame. */
static void __chkstk_darwin(void) { }

static void *shim_reallocf(void *p, size_t n)
{
    void *q = realloc(p, n);
    if (!q && n) free(p);
    return q;
}

static const char *mach_error_string(int code)
{
    static char buf[64];
    snprintf(buf, sizeof buf, "mach error %d", code);
    return buf;
}

/* Darwin's strmode: 11 characters, "drwxr-xr-x " */
static void shim_strmode(int mode, char *p)
{
    static const char rwx[] = "rwxrwxrwx";
    switch (mode & S_IFMT) {
    case S_IFDIR:  p[0] = 'd'; break;
    case S_IFCHR:  p[0] = 'c'; break;
    case S_IFBLK:  p[0] = 'b'; break;
    case S_IFLNK:  p[0] = 'l'; break;
    case S_IFSOCK: p[0] = 's'; break;
    case S_IFIFO:  p[0] = 'p'; break;
    default:       p[0] = '-'; break;
    }
    for (int i = 0; i < 9; i++)
        p[i + 1] = (mode & (1 << (8 - i))) ? rwx[i] : '-';
    if (mode & S_ISUID) p[3] = (p[3] == 'x') ? 's' : 'S';
    if (mode & S_ISGID) p[6] = (p[6] == 'x') ? 's' : 'S';
    if (mode & S_ISVTX) p[9] = (p[9] == 'x') ? 't' : 'T';
    p[10] = ' ';
    p[11] = 0;
}


/* ---- Darwin <sys/stat.h> ABI --------------------------------------------
 * Darwin arm64 struct stat is 144 bytes and orders fields differently from
 * Linux: passing a Linux struct stat through gives the guest wrong st_mode,
 * wrong st_size and garbage pointers where it expects times.  Everything the
 * guest can see must be written in the Darwin layout. */
struct ds_timespec { int64_t tv_sec, tv_nsec; };
struct ds_stat {
    int32_t  st_dev;
    uint16_t st_mode;
    uint16_t st_nlink;
    uint64_t st_ino;
    uint32_t st_uid, st_gid;
    int32_t  st_rdev;
    int32_t  _pad0;
    struct ds_timespec st_atimespec, st_mtimespec, st_ctimespec, st_birthtimespec;
    int64_t  st_size, st_blocks;
    int32_t  st_blksize;
    uint32_t st_flags, st_gen;
    int32_t  st_lspare;
    int64_t  st_qspare[2];
};

static void ds_fill(struct ds_stat *o, const struct stat *i)
{
    memset(o, 0, sizeof *o);
    o->st_dev = (int32_t)i->st_dev;
    o->st_mode = (uint16_t)i->st_mode;
    o->st_nlink = (uint16_t)i->st_nlink;
    o->st_ino = (uint64_t)i->st_ino;
    o->st_uid = (uint32_t)i->st_uid;
    o->st_gid = (uint32_t)i->st_gid;
    o->st_rdev = (int32_t)i->st_rdev;
    o->st_atimespec.tv_sec = i->st_atim.tv_sec;  o->st_atimespec.tv_nsec = i->st_atim.tv_nsec;
    o->st_mtimespec.tv_sec = i->st_mtim.tv_sec;  o->st_mtimespec.tv_nsec = i->st_mtim.tv_nsec;
    o->st_ctimespec.tv_sec = i->st_ctim.tv_sec;  o->st_ctimespec.tv_nsec = i->st_ctim.tv_nsec;
    o->st_size = (int64_t)i->st_size;
    o->st_blocks = (int64_t)i->st_blocks;
    o->st_blksize = (int32_t)i->st_blksize;
}

static int ds_stat(const char *p, struct ds_stat *o)
{ struct stat s; if (stat(p, &s) != 0) return -1; ds_fill(o, &s); return 0; }
static int ds_lstat(const char *p, struct ds_stat *o)
{ struct stat s; if (lstat(p, &s) != 0) return -1; ds_fill(o, &s); return 0; }
static int ds_fstat(int fd, struct ds_stat *o)
{ struct stat s; if (fstat(fd, &s) != 0) return -1; ds_fill(o, &s); return 0; }
static int ds_fstatat(int fd, const char *p, struct ds_stat *o, int fl)
{ struct stat s; if (fstatat(fd, p, &s, fl) != 0) return -1; ds_fill(o, &s); return 0; }

/* Darwin sigset_t is a single 32-bit word; Linux's is 128 bytes. */
static int ds_sigprocmask(int how, const uint32_t *set, uint32_t *old)
{
    sigset_t ns, os;
    sigemptyset(&ns);
    if (set) for (int i = 0; i < 32; i++) if (*set & (1u << i)) sigaddset(&ns, i);
    if (sigprocmask(how, set ? &ns : NULL, old ? &os : NULL) != 0) return -1;
    if (old) { uint32_t m = 0; for (int i = 0; i < 32; i++) if (sigismember(&os, i)) m |= 1u << i; *old = m; }
    return 0;
}


/* ---- Darwin <fcntl.h> flags --------------------------------------------
 * Every one of these numbers differs from Linux, so a passthrough does not
 * just fail - it fails with the wrong meaning (Darwin O_CREAT 0x200 is
 * Linux O_SYNC bits; Darwin O_TRUNC 0x400 is Linux O_APPEND). */
#include <fcntl.h>
#define DS_O_ACCMODE   0x0003
#define DS_O_NONBLOCK  0x0004
#define DS_O_APPEND    0x0008
#define DS_O_FSYNC     0x0080
#define DS_O_NOFOLLOW  0x0100
#define DS_O_CREAT     0x0200
#define DS_O_TRUNC     0x0400
#define DS_O_EXCL      0x0800
#define DS_O_NOCTTY    0x20000
#define DS_O_DIRECTORY 0x100000
#define DS_O_CLOEXEC   0x1000000

static int ds_xlate_open(int f)
{
    int r = f & DS_O_ACCMODE;
    if (f & DS_O_NONBLOCK)  r |= O_NONBLOCK;
    if (f & DS_O_APPEND)    r |= O_APPEND;
    if (f & DS_O_FSYNC)     r |= O_SYNC;
    if (f & DS_O_NOFOLLOW)  r |= O_NOFOLLOW;
    if (f & DS_O_CREAT)     r |= O_CREAT;
    if (f & DS_O_TRUNC)     r |= O_TRUNC;
    if (f & DS_O_EXCL)      r |= O_EXCL;
#ifndef __linux__
    if (f & DS_O_NOCTTY)    r |= O_NOCTTY;
#endif
    if (f & DS_O_DIRECTORY) r |= O_DIRECTORY;
    if (f & DS_O_CLOEXEC)   r |= O_CLOEXEC;
    return r;
}

static int ds_open(const char *path, int flags, ...)
{
    mode_t mode = 0;
    if (flags & DS_O_CREAT) {
        va_list ap; va_start(ap, flags); mode = (mode_t)va_arg(ap, int); va_end(ap);
    }
    return open(path, ds_xlate_open(flags), mode);
}

static int ds_openat(int dirfd, const char *path, int flags, ...)
{
    mode_t mode = 0;
    if (flags & DS_O_CREAT) {
        va_list ap; va_start(ap, flags); mode = (mode_t)va_arg(ap, int); va_end(ap);
    }
    return openat(dirfd, path, ds_xlate_open(flags), mode);
}

static int ds_mkstemp(char *tmpl) { return mkstemp(tmpl); }


/* ---- exec of another Darwin binary -------------------------------------
 * The kernel cannot run Mach-O, so re-enter ourselves: execv("/proc/self/exe",
 * {me, "run", <mach-o>, <argv...>}).  That makes ar's internal ranlib call
 * work, and any tool that shells out to another Xcode binary. */
static const char *macho_path_of(const char *file, char *out, size_t outsz)
{
    const char *use = file;
    char cand[1024];
    if (!strchr(file, '/')) {
        const char *path = getenv("PATH");
        use = NULL;
        while (path && *path) {
            const char *colon = strchr(path, ':');
            size_t l = colon ? (size_t)(colon - path) : strlen(path);
            if (l && l < sizeof cand - strlen(file) - 2) {
                memcpy(cand, path, l);
                cand[l] = '/';
                strcpy(cand + l + 1, file);
                if (access(cand, X_OK) == 0) { use = cand; break; }
            }
            path = colon ? colon + 1 : NULL;
        }
        if (!use) return NULL;
    }
    int fd = open(use, O_RDONLY);
    if (fd < 0) return NULL;
    unsigned char m[8];
    ssize_t n = read(fd, m, 8);
    close(fd);
    if (n < 8) return NULL;
    int macho = (m[0] == 0xcf && m[1] == 0xfa && m[2] == 0xed && m[3] == 0xfe)
             || (m[0] == 0xca && m[1] == 0xfe && m[2] == 0xba && m[3] == 0xbe)
             || (m[0] == 0xce && m[1] == 0xfa && m[2] == 0xed && m[3] == 0xfe);
    if (!macho) return NULL;
    snprintf(out, outsz, "%s", use);
    return out;
}

int ds_execvp(const char *file, char *const argv[])
{
    char path[1024];
    if (macho_path_of(file, path, sizeof path)) {
        int n = 0;
        while (argv && argv[n]) n++;
        char **na = calloc((size_t)n + 4, sizeof(char *));
        char me[1024];
        ssize_t l = readlink("/proc/self/exe", me, sizeof me - 1);
        if (l <= 0) snprintf(me, sizeof me, "/proc/self/exe"); else me[l] = 0;
        na[0] = me; na[1] = "run"; na[2] = path;
        for (int i = 1; i < n; i++) na[i + 2] = argv[i];
        na[n + 2] = NULL;
        execv(me, na);
        free(na);
    }
    return execvp(file, argv);
}


/* ---- Blocks runtime ---------------------------------------------------
 * The compiler-emitted block literals reference these; for the plain blocks
 * Xcode tools use (no __block objects captured), assign/dispose are no-ops. */
static void __Block_object_assign(void *dst, const void *src, int flags)
{ (void)dst; (void)src; (void)flags; }
static void __Block_object_dispose(const void *obj, int flags) { (void)obj; (void)flags; }
static struct { void *isa; int flags; int reserved; void *invoke; void *descriptor; } g_global_block;
static struct { void *isa; int flags; int reserved; void *invoke; void *descriptor; } g_stack_block;
#define __NSConcreteGlobalBlock (&g_global_block)
#define __NSConcreteStackBlock  (&g_stack_block)

/* ---- <ctype.h> private entry points (Darwin masks) ---- */
/* Darwin's _DefaultRuneLocale is a 256-entry table; guests rarely read it
 * directly, but the symbol must exist and be readable. */
static unsigned char g_rune_locale[4096];
#define _DefaultRuneLocale ((void *)g_rune_locale)
static int __maskrune(int c, unsigned long f)
{
    unsigned char ch = (unsigned char)c;
    int r = 0;
    if ((f & 0x00000100) && isalpha(ch)) r = 1;
    if ((f & 0x00000200) && iscntrl(ch)) r = 1;
    if ((f & 0x00000400) && isdigit(ch)) r = 1;
    if ((f & 0x00000800) && isgraph(ch)) r = 1;
    if ((f & 0x00001000) && islower(ch)) r = 1;
    if ((f & 0x00002000) && ispunct(ch)) r = 1;
    if ((f & 0x00004000) && isspace(ch)) r = 1;
    if ((f & 0x00008000) && isupper(ch)) r = 1;
    if ((f & 0x00010000) && isxdigit(ch)) r = 1;
    if ((f & 0x00020000) && (ch == ' ' || ch == '\t')) r = 1;
    if ((f & 0x00040000) && isprint(ch)) r = 1;
    return r;
}


/* Darwin copyfile(3): plain byte copy is enough for the tools that use it. */
#define COPYFILE_ACL      (1<<0)
#define COPYFILE_STAT     (1<<1)
#define COPYFILE_XATTR    (1<<2)
#define COPYFILE_DATA     (1<<3)
static void *copyfile_state_alloc(void) { return calloc(1, 64); }
static int copyfile_state_free(void *st) { free(st); return 0; }
static int ds_copyfile(const char *from, const char *to, void *state, int flags)
{
    (void)state; (void)flags;
    int in = open(from, O_RDONLY);
    if (in < 0) return -1;
    int out = open(to, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (out < 0) { close(in); return -1; }
    char buf[65536];
    ssize_t n;
    int rc = 0;
    while ((n = read(in, buf, sizeof buf)) > 0) {
        ssize_t off = 0;
        while (off < n) {
            ssize_t w = write(out, buf + off, (size_t)(n - off));
            if (w <= 0) { rc = -1; break; }
            off += w;
        }
        if (rc) break;
    }
    close(in); close(out);
    return rc;
}


/* ---- remaining small libSystem pieces ---- */
static size_t __strlcpy_chk(char *d, const char *s, size_t n, size_t dstlen)
{ if (n > dstlen) n = dstlen; return shim_strlcpy(d, s, n); }
static size_t __strlcat_chk(char *d, const char *s, size_t n, size_t dstlen)
{ if (n > dstlen) n = dstlen; return shim_strlcat(d, s, n); }

static int __toupper(int c) { return toupper(c); }
static int __tolower(int c) { return tolower(c); }

static long long ds_strtonum(const char *s, long long lo, long long hi, const char **err)
{
    char *end = NULL;
    errno = 0;
    long long v = strtoll(s, &end, 10);
    if (err) *err = NULL;
    if (end == s || (end && *end)) { if (err) *err = "invalid"; return 0; }
    if (errno || v < lo || v > hi) { if (err) *err = "out of range"; return 0; }
    return v;
}

static int ds_backtrace(void **buf, int size) { (void)size; if (size > 0) buf[0] = NULL; return 0; }
static int darwin_check_fd_set_overflow(int fd, const void *set, int which)
{ (void)fd; (void)set; (void)which; return 1; }

static unsigned __int128 ds_udivti3(unsigned __int128 a, unsigned __int128 b) { return a / b; }
static __int128 ds_divti3(__int128 a, __int128 b) { return a / b; }
static unsigned __int128 ds_umodti3(unsigned __int128 a, unsigned __int128 b) { return a % b; }
static __int128 ds_modti3(__int128 a, __int128 b) { return a % b; }

static int ds_tlv_atexit(void (*fn)(void *), void *arg, void *dso)
{ return __cxa_atexit(fn, arg, dso); }

static int ds_clonefile(const char *a, const char *b, int flags) { (void)a; (void)b; (void)flags; errno = ENOTSUP; return -1; }
static int ds_clonefileat(int fd1, const char *a, int fd2, const char *b, int flags)
{ (void)fd1; (void)a; (void)fd2; (void)b; (void)flags; errno = ENOTSUP; return -1; }
static int ds_fsctl(int fd, unsigned long req, void *data) { (void)fd; (void)req; (void)data; errno = ENOTSUP; return -1; }


/* ---- Mach traps (just enough for tools that ask who they are) ---- */
typedef unsigned int mach_port_t;
static mach_port_t ds_mach_task_self = 0x0203;      /* data symbol */
static mach_port_t ds_mach_host_self(void) { return 0x0303; }
static int ds_task_set_exception_ports(mach_port_t t, int exc, mach_port_t hp,
                                       int flavor, unsigned long long *masks, unsigned nm)
{ (void)t; (void)exc; (void)hp; (void)flavor; (void)masks; (void)nm; return 0; }
static int ds_host_info(mach_port_t h, int flavor, void *info, unsigned *cnt)
{ (void)h; if (info && cnt && *cnt >= 16) { memset(info, 0, *cnt); return 0; } (void)flavor; return 5; }
static int ds_mach_port_deallocate(mach_port_t t, mach_port_t p) { (void)t; (void)p; return 0; }
static int ds_vm_deallocate(mach_port_t t, unsigned long long a, unsigned long long n)
{ (void)t; (void)a; (void)n; return 0; }

/* ---- xcselect: how Xcode tools locate the developer directory ---- */
static char *ds_xcselect_get_developer_dir_path(char *path, size_t pathsz, size_t *actual,
                                                char **version, int *fromcache)
{
    const char *dd = getenv("DEVELOPER_DIR");
    if (!dd) dd = "/root/xc/Xcode-beta.app/Contents/Developer";
    size_t n = strlen(dd);
    if (getenv("DS_TRACE_XC")) fprintf(stderr, "[xc] enter get_developer_dir_path\n");
    if (path && pathsz) { size_t c = n < pathsz - 1 ? n : pathsz - 1; memcpy(path, dd, c); path[c] = 0; }
    if (actual) *actual = n;
    if (version) *version = NULL;
    if (fromcache) *fromcache = 0;
    if (getenv("DS_TRACE_XC"))
        fprintf(stderr, "[xc] get_developer_dir_path(path=%p sz=%zu) -> '%s' len=%zu\n",
                (void *)path, pathsz, path ? path : "(null)", n);
    return 0;                       /* Darwin returns int, 0 = success */
}
static int ds_xcselect_invoke_xcrun(const char *tool, int argc, char **argv, int verbose)
{
    (void)verbose;
    if (getenv("DS_TRACE_XC"))
        fprintf(stderr, "[xc] invoke_xcrun(tool=%s, argc=%d, argv0=%s)\n",
                tool ? tool : "(null)", argc, (argv && argv[0]) ? argv[0] : "-");
    if (!tool) return -1;
    char *nargv[64];
    int n = 0;
    nargv[n++] = (char *)tool;
    for (int i = 0; i < argc && n < 63; i++) nargv[n++] = argv[i];
    nargv[n] = NULL;

    extern int ds_execvp(const char *file, char *const argv[]);
    /* xcrun's job: run the tool out of the developer directory.  ds_execvp
     * re-enters this loader for Mach-O, so the tool really runs. */
    char cand[1024];
    const char *dd = getenv("DEVELOPER_DIR");
    if (!dd) dd = "/root/xc/Xcode-beta.app/Contents/Developer";
    const char *subs[] = {
        "/usr/bin/", "/Toolchains/XcodeDefault.xctoolchain/usr/bin/",
        "/usr/libexec/", NULL
    };
    for (int i = 0; subs[i]; i++) {
        snprintf(cand, sizeof cand, "%s%s%s", dd, subs[i], tool);
        ds_execvp(cand, nargv);
    }
    ds_execvp(tool, nargv);
    return -1;
}

/* ---- BSD stdio internals used by bison/yacc ---- */
static int ds___srget(FILE *fp) { return fgetc(fp); }
static int ds___swbuf(int c, FILE *fp) { return fputc(c, fp); }
static size_t ds___mb_cur_max = 1;

/* ---- misc ---- */
static void ds_memset_pattern16(void *b, const void *pat, size_t len)
{
    unsigned char *p = (unsigned char *)b;
    for (size_t i = 0; i < len; i++) p[i] = ((const unsigned char *)pat)[i & 15];
}
static unsigned ds_arc4random(void)
{
    unsigned v = 0;
    FILE *f = fopen("/dev/urandom", "rb");
    if (f) { if (fread(&v, sizeof v, 1, f) != 1) v = 0; fclose(f); }
    return v;
}
static void ds_arc4random_buf(void *buf, size_t n)
{
    FILE *f = fopen("/dev/urandom", "rb");
    if (f) { size_t got = fread(buf, 1, n, f); (void)got; fclose(f); }
}


/* ---- Darwin signal ABI ----
 * Darwin: struct sigaction { handler; uint32 mask; int flags; }
 * Linux : { handler; sigset128 mask; int flags; restorer; }
 * Flags disagree in every bit too, and stack_t orders ss_size before ss_flags
 * on Darwin but after them on Linux - passing one through gives ENOMEM from
 * sigaltstack and silently wrong handlers from sigaction. */
struct ds_sigaction { void *handler; uint32_t sa_mask; int sa_flags; };

static int ds_xlate_sigflags(int f)
{
    int r = 0;
    if (f & 0x0001) r |= SA_ONSTACK;
    if (f & 0x0002) r |= SA_RESTART;
    if (f & 0x0004) r |= SA_RESETHAND;
    if (f & 0x0008) r |= SA_NOCLDSTOP;
    if (f & 0x0010) r |= SA_NODEFER;
    if (f & 0x0020) r |= SA_NOCLDWAIT;
    if (f & 0x0040) r |= SA_SIGINFO;
    return r;
}

static int ds_sigaction(int sig, const struct ds_sigaction *act, struct ds_sigaction *old)
{
    struct sigaction na, oa;
    memset(&na, 0, sizeof na);
    if (act) {
        na.sa_handler = (void (*)(int))act->handler;
        sigemptyset(&na.sa_mask);
        for (int i = 0; i < 32; i++)
            if (act->sa_mask & (1u << i)) sigaddset(&na.sa_mask, i);
        na.sa_flags = ds_xlate_sigflags(act->sa_flags);
    }
    if (sigaction(sig, act ? &na : NULL, old ? &oa : NULL) != 0) return -1;
    if (old) {
        uint32_t m = 0;
        for (int i = 0; i < 32; i++) if (sigismember(&oa.sa_mask, i)) m |= 1u << i;
        old->handler = (void *)oa.sa_handler;
        old->sa_mask = m;
        old->sa_flags = 0;
    }
    return 0;
}

struct ds_stack_t { void *ss_sp; size_t ss_size; int ss_flags; };

static int ds_sigaltstack(const struct ds_stack_t *ss, struct ds_stack_t *old)
{
    stack_t ns, os;
    memset(&ns, 0, sizeof ns);
    if (ss) {
        ns.ss_sp = ss->ss_sp;
        ns.ss_size = ss->ss_size > 8192 ? ss->ss_size : 8192;
        ns.ss_flags = ss->ss_flags == 1 ? SS_DISABLE : 0;
    }
    if (sigaltstack(ss ? &ns : NULL, old ? &os : NULL) != 0) return -1;
    if (old) { old->ss_sp = os.ss_sp; old->ss_size = os.ss_size; old->ss_flags = 0; }
    return 0;
}


/* ---- more mach ---- */
typedef unsigned long long vm_address_t;
static int ds_vm_allocate(mach_port_t t, vm_address_t *addr, unsigned long long size, int flags)
{
    (void)t; (void)flags;
    void *p = plat_map(NULL, (size_t)size, PROT_R | PROT_W, 0);
    if (!p) return 3;                 /* KERN_NO_SPACE */
    *addr = (vm_address_t)p;
    return 0;
}

/* ---- sysctl: answer the handful of MIBs tools ask for ---- */
struct ds_sysctl_args { int name0, name1; };
static int ds_sysctl(const int *name, unsigned namelen, void *oldp, size_t *oldlenp,
                     void *newp, size_t newlen)
{
    (void)newp; (void)newlen;
    if (namelen >= 2 && name[0] == 6 /* CTL_HW */) {
        if (name[1] == 24 /* HW_MEMSIZE */ && oldp && oldlenp && *oldlenp >= 8) {
            unsigned long long v = 1ULL << 31;
            memcpy(oldp, &v, 8); *oldlenp = 8; return 0;
        }
        if (name[1] == 3 /* HW_NCPU */ && oldp && oldlenp && *oldlenp >= 4) {
            int v = 1; memcpy(oldp, &v, 4); *oldlenp = 4; return 0;
        }
    }
    if (namelen >= 2 && name[0] == 1 /* CTL_KERN */) {
        if (oldlenp) *oldlenp = 0;
        errno = ENOTSUP;
        return -1;
    }
    errno = ENOENT;
    return -1;
}


/* ---- dyld introspection (tools print their image list) ---- */
#include "dso.h"
static int ds_dyld_image_count(void) { return dso_count(); }
static const char *ds_dyld_get_image_name(int i) { return dso_path_at(i); }
static void ds_dyld_get_shared_cache_range(unsigned long *len) { if (len) *len = 0; }
static int ds_dyld_for_each_installed_shared_cache(void *ctx, void *cb) { (void)ctx; (void)cb; return 0; }
static int ds_dyld_shared_cache_for_each_file(void *cb) { (void)cb; return 0; }

/* ---- path + misc helpers ---- */
static char *ds_dirname_r(char *path, char *buf, size_t len)
{
    size_t n = strlen(path);
    while (n > 1 && path[n - 1] == '/') n--;
    while (n > 0 && path[n - 1] != '/') n--;
    if (n == 0) n = 1;
    if (len < n + 1) return NULL;
    memcpy(buf, path, n);
    buf[n] = 0;
    return buf;
}

static int ds_mkpath_np(const char *path, unsigned short mode)
{
    char tmp[1024];
    snprintf(tmp, sizeof tmp, "%s", path);
    for (char *p = tmp + 1; *p; p++) {
        if (*p != '/') continue;
        *p = 0;
        if (mkdir(tmp, mode) != 0 && errno != EEXIST) return errno;
        *p = '/';
    }
    if (mkdir(tmp, mode) != 0 && errno != EEXIST) return errno;
    return 0;
}

static const char *ds_simple_getenv(const char *name) { return getenv(name); }
static size_t ds_vm_page_size_v = 16384;

/* qsort_b: block comparator - the invoke pointer is the third word */
typedef int (*cmpfn_t)(const void *, const void *);
static int (*g_qcmp)(const void *, const void *);
static int qsort_b_thunk(const void *a, const void *b) { return g_qcmp(a, b); }
static void ds_qsort_b(void *base, size_t n, size_t sz, void *block)
{
    if (!block) return;
    void **words = (void **)block;
    g_qcmp = (int (*)(const void *, const void *))words[3];
    if (g_qcmp) qsort(base, n, sz, qsort_b_thunk);
}

static void ds_uuid_unparse_upper(const unsigned char *u, char *out)
{
    snprintf(out, 37, "%02X%02X%02X%02X-%02X%02X-%02X%02X-%02X%02X-%02X%02X%02X%02X%02X%02X",
             u[0],u[1],u[2],u[3],u[4],u[5],u[6],u[7],u[8],u[9],u[10],u[11],u[12],u[13],u[14],u[15]);
}

/* corecrypto entry points libtool pulls in for cdhash */
static void ds_cc_clear(size_t n, void *p) { memset(p, 0, n); }
static void ds_ccdigest_init(void *ctx, const void *di) { (void)ctx; (void)di; }
static void ds_ccdigest_update(void *ctx, size_t n, const void *p) { (void)ctx; (void)n; (void)p; }
static void *ds_ccsha256_di(void) { static char dummy[256]; return dummy; }


static void *ds_dyld_get_image_header(unsigned int idx)
{
    image_t *im = dso_image_at((int)idx);
    if (!im) return NULL;
    return (void *)(uintptr_t)(im->vmtext + im->slide);
}
static long ds_dyld_get_image_vmaddr_slide(unsigned int idx)
{
    image_t *im = dso_image_at((int)idx);
    return im ? (long)im->slide : 0;
}
static unsigned ds_mach_thread_self(void) { return 0x0403; }

/* thread-local variable bootstrap: descriptor = { thunk, key, size } */
static void *ds_tlv_bootstrap(void *desc, void **key)
{
    if (!desc) return NULL;
    uintptr_t *d = (uintptr_t *)desc;
    uintptr_t size = d[2];
    if (size == 0 || size > (1u << 20)) size = 8;
    void *p = calloc(1, (size_t)size);
    if (key) *key = p;
    return p;
}


/* ---- Darwin <dirent.h> ----
 * Darwin's struct dirent is 1048 bytes: d_ino@0, d_seekoff@8, d_reclen@16,
 * d_namlen@18, d_type@20, d_name@21[1024].  Linux musl's has no d_seekoff,
 * no d_namlen and a 256-byte name, so a passthrough hands the guest a struct
 * with the wrong name offset.  We hand back our own translated buffer. */
#include <dirent.h>
#include <stddef.h>

struct ds_dirent {
    uint64_t d_ino;
    uint64_t d_seekoff;
    uint16_t d_reclen;
    uint16_t d_namlen;
    uint8_t  d_type;
    char     d_name[1024];
};

#define DS_MAXDIR 32
static struct { DIR *host; struct ds_dirent de; } g_dirs[DS_MAXDIR];
static int g_ndirs;

_Static_assert(sizeof(struct ds_dirent) == 1048, "Darwin dirent is 1048 bytes");
_Static_assert(offsetof(struct ds_dirent, d_seekoff) == 8, "d_seekoff@8");
_Static_assert(offsetof(struct ds_dirent, d_namlen) == 18, "d_namlen@18");
_Static_assert(offsetof(struct ds_dirent, d_name) == 21, "d_name@21");

static DIR *ds_opendir(const char *path)
{
    if (g_ndirs >= DS_MAXDIR) { errno = EMFILE; return NULL; }
    DIR *h = opendir(path);
    if (!h) return NULL;
    g_dirs[g_ndirs].host = h;
    return (DIR *)&g_dirs[g_ndirs++];
}

static int dir_slot(DIR *d)
{
    for (int i = 0; i < g_ndirs; i++)
        if ((DIR *)&g_dirs[i] == d) return i;
    return -1;
}

static struct ds_dirent *ds_readdir(DIR *d)
{
    int i = dir_slot(d);
    if (i < 0) { errno = EBADF; return NULL; }
    struct dirent *e = readdir(g_dirs[i].host);
    if (!e) return NULL;
    struct ds_dirent *o = &g_dirs[i].de;
    memset(o, 0, sizeof *o);
    o->d_ino = (uint64_t)e->d_ino;
    o->d_seekoff = 0;
    o->d_type = e->d_type;
    size_t n = strlen(e->d_name);
    if (n > sizeof o->d_name - 1) n = sizeof o->d_name - 1;
    memcpy(o->d_name, e->d_name, n);
    o->d_namlen = (uint16_t)n;
    o->d_reclen = (uint16_t)(21 + n + 1);
    return o;
}

static int ds_closedir(DIR *d)
{
    int i = dir_slot(d);
    if (i < 0) { errno = EBADF; return -1; }
    int r = closedir(g_dirs[i].host);
    g_dirs[i] = g_dirs[--g_ndirs];
    return r;
}

static void ds_rewinddir(DIR *d) { int i = dir_slot(d); if (i >= 0) rewinddir(g_dirs[i].host); }
static void ds_seekdir(DIR *d, long off) { int i = dir_slot(d); if (i >= 0) seekdir(g_dirs[i].host, off); }
static long ds_telldir(DIR *d) { int i = dir_slot(d); return i >= 0 ? telldir(g_dirs[i].host) : -1; }

/* ---- calls that must fail loudly rather than lie ---- */
static int ds_fsgetpath(char *buf, size_t len, void *fsid, uint64_t objid)
{ (void)buf; (void)len; (void)fsid; (void)objid; errno = ENOSYS; return -1; }
static int ds_proc_pidinfo(int pid, int flavor, uint64_t arg, void *buf, int bufsize)
{ (void)pid; (void)flavor; (void)arg; (void)buf; (void)bufsize; errno = ENOSYS; return -1; }
static void ds_terminate_with_payload(int pid, int ns, uint64_t code, void *p, int psz,
                                      const char *reason, uint64_t flags)
{ (void)pid; (void)ns; (void)code; (void)p; (void)psz; (void)reason; (void)flags; errno = ENOSYS; }

/* ---- compile-time proof of the struct ABI we claim to implement ---- */
_Static_assert(sizeof(struct ds_stat) == 144, "Darwin struct stat is 144 bytes");
_Static_assert(offsetof(struct ds_stat, st_mode) == 4, "st_mode@4");
_Static_assert(offsetof(struct ds_stat, st_ino) == 8, "st_ino@8");
_Static_assert(offsetof(struct ds_stat, st_mtimespec) == 48, "st_mtimespec@48");
_Static_assert(offsetof(struct ds_stat, st_size) == 96, "st_size@96");
_Static_assert(offsetof(struct ds_stat, st_blksize) == 112, "st_blksize@112");
_Static_assert(sizeof(struct ds_sigaction) == 16, "Darwin sigaction is 16 bytes");
_Static_assert(sizeof(struct ds_stack_t) == 24, "Darwin stack_t is 24 bytes");


/* ---- Darwin <sys/mount.h> statfs ----
 * 2168 bytes, and the field order is nothing like Linux's statvfs.  Tools
 * (gcov, llvm-cov, objdump, c++filt) call it on the path they are working on,
 * so a passthrough writes 120 bytes into a 2168-byte buffer and leaves the
 * rest as whatever was on the guest's stack. */
#include <sys/statvfs.h>

struct ds_statfs {
    uint32_t f_bsize;
    int32_t  f_iosize;
    uint64_t f_blocks, f_bfree, f_bavail, f_files, f_ffree;
    int32_t  f_fsid[2];
    uint32_t f_owner;
    uint32_t f_type;
    uint32_t f_flags;
    uint32_t f_fssubtype;
    char     f_fstypename[16];
    char     f_mntonname[1024];
    char     f_mntfromname[1024];
    uint32_t f_flags_ext;
    uint32_t f_reserved[7];
};

_Static_assert(sizeof(struct ds_statfs) == 2168, "Darwin struct statfs is 2168 bytes");
_Static_assert(offsetof(struct ds_statfs, f_blocks) == 8, "f_blocks@8");
_Static_assert(offsetof(struct ds_statfs, f_fstypename) == 72, "f_fstypename@72");
_Static_assert(offsetof(struct ds_statfs, f_mntfromname) == 1112, "f_mntfromname@1112");

static void fill_statfs(struct ds_statfs *o, const struct statvfs *v, const char *path)
{
    memset(o, 0, sizeof *o);
    o->f_bsize = (uint32_t)(v->f_frsize ? v->f_frsize : v->f_bsize);
    o->f_iosize = (int32_t)v->f_bsize;
    o->f_blocks = v->f_blocks;
    o->f_bfree = v->f_bfree;
    o->f_bavail = v->f_bavail;
    o->f_files = v->f_files;
    o->f_ffree = v->f_ffree;
    o->f_fsid[0] = (int32_t)v->f_fsid;
    o->f_owner = (uint32_t)getuid();
    strlcpy(o->f_fstypename, "apfs", sizeof o->f_fstypename);
    snprintf(o->f_mntonname, sizeof o->f_mntonname, "%s", path ? path : "/");
    strlcpy(o->f_mntfromname, "/dev/disk1s1", sizeof o->f_mntfromname);
}

static int ds_statfs(const char *path, struct ds_statfs *out)
{
    struct statvfs v;
    if (!out) { errno = EFAULT; return -1; }
    if (statvfs(path, &v) != 0) return -1;
    fill_statfs(out, &v, path);
    return 0;
}

static int ds_fstatfs(int fd, struct ds_statfs *out)
{
    struct statvfs v;
    if (!out) { errno = EFAULT; return -1; }
    if (fstatvfs(fd, &v) != 0) return -1;
    fill_statfs(out, &v, "/");
    return 0;
}


/* ---- dyld image-notification callbacks -------------------------------
 * This is how the Swift runtime learns about images: it registers a callback
 * and walks each image's __swift5_* sections to register type metadata,
 * protocol conformances and field descriptors.  dyld invokes the callback for
 * images already loaded at registration time, then for each new image. */
static void (*g_add_image_cb)(const void *mh, intptr_t slide);
static void (*g_remove_image_cb)(const void *mh, intptr_t slide);

static void ds_dyld_register_func_for_add_image(void (*cb)(const void *, intptr_t))
{
    g_add_image_cb = cb;
    if (!cb) return;
    for (int i = 0; i < dso_count(); i++) {
        image_t *im = dso_image_at(i);
        if (!im) continue;
        cb((const void *)(uintptr_t)(im->vmtext + im->slide), (intptr_t)im->slide);
    }
}
static void ds_dyld_register_func_for_remove_image(void (*cb)(const void *, intptr_t))
{
    g_remove_image_cb = cb;
}
void shim_notify_image_added(void *p)
{
    image_t *im = (image_t *)p;
    if (g_add_image_cb)
        g_add_image_cb((const void *)(uintptr_t)(im->vmtext + im->slide), (intptr_t)im->slide);
}

/* ---- table ---- */
/* Darwin exports these as *variables* holding the FILE*, not as functions:
 *   FILE *__stdoutp;   guest code does  ldr x0,[x0]
 * Binding them to a function makes the guest read machine code as a FILE*. */
FILE *shim_stdin_p, *shim_stdout_p, *shim_stderr_p;
__attribute__((constructor)) static void shim_init_streams(void)
{
    shim_stdin_p = stdin; shim_stdout_p = stdout; shim_stderr_p = stderr;
}

typedef struct { const char *name; void *fn; } ent_t;
#define E(x) { #x, (void *)x }
static const ent_t table[] = {
    E(dyld_stub_binder),
    { "__error", (void *)__error },
    { "_NSGetArgc", (void *)_NSGetArgc },
    { "_NSGetArgv", (void *)_NSGetArgv },
    { "_NSGetEnviron", (void *)_NSGetEnviron },
    { "_NSGetExecutablePath", (void *)_NSGetExecutablePath },
    E(mach_absolute_time), E(mach_continuous_time), E(mach_timebase_info),
    E(clock_gettime_nsec_np),
    E(malloc_default_zone), E(malloc_zone_malloc), E(malloc_zone_calloc),
    E(malloc_zone_realloc), E(malloc_zone_free), E(malloc_size), E(malloc_good_size),
    { "_os_log_impl", (void *)os_log_impl },
    { "os_log", (void *)os_log_impl },
    { "os_log_error", (void *)os_log_impl },
    { "os_log_fault", (void *)os_log_impl },
    { "os_log_info", (void *)os_log_impl },
    { "os_log_debug", (void *)os_log_impl },
    E(os_log_create), E(os_log_type_enabled),
    E(sysctlbyname), E(getprogname), E(setprogname),
    { "strlcpy", (void *)shim_strlcpy },
    { "strlcat", (void *)shim_strlcat },
    { "_platform_memcpy", (void *)p_memcpy },
    { "_platform_memmove", (void *)p_memmove },
    { "_platform_memset", (void *)p_memset },
    { "_platform_memcmp", (void *)p_memcmp },
    { "_platform_strlen", (void *)p_strlen },
    { "pthread_setname_np", (void *)shim_pthread_setname_np },
    E(pthread_threadid_np),
    { "__cxa_atexit", (void *)__cxa_atexit },
    { "__cxa_finalize", (void *)__cxa_finalize },
    { "__assert_rtn", (void *)__assert_rtn },
    { "__sprintf_chk", (void *)ds___sprintf_chk_stub },
    { "__snprintf_chk", (void *)ds___snprintf_chk_stub },
    { "__vsnprintf_chk", (void *)ds_vsnprintf_chk },
    { "__strcpy_chk", (void *)__strcpy_chk },
    { "__strcat_chk", (void *)__strcat_chk },
    { "__memcpy_chk", (void *)__memcpy_chk },
    { "__memset_chk", (void *)__memset_chk },
    { "__chkstk_darwin", (void *)__chkstk_darwin },
    { "reallocf", (void *)shim_reallocf },
    { "mach_error_string", (void *)mach_error_string },
    { "strmode", (void *)shim_strmode },
    { "__stdinp", (void *)&shim_stdin_p },
    { "__stdoutp", (void *)&shim_stdout_p },
    { "__stderrp", (void *)&shim_stderr_p },
    { "stat", (void *)ds_stat },
    { "lstat", (void *)ds_lstat },
    { "fstat", (void *)ds_fstat },
    { "fstatat", (void *)ds_fstatat },
    { "sigprocmask", (void *)ds_sigprocmask },
    { "sigaction", (void *)ds_sigaction },
    { "sigaltstack", (void *)ds_sigaltstack },
    { "setjmp", (void *)ds_setjmp },
    { "_setjmp", (void *)ds_setjmp },
    { "longjmp", (void *)ds_longjmp },
    { "_longjmp", (void *)ds_longjmp },
    { "sigsetjmp", (void *)ds_sigsetjmp },
    { "siglongjmp", (void *)ds_siglongjmp },
    { "statfs", (void *)ds_statfs },
    { "fstatfs", (void *)ds_fstatfs },
    { "opendir", (void *)ds_opendir },
    { "readdir", (void *)ds_readdir },
    { "closedir", (void *)ds_closedir },
    { "rewinddir", (void *)ds_rewinddir },
    { "seekdir", (void *)ds_seekdir },
    { "telldir", (void *)ds_telldir },
    { "fsgetpath", (void *)ds_fsgetpath },
    { "proc_pidinfo", (void *)ds_proc_pidinfo },
    { "terminate_with_payload", (void *)ds_terminate_with_payload },
    { "printf", (void *)ds_printf_stub },
    { "fprintf", (void *)ds_fprintf_stub },
    { "sprintf", (void *)ds_sprintf_stub },
    { "snprintf", (void *)ds_snprintf_stub },
    { "asprintf", (void *)ds_asprintf_stub },
    { "dprintf", (void *)ds_dprintf_stub },
    { "err", (void *)ds_err_stub },
    { "warn", (void *)ds_warn_stub },
    { "warnx", (void *)ds_warnx_stub },
    { "vfprintf", (void *)ds_vfprintf },
    { "vsprintf", (void *)ds_vsprintf },
    { "vsnprintf", (void *)ds_vsnprintf },
    { "vasprintf", (void *)ds_vasprintf },
    { "execvp", (void *)ds_execvp },
    { "_Block_object_assign", (void *)__Block_object_assign },
    { "_Block_object_dispose", (void *)__Block_object_dispose },
    { "_NSConcreteGlobalBlock", (void *)__NSConcreteGlobalBlock },
    { "_NSConcreteStackBlock", (void *)__NSConcreteStackBlock },
    { "_maskrune", (void *)__maskrune },
    { "_DefaultRuneLocale", (void *)_DefaultRuneLocale },
    { "__maskrune", (void *)__maskrune },
    { "copyfile", (void *)ds_copyfile },
    { "copyfile_state_alloc", (void *)copyfile_state_alloc },
    { "copyfile_state_free", (void *)copyfile_state_free },
    { "__strlcpy_chk", (void *)__strlcpy_chk },
    { "__strlcat_chk", (void *)__strlcat_chk },
    { "__toupper", (void *)__toupper },
    { "__tolower", (void *)__tolower },
    { "strtonum", (void *)ds_strtonum },
    { "backtrace", (void *)ds_backtrace },
    { "__darwin_check_fd_set_overflow", (void *)darwin_check_fd_set_overflow },
    { "__udivti3", (void *)ds_udivti3 },
    { "__divti3", (void *)ds_divti3 },
    { "__umodti3", (void *)ds_umodti3 },
    { "__modti3", (void *)ds_modti3 },
    { "__tlv_atexit", (void *)ds_tlv_atexit },
    { "clonefile", (void *)ds_clonefile },
    { "clonefileat", (void *)ds_clonefileat },
    { "fsctl", (void *)ds_fsctl },
    { "mach_task_self_", (void *)&ds_mach_task_self },
    { "mach_host_self", (void *)ds_mach_host_self },
    { "task_set_exception_ports", (void *)ds_task_set_exception_ports },
    { "host_info", (void *)ds_host_info },
    { "mach_port_deallocate", (void *)ds_mach_port_deallocate },
    { "vm_deallocate", (void *)ds_vm_deallocate },
    { "vm_allocate", (void *)ds_vm_allocate },
    { "sysctl", (void *)ds_sysctl },
    { "dyld_image_count", (void *)ds_dyld_image_count },
    { "dyld_get_image_name", (void *)ds_dyld_get_image_name },
    { "dyld_get_shared_cache_range", (void *)ds_dyld_get_shared_cache_range },
    { "dyld_for_each_installed_shared_cache", (void *)ds_dyld_for_each_installed_shared_cache },
    { "dyld_shared_cache_for_each_file", (void *)ds_dyld_shared_cache_for_each_file },
    { "dirname_r", (void *)ds_dirname_r },
    { "mkpath_np", (void *)ds_mkpath_np },
    { "_simple_getenv", (void *)ds_simple_getenv },
    { "vm_page_size", (void *)&ds_vm_page_size_v },
    { "qsort_b", (void *)ds_qsort_b },
    { "uuid_unparse_upper", (void *)ds_uuid_unparse_upper },
    { "cc_clear", (void *)ds_cc_clear },
    { "ccdigest_init", (void *)ds_ccdigest_init },
    { "ccdigest_update", (void *)ds_ccdigest_update },
    { "ccsha256_di", (void *)ds_ccsha256_di },
    { "dyld_register_func_for_add_image", (void *)ds_dyld_register_func_for_add_image },
    /* libdyld exports both spellings; Apple's own tools import the underscored
     * private ones, so both must resolve to the same implementation. */
    { "_dyld_image_count", (void *)ds_dyld_image_count },
    { "_dyld_get_image_name", (void *)ds_dyld_get_image_name },
    { "_dyld_get_image_header", (void *)ds_dyld_get_image_header },
    { "_dyld_get_image_vmaddr_slide", (void *)ds_dyld_get_image_vmaddr_slide },
    { "_dyld_get_shared_cache_range", (void *)ds_dyld_get_shared_cache_range },
    { "_dyld_for_each_installed_shared_cache", (void *)ds_dyld_for_each_installed_shared_cache },
    { "_dyld_shared_cache_for_each_file", (void *)ds_dyld_shared_cache_for_each_file },
    { "_dyld_register_func_for_add_image", (void *)ds_dyld_register_func_for_add_image },
    { "_dyld_register_func_for_remove_image", (void *)ds_dyld_register_func_for_remove_image },
    { "dyld_register_func_for_remove_image", (void *)ds_dyld_register_func_for_remove_image },
    { "dyld_get_image_header", (void *)ds_dyld_get_image_header },
    { "dyld_get_image_vmaddr_slide", (void *)ds_dyld_get_image_vmaddr_slide },
    { "mach_thread_self", (void *)ds_mach_thread_self },
    { "_tlv_bootstrap", (void *)ds_tlv_bootstrap },
    { "xcselect_get_developer_dir_path", (void *)ds_xcselect_get_developer_dir_path },
    { "xcselect_invoke_xcrun", (void *)ds_xcselect_invoke_xcrun },
    { "__srget", (void *)ds___srget },
    { "__swbuf", (void *)ds___swbuf },
    { "__mb_cur_max", (void *)&ds___mb_cur_max },
    { "memset_pattern16", (void *)ds_memset_pattern16 },
    { "arc4random", (void *)ds_arc4random },
    { "arc4random_buf", (void *)ds_arc4random_buf },
    { "open", (void *)ds_open },
    { "openat", (void *)ds_openat },
};

void *dshim_lookup(const char *macho_name)
{
    const char *n = macho_name;
    if (*n == '_') n++;
    /* Darwin mangles availability variants as name$FEATURE */
    char clean[256];
    const char *dollar = strchr(n, '$');
    if (dollar) {
        size_t l = (size_t)(dollar - n);
        if (l >= sizeof clean) l = sizeof clean - 1;
        memcpy(clean, n, l); clean[l] = 0; n = clean;
    }
    /* exact shim entries take precedence over host symbols */
    for (size_t i = 0; i < sizeof table / sizeof *table; i++)
        if (!strcmp(table[i].name, n)) return table[i].fn;
    void *a = plat_sym(n);
    if (a) return a;
    /* Darwin C++ uses std::__1, which is also what libc++ uses everywhere:
       a mangled name only differs by the leading underscore.  plat_sym above
       already handled the common case. */
    return NULL;
}
