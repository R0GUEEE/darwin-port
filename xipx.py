#!/usr/bin/env python3
"""XIP (.xip) extractor for Apple Xcode archives.

Container: XAR  ->  member "Content"  ->  pbzx stream (16 MiB xz chunks)
           -> cpio "odc" archive (070707) with unpadded headers/data.

Usage:
  xipx.py list   <xip> [-o manifest.json]
  xipx.py extract <xip> <dest> [--include PREFIX ...] [--manifest m.json]

Extraction is single-pass and streaming: memory stays at one chunk (~16 MiB),
so it works on a 4 GB device with a 30 GB archive.
"""
import struct, sys, os, json, stat, lzma, hashlib, time

XZ_MAGIC = b'\xfd7zXZ\x00'


def xar_header(f):
    h = f.read(28)
    magic, hsz, ver, tc, tu, ck = struct.unpack('>IHHQQI', h)
    if magic != 0x78617221:
        raise SystemExit('not a xar/xip')
    return hsz, tc, tu


def content_offset(f):
    """Find the payload offset of the member named Content."""
    import zlib, xml.etree.ElementTree as ET
    hsz, tc, tu = xar_header(f)
    toc = zlib.decompress(f.read(tc))
    root = ET.fromstring(toc)
    for el in root.iter('file'):
        name = el.findtext('name')
        if name == 'Content':
            # offsets are relative to the xar heap, which starts after header+toc
            return hsz + tc + int(el.findtext('data/offset')), int(el.findtext('data/size'))
    raise SystemExit('no Content member')


class Pbzx:
    """Streaming reader over a pbzx stream, driven by its chunk table.

    Layout (verified):
        'pbzx' | chunk_size u64 | { out_size u64, comp_size u64, data[] } *
    A chunk whose comp_size equals out_size is stored, not XZ compressed;
    that is how the archive switches to incompressible data further in.
    """

    def __init__(self, f, start, end):
        self.f = f
        self.end = end
        self.pos = start + 4
        f.seek(self.pos)
        self.chunk_size = struct.unpack('>Q', f.read(8))[0] or 0x1000000
        self.pos += 8
        self.buf = bytearray()
        self.chunks = 0

    def _next_chunk(self):
        if self.pos + 16 > self.end:
            return False
        self.f.seek(self.pos)
        out_sz, comp = struct.unpack('>QQ', self.f.read(16))
        if not (0 < comp <= 64 << 20 and 0 < out_sz <= 64 << 20):
            return False
        self.f.seek(self.pos + 16)
        raw = self.f.read(comp)
        if len(raw) < comp:
            return False
        self.pos += 16 + comp
        self.chunks += 1
        if comp == out_sz:
            self.buf += raw                      # stored chunk
        else:
            try:
                self.buf += lzma.decompress(raw)
            except lzma.LZMAError as e:
                raise SystemExit('chunk %d at %d: %s' % (self.chunks, self.pos, e))
        return True

    def read(self, n):
        while len(self.buf) < n:
            if not self._next_chunk():
                break
        if not self.buf:
            return b''
        out = bytes(self.buf[:n])
        del self.buf[:n]
        return out


def odc_entries(rd):
    """Yield (name, mode, size, data_reader_or_bytes) from an odc cpio stream."""
    while True:
        hdr = rd.read(76)
        if len(hdr) < 76:
            return
        if hdr[:6] != b'070707':
            # resync / stop
            if hdr[:6] == b'\0' * 6:
                return
            raise SystemExit('bad cpio magic %r' % hdr[:16])
        try:
            mode = int(hdr[18:24], 8)
            nlink = int(hdr[36:42], 8)
            mtime = int(hdr[48:59], 8)
            namesize = int(hdr[59:65], 8)
            filesize = int(hdr[65:76], 8)
        except ValueError:
            raise SystemExit('bad odc header %r' % hdr)
        name = rd.read(namesize)
        if not name:
            return
        name = name[:-1].decode('utf-8', 'surrogateescape')
        if name == 'TRAILER!!!':
            return
        yield name, mode, filesize, mtime


def main():
    if len(sys.argv) < 3:
        raise SystemExit(__doc__)
    cmd, xip = sys.argv[1], sys.argv[2]
    dest = None
    includes, excludes, patterns, manifest_path = [], [], [], None
    args = sys.argv[3:]
    i = 0
    while i < len(args):
        a = args[i]
        if a == '--include':
            includes.append(args[i + 1]); i += 2
        elif a == '--exclude':
            excludes.append(args[i + 1]); i += 2
        elif a == '--pattern':
            patterns.append(args[i + 1]); i += 2
        elif a in ('-o', '--manifest'):
            manifest_path = args[i + 1]; i += 2
        else:
            dest = a; i += 1

    f = open(xip, 'rb')
    off, size = content_offset(f)
    rd = Pbzx(f, off, off + size)

    seen = 0
    total = 0
    man = open(manifest_path, 'w') if manifest_path else None
    if man:
        man.write('[\n')
    t0 = time.time()
    first = True
    for name, mode, filesize, mtime in odc_entries(rd):
        rel = name.lstrip('./')
        seen += 1
        if man:
            man.write(('' if first else ',\n') + json.dumps(
                {'p': rel, 'm': oct(mode), 's': filesize}))
            first = False
        want = dest is not None and (not includes or any(
            rel == p or rel.startswith(p.rstrip('/') + '/') for p in includes))
        if not want and patterns:
            import fnmatch
            want = any(fnmatch.fnmatch(rel, p) for p in patterns)
        if want and any(x in rel for x in excludes):
            want = False
        if not want:
            # still must consume the data
            left = filesize
            while left > 0:
                b = rd.read(min(left, 1 << 20))
                if not b:
                    break
                left -= len(b)
            continue
        full = os.path.join(dest, rel)
        if stat.S_ISDIR(mode):
            os.makedirs(full, exist_ok=True)
            os.chmod(full, (mode & 0o7777) | 0o700)
        elif stat.S_ISLNK(mode):
            target = rd.read(filesize).decode('utf-8', 'surrogateescape')
            os.makedirs(os.path.dirname(full), exist_ok=True)
            if os.path.lexists(full):
                os.unlink(full)
            os.symlink(target, full)
        elif stat.S_ISREG(mode):
            os.makedirs(os.path.dirname(full), exist_ok=True)
            with open(full, 'wb') as o:
                left = filesize
                while left > 0:
                    b = rd.read(min(left, 1 << 20))
                    if not b:
                        break
                    o.write(b)
                    left -= len(b)
            os.chmod(full, (mode & 0o7777) | 0o600)
        total += filesize
        if seen % 2000 == 0:
            sys.stderr.write('\r%d entries  %.1f MiB out  %.0fs' %
                             (seen, total / 1048576.0, time.time() - t0))
            sys.stderr.flush()
    if man:
        man.write('\n]\n')
        man.close()
    sys.stderr.write('\rdone: %d entries, %.1f MiB, %.0fs\n' %
                     (seen, total / 1048576.0, time.time() - t0))


if __name__ == '__main__':
    main()
