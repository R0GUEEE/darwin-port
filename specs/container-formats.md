# Spec — Apple container formats (XIP, DMG, pkg)

Byte-level contract used by `scripts/xipx.py`, `dmgx.py`, `xarx.py`, `pkgx.py`.
Each clause was required to extract a real archive; several cost a full pass to
discover.

## 1. XAR (`.xip`, `.pkg`)

```
offset 0   magic 'xar!' (0x78617221, BE)
           header_size  u16   (28)
           version      u16   (1)
           toc_compressed   u64
           toc_uncompressed u64
           checksum_alg u32   (1 = sha1)
offset 28  TOC — zlib-deflated XML
heap       = header_size + toc_compressed_size
```

1. **Every member offset in the TOC is relative to the heap, not the file.**
   In this XIP the heap is at 3937 and `Content` is at heap+276 = 4213.
2. The heap begins with the TOC checksum (20 B) and the RSA signature (256 B),
   both recorded by offset inside the TOC.
3. `archived-checksum` covers the bytes as stored; `extracted-checksum` covers
   the decompressed form. They differ only when `encoding` is present.
4. Member `encoding` styles seen: `application/x-gzip`, `application/octet-stream`.
   `.pkg` payloads use neither — they are `pbzx`.

## 2. pbzx (Xcode `Content`, `.pkg` payloads)

```
'pbzx' | chunk_size u64 | { out_size u64 | comp_size u64 | data[comp_size] } *
```

1. `chunk_size` is 0x01000000 (16 MiB) in every observed archive.
2. If `comp_size == out_size` the chunk is **stored**, not XZ. Archives switch
   to stored chunks when data stops compressing (cpio headers, zero-filled
   regions). Scanning for XZ magic instead of reading the table truncates the
   archive silently.
3. Each compressed chunk is a complete XZ stream, decodable independently.
4. Verify the table: `sum(out_size)` must land exactly on the end of the member.
   This XIP: 622 chunks, 10,433,180,672 bytes.

## 3. cpio "odc" (the archive inside)

```
'070707' | dev[6] ino[6] mode[6] uid[6] gid[6] nlink[6] rdev[6]
         | mtime[11] namesize[6] filesize[11]     (all octal ASCII, 76 bytes)
name (namesize bytes, NUL-terminated)
data (filesize bytes)
```

1. **No padding** of names or data. Walking with `newc`'s 4-byte alignment
   desynchronises on the second entry.
2. `filesize == 0` with `nlink > 1` are additional hard links; the last link
   carries the data.
3. End of archive is a `TRAILER!!!` entry.

## 4. UDIF (`.dmg`)

```
EOF-512   'koly' | version | header_size | flags | running_data_fork_offset
          | data_fork_offset | data_fork_length | rsrc_fork_offset | ...
          | segment_number | segment_count | segment_id[16]
          | checksum fields | xml_offset | xml_length | ... | checksum[32]
          | image_variant | sector_count
```

1. The magic sits at the **start** of the 512-byte trailer, not at its end.
2. All fields are **big-endian** (little-endian reads make every sector count
   look like 2^51).
3. The XML plist at `xml_offset` lists block maps (`blkx`), base64 in
   `<data>`, each a `mish` table:
   `'mish' | version | first_sector | sector_count | data_offset | buffers | ...`
   then 40-byte run records at 0xCC:
   `type u32 | comment u32 | sector u64 | count u64 | offset u64 | length u64`.
   Types: `0x80000005` zlib, `0x80000006` bzip2, `0x00000001` raw,
   `0x00000002` ignore, `0xFFFFFFFF` end.
4. A `NULLcanary` placeholder (see extraction.md) is a *file* of the correct
   size whose first 10 bytes are `NULLcanary` and the rest zero. It is the
   archive's deduplication marker, not corruption.

## 5. Acceptance tests

| check | expected |
|---|---|
| `sha1(Content)` vs TOC `archived-checksum` | equal |
| chunk table end vs member end | equal, 0 bytes slack |
| extracted cpio entries | 167,522 |
| extracted file bytes | 9.68 GiB |
