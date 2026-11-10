# darwinld — loader internals

`darwinld` maps an arm64 Mach-O at its preferred address, applies its fixups,
resolves every import against the host libc (or a loaded guest dylib), then
calls `LC_MAIN`.

## Pipeline

```
load(path)                     macho.c
  ├─ fat? pick the arm64 slice
  ├─ parse load commands → segments, symtab, dylib refs, dyld_info, chained fixups, exports
  └─ refuse if LC_ENCRYPTION_INFO_64.cryptid != 0
map()                          macho.c:plat_map
  ├─ lo = min(vmaddr), hi = max(vmaddr+vmsize), skipping __PAGEZERO
  ├─ reserve [lo,hi) at the preferred address (16 KiB granularity)
  │   if lo < 0x10000 (a dylib) → allocate anywhere and slide
  └─ copy each segment's file bytes, zero the rest
fixups()                       macho.c
  ├─ chained: walk per-segment page chains, rebase or bind each entry
  └─ dyld_info: rebase stream (if slid), bind stream, weak bind stream, lazy bind stream
bind()                         dso.c:resolve_sym
  ├─ lib_ordinal > 0 → that dependency's export trie
  ├─ lib_ordinal = 0 → the image's own exports
  ├─ lib_ordinal < 0 → flat search across all loaded images
  └─ else → shim (then host dlsym)
init()                         dso.c
  └─ __mod_init_func of every loaded image, in load order
entry()                        main.c
  └─ (__TEXT.vmaddr + slide + LC_MAIN.entryoff)(argc, argv, envp, NULL)
```

## Stage details

### Mapping

* `__PAGEZERO` (vmaddr 0, 4 GB, filesize 0) is normal — skip it, do not treat
  it as geometry.
* A dylib's `__TEXT` starts at vmaddr 0. `mmap(0, …, MAP_FIXED_NOREPLACE)`
  **succeeds** on some kernels, which puts every segment on the null page. Any
  preferred base below `0x10000` forces the slide path.
* Segments are mapped read-write-execute while binding and left that way;
  `initprot` is recorded but not enforced (there is no protecting a guest from
  itself here).

### Fixups

**Chained** (`LC_DYLD_CHAINED_FIXUPS`):

```
hdr = image + chain_off
  fixups_version, starts_offset, imports_offset, symbols_offset,
  imports_count, imports_format, symbols_format
starts = hdr + starts_offset
  seg_count, seg_info_offset[]
seg    = starts + seg_info_offset[i]
  dialect A (dyld):  size, page_size, page_format, max_valid(8),
                     pointer_format, segment_offset(8), overflow_count, page_start[]
  dialect B (LLVM):  size, page_size, pointer_format, segment_offset(8),
                     max_valid(4), page_count, page_start[]
  discriminated by  size == 22 + 2 * u16@20
```

Pointer formats handled: `DYLD_CHAINED_PTR_64` (2), `PTR_64_OFFSET` (6),
`PTR_ARM64E`/`ARM64E_USERLAND` (1/9). `page_start[i] == 0xFFFF` means no fixups.

**Classic** (`LC_DYLD_INFO[_ONLY]`): four opcode streams — rebase, bind, weak
bind, lazy bind — each a little VM over the same structure. Weak binds matter:
two binaries in the corpus (`coremlc`, `coremlcompiler`) hide 3,083 imports
there. `DONE` ends the whole eager stream but only one entry of a lazy stream.

### Binding

The import table is `{name, lib_ordinal}`. Resolution order is dyld's,
simplified to one flat fallback:

```
ordinal > 0   → the ordinal-th LC_LOAD_DYLIB of this image → export trie lookup
ordinal = 0   → this image's own exports
ordinal < 0   → every loaded image (flat namespace: 0xFE self, 0xFF main+flat)
still nothing → dshim_lookup(), then host dlsym()
```

Export lookup walks the trie byte by byte, follows reexport edges to the target
image, and falls back to `LC_SYMTAB` for images without a trie.

### Symbol naming

1. Strip exactly one leading `_` from the Mach-O name.
2. Truncate at `$` (Darwin's availability mangling: `realpath$DARWIN_EXTSN`).
3. Look in the shim table first, then `dlsym(RTLD_DEFAULT, name)`.

The shim also `dlopen`s `libc++.so.1`, `libc++abi.so.1`, `libz.so.1`,
`libbz2.so.1` and `libresolv.so.2` at start-up so their symbols are reachable —
libc++ uses `std::__1` everywhere, which is exactly Darwin's C++ namespace.

## Modes

```
darwinld info   <image>          segments, dylib list, fixup kind, cryptid
darwinld dylibs <image>          resolve the dependency graph and stop
darwinld check  <image>          bind; exit 1 if anything is unresolved
darwinld report <image>          bind; emit JSON (per image, per import, per name)
darwinld run    <image> [args]   bind and execute
```

Exit codes: `0` success · `1` load/map failure · `2` usage · `3` encrypted ·
`4` unresolved imports.

`report` output:

```json
{"tool": "/tmp/greeter", "unresolved_total": 0,
 "images": [{"path": "/tmp/greeter", "imports": 4, "unresolved": 0},
            {"path": "/tmp/libgreet.dylib", "imports": 1, "unresolved": 0}]}
```

## Environment

| variable | effect |
|---|---|
| `DS_SYSROOT` | prefix for absolute install names (`/usr/lib/libX.dylib` → `$DS_SYSROOT/usr/lib/libX.dylib`) |
| `DS_LIB_PATH` | colon-separated search path for dylib basenames |
| `DS_MISSING_FILE` | append unresolved symbol names to this file instead of stderr |
| `DS_TRACE=1` | print every binding: `name (lib N) -> address` |
| `DS_TRACE_FMT=1` | trace `__sprintf_chk` calls with their format and result |
| `DS_TRACE_XC=1` | trace `xcselect_*` (how Xcode's tools find each other) |
| `DEVELOPER_DIR` | what `xcselect_get_developer_dir_path` returns |

## Diagnostics

Loader chatter goes to **stderr** prefixed `[darwinld]`, so a guest's stdout
stays pipeable. That was a bug for a while: `strings tool | head` returned the
loader's status line first.

On `SIGSEGV`/`SIGBUS`/`SIGILL`/`SIGFPE` the handler prints the faulting PC, the
host symbol if any, the guest symbol from `LC_SYMTAB`, the fault address, the
argument registers, and a frame-pointer walk mapped back to guest symbols:

```
[darwinld] signal 4 at pc=0x10009f8a8 (+0x9f8a8 from image base) in __ZL16RegisterHandlersv
   fault=0x10009f8a8 x0=0xffffd4a0 x1=0xffffd544 x2=0xffffd538
   #0 0x305ac __ZN4llvm8InitLLVMC2ERiRPPKcbb+0x170
   #1 0x19ba8 _main+0x4c
```

That output is what identified LLVM's `InitLLVM` static initialiser as the
reason `swift-demangle` traps even though every import resolved.

## Extending

**Add a libSystem entry point** — one row in the table in `shim.c`:

```c
static int ds_getentropy(void *buf, size_t n) { /* … */ }
…
{ "getentropy", (void *)ds_getentropy },
```

The key is the *C* name (after stripping one underscore from the Mach-O symbol).
Add a variadic entry only through `fmt_asm.S` — a plain C function cannot read
Darwin's stack-passed arguments.

**Add a platform** — implement the three `plat.h` calls. `plat_win32.c` is the
template; the only nontrivial part is the Darwin→UCRT name map
(`open`→`_open`, `lseek`→`_lseeki64`, `stat`→`_stat64`, …).

**Add a struct translation** — declare the Darwin layout, `_Static_assert`
every offset that matters, translate field by field, and never `memcpy` between
the two.
