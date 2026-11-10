#!/usr/bin/env python3
"""Inventory an SDK's text-based API (.tbd) surface.

  sdk_api.py <SDK dir> [-o out.json] [--top N]

Counts, per stub: C symbols, Objective-C classes, weak symbols, reexported
libraries, and the platform targets it declares. Builds the reexport graph so
the API-level dependency chain is visible (e.g. Foundation → CoreFoundation →
libSystem) without reading a single binary.
"""
import json, os, re, sys, collections


def parse(path):
    try:
        txt = open(path, 'r', errors='replace').read()
    except OSError:
        return None
    rec = {'path': path, 'install': None, 'targets': [],
           'symbols': 0, 'objc_classes': 0, 'objc_ivars': 0, 'weak': 0,
           'reexport_libs': [], 'reexport_symbols': 0, 'swift_abi': None}

    m = re.search(r"install-name:\s*'([^']+)'", txt)
    if m:
        rec['install'] = m.group(1)
    m = re.search(r'targets:\s*\[([^\]]*)\]', txt)
    if m:
        rec['targets'] = [t.strip() for t in m.group(1).split(',') if t.strip()]
    m = re.search(r'swift-abi-version:\s*(\d+)', txt)
    if m:
        rec['swift_abi'] = int(m.group(1))

    def blocks(txt):
        """linear scan: yields (key, body) for every `key: [ ... ]` block"""
        i, n = 0, len(txt)
        pat = re.compile(r"([a-z-]+):\s*\[")
        while i < n:
            m = pat.search(txt, i)
            if not m:
                return
            j, depth, start = m.end(), 1, m.end()
            while j < n and depth:
                c = txt[j]
                if c == '[':
                    depth += 1
                elif c == ']':
                    depth -= 1
                j += 1
            yield m.group(1), txt[start:j - 1]
            i = j

    counts = collections.Counter()
    for key, body in blocks(txt):
        if key in ('objc-classes', 'objc-ivars'):
            counts[key] += len([t for t in body.split(',') if t.strip()])
        elif key == 'libraries':
            rec['reexport_libs'] += re.findall(r"'([^']+)'", body)
        else:
            counts[key] += len(re.findall(r"'[^']*'", body))
    rec['symbols'] = counts['symbols']
    rec['objc_classes'] = counts['objc-classes']
    rec['objc_ivars'] = counts['objc-ivars']
    rec['weak'] = counts['weak-symbols']
    rec['reexport_symbols'] = counts['symbols'] if False else 0
    return rec


def main():
    root = sys.argv[1]
    out = None
    top = 15
    a = sys.argv[2:]
    for i, x in enumerate(a):
        if x == '-o':
            out = a[i + 1]
        if x == '--top':
            top = int(a[i + 1])

    stubs = []
    seen = set()
    for dp, dn, fn in os.walk(root):
        for n in fn:
            if n.endswith('.tbd'):
                real = os.path.realpath(os.path.join(dp, n))
                if real in seen:      # symlinked stubs (Framework.tbd -> Versions/A/)
                    continue
                seen.add(real)
                r = parse(os.path.join(dp, n))
                if r is None:
                    continue
                r['name'] = n[:-4]
                stubs.append(r)

    tot = collections.Counter()
    for s in stubs:
        for k in ('symbols', 'objc_classes', 'objc_ivars', 'weak', 'reexport_symbols'):
            tot[k] += s[k]
    print('%d stub libraries' % len(stubs))
    print('  C symbols        %6d' % tot['symbols'])
    print('  ObjC classes     %6d' % tot['objc_classes'])
    print('  ObjC ivars       %6d' % tot['objc_ivars'])
    print('  weak symbols     %6d' % tot['weak'])
    print('  reexported syms  %6d' % tot['reexport_symbols'])

    print('\nmost C symbols:')
    for s in sorted(stubs, key=lambda x: -x['symbols'])[:top]:
        print('  %6d  %s' % (s['symbols'], s['name']))
    print('\nmost ObjC classes:')
    for s in sorted(stubs, key=lambda x: -x['objc_classes'])[:top]:
        print('  %6d  %s' % (s['objc_classes'], s['name']))

    by_install = {s['install']: s for s in stubs if s['install']}
    edges = []
    for s in stubs:
        for lib in s['reexport_libs']:
            t = by_install.get(lib)
            if t:
                edges.append([s['name'], t['name']])
    print('\nreexport edges (API-level dependency graph): %d' % len(edges))
    indeg = collections.Counter(e[1] for e in edges)
    print('  most reexported:', ', '.join('%s(%d)' % (k, v) for k, v in indeg.most_common(6)))

    if out:
        json.dump({'stubs': stubs, 'reexports': edges}, open(out, 'w'), indent=1)
        print('\nwrote', out)


if __name__ == '__main__':
    main()
