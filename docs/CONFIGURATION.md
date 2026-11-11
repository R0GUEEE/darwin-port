# darwin-port environment and profiles

`darwin-port.env.example` is the configuration template. Copy it to
`darwin-port.env`, edit the paths for your machine, then load it in a shell:

```sh
cp darwin-port.env.example darwin-port.env
${EDITOR:-vi} darwin-port.env
set -a
. ./darwin-port.env
set +a
```

The local `darwin-port.env` file is intended for shell assignments. It is
explicitly loaded by `scripts/doctor.sh`; other commands do not source config
files automatically, so source it in the shell before invoking tools.

## Profiles

Run `scripts/install.sh` for a guided setup. It detects Alpine or Debian/Ubuntu,
installs only the packages for the selected profile, validates tools, optionally
configures a locally available SDK, builds the loader, and (for `full`) runs
the suite. The canonical end-to-end guide is [INSTALLATION](INSTALLATION.md).

```sh
scripts/install.sh                         # full setup + tests
scripts/install.sh --profile core          # loader + Python tools only
scripts/install.sh --profile sdk           # full + local SDK detection/config
scripts/install.sh --check-only --no-sdk   # report without installing
scripts/install.sh --sdk /path/MacOSX.sdk  # configure an explicit SDK
```

The installer does not fetch an SDK. `--profile core` requires make, a C
compiler, and Python 3. `full` (default) adds LLVM tools for Mach-O fixture
tests and SDK cross-compilation (`clang`, `llvm-mc`, `ld64.lld`) plus OpenSSL
for XIP verification. `sdk` adds discovery/configuration of an already present
macOS SDK. If no SDK exists, the SDK step is clearly skipped; loader tests
remain available.

Run `scripts/doctor.sh` after setup. Exit 0 means required tools for the
selected profile were found. An unset SDK and missing OpenSSL are informational;
an SDK path that is explicitly configured but invalid is a failure. OpenSSL is
only needed for XIP signature verification.

## Settings

| Variable | Purpose | Default |
|---|---|---|
| `DARWIN_SDK` | Apple macOS SDK root for xclang/pxcodebuild | `/opt/xcode/MacOSX.sdk` |
| `DARWIN_TARGET` | clang target triple | `arm64-apple-macos15.0` |
| `DARWIN_MINOS` | minimum macOS deployment version | `15.0` |
| `HOST_CLANG` | host clang executable used by xclang | `clang` |
| `DS_SYSROOT` | root prefix when resolving guest absolute install names | unset |
| `DS_LIB_PATH` | colon-separated guest dylib search roots | unset |
| `DEVELOPER_DIR` | developer directory reported to guest tools | unset |
| `DARWIN_PORT_CONFIG` | env-file path loaded by doctor | `<repo>/darwin-port.env` |
| `DARWIN_PORT_PROFILE` | doctor profile: `core` or `full` | `full` |

`xclang` currently reads its settings from the process environment and does
not source configuration files itself. `pxcodebuild.py --sdk` overrides
`DARWIN_SDK`; otherwise it reads the environment. `darwinld` runtime search
settings are likewise environment variables.

## Target examples

```sh
# macOS arm64, default target
DARWIN_SDK=/path/to/MacOSX.sdk xclang hello.c -o hello

# macOS Intel, lower deployment target
DARWIN_SDK=/path/to/MacOSX.sdk \
DARWIN_TARGET=x86_64-apple-macos13.0 DARWIN_MINOS=13.0 \
  xclang hello.c -o hello

# Loader search roots for a staged runtime tree
DS_SYSROOT=/opt/guest-root DS_LIB_PATH=/opt/guest-root/usr/lib \
  bin/darwinld check ./some-tool
```

The project does not download or redistribute an Apple SDK. SDK acquisition is
separate from configuring a local SDK path; see [Reproducing](REPRODUCING.md).
Use `scripts/sdk-setup.sh` to locate a conventional local macOS SDK and optionally
write `DARWIN_SDK` into your config file:

```sh
scripts/sdk-setup.sh
scripts/sdk-setup.sh --write-config darwin-port.env
```

Then source `darwin-port.env` before building, or pass `DARWIN_SDK` directly in
the environment. The helper validates SDK headers, libraries, and frameworks;
it never downloads an SDK.
