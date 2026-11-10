#!/usr/bin/env python3
"""Stream a .pkg (xar) payload through pbzx/gzip/xz into a cpio extraction.

  pkgx.py <pkg> <member> <dest>       extract one payload member
  pkgx.py <pkg> --list-members
"""
import sys, os, stat, gzip, lzma, bz2
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from xarx import Xar
from xipx import Pbzx, odc_entries


def main():
    pkg, member, dest = sys.argv[1], sys.argv[2], sys.argv[3]
    a = Xar(pkg)
    el = None
    for p, e in a.walk():
        if p == member:
            el = e
            break
    if el is None:
        raise SystemExit('no member ' + member)
    d = el.find('data')
    off = int(d.findtext('offset')); ln = int(d.findtext('size'))
    start = a.heap + off
    a.f.seek(start)
    magic = a.f.read(8)
    if magic[:4] == b'pbzx':
        rd = Pbzx(a.f, start, start + ln)
    elif magic[:2] == b'\x1f\x8b':
        rd = _Whole(gzip.decompress(a.f.read(ln)))
    elif magic == b'\xfd7zXZ\x00\x00':
        rd = _Whole(lzma.decompress(a.f.read(ln)))
    else:
        rd = _Whole(a.f.read(ln))

    n = 0
    tot = 0
    for name, mode, filesize, mtime in odc_entries(rd):
        rel = name.lstrip('./')
        if rel.startswith('..'):
            rel = rel.replace('../', '')
        full = os.path.join(dest, rel)
        n += 1
        if stat.S_ISDIR(mode):
            os.makedirs(full, exist_ok=True)
            os.chmod(full, (mode & 0o7777) | 0o700)
        elif stat.S_ISLNK(mode):
            t = rd.read(filesize).decode('utf-8', 'surrogateescape')
            os.makedirs(os.path.dirname(full), exist_ok=True)
            if os.path.lexists(full):
                os.unlink(full)
            os.symlink(t, full)
        else:
            os.makedirs(os.path.dirname(full), exist_ok=True)
            with open(full, 'wb') as o:
                left = filesize
                while left > 0:
                    b = rd.read(min(left, 1 << 20))
                    if not b:
                        break
                    o.write(b); left -= len(b)
            os.chmod(full, (mode & 0o7777) | 0o600)
            tot += filesize
        if n % 1000 == 0:
            sys.stderr.write('\r%d files %.1f MiB' % (n, tot / 1048576.0))
            sys.stderr.flush()
    sys.stderr.write('\n')


class _Whole:
    def __init__(self, b):
        self.b = b; self.p = 0
    def read(self, n):
        o = self.b[self.p:self.p + n]; self.p += len(o); return o


if __name__ == '__main__':
    main()
