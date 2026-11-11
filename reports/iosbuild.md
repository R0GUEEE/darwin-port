# Report: iOS build path — what was measured, not assumed

Phase: 5 (implementation) and 6 (verification) of the Xcode-RE program.
Date: 2026-11-11. Host: Alpine Linux aarch64 inside iSH on an iPhone (4 GB).

## Inputs

| item | value |
|---|---|
| SDK | `/root/iphoneos27.3-compat-sdk-with-headers` (derived from the Xcode 27.3 beta iPhoneOS SDK; see `acquisition.json` for the archive hash) |
| SDKSettings | Version 27.3, CanonicalName `iphoneos27.3`, Archs `arm64e, arm64, arm64e.x1` |
| framework stubs | 293 `*.tbd` under `usr/lib`, plus per-framework stubs |
| compiler | Alpine clang 19.1.4 (host target `aarch64-alpine-linux-musl`) |
| linker | LLD 19.1.4, driver invoked as `ld64.lld` |
| toolchain binaries from Xcode | **none used** — the clang and linker are upstream builds, not Apple's |

## Measured facts

1. **Compile to device object.** `clang -target arm64-apple-ios15.0
   -isysroot <sdk> -O2 -c` produces `Mach-O 64-bit arm64 object,
   flags:<|SUBSECTIONS_VIA_SYMBOLS>`. Objective-C with `-fobjc-arc` compiles
   against the SDK's UIKit headers once both framework roots are on the search
   path.
2. **SubFrameworks is not optional.** `#import <UIKit/UIKit.h>` fails immediately
   with `'UIUtilities/UIDefines.h' file not found` when only
   `System/Library/Frameworks` is a framework path. The header lives in
   `System/Library/SubFrameworks/UIUtilities.framework/Headers/UIDefines.h`.
   Observation, not inference: the error names UIKitDefines.h:10.
3. **Link to a device executable.** `ld64.lld -arch arm64 -platform_version ios
   15.0 27.3 -syslibroot <overlay> -e _main -framework UIKit ...` yields
   `Mach-O 64-bit arm64 executable, flags:<NOUNDEFS|DYLDLINK|TWOLEVEL|PIE>`,
   69,872 bytes for the window probe. `LC_BUILD_VERSION` parses back as
   platform 2 (iOS) with minos 15.0.0.
4. **The arm64e problem.** SDK stubs list `arm64e` and `arm64e.x1` targets.
   Where the untouched SDK is used for linking, LLD 19 fails; the overlay
   rewrites those tokens to `arm64`. On this machine's compat SDK the overlay
   rewrote **0** files (it was already normalised by the earlier SDK-overlay
   phase), so the rewrite path is covered by fixtures rather than by this run —
   stated here so the claim is not overstated.
5. **Import set.** The linked probe imports exactly
   `/System/Library/Frameworks/{UIKit,Foundation,CoreGraphics,QuartzCore}`,
   `/usr/lib/libobjc.A.dylib`, `/usr/lib/libSystem.B.dylib` — i.e. dyld
   shared-cache libraries only.
6. **Package.** IPA = 6,073 bytes, SHA-256
   `069008a514ba1ae7a23116b2901a65278afecd4dcb8a41e504bd8558de1b226d`,
   two entries: `Payload/WindowProbe.app/{Info.plist,WindowProbe}`.
7. **Time.** Full plan → compile → link → validate → zip: 19.6 s wall on the
   phone, dominated by the SDK overlay tree walk (20 s of `sys`-light I/O on a
   fakefs); the second build with a warm overlay is materially faster.

## What is not verified here

* **Execution on a device.** The IPA is unsigned and unprovisioned; it was not
  installed or launched. Nothing in this report claims it boots.
* **Swift.** No `libswiftCore` stubs exist in a stock SDK; `iosbuild.py` refuses
  `.swift` sources rather than failing at link time.
* **nibs, storyboards, asset catalogs.** Require `ibtool`/`actool`, which are
  macOS binaries and are not runnable here.
* **Xcode project targeting iOS.** `pxcodebuild.py` covers macOS products
  today.

## Reproduction

```sh
python3 iosbuild.py ipa examples/winprobe --out /tmp/out
python3 iosbuild.py inspect /tmp/out/WindowProbe.ipa
python3 tests/iosbuild/test_iosbuild.py -v      # 20 tests
sh tests/run.sh                                  # 14 checks incl. T12
```
