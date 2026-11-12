# darwin-port

Extract Apple Xcode archives, inspect their contents, and run a useful subset of the Darwin toolchain on Linux. The project contains a Mach-O loader with a libSystem ABI shim, archive inspection tools, and a cross-compiler that emits real Mach-O binaries against a local macOS SDK.

No third-party Python packages are required. The checked-in [`dependencies.json`](dependencies.json) is the machine-readable dependency manifest used by the installer for package selection, `doctor` checks, and profile build/test behavior; see [dependency documentation](docs/DEPENDENCIES.md) for OS package equivalents and optional inputs. Apple SDKs and Xcode archives are not included or downloaded by this project.

## Guided installation and configuration

```sh
git clone https://github.com/R0GUEEE/darwin-port.git
cd darwin-port
./setup.sh
```

The terminal wizard selects a profile, accepts an optional local SDK, previews
package operations, and then installs dependencies and saves configuration.
Python 3.8+ is needed to start; the launcher prints bootstrap instructions if missing.
Supported package managers: Debian/Ubuntu `apt-get`, Alpine `apk`, Fedora `dnf`,
and Arch `pacman`. Other distributions can use `--no-packages` after installing
requirements manually.

**Host compatibility:** the loader uses ARM64 assembly and requires ARM64 Linux.
`auto` selects `full` there and `tools` elsewhere. On x64 Linux/WSL, `tools`
installs the Python archive/inspection utilities; it does not build or execute
the ARM64 loader. This installer does not add CPU emulation.

```sh
./setup.sh plan --json                    # read-only preview
./setup.sh install --yes                  # automatic compatible profile
./setup.sh install --profile core --yes   # ARM64 loader, no LLVM fixture tests
./setup.sh configure --sdk /path/MacOSX.sdk
./setup.sh doctor
./setup.sh run -- python3 inventory.py tests/fixtures
```

On Windows, install and initialize a Linux WSL distribution, then run
`.\setup.ps1` from PowerShell in this checkout, or `./setup.sh` inside WSL.
Use Linux paths for SDK/config arguments. The PowerShell launcher uses the default
WSL distribution; setup does not install WSL or change its default distribution.

See the [complete setup guide](docs/CONFIGURATION.md) for profiles, settings,
noninteractive use, recovery, logs, and configuration activation. The old
`scripts/install.sh`, `scripts/doctor.sh`, `scripts/sdk-setup.sh`, and
`bootstrap.sh` entry points delegate to the unified utility.

## Build and use

The loader build and execution commands require ARM64 Linux.

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

### Build an iOS IPA from Windows-authored source

```powershell
.\iosbuild.ps1 ipa examples/windows-native --out build-out
```

The Windows launcher invokes the Linux builder in WSL2. Install the IPA-only
toolchain profile with `./setup.sh install --profile ipa --yes` (it installs
Python, Clang, and LLD without attempting to build the ARM64-hosted loader),
then configure a local iPhoneOS SDK. Pass an SDK path in WSL form, such as
`--sdk /mnt/c/ios-sdk/iPhoneOS.sdk`; relative source/output paths resolve from
the repository root. Source must be portable to iOS (C/Objective-C and iOS
frameworks), not Win32 desktop UI/API code. Output is unsigned and not
App Store-ready. See [IPA](docs/IPA.md) for exact boundaries.

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
