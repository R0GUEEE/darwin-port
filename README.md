# darwin-port

Extract Apple Xcode archives, inspect their contents, and run a useful subset of the Darwin toolchain on Linux. The project contains a Mach-O loader with a libSystem ABI shim, archive inspection tools, and a cross-compiler that emits real Mach-O binaries against a local macOS SDK.

No macOS host is required. Apple SDKs and Xcode archives are not included or downloaded by this project.

## Install and verify

For a clean Alpine, Debian, or Ubuntu host:

```sh
git clone https://github.com/R0GUEEE/darwin-port.git
cd darwin-port
./scripts/install.sh
```

The default `full` profile installs the host build tools, Python 3, LLVM Mach-O tools, builds `bin/darwinld`, and runs the compatibility suite. Package installation may require root or `sudo`.

If the dependencies are already installed, check and test without installing packages:

```sh
./scripts/install.sh --check-only --no-sdk
```

Choose a smaller or faster setup:

```sh
./scripts/install.sh --profile core   # loader + Python utilities; build only
./scripts/install.sh --no-tests       # full dependencies and build, defer tests
./scripts/install.sh --help           # all installer options
```

For other Linux distributions, manually install `make`, a C compiler and libc development files, Python 3, `clang`, `llvm-mc`, and `ld64.lld`, then run `./scripts/doctor.sh && make test`. OpenSSL is optional unless verifying XIP signatures.

**A macOS SDK is optional.** The default build and tests need no SDK, Xcode archive, macOS host, Python package installation, virtualenv, Docker, or CMake. For SDK-dependent compilation, supply an SDK separately and configure it with:

```sh
./scripts/sdk-setup.sh /absolute/path/to/MacOSX.sdk --write-config darwin-port.env
set -a; . ./darwin-port.env; set +a
```

The project does not download or redistribute Apple SDK/Xcode content. See the complete [Installation and setup guide](docs/INSTALLATION.md) for prerequisites, profiles, configuration, verification, and troubleshooting.

## Build and use

```sh
make                       # → bin/darwinld
make test                  # build + compatibility suite
bin/darwinld info /path/to/macho
bin/darwinld check /path/to/macho
bin/darwinld run /path/to/macho arg1 arg2
```

`darwinld` loads and executes supported Darwin arm64 Mach-O binaries. It does not provide a full macOS environment or implement all Apple frameworks; use `darwinld check` to identify unresolved symbols before running a tool.

## Optional workflows

### Cross-compile against a local SDK

```sh
DARWIN_SDK=/absolute/path/to/MacOSX.sdk ./xclang -O2 -o hello hello.c
bin/darwinld check ./hello
```

For supported Xcode project formats:

```sh
python3 pxcodebuild.py build path/to/App.xcodeproj \
  --sdk /absolute/path/to/MacOSX.sdk \
  --out ./build
```

Project-builder limitations (Swift, asset catalogs, storyboards, signing, target dependencies, and `.xcconfig`) are documented in [CLI](docs/CLI.md) and [Xcode on Linux](docs/XCODE-ON-LINUX.md).

### Verify and extract an Xcode archive

These steps require a separately obtained Xcode `.xip` and substantial free disk space. Verify it first:

```sh
python3 verify_xip.py /path/to/Xcode.xip
python3 xipx.py list /path/to/Xcode.xip -o xip-manifest.json
```

Extract selected content after checking the archive's actual paths:

```sh
python3 xipx.py extract /path/to/Xcode.xip ./xcode-extracted \
  --include 'Xcode.app/Contents/Developer/Platforms/MacOSX.platform/Developer/SDKs/MacOSX.sdk' \
  --include 'Xcode.app/Contents/Developer/Toolchains/XcodeDefault.xctoolchain/usr/bin'
```

Xcode XIP layouts vary. Extracted SDKs may contain `NULLcanary` placeholder headers; see [Reproducing](docs/REPRODUCING.md) and [Quickstart](docs/QUICKSTART.md) for the repair workflow.

## Documentation

- [Installation and setup](docs/INSTALLATION.md) — canonical clean install, dependencies, SDK, configuration, verification, troubleshooting
- [Quickstart](docs/QUICKSTART.md) — build, run, extract, and cross-compile
- [Dependencies](docs/DEPENDENCIES.md) — host and guest dependency rationale
- [Configuration](docs/CONFIGURATION.md) — installer profiles and environment variables
- [CLI reference](docs/CLI.md)
- [Compatibility status](docs/COMPATIBILITY.md)
- [Troubleshooting](docs/TROUBLESHOOTING.md)
- [Full reproduction workflow](docs/REPRODUCING.md)
- [Documentation index](docs/INDEX.md)

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

## License

See [LICENSE](LICENSE) and [LICENSES.md](LICENSES.md)
