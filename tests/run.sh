#!/bin/sh
# Reproducible compatibility tests for the Darwin arm64 loader.
#
#   ./tests/run.sh
#
# Requires: clang/llvm-mc + ld64.lld (for the Mach-O fixtures).
# Optional: an extracted Xcode tree (APPLE_BIN=...) to run the real-tool tests;
#           those tests skip cleanly when it is absent.
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(dirname "$HERE")
# repo layout: src/ + Makefile at the root; audit layout: ports/darwin-port/
if [ -f "$ROOT/Makefile" ] && [ -d "$ROOT/src" ]; then
    PORT="$ROOT"
else
    PORT="$ROOT/ports/darwin-port"
fi
BIN="$PORT/bin/darwinld"
SDK_MINI="$HERE/fixtures/mini-sdk"
OUT="${TMPDIR:-/tmp}/xcode-re-tests"
mkdir -p "$OUT"

pass=0; fail=0; skip=0
ok()   { printf '  PASS  %s\n' "$1"; pass=$((pass+1)); }
bad()  { printf '  FAIL  %s\n     %s\n' "$1" "${2:-}"; fail=$((fail+1)); }
skip_() { printf '  SKIP  %s (%s)\n' "$1" "$2"; skip=$((skip+1)); }

echo "== build =="
make -C "$PORT" >/dev/null 2>"$OUT/build.err" || { cat "$OUT/build.err"; exit 1; }
chmod +x "$BIN" 2>/dev/null; chmod +x "$PORT/xclang" "$ROOT/bin/xclang" 2>/dev/null
[ -x "$BIN" ] && ok "loader builds" || { bad "loader builds"; exit 1; }

echo "== T1: freestanding Mach-O, exit code propagation =="
llvm-mc -triple=arm64-apple-macos14 -filetype=obj -o "$OUT/hello.o" "$HERE/fixtures/hello.s" 2>/dev/null
ld64.lld -arch arm64 -platform_version macos 14.0 14.0 -e _main -o "$OUT/hello" "$OUT/hello.o" 2>/dev/null
"$BIN" run "$OUT/hello" >/dev/null 2>&1
rc=$?
[ "$rc" = 42 ] && ok "exit code 42 survives the loader" || bad "exit code" "got $rc, want 42"

echo "== T2: Darwin stack-passed varargs =="
"$PORT/xclang" --help >/dev/null 2>&1
DARWIN_SDK="$SDK_MINI" DARWIN_MINOS=14.0 "$PORT/xclang" -O2 -fno-stack-protector \
    -o "$OUT/varargs" "$HERE/fixtures/variadic.c" 2>/dev/null
got=$("$BIN" run "$OUT/varargs" 2>/dev/null)
[ "$got" = "x=42 s=abc c=Z u=7" ] && ok "printf reimplemented for the stack ABI" \
    || bad "variadic formatting" "got '$got'"

echo "== T3: two images, two-level namespace =="
clang -target arm64-apple-macos14 -O1 -c "$HERE/fixtures/libgreet.c" -o "$OUT/libgreet.o" 2>/dev/null
clang -target arm64-apple-macos14 -O1 -c "$HERE/fixtures/uselib.c"   -o "$OUT/uselib.o" 2>/dev/null
ld64.lld -dylib -arch arm64 -platform_version macos 14.0 14.0 \
    -install_name "$OUT/libgreet.dylib" -o "$OUT/libgreet.dylib" "$OUT/libgreet.o" \
    -L"$SDK_MINI/usr/lib" -lSystem 2>/dev/null
ld64.lld -arch arm64 -platform_version macos 14.0 14.0 -e _main -o "$OUT/greeter" "$OUT/uselib.o" \
    -L"$OUT" -lgreet -L"$SDK_MINI/usr/lib" -lSystem 2>/dev/null
got=$("$BIN" run "$OUT/greeter" 2>/dev/null | tr '\n' '|')
case "$got" in
  *"20+22=42, shout=7"*) ok "dylib bound through the export trie" ;;
  *) bad "dylib binding" "got '$got'" ;;
esac

echo "== T4: unresolved import is an error, not a crash =="
cat > "$OUT/missing.c" <<'EOF'
typedef unsigned long size_t;
extern long write(int, const void *, size_t);
extern void not_a_real_symbol(void);
int main(void) { write(1, "x", 1); not_a_real_symbol(); return 0; }
EOF
DARWIN_SDK="$SDK_MINI" DARWIN_MINOS=14.0 "$PORT/xclang" -O2 -fno-stack-protector \
    -Wl,-undefined,dynamic_lookup -o "$OUT/missing" "$OUT/missing.c" 2>/dev/null
"$BIN" run "$OUT/missing" >/dev/null 2>&1
[ $? = 4 ] && ok "unresolved import → exit 4 (check reports the name)" \
    || bad "missing-symbol handling" "expected exit 4"

echo "== T5: Darwin dirent layout =="
DARWIN_SDK="$SDK_MINI" DARWIN_MINOS=14.0 "$PORT/xclang" -O2 -fno-stack-protector \
    -o "$OUT/dirent" "$HERE/fixtures/dirent.c" 2>/dev/null
got=$("$BIN" run "$OUT/dirent" 2>/dev/null)
[ "$got" = "dirent ok 1" ] && ok "readdir fills d_namlen/d_name at Darwin offsets" \
    || bad "dirent translation" "got '$got'"

echo "== T6: JSON import census =="
if "$BIN" report "$OUT/varargs" 2>/dev/null | grep -q '"unresolved_total": 0'; then
    ok "report mode emits per-image JSON"
else
    bad "report mode"
fi

echo "== T7: setjmp cannot smash a 192-byte Darwin jmp_buf =="
DARWIN_SDK="$SDK_MINI" DARWIN_MINOS=14.0 "$PORT/xclang" -O1 -fno-stack-protector \
    -o "$OUT/setjmp" "$HERE/fixtures/setjmp.c" 2>/dev/null
got=$("$BIN" run "$OUT/setjmp" 2>/dev/null)
[ "$got" = "longjmp=7 canary=intact" ] && ok "setjmp/longjmp sized to Darwin jmp_buf" \
    || bad "setjmp" "got '$got'"

echo "== T8: statfs uses the Darwin 2168-byte layout =="
DARWIN_SDK="$SDK_MINI" DARWIN_MINOS=14.0 "$PORT/xclang" -O1 -fno-stack-protector \
    -o "$OUT/statfs" "$HERE/fixtures/statfs.c" 2>/dev/null
got=$("$BIN" run "$OUT/statfs" 2>/dev/null)
case "$got" in
  *"fstype=apfs tail=zeroed"*) ok "statfs fills the Darwin layout, zeroes the tail" ;;
  *) bad "statfs" "got '$got'" ;;
esac

echo "== T9: build an Xcode project without Xcode =="
PB="$ROOT/pxcodebuild.py"; [ -f "$PB" ] || PB="$ROOT/../darwin-port/pxcodebuild.py"
if [ -f "$PB" ] && [ -d "${DARWIN_SDK:-/root/xc/Xcode-beta.app/Contents/Developer/Platforms/MacOSX.platform/Developer/SDKs/MacOSX.sdk}" ]; then
    SDK_REAL="${DARWIN_SDK:-/root/xc/Xcode-beta.app/Contents/Developer/Platforms/MacOSX.platform/Developer/SDKs/MacOSX.sdk}"
    python3 "$PB" build "$HERE/fixtures/Demo/Demo.xcodeproj" --sdk "$SDK_REAL" --out "$OUT/pbuild" >/dev/null 2>&1
    got=$(cd /tmp && "$BIN" run "$OUT/pbuild/Demo.app/Contents/MacOS/Demo" 2>/dev/null)
    case "$got" in
      *"built by pxcodebuild"*) ok "project.pbxproj → .app bundle → runs" ;;
      *) bad "pxcodebuild" "got '$got'" ;;
    esac
else
    skip_ "pxcodebuild" "needs the real macOS SDK"
fi

echo "== T10: real Apple toolchain (optional) =="
APPLE_BIN="${APPLE_BIN:-/root/xc/Xcode-beta.app/Contents/Developer/Toolchains/XcodeDefault.xctoolchain/usr/bin}"
if [ -x "$APPLE_BIN/ar" ]; then
    printf 'x\n' > "$OUT/member.txt"
    ( cd "$OUT" && "$BIN" run "$APPLE_BIN/ar" -rc t.a hello.o >/dev/null 2>&1 )
    if [ -s "$OUT/t.a" ] && head -c 8 "$OUT/t.a" | grep -q '!<arch>'; then
        ok "Xcode's ar writes a real archive"
    else
        bad "ar archive" "no valid !<arch> header"
    fi
    if "$BIN" run "$APPLE_BIN/otool" -h "$OUT/hello" 2>/dev/null | grep -q 0xfeedfacf; then
        ok "Xcode's otool reads a Mach header"
    else
        bad "otool" "no magic in output"
    fi
else
    skip_ "Apple toolchain tests" "set APPLE_BIN to an extracted Xcode tree"
fi

echo
echo "passed $pass, failed $fail, skipped $skip"
[ "$fail" = 0 ]
