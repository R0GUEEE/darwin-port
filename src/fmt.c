/* fmt.c - printf family for Darwin arm64 variadic calls.
 *
 * Apple's arm64 ABI differs from AAPCS64 in one fatal way: anonymous
 * (variadic) arguments are passed on the stack, and va_list is just a pointer
 * to them.  A guest calling printf("%d", 42) leaves 42 at [sp], not in w4.
 *
 * So the guest's varargs are a flat cursor of 8-byte slots, and every
 * variadic libSystem entry point needs its own front end that turns sp into
 * that cursor.  The assembly stubs live in fmt_asm.S; this file does the
 * formatting.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdarg.h>
#include <unistd.h>

/* ---- argument cursor ---- */
typedef struct { const uint8_t *p; } dcur;
static uint64_t arg_u64(dcur *c) { uint64_t v; memcpy(&v, c->p, 8); c->p += 8; return v; }
static int64_t  arg_i64(dcur *c) { return (int64_t)arg_u64(c); }
static int      arg_int(dcur *c) { return (int)(int32_t)arg_u64(c); }
static void    *arg_ptr(dcur *c) { return (void *)(uintptr_t)arg_u64(c); }
static double   arg_dbl(dcur *c) { double v; memcpy(&v, c->p, 8); c->p += 8; return v; }

/* ---- sink ---- */
typedef struct {
    char  *buf;
    size_t cap;      /* capacity, including NUL */
    size_t len;
    FILE  *fp;       /* when buf == NULL */
} sink;

static void emit(sink *s, const char *p, size_t n)
{
    if (s->buf) {
        /* cap == SIZE_MAX means "sprintf": no bound, caller guarantees room */
        size_t room = (s->cap == (size_t)-1) ? n
                    : (s->cap ? s->cap - 1 - s->len : 0);
        if (n > room) n = room;
        memcpy(s->buf + s->len, p, n);
        s->len += n;
    } else if (s->fp) {
        fwrite(p, 1, n, s->fp);
        s->len += n;
    }
}

static void emit_pad(sink *s, char c, int n)
{
    char tmp[64];
    memset(tmp, c, sizeof tmp);
    while (n > 0) { int k = n > (int)sizeof tmp ? (int)sizeof tmp : n; emit(s, tmp, (size_t)k); n -= k; }
}

static void emit_str(sink *s, const char *str, int width, int prec, int left)
{
    size_t n = str ? strlen(str) : 6;
    if (!str) str = "(null)";
    if (prec >= 0 && (size_t)prec < n) n = (size_t)prec;
    int pad = width - (int)n;
    if (!left && pad > 0) emit_pad(s, ' ', pad);
    emit(s, str, n);
    if (left && pad > 0) emit_pad(s, ' ', pad);
}

static void emit_num(sink *s, uint64_t v, int base, int neg, int upper, int prec,
                     int width, int left, int zero, int alt, char conv)
{
    char tmp[72];
    const char *digs = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    int n = 0;
    if (v == 0 && prec != 0) tmp[n++] = '0';
    while (v) { tmp[n++] = digs[v % base]; v /= base; }
    while (prec > n) tmp[n++] = '0';
    char prefix[3]; int pl = 0;
    if (neg) prefix[pl++] = '-';
    else if (alt && base == 16) { prefix[pl++] = '0'; prefix[pl++] = upper ? 'X' : 'x'; }
    else if (alt && base == 8 && tmp[n - 1] != '0') { prefix[pl++] = '0'; }
    int pad = width - (n + pl);
    if (!left && pad > 0 && !zero) emit_pad(s, ' ', pad);
    emit(s, prefix, (size_t)pl);
    if (!left && pad > 0 && zero) emit_pad(s, '0', pad);
    for (int i = n - 1; i >= 0; i--) emit(s, &tmp[i], 1);
    if (left && pad > 0) emit_pad(s, ' ', pad);
    (void)conv;
}

static void emit_float(sink *s, double d, int prec, int width, int left, int zero, char conv)
{
    char tmp[64];
    if (prec < 0) prec = 6;
    int n = snprintf(tmp, sizeof tmp, "%.*f", prec, d);
    if (conv == 'e' || conv == 'E') n = snprintf(tmp, sizeof tmp, "%.*e", prec, d);
    if (conv == 'g' || conv == 'G') n = snprintf(tmp, sizeof tmp, "%.*g", prec ? prec : 6, d);
    if (n < 0) return;
    if (conv == 'E' || conv == 'G') for (int i = 0; i < n; i++) tmp[i] = (char)(tmp[i] == 'e' ? 'E' : tmp[i]);
    int pad = width - n;
    if (!left && pad > 0 && !zero) emit_pad(s, ' ', pad);
    if (!left && pad > 0 && zero) emit_pad(s, '0', pad);
    emit(s, tmp, (size_t)n);
    if (left && pad > 0) emit_pad(s, ' ', pad);
}

/* returns number of bytes that would have been written */
static size_t format(sink *s, const char *fmt, const void *cursor)
{
    dcur c = { (const uint8_t *)cursor };
    const char *p = fmt;
    size_t start = s->len;

    while (*p) {
        if (*p != '%') { const char *q = p; while (*q && *q != '%') q++; emit(s, p, (size_t)(q - p)); p = q; continue; }
        p++;
        int left = 0, zero = 0, alt = 0, plus = 0, space = 0;
        for (;;) {
            if (*p == '-') { left = 1; p++; }
            else if (*p == '0') { zero = 1; p++; }
            else if (*p == '#') { alt = 1; p++; }
            else if (*p == '+') { plus = 1; p++; }
            else if (*p == ' ') { space = 1; p++; }
            else break;
        }
        int width = 0, prec = -1;
        if (*p == '*') { width = arg_int(&c); p++; if (width < 0) { left = 1; width = -width; } }
        else while (*p >= '0' && *p <= '9') width = width * 10 + (*p++ - '0');
        if (*p == '.') {
            p++;
            if (*p == '*') { prec = arg_int(&c); p++; }
            else { prec = 0; while (*p >= '0' && *p <= '9') prec = prec * 10 + (*p++ - '0'); }
        }
        /* length modifiers */
        int lng = 0, half = 0, sizem = 0, ptrdiff = 0, ldouble = 0;
        for (;;) {
            if (*p == 'l') { lng++; p++; }
            else if (*p == 'q' || *p == 'L') { lng = 2; if (*p == 'L') ldouble = 1; p++; }
            else if (*p == 'h') { half++; p++; }
            else if (*p == 'z') { sizem = 1; p++; }
            else if (*p == 't') { ptrdiff = 1; p++; }
            else if (*p == 'j') { lng = 2; p++; }
            else break;
        }
        char conv = *p ? *p++ : 0;
        switch (conv) {
        case 'd': case 'i': {
            int64_t v;
            if (lng) v = arg_i64(&c);
            else if (half || sizem || ptrdiff) v = (int64_t)(int32_t)arg_u64(&c);
            else v = arg_int(&c);
            int neg = v < 0;
            uint64_t u = neg ? (uint64_t)(-v) : (uint64_t)v;
            if (!neg && plus) { emit(s, "+", 1); if (width > 0) width--; }
            else if (!neg && space) { emit(s, " ", 1); if (width > 0) width--; }
            emit_num(s, u, 10, neg, 0, prec, width, left, zero, 0, conv);
            break; }
        case 'u': case 'o': case 'x': case 'X': {
            uint64_t v = lng ? arg_u64(&c) : (uint64_t)(uint32_t)arg_u64(&c);
            int base = conv == 'u' ? 10 : (conv == 'o' ? 8 : 16);
            emit_num(s, v, base, 0, conv == 'X', prec, width, left, zero, alt, conv);
            break; }
        case 'c': {
            char ch = (char)arg_int(&c);
            int pad = width - 1;
            if (!left && pad > 0) emit_pad(s, ' ', pad);
            emit(s, &ch, 1);
            if (left && pad > 0) emit_pad(s, ' ', pad);
            break; }
        case 's': {
            const char *str = arg_ptr(&c);
            emit_str(s, str, width, prec, left);
            break; }
        case 'p': {
            uint64_t v = arg_u64(&c);
            if (!v) { emit_str(s, "0x0", width, -1, left); break; }
            char tmp[32]; int n = snprintf(tmp, sizeof tmp, "0x%llx", (unsigned long long)v);
            emit_str(s, tmp, width, -1, left);
            (void)n;
            break; }
        case 'f': case 'F': case 'e': case 'E': case 'g': case 'G': {
            double d = arg_dbl(&c);
            emit_float(s, d, prec, width, left, zero, conv);
            break; }
        case '%': emit(s, "%", 1); break;
        case 'n': arg_u64(&c); break;
        default:
            if (conv) { emit(s, "%", 1); emit(s, &conv, 1); }
            break;
        }
    }
    return s->len - start;
}

/* ---- entry points called from the assembly stubs ---- */
int ds_printf_c(const char *fmt, const void *cur)
{
    sink s = { NULL, 0, 0, stdout };
    int r = (int)format(&s, fmt, cur);
    fflush(stdout);
    return r;
}

int ds_fprintf_c(FILE *fp, const char *fmt, const void *cur)
{
    sink s = { NULL, 0, 0, fp ? fp : stdout };
    return (int)format(&s, fmt, cur);
}

int ds_sprintf_c(char *dst, const char *fmt, const void *cur)
{
    sink s = { dst, (size_t)-1, 0, NULL };
    size_t r = format(&s, fmt, cur);
    dst[s.len] = 0;
    return (int)r;
}

int ds_snprintf_c(char *dst, size_t n, const char *fmt, const void *cur)
{
    sink s = { dst, n, 0, NULL };
    size_t r = format(&s, fmt, cur);
    if (n) dst[s.len] = 0;
    return (int)r;
}

int ds_sprintf_chk_c(char *dst, int flag, size_t slen, const char *fmt, const void *cur)
{
    (void)flag; (void)slen;
    int r = ds_sprintf_c(dst, fmt, cur);
    if (getenv("DS_TRACE_FMT"))
        fprintf(stderr, "[fmt] sprintf_chk dst=%p fmt=\"%s\" -> \"%s\"\n", (void *)dst, fmt, dst);
    return r;
}

int ds_snprintf_chk_c(char *dst, size_t n, int flag, size_t slen, const char *fmt, const void *cur)
{
    (void)flag; (void)slen;
    return ds_snprintf_c(dst, n, fmt, cur);
}

int ds_asprintf_c(char **out, const char *fmt, const void *cur)
{
    sink s = { NULL, 0, 0, NULL };
    size_t need = format(&s, fmt, cur);          /* measure */
    char *b = malloc(need + 1);
    if (!b) return -1;
    sink t = { b, need + 1, 0, NULL };
    format(&t, fmt, cur);
    b[t.len] = 0;
    *out = b;
    return (int)t.len;
}

/* vfprintf: Darwin's va_list is already a pointer to the stack arguments */
int ds_vfprintf(FILE *fp, const char *fmt, void *ap) { return ds_fprintf_c(fp, fmt, ap); }
int ds_vsprintf(char *dst, const char *fmt, void *ap) { return ds_sprintf_c(dst, fmt, ap); }
int ds_vsnprintf(char *dst, size_t n, const char *fmt, void *ap) { return ds_snprintf_c(dst, n, fmt, ap); }
int ds_vasprintf(char **out, const char *fmt, void *ap) { return ds_asprintf_c(out, fmt, ap); }
int ds_vsnprintf_chk(char *dst, size_t n, int flag, size_t slen, const char *fmt, void *ap)
{ (void)flag; (void)slen; return ds_snprintf_c(dst, n, fmt, ap); }

/* err/warn: same stack-varargs problem, so they get their own front ends */
void ds_err_c(int eval, const char *fmt, const void *cur, const char *prog)
{
    if (prog && *prog) { fputs(prog, stderr); fputs(": ", stderr); }
    if (fmt) { sink s = { NULL, 0, 0, stderr }; format(&s, fmt, cur); }
    fputc('\n', stderr);
    exit(eval);
}
void ds_warn_c(const char *fmt, const void *cur, const char *prog)
{
    if (prog && *prog) { fputs(prog, stderr); fputs(": ", stderr); }
    if (fmt) { sink s = { NULL, 0, 0, stderr }; format(&s, fmt, cur); }
    fputc('\n', stderr);
}

void ds_dprintf_c(int fd, const char *fmt, const void *cur)
{
    sink s = { NULL, 0, 0, NULL };
    size_t need = format(&s, fmt, cur);
    char *b = malloc(need + 1);
    if (!b) return;
    sink t = { b, need + 1, 0, NULL };
    format(&t, fmt, cur);
    b[t.len] = 0;
    if (write(fd, b, t.len) < 0) { /* ignore */ }
    free(b);
}

/* err/warn need the program name; fetch it in C so the asm stub needs no
 * dynamic relocation for a data symbol. */
extern const char *ds_progname;
void ds_err_cur(int eval, const char *fmt, const void *cur) { ds_err_c(eval, fmt, cur, ds_progname); }
void ds_warn_cur(const char *fmt, const void *cur) { ds_warn_c(fmt, cur, ds_progname); }
