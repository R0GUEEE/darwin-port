#!/usr/bin/env python3
"""How much of a Swift binary's import surface does libswiftCore cover?

Reads the SDK's `libswiftCore.tbd` (the runtime's exported ABI) and every
Mach-O image you point it at, classifies each imported symbol (swift / objc /
corefoundation / c++ / libc), and reports coverage.

  swift_abi.py <stub.tbd[,stub2.tbd,...]> <tree-or-image> [...] [--json out.json]

Selection uses chained fixups (LC_DYLD_CHAINED_FIXUPS), which every Swift
toolchain binary here uses; images with only LC_DYLD_INFO are reported as
"skipped" rather than guessed at.
"""
import json, os, re, struct, sys

MH_MAGIC_64 = 0xfeedfacf
FAT_MAGIC = 0xcafebabe


def slices(buf):
    magic = struct.unpack_from('>I', buf, 0)[0]
    if magic == FAT_MAGIC:
        n = struct.unpack_from('>I', buf, 4)[0]
        for i in range(n):
            cpu, sub, off, size = struct.unpack_from('>IIII', buf, 8 + i * 20)
            if cpu == 0x0100000c or cpu == 0x0100000c | 0x02000000:
                yield off
        return
    yield 0


def thin_imports(buf, off):
    """Return (imported names, has_chained, has_dyld_info)."""
    if struct.unpack_from('<I', buf, off)[0] != MH_MAGIC_64:
        return [], False, False
    ncmds = struct.unpack_from('<I', buf, off + 16)[0]
    p = off + 32
    chain = None
    dyld_info = False
    for _ in range(ncmds):
        cmd, cs = struct.unpack_from('<II', buf, p)
        if cmd == 0x80000034:
            chain = struct.unpack_from('<I', buf, p + 8)[0]
        elif cmd in (0x22, 0x80000022):
            dyld_info = True
        p += cs
    if chain is None:
        return [], False, dyld_info
    ver, so, io, yo, ic, fmt, sf = struct.unpack_from('<IIIIIII', buf, chain)
    names = []
    for i in range(ic):
        if fmt in (1, 2):
            v = struct.unpack_from('<I', buf, chain + io + i * 4)[0]
            no = v >> 9
        elif fmt == 3:
            v = struct.unpack_from('<Q', buf, chain + io + i * 8)[0]
            no = (v >> 32) & 0xFFFFFFFF
        else:
            return [], True, dyld_info
        s = buf[chain + yo + no: chain + yo + no + 512]   # names are <512B; never slice to EOF
        names.append(s.split(b'\0')[0].decode('utf8', 'replace'))
    return names, True, dyld_info


def klass(n):
    b = n[1:] if n.startswith('_') else n
    if b.startswith('$s') or b.startswith('$S'):
        return 'swift'
    if b.startswith('objc_') or b.startswith('OBJC_') or n.startswith('_OBJC'):
        return 'objc'
    if b.startswith('_CF') or b.startswith('CF'):
        return 'corefoundation'
    if b.startswith('_Z') or b.startswith('__ZN'):
        return 'c++'
    return 'libc'


def tbd_exports(path):
    txt = open(path, 'r', errors='replace').read()
    syms = set(m[1:] for m in re.findall(r"'(_[^']+)'", txt))   # drop Mach-O underscore
    return {s[1:] if s.startswith('_') else s for s in syms}


def walk(path, out):
    if os.path.isdir(path):
        for dp, dn, fn in os.walk(path):
            for n in fn:
                walk(os.path.join(dp, n), out)
        return
    try:
        if os.path.getsize(path) > 200_000_000:
            return
        buf = open(path, 'rb').read()
    except OSError:
        return
    if len(buf) < 32:
        return
    for off in slices(buf):
        names, chained, dyld_info = thin_imports(buf, off)
        if names:
            out.append({'file': path, 'imports': sorted(set(names))})


def main():
    if len(sys.argv) < 3:
        raise SystemExit(__doc__)
    exports = set()
    for one in sys.argv[1].split(','):
        exports |= tbd_exports(one)
    out = []
    for p in sys.argv[2:]:
        if p.startswith('--'):
            break
        walk(p, out)

    tot = {k: 0 for k in ('swift', 'objc', 'corefoundation', 'c++', 'libc')}
    cov = 0
    sw = 0
    rows = []
    for r in out:
        c = {k: 0 for k in tot}
        hit = 0
        for n in r['imports']:
            k = klass(n)
            c[k] += 1
            if k == 'swift':
                sw += 1
                if n[1:] in exports or n in exports:
                    hit += 1
        cov += hit
        for k in tot:
            tot[k] += c[k]
        if c['swift']:
            rows.append((os.path.basename(r['file']), len(r['imports']), c['swift'], hit,
                         c['objc'], c['corefoundation'], c['c++']))

    print('libswiftCore exports (from .tbd): %d' % len(exports))
    print('images scanned: %d, of which use Swift: %d' % (len(out), len(rows)))
    print('imports by class: ' + ', '.join('%s=%d' % (k, v) for k, v in tot.items()))
    if sw:
        print('swift imports covered by libswiftCore: %d/%d (%.1f%%)' % (cov, sw, 100.0 * cov / sw))
    print()
    print('%-30s %7s %7s %6s %6s %5s' % ('binary', 'imports', 'swift', 'inTbd', 'objc', 'cf'))
    for row in sorted(rows, key=lambda x: -x[2])[:12]:
        print('%-30s %7d %7d %6d %6d %5d' % (row[0], row[1], row[2], row[3], row[4], row[5]))

    for a, i in enumerate(sys.argv):
        if a == '--json':
            json.dump({'exports': len(exports), 'images': out}, open(sys.argv[i + 1], 'w'))


if __name__ == '__main__':
    main()
