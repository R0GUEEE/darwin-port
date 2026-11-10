/* plat_win32.c - Windows backend for darwinld.
 *
 * Runs the same Mach-O images as the POSIX backend on Windows on ARM64.
 * On x86_64 Windows the loader still maps and binds correctly (so `check`
 * works), but arm64 instructions cannot execute natively: pair it with an
 * arm64 interpreter/JIT to actually run code.
 *
 * Build (MSVC):    cl /O2 /Fe:darwinld.exe main.c macho.c shim.c plat_win32.c
 * Build (mingw):   x86_64-w64-mingw32-gcc -O2 -o darwinld.exe *.c
 */
#ifdef _WIN32
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "plat.h"

static DWORD page_of(int prot)
{
    int x = prot & PROT_X, w = prot & PROT_W;
    if (x && w) return PAGE_EXECUTE_READWRITE;
    if (x)      return PAGE_EXECUTE_READ;
    if (w)      return PAGE_READWRITE;
    return PAGE_READONLY;
}

void *plat_map(void *addr, size_t len, int prot, int fixed)
{
    DWORD type = MEM_RESERVE | MEM_COMMIT;
    DWORD fl = page_of(prot);
    if (fixed) {
        void *r = VirtualAlloc(addr, len, type, fl);
        if (r) return r;
        if (fixed == 2) return NULL;
        /* exact placement refused: fall through to anywhere */
    }
    return VirtualAlloc(NULL, len, type, fl);
}

int plat_unmap(void *a, size_t l) { (void)l; return VirtualFree(a, 0, MEM_RELEASE) ? 0 : -1; }

int plat_protect(void *a, size_t l, int p)
{
    DWORD old;
    return VirtualProtect(a, l, page_of(p), &old) ? 0 : -1;
}

/* Darwin name -> CRT name, for the calls whose ELF/POSIX spelling Windows
 * does not export.  Anything else is tried verbatim, then with a single
 * leading underscore, which is how the UCRT exports its POSIX layer. */
static const struct { const char *darwin, *win; } alias[] = {
    { "open", "_open" }, { "read", "_read" }, { "write", "_write" },
    { "close", "_close" }, { "lseek", "_lseeki64" }, { "unlink", "_unlink" },
    { "access", "_access" }, { "getpid", "_getpid" }, { "isatty", "_isatty" },
    { "dup", "_dup" }, { "dup2", "_dup2" }, { "mkdir", "_mkdir" },
    { "stat", "_stat64" }, { "fstat", "_fstat64" }, { "strdup", "_strdup" },
    { "popen", "_popen" }, { "pclose", "_pclose" }, { "snprintf", "_snprintf" },
    { "vsnprintf", "_vsnprintf" }, { "fileno", "_fileno" }, { "fdopen", "_fdopen" },
    { "getcwd", "_getcwd" }, { "chdir", "_chdir" }, { "rmdir", "_rmdir" },
    { "time", "_time" }, { "setmode", "_setmode" }, { "fstatat", "_fstatat64" },
};

static HMODULE mods[] = { NULL, NULL, NULL };

static HMODULE* mod_list(void)
{
    if (!mods[0]) {
        mods[0] = GetModuleHandleA("ucrtbase.dll");
        mods[1] = GetModuleHandleA("msvcrt.dll");
        mods[2] = GetModuleHandleA("kernel32.dll");
        if (!mods[0]) mods[0] = GetModuleHandleA(NULL);
    }
    return mods;
}

void *plat_sym(const char *name)
{
    HMODULE *m = mod_list();
    for (int i = 0; i < 3; i++) {
        if (!m[i]) continue;
        void *p = (void *)GetProcAddress(m[i], name);
        if (p) return p;
    }
    for (size_t i = 0; i < sizeof alias / sizeof *alias; i++)
        if (!strcmp(alias[i].darwin, name)) {
            for (int k = 0; k < 3; k++)
                if (m[k]) {
                    void *p = (void *)GetProcAddress(m[k], alias[i].win);
                    if (p) return p;
                }
        }
    char buf[256];
    _snprintf_s(buf, sizeof buf, _TRUNCATE, "_%s", name);
    for (int k = 0; k < 3; k++)
        if (m[k]) {
            void *p = (void *)GetProcAddress(m[k], buf);
            if (p) return p;
        }
    return NULL;
}

int plat_read_file(const char *path, void **buf, size_t *len)
{
    HANDLE h = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return -1;
    LARGE_INTEGER sz;
    if (!GetFileSizeEx(h, &sz)) { CloseHandle(h); return -1; }
    void *b = malloc((size_t)sz.QuadPart);
    DWORD got = 0;
    if (!ReadFile(h, b, (DWORD)sz.QuadPart, &got, NULL)) { CloseHandle(h); free(b); return -1; }
    CloseHandle(h);
    *buf = b; *len = got;
    return 0;
}

const char *plat_name(void)
{
#if defined(_M_ARM64) || defined(__aarch64__)
    return "windows-arm64";
#else
    return "windows-x64 (guest needs an arm64 JIT)";
#endif
}

#endif /* _WIN32 */
