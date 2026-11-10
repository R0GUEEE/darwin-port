#!/usr/bin/env python3
"""Minimal xar (.pkg / .xip) reader: list members, extract by name, decode encodings.

Encodings handled: application/x-gzip, application/x-bzip2, application/x-xz,
application/octet-stream (raw), and the pkg payload variants (gzip/xz/raw cpio).

  xarx.py list <archive>
  xarx.py cat  <archive> <member> > out.bin
"""
import struct, sys, zlib, gzip, bz2, lzma, io
import xml.etree.ElementTree as ET


class Xar:
    def __init__(self, path):
        self.f = open(path, 'rb')
        h = self.f.read(28)
        magic, self.hsz, self.ver, tc, tu, self.ck = struct.unpack('>IHHQQI', h)
        if magic != 0x78617221:
            raise SystemExit('not a xar')
        toc = zlib.decompress(self.f.read(tc))
        self.root = ET.fromstring(toc)
        self.heap = self.hsz + tc

    def walk(self, node=None, prefix=''):
        node = node if node is not None else self.root.find('toc')
        for el in node.findall('file'):
            name = el.findtext('name')
            path = prefix + name
            d = el.find('data')
            if d is not None:
                yield path, el
            else:
                for x in self.walk(el, path + '/'):
                    yield x

    def read(self, el):
        d = el.find('data')
        off = int(d.findtext('offset'))
        ln = int(d.findtext('size'))
        enc = d.find('encoding')
        self.f.seek(self.heap + off)
        raw = self.f.read(ln)
        return raw, (enc.get('style') if enc is not None else None)

    def decode(self, raw, style):
        if not style or 'octet-stream' in style:
            return raw
        if 'gzip' in style:
            return gzip.decompress(raw)
        if 'bzip2' in style:
            return bz2.decompress(raw)
        if 'xz' in style or 'lzma' in style:
            return lzma.decompress(raw)
        raise SystemExit('unknown encoding %r' % style)


if __name__ == '__main__':
    a = Xar(sys.argv[1])
    cmd = sys.argv[2]
    if cmd == 'list':
        for p, el in a.walk():
            d = el.find('data')
            enc = d.find('encoding')
            style = enc.get('style') if enc is not None else 'raw'
            print('%12s  %-22s %s' % (d.findtext('size'), style, p))
    elif cmd == 'cat':
        want = sys.argv[3]
        for p, el in a.walk():
            if p == want:
                raw, style = a.read(el)
                sys.stdout.buffer.write(a.decode(raw, style))
                break
        else:
            raise SystemExit('no such member: ' + want)
    elif cmd == 'raw':          # cat without decoding (payload streams)
        want = sys.argv[3]
        for p, el in a.walk():
            if p == want:
                raw, style = a.read(el)
                sys.stderr.write('encoding=%s\n' % style)
                sys.stdout.buffer.write(raw)
                break
