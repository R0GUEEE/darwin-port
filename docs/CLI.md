# CLI reference

Every tool, every flag, every environment variable. Exit codes are part of the
contract; scripts in this project depend on them.

---

## `bin/darwinld` — run a Darwin arm64 binary

```
darwinld <mode> <macho> [args…]
```

| mode | does | exit |
|---|---|---|
| `info` | prints segments (vmaddr/vmsize/fileoff/filesize/prot), dylib list, fixup kind, export size, cryptid | 0 |
| `dylibs` | resolves and maps the whole dependency graph, lists the images, stops before binding | 0 |
| `check` | maps, binds, prints every unresolved symbol to stderr | 0 clean, 1 dirty |
| `report` | same as check but emits JSON per image and per import | 0 clean, 1 dirty |
| `run` | binds and executes; arguments after `<macho>` go to the guest | guest's exit code |

Exit codes: `0` ok · `1` load/map/bind failure · `2` usage · `3` FairPlay
encrypted · `4` unresolved imports (nothing is executed).

```sh
darwinld info   /path/tool
darwinld check  /path/tool          # reads: "unresolved N" + MISSING lines
darwinld run    /path/ar -t libfoo.a
darwinld report /path/tool > census.json
```

Loader diagnostics go to stderr with a `[darwinld]` prefix; stdout belongs to
the guest.

Environment:

| var | meaning |
|---|---|
| `DS_SYSROOT` | prefix applied to absolute install names |
| `DS_LIB_PATH` | colon-separated dylib search path |
| `DS_MISSING_FILE` | write unresolved names to this file instead of stderr |
| `DS_TRACE=1` | log every binding with its lib ordinal and resolved address |
| `DS_TRACE_FMT=1` | log `__sprintf_chk` format strings and results |
| `DS_TRACE_XC=1` | log `xcselect_*` calls |
| `DEVELOPER_DIR` | returned by `xcselect_get_developer_dir_path` |

---

## `bin/xclang` — compile for macOS

```
xclang [clang options] file.c -o out
```

Drives the host `clang` with `-target $DARWIN_TARGET`, `-isysroot $DARWIN_SDK`,
framework search paths inside the SDK, and `-fuse-ld=lld`.

| var | default | meaning |
|---|---|---|
| `DARWIN_SDK` | `/opt/xcode/MacOSX.sdk` | the extracted SDK |
| `DARWIN_TARGET` | `arm64-apple-macos15.0` | clang triple |
| `DARWIN_MINOS` | `15.0` | passed to `-platform_version` |
| `HOST_CLANG` | `clang` | the compiler to drive |

```sh
DARWIN_SDK=/root/xc/.../MacOSX.sdk xclang -O2 -o hello hello.c
DARWIN_TARGET=x86_64-apple-macos13.0 xclang -c foo.c      # other arch
```

Exit codes are clang's.

---

## `sdk_api.py` — inventory an SDK's API surface

```
sdk_api.py <SDK dir> [-o out.json] [--top N]
```

Counts per `.tbd` stub: C symbols, Objective-C classes and ivars, weak symbols,
reexported libraries, target triples, Swift ABI version. Builds the reexport
graph. Symlinked stubs are deduplicated by realpath.

```sh
sdk_api.py MacOSX.sdk -o sdk-api.json
# 882 stub libraries · 590,795 C symbols · 24,183 ObjC classes · 226 reexport edges
```

---

## `pxcodebuild.py` — build an Xcode project

```
pxcodebuild.py build <proj.xcodeproj|project.pbxproj> [--target NAME] [--sdk PATH] [--out DIR] [-v]
pxcodebuild.py list  <proj> [--target NAME]
pxcodebuild.py dump  <proj> [--target NAME]          # resolved plan as JSON
```

Parses `project.pbxproj` (OpenStep plist), compiles every source with `xclang`
against the extracted SDK, links with `ld64.lld`, and assembles
`<PRODUCT_NAME>.app/Contents/{MacOS,Resources,Info.plist}`.

| flag | meaning |
|---|---|
| `--target NAME` | build one target (default: all) |
| `--sdk PATH` | macOS SDK; falls back to `$DARWIN_SDK` |
| `--out DIR` | output directory (default `<project>/build`) |
| `-v` | echo every compiler and linker command |

Exit `0` built, `1` error, `2` usage. Honours `PRODUCT_NAME`,
`PRODUCT_BUNDLE_IDENTIFIER`, `GCC_PREPROCESSOR_DEFINITIONS`,
`HEADER_SEARCH_PATHS`, `FRAMEWORK_SEARCH_PATHS`, `INFOPLIST_FILE`, and `$()`
substitution. Not yet: Swift, asset catalogs, storyboards, signing, target
dependencies, `.xcconfig`.

```sh
pxcodebuild.py build Demo.xcodeproj --sdk $SDK
darwinld run build/Demo.app/Contents/MacOS/Demo
```

---

## `verify_xip.py` — prove an `.xip` is Apple's

```
verify_xip.py <file.xip>
```

Checks, in order: TOC SHA-1 · RSA signature over the TOC with the embedded leaf
certificate · the chain leaf→intermediate→Apple Root CA · and the archive's own
`sha1(Content)` against the TOC. Prints a verdict; exit 0 only if all pass.

```
TOC sha1        MATCH (4868197f…c81c)
RSA(TOC)        VERIFIED   cert0 ← cert1 ← cert2 (Apple Root CA)
sha1(Content)   MATCH
VERDICT: authentic — signed by Apple and internally consistent
```

Needs `openssl` for the signature steps; the hashes are stdlib-only. 14 s for a
2 GB XIP.

---

## `sdk_api.py` — inventory an SDK's API surface

```
sdk_api.py <SDK-dir> [-o out.json] [--top N]
```

Parses every `.tbd`, counting C symbols, ObjC classes/ivars, weak symbols and
reexported libraries, de-duplicating symlinked framework stubs, and building the
reexport graph. No binaries involved.

```
882 stub libraries · 590,795 C symbols · 24,183 ObjC classes · 226 reexport edges
```

---

## `xipx.py` — expand an `.xip`

```
xipx.py list    <xip> [-o manifest.json]
xipx.py extract <xip> <dest> [--include PREFIX]… [--pattern GLOB]… [--exclude SUBSTR]… [-o manifest.json]
```

| flag | meaning |
|---|---|
| `--include PREFIX` | extract entries whose path starts with `PREFIX` (repeatable) |
| `--pattern GLOB` | extract entries matching a shell glob, e.g. `*/SDKs/*/usr/include*` (repeatable) |
| `--exclude SUBSTR` | drop matches containing `SUBSTR` (applied after the above) |
| `-o manifest.json` | write `[{path, mode, size}]` for **every** entry in the archive |

Behaviour: single streaming pass over the 10.4 GiB content; memory stays at one
16 MiB chunk; symlinks, modes and directories are recreated; hard links land as
separate copies. `list` writes the manifest without extracting anything.

```sh
xipx.py list Xcode.xip -o all.json
xipx.py extract Xcode.xip /root/xc \
    --include 'Xcode-beta.app/Contents/Developer/Toolchains/XcodeDefault.xctoolchain/usr/bin'
```

Notes: paths are printed relative to the archive root and keep the
`Xcode-beta.app/` prefix; the `.xip` member offsets are heap-relative, which the
tool handles (see [FORMATS](FORMATS.md)).

---

## `dmgx.py` — decompress a `.dmg` to a raw HFS image

```
dmgx.py <dmg> <out.raw> [name-substring]
```

Without the third argument every block map is written (MBR, GPT, HFS). Pass
`Apple_HFS` to get just the volume. Prints progress to stderr and the byte count
to stdout.

```sh
dmgx.py CLT.dmg clt.hfs Apple_HFS     # 665 MB volume from a 502 MB dmg
7z l clt.hfs                          # list; p7zip reads HFS+
```

Only UDIF with `koly`, zlib/bzip2/raw/ignore runs is supported — which covers
every Apple `.dmg` seen. It does not support APFS containers (nothing in this
project needed one).

---

## `xarx.py` — read a xar (`.pkg`, `.xip`)

```
xarx.py <archive> list
xarx.py <archive> cat   <member>      # decoded (gzip/bzip2/xz)
xarx.py <archive> raw   <member>      # bytes as stored
```

Note the argument order: the archive comes first. `list` prints
`size  encoding  path` per member.

---

## `pkgx.py` — extract one `.pkg` payload

```
pkgx.py <pkg> <member> <dest>
```

Streams the named payload (usually `<Sub>.pkg/Payload`) through pbzx and unpacks
the cpio into `dest`, preserving modes and symlinks.

```sh
pkgx.py "Command Line Tools.pkg" CLTools_Executables.pkg/Payload /root/clt
```

---

## `inventory.py` — Mach-O census of a tree

```
inventory.py <dir> [-o out.json] [--top N]
```

Walks the tree, identifies fat/thin Mach-O, and reports per image: CPU
architecture, file type, `LC_LOAD_DYLIB` names, symbol count, fixup kind. Prints
architecture and file-type histograms, the most depended-on dylibs, and the
executables with the fewest dependencies.

```sh
inventory.py /root/xc -o inv.json --top 10
```

---

## `hdrmerge.py` — repair `NULLcanary` SDK headers

```
hdrmerge.py <harvest-root> <target-sdk>
```

Apple's xip stores one real copy of each duplicated file and replaces the rest
with the literal bytes `NULLcanary` + zeros. Given a tree that contains several
SDKs (extract with `--pattern '*/SDKs/*/usr/include*'`), this copies the first
real twin over every placeholder in `<target-sdk>`.

Prints `patched N, already real M, unresolved K`. Unresolved entries are named.

---

## `build_reports.py` (in `xcode-re/scripts`) — generate the report set

```
build_reports.py
```

No arguments; reads the manifests and inventory by absolute path and writes
`reports/inventory.jsonl.gz`, `reports/dependencies.json`,
`reports/acquisition.json`. Idempotent.

---

## `tests/run.sh` (in `xcode-re`) — the compatibility suite

```
sh tests/run.sh
```

Builds the loader from `ports/darwin-port`, compiles fixtures with `xclang` and
`llvm-mc`, runs nine behavioural tests plus an optional real-toolchain pair.
`APPLE_BIN=<toolchain>/usr/bin` enables the last tests; they skip cleanly
otherwise. Exit 0 only if every test passes.

---

## Quick combinations

```sh
# does this binary have a chance?
darwinld report tool | grep -c '"unresolved"' 

# what does it need that we do not have?
DS_MISSING_FILE=/tmp/m darwinld check tool >/dev/null; sort -u /tmp/m

# which binaries in a tree are closest to running?
inventory.py tree -o inv.json && darwinld check <candidate>

# build and run a macOS binary from source, on Linux
DARWIN_SDK=$SDK xclang -O2 -o x x.c && darwinld run x
```

---

## `iosbuild.py` — build an unsigned iOS app

```
iosbuild.py plan    <dir|ipa.json> [--sdk PATH] [--json]
iosbuild.py build   <dir|ipa.json> [--sdk PATH] [--out DIR] [-v]
iosbuild.py ipa     <dir|ipa.json> [--sdk PATH] [--out DIR] [--name NAME]
iosbuild.py inspect <file.ipa|File.app> [--json]
```

| command | does | exit |
|---|---|---|
| `plan` | resolves the manifest and SDK, prints sources, frameworks, compile flags, overlay path | 0 |
| `build` | compiles and links a thin arm64 `platform IOS` executable | 0 built |
| `ipa` | the above, then validates the bundle and writes `Payload/<Name>.app` as an `.ipa`; prints `{ipa, sha256, size}` JSON | 0 packaged |
| `inspect` | reports an IPA/app: bundle id, executable arch/platform/minos, imports, validation problems | 0 clean, 1 problems |

Exit codes: `0` ok · `1` build/validation failure · `2` usage/manifest
error · `3` missing SDK or `ld64.lld`.

Environment: `IPHONEOS_SDK` (same as `--sdk`), `XCLANG` (for `pxcodebuild`).

```sh
python3 iosbuild.py ipa examples/winprobe --out /tmp/out
python3 iosbuild.py inspect /tmp/out/WindowProbe.ipa
```

Full contract, including what it refuses to do and why: [IPA](IPA.md).
