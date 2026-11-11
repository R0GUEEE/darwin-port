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

The setup doctor has two profiles:

* `core`: loader build and Python utility requirements (`make`, C compiler,
  Python 3). Use `DARWIN_PORT_PROFILE=core scripts/doctor.sh`.
* `full` (default): core plus LLVM tools needed by Mach-O fixture tests and
  SDK cross-compilation (`clang`, `llvm-mc`, `ld64.lld`). An SDK is reported
  as optional because it is not required for the loader test suite.

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
