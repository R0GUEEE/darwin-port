extern int printf(const char *, ...);
static const char *msg = "hello from a guest dylib";
const char *greet(void) { return msg; }
int add(int a, int b) { return a + b; }
int shout(void) { printf("[dylib] shout() called\n"); return 7; }
