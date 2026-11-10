# Licensing and provenance

## This tree

Everything authored here — the loader, the shim, the container parsers, the
reports, the specs, the tests — is released under the MIT licence:

```
Copyright (c) 2026 the darwin-port authors
Permission is hereby granted, free of charge, to any person obtaining a copy of
this software and associated documentation files (the "Software"), to deal in
the Software without restriction, including without limitation the rights to
use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies
of the Software, and to permit persons to whom the Software is furnished to do
so, subject to the following conditions: the above copyright notice and this
permission notice shall be included in all copies or substantial portions of
the Software. THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND.
```

## Apple content — not redistributed

`inputs/` exists as a placeholder for the user-supplied archives and contains
nothing. No Xcode binary, dylib, framework, SDK header, `.tbd` stub or other
Apple asset is included in this tree.

What the reports contain instead: **paths, sizes and hashes** of extracted
files (`reports/inventory.jsonl.gz`). That is metadata, not content. Anyone
reproducing this work must obtain Xcode under Apple's own licence and extract
it locally; the extraction scripts are in `scripts/`.

`darwinld` is an independent implementation of a documented ABI. It contains no
Apple code, no disassembled Apple code, and no SDK data. `xclang` merely drives
LLVM with `-isysroot` pointing at a tree the user extracted.

## Third-party components referenced, not bundled

| component | licence | how it is used |
|---|---|---|
| LLVM / Clang / LLD | Apache-2.0 WITH LLVM-exception | host toolchain, invoked as an executable |
| libc++ / libc++abi | Apache-2.0 WITH LLVM-exception | host shared library, loaded by name at runtime |
| musl libc | MIT | host C library |
| zlib | zlib licence | host shared library, loaded by name at runtime |
| bison, flex, m4, gperf | GPL (various) | *guests*: Apple-built binaries executed, not redistributed |

## Data files in `tests/`

`tests/fixtures/mini-sdk/usr/lib/libSystem.tbd` is a hand-written 20-symbol stub
written for these tests. It is not Apple's file and contains no Apple data.

## Provenance table

| artefact | origin |
|---|---|
| `inputs/*.xip`, `inputs/*.dmg` | user-supplied; SHA-256 in `evidence/sha256.txt` |
| `reports/inventory*.jsonl*` | generated from the archive by `scripts/` |
| `ports/darwin-port/*` | authored for this work |
| `specs/*` | authored from measurements (see each spec's first line) |
