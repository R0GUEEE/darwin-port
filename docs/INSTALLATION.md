# Installation and setup

This is the canonical installation guide for **darwin-port**. It covers the host prerequisites, clean install, optional SDK configuration, verification, and troubleshooting.

## Choose your setup

| Goal | Setup |
|---|---|
| Build/run the Mach-O loader and use Python archive utilities | `core` |
| Run the full fixture suite and use `xclang` | `full` (recommended) |
| Cross-compile against an existing macOS SDK | `full` + SDK path |
| Verify Xcode XIP signatures | `full` + OpenSSL CLI |

A macOS host, Xcode installation, SDK, Python packages, virtualenv, Docker, CMake, and network access are **not** needed for the default full test suite. The project does not provide, download, or redistribute Apple SDK/Xcode content. Supply an SDK separately only for SDK-dependent workflows.

## Requirements

### Core profile

- POSIX shell
- `make`
- C compiler (`cc`) and the host libc development files
- Python 3 (standard library only)

### Full profile

Everything in core, plus:

- `clang`
- `llvm-mc`
- `ld64.lld`
- OpenSSL CLI is optional for tests, but required by XIP signature verification

`llvm-mc` assembles Mach-O fixtures; GNU `as` is not a substitute. The tested environment uses LLVM 19, but the installer does not pin versions.

### Optional guest runtime libraries

`libc++.so.1`, `libc++abi.so.1`, and `libz.so.1` are providers for particular guest binaries, not build dependencies. Install the host runtime package only when a guest reports it needs one. They do not add Apple framework, Objective-C, or Swift support.

## Clean install (Alpine, Debian, Ubuntu)

### 1. Clone and enter the repository

```sh
git clone https://github.com/R0GUEEE/darwin-port.git
cd darwin-port
```

### 2. Install, build, and test

Recommended: full profile. The installer detects `apk` or `apt-get`, installs the relevant packages, checks the toolchain, builds the loader, and runs the compatibility tests.

```sh
./scripts/install.sh
```

Package installation may need root or `sudo`. On Debian/Ubuntu, the installer runs `apt-get update` and installs packages noninteractively. On Alpine, it uses `apk add`.

When the tools are already installed, validate and test without a package-manager operation:

```sh
./scripts/install.sh --check-only --no-sdk
```

The core profile installs/checks only the loader and Python utility requirements and builds `bin/darwinld`; it skips LLVM-dependent fixture tests:

```sh
./scripts/install.sh --profile core
```

To install the full toolchain but defer tests:

```sh
./scripts/install.sh --no-tests
```

See all options with `./scripts/install.sh --help`.

### 3. Verify

```sh
./scripts/doctor.sh
make test
```

A successful test run ends with `passed 13, failed 0` (the exact count may change as tests are added). Optional Apple-tool integration checks are skipped unless `APPLE_BIN` points to an extracted tool directory.

## Manual setup on other Linux distributions

Install the equivalent packages using your distribution's package manager:

| Requirement | Alpine | Debian/Ubuntu |
|---|---|---|
| Core build tools | `build-base` | `build-essential` |
| Python | `python3` | `python3` |
| Clang | `clang` | `clang` |
| LLVM assembler (`llvm-mc`) | `llvm` | `llvm` |
| Mach-O linker (`ld64.lld`) | `lld` | `lld` |
| XIP signature verification | `openssl` | `openssl` |

For other distributions, package names may differ. Ensure all required executables are on `PATH`, then run:

```sh
./scripts/doctor.sh
make clean
make test
```

`./bootstrap.sh --check` is a legacy dependency check; use `scripts/doctor.sh` for profile-aware checks. `bootstrap.sh --install` is retained for compatibility, but the guided `scripts/install.sh` is the primary installer.

## Optional macOS SDK setup

An SDK is needed only for compiling against Apple headers/framework stubs with `xclang` or `pxcodebuild.py`. It is **not** needed to build/test the loader. Obtain and extract a macOS SDK separately; this repository never fetches it.

A usable SDK directory should include:

```text
MacOSX.sdk/
├── usr/include/
├── usr/lib/
└── System/Library/Frameworks/
```

Validate and save its absolute path in a local config file:

```sh
./scripts/sdk-setup.sh /absolute/path/to/MacOSX.sdk --write-config darwin-port.env
```

Or use the installer to validate an explicit SDK while setting up the full profile:

```sh
./scripts/install.sh --profile sdk --sdk /absolute/path/to/MacOSX.sdk
```

The installer writes an SDK setting only when the config file already exists;
`darwin-port.env` is not created automatically. Copy `darwin-port.env.example`
first, or create an empty config file, then pass `--config` if it is elsewhere:

```sh
cp darwin-port.env.example darwin-port.env
./scripts/install.sh --profile sdk --sdk /absolute/path/to/MacOSX.sdk
```

The `sdk` profile always uses the full dependency set. The config file is local and should not be committed.

The config file is local and should not be committed. Load it into the current shell before invoking SDK-dependent commands:

```sh
set -a
. ./darwin-port.env
set +a
```

Alternatively, set `DARWIN_SDK` for one command:

```sh
DARWIN_SDK=/absolute/path/to/MacOSX.sdk ./xclang -O2 -o hello hello.c
```

To discover a conventional local SDK path and write it to config:

```sh
./scripts/sdk-setup.sh --write-config darwin-port.env
```

If no SDK is present, skip this section. `darwinld`, the Python utilities, and the default tests remain usable.

## Build and use

The normal build entry point is:

```sh
make                 # builds bin/darwinld
make test             # builds and runs compatibility tests
make clean            # removes bin/ and build/
```

Inspect or run a Darwin arm64 Mach-O binary through the loader:

```sh
bin/darwinld info /path/to/macho
bin/darwinld check /path/to/macho
bin/darwinld run /path/to/macho arg1 arg2
```

`check` reports unresolved imports before execution. This is a measured compatibility subset, not a complete macOS environment; unsupported imports/frameworks prevent some binaries from running.

Cross-compile a C program with a configured SDK:

```sh
DARWIN_SDK=/absolute/path/to/MacOSX.sdk \
  ./xclang -O2 -o hello hello.c
file hello
bin/darwinld check ./hello
```

For supported Xcode project formats, see [Xcode on Linux](XCODE-ON-LINUX.md) and [CLI reference](CLI.md). The project builder has limitations, including Swift, asset catalogs, storyboards, code signing, target dependencies, and `.xcconfig` handling.

## Environment configuration

`darwin-port.env.example` lists available settings. Copy it only if you want a reusable local shell configuration:

```sh
cp darwin-port.env.example darwin-port.env
```

The doctor loads `darwin-port.env` automatically. Other programs consume environment variables, so source the file in the shell as shown above. Common settings:

| Variable | Purpose |
|---|---|
| `DARWIN_SDK` | macOS SDK root for `xclang` / `pxcodebuild.py` |
| `DARWIN_TARGET` | Darwin target triple, e.g. `arm64-apple-macos15.0` |
| `DARWIN_MINOS` | Minimum macOS deployment version |
| `HOST_CLANG` | Host Clang executable |
| `DS_SYSROOT` | Root prefix for guest absolute install-name resolution |
| `DS_LIB_PATH` | Colon-separated guest dylib search paths |
| `DEVELOPER_DIR` | Developer directory reported to guest tools |
| `APPLE_BIN` | Extracted Apple tool directory for optional integration tests |

Full profile and option reference: [Configuration](CONFIGURATION.md).

## Troubleshooting

- **`command not found` in doctor:** install the missing profile dependency and ensure it is on `PATH`; rerun `./scripts/doctor.sh`.
- **`llvm-mc` or `ld64.lld` missing:** install LLVM tools. `make test` needs them for Mach-O fixtures.
- **SDK not found:** SDK configuration is optional. For SDK-dependent builds, pass the correct absolute SDK root (the directory containing `usr/include`, `usr/lib`, and `System/Library/Frameworks`).
- **SDK config path is invalid:** rerun `scripts/sdk-setup.sh` with the correct path; edit or remove the stale `DARWIN_SDK` assignment in `darwin-port.env`.
- **XIP signature verification fails due to missing OpenSSL:** install the host OpenSSL CLI package. OpenSSL is not a loader build requirement.
- **Guest reports unresolved imports:** run `bin/darwinld check <binary>` and compare the imports with [Compatibility](COMPATIBILITY.md). A host SDK does not supply every runtime library required by a guest.
- **Build fails after changing compilers/flags:** run `make clean && make test` to discard stale artifacts.

For loader/runtime issues see [Troubleshooting](TROUBLESHOOTING.md); for archive extraction and SDK-header repair see [Reproducing](REPRODUCING.md).

## Related guides

- [Quickstart](QUICKSTART.md)
- [Dependencies](DEPENDENCIES.md)
- [Configuration](CONFIGURATION.md)
- [Reproducing Xcode extraction](REPRODUCING.md)
- [Documentation index](INDEX.md)
