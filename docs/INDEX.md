# Documentation index

This is the full documentation set for **darwin-port**: extracting Apple's Xcode
archives byte-by-byte, and running the Darwin toolchain they contain on Linux.

Two trees exist and they are not duplicates:

| tree | what it is |
|---|---|
| `workspace/darwin-port/` | the project: source, binaries, these docs |
| `workspace/xcode-re/` | the audit trail: skill-shaped reports, inventory, specs, tests |

## Read it by role

**I want to know what happens to Swift**
→ [SWIFT](SWIFT.md) → `specs/swift-runtime.md`

**I want to build an Xcode project on Linux**
→ [XCODE-ON-LINUX](XCODE-ON-LINUX.md) → [CLI §pxcodebuild](CLI.md#pxcodebuildpy--build-an-xcodeproj)

**I want to run a macOS binary on Linux**
→ [QUICKSTART](QUICKSTART.md) → [CLI](CLI.md) → [TROUBLESHOOTING](TROUBLESHOOTING.md)

**I want to compile macOS binaries on Linux**
→ [QUICKSTART §4](QUICKSTART.md#4-cross-compile-for-macos) → [ABI](ABI.md) → [CLI §xclang](CLI.md#xclang)

**I want to know whether tool *X* works**
→ [COMPATIBILITY](COMPATIBILITY.md) (matrix) → [COMPAT](COMPAT.md) (per call)

**I am porting my own loader / compat layer**
→ [ARCHITECTURE](ARCHITECTURE.md) → [DARWINLD](DARWINLD.md) → [ABI](ABI.md) → [FORMATS](FORMATS.md)

**I want the evidence behind the claims**
→ [INVESTIGATION](INVESTIGATION.md) → `xcode-re/reports/`

**Something is broken**
→ [TROUBLESHOOTING](TROUBLESHOOTING.md) → [DEVIATIONS](DEVIATIONS.md)

## Documents

| doc | answers |
|---|---|
| [QUICKSTART](QUICKSTART.md) | bootstrap, build, run, and cross-compile |
| [DEPENDENCIES](DEPENDENCIES.md) | required/optional host tools and setup rationale |
| [CONFIGURATION](CONFIGURATION.md) | profiles, environment template, SDK and runtime paths |
| [ARCHITECTURE](ARCHITECTURE.md) | how the pieces fit, and why a loader and not a converter |
| [DARWINLD](DARWINLD.md) | loader internals: mapping, fixups, binding, entry, diagnostics |
| [ABI](ABI.md) | Darwin vs Linux, structure by structure, with offsets and flags |
| [CLI](CLI.md) | every tool, every flag, every environment variable |
| [FORMATS](FORMATS.md) | xip, xar, pbzx, cpio-odc, dmg/UDIF, pkg — with byte offsets |
| [COMPATIBILITY](COMPATIBILITY.md) | what runs where, and how that was measured |
| [COMPAT](COMPAT.md) | one line per translated call, naming the test that proves it |
| [DEVIATIONS](DEVIATIONS.md) | every deliberate difference from macOS |
| [TROUBLESHOOTING](TROUBLESHOOTING.md) | symptom → cause → fix, from failures actually seen |
| [REPRODUCING](REPRODUCING.md) | the end-to-end recipe, with timings and sizes |
| [INVESTIGATION](INVESTIGATION.md) | the reverse-engineering narrative and measurements |
| [XCODE-ON-LINUX](XCODE-ON-LINUX.md) | what "port Xcode" means: IDE verdict, the project builder, the roadmap |
| [SWIFT](SWIFT.md) | what Swift-for-iOS means off Apple: ABI vs runtime, measured |
| [GLOSSARY](GLOSSARY.md) | xar, pbzx, odc, chained fixups, TLV, … |
| [REPORT](REPORT.md) | appendix: first-pass findings, tool-by-tool results |
| [DEEP-DIVE](DEEP-DIVE.md) | appendix: multi-image loading and the coverage census |

## Status in one paragraph

The container chain works end to end and is verified against Apple's own
checksums. 138 Mach-O images were parsed; **40 of 76** toolchain executables
bind completely and about twenty were observed running real work (`ar` writing
archives, `otool` printing Mach headers, `ctags` writing tag files, `bison`,
`flex`, `m4`). Cross-compiling to `arm64-apple-macos` with the extracted SDK
works and its output runs through the same loader. Objective-C, Swift and
CoreFoundation binaries do not run: they need those runtimes loaded as images,
which is a different class of work (see [COMPATIBILITY](COMPATIBILITY.md)).

## Conventions in these docs

* **Measured** means it was observed on this device and is reproducible with the
  commands shown. Numbers in tables come from `xcode-re/reports/`.
* File paths shown as `/root/xc/...` are the extraction used here; yours differ.
* `T1`…`T9` refer to `xcode-re/tests/run.sh`.
* Anything unverified is labelled as such — the Windows build is the main one.
