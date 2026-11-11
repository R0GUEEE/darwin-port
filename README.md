# darwin-port

Extract Apple Xcode archives, inspect their contents, and run a useful subset of the Darwin toolchain on Linux. The project contains a Mach-O loader with a libSystem ABI shim, archive inspection tools, and a cross-compiler that emits real Mach-O binaries against a local macOS SDK.

No macOS host is required. Apple SDKs and Xcode archives are not included or downloaded by this project.

## Quick install (Alpine or Debian/Ubuntu)

The guided installer detects the package manager, installs host dependencies, validates the environment, and runs the test suite:

```sh
git clone https://github.com/R0GUEEE/darwin-port.git
cd darwin-port
./scripts/install.sh
```

This installs the **full** profile and runs the compatibility suite. To install only the loader and Python utilities (without the Mach-O test toolchain):

```sh
./scripts/install.sh --profile core
```

To inspect requirements without installing packages:

```sh
./scripts/install.sh --check-only --no-sdk --no-tests
```

The default installer runs the test suite. Add `--no-tests` to install and configure without running it.
The installer uses `apk` on Alpine and `apt-get` on Debian/Ubuntu. Package installation may require root or `sudo`. On other Linux distributions, install the packages listed below manually, then run `./scripts/doctor.sh` and `make test`.

## Requirements

### Full build and test profile

| Requirement | Purpose | Alpine package | Debian/Ubuntu package |
|---|---|---|---|
| `make`, C compiler, libc development files | Build the loader | `build-base` | `build-essential` |
| Python 3 | Utilities and test orchestration | `python3` | `python3` |
| LLVM `clang` | Compile Darwin-target fixtures | `clang` | `clang` |
| LLVM `llvm-mc` | Assemble Mach-O fixtures | `llvm` | `llvm` |
| `ld64.lld` | Link Mach-O fixtures | `lld` | `lld` |
| OpenSSL CLI | Verify XIP certificate chain/signature | `openssl` | `openssl` |

The Python utilities use only the standard library. No `pip`, virtualenv, `requirements.txt`, CMake, or Docker setup is required. LLVM 19.1.4 is the validated environment; versions are not pinned by the installer.

### Optional requirements

- **macOS SDK:** Needed only for compiling against Apple's SDK or building Xcode projects. Supply an SDK obtained separately; this project does not fetch or distribute Apple SDK content.
- **Xcode command-line tools:** `APPLE_BIN` enables optional tests against extracted Apple tools. These tests skip if unavailable.
- **Guest `libc++`, `libc++abi`, or zlib libraries:** Needed only by guest programs importing those libraries, not to build or test the loader.

## Full installation from a clean machine

### 1. Clone the project

```sh
git clone https://github.com/R0GUEEE/darwin-port.git
cd darwin-port
```

### 2. Install dependencies and run setup

Alpine, Debian, or Ubuntu:

```sh
./scripts/install.sh
```

The default `full` profile installs the C build tools, Python 3, LLVM tools, and OpenSSL, then runs the doctor and test suite. The `core` profile installs only the loader and Python utility requirements and skips fixture tests that need LLVM. For a lighter install:

```sh
./scripts/install.sh --profile core
```

For a non-mutating tool check with no tests:

```sh
./scripts/install.sh --check-only --no-sdk --no-tests
```

To install dependencies but skip tests:

```sh
./scripts/install.sh --no-tests
```

### 3. Verify the installation manually

```sh
./scripts/doctor.sh
./bootstrap.sh --check
make clean
make test
```

A successful suite ends with a summary like `passed 13, failed 0`. The SDK is optional for these tests. A few Apple-tool integration tests are skipped unless `APPLE_BIN` points to an extracted toolchain; the regular Mach-O and loader tests do not need it.

### 4. Build and run the loader

```sh
make
./bin/darwinld info /path/to/macho
./bin/darwinld check /path/to/macho
./bin/darwinld run /path/to/macho arg1 arg2
```

The `darwinld` program loads and executes supported Darwin arm64 Mach-O binaries. It does not provide a full macOS environment or implement all Apple frameworks; check unresolved symbols before running a tool.

## Optional: install a local macOS SDK path

SDK acquisition is separate. If you already have a `MacOSX.sdk` directory, validate it and save its path:

```sh
./scripts/sdk-setup.sh /absolute/path/to/MacOSX.sdk --write-config darwin-port.env
./scripts/doctor.sh
```

Alternatively, use the guided SDK profile:

```sh
./scripts/install.sh --profile sdk --sdk /absolute/path/to/MacOSX.sdk
```

The SDK should contain at least:

```text
MacOSX.sdk/
├── usr/include/
├── usr/lib/
└── System/Library/Frameworks/
```

The generated `darwin-port.env` is a local shell assignment file and is not committed. Load it into the current shell before using SDK-dependent tools:

```sh
set -a
. ./darwin-port.env
set +a
```

Or set the SDK only for one command:

```sh
DARWIN_SDK=/absolute/path/to/MacOSX.sdk ./xclang -O2 -o hello hello.c
```

Cross-compile and run a simple program:

```sh
cat > hello.c <<'EOF'
#include <stdio.h>
int main(void) { puts("hello from Darwin"); return 0; }
EOF
DARWIN_SDK=/absolute/path/to/MacOSX.sdk ./xclang -O2 -o hello hello.c
./bin/darwinld run ./hello
```

To choose a target and deployment version:

```sh
DARWIN_SDK=/absolute/path/to/MacOSX.sdk \
DARWIN_TARGET=arm64-apple-macos14.0 \
DARWIN_MINOS=14.0 \
  ./xclang -O2 -o hello hello.c
```

Build a supported Xcode project:

```sh
python3 pxcodebuild.py build path/to/App.xcodeproj \
  --sdk /absolute/path/to/MacOSX.sdk \
  --out ./build
```

Project-builder limitations (Swift, asset catalogs, storyboards, signing, target dependencies, and `.xcconfig`) are documented in [`docs/CLI.md`](docs/CLI.md) and [`docs/XCODE-ON-LINUX.md`](docs/XCODE-ON-LINUX.md).

## Optional: verify an Xcode archive and extract tools

These steps require a separately obtained Xcode `.xip` and substantial free disk space. Verify the archive before extracting:

```sh
python3 verify_xip.py /path/to/Xcode.xip
```

Extract selected content, for example the macOS SDK and toolchain binaries:

```sh
python3 xipx.py extract /path/to/Xcode.xip ./xcode-extracted \
  --include 'Xcode.app/Contents/Developer/Platforms/MacOSX.platform/Developer/SDKs/MacOSX.sdk' \
  --include 'Xcode.app/Contents/Developer/Toolchains/XcodeDefault.xctoolchain/usr/bin'
```

Xcode XIP layouts vary by release. List the archive first and adjust prefixes to the paths actually present:

```sh
python3 xipx.py list /path/to/Xcode.xip -o xip-manifest.json
```

The extracted Xcode SDK may contain `NULLcanary` placeholder headers. The documented repair flow uses headers harvested from other SDKs in the same archive. See [`docs/REPRODUCING.md`](docs/REPRODUCING.md) and [`docs/QUICKSTART.md`](docs/QUICKSTART.md) before attempting full archive reproduction.

For optional real-toolchain tests, point `APPLE_BIN` at the extracted tool directory:

```sh
APPLE_BIN="$PWD/xcode-extracted/Xcode.app/Contents/Developer/Toolchains/XcodeDefault.xctoolchain/usr/bin" \
  make test
```

## Configuration reference

| Variable | Purpose | Default |
|---|---|---|
| `DARWIN_SDK` | macOS SDK used by `xclang` / `pxcodebuild.py` | `/opt/xcode/MacOSX.sdk` |
| `DARWIN_TARGET` | Clang target triple | `arm64-apple-macos15.0` |
| `DARWIN_MINOS` | Minimum macOS version | `15.0` |
| `HOST_CLANG` | Host Clang executable | `clang` |
| `DS_SYSROOT` | Prefix for resolving guest absolute install names | unset |
| `DS_LIB_PATH` | Colon-separated guest dylib paths | unset |
| `DEVELOPER_DIR` | Developer directory returned to guest tools | unset |
| `APPLE_BIN` | Extracted Apple tool directory for optional tests | unset |

The installer exposes additional switches; use:

```sh
./scripts/install.sh --help
```

## What the project supports

- Inspect XIP, xar, DMG, and package formats using Python utilities.
- Parse and load supported arm64 Mach-O executables and dylibs.
- Translate a subset of Darwin ABI calls through the Linux host shim.
- Cross-compile supported C projects for macOS when a usable SDK is configured.
- Run a measured subset of Xcode command-line tools under the loader.

It is not a full macOS compatibility layer: Objective-C, Swift runtime, CoreFoundation, and most Apple frameworks are incomplete. Review [`docs/COMPATIBILITY.md`](docs/COMPATIBILITY.md), [`docs/TROUBLESHOOTING.md`](docs/TROUBLESHOOTING.md), and [`docs/INDEX.md`](docs/INDEX.md).

## Repository map

```text
src/        Mach-O loader, image graph, ABI shim, formatter
scripts/    installer, environment doctor, SDK path helper
*.py        archive, SDK, inventory, build and verification tools
tests/      compatibility suite and fixtures
docs/       setup, CLI, architecture, ABI and reproduction guides
reports/    measured inventories and dependency reports
specs/      behavioural and format specifications
```

## Key documentation

- [Quickstart](docs/QUICKSTART.md)
- [Installation dependencies](docs/DEPENDENCIES.md)
- [Configuration and installer](docs/CONFIGURATION.md)
- [CLI reference](docs/CLI.md)
- [Compatibility status](docs/COMPATIBILITY.md)
- [Troubleshooting](docs/TROUBLESHOOTING.md)
- [Full reproduction workflow](docs/REPRODUCING.md)

## License

See [LICENSE](LICENSE) and [LICENSES.md](LICENSES.md)
