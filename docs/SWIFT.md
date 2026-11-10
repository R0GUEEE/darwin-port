# Swift for iOS → Linux

Reverse engineering the Swift half of the Xcode toolchain: what is in the
archive, what is not, and what that means for running or building Swift off
Apple hardware.

## Summary

| question | answer | evidence |
|---|---|---|
| Is the Swift **compiler** in the XIP? | yes, `swift-frontend`/`swiftc` (Mach-O, arm64) | 35 of 105 toolchain binaries import Swift |
| Can it run here? | no — 34,011 unresolved imports | `darwinld check` |
| Is the Swift **runtime** in the XIP? | **no** — not one `libswiftCore.dylib` for `iphoneos` or `macosx` | manifest query over all 167,522 entries |
| Is the runtime **ABI** in the XIP? | **yes, completely** — 94 stub libraries per iOS SDK, 104 for macOS | `*.tbd` under `usr/lib/swift/` |
| Is that ABI portable? | yes — Swift has a stable ABI (`swift-abi-version: 7`) | `libswiftCore.tbd` header |

The consequence is the important part: **Xcode gives you the Swift ABI, not the
Swift runtime.** Anything that runs Swift off Apple hardware must get the
runtime implementation from the open-source toolchain (swift.org), and the
mangled names will match because the ABI is stable.

## What is actually in the archive

The Swift libraries are shipped as stub libraries, one file per library per
platform:

| SDK | Swift stub libraries |
|---|---|
| `iPhoneOS.sdk` | **94** |
| `iPhoneSimulator.sdk` | 94 |
| `MacOSX.sdk` | **104** |
| `XROS.sdk` / `XRSimulator.sdk` | 89 |
| `AppleTVOS` / `AppleTVSimulator` | 78 |
| `WatchOS` / `WatchSimulator` | 74 |
| `DriverKit.sdk` | 5 |

The iOS set is a strict subset of the macOS set — the ten macOS-only libraries
are the platform frameworks: `libswiftAppKit`, `libswiftIOKit`,
`libswiftOpenCL`, `libswiftCreateML`, `libswiftCoreMediaIO`,
`libswiftQuickLookUI`, and four more.

`libswiftCore.tbd` alone describes **14,853 exported symbols**, and carries the
contract in its header:

```
install-name:      '/usr/lib/swift/libswiftCore.dylib'
swift-abi-version: 7
```

That version number is why this is tractable at all: Swift's ABI is stable, so
`_$sSS5countSivg` (`String.count`) names the same function with the same layout
on every platform, including Linux.

## The measurement that matters

Every import of every toolchain binary, classified (`swift_abi.py`):

```
images scanned                     105
images that import Swift            35
import references                 swift 60,455 · C++ 14,991 · libc 14,043
                                  objc 788 · CoreFoundation 46
distinct Swift symbols              7,663
  …described by the SDK stubs       2,364  (30.8%)
  …compiler-internal (no stub)      5,299  (69.2%)
```

Two conclusions fall out of those numbers:

1. **The stdlib ABI is fully described** — 70,963 symbols across the 94 stub
   libraries, which is everything a normal app links against.
2. **Most of what the *compiler* imports is not** — `lib_CompilerSwiftSyntax*`,
   `libSwiftScan`, `lib_CompilerSwiftIDEUtils` and friends are real dylibs
   inside `XcodeDefault.xctoolchain/usr/lib/swift/host`, shipped without stubs.
   They are Apple's build of the open-source SwiftSyntax package, not part of
   the SDK.

## Why "Swift for iOS on Linux" is not one problem

| goal | what it needs | status |
|---|---|---|
| compile Swift for iOS/macOS on Linux | a Swift compiler that runs on Linux + the SDK's stub libraries | **feasible** — the compiler is open source; only the `.tbd` files come from Xcode |
| run a Swift binary on Linux | the runtime implementation, plus its Darwin substrate | **partial** — see below |
| run a Swift **iOS app** on Linux | all of the above + UIKit/Foundation/CoreFoundation + a device | no |

### What a Swift binary actually imports

A Swift guest is not only Swift. Across the toolchain's 35 Swift binaries the
non-Swift imports are the tell:

```
788  objc_* / _OBJC_CLASS_$_…      Objective-C runtime
 46  ___CFConstantStringClassReference
```

Those 834 symbols are the interop layer: Swift on Apple platforms uses the ObjC
runtime for class metadata and for `@objc` members, and CoreFoundation for
string literals. A pure-Swift program that never touches Foundation keeps that
tail small — which is why the honest statement is "a pure-Swift hello world has
a chance; anything importing Foundation does not".

## The path that works

```
   swift.org toolchain (Linux, arm64)          — compiler + libswiftCore.so
                │
                ├─ compile:  swiftc -target arm64-apple-ios15.0 -sdk iPhoneOS.sdk
                │            links against the SDK's 94 stub libraries
                ▼
        Mach-O arm64 object / executable         — inspectable with darwinld
                │
                └─ run on Linux:  libswiftCore.so provides the $s… symbols
                                  (ABI-stable names), ObjC/CF must be absent
```

Three steps, in order of certainty:

1. **Cross-compile.** Upstream Swift can emit `arm64-apple-ios` given the SDK.
   The `.tbd` files this project extracted are exactly what the linker needs.
   This is the useful half for CI and it needs no Apple code beyond headers and
   stubs the user already owns.
2. **Link against the Linux runtime for execution.** Because `libswiftCore.so`
   exports the same mangled names, `darwinld` can `dlopen` it and satisfy a
   Darwin guest's Swift imports by name. This is untested here only because the
   runtime implementation is not in the XIP — the piece is a download, not a
   reverse-engineering problem.
3. **Drop the ObjC tail.** A guest that imports `objc_msgSend` or
   `__CFConstantString` still needs those runtimes. Pure Swift does not.

## What would have to be built for full support

| piece | work |
|---|---|
| Swift section walk | parse `__swift5_types`, `__swift5_protocols`, `__swift5_protocol_conformances`, `__swift5_fieldmd` and hand the records to the runtime's registration hook (`swift_registerTypeMetadataRecords`) |
| concurrency | `libswift_Concurrency.dylib` brings its own thread pool over `pthread`; it needs `__swift_task_*` entry points from the runtime, not the loader |
| runtime version pinning | a guest built for Swift 6.2 needs a runtime of the same ABI version; the `.tbd` header states it |
| ObjC | out of scope for this project — same wall as [COMPATIBILITY](COMPATIBILITY.md) describes |

The loader needs almost no new code for step 2: it already loads guest dylibs
and binds by ordinal. What it would add is the section walk above, and that is
only meaningful once a runtime is present.

## Method

```sh
# ABI surface of the runtime, straight from the SDK
python3 swift_abi.py "MacOSX.sdk/usr/lib/swift/*.tbd" <toolchain>/usr/bin
#→ distinct Swift symbols 7,663 · in SDK stubs 2,364 · compiler-internal 5,299

# what a single binary needs
darwinld report <toolchain>/usr/bin/swift-frontend | head
```

`swift_abi.py` classifies every import as swift / objc / corefoundation / c++ /
libc and reports coverage against the stub libraries. It reads chained fixups
directly, so it works on any arm64 Mach-O without running it.
