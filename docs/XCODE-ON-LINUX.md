# Xcode on Linux — what converts, what does not

"Convert Xcode to Linux" is three different projects. Only one of them is a
port problem; one is already done; one is a multi-year runtime project.

| track | what it is | status |
|---|---|---|
| **C. the toolchain** | clang, ld, the SDK | **done** — `xclang` builds Mach-O with the real SDK |
| **B. the build system** | project → `.app` | **done** — `pxcodebuild`, see below |
| **A. the IDE** | `Xcode.app` itself | **not a port** — see the numbers |

---

## Track B — building Xcode projects without Xcode

`pxcodebuild.py` reads a real `project.pbxproj` and produces a real `.app`:

```
$ python3 pxcodebuild.py list Demo.xcodeproj
Demo  (application)
   src   src/main.c
   res   src/notes.txt

$ python3 pxcodebuild.py build Demo.xcodeproj --sdk .../MacOSX.sdk
[macosx] Demo → /tmp/Demo/build/Demo.app
  1 objects, 33536 bytes binary

$ find Demo.app
Demo.app/Contents/MacOS/Demo            # Mach-O 64-bit arm64 executable
Demo.app/Contents/Info.plist
Demo.app/Contents/Resources/notes.txt

$ darwinld run Demo.app/Contents/MacOS/Demo ARG1
built by pxcodebuild: argc=2 v=2479
```

What it implements:

| part of the format | handled |
|---|---|
| OpenStep plist parsing (comments, quoted strings, data, nested dicts/arrays) | yes |
| `PBXProject` → `targets` → `PBXNativeTarget` | yes |
| `PBXSourcesBuildPhase` / `PBXFrameworksBuildPhase` / `PBXResourcesBuildPhase` | yes |
| `PBXFileReference` paths through `PBXGroup` chains (both path conventions) | yes |
| `XCBuildConfiguration.buildSettings`: `PRODUCT_NAME`, `PRODUCT_BUNDLE_IDENTIFIER`, `GCC_PREPROCESSOR_DEFINITIONS`, `HEADER_SEARCH_PATHS`, `FRAMEWORK_SEARCH_PATHS`, `INFOPLIST_FILE` | yes |
| `$()` / `${}` variable substitution | yes |
| `.app` bundle layout + generated `Info.plist` when none is supplied | yes |
| Objective-C sources (`-fobjc-arc`) | compiles; linking needs the ObjC runtime |
| Swift sources | not yet (needs the Swift toolchain) |
| targets with dependencies, `.xcconfig` inheritance, run-script phases, asset catalogs, `.storyboard` compilation, code signing | not yet |

The build half is the half that matters for CI: a project that Xcode builds,
built on Linux, with the same bundle shape and a binary that runs.

---

## Track A — the IDE, by the numbers

Measured from the XIP's own file table (every entry, not a sample):

| part of `Xcode-beta.app` | files | size |
|---|---|---|
| `Contents/Developer` (toolchains, platforms, SDKs, templates) | 152,743 | 7.68 GB |
| `Contents/SharedFrameworks` | 6,448 | 974 MB |
| `Contents/PlugIns` | 3,979 | 338 MB |
| `Contents/Frameworks` | 1,510 | 142 MB |
| `Contents/Applications` | 1,642 | 142 MB |
| `Contents/Resources` | 17 | 519 MB |
| `Contents/MacOS/Xcode` (the launcher) | 1 | **80,592 B** |

* **559 distinct frameworks** across `Frameworks/` and `SharedFrameworks/`.
* **46 XPC services.**
* The 80 KB `Xcode` binary is not the IDE — it is a launcher that loads
  `DVTFoundation`, `IDEKit`, `IDEFoundation` and the plug-in bundles.

### Why this is not a shim job

Each of these is a different class of problem, and the measurements in this
project already show the small version of the big one:

| dependency | evidence class | why |
|---|---|---|
| **ObjC runtime** (`libobjc.A.dylib`, 1,084 `__objc_empty_cache` in the *CLI* tools alone) | measured | classes are data structures the runtime rewrites; message sends go through the cache |
| **CoreFoundation** (3,019 `___CFConstantStringClassReference`) | measured | constant strings are objects created at start-up; the class pointer is written by CF |
| **AppKit / SwiftUI / Metal** | — | window server, GPU, CoreAnimation; nothing to shim against on Linux |
| **XPC** (46 services) | — | mach ports + `launchd`; the IDE's whole architecture is XPC calls between processes |
| **Plug-in model** (3,979 files) | — | `dyld` loading ObjC bundles with `+load` ordering |
| **Signing / notarisation** | — | Apple CA, private keys, online services |

The CLI-tool measurement is the honest scale marker: **a single C++ tool that
imports nothing but libc++ and libSystem is already at the edge of what a shim
can do.** Now multiply by 559 frameworks whose symbols are objects, blocks,
classes and Swift metadata.

### Verdict

Running `Xcode.app` on Linux is not a port, it is a **Darling-class userland**
(macOS `dyld`, ObjC runtime, Foundation/CoreFoundation, XPC, a window server
bridge) — person-years, and it would still have no Metal. The realistic options
are exactly three:

1. **macOS VM** (KVM on Apple silicon, or a rented Mac) — runs Xcode unmodified.
2. **Remote build host** — keep Xcode on a Mac, drive it from Linux over SSH/CI.
3. **Headless replacement** — what this project does: SDK + clang + ld + a
   project driver. You lose the IDE, not the build.

---

## What a Linux user actually gets today

| need | on Linux |
|---|---|
| compile C/C++/ObjC for macOS | **yes** — `xclang` + real SDK + `ld64.lld` |
| link dylibs and executables | **yes** |
| build a `.app` from a `.pbxproj` | **yes** — `pxcodebuild` (C/C++/ObjC; no Swift yet) |
| inspect Mach-O | **yes** — `darwinld`, plus Xcode's own `otool`/`nm`/`size` running under it |
| run the result | **yes** on Linux arm64 via `darwinld`; on a Mac natively |
| Swift | not yet — needs swift.org's Linux toolchain, which does exist upstream |
| asset catalogs (`actool`) | no — `.car` format is proprietary; community tools exist |
| Interface Builder / storyboards | no — `ibtool` is proprietary |
| codesign / notarise | no — but `ldid` signs ad-hoc for development |
| iOS deploy / simulator | no — device services and simulator runtimes are macOS-only |

---

## If someone wants to push further

Ordered by value per unit of work:

1. **Swift.** Use the upstream Linux Swift toolchain to emit `arm64-apple-macos`
   objects and link them with `ld64.lld` against the extracted SDK's
   `libswiftCore.tbd`. This is the biggest missing piece for real projects, and
   it does not require running any Apple binary.
2. **`pxcodebuild` depth**: target dependencies, `.xcconfig` inheritance,
   multiple configurations, run-script phases, incremental builds. All format
   work, all testable without macOS.
3. **Asset catalogs**: parse `Assets.xcassets` and emit a `.car`, or fall back to
   copying PNGs — enough for many apps.
4. **Ad-hoc signing** with `ldid` so the produced binaries can be installed on a
   development device.
5. **Only then** consider the IDE — and the honest first step there is not a
   port but a headless `xcodebuild`-compatible CLI plus a separate editor.

Everything in 1–4 is ordinary engineering with tests. Track A is not, and
saying otherwise would be the one thing this project has consistently refused
to do.
