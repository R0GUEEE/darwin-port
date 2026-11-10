extern const char *greet(void);
extern int add(int, int);
extern int shout(void);
extern int printf(const char *, ...);
int main(void) { printf("%s: 20+22=%d, shout=%d\n", greet(), add(20, 22), shout()); return 0; }
