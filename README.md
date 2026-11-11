# darwin-port

Extract Apple's Xcode archives at the byte level, then **run the Darwin
toolchain they contain on Linux** — a Mach-O loader with a libSystem ABI shim,
and a cross-compiler that builds real Mach-O against the real SDK.

No macOS host is involved, and no Apple binary is redistributed.

## What it does

```sh
# run a real Xcode tool
$ darwinld run .../usr/bin/otool -h a.out
Mach header
      magic  cputype cpusubtype  caps    filetype ncmds sizeofcmds      flags
 0xfeedfacf 16777228          0  0x00           2    16       1120 0x00200085

# build for macOS on Linux, then run the result here
$ DARWIN_SDK=/root/xc/.../MacOSX.sdk xclang -O2 -o hello hello.c
$ darwinld run hello
args=2 first=ARG1 pi=3.1416 sorted=1359

# unpack a 2 GB .xip (10.4 GiB of stream) in one pass
$ python3 xipx.py extract Xcode.xip /root/xc --include '.../MacOSX.sdk/usr'

# build an Xcode project without Xcode, then run it
$ python3 pxcodebuild.py build Demo.xcodeproj --sdk $SDK
[macosx] Demo → build/Demo.app   1 objects, 33536 bytes binary
```

## Status

| | |
|---|---|
| XIP integrity | **authentic** — RSA signature over the TOC verifies, chain to Apple Root CA, `sha1(Content)` matches |
| CLT DMG | **damaged** — CRC mismatch, 4 payloads holed, 95 % zeros |
| Mach-O images parsed | 138 (155 arm64 slices, 25 x86_64, 2 arm64_32) |
| toolchain executables that bind fully | **40 of 76** |
| verified running | `ar`, `otool`, `nm`, `size`, `strings`, `ctags`, `bison`, `flex`, `m4`, `gperf`, `clang-format`, … |
| cross-compile to `arm64-apple-macos` | **works**, output runs through the loader |
| build an `.xcodeproj` without Xcode | **works** — `pxcodebuild` → `.app`, runs |
| ObjC / Swift / CoreFoundation | **does not run** — 3,019 + 1,084 + 904 unresolved names |
| Windows | backend written, **never compiled** |

## Layout

```
src/        loader: macho (parse+fixups+trie), dso (images), shim (ABI), fmt (varargs)
bin/        darwinld (built), xclang
*.py        xipx (xip) · dmgx (dmg) · xarx + pkgx (pkg) · inventory · hdrmerge
            pxcodebuild (build .xcodeproj on Linux) · swift_abi (Swift ABI coverage)
docs/       17 documents — start at docs/INDEX.md
reports/    acquisition, dependency graph, 167,522-entry inventory (gzipped)
specs/      behavioural contracts for the loader and the containers
tests/      run.sh + fixtures — 12 tests, no Apple SDK needed for 11 of them
```

Build and test:

```sh
make            # → bin/darwinld
make test       # → suite result depends on optional Xcode tools
```

## Documentation

| | |
|---|---|
| [INDEX](docs/INDEX.md) | the map, and which doc answers your question |
| [QUICKSTART](docs/QUICKSTART.md) | install → build → run, in four steps |
| [DEPENDENCIES](docs/DEPENDENCIES.md) | audited host requirements and bootstrap setup |
| [CONFIGURATION](docs/CONFIGURATION.md) | smart installer, profiles, environment template, SDK/runtime paths |
| [ARCHITECTURE](docs/ARCHITECTURE.md) | why a loader, and how the pieces fit |
| [DARWINLD](docs/DARWINLD.md) | loader internals: mapping, fixups, binding, diagnostics |
| [ABI](docs/ABI.md) | Darwin vs Linux, structure by structure, with offsets |
| [CLI](docs/CLI.md) | every tool and environment variable |
| [FORMATS](docs/FORMATS.md) | xip, xar, pbzx, cpio-odc, dmg/UDIF |
| [XCODE-ON-LINUX](docs/XCODE-ON-LINUX.md) | the IDE verdict, the project builder, the roadmap |
| [SWIFT](docs/SWIFT.md) | Swift for iOS → Linux: ABI vs runtime, with measurements |
| [COMPATIBILITY](docs/COMPATIBILITY.md) | what runs, where, and how it was measured |
| [COMPAT](docs/COMPAT.md) | one line per translated call + its proving test |
| [DEVIATIONS](docs/DEVIATIONS.md) | every deliberate difference from macOS |
| [TROUBLESHOOTING](docs/TROUBLESHOOTING.md) | symptom → cause → fix |
| [REPRODUCING](docs/REPRODUCING.md) | the full recipe with budgets |
| [INVESTIGATION](docs/INVESTIGATION.md) | the reverse-engineering log |
| [GLOSSARY](docs/GLOSSARY.md) | xar, pbzx, odc, chained fixups, TLV, … |

## Build and setup

```sh
./bootstrap.sh --install   # optional: install host build/test tools (Alpine/Debian)
./bootstrap.sh --check
make
make test
```

The loader build itself is C plus one assembly file; Mach-O fixture tests also
need LLVM `clang`, `llvm-mc`, and `ld64.lld`. Python utilities use only the
standard library. No SDK, Xcode download, or optional guest runtime libraries
are needed for the default build/test. See [dependency and setup details](docs/DEPENDENCIES.md)
and the [configuration guide](docs/CONFIGURATION.md). `src/plat_win32.c` is the platform seam for Windows (see [ARCHITECTURE](docs/ARCHITECTURE.md)).

## Honesty

Every number in the docs is measured on this device and marked as such.
Unverified things are labelled: the Windows build has never been compiled, no
macOS host existed for differential testing, and two tools (`llvm-profdata`,
`swift-demangle`) bind completely and then trap in LLVM's own initialiser —
binding is not running.

The companion tree `workspace/xcode-re/` holds the audit trail: skill-shaped
reports, the 167,522-entry inventory, dependency classification, behavioural
specs and an 11-test suite.
