# Deeper dive: multi-image loading and the ABI layer

*Part of the [darwin-port documentation](INDEX.md).*

First pass got single-image tools working (15 of 75). This pass adds the parts
a real toolchain needs: loading the dylibs a binary names, binding imports
against the right image, and the remaining Darwin/Linux ABI translations that
turned out to matter. Result: **40 of 75 fully bind**, including `otool`,
`nm`, `size`, `strings`, `bison`, `flex`, `m4`, `yacc`, `swift-demangle`.

## 1. Loading more than one image

`src/dso.c` implements what dyld does for a non-flat namespace:

```
load(path)
 ├─ macho_load + macho_map                 (slide if the preferred base is taken)
 ├─ record LC_ID_DYLIB as the install name
 └─ for each LC_LOAD_DYLIB: resolve → load()   (depth-limited, memoised)

bind: for every image, for every import (name, lib_ordinal)
 ├─ lib > 0 : the lib_ordinal-th dependency of *this* image
 ├─ lib = 0 : this image's own exports
 ├─ lib < 0 : flat search across every loaded image   (0xFE, 0xFF)
 └─ still nothing : the host libSystem shim
```

Symbol lookup uses the **export trie** (`LC_DYLD_EXPORTS_TRIE` or the
`LC_DYLD_INFO` export range), walked character by character, with reexport
edges followed to their target image, and a fallback scan of `LC_SYMTAB` for
images that carry no trie.

Path resolution order for an install name: as-is → `$DS_SYSROOT<name>` →
`$DS_LIB_PATH` (colon-separated) + basename → next to the loading image.
`@loader_path/`, `@executable_path/` and `@rpath/` are substituted.

### The null-page bug this exposed

A dylib's `__TEXT` has `vmaddr = 0`. `mmap(0, len, PROT_READ,
MAP_FIXED_NOREPLACE)` **succeeds** on this kernel, so `slide` stayed 0 and every
segment was copied to address 0 — `memcpy(NULL, …, 0x4000)`. The loader now
refuses any preferred base below `0x10000` and takes the slide path, which is
correct anyway: dylibs are position independent and dyld slides them the same
way.

### Test

```
$ ld64.lld -dylib -install_name /tmp/libgreet.dylib -o libgreet.dylib libgreet.o
$ ld64.lld -o greeter uselib.o -L/tmp -lgreet ...
$ darwinld run greeter
[dylib] shout() called
hello from a guest dylib: 20+22=42, shout=7
```

`greeter` is two images: the main image binds `_greet`/`_add`/`_shout` through
ordinal 1 to the dylib's trie, the dylib binds `_printf` to the shim, and both
are mapped at different slides.

## 2. ABI translations added in this pass

| Darwin API | problem | fix |
|---|---|---|
| `sigaltstack` | `stack_t` is `{ss_sp, ss_size, ss_flags}` on Darwin, `{ss_sp, ss_flags, ss_size}` on Linux → musl reads a 4-byte flags field as the stack size and returns ENOMEM | translate both directions |
| `sigaction` | Darwin's struct is `{handler, uint32 mask, int flags}`; every `SA_*` bit differs (`SA_ONSTACK` 0x1 vs 0x08000000 …) | full translation, both directions |
| `vm_allocate` | mach VM has no Linux equivalent | `mmap` anonymous |
| `sysctl` | no MIB on Linux | answer `CTL_HW/HW_MEMSIZE`, `HW_NCPU`; ENOTSUP otherwise |
| `xcselect_get_developer_dir_path` | returns int, 0 = success | fills `$DEVELOPER_DIR` |
| `xcselect_invoke_xcrun` | must exec a tool out of the developer dir | re-enters `darwinld` for Mach-O, so `m4` → `gm4` actually runs |
| `__srget`, `__swbuf`, `__mb_cur_max` | BSD stdio internals used by bison/yacc | `fgetc`/`fputc`/1 |
| `mach_task_self_`, `mach_host_self`, `host_info`, `mach_port_deallocate`, `task_set_exception_ports` | mach ports | constants + no-ops |
| `memset_pattern16` | vector pattern fill | byte loop on a 16-byte pattern |
| `uncompress` | lives in `libz.1.dylib` | `dlopen("libz.so.1")` at start-up |

`gm4` also showed why the exec interceptor matters: Apple's `m4` is a wrapper
that calls `xcselect_invoke_xcrun("gm4", …)`, which now re-enters the loader
instead of failing with `ENOEXEC`.

## 3. Coverage census

75 executables in the toolchain's `usr/bin` under 12 MB, run through
`darwinld check` with missing symbols logged to a file:

| pass | fully bindable | distinct unresolved |
|---|---|---|
| first | 15 / 75 | 3,270 |
| + dylib loader, signals, mach, xcselect | 34 / 75 | 3,256 |
| + dyld accessors, TLV bootstrap, dyld image list | **40 / 75** | 3,256 |

Fully bindable (40): `ar asa bison bm4 byacc c++filt c89 c99
cache-build-session clang-cas-test clang-format codesign_allocate ctags flex
gcov gm4 gperf indent llvm-cov llvm-cxxfilt llvm-otool llvm-profdata
llvm-readtapi llvm-size m4 metal nm-classic otool otool-classic readtapi
rpcgen segedit size size-classic strings swift-demangle tapi-analyze unifdef
unwinddump yacc`

Verified running (not just binding): `otool -h`, `otool -l`, `size`,
`nm -a`, `strings`, `ctags -x`, `bison --version`, `flex --version`,
`gperf --version`, `clang-format`, `codesign_allocate`, `ar -rc/-t`,
`indent`, `unifdef`, `bm4`, `rpcgen`, `asa`.

### What is left, by symbol count

The 3,256 unresolved names are almost entirely three runtimes:

```
3019  ___CFConstantStringClassReference    CoreFoundation string literals
1084  __objc_empty_cache                   Objective-C runtime
 904  _swift_deletedMethodError            Swift runtime
 687  _OBJC_METACLASS_$_NSObject           Objective-C runtime
 311  _OBJC_CLASS_$_NSObject
 302  _$sSQMp ...                          Swift protocol witnesses
```

That is a clean boundary: everything that is C, C++ (against host libc++ via
`std::__1`) and BSD gets through; everything that is ObjC/Swift/CoreFoundation
needs those runtimes loaded as real images, which is the next layer, not a
shim entry. `swiftc`, `sourcekit-lsp` and the Metal compiler sit behind it.

Two of the newest binders start and then trap inside LLVM's static
initialisers (`llvm::InitLLVM` calls `llvm_unreachable`, SIGILL) — binding is
necessary, not sufficient; runtime behaviour still has to be tested per tool.

## 4. What a next pass would do

1. Load `libobjc.A.dylib` and `CoreFoundation` as guest images (they are in
   the SDK's `usr/lib`), which requires: class registration, `+load` order,
   `__DATA` protection changes, and trampolines for message sends.
2. Load `libswiftCore.dylib` — same machinery plus Swift metadata
   registration.
3. `dirent` translation (`readdir` hands back a 1024-byte `d_name` struct on
   Darwin, 256 on Linux) for tools that walk directories.
