#!/usr/bin/env python3
"""Verify an Apple .xip end to end: TOC checksum, TOC signature, cert chain,
and the archive's own member checksum for `Content`.

  verify_xip.py <file.xip>

Prints a verdict and exits 0 only if every step passes.  Needs `openssl` on
PATH for the RSA and chain steps; the checksum and member-hash steps are
stdlib-only.
"""
import base64, hashlib, os, re, struct, subprocess, sys, tempfile, zlib

MEMBER = 'Content'


def run(cmd):
    return subprocess.run(cmd, capture_output=True, text=True)


def main():
    if len(sys.argv) != 2:
        print(__doc__)
        return 2
    path = sys.argv[1]
    size = os.path.getsize(path)
    ok = True

    with open(path, 'rb') as f:
        magic, hsz, ver, tc, tu, ck = struct.unpack('>IHHQQI', f.read(28))
        assert magic == 0x78617221, 'not a xar/xip'
        toc_z = f.read(tc)
        toc = zlib.decompress(toc_z)
        heap = hsz + tc
        f.seek(heap)
        heap_head = f.read(1024)

    print('%-28s %s' % ('file', os.path.basename(path)))
    print('%-28s %d bytes  (xar v%d, toc %d/%d bytes compressed/uncompressed)' %
          ('container', size, ver, tc, tu))

    # 1. TOC checksum (sha1, stored at heap+0)
    stored_chk = heap_head[0:20]
    calc = hashlib.sha1(toc_z).digest()
    same = stored_chk == calc
    ok &= same
    print('%-28s %s  (%s)' % ('TOC sha1', 'MATCH' if same else 'MISMATCH', calc.hex()))

    # 2. RSA signature over the TOC (stored at heap+20)
    sig_el = re.search(rb'<signature[^>]*>(.*?)</signature>', toc, re.S)
    sig_off = sig_len = None
    if sig_el:
        sig_off = int(re.search(rb'<offset>(\d+)</offset>', sig_el.group(1)).group(1))
        sig_len = int(re.search(rb'<size>(\d+)</size>', sig_el.group(1)).group(1))
    sig = heap_head[sig_off:sig_off + sig_len] if sig_off is not None else b''
    certs = [base64.b64decode(re.sub(rb'\s', b'', c))
             for c in re.findall(rb'<X509Certificate>(.*?)</X509Certificate>', toc, re.S)]
    print('%-28s %d bytes RSA, %d certificate(s) in TOC' % ('signature', len(sig), len(certs)))

    tmp = tempfile.mkdtemp()
    def w(name, data):
        p = os.path.join(tmp, name)
        open(p, 'wb').write(data)
        return p

    if certs and sig:
        chain_ok = True
        for i, der in enumerate(certs):
            r = run(['openssl', 'x509', '-inform', 'DER', '-in', w('c%d.der' % i, der),
                     '-noout', '-subject'])
            print('%-28s %s' % ('  cert %d' % i, r.stdout.strip().replace('subject=', '')))
        pub = run(['openssl', 'x509', '-inform', 'DER', '-in', w('leaf.der', certs[0]),
                   '-pubkey', '-noout'])
        w('pub.pem', pub.stdout.encode())
        w('toc.z', toc_z)
        w('sig.bin', sig)
        v = run(['openssl', 'dgst', '-sha1', '-verify', os.path.join(tmp, 'pub.pem'),
                 '-signature', os.path.join(tmp, 'sig.bin'), os.path.join(tmp, 'toc.z')])
        good = 'Verified OK' in v.stdout
        ok &= good
        print('%-28s %s' % ('RSA(TOC) with leaf key', 'VERIFIED' if good else 'FAILED'))
        for i in range(1, len(certs)):
            run(['openssl', 'x509', '-inform', 'DER', '-in', w('a.der', certs[i - 1]), '-out',
                 os.path.join(tmp, 'a.pem')])
            run(['openssl', 'x509', '-inform', 'DER', '-in', w('b.der', certs[i]), '-out',
                 os.path.join(tmp, 'b.pem')])
            v = run(['openssl', 'verify', '-partial_chain', '-CAfile',
                     os.path.join(tmp, 'b.pem'), os.path.join(tmp, 'a.pem')])
            good = ': OK' in v.stdout
            chain_ok &= good
            print('%-28s %s' % ('  cert %d signed by cert %d' % (i - 1, i), 'OK' if good else 'FAIL'))
        ok &= chain_ok

    # 3. the archive's own member checksum for Content
    m = None
    for blk in re.findall(rb'<file\b.*?</file>', toc, re.S):
        if b'<name>' + MEMBER.encode() + b'</name>' in blk:
            m = re.search(rb'<offset>(\d+)</offset>.*?<size>(\d+)</size>.*?'
                          rb'<archived-checksum[^>]*>([0-9a-fA-F]+)</archived-checksum>',
                          blk, re.S)
            break
    if m:
        off, length, want = int(m.group(1)), int(m.group(2)), m.group(3).decode().lower()
        h = hashlib.sha1()
        with open(path, 'rb') as f:
            f.seek(heap + off)
            left = length
            while left:
                b = f.read(min(4 << 20, left))
                if not b:
                    break
                h.update(b); left -= len(b)
        got = h.hexdigest()
        good = got == want
        ok &= good
        print('%-28s %s' % ('sha1(%s)' % MEMBER, 'MATCH' if good else 'MISMATCH'))

    print()
    print('VERDICT: %s' % ('authentic — signed by Apple and internally consistent'
                           if ok else 'FAILED a check above'))
    return 0 if ok else 1


if __name__ == '__main__':
    sys.exit(main())
