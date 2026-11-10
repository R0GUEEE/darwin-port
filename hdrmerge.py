#!/usr/bin/env python3
"""Repair 'NULLcanary' placeholder headers in one SDK from the other SDKs.

Apple's xip stores one real copy of each unique file and replaces the rest
with the literal bytes "NULLcanary" followed by zeros.  The archive carries
12 SDKs, so almost every placeholder has a real twin elsewhere.
"""
import os, sys, shutil, collections

hdr_root = sys.argv[1]          # harvested tree with all SDKs
target   = sys.argv[2]          # SDK whose placeholders we patch

CANARY = b'NULLcanary'
real = {}
canary_count = 0
for dp, dn, fn in os.walk(hdr_root):
    if '/usr/include' not in dp:
        continue
    rel_dir = dp.split('/usr/include', 1)[1]
    for n in fn:
        p = os.path.join(dp, n)
        rel = os.path.join(rel_dir, n).lstrip('/')
        try:
            with open(p, 'rb') as f:
                head = f.read(10)
        except OSError:
            continue
        if head == CANARY:
            canary_count += 1
            continue
        if rel not in real or os.path.getsize(p) > os.path.getsize(real[rel]):
            real[rel] = p

print('harvest: %d real paths, %d placeholders seen' % (len(real), canary_count))

fixed = missing = ok = 0
inc = os.path.join(target, 'usr/include')
for dp, dn, fn in os.walk(inc):
    rel_dir = dp[len(inc):].lstrip('/')
    for n in fn:
        p = os.path.join(dp, n)
        try:
            if os.path.islink(p):
                continue
            with open(p, 'rb') as f:
                head = f.read(10)
        except OSError:
            continue
        if head != CANARY:
            ok += 1
            continue
        rel = os.path.join(rel_dir, n).lstrip('/')
        src = real.get(rel)
        if src and os.path.abspath(src) != os.path.abspath(p):
            os.chmod(p, 0o644)
            shutil.copyfile(src, p)
            fixed += 1
        else:
            missing += 1
            print('no twin for', rel)
print('patched %d, already real %d, unresolved %d' % (fixed, ok, missing))
