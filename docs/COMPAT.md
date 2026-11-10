# COMPAT — every translated call, and the test that proves it

Format: **call** → translation → *proof*. Test names refer to `tests/run.sh`
(`T1`…`T7`); "corpus" means the 40 executables that bind completely in the
Xcode 27.3 toolchain and are exercised by `reports/compatibility-matrix.md`.

## Structs

| call | translation | proof |
|---|---|---|
| `statfs`/`fstatfs` | Linux `statvfs` → Darwin 2168-byte layout (`f_fstypename@72`, `f_mntonname@88`, `f_mntfromname@1112`), tail zeroed | **T8** `statfs fills the Darwin layout, zeroes the tail` |
| `setjmp`/`longjmp`/`_setjmp`/`_longjmp`/`sigsetjmp`/`siglongjmp` | own arm64 implementation writing 168 B into Darwin's 192-byte `jmp_buf` slot; sig variants tail-call | **T7** `setjmp/longjmp sized to Darwin jmp_buf` |
| `stat`/`lstat`/`fstat`/`fstatat` | Linux `struct stat` → Darwin 144-byte layout (`mode@4`, `ino@8`, times `32/48/64/80`, `size@96`, `blksize@112`); `_Static_assert` on every offset | corpus: `ar` writes 60-byte member headers with correct size/mtime; `size` prints segment sizes |
| `dirent` (`readdir`) | host `struct dirent` → 1048-byte Darwin layout (`d_seekoff@8`, `d_namlen@18`, `d_name@21[1024]`), `_Static_assert` | **T5** `readdir fills d_namlen/d_name at Darwin offsets` |
| `opendir`/`closedir`/`rewinddir`/`seekdir`/`telldir` | 32-slot wrapper table mapping the guest's `DIR *` to the host's | **T5** |
| `sigaction` | `{handler, u32 mask, int flags}` ↔ `{handler, sigset128, int flags, restorer}`, `SA_*` bit map | corpus: `gm4` installs handlers then runs |
| `sigaltstack` | `{ss_sp, ss_size, ss_flags}` ↔ Linux order; sizes < 8 KiB raised | corpus: `gm4` no longer returns ENOMEM |
| `sigprocmask` | 32-bit Darwin mask ↔ 128-byte Linux `sigset_t` | corpus |
| `open`/`openat` | Darwin `O_CREAT 0x200 / O_TRUNC 0x400 / O_EXCL 0x800 / O_APPEND 0x8` → per-flag translation | **T9** `Xcode's ar writes a real archive` |

## Variadic calls (Darwin passes anonymous args on the stack)

| call | translation | proof |
|---|---|---|
| `printf`, `fprintf`, `sprintf`, `snprintf`, `asprintf`, `dprintf` | asm front end captures `sp`; `fmt.c` formats from an 8-byte cursor | **T2** `printf reimplemented for the stack ABI` |
| `__sprintf_chk`, `__snprintf_chk` | same cursor + bounded buffer | corpus: `ar` fills its member header |
| `vfprintf`, `vsprintf`, `vsnprintf`, `vasprintf`, `__vsnprintf_chk` | Darwin `va_list` *is* the cursor; no conversion | corpus |
| `err`, `warn`, `warnx` | cursor + program name | corpus: `bison`, `m4` diagnostics |

## Data symbols (must be variables, never functions)

| call | translation | proof |
|---|---|---|
| `__stdinp`/`__stdoutp`/`__stderrp` | address of a `FILE *` variable | corpus: `ar` usage output |
| `mach_task_self_`, `mach_thread_self` | constant port numbers | corpus |
| `_NSGetArgc/Argv/Environ/ExecutablePath` | process globals | corpus: `ar -t` reads its argv |
| `__mb_cur_max`, `_DefaultRuneLocale` | 1 / zeroed 4 KiB table | corpus: `bison` |
| `optind`, `optarg`, `environ`, `__progname` | host libc's own variables | corpus: `ar -t`, `ctags -x` |

## Mach / dyld

| call | translation | proof |
|---|---|---|
| `vm_allocate` / `vm_deallocate` | `mmap`/`munmap` | corpus: `codesign_allocate`, `segedit` |
| `host_info`, `mach_port_deallocate`, `task_set_exception_ports` | zeros / no-ops | corpus: `size`, `strings`, `nm` |
| `dyld_image_count`, `dyld_get_image_name`, `dyld_get_image_header`, `dyld_get_image_vmaddr_slide` | answered from the loader's image table | corpus: `readtapi`, `swift-demangle` bind |
| `dyld_get_shared_cache_range`, `dyld_for_each_installed_shared_cache` | 0 / no callbacks | corpus |
| `__tlv_bootstrap` | one block per descriptor | corpus |
| `dyld_stub_binder` | no-op (all binds eager) | **T1** |
| `LC_DYLD_INFO` weak binds | executed like eager binds | corpus: `coremlc` now reports 3,083 weak imports it used to hide |
| `LC_DYLD_INFO` rebase opcodes | applied when the image slid | **T3** (dylib mapped at a slide) |

## libSystem odds and ends

| call | translation | proof |
|---|---|---|
| `strlcpy`, `strlcat`, `strmode`, `strtonum`, `reallocf` | reimplemented | corpus |
| `__strlcpy_chk`, `__strlcat_chk`, `__strcpy_chk`, `__strcat_chk`, `__memcpy_chk`, `__memset_chk` | bounded wrappers | corpus: `ctags`, `unifdef` |
| `__maskrune`, `__toupper`, `__tolower` | ctype with Darwin `_CTYPE_*` masks | corpus: `unifdef`, `rpcgen` |
| `__srget`, `__swbuf` | `fgetc`/`fputc` | corpus: `bison`, `yacc` |
| `memset_pattern16` | 16-byte pattern loop | corpus: `flex` |
| `copyfile`, `copyfile_state_alloc/free` | byte copy / dummy state | corpus: `unifdef` |
| `qsort_b` | block invoke at word 3 → thunk into `qsort` | corpus |
| `arc4random`, `arc4random_buf` | `/dev/urandom` | corpus: `clang-format` |
| `sysctl` | `hw.memsize`, `hw.ncpu`; else ENOTSUP | corpus: `gcov`, `llvm-cov` |
| `xcselect_get_developer_dir_path`, `xcselect_invoke_xcrun` | `$DEVELOPER_DIR`; exec through the loader | corpus: `m4` → `gm4` |
| `execvp` | Mach-O targets re-enter `darwinld` | **T7** (`ar` → `ranlib`) |
| `__cxa_atexit`, `__cxa_finalize`, `__tlv_atexit` | reverse-order list on `atexit` | corpus |
| `__chkstk_darwin` | empty | corpus: every clang-built guest |
| `__assert_rtn` | message + `abort` | corpus |
| `fsgetpath`, `proc_pidinfo`, `terminate_with_payload` | **ENOSYS**, never a plausible lie | static: `shim.c` table |
| `_Block_object_assign/dispose`, `_NSConcrete*Block` | no-ops / static literals | corpus |

## What is *not* translated (see DEVIATIONS.md)

`termios`/`ioctl`, `locale_t`, `getmntinfo`, per-thread TLS, `LC_RPATH` search
order, dyld shared cache, `pthread_setname_np` semantics, `os_log` delivery,
`dispatch_*`.
