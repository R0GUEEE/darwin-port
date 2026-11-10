# Compatibility

What runs, where, and how it was measured. "binds" = every import resolved;
"runs" = observed producing correct output on this device.

## Hosts

| host | guest arm64 Mach-O | guest x86_64 Mach-O |
|---|---|---|
| Linux aarch64 | **runs** (same ISA) | no — needs an x86 JIT (not present) |
| Windows on ARM64 | designed to run (same ISA), **never compiled** | no |
| Windows x64 | binds only (`check`/`report` work) | no |
| macOS | n/a — the kernel already does this | n/a |

## Toolchain executables (Xcode 27.3, `usr/bin`, ≤ 12 MB)

40 of 76 bind completely. Those marked **run** were executed and produced real
output; the rest bind but were not exercised.

| tool | bind | run | evidence |
|---|---|---|---|
| `ar` | ✓ | **yes** | creates `!<arch>` archives with correct headers; readable by host `ar`; `-t` lists members |
| `otool`, `otool-classic` | ✓ | **yes** | `-h` prints the Mach header, `-l` prints load commands |
| `nm-classic` | ✓ | **yes** | symbol table with `U`/`T` types |
| `size`, `size-classic` | ✓ | **yes** | `__TEXT/__DATA/__OBJC/others/dec/hex` columns |
| `strings` | ✓ | **yes** | string table dump |
| `ctags` | ✓ | **yes** | `-x` cross-reference correct (`cmp 6 file`, `main 8 file`) |
| `bison` | ✓ | **yes** | `bison (GNU Bison) 2.3` |
| `flex` | ✓ | **yes** | `flex 2.6.4` |
| `yacc` | ✓ | partial | dispatches to `bison`; needs `usr/share/bison` which was not extracted |
| `m4` | ✓ | **yes** | dispatches to `gm4` through `xcselect_invoke_xcrun` |
| `gm4` | ✓ | **yes** | expands macros |
| `gperf` | ✓ | **yes** | `GNU gperf 3.0.3` |
| `c++filt`, `llvm-cxxfilt` | ✓ | **yes** | demangles |
| `clang-format` | ✓ | **yes** | formats a C file |
| `codesign_allocate`, `segedit`, `cache-build-session`, `unwinddump`, `asa`, `indent`, `unifdef`, `bm4`, `rpcgen`, `metal`, `llvm-size`, `llvm-profdata`, `llvm-readtapi`, `readtapi`, `swift-demangle`, `tapi-analyze`, `clang-cas-test`, `llvm-cov`, `gcov`, `c89`, `c99`, `byacc` | ✓ | mostly | bind fully; execution verified for the ones with an obvious CLI. `llvm-profdata` and `swift-demangle` bind then trap in `llvm::InitLLVM` |

Near misses: `lipo` 5 symbols, `llvm-cas` 4, `libtool`/`ranlib` 10 (`_lto_*`),
`clang` 44 (`libclang-cpp.dylib`), `ld` 138, `swiftc` 34,011.

## What the loader itself can do

| capability | status |
|---|---|
| single arm64 Mach-O executable | **yes** — 138 images parsed, dozens run |
| chained fixups, both dialects | **yes** |
| classic `LC_DYLD_INFO` bind/rebase/weak/lazy | **yes** |
| two-level namespace across images | **yes** — exe + dylib test |
| export trie, reexports | **yes** |
| `__mod_init_func` ordering | **yes**, per image, load order |
| thread-local variables | one block per descriptor, **not per thread** |
| ObjC / Swift / CoreFoundation | **no** — 3,019 + 1,084 + 904 unresolved names |
| FairPlay images | refused (exit 3) |
| x86_64 / arm64_32 guests | not supported |

## Cross-compilation

| output | status |
|---|---|
| `arm64-apple-macos` executable | **yes**, with the real SDK (headers + `.tbd` + LLD) |
| `arm64-apple-macos` dylib | **yes** (`ld64.lld -dylib`) |
| `x86_64-apple-macos` | linkable, not runnable here |
| `arm64-apple-ios*` | needs the iPhoneOS SDK extracted; not attempted |
| Swift / ObjC sources | no (needs the Swift toolchain and runtime) |

## How the numbers were produced

```sh
# bind census over every toolchain binary under 12 MB
for f in $BIN/*; do
  DS_MISSING_FILE=/tmp/m darwinld check "$f" >/dev/null 2>&1
  echo "$(wc -l < /tmp/m 2>/dev/null || echo 0) $f"
done | sort -n

# run a binary and check its output, not just its exit code
darwinld run $BIN/otool -h a.out | grep 0xfeedfacf
```

The raw census is in `xcode-re/reports/`; the per-call proof table is
[COMPAT](COMPAT.md); the behavioural differences are [DEVIATIONS](DEVIATIONS.md).

## The boundary, stated precisely

The three runtimes that stop the port are not "harder functions". They are data
structures the code walks:

* `___CFConstantStringClassReference` (3,019 references) — CoreFoundation's
  `__CFConstantString` objects; the class pointer is written by CF at start-up.
* `__objc_empty_cache` (1,084) — a field inside every class object; the ObjC
  runtime rewrites it during method-cache installation.
* `_$s…` Swift symbols (904 in the tools that use Swift at all) — protocol
  witness tables and metadata records.

No shim entry can satisfy those: they require the real runtime image loaded,
registered, and its initialisers run in dyld's order. That is the next project,
not the next patch.
