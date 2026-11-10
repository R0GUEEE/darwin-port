# Formats — xip, xar, pbzx, cpio-odc, dmg/UDIF, pkg

Part of the [darwin-port documentation](INDEX.md). Implementation:
`xipx.py`, `dmgx.py`, `xarx.py`, `pkgx.py`. ABI side: [ABI](ABI.md).

Every statement below was verified against the two files, not taken from
documentation.  Numbers are reproducible with the tools in this directory.

## 1. `.xip` = XAR + pbzx + odc cpio

```
Xcode_27.3_beta.xip
└── xar archive                      magic 'xar!', BE header, 28 bytes
    ├── heap[0..20]                  toc checksum   (sha1)
    ├── heap[20..276]                RSA signature  (Apple Software Update CA)
    ├── heap[276 .. +2075396748]     file "Content"
    └── heap[+0 .. 389]              file "Metadata" (389 B plist)
```

* XAR header: `magic u32 | header_size u16 | version u16 | toc_compressed u64 |
  toc_uncompressed u64 | checksum_alg u32` — 28 bytes, big endian.
* The TOC is zlib-deflated XML; `ZIP`-style member offsets are **relative to
  the heap**, which starts at `header_size + toc_compressed_size` = 3937.
  Reading `Content` from the literal offset 276 yields 9 MiB of signature
  bytes that a decompressor will happily turn into garbage.  This is the
  single most expensive trap in the format.
* `Content` is a **pbzx** stream: 16 MiB-uncompressed chunks, each a complete
  XZ stream.  Header layout found by walking the chunk table:
  `'pbzx' | flags u64 (0x01000000) | {out u64, comp u64, data[comp]}` * 622.
  Total uncompressed: 10,433,180,672 B (9.72 GiB), and the table lands
  exactly on the end of the member — a self-check the extractor uses.
* Verified: `sha1(Content) = 53a622bf007cdb198d87f3552cecd7aca30b5a80`,
  identical to the TOC's `archived-checksum`.  The XIP is byte-perfect.
* Inside is a **cpio "odc"** archive (`070707`), 76-byte octal ASCII headers,
  **no padding** of names or data (unlike `newc`), entries in path order:
  `./Xcode-beta.app/...`.

## 2. `.dmg` = UDIF (koly) + Apple_HFS volume + xar .pkg

```
Command_Line_Tools_27.3_beta.dmg   502 MiB
├── data fork (526,613,183 B)      blkx chunks, zlib
├── koly trailer (last 512 B)      magic at the START of the trailer
└── xml plist (10,832 B)           6 blkx tables: MBR, GPT x2, Apple_HFS, backup GPT
```

* `koly` sits at `file_size - 512`; fields are **big endian** (the usual
  mistake is little endian — every sector count then looks like 2^51).
* blkx `mish` header is 0xCC bytes; each 40-byte run record is
  `type u32 | comment u32 | sector u64 | count u64 | offset u64 | length u64`
  (also big endian).  `0x80000005` = zlib, `0x80000006` = bzip2,
  `0x00000001` = raw, `0x00000002` = ignore, `0xFFFFFFFF` = end.
* Decompressed: an HFS+ volume, 665,541,120 B, 16 KiB clusters, holding
  `Command Line Tools/Command Line Tools.pkg` (540,503,086 B).
* The .pkg is a product archive (XAR) of 12 sub-packages.  Payloads are
  **pbzx** too, not gzip: `CLTools_Executables.pkg/Payload` (350 MB),
  `CLTools_macOSNMOS_SDK.pkg/Payload`, `CLTools_macOSLMOS_SDK.pkg/Payload`,
  `CLTools_SwiftBackDeploy.pkg/Payload`.

### This particular .dmg is damaged

* 60 of 60 small members hash exactly to their `archived-checksum`.
* The four large payloads do not: `CLTools_Executables.pkg/Payload` hashes to
  `56c728fe…` where the archive says `5c922ce0…`, and XZ decoding dies with
  `Corrupt input data` after 144 KB of output.
* The decompressed HFS image is 95.1 % zero bytes, with 121 MB hole runs at
  file offsets 166 MB, 36 MB at 289 MB, 37 MB at 326 MB.
* The koly data-fork CRC32 is `0xd5ca3f29`; the footer stores `0x9e458a9e`.

Conclusion: a partial/holed copy, not an Apple artifact.  Nothing in it is
needed — the XIP carries the same toolchain and SDKs, and it verifies.

## 3. Mach-O dialects that matter for a loader

The Xcode toolchain images use `LC_DYLD_CHAINED_FIXUPS`.  Two different
`dyld_chained_starts_in_segment` layouts exist in the wild and they must be
told apart by the size field:

```
dyld  : size u32 | page_size u16 | page_format u16 | max_valid u64 |
        pointer_format u16 | segment_offset u64 | overflow_count u16 | page_start[]
        size == 28 + 2*page_count

LLVM  : size u32 | page_size u16 | pointer_format u16 | segment_offset u64 |
        max_valid u32 | page_count u16 | page_start[]
        size == 22 + 2*page_count        <- what ld64.lld writes
```

Reading the LLVM variant with the dyld offsets gives `pointer_format = 0` and
a 16 TB `segment_offset`.  The check `size == 22 + 2*u16@20` separates them.
