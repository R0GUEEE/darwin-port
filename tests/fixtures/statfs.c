typedef unsigned long size_t;
typedef unsigned int uint32_t;
extern int printf(const char *, ...);
extern int statfs(const char *, void *);
struct statfs {
    uint32_t f_bsize; int f_iosize;
    unsigned long long f_blocks, f_bfree, f_bavail, f_files, f_ffree;
    int f_fsid[2]; unsigned int f_owner, f_type, f_flags, f_fssubtype;
    char f_fstypename[16]; char f_mntonname[1024]; char f_mntfromname[1024];
    uint32_t f_flags_ext, f_reserved[7];
};
int main(void) {
    struct statfs s;
    for (unsigned i = 0; i < sizeof s; i++) ((char *)&s)[i] = (char)0xCC;
    if (statfs("/", &s) != 0) return 1;
    int clean = 1;
    for (int i = 2150; i < 2168; i++) if (((char *)&s)[i] != 0) clean = 0;
    printf("bsize=%u fstype=%s tail=%s\n", s.f_bsize, s.f_fstypename, clean ? "zeroed" : "dirty");
    return s.f_blocks > 0 && clean ? 0 : 1;
}
