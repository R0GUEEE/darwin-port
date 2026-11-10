# Investigation log

What was done, in order, with the measurement that decided each step. Numbers
are from this device; the raw artifacts are in `workspace/xcode-re/reports/`.

## Scope and provenance

| file | bytes | sha256 | verdict |
|---|---|---|---|
| `Xcode_27.3_beta.xip` | 2,075,401,264 | `7263cc66…15cb58` | **authentic** |
| `Command_Line_Tools_27.3_beta.dmg` | 526,624,768 | `1288aa0c…1c430` | **damaged** |

The XIP verifies against itself: `sha1(Content) = 53a622bf…5a80` equals the
XAR TOC's member checksum, and the TOC carries an RSA signature by *Apple
Software Update Certification Authority*. That single check is what makes
everything after it meaningful.

The DMG fails four independent ways: koly data-fork CRC32 `0xd5ca3f29` vs stored
`0x9e458a9e`; four payloads fail their SHA-1; the decompressed HFS+ volume is
95.1 % zeros with 121 MB hole runs. Its 60 small members do verify — which is
exactly how a damaged archive passes a casual look. It was abandoned; the XIP
carries the same toolchain and every SDK.

## Phase A — containers

The chain is `xar → pbzx → cpio-odc` for the XIP and
`koly → blkx → HFS+ → xar(pkg) → pbzx → cpio-odc` for the DMG. Both were
implemented from the bytes, not from `xar(1)` or `7z`, because neither handles
pbzx or odc correctly.

Three measurements that cost a full pass each:

1. **Heap-relative offsets.** `Content` is at TOC offset 276 *inside the heap*,
   i.e. file offset 4213. Read at 276 and the decompressor finds a stray XZ
   header inside the RSA signature and emits 9 MiB of plausible garbage that
   parses as a 46,617-entry cpio. The extractor now asserts that the chunk table
   lands exactly on the end of the member.
2. **Stored pbzx chunks.** When `comp_size == out_size` the chunk is raw. A
   magic-scanning reader silently stops at ~1.5 GiB — two extraction passes were
   lost to this before the chunk table was honoured.
3. **`NULLcanary`.** 5,955 of the macOS SDK's ~6,000 headers are the literal
   string `NULLcanary` plus zeros: Apple stores one real copy of each duplicated
   file across the ten SDKs. Repairing them from twin SDKs patched 846 headers
   in the macOS SDK, with 1 unresolved (`hfs/hfs_format.h`).

## Phase B — inventory

167,522 cpio entries, 9.68 GiB of file data, ten platform SDKs.
Extracted subset: 29,955 files, 138 Mach-O images (155 arm64 slices, 25 x86_64,
2 arm64_32), 131 of them with dependencies, 120 distinct dylibs.

Two facts shaped everything downstream:

* **0 of 138 images are FairPlay-encrypted.** Developer toolchain binaries are
  not App Store binaries; there is no per-device key to defeat.
* **131 of 138 depend on `libSystem.B.dylib`.** A shim is not optional.

Dependency classes: Swift runtime 52, Apple frameworks 40, LLVM 17, libc++ 2,
libSystem 1, misc 9.

## Phase C — execution, one crash at a time

A loader was built stage by stage; each stage was discovered by a specific
failure:

| symptom | cause | fix |
|---|---|---|
| `pointer_format 0`, 16 TB `segment_offset` | two `dyld_chained_starts_in_segment` dialects (dyld 28+2n vs LLD 22+2n) | discriminate on `size == 22 + 2·u16@20` |
| plausible but wrong symbol names, then SIGSEGV | `dyld_chained_import` packs `name_offset` in the **high** 23 bits (`v >> 9`) | fix the bitfields |
| jump to address `0x3a93` | `DYLD_CHAINED_PTR_64_OFFSET` stores vm *offsets*; the base must be added even at slide 0 | fix rebase |
| `ar` writes 60-byte headers of zeros | Darwin passes anonymous arguments **on the stack**; the host `vsnprintf` read registers | assembly front ends + `fmt.c` |
| crash inside `vfprintf` with `x0` = machine code | `__stdoutp`/`__stderrp` bound to functions instead of `FILE *` variables | export the variables |
| `ar` creates the wrong file | `O_CREAT/O_TRUNC/O_EXCL` differ between the two ABIs | translate flags |
| file sizes are garbage | Darwin `struct stat` is 144 B with `st_size@96` | translate the struct |
| `gm4: sigaltstack: Out of memory` | `stack_t` orders `ss_size`/`ss_flags` differently | translate both directions |
| `m4` refuses to run | Apple's `m4` is a wrapper calling `xcselect_invoke_xcrun("gm4")` | intercept and re-enter the loader |

## Phase D — more than one image

`dso.c` was added: dependency graph, export-trie lookup, reexports, path
resolution, two-level namespace binding. Proven with an executable and dylib
built by `ld64.lld`, then applied to the corpus.

A dedicated bug: a dylib's `__TEXT` has `vmaddr = 0`, and
`mmap(0, …, MAP_FIXED_NOREPLACE)` **succeeds** on this kernel — every segment was
copied to the null page. Any preferred base below `0x10000` now forces the slide
path.

## Phase E — the ABI audit

Four defects were silent corruption rather than errors:

| defect | measured | effect |
|---|---|---|
| `LC_DYLD_INFO` weak binds ignored | `coremlc`, `coremlcompiler` carry 3,083 weak imports | they looked import-free in every earlier census |
| `setjmp` | Darwin `jmp_buf` 192 B, musl writes **312 B** | 120-byte stack smash per call |
| `statfs` | Darwin 2168 B vs Linux 112 B | 2 KB of stack garbage handed to the guest as a filesystem |
| `dirent` | Darwin 1048 B with `d_namlen@18` | wrong names, wrong lengths |

All four are fixed and covered by tests T5/T7/T8 plus the weak-bind check.

## Phase F — the SDK's API surface, without a single binary

The SDK ships its entire API as text (`.tbd`). Parsing all of them gives the
shape of macOS's interface without disassembling anything (`sdk_api.py`):

```
882 stub libraries (after de-duplicating symlinked framework stubs)
  590,795 C symbols
   24,183 Objective-C classes
   10,976 Objective-C ivars
      226 reexport edges between stubs
```

| stub | C symbols | ObjC classes |
|---|---|---|
| `SwiftUICore` | 74,465 | 8 |
| `RealityFoundation` | 41,273 | — |
| `FinanceKit` | 32,179 | — |
| `SwiftUI` | 21,396 | 2 |
| `AppKit` | 9,530 | 847 |
| `PassKit` | 3,839 | 1,828 |
| `Matter` | 0 | 1,518 |

Two observations that matter for any port:

1. **Swift-heavy frameworks carry the symbol mass.** `SwiftUICore` alone
   describes 74k symbols, almost all Swift-mangled, and almost no ObjC classes.
   The API surface of modern macOS is majority-Swift — which is why the Swift
   runtime question ([SWIFT](SWIFT.md)) is not a side quest.
2. **ObjC class counts are concentrated in old frameworks** — PassKit (1,828),
   Matter (1,518), Intents (1,326), AddressBook (946), AppKit (847). Those are
   the ones an ObjC runtime shim would have to carry.

The reexport graph is shallow and predictable:

```
AppKit  → ApplicationServices → CoreServices → CoreFoundation → libobjc
SwiftUI → SwiftUICore → CoreTransferable
Metal   → (leaf)
```

Every framework eventually lands on `libobjc` and `libSystem`, which is exactly
the boundary measured in [COMPATIBILITY](COMPATIBILITY.md): the substrate is
small and named; the mass is in Swift and in the ObjC class-heavy frameworks.

Raw data: `reports/sdk-api.json` (882 stubs, 226 edges).

## Provenance, verified rather than assumed

`verify_xip.py` closes the chain cryptographically — Apple signs the TOC, the
TOC names `sha1(Content)`, the measured content hash matches:

```
TOC sha1        MATCH   (4868197f…c81c)
RSA(TOC)        VERIFIED with the leaf key
  cert0 ← cert1 ← cert2   (Software Update ← Apple Software Update CA ← Apple Root CA)
sha1(Content)   MATCH
VERDICT: authentic — signed by Apple and internally consistent
```

## Where it stands

```
executables checked : 76  (< 12 MB in the toolchain)
fully bind          : 40
verified running    : ~20   (ar, otool, nm, size, strings, ctags, bison,
                             flex, m4/gm4, gperf, clang-format, …)
distinct unresolved : 3,256 — 3,019 CoreFoundation, 1,084 ObjC, 904 Swift
```

Cross-compilation closes the loop: a C program using `stdio`, `stdlib`, `math`
and `qsort` compiles against the real 27.3 SDK with host clang + `ld64.lld`,
and the resulting Mach-O runs through the same loader.

## What was not done, and why

* No macOS host existed, so no differential test against real Xcode behaviour:
  fidelity claims stop at stdout, exit status and produced files.
* The Windows backend is written and reviewable but never compiled — no mingw,
  no Windows host.
* Objective-C, Swift and CoreFoundation are not attempted: their symbols are
  data structures the code walks, and satisfying them needs the runtime images
  loaded and registered.
* `libLTO.dylib` and `libclang-cpp.dylib` were never extracted (the pass over
  `usr/lib/*` pulled 6 GB and was killed twice), so the multi-image loader is
  proven against dylibs built here, not against those two.

## Evidence index

| claim | artifact |
|---|---|
| XIP authenticity | `xcode-re/reports/acquisition.json` |
| file inventory + hashes | `xcode-re/reports/inventory.jsonl.gz`, `inventory-extracted.jsonl` |
| dependency graph | `xcode-re/reports/dependencies.json` |
| bind census | `xcode-re/reports/compatibility-matrix.md`, `evidence/tests.log` |
| ABI contracts | [ABI](ABI.md), `specs/macho-arm64-loader.md` |
| per-call proofs | [COMPAT](COMPAT.md) |
| deviations | [DEVIATIONS](DEVIATIONS.md) |
| detailed write-ups | [REPORT](REPORT.md), [DEEP-DIVE](DEEP-DIVE.md), [FORMATS](FORMATS.md) |
