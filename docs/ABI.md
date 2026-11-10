# ABI reference — Darwin arm64 vs Linux aarch64

Every number here was measured: Darwin layouts from the extracted SDK headers
and guarded by `_Static_assert` in `src/shim.c`, Linux layouts from an
`offsetof` probe compiled on the target host. If a value changes, the build
fails rather than silently corrupting.

## 1. Calling convention

The instruction set is identical. The two dialects diverge in exactly one place,
and it decides whether a toolchain works.

### Variadic calls and `va_list`

| | Darwin arm64 | AAPCS64 (Linux) |
|---|---|---|
| anonymous arguments | **all on the stack**, 8-byte slots, starting at `sp` | first 8 in `x0`–`x7`, rest on the stack |
| `va_list` | a bare pointer to the first slot | a 32-byte register-save-area struct |
| `va_arg` for `double` | read the slot as `double` | read from the FP save area |

Consequence: a Darwin guest calling `printf("%d", 42)` leaves `42` at `[sp]`
and `x1`–`x7` untouched. Forwarding that call to the host `printf` makes it
read `x1` — which holds whatever the caller last used.

Fix: every variadic libSystem entry point gets an assembly front end that
captures `sp` and tail-branches to a cursor-based formatter:

```asm
ds_printf_stub:
    mov x1, sp          ; anonymous args are already contiguous at sp
    b   ds_printf_c     ; tail call: the guest's x30 is preserved
```

`fmt.c` consumes 8-byte slots (`arg_u64`, `arg_ptr`, `arg_dbl`) and implements
`%d i u o x X c s p f e g %`, flags, width, precision (including `*`) and the
`l/ll/q/z/t/j` length modifiers.

The same applies to `__sprintf_chk`, `__snprintf_chk`, `asprintf`, `dprintf`,
`err`, `warn`, `warnx`. Non-variadic wrappers (`vfprintf` and friends) receive
the cursor directly — a Darwin `va_list` *is* that cursor.

`sigsetjmp`/`siglongjmp` must also tail-call rather than wrap: a wrapper's stack
frame is gone by the time `longjmp` returns into it.

### Everything else

Integer and floating-point argument passing, structure passing, return values,
and stack alignment are the same. No translation is needed for ordinary calls.

## 2. Symbols: functions vs data

Darwin exports two kinds of symbol and they are not interchangeable:

| symbol | kind | if you bind it to a function |
|---|---|---|
| `printf`, `strlen` | function | correct |
| `__stdoutp`, `__stderrp`, `__stdinp` | **variable** holding `FILE *` | the guest loads the function's first instruction as a `FILE *` → crash inside `vfprintf` |
| `mach_task_self_`, `mach_thread_self` | variable | same |
| `optind`, `optarg`, `environ`, `__progname` | variable | same |
| `_NSGetArgv` | *function* returning `char ***` | correct |
| `__mb_cur_max` | variable | same |

The shim therefore exports `&variable` for the data symbols and a function
pointer for the function symbols, and the same C identifier must not be used
for both.

### Name mangling

1. A Mach-O symbol carries one extra leading underscore: `_printf` → `printf`.
2. Darwin mangles availability variants with `$`: `realpath$DARWIN_EXTSN` is
   `realpath` with an availability annotation. Truncate at `$`.
3. C++ needs no translation at all: Apple's libc++ and Linux libc++ both use
   `std::__1`, so `__ZNSt3__1…` is already the right mangled name once the
   leading underscore is stripped.

## 3. Structures

### `struct stat` — 144 B (Darwin) vs 128 B (Linux)

| field | Darwin offset | Linux (musl) offset |
|---|---|---|
| `st_dev` | 0 (int32) | 0 (int64) |
| `st_mode` | **4 (uint16)** | **16 (uint32)** |
| `st_nlink` | 6 (uint16) | 20 (uint32) |
| `st_ino` | 8 (uint64) | 8 |
| `st_uid` / `st_gid` | 16 / 20 | 24 / 28 |
| `st_rdev` | 24 | 32 |
| `st_atimespec` | 32 (16 B) | 72 (`st_atim`) |
| `st_mtimespec` | **48** | 88 |
| `st_ctimespec` | 64 | 104 |
| `st_birthtimespec` | 80 | — (no birth time) |
| `st_size` | **96** | **48** |
| `st_blocks` | 104 | 64 |
| `st_blksize` | 112 (int32) | 56 |

A passthrough gives the guest a file size read from the middle of a timestamp,
and a mode read from the inode. `ar` writes 60-byte headers containing exactly
those fields — that is the test that caught it.

`st_birthtimespec`, `st_flags`, `st_gen` are zero-filled: Linux has no
equivalent.

### `struct statfs` — 2168 B (Darwin) vs `struct statvfs` 112 B (Linux)

| field | Darwin offset |
|---|---|
| `f_bsize` (u32), `f_iosize` (i32) | 0, 4 |
| `f_blocks`, `f_bfree`, `f_bavail`, `f_files`, `f_ffree` (u64) | 8, 16, 24, 32, 40 |
| `f_fsid` (2 × i32) | 48 |
| `f_owner`, `f_type`, `f_flags`, `f_fssubtype` | 56, 60, 64, 68 |
| `f_fstypename[16]` | **72** |
| `f_mntonname[1024]` | 88 |
| `f_mntfromname[1024]` | **1112** |
| `f_flags_ext`, `f_reserved[7]` | 2136, 2140 |

Writing a 112-byte `statvfs` into a 2168-byte buffer leaves 2 KB of whatever
was on the guest's stack.

### `struct dirent` — 1048 B (Darwin) vs 280 B (Linux)

| field | Darwin | Linux |
|---|---|---|
| `d_ino` | 0 | 0 |
| `d_seekoff` | 8 | — (`d_off` at 8, different meaning) |
| `d_reclen` | 16 (u16) | 16 (u16) |
| `d_namlen` | **18 (u16)** | — |
| `d_type` | 20 | **18** |
| `d_name[1024]` | **21** | 19, 256 bytes |

The loader keeps a 32-slot table mapping the guest's `DIR *` to the host's and
translates each entry into its own buffer.

### Signals

`struct sigaction` — Darwin 16 B, Linux 152 B:

| field | Darwin | Linux |
|---|---|---|
| handler | 0 | 0 |
| mask | **8 (uint32)** | 8 (`sigset_t`, 128 B) |
| flags | 12 | 136 |
| restorer | — | 144 |

`sigset_t`: **4 bytes** on Darwin, 128 on Linux.

`stack_t` (24 B both, different order):

| | Darwin | Linux |
|---|---|---|
| `ss_sp` | 0 | 0 |
| `ss_size` | **8** | **16** |
| `ss_flags` | **16** | **8** |

The swap makes musl read a 4-byte flags word as the stack size and return
ENOMEM. That is what killed `gm4` until it was translated.

### `jmp_buf` — 192 B (Darwin) vs 312 B (musl)

Darwin: `_JBLEN = (14 + 8 + 2) * 2 = 48` ints. musl's `setjmp` writes 312 bytes.
A guest that declares the Darwin size and calls the host's `setjmp` gets a
120-byte stack smash on every call. The loader ships its own arm64
`setjmp`/`longjmp` writing exactly 168 bytes (x19–x28, fp, lr, sp, d8–d15).

### Others

| type | Darwin | Linux | status |
|---|---|---|---|
| `struct termios` | 72 | 60 | **not translated** |
| `pthread_t` | 8 | 8 | ok |
| `struct tm` | same | same | ok |

## 4. Flags

### `open()`

| flag | Darwin | Linux | |
|---|---|---|---|
| `O_CREAT` | 0x200 | **0x40** | translate |
| `O_EXCL` | 0x800 | **0x80** | translate |
| `O_TRUNC` | 0x400 | **0x200** | translate |
| `O_APPEND` | 0x8 | **0x400** | translate |
| `O_NONBLOCK` | 0x4 | 0x800 | translate |
| `O_NOFOLLOW` | 0x100 | 0x8000 | translate |
| `O_DIRECTORY` | 0x100000 | 0x4000 | translate |
| `O_CLOEXEC` | 0x1000000 | 0x80000 | translate |
| `O_SHLOCK`/`O_EXLOCK`/`O_EVTONLY` | 0x10 / 0x20 / 0x8000 | — | dropped |

A passthrough does not fail — it truncates a different file than the one asked
for (Darwin `O_TRUNC 0x400` is Linux `O_APPEND`).

### `sigaction`

| flag | Darwin | Linux |
|---|---|---|
| `SA_ONSTACK` | 0x1 | 0x08000000 |
| `SA_RESTART` | 0x2 | 0x10000000 |
| `SA_RESETHAND` | 0x4 | 0x80000000 |
| `SA_NOCLDSTOP` | 0x8 | 0x1 |
| `SA_NODEFER` | 0x10 | 0x40000000 |
| `SA_NOCLDWAIT` | 0x20 | 0x2 |
| `SA_SIGINFO` | 0x40 | 0x4 |

### `<ctype.h>` masks (`__maskrune`)

Darwin uses bit flags (`_CTYPE_A 0x100`, `_CTYPE_D 0x400`, `_CTYPE_S 0x4000`,
`_CTYPE_U 0x8000`, …); Linux uses an 8-bit class table with completely different
bits. `__maskrune` is reimplemented against `isalpha`/`isdigit`/… rather than
shared.

## 5. Runtime services

| Darwin | translation |
|---|---|
| `errno` | `__error()` returns `&errno`; there is no `__errno_location` |
| `mach_absolute_time()` | `clock_gettime(CLOCK_MONOTONIC)` in ns, timebase 1/1 |
| `mach_task_self_`, `mach_host_self`, `mach_thread_self` | constant port numbers |
| `vm_allocate` / `vm_deallocate` | `mmap` / `munmap` |
| `__tlv_bootstrap` | one block per descriptor — **not** per thread |
| `__cxa_atexit` | one global list, run in reverse at process exit |
| `dyld_image_count`, `dyld_get_image_name/header/vmaddr_slide` | from the loader's image table |
| `dyld_get_shared_cache_range` | length 0 (there is no shared cache) |
| `xcselect_get_developer_dir_path` | `$DEVELOPER_DIR` |
| `xcselect_invoke_xcrun` | exec through the loader, so the tool really runs |
| `execvp` of a Mach-O | re-enters `darwinld` (the kernel cannot exec Mach-O) |

## 6. Translation policy

| category | policy |
|---|---|
| same behaviour on both sides (`memcpy`, `strlen`, `malloc`, `qsort`) | **forward** to the host; do not shim |
| different struct, different flags, different calling convention | **translate** field by field, guarded by `_Static_assert` |
| exists only on Darwin (`os_log`, mach traps) | **emulate** with a documented minimum, or refuse |
| cannot be honestly provided (`fsgetpath`, `proc_pidinfo`) | **refuse**: `-1` + `ENOSYS`, never a plausible answer |
| runtime the loader does not have (ObjC, Swift, CF) | **missing**: `check` reports the symbol by name |

The full call-by-call table with the test that proves each entry is
[COMPAT](COMPAT.md); the list of places where behaviour deliberately differs is
[DEVIATIONS](DEVIATIONS.md).
