# Glossary

Terms as they are used in this project, not as a dictionary defines them.

## Containers

**XAR** — the archive format Apple uses for `.xip` and `.pkg`. 28-byte
big-endian header, zlib-deflated XML table of contents, then a heap.

**TOC** — the table of contents at the front of a XAR: one `<file>` element per
member with `<data><offset>`, `<size>`, `<encoding>`, and both an
`archived-checksum` and an `extracted-checksum`.

**heap** — everything after the TOC. **Member offsets are relative to it**, not
to the file. Getting this wrong is the single most expensive mistake in the
whole format.

**pbzx** — Apple's chunked container used for the XIP's `Content` member and for
`.pkg` payloads. `'pbzx'`, a chunk size, then `{out_size, comp_size, data}`
records; each compressed chunk is an independent XZ stream, and a chunk whose
sizes are equal is stored raw.

**odc** — the cpio variant inside pbzx: `070707`, 76-byte octal ASCII header,
**no padding**. Not `newc`, which pads to 4 bytes.

**mish / blkx** — the block-map table inside a UDIF's plist: 40-byte run records
mapping output sectors to compressed byte ranges in the data fork.

**koly** — the 512-byte UDIF trailer, magic at the *start*, all fields
big-endian.

**UDIF** — Apple's `.dmg` container: data fork + blkx block map + koly trailer.

**HFS+** — hierarchical file system; the volume inside an Apple disk image.
16 KiB clusters, B-trees for catalog and extents.

**product archive** — a `.pkg` that contains sub-packages (`CLTools_*.pkg`),
each of which has a `Payload` (a pbzx-compressed odc cpio), a `Bom` and
sometimes a `Scripts` archive.

## Mach-O

**slice** — one architecture inside a fat binary. A fat header lists
`(cputype, cpusubtype, offset, size)`.

**`__TEXT` / `__DATA_CONST` / `__DATA` / `__LINKEDIT`** — the standard segments.
`__TEXT` is executable, `__DATA_CONST` was written at link time and should be
read-only at run time, `__LINKEDIT` holds symbol tables and fixup metadata.

**load command** — a typed record in the header: segments, dylib references,
symbol table location, code signature, entry point.

**`LC_MAIN`** — where the entry point is: an offset from the start of `__TEXT`.

**`LC_SEGMENT_64`** — a mapped region: vmaddr, vmsize, fileoff, filesize,
protection.

**`LC_LOAD_DYLIB`** — "I depend on this library"; the order of these commands is
the numeric **ordinal** used by two-level namespace binds.

**fixup** — a pointer the loader must adjust before the image can run.

**rebase** — a fixup that changes a pointer by the **slide** (where the image
actually landed versus where it was linked).

**bind** — a fixup that writes the address of an imported symbol.

**lazy bind** — a bind deferred until first call, through a stub that calls
`dyld_stub_binder`. This loader binds eagerly instead.

**weak bind** — an import that may be absent. Easy to forget, and forgetting it
makes a binary look dependency-free.

**chained fixups** — modern fixup encoding (`LC_DYLD_CHAINED_FIXUPS`): pointers
linked into per-page chains by a stride held in the pointer's own high bits.
Two incompatible struct dialects exist (dyld's and LLVM's).

**export trie** — a prefix tree of a dylib's exported symbols; each terminal
node carries an address, a reexport edge, or a stub-and-resolver pair.

**two-level namespace** — symbol resolution keyed by `(library, name)` rather
than name alone. `ordinal 0` means "this image"; negative ordinals mean flat
lookup.

**slide** — the runtime load address minus the link-time address of `__TEXT`.

**`__mod_init_func`** — C/C++ constructors run by dyld before `main`.

**TLV** — thread-local variable descriptor; `__tlv_bootstrap` allocates the
per-thread block.

**dylib / framework / bundle** — all Mach-O; a framework is a directory with a
`Versions/A` inside and usually a symlink to it; a bundle (`.bundle`) is not
loadable as a library but is still Mach-O.

## SDK and signing

**`.tbd`** — a text stub describing a dylib's exports, used in place of the real
binary at link time. `tbd-version: 4` lists `targets` per export block.

**`arm64e.x1-macos`** — a target token in newer `.tbd` files meaning "arm64e,
ABI variant 1". LLD 19 rejects it; it can be dropped.

**cryptid** — `LC_ENCRYPTION_INFO_64`'s encryption id. Non-zero means FairPlay,
so the text is ciphertext keyed to a device. Every image here is 0.

**codesign / entitlements** — the signature blob and the rights it grants.
Presence is recorded (`LC_CODE_SIGNATURE`) but not verified: there is no Apple
trust store here.

**`NULLcanary`** — the literal string Apple writes into duplicate files in a
XIP. A file of the right size whose first 10 bytes are `NULLcanary` is a
placeholder, not corruption.

## ABI and toolchain

**ABI** — application binary interface: calling convention, struct layout,
symbol naming, flag values. This project is mostly an ABI project.

**AAPCS64** — the ARM 64-bit procedure call standard used by Linux. Apple
follows it except for varargs.

**varargs on the stack** — Apple arm64 passes *all* anonymous arguments in
memory at `sp` and makes `va_list` a pointer. Linux puts the first eight in
`x0`–`x7`.

**`std::__1`** — the libc++ ABI namespace. Apple's libc++ and Linux libc++ use
the same name, so C++ symbols need no translation beyond the leading underscore.

**LLD / `ld64.lld`** — LLVM's Mach-O linker; the only free one.

**xclang** — this project's wrapper that drives host clang with
`-target arm64-apple-macos`, `-isysroot <SDK>`, `-fuse-ld=lld`.

**shim** — the table of Darwin entry points implemented over the host libc.

**emulation vs translation** — this project *translates* ABI: the guest's arm64
instructions run natively. It never interprets x86_64 code.

**bind census** — the measurement used throughout: run `darwinld check` over a
corpus and count binaries with zero unresolved imports.

## Failure vocabulary

**silent corruption** — the interesting class of bug: everything "works" but a
struct field is read from the wrong offset. Four such bugs were found here.

**ENOSYS over a lie** — the rule that a call we cannot implement returns
`-1`/`ENOSYS` rather than a plausible-looking answer.
