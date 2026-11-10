typedef unsigned long size_t;
typedef long intptr_t;
extern int printf(const char *, ...);
extern void dyld_register_func_for_add_image(void (*cb)(const void *, intptr_t));
static int count;
static void cb(const void *mh, intptr_t slide) {
    (void)mh; (void)slide;
    count++;
}
int main(void) {
    dyld_register_func_for_add_image(cb);
    printf("callbacks=%d\n", count);
    return count > 0 ? 0 : 1;
}
