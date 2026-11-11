# Installation and configuration utility

## Quick start

```sh
git clone https://github.com/R0GUEEE/darwin-port.git
cd darwin-port
./setup.sh
```

Setup uses Python 3.8+ and its standard library. If Python is missing, the launcher
prints the package command needed to bootstrap it. No pip or virtual environment
is required. The wizard asks for a profile and optional SDK, displays the planned
package operations, and confirms before installing. Only package commands use
sudo; run setup as your normal account to keep build/config files user-owned.

On Windows, use `.\setup.ps1` in PowerShell or run `./setup.sh` inside WSL.
The PowerShell launcher requires an initialized default Linux WSL distribution
and Python inside that distribution. SDK and configuration paths must be Linux
paths, such as `/mnt/c/SDKs/MacOSX.sdk`. WSL on x64 remains an x64 host: it does
not make the ARM64 loader executable. Use ARM64 Linux for loader workflows.
Native Windows and macOS loader builds are not implemented by this utility.

## Profiles

| Profile | Installed dependencies and behavior |
|---|---|
| `auto` (default) | `full` on ARM64 Linux; `tools` on other hosts |
| `tools` | Python and OpenSSL; archive/inspection utilities; no loader build/tests |
| `core` | Python, make, C compiler and libc headers; build ARM64 Linux loader |
| `full` | Core plus clang, llvm-mc, ld64.lld and OpenSSL; build and test |
| `sdk` | Full profile plus a required, valid local macOS SDK |

The package installer supports `apt-get`, `apk`, `dnf`, and `pacman` on Linux.
Package availability depends on the distribution's enabled repositories. The
installer verifies executables after installation and fails if tools are missing.
Only package names/command construction for Fedora and Arch have automated test
coverage; real package installation on every distribution is not validated.
Other hosts can use portable Python utilities with preinstalled dependencies and
`--profile tools --no-packages`. Core/full/sdk reject unsupported hosts before
installing anything. Apple SDKs and archives are never downloaded.

## Commands

```sh
./setup.sh plan --json
./setup.sh install --yes
./setup.sh install --profile core --yes --jobs 4
./setup.sh install --profile full --no-packages --no-tests
./setup.sh install --profile sdk --sdk /path/MacOSX.sdk --yes
./setup.sh configure --sdk '/path with spaces/MacOSX.sdk'
./setup.sh configure --set DARWIN_MINOS=14.0 --set DARWIN_TARGET=arm64-apple-macos14.0
./setup.sh doctor --json
./setup.sh env
./setup.sh run -- python3 pxcodebuild.py --help
```

No arguments starts the interactive wizard. Noninteractive callers must select an
action. `install --yes` approves package prompts; without it a terminal prompt
is required. `--no-packages` never invokes a package manager and requires all
selected tools to exist already. Re-running installation is supported: package
managers retain installed dependencies and make rebuilds changed sources.

`plan`, `--check-only`, and `--dry-run` are read-only, including no configuration
writes or builds. Missing executables appear in the plan but do not make it fail;
`doctor` returns exit 1 when required tools are missing. Invalid arguments/config
or an incompatible requested profile fail before any package operation.
`--json` provides structured output for plan/doctor; install output includes
progress and is not a standalone JSON document.

`--no-tests` skips loader compatibility tests but still builds. `--jobs N` controls
parallel compilation (default: CPU count capped at eight). Build/test output is
streamed to the terminal and appended to `build/setup.log`; `--log PATH` overrides
it. Package operations are displayed directly in the terminal. A failed command
stops setup with a nonzero status; rerun after correcting the reported problem.
There is no automatic rollback of system packages.

## Configuration

The default file is `<checkout>/darwin-port.env`. Override it with `--config PATH`
or `DARWIN_PORT_CONFIG`. Relative `--config` paths resolve from the current working
directory. Setup creates missing parent directories and writes the file atomically.
Existing changed files get a `.bak` copy; identical writes do nothing. The default
config and backup are git-ignored. Use `.gitignore` for any custom paths in the repo.

Precedence, from lowest to highest: saved file, environment variables, repeated
`--set KEY=VALUE`, then `--sdk` / `--no-sdk`. `--no-sdk` removes the saved SDK on
configure/install and clears it for commands launched by that invocation.
Config files contain literal shell assignments; setup never executes their content.
Unknown keys and malformed assignments are rejected. Shell variables, `~`, and
command substitution in values are not evaluated by the parser. Pass absolute
paths; `--sdk` expands `~` and normalizes the path. `--sdk` and `--no-sdk` conflict.

| Setting | Purpose/default |
|---|---|
| `DARWIN_SDK` | Local SDK; optional except for `sdk` profile |
| `DARWIN_MINOS` | Minimum macOS version; `15.0` |
| `DARWIN_TARGET` | macOS triple; initially derived from minimum version |
| `HOST_CLANG` | Clang executable; `clang` |
| `DS_SYSROOT` | Guest library root |
| `DS_LIB_PATH` | Colon-separated guest dylib directories |
| `DEVELOPER_DIR` | Developer directory reported to guests |
| `APPLE_BIN` | Extracted Apple binaries for optional tests |

Set both target and minimum version when changing deployment version on an
existing config. SDK discovery checks `/opt/xcode/MacOSX.sdk` and conventional
Xcode locations in `/opt` and `/Applications`; a configured invalid SDK fails
rather than silently selecting another. Validation checks `usr/include`, `usr/lib`,
and `System/Library/Frameworks`. This validates layout, not SDK completeness,
header quality, provenance, or successful compilation.

## Activate or run

Saved configuration is passed automatically to install's builds and tests.
Use the command runner to load it for any tool without modifying shell startup files:

```sh
./setup.sh run -- ./xclang hello.c -o hello
./setup.sh run -- darwinld run ./hello
```

For the current POSIX shell:

```sh
eval "$(./setup.sh env)"
```

This exports the saved settings and adds this checkout and its `bin` directory to
PATH. Direct invocations of `xclang`, Python scripts, or `make` otherwise continue
to use their normal process environment. The command runner executes from the
checkout directory; use absolute paths for inputs stored elsewhere. No tools are copied to system directories;
the checkout must stay in place. Moving it requires running `env` again. To remove
the local setup, close the activated shell and remove the checkout when no longer
needed. System dependencies remain managed by the host package manager.

## Legacy entry points

- `scripts/install.sh [options]` → `setup.sh install [options]`
- `scripts/doctor.sh` → `setup.sh doctor` (honors `DARWIN_PORT_PROFILE`)
- `bootstrap.sh --check` / `--install` → doctor / install
- `scripts/sdk-setup.sh PATH --write-config FILE` → configuration with SDK validation

Legacy installation now defaults to an architecture-compatible profile and
prompts before package installation unless `--yes` is passed. SDK setup can create
new config files. Without `--write-config`, the SDK helper only reports a plan.

## Verification and troubleshooting

```sh
python3 -m unittest discover -s tests -p 'test_setup.py' -v
./setup.sh plan --json
./setup.sh doctor
```

- **ARM64 Linux required:** use tools profile on x64 or move loader execution to
  ARM64 Linux. Changing `DARWIN_TARGET` does not emulate a CPU.
- **Missing llvm-mc/ld64.lld:** install the full distribution LLVM/LLD packages and
  expose their bin directory on PATH; vendor clang alone may not include them.
- **Missing sudo:** install packages as administrator separately, then use
  `--no-packages` as your normal account.
- **Invalid saved SDK:** configure a valid replacement or use `configure --no-sdk`.
- **Unsupported config:** migrate executable shell snippets into your own shell
  startup and keep only supported literal assignments in this config.
- **Build/test failure:** inspect `build/setup.log`, fix the underlying issue, and
  rerun. Configuration remains available for diagnosis; success is not reported.

CI runs the installer tests and tools-profile smoke workflow on x64 Ubuntu.
ARM64 loader compatibility tests remain `make test` on an ARM64 Linux host.
