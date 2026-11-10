# Troubleshooting

Every entry here is a failure that actually happened during this work, with the
symptom first so you can match it.

## Loader

### `refusing: FairPlay-encrypted (cryptid=1)` — exit 3
App Store binaries are encrypted with a per-device key. Nothing in a compat
layer can decrypt them. Developer toolchain binaries are `cryptid = 0` (131 of
138 images here), which is why this project is possible at all.

### `cannot run: N imports unresolved` — exit 4
The shim does not cover something. Get the names:

```sh
DS_MISSING_FILE=/tmp/m darwinld check tool >/dev/null; sort -u /tmp/m
```

Then classify: `_Z…`/`__ZNSt3__1…` is C++ (host libc++ should cover it — check
`libc++.so.1` is installed), `_OBJC_…`/`_$s…`/`___CF…` is a runtime you do not
have, anything else is a missing shim entry (add one row to `shim.c`).

### Segmentation fault, and the report says `in outside-image`
A jump through a bad pointer. Look at the frame list: if frame #0 is inside
libc, a struct or a file pointer was translated wrongly; if it is a guest
address near `0x3a93`, a `DYLD_CHAINED_PTR_64_OFFSET` rebase was not applied
(offsets need the image base added even at slide 0).

### `[darwinld] signal 4 … in llvm::InitLLVM`
SIGILL from `ud2`: the guest took an LLVM `llvm_unreachable`. Binding is fine —
the tool is asserting about something in its environment. Not a loader bug,
and not fixed by more shims.

### `Exec format error`
You executed a Mach-O directly. The kernel cannot run Mach-O; use
`darwinld run`. The loader's own `execvp` shim exists precisely so a guest that
execs another Xcode tool (ar → ranlib, m4 → gm4) re-enters the loader.

### Guest output is polluted by `[darwinld] images N …`
Old behaviour, fixed: status goes to stderr now. If you see it on stdout, you
are running a build from before that fix.

## Extraction

### `bad cpio magic b'do not prune dir'` or garbage filenames
The extractor is reading from the wrong offset. XAR member offsets are relative
to the **heap** (`header_size + toc_size`), not the file. In the Xcode 27.3
archive: heap 3937, `Content` at 4213 — not at the literal 276 in the TOC.

### Extraction stops after ~1.5 GiB
pbzx chunks whose `comp_size == out_size` are **stored**, not XZ. Scanning for
XZ magic instead of honouring the chunk table truncates the stream silently.
Check the invariant: `sum(out_size)` must equal the member size (622 chunks →
10,433,180,672 bytes for this XIP).

### `MemoryError` / process killed mid-extraction
A 16 MiB chunk plus repeated `bytes +=` copying exhausts a 4 GB device. The
current reader keeps one chunk in a `bytearray`. If it still dies, narrow the
selection (`--include`, not the whole archive) — and expect ~25 min per pass
regardless, since the stream is sequential.

### Headers compile to `unknown type name 'NULLcanary'`
Apple's xip deduplicates: 5,955 of the macOS SDK's ~6,000 headers are the
literal bytes `NULLcanary` + zeros. Extract every SDK's `usr/include` and run
`hdrmerge.py` (846 headers patched here, 3 left unresolved).

### `ld64.lld: error: malformed file … arm64e.x1-macos`
844 of 1,395 `.tbd` stubs declare a target LLD 19 does not know. Strip the
token:

```sh
find $SDK -name '*.tbd' -exec sed -i 's/, *arm64e\.x1-\(macos\|maccatalyst\)//' {} +
```

## Working with guests

### `opendir` returns entries with garbage names
`struct dirent` is 1048 bytes on Darwin with `d_namlen` at 18 and `d_name` at 21;
Linux's is 280 bytes with a 256-byte name. If you shim it, translate into your
own buffer (the loader keeps a 32-slot `DIR *` table).

### A tool loops forever or crashes inside `longjmp`
`jmp_buf` is 192 bytes on Darwin and 312 in musl. Passing musl's `setjmp`
through writes 120 bytes past the guest's buffer, which corrupts the caller's
frame. The loader ships its own implementation; do not "fix" it by forwarding
again.

### `sigaltstack: Out of memory`
`stack_t` orders `ss_size` and `ss_flags` differently (Darwin: sp, size,
flags; Linux: sp, flags, size). musl reads the 4-byte flags field as the stack
size and returns ENOMEM. Translate both directions.

### A tool creates or truncates the wrong file
`open()` flags are not the same numbers: Darwin `O_CREAT 0x200`, `O_TRUNC 0x400`,
`O_EXCL 0x800`; Linux `0x40`, `0x200`, `0x80`. Darwin `O_TRUNC` is Linux
`O_APPEND` — a passthrough appends to a file you meant to truncate.

### Sizes and dates are nonsense in the guest
`struct stat` is 144 bytes on Darwin (`st_mode@4` as `uint16`, `st_size@96`) and
128 on Linux (`st_mode@16`, `st_size@48`). Always translate field by field.

## Instrumentation

| need | do |
|---|---|
| see every binding | `DS_TRACE=1 darwinld run …` |
| see why a `sprintf` misbehaves | `DS_TRACE_FMT=1` |
| see how a tool locates its siblings | `DS_TRACE_XC=1` |
| get the unresolved list into a file | `DS_MISSING_FILE=/tmp/m darwinld check …` |
| see the image graph | `darwinld dylibs tool` |
| machine-readable census | `darwinld report tool` |

## Filesystem quirks on mobile hosts

* Some filesystems drop the executable bit on create. Build rules must
  `chmod +x` after linking, and test scripts must `chmod` before running —
  otherwise `Permission denied` looks like a build failure.
* `realpath()` can fail on children of some mounted directories even when the
  parent resolves. If a tool dies in `perror(root)`, move the data out of that
  mount.
* A VM with no swap will OOM-kill large writes (9.9 GB of headers did it
  twice here). Extract only what you need, in one pass.
