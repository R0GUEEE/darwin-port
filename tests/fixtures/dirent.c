typedef unsigned long size_t;
typedef struct __dirstream DIR;
struct dirent { unsigned long long d_ino, d_seekoff; unsigned short d_reclen, d_namlen;
                unsigned char d_type; char d_name[1024]; };
extern DIR *opendir(const char *);
extern struct dirent *readdir(DIR *);
extern int closedir(DIR *);
extern int printf(const char *, ...);
static unsigned long len_of(const char *s) { unsigned long n = 0; while (s[n]) n++; return n; }
int main(void) {
    DIR *d = opendir("/tmp");
    if (!d) return 1;
    struct dirent *e; int seen = 0;
    while ((e = readdir(d))) {
        if (e->d_namlen != (unsigned short)len_of(e->d_name)) return 2;
        if (++seen >= 50) break;
    }
    closedir(d);
    printf("dirent ok %d\n", seen > 0);
    return seen > 0 ? 0 : 3;
}
