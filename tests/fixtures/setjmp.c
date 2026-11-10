typedef unsigned long size_t;
extern int printf(const char *, ...);
extern int setjmp(int *);
extern void longjmp(int *, int);
static int buf[48];                       /* Darwin jmp_buf = int[48] = 192 B */
static volatile unsigned int canary[32];
int main(void) {
    for (int i = 0; i < 32; i++) canary[i] = 0x5A5A5A5Au;
    int v = setjmp(buf);
    if (v == 0) longjmp(buf, 7);
    int ok = 1;
    for (int i = 0; i < 32; i++) if (canary[i] != 0x5A5A5A5Au) ok = 0;
    printf("longjmp=%d canary=%s\n", v, ok ? "intact" : "SMASHED");
    return ok && v == 7 ? 0 : 1;
}
