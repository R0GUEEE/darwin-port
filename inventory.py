#!/usr/bin/env python3
"""Inventory a Darwin tree: every Mach-O, its architectures, dylibs and imports.

  inventory.py <dir> [-o out.json] [--top N]

Fast path: raw header parsing (no llvm-objdump per file).  Reports, for each
image, how expensive it would be to run under a libSystem shim: the number of
distinct imported symbols, and how many come from the C++/ObjC/runtime dylibs
that a shim cannot fake.
"""
import os, sys, json, struct, collections

MH_MAGIC = 0xfeedface
MH_MAGIC_64 = 0xfeedfacf
FAT_MAGIC = 0xcafebabe

LC_SEGMENT_64 = 0x19
LC_SYMTAB = 0x02
LC_LOAD_DYLIB = 0x0c
LC_LOAD_WEAK_DYLIB = 0x80000018
LC_REEXPORT_DYLIB = 0x8000001f
LC_LOAD_UPWARD = 0x80000023
LC_DYLD_INFO = 0x80000022
LC_DYLD_CHAINED = 0x80000034

CPU = {0x0100000c: 'arm64', 0x01000007: 'x86_64', 0x7: 'i386', 0xc: 'arm',
       0x0100000c + 0x02000000: 'arm64e'}
FTYPE = {1: 'object', 2: 'exec', 3: 'fvmlib', 4: 'core', 5: 'preload',
         6: 'dylib', 7: 'dylinker', 8: 'bundle', 9: 'dylib_stub', 10: 'dsym',
         11: 'kext'}


def thin_info(d, off, endian='<'):
    magic, cpu, sub, ft, ncmds, szcmds = struct.unpack_from(endian + 'IiiIII', d, off)
    if magic not in (MH_MAGIC_64, MH_MAGIC):
        return None
    hdrsize = 32 if magic == MH_MAGIC_64 else 28
    p = off + hdrsize
    dylibs, syms, chained, dyld_info = [], 0, False, False
    while p + 8 <= off + hdrsize + szcmds and p + 8 <= len(d):
        cmd, cs = struct.unpack_from(endian + 'II', d, p)
        if cmd in (LC_LOAD_DYLIB, LC_LOAD_WEAK_DYLIB, LC_REEXPORT_DYLIB, LC_LOAD_UPWARD):
            nameoff = struct.unpack_from(endian + 'I', d, p + 8)[0]
            s = d[p + nameoff:p + nameoff + 256].split(b'\0')[0].decode('utf8', 'replace')
            dylibs.append(s)
        elif cmd == LC_SYMTAB:
            syms = struct.unpack_from(endian + 'I', d, p + 12)[0]
        elif cmd == LC_DYLD_CHAINED:
            chained = True
        elif cmd == LC_DYLD_INFO:
            dyld_info = True
        p += cs
    return dict(cpu=CPU.get(cpu, hex(cpu)), subtype=sub & 0xff, ftype=FTYPE.get(ft, str(ft)),
                dylibs=dylibs, nsyms=syms, chained=chained,
                fixups='chained' if chained else ('dyld_info' if dyld_info else 'none'))


def info(path):
    try:
        sz = os.path.getsize(path)
        if sz < 32 or sz > 2_000_000_000:
            return None
        with open(path, 'rb') as f:
            # load commands and LC_STR names can sit past the first page
            d = f.read() if sz <= 40_000_000 else f.read(1 << 20)
            magic = struct.unpack_from('>I', d, 0)[0]
            if magic in (FAT_MAGIC, 0xcafebabf):
                n = struct.unpack_from('>I', d, 4)[0]
                slices = []
                f.seek(0)
                head = f.read(8 + 20 * n)
                for i in range(n):
                    cpu, sub, off, size = struct.unpack_from('>IIII', head, 8 + i * 20)
                    slices.append((CPU.get(cpu, hex(cpu)), off))
                res = {'file': path, 'slices': []}
                f.seek(0)
                blob = f.read()
                for cpu, off in slices:
                    t = thin_info(blob, off)
                    if t:
                        t['cpu'] = cpu
                        res['slices'].append(t)
                return res if res['slices'] else None
            le = struct.unpack_from('<I', d, 0)[0]
            be = struct.unpack_from('>I', d, 0)[0]
            if le in (MH_MAGIC_64, MH_MAGIC):
                t = thin_info(d, 0, '<')
            elif be in (MH_MAGIC_64, MH_MAGIC):
                t = thin_info(d, 0, '>')
            else:
                t = None
            return {'file': path, 'slices': [t]} if t else None
    except Exception:
        return None
    return None


def main():
    root = sys.argv[1]
    outp = None
    top = 25
    args = sys.argv[2:]
    for i, a in enumerate(args):
        if a == '-o': outp = args[i + 1]
        if a == '--top': top = int(args[i + 1])

    rows = []
    seen = 0
    for dp, dn, fn in os.walk(root):
        for n in fn:
            p = os.path.join(dp, n)
            seen += 1
            r = info(p)
            if r:
                rows.append(r)
        if seen % 20000 < len(fn):
            sys.stderr.write('\rscanned %d files, %d mach-o' % (seen, len(rows)))

    arch = collections.Counter()
    kind = collections.Counter()
    deps = collections.Counter()
    for r in rows:
        for s in r['slices']:
            arch[s['cpu']] += 1
            kind[s['ftype']] += 1
            for d in s['dylibs']:
                deps[d] += 1
    print('\n%d files scanned, %d Mach-O images' % (seen, len(rows)))
    print('architectures :', dict(arch))
    print('file types    :', dict(kind))
    print('\ntop dylib dependencies:')
    for d, c in deps.most_common(top):
        print('%6d  %s' % (c, d))

    if outp:
        json.dump(rows, open(outp, 'w'))

    # cheap-to-run candidates: executables with the fewest dylibs
    ex = [r for r in rows if any(s['ftype'] == 'exec' for s in r['slices'])]
    ex.sort(key=lambda r: (len(r['slices'][0]['dylibs']), os.path.getsize(r['file'])))
    print('\nlightest executables (fewest dylibs):')
    for r in ex[:top]:
        s = r['slices'][0]
        print('%6.2f MB  %-4d dylibs  %-14s %s' % (
            os.path.getsize(r['file']) / 1e6, len(s['dylibs']),
            '/'.join(x['cpu'] for x in r['slices']), r['file'].replace(root + '/', '')))


if __name__ == '__main__':
    main()
