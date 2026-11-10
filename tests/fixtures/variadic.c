typedef unsigned long size_t;
extern int snprintf(char *, size_t, const char *, ...);
extern int puts(const char *);
int main(void) {
    char b[128];
    snprintf(b, sizeof b, "x=%d s=%s c=%c u=%u", 42, "abc", 'Z', 7u);
    puts(b);
    return 0;
}
