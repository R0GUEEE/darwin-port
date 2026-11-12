# Dependencies and setup

## Supported host tools

The project has no third-party Python modules: its command-line utilities use
only the Python 3 standard library (`zlib`, `gzip`, `bz2`, `lzma`, `plistlib`,
XML, JSON, hashing and filesystem modules). Do not create a virtualenv or run
`pip install -r requirements.txt`; there is no Python package dependency set.

| Tool | Required for | Alpine | Debian/Ubuntu |
|---|---|---|---|
| POSIX shell, `make`, C compiler | build and test loader | `build-base` | `build-essential` |
| LLVM `clang` | compile Darwin-target fixture and `xclang` usage | `clang` | `clang` |
| LLVM `llvm-mc` | assemble arm64 Mach-O fixture | `llvm` | `llvm` |
| `ld64.lld` | link Mach-O test fixtures / Darwin-target output | `lld` | `lld` |
| Python 3 | XIP/DMG/pkg tools, project builder, tests | `python3` | `python3` |
| OpenSSL CLI | optional: verify XIP RSA signature and certificate chain | `openssl` | `openssl` |

Use `./scripts/install.sh` as the primary setup path. It supports Alpine (`apk`)
and Debian/Ubuntu (`apt-get`), with `--check-only` to avoid package changes.
Package installation requires root or `sudo`. `./scripts/doctor.sh` checks the
selected `core` or `full` profile. `bootstrap.sh --check` and `--install` remain
available for compatibility, but the installer and doctor are the maintained
profile-aware path. OpenSSL is included for XIP signature verification; it is
not needed to build or test the loader. Distribution package versions are used;
LLVM 19 is the tested baseline, not a hard-coded mandatory version.

## Optional runtime libraries

`libc++.so.1`, `libc++abi.so.1`, and `libz.so.1` are optional **guest import
providers** loaded by `darwinld` for Darwin programs that import these APIs.
They are not build dependencies and do not affect the core loader test suite.
Install the host distribution's libc++ and zlib runtime packages only when the
particular guest needs them. They do not add Objective-C, Swift, or Apple
framework support.

## Minimal reproducible workflow

From a clean checkout:

```sh
./scripts/install.sh         # recommended: installs, checks, builds, tests
# or, when dependencies are already installed:
./scripts/install.sh --check-only --no-sdk
make clean
make test
```

The default test suite is run by `./scripts/install.sh` or `make test`. It builds
the loader, assembles/links fixture Mach-O images, compiles Darwin-target
fixtures, and runs the compatibility suite. Some checks require only the
checked-in mini SDK; real Xcode-tool tests run only when `APPLE_BIN` points to
an extracted toolchain. No Apple host, network download, Xcode XIP, SDK
extraction, or Python packages are needed for the default test suite.

To cross-compile an application, separately obtain a legally sourced macOS
SDK and set `DARWIN_SDK` to its root. SDK acquisition/extraction and its disk
requirements are a workload-specific optional step, not prerequisites for
building/testing the loader. See [Quickstart](QUICKSTART.md) and
[Reproducing](REPRODUCING.md) for the full extraction workflow and limitations.

## IPA cross-build profile (Linux/WSL, x64 or ARM64)

`./setup.sh install --profile ipa --yes` installs Python 3, Clang, and LLD (including `ld64.lld`) without trying to build `darwinld`, whose runtime backend is ARM64 Linux-specific. An iPhoneOS SDK is supplied separately; the project does not fetch or redistribute it. This profile supports C/Objective-C cross-compilation and unsigned IPA packaging. It does not provide Win32 API emulation, Swift, Apple signing/provisioning, or a native Windows compiler.

## Dependency decisions

* Retain `make` and the platform C toolchain: the loader is C plus one small
  assembly source file, and the Makefile already provides the shortest
  reliable build path.
* Retain LLVM `clang`, `llvm-mc`, and `ld64.lld`: the test suite exercises
  real Mach-O objects and linker behavior; substituting GNU binutils would
  not preserve those tests or the product's cross-compilation feature.
* Retain Python 3 standard library for archive/container processing. Avoid
  external Python dependencies; none are imported by the repository.
* Do not require libc++, zlib development headers, CMake, Git at runtime,
  Docker, or a macOS host for the default build/test path. The loader's C
  build links against the host C library only; `libz`/libc++ are optional
  runtime providers for guest images.
* Avoid pinning distribution package versions in the bootstrap: the repository
  gives no evidence for a strict LLVM version constraint, and the available
  package stream can differ. Reported validation used Alpine LLVM 19.1.4.
