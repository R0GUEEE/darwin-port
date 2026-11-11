# IPA — building an unsigned iOS app on Linux

`iosbuild.py` turns Objective-C sources into `Payload/Foo.app/Foo`: a real
thin arm64 Mach-O, `platform IOS`, linked against the iPhoneOS SDK's text
stubs, wrapped in a bundle with a valid `Info.plist`, zipped as an `.ipa`.

It is the first thing in this project that produces an artifact for the *other*
end of the pipeline: everything else here runs Apple's toolchain off Apple;
this one emits something an iPhone would load.

```sh
python3 iosbuild.py plan    examples/winprobe
python3 iosbuild.py build   examples/winprobe --out /tmp/out
python3 iosbuild.py ipa     examples/winprobe --out /tmp/out
python3 iosbuild.py inspect /tmp/out/WindowProbe.ipa
```

Measured on this machine (4 GB iPhone, iSH, clang 19 + ld64.lld 19, iPhoneOS
27.3 compat SDK): a UIKit app compiles, links and packages in **~20 s**, and the
result is a 6 KB IPA containing a 69 KB executable that imports only
`/System/Library/Frameworks/{UIKit,Foundation,CoreGraphics,QuartzCore}`,
`/usr/lib/libobjc.A.dylib` and `/usr/lib/libSystem.B.dylib`.

## What it does, in order

1. **Manifest.** `ipa.json` next to the sources, or the directory itself when it
   contains `.m`/`.c` sources and no manifest. Glob patterns are expanded and a
   pattern that matches nothing is an error — a build that silently compiles
   zero files is worse than a failure.
2. **SDK discovery.** `--sdk`, else `$IPHONEOS_SDK`, else an `iphoneos*compat*`
   directory, else any iPhoneOS SDK inside an extracted Xcode tree. Missing SDK
   is exit code 3, not a traceback.
3. **Overlay.** iPhoneOS 27.x stubs declare `arm64e` and `arm64e.x1` targets and
   `ld64.lld` 19 rejects both. The overlay writes a shadow tree of rewritten
   `.tbd` text (targets collapsed to `arm64`) and symlinks everything else.
   The original SDK is never modified; the overlay is cached by SDK path+version.
4. **Compile.** `clang -target arm64-apple-ios<min> -isysroot <sdk> -fobjc-arc`,
   with **both** framework roots: `System/Library/Frameworks` and
   `System/Library/SubFrameworks`. UIKit's headers reach into the latter
   (`#import <UIUtilities/UIDefines.h>`), and without it the first line of
   `UIKit.h` fails with "file not found".
5. **Link.** `ld64.lld -arch arm64 -platform_version ios <min> <sdk>
   -syslibroot <overlay> -e _main -framework … -l…`.
6. **Validate before packaging.** The executable must be thin arm64, PIE, not
   encrypted, `platform IOS`, file type executable, and import nothing outside
   `/usr/lib`, `/System/Library`, `@rpath/libswift*`, `@executable_path`,
   `@loader_path`. This is the check that catches an SDK overlay mistake, which
   otherwise produces a perfectly valid *macOS* binary with an iOS name.
7. **Package.** `Info.plist` (XML plist, no `plutil` needed), resources copied,
   `Payload/<Name>.app` zipped with unix modes preserved.

## What it deliberately does not do

| missing | why, and what it would take |
|---|---|
| **signing** | an IPA here is unsigned. Install it through a signing service, or `ldid -S<entitlements>` on a jailbroken device. Apple signing needs Apple's authenticated path. |
| **Swift** | a stock SDK ships no `libswiftCore` stubs, so a Swift binary cannot even be linked, let alone run: the Swift runtime would have to be shipped inside the app and registered before `main`. `iosbuild.py` rejects `.swift` sources with that sentence rather than a linker dump. See [SWIFT](SWIFT.md). |
| **nibs/storyboards/asset catalogs** | they need `ibtool`/`actool`, which are macOS-only binaries that no Linux host can execute. The example app builds its UI in code. |
| **Xcode project files** | `pxcodebuild.py` handles `.xcodeproj` for *macOS* products; an `.xcodeproj` targeting iOS is Phase 2 of the roadmap in [XCODE-ON-LINUX](XCODE-ON-LINUX.md). |
| **device install** | an unsigned, unprovisioned IPA installs only through a signing service or a jailbroken device. |
| **App Store submission** | requires Apple's signing and notarization chain by definition. |

## The example

`examples/winprobe/` is the smallest honest proof: a `UIApplicationMain`, an
`AppDelegate`, a `UIWindow`, a root view controller built in code, and one
`UILabel`. No nibs, no assets, no third-party code. Its `ipa.json` is the
manifest reference — name, bundle id, version, minimum iOS, sources,
frameworks, libraries.

## Tests

```
python3 tests/iosbuild/test_iosbuild.py -v      # 20 tests
```

Fixture tests synthesise Mach-O load commands, TBD text, bundles and IPAs, so
the parsers that decide what ships are covered without an SDK. The integration
test builds `examples/winprobe` end to end and skips cleanly when no SDK is
present. `tests/run.sh` runs them as T12.

## Exit codes

`0` ok · `1` build/validation failure · `2` usage/manifest error ·
`3` missing SDK or linker.
