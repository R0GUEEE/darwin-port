#!/usr/bin/env python3
"""UDIF (.dmg) extractor: koly trailer -> plist -> mish blkx -> raw HFS/APFS image.
No dependencies beyond python3 stdlib. Streams chunk by chunk (no full RAM image)."""
import struct, sys, zlib, plistlib, base64, os

KOLY = 512

def load(path):
    sz = os.path.getsize(path)
    f = open(path, 'rb')
    f.seek(sz - KOLY)
    t = f.read(KOLY)
    if t[:4] != b'koly':
        raise SystemExit('not a UDIF: no koly magic')
    (dfo, dfl) = struct.unpack('>QQ', t[24:40])
    (xmlo, xmll) = struct.unpack('>QQ', t[216:232])
    f.seek(xmlo)
    pl = plistlib.loads(f.read(xmll))
    return f, dfo, pl

def mish_chunks(data):
    assert data[:4] == b'mish', data[:4]
    ver, first_sector, num_sectors, data_off, buff_off = struct.unpack('>IQQQQ', data[4:40])
    pos = 0xCC
    while pos + 40 <= len(data):
        typ, comment, secnum, seccnt, comp_off, comp_len = struct.unpack('>IIQQQQ', data[pos:pos+40])
        if typ == 0xFFFFFFFF:
            break
        yield typ, secnum, seccnt, comp_off, comp_len
        pos += 40

def extract(dmg, out, only=None):
    f, dfo, pl = load(dmg)
    blocks = pl['resource-fork']['blkx']
    total = 0
    with open(out, 'wb') as o:
        o.truncate(0)
        for blk in blocks:
            name = blk.get('Name', '?')
            if only and only not in name:
                continue
            data = blk['Data']
            if isinstance(data, str):
                data = base64.b64decode(data)
            for typ, secnum, seccnt, coff, clen in mish_chunks(data):
                o.seek(secnum * 512)
                if typ == 0x00000000:
                    break
                if typ in (0x7FFFFFFE, 0x7FFFFFFF, 0x00000002):
                    continue
                if typ == 0x00000001:
                    buf = b'\0' * (seccnt * 512)
                else:
                    f.seek(dfo + coff)
                    raw = f.read(clen)
                    if typ == 0x80000005:
                        buf = zlib.decompress(raw)
                    elif typ == 0x80000006:
                        import bz2
                        buf = bz2.decompress(raw)
                    elif typ == 0x80000007:
                        try:
                            import lzma
                        except ImportError:
                            raise SystemExit('ADC block: needs lzma')
                        # ADC stream is not xz; handled below
                        raise SystemExit('ADC unsupported')
                    else:
                        raise SystemExit('block type %#x' % typ)
                o.seek(secnum * 512)
                o.write(buf)
                total += len(buf)
                sys.stderr.write('\r%s %8.1f MiB' % (name, total / 1048576))
    sys.stderr.write('\n')
    return total

if __name__ == '__main__':
    dmg = sys.argv[1]
    out = sys.argv[2]
    only = sys.argv[3] if len(sys.argv) > 3 else None
    n = extract(dmg, out, only)
    print('wrote', n, 'bytes to', out)
