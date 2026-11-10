# Architecture

## The problem

Apple ships a compiler toolchain as **Mach-O arm64 binaries that import from
`libSystem.B.dylib` at runtime**. Running them off macOS is not a format
conversion problem, it is an ABI problem: the instruction set is the same
(arm64 is arm64), the calling convention is nearly the same, but the C library
they call into does not exist and several data structures disagree.

Three layers are possible, cheapest first:

| layer | what it buys | what it cannot do |
|---|---|---|
| **shim** (a `.so` of translated functions) | recompiled Darwin *source* runs | needs source or an object file; useless for a shipped binary |
| **convert** (Mach-O → ELF) | a single image becomes a Linux executable | no dyld: no dylibs, no lazy binding, no two-level namespace |
| **loader** (this project) | a Mach-O image runs *as* Mach-O, with dyld's semantics reimplemented | must reimplement mapping, fixups, symbol resolution, entry |

**The rule that decides it:** a binary that imports from `libSystem` and relies
on lazy binding needs the loader. `ld64.lld` will not emit a lazy-bind entry
against a synthesized stub dylib, so a converted ELF has no runtime path to
`printf` unless symbols are substituted before conversion — at which point you
have written half a loader anyway.

## Component map

```
                       ┌──────────────────────────────┐
  Xcode.xip  ─────────►│ xipx.py   xar → pbzx → cpio  │──► SDK (headers, .tbd)
  CLT.dmg    ─────────►│ dmgx.py   koly → HFS+ → pkg  │──► toolchain (Mach-O)
  *.pkg      ─────────►│ xarx.py / pkgx.py            │
                       └──────────────────────────────┘
                                     │
              ┌──────────────────────┴─────────────────────┐
              ▼                                            ▼
     ┌─────────────────┐                        ┌────────────────────┐
     │  xclang         │  host clang + SDK      │  darwinld          │
     │  -target        │  + ld64.lld            │  map → fixup →     │
     │  arm64-apple-   │                        │  bind → run        │
     │  macos15.0      │                        │  (src/)            │
     └────────┬────────┘                        └─────────┬──────────┘
              │ Mach-O out                                │ guest
              └───────────────────────────────────────────┘
                        (the same loader runs both)
```

## Modules

| file | responsibility | lines |
|---|---|---|
| `src/macho.c/.h` | fat/thin parsing, segments, symtab, chained fixups (two dialects), dyld_info bind/rebase/weak streams, export trie | ~700 |
| `src/dso.c/.h` | dependency graph, path resolution, two-level namespace binding, init order | ~330 |
| `src/shim.c/.h` | ~110 libSystem entry points; struct and flag translation | ~900 |
| `src/fmt.c`, `src/fmt_asm.S` | printf family for Darwin's stack-passed variadic ABI; setjmp/longjmp sized to Darwin's `jmp_buf` | ~600 |
| `src/main.c` | CLI, crash reporter with guest stack unwind | ~200 |
| `src/plat_posix.c` | `mmap`/`dlopen`/`read` backend | 60 |
| `src/plat_win32.c` | `VirtualAlloc`/`LoadLibrary`/`CreateFileA` backend + Darwin→UCRT name map | 120 |

Everything above `plat.h` is host-neutral C. The seam is three calls:

```c
void *plat_map(void *addr, size_t len, int prot, int fixed);  /* fixed: 0=anywhere 1=exact 2=here-or-fail */
void *plat_sym(const char *name);
int   plat_read_file(const char *path, void **buf, size_t *len);
```

## Why the ABI, and not the format, is the work

A Mach-O loader that only maps segments runs nothing. The measured deltas that
had to be closed, in the order they bit:

1. **Chained fixups have two dialects.** dyld's `dyld_chained_starts_in_segment`
   is 28+2n bytes, ld64.lld's is 22+2n. Reading one as the other yields
   `pointer_format = 0` and a 16 TB `segment_offset`.
2. **Import bitfields are reversed** relative to intuition:
   `lib_ordinal:8, weak:1, name_offset:23` — the name is `v >> 9`.
3. **`DYLD_CHAINED_PTR_64_OFFSET` stores vm offsets**, so the image base is
   added even at slide 0; `DYLD_CHAINED_PTR_64` stores absolute vmaddrs.
4. **Apple's arm64 variadic ABI puts anonymous arguments on the stack** and
   `va_list` is a pointer. Every `printf`-family entry point needs its own
   assembly front end. Without it, `ar` writes 60-byte member headers of zeros.
5. **Structs disagree in layout, not just size**: `stat` (144 B),
   `statfs` (2168 B), `dirent` (1048 B), `jmp_buf` (192 B), `sigaction` (16 B),
   `stack_t` (24 B), `sigset_t` (4 B). Passing a Linux struct through is silent
   corruption, so each is translated field by field and guarded by
   `_Static_assert`.
6. **Flag values disagree**: `open()`'s `O_CREAT`/`O_TRUNC`/`O_EXCL`, every
   `SA_*`, every `_CTYPE_*` bit.

Details and proofs: [ABI](ABI.md), [COMPAT](COMPAT.md).

## Constraints that shaped the design

| constraint | consequence |
|---|---|
| 4 GB RAM device | extractor streams one 16 MiB chunk; loader never reads a whole image twice |
| no `ptrace` on iOS | crash diagnostics are in-process: a `SIGSEGV` handler walks the guest frame chain and names the function |
| filesystem drops the exec bit | Makefiles `chmod +x` after linking |
| no macOS host | fidelity is claimed only for what a Linux host can observe: stdout, exit status, produced files |
| no mingw/Windows here | `plat_win32.c` is written and reviewable but has never been compiled |

## Deliberate non-goals

Objective-C, Swift and CoreFoundation runtimes; the GUI IDE; the simulator;
codesigning. Those are not shim entries — they are runtimes whose symbols are
*data structures the code walks*. See [COMPATIBILITY](COMPATIBILITY.md) for the
measured boundary (3,019 `___CFConstantStringClassReference`, 1,084
`__objc_empty_cache`, 904 Swift symbols).
