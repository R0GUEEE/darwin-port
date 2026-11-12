#!/bin/sh
# make-tbd-stubs.sh — turn Apple's text-based .tbd stub libraries into real
# Mach-O dylibs the loader can map and bind.
#
#   ./make-tbd-stubs.sh <dir-with-.tbd> <out-dir> [--arch arm64] [-q]
#
# Why this exists: a .tbd file describes a dylib's exports but contains no
# code. `ld64.lld` links against them; `dyld` (and therefore `darwinld`) needs
# a Mach-O image with an LC_ID_DYLIB and an export trie. This script produces
# exactly that, exporting every symbol the .tbd declares.
#
# Symbols get absolute addresses (Mach-O symbols with type 'A'), so a guest that
# calls one jumps to a small integer and faults immediately — the stub is for
# *resolution*, not for execution. Loading with RTLD_LAZY and binding on first
# use turns that into a crash at the call site, which is the honest outcome.
#
# Requires: python3 and LLVM's `ld64.lld` (package `lld`).
set -eu
SRC=${1:-}; OUT=${2:-}; ARCH=arm64; QUIET=0
[ -n "$SRC" ] && [ -n "$OUT" ] || { echo "usage: $0 <.tbd dir> <out dir> [--arch arm64] [-q]"; exit 2; }
shift 2 || true
for a in "$@"; do
    case "$a" in
        --arch) shift; ARCH=$1 ;;
        -q) QUIET=1 ;;
    esac
done

mkdir -p "$OUT"
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

n=0; skip=0
for tbd in "$SRC"/*.tbd; do
    [ -f "$tbd" ] || continue
    base=$(basename "$tbd" .tbd)
    # install-name determines where the loader will look it up
    name=$(python3 - "$tbd" <<'PY'
import re, sys
txt = open(sys.argv[1], errors='replace').read()
m = re.search(r"install-name:\s*'([^']+)'", txt)
print(m.group(1) if m else '')
PY
)
    [ -n "$name" ] || { skip=$((skip+1)); continue; }
    # The loader (and dyld) find an image by the last path component of its
    # install-name, so the file must carry exactly that name — framework stubs
    # are extension-less ("Foundation"), /usr/lib ones keep ".dylib".
    outname=${name##*/}

    # one assembly file: a zero byte per symbol, each labelled
    python3 - "$tbd" "$TMP/$base.s" <<'PY'
import re, sys
txt = open(sys.argv[1], errors='replace').read()
syms = []
# .tbd lists C names bare and only quotes what needs it, so split the block on
# commas instead of hunting for quoted strings — a quote-only match silently
# dropped every unprefixed symbol (that is how _swift_retain went missing).
for m in re.finditer(r"symbols:\s*\[(.*?)\]", txt, re.S):
    for tok in m.group(1).split(','):
        tok = tok.strip().strip("'").strip()
        if tok:
            syms.append(tok)
# objc metadata lives in its own blocks; a guest referencing the class needs
# both the class and meta-class symbols
for m in re.finditer(r"objc-classes:\s*\[(.*?)\]", txt, re.S):
    for tok in m.group(1).split(','):
        tok = tok.strip().strip("'").strip()
        if tok:
            syms.append('_OBJC_CLASS_$_' + tok)
            syms.append('_OBJC_METACLASS_$_' + tok)
seen, out = set(), []
out.append('	.section __DATA,__stubs_marker,regular')
out.append('	.globl _ds_stub_marker')
out.append('_ds_stub_marker:')
out.append('	.space 1')
for s in syms:
    if not s or s in seen:
        continue
    seen.add(s)
    label = s if s.startswith('_') else '_' + s
    # Mach-O symbols may contain $ and . but not whitespace; quote the rest
    if re.search(r'[\s"\'/]', label):
        continue
    # a `.set` symbol only survives into the object (and hence the archive's
    # export trie) if it carries .globl — an unlabelled .set is silently local
    out.append('	.globl %s' % label)
    out.append('	.set %s, 0x1000' % label)
open(sys.argv[2], 'w').write('\n'.join(out) + '\n')
PY

    if ! clang -target "$ARCH-apple-macos14" -c "$TMP/$base.s" -o "$TMP/$base.o" 2>"$TMP/$base.err"; then
        skip=$((skip+1)); [ $QUIET -eq 1 ] || echo "skip $base (asm: $(head -1 "$TMP/$base.err"))" >&2
        continue
    fi
    if ! ld64.lld -dylib -arch "$ARCH" -platform_version macos 14.0 14.0 \
            -install_name "$name" -o "$OUT/$outname" "$TMP/$base.o" \
            -undefined dynamic_lookup 2>"$TMP/$base.linkerr"; then
        skip=$((skip+1)); [ $QUIET -eq 1 ] || echo "skip $base (link: $(head -1 "$TMP/$base.linkerr"))" >&2
        continue
    fi
    n=$((n+1))
done
echo "built $n stub dylibs, skipped $skip, out=$OUT"
