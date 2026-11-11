# Reproducing

The exact sequence, with the budgets that matter. Measured on a 4 GB ARM64
Linux host (iSH/Alpine); a laptop is roughly an order of magnitude faster.

## Budgets

| step | wall clock | disk | peak RAM |
|---|---|---|---|
| verify XIP (`sha1` of the 2 GB content) | 13 s | 0 | 8 MB |
| `xipx.py list` (one full stream) | 24 min | 30 MB manifest | 40 MB |
| extract macOS SDK + toolchain `usr/bin` | 24 min | 1.3 GB | 40 MB |
| extract all SDK `usr/include` (12 SDKs) | 31 min | 9.9 GB | 40 MB |
| `hdrmerge.py` | 3.5 min | +0 | 20 MB |
| `inventory.py` over 30 k files | 6 min | 8 MB | 30 MB |
| build `darwinld` | 8 s | 85 KB | — |
| `tests/run.sh` | 40 s | 5 MB | — |
| bind census over 76 binaries | 4 min | 1 MB | — |

Total ≈ 90 minutes of wall clock, 11 GB of disk at peak (the 9.9 GB header
harvest exists only to repair placeholders and can be deleted afterwards).

## 0. Build and test without an Xcode archive

The compatibility suite does not require the large XIP or a real SDK. From a
clean checkout, run `./bootstrap.sh --check` (or `--install` on Alpine or
Debian/Ubuntu), then `make clean && make test`. See
[DEPENDENCIES](DEPENDENCIES.md) for exact host requirements. Continue below
only when reproducing the archive-derived reports or extracting an SDK.

## 1. Verify before you extract

```sh
sha256sum Xcode_27.3_beta.xip                 # 7263cc66…15cb58

python3 - <<'EOF'
import hashlib, struct, zlib
f = open('Xcode_27.3_beta.xip','rb')
magic, hsz, ver, tc, tu, ck = struct.unpack('>IHHQQI', f.read(28))
toc = zlib.decompress(f.read(tc))
heap = hsz + tc                                  # member offsets are heap-relative
f.seek(heap + 276)                               # Content's offset inside the TOC
h = hashlib.sha1(); left = 2075396748
while left:
    b = f.read(min(4 << 20, left))
    if not b: break
    h.update(b); left -= len(b)
print(h.hexdigest())        # must equal 53a622bf007cdb198d87f3552cecd7aca30b5a80
EOF
```

If that hash does not match, stop: nothing downstream is meaningful.

## 2. Extract what you need

```sh
python3 xipx.py extract Xcode_27.3_beta.xip /root/xc \
  --include 'Xcode-beta.app/Contents/Developer/Platforms/MacOSX.platform/Developer/SDKs/MacOSX.sdk/usr' \
  --include 'Xcode-beta.app/Contents/Developer/Platforms/MacOSX.platform/Developer/SDKs/MacOSX.sdk/System/Library/Frameworks' \
  --include 'Xcode-beta.app/Contents/Developer/Toolchains/XcodeDefault.xctoolchain/usr/bin' \
  --include 'Xcode-beta.app/Contents/Developer/Toolchains/XcodeDefault.xctoolchain/usr/lib/clang' \
  --exclude Resources --exclude AssetRuntime --exclude DeviceSupport \
  -o /root/logs/xcode_manifest.json
```

One pass. Do **not** extract `Platforms/*` wholesale (simulators, device support
and asset runtimes add several GB of nothing useful), and do **not** run two
passes concurrently — each streams the whole 10.4 GiB.

Sampling check afterwards:

```sh
find /root/xc -name '*.tbd' | wc -l                  # ≈ 1395 in the macOS SDK
ls /root/xc/.../MacOSX.sdk/usr/include/stdio.h       # present
```

## 3. Repair the SDK

```sh
python3 xipx.py extract Xcode_27.3_beta.xip /root/hdr --pattern '*/SDKs/*/usr/include*'
python3 hdrmerge.py /root/hdr /root/xc/.../MacOSX.sdk
# patched 846, already real 2613, unresolved 1

find /root/xc/.../MacOSX.sdk -name '*.tbd' -exec \
  sed -i 's/, *arm64e\.x1-\(macos\|maccatalyst\)//' {} +
```

After this, `#include <stdio.h>` compiles and `ld64.lld` accepts `libSystem.tbd`.

## 4. Build and test the loader

```sh
cd workspace/darwin-port/src
cc -O2 -o ../bin/darwinld main.c macho.c shim.c fmt.c fmt_asm.S plat_posix.c dso.c
chmod +x ../bin/darwinld

cd ../../xcode-re && sh tests/run.sh     # expect: passed 11, failed 0
```

The suite needs no Apple SDK for T1–T8 (it ships a 20-symbol stub); T9 uses the
toolchain if `APPLE_BIN` points at one.

## 5. Reproduce the census

```sh
B=/root/xc/Xcode-beta.app/Contents/Developer/Toolchains/XcodeDefault.xctoolchain/usr/bin
for f in "$B"/*; do
  [ -f "$f" ] || continue
  rm -f /tmp/m
  DS_MISSING_FILE=/tmp/m darwinld check "$f" >/dev/null 2>&1
  printf '%6s %s\n' "$(wc -l < /tmp/m 2>/dev/null || echo 0)" "$(basename "$f")"
done | sort -n
```

Expect 40 lines with `0`. `grep -c ' 0 '` is the headline number.

## 6. Rebuild the written reports

```sh
cd workspace/xcode-re && python3 scripts/build_reports.py
# inventory: 167522 entries
# dependencies: 131 images, 120 distinct dylibs
```

It reads `/root/logs/xcode_manifest.json`, `reports/inventory-extracted.jsonl`
and `/root/logs/inv.json`; regenerate the middle one with
`python3 scripts/inventory.py /root/xc --output reports/inventory-extracted.jsonl`.

## 7. Determinism notes

| thing | deterministic? |
|---|---|
| container extraction | yes — byte-for-byte, verified by SHA-1 |
| Mach-O parsing / bind census | yes |
| `ar` archive output | yes except the member timestamps (they come from the host `stat`) |
| `ctags`/`bison`/`flex` output | yes |
| anything Swift/ObjC | does not run |

## What to do when a step diverges

Diagnose with the symptom list in [TROUBLESHOOTING](TROUBLESHOOTING.md); the two
most likely divergences are a partial XIP (hash mismatch at step 1) and an
unrepaired SDK (`NULLcanary` at compile time).
