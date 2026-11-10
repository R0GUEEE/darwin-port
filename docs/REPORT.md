# Reverse engineering Xcode 27.3 beta for non-Apple hosts — results

*Part of the [darwin-port documentation](INDEX.md).*

Scope: `Xcode_27.3_beta.xip` (2,075,401,264 B) and
`Command_Line_Tools_27.3_beta.dmg` (526,624,768 B), both supplied as-is.
Everything below was measured on an iPhone-class ARM64 Linux sandbox
(4 GB RAM, iSH/Alpine), not inferred from documentation.

## 1. Verdict on the two files

| file | integrity | notes |
|---|---|---|
| `Xcode_27.3_beta.xip` | **authentic** | `sha1(Content) = 53a622bf…5a80`, byte-identical to the TOC's `archived-checksum` |
| `Command_Line_Tools_27.3_beta.dmg` | **damaged** | koly CRC32 `0xd5ca3f29` vs stored `0x9e458a9e`; 4 large pkg payloads fail their SHA-1; decompressed HFS image is 95.1 % zeros with 121 MB hole runs |

The DMG is not needed: the XIP contains the same toolchain plus every SDK.
Its 60 small members do verify, which is why it looks healthy at first glance —
only the four `*.pkg/Payload` blobs (350 MB, 66 MB, 56 MB, 49 MB) are holed.

## 2. What is inside the XIP

```
xar (28-byte header, zlib TOC, RSA signature)
└── Content: pbzx, 622 chunks, 10,433,180,672 B uncompressed
    └── cpio "odc" (070707), 167,522 entries, 9.68 GiB of file data
        ├── Xcode-beta.app/Contents/Developer/Toolchains/XcodeDefault.xctoolchain
        ├── .../Platforms/{MacOSX,iPhoneOS,iPhoneSimulator,WatchOS,WatchSimulator,
        │                AppleTVOS,AppleTVSimulator,XROS,XRSimulator,DriverKit}.platform
        ├── .../Applications, Library, Resources
        └── SDKs: 10 platforms, 1,395 .tbd stubs in the macOS SDK alone
```

Extracted for this work: 1.3 GB (macOS SDK, toolchain `usr/bin`, clang resource
dir) + 9.9 GB of SDK `usr/include` across all platforms for header repair.

### 2.1 A trap that cost a full pass

XAR member offsets are relative to the **heap** (`header_size + toc_size` =
3937 here), not to the file. Reading `Content` at the literal offset 276 lands
inside the RSA signature; the decompressor finds a stray XZ header a few KB
later and produces 9 MiB of plausible-looking garbage that parses as a cpio
for 46,617 entries before desynchronising. All of that output was worthless.
The extractor now self-checks: the 622-entry chunk table must land exactly on
the end of the member.

### 2.2 `NULLcanary` — Apple's xip deduplicates files

5,955 of the SDK's ~6,000 headers in this XIP contain the literal bytes
`NULLcanary` followed by zeros, in a file of the correct size. Apple ships one
real copy of each unique file and replaces duplicates across the ten SDKs with
that placeholder. Consequences: `stdint.h` and `sys/_symbol_aliasing.h` are
placeholders in the macOS SDK, so nothing compiles with it as shipped.

`hdrmerge.py` walks all SDKs, keeps the first non-placeholder copy of each
relative path and patches the target SDK: **846 headers repaired, 3,519 real
copies harvested, 1 unresolved** (`hfs/hfs_format.h`). After that the macOS SDK
compiles real programs.

### 2.3 `.tbd` stubs and LLD

844 of the 1,395 stubs declare the target `arm64e.x1-macos`, which LLD 19 does
not parse (`malformed file`). Stripping that token is enough; the arm64-macos
export lists are untouched. With that, `ld64.lld` links against Apple's text
stubs directly.

## 3. The loader

`darwinld` maps an arm64 Mach-O at its preferred base (0x1000_0000_0000
normally succeeds), applies bind/rebase fixups, resolves every import against
the host libc through a shim table, runs `__mod_init_func`, and calls the
`LC_MAIN` entry.

Five ABI facts had to be discovered by crashing into them:

1. **Two chained-fixup dialects.** dyld's `dyld_chained_starts_in_segment` is
   `size, page_size, page_format, max_valid(8), pointer_format, segment_offset(8),
   overflow_count, page_start[]` (size = 28 + 2n). What ld64.lld writes is
   `size, page_size, pointer_format, segment_offset(8), max_valid(4), page_count,
   page_start[]` (size = 22 + 2n). Reading the second with the first's offsets
   yields `pointer_format = 0` and a 16 TB segment_offset. Detect with
   `size == 22 + 2 * u16@20`.
2. **Import bitfields run the other way.** `dyld_chained_import` is
   `lib_ordinal:8, weak:1, name_offset:23` — the name is in the *high* bits
   (`name_offset = v >> 9`). Getting this backwards produces plausible but
   wrong symbol names, then segfaults.
3. **`DYLD_CHAINED_PTR_64_OFFSET` stores vm *offsets*, not addresses.** The
   pointer must have the image base added even when the slide is zero;
   `DYLD_CHAINED_PTR_64` stores absolute vmaddrs. Mixing them is a jump into
   address 0x3a93.
4. **Darwin's `struct stat` is 144 bytes** with `st_mode` at 4, `st_mtime` at 48
   and `st_size` at 96. Handing the guest a Linux `struct stat` corrupts every
   size and turns timestamps into pointers.
5. **Darwin's `open()` flags are not Linux's.** Darwin `O_CREAT=0x200`,
   `O_TRUNC=0x400`, `O_EXCL=0x800`; Linux `O_CREAT=0x40`, `O_APPEND=0x400`.
   A passthrough does not fail — it truncates the wrong file or creates with
   the wrong intent.

### 3.1 The one that makes or breaks printf

Apple's arm64 ABI passes **all anonymous arguments on the stack**, and
`va_list` is a plain pointer to them. AAPCS64 (Linux) passes the first eight in
x0–x7 with a register-save-area `va_list`. Every guest call to `printf`,
`snprintf`, `err`, `warn`, `__sprintf_chk` etc. therefore had to get its own
front end:

```asm
ds_printf_stub:
    mov x1, sp        ; anonymous args are already at sp
    b   ds_printf_c   ; tail call, guest's x30 preserved
```

`fmt.c` is a formatter that consumes that cursor (flags, width, precision,
`*`, length modifiers, `d i u o x X c s p f e g %`). Without it, Xcode's `ar`
wrote 60-byte member headers of pure zeros.

Two smaller traps in the same family: `__stdoutp/__stderrp/__stdinp` are
*variables* holding `FILE *` (binding them to functions makes the guest read
machine code as a FILE), and Darwin mangles availability variants as
`name$FEATURE` (`realpath$DARWIN_EXTSN`), which must be stripped at lookup.

## 4. Results

Images in the extracted subset: 191 Mach-O (153 arm64 slices, 25 x86_64,
2 arm64_32). Dependency census: `libSystem` 154, `libc++.1` 120, `libc++abi`
68, `libobjc.A` 42, `libz` 36, `libswiftCore` 22.

Deeper pass (multi-image loader + signal/mach/xcselect/stdio ABI): 34 of 75
toolchain executables under 12 MB now bind completely. See `DEEP-DIVE.md`.

| binary | unresolved imports | behaviour on Linux |
|---|---|---|
| `ar` | 0 | creates a valid archive and lists it; the produced file is readable by the host `ar` |
| `ctags` | 0 | `-x` cross-reference output is exactly right (`cmp`, `main` with line numbers) |
| `asa` | 0 | runs, exits 0 |
| `unifdef` | 0 | runs, but every option is rejected (getopt path, see below) |
| `bm4`, `rpcgen` | 0 | bind and execute |
| `indent` | 0 | binds; prints an empty `warn()` message before output |
| `otool`, `otool-classic` | 0 | `-h`, `-l` parse real Mach-O headers and load commands |
| `nm-classic`, `size`, `strings` | 0 | symbol table, segment sizes, string table |
| `bison`, `flex`, `m4`, `yacc`, `gm4`, `gperf` | 0 | version banners and real work (m4 → gm4 via the exec interceptor) |
| `clang-format`, `segedit`, `codesign_allocate`, `llvm-size`, `llvm-cxxfilt` | 0 | run |
| `libtool` (=`ranlib`) | 27 | needs libLTO + more libc++ than the host has |
| `clang` | 44 | needs `libclang-cpp.dylib` (a guest dylib) |
| `ld` | 138 | C++/LTO heavy |
| `swiftc` | 34,011 | Swift runtime + all Swift modules |

Cross-compiled and executed through the same loader (the closed loop):

```c
/* built with xclang + the real MacOSX.sdk, run by darwinld */
printf("args=%d first=%s pi=%.4f sorted=%d%d%d%d\n", ...);
→ args=2 first=ARG1 pi=3.1416 sorted=1359
```

## 5. What cannot work here, and why

* **Loading a guest dylib.** Every tool above binds its imports to *host*
  functions. `clang`, `ld`, `libtool` and `swiftc` instead need their own
  `libclang-cpp.dylib`, `libLTO.dylib`, `libswiftCore.dylib` loaded as Mach-O
  images, with cross-image symbol resolution, `__DATA_CONST` protection
  changes and ObjC runtime init. That is the next layer of work, not a shim
  entry.
* **Xcode.app itself.** GUI application: AppKit/SwiftUI, XPC services, the
  Metal layer preview, `XCBuild` over XPC, plus 592 bundled frameworks in a
  typical bundle. Not reachable by an ABI shim; it needs a full Darling-class
  userland (or a macOS VM).
* **Swift runtime.** 34k imports, `@objc` metadata, `libswiftCore` — same
  category.
* `unifdef`'s option parsing still differs from Darwin's `getopt`; the
  no-argument path works, so the loader is not at fault.

## 6. Reproduce

```
xipx.py     → extract            (25 min, 3 GB RAM, streams 10.4 GiB)
hdrmerge.py → repair headers     (4 min)
xclang      → arm64-apple-macos  (real SDK, LLD)
darwinld    → run it here
```

Numbers in this report are from the run on 2026-11-10; the manifest of all
167,522 archive entries is `/root/logs/xcode_manifest.json`.
