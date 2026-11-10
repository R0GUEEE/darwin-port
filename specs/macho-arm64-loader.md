# Spec — Darwin arm64 Mach-O loading (behavioral contract)

Derived from measurement on Xcode 27.3 beta images, not from documentation.
Every clause below was necessary to execute a real Apple binary; each is
covered by a test in `tests/`.

## 1. Image mapping

1. Reject `LC_ENCRYPTION_INFO_64` images with `cryptid != 0` (FairPlay).
2. Compute `lo = min(vmaddr)` over all segments except `__PAGEZERO`,
   `hi = max(vmaddr + vmsize)`, reserve `[lo, hi)` rounded to 16 KiB.
3. If `lo < 0x10000` the image is position independent (a dylib): do **not**
   map at the preferred address. `mmap(0, …, MAP_FIXED_NOREPLACE)` succeeds on
   some kernels and every segment then lands on the null page.
4. Copy `filesize` bytes of each segment from `fileoff`; leave the rest zero.

## 2. Fixups

1. `LC_DYLD_CHAINED_FIXUPS` takes precedence over `LC_DYLD_INFO`.
2. `dyld_chained_starts_in_segment` has two dialects. Discriminate with
   `size == 22 + 2 * u16@20`:
   * true → LLVM/ld64.lld: `page_size@4, pointer_format@6, segment_offset@8,
     max_valid@16, page_count@20, page_start@22`
   * false → dyld: `page_size@4, page_format@6, max_valid@8,
     pointer_format@16, segment_offset@18, overflow_count@26, page_start@28`
3. `page_start[i] == 0xFFFF` means "no fixups on this page".
4. `DYLD_CHAINED_PTR_64` (2): stride 4, bind flag = bit 63,
   `ordinal = raw & 0xFFFFFF`, `addend = s8(raw >> 24)`, `next = (raw >> 51) & 0xFFF`.
   Rebase target is an absolute vmaddr.
5. `DYLD_CHAINED_PTR_64_OFFSET` (6): same layout, but the rebase target is a
   **vm offset from the image base** and must have the base added even when the
   slide is zero.
6. Import records (`DYLD_CHAINED_IMPORT`) are `lib_ordinal:8, weak:1,
   name_offset:23` — the name lives in the **high** bits (`v >> 9`).
7. Classic `LC_DYLD_INFO` bind streams: `DONE` terminates the whole stream for
   eager binds but only one entry for lazy binds.

## 3. Symbol resolution

1. Strip exactly one leading `_` from a Mach-O symbol to get the host name.
2. Darwin mangles availability variants as `name$FEATURE` (`realpath$DARWIN_EXTSN`);
   truncate at `$` before lookup.
3. Resolve in this order: the image named by `lib_ordinal` (two-level
   namespace) → the image itself (`ordinal == 0`) → every loaded image
   (`0xFE`/`0xFF`) → the host shim.
4. A symbol that is a **variable** in Darwin (`__stdoutp`, `mach_task_self_`,
   `_NSGetArgv`) must resolve to the address of a variable, never to a function.

## 4. Entry

1. `LC_MAIN.entryoff` is a file offset from the start of `__TEXT`;
   `entry = __TEXT.vmaddr + slide + entryoff`.
2. Run `__mod_init_func` sections of every loaded image before the entry.
3. Call `main(argc, argv, envp, apple)` with `apple = NULL`; the guest may read
   `_NSGetArgv`/`_NSGetEnviron` instead.

## 5. Variadic calls (the one that decides whether tools work)

Apple's arm64 ABI passes **all anonymous arguments on the stack** and
`va_list` is a plain `char *`. Consequences:

1. Every variadic libSystem entry point (`printf`, `snprintf`, `err`, `warn`,
   `__sprintf_chk`, …) needs an assembly front end that captures `sp` at entry
   (`mov x4, sp; b c_impl`) and a formatter that consumes 8-byte slots.
2. The same applies to `err`/`warn`/`warnx`, whose message is a format string.
3. Non-variadic wrappers (`vfprintf`) receive the cursor directly — a Darwin
   `va_list` is already that cursor.

## 6. Struct ABI

| type | Darwin | Linux | consequence if untranslated |
|---|---|---|---|
| `struct stat` | 144 B: `mode@4, nlink@6, ino@8, times@32/48/64/80, size@96, blocks@104` | different order | sizes and times read from wrong fields |
| `open()` flags | `O_CREAT 0x200, O_TRUNC 0x400, O_EXCL 0x800, O_APPEND 0x8` | `0x40, 0x200, 0x80, 0x400` | creates/truncates the wrong file |
| `stack_t` | `{ss_sp, ss_size, ss_flags}` | `{ss_sp, ss_flags, ss_size}` | `sigaltstack` → ENOMEM |
| `struct sigaction` | `{handler, u32 mask, int flags}`, `SA_ONSTACK 0x1` | 128-byte mask, `SA_ONSTACK 0x08000000` | handlers never fire correctly |
| `sigset_t` | 32-bit | 128 bytes | `sigprocmask` corruption |
| `dirent` | `d_namlen` + 1024-byte name | 256-byte name, no `d_namlen` | `readdir` garbage (not yet done) |

## 7. Diagnostics

The loader's own output must never share stdout with the guest: a tool's
pipeline behaves differently otherwise. Status lines go to stderr prefixed
`[darwinld]`.
