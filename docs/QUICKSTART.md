# Quickstart

Four things you can do, in increasing order of effort. Each section is
self-contained.

## 0. Prerequisites

| need | why | install |
|---|---|---|
| a C compiler | builds the loader | `apk add build-base` / `apt install build-essential` |
| LLVM 19 `clang` + `ld64.lld` | builds Mach-O fixtures and cross-compiles | `apk add clang lld llvm` |
| python3 | extraction and reporting scripts | `apk add python3` |
| ~50 GB free | the XIP expands to ~10 GiB of stream, ~1.3 GB extracted | — |
| `libc++.so.1`, `libz.so.1` | optional: lets C++/zlib imports resolve | `apk add libc++ zlib` |

No macOS host is needed for anything below.

## 1. Build the loader

```sh
cd workspace/darwin-port/src
cc -O2 -o ../bin/darwinld main.c macho.c shim.c fmt.c fmt_asm.S plat_posix.c dso.c
```

That is the whole build — no configure, no dependencies. The binary is ~85 KB.
If your filesystem drops the executable bit on create (some iOS/mobile
filesystems do), `chmod +x ../bin/darwinld`.

## 2. Run a Darwin binary

```sh
darwinld info   tool            # segments, dylibs, fixup kind
darwinld dylibs tool            # map the dependency graph, stop
darwinld check  tool            # bind; lists every unresolved symbol
darwinld report tool            # same, as JSON
darwinld run    tool args...    # execute
```

`check` is the important one. It tells you *before* anything executes whether
the shim covers the imports:

```
$ darwinld check .../usr/bin/otool
[darwinld] images 1, unresolved 0, base 0x100000000
$ darwinld run .../usr/bin/otool -h a.out
Mach header
      magic  cputype cpusubtype  caps    filetype ncmds sizeofcmds      flags
 0xfeedfacf 16777228          0  0x00           2    16       1120 0x00200085
```

Exit codes: `0` ok, `1` load/bind failure, `2` usage, `3` FairPlay-encrypted,
`4` unresolved imports.

## 3. Extract an Xcode XIP

```sh
python3 xipx.py extract Xcode.xip /root/xc \
  --include 'Xcode-beta.app/Contents/Developer/Platforms/MacOSX.platform/Developer/SDKs/MacOSX.sdk/usr' \
  --include 'Xcode-beta.app/Contents/Developer/Toolchains/XcodeDefault.xctoolchain/usr/bin' \
  -o manifest.json
```

One pass, ~25 minutes for 10.4 GiB streamed, memory stays at one 16 MiB chunk.
`--include` takes a path prefix; `--pattern` takes a shell glob;
`--exclude` removes matches. Without either, everything is extracted.

The macOS SDK ships with **`NULLcanary` placeholders** instead of real headers
(Apple deduplicates across the ten SDKs). Repair them before compiling:

```sh
python3 xipx.py extract Xcode.xip /root/hdr --pattern '*/SDKs/*/usr/include*'
python3 hdrmerge.py /root/hdr /root/xc/.../MacOSX.sdk      # 846 headers patched
```

## 4. Cross-compile for macOS

```sh
DARWIN_SDK=/root/xc/.../MacOSX.sdk DARWIN_MINOS=15.0 \
  bin/xclang -O2 -o hello hello.c

file hello        # Mach-O 64-bit arm64 executable
bin/darwinld run hello
```

`xclang` drives the host clang with `-target arm64-apple-macos15.0`,
`-isysroot <SDK>` and `-fuse-ld=lld`. Everything Apple-specific comes from the
extracted SDK: headers, framework stubs, and the `.tbd` text stubs that stand
in for the dylibs. See [ABI](ABI.md) if it fails with `malformed file` (that is
the `arm64e.x1-macos` target token, fixable by stripping it).

## 5. Verified end-to-end tests

```sh
cd workspace/xcode-re
sh tests/run.sh
# passed 11, failed 0, skipped 0
```

| test | proves |
|---|---|
| T1 | exit codes survive the loader (42 in, 42 out) |
| T2 | printf works despite Darwin's stack-passed varargs |
| T3 | a dylib is bound through its export trie (two-level namespace) |
| T4 | an unresolved import is exit 4, not a crash |
| T5 | `readdir` fills `d_namlen`/`d_name` at Darwin offsets |
| T6 | `report` emits per-image JSON |
| T7 | `setjmp` cannot smash a 192-byte Darwin `jmp_buf` |
| T8 | `statfs` fills the 2168-byte Darwin layout and zeroes the tail |
| T9 | (optional) Xcode's own `ar` and `otool` run |

## 6. Common first-run problems

| symptom | cause | fix |
|---|---|---|
| `refusing: FairPlay-encrypted` | App Store binary | not supported by design |
| `cannot run: N imports unresolved` | shim gap | `darwinld check` lists them; see [CLI](CLI.md#ds_missing_file) |
| `Exec format error` | you exec'd a Mach-O directly | use `darwinld run`, the kernel cannot run Mach-O |
| `malformed file` from lld | `arm64e.x1-macos` in a `.tbd` | strip the token (see [TROUBLESHOOTING](TROUBLESHOOTING.md)) |
| extraction killed | memory | one pass, includes only; see [REPRODUCING](REPRODUCING.md) |

Full catalogue: [TROUBLESHOOTING](TROUBLESHOOTING.md).
