# Spec — hosting the Swift runtime in a non-Apple loader

What a loader must provide for a Swift guest to boot. Derived from the ABI the
Xcode SDK describes (`libswiftCore.tbd`, `swift-abi-version: 7`) and from the
import surface measured in `docs/SWIFT.md`.

## 1. What the runtime expects from dyld

Swift does not register its metadata by itself. `libswiftCore` installs a
callback with the dynamic loader and walks each image when it arrives:

```c
dyld_register_func_for_add_image(cb);      // cb(mach_header *, intptr_t slide)
```

dyld semantics, which the loader must reproduce exactly:

1. Call `cb` once for **every image already loaded**, in load order, before
   `dyld_register_func_for_add_image` returns.
2. Call `cb` for each image loaded afterwards.
3. Pass the image's Mach header pointer and its **slide**, not its base address.

Verified: `tests/run.sh` T9 registers a callback and asserts it fires.

## 2. Sections the runtime walks

For each image handed to it, the runtime reads these `__TEXT` sections:

| section | contents | consequence if absent |
|---|---|---|
| `__swift5_types` | type metadata records | classes/structs have no metadata; `type(of:)` fails |
| `__swift5_protocols` | protocol descriptors | conformances cannot be resolved |
| `__swift5_protocol_conformances` | conformance records | `as?`/generic dispatch fails |
| `__swift5_fieldmd` | field descriptors | reflection and `Mirror` fail |
| `__swift5_typeref` | type references | generic instantiation fails |
| `__swift5_reflstr` | reflection strings | field names lost |
| `__swift5_assocty` | associated-type records | constrained generics fail |
| `__swift5_capture` | closure capture descriptors | closures over context crash |
| `__swift5_entry` | `@main` entry | no entry point discovered |
| `__swift5_replace`, `__swift5_replace2` | dynamic replacement records | `@_dynamicReplacement` ignored |

The loader does not need to parse these — the runtime does. It only has to map
the image and hand over the header. Parsing them is needed only for tooling
(the crash reporter, `swift_abi.py`-style analysis).

## 3. Symbol binding

Swift's mangled names are ABI-stable across platforms:

```
_$sSS5countSivg          String.count
_$sSS6appendyySSF        String.append(String)
_$sSa6appendyyxnF        Array.append(Element)
```

A Darwin guest's `_$s…` imports may therefore be satisfied by a **Linux**
`libswiftCore.so`, provided the ABI version (`swift-abi-version`) matches the
one the guest was compiled against. The loader does this with the mechanism it
already has: `dlopen("libswiftCore.so")` at start-up plus name-based `dlsym`,
exactly as it does for `libc++`.

Rules:

1. Match on the mangled name after stripping one leading `_`.
2. Refuse a runtime whose ABI version differs from the guest's build: the
   `.tbd` header states it, the guest records it in `LC_BUILD_VERSION`/metadata.
3. Keep the runtime in the flat search path only — never bind `_$s…` to an
   image the guest did not ask for.

## 4. What the loader must *not* do

* Do not attempt to implement `_$s…` symbols by hand. They are not functions
  with substitute behaviour; they operate on metadata laid out by the compiler.
* Do not run `__mod_init_func` of a Swift dylib before the runtime has
  registered it (order: map → bind → **runtime registration** → initialisers).
* Do not assume `swift_once`-guarded initialisation is single-threaded safe if
  the loader starts threads before `main`.

## 5. Companion libraries

A Swift guest links the runtime plus, usually, several of:

| library | why it appears |
|---|---|
| `libswiftCore` | stdlib |
| `libswift_Concurrency` | `async`/`await`, task groups |
| `libswift_StringProcessing` | regex literals |
| `libswift_RegexParser` | `Regex` |
| `libswift_Differentiation` | `@differentiable` |
| `libswiftDistributed` | distributed actors |
| `libswiftObservation` | `@Observable` |
| `libswiftSynchronization` | `Mutex`, `Atomics` |

All eight exist for Linux in the open-source toolchain; none exist in the Xcode
XIP as dylibs (only as `.tbd` stubs). Hosting them is a matter of pointing the
loader at the Linux build.

## 6. Acceptance tests for a Swift-capable loader

1. A pure-Swift binary with a struct, a class and a protocol prints correctly.
2. A generic function instantiated twice returns distinct results.
3. A closure capturing a variable works (`__swift5_capture`).
4. `async let` completes (`libswift_Concurrency`).
5. Reflection (`Mirror`) lists fields (`__swift5_fieldmd`).
6. The same binary fails cleanly, not with SIGSEGV, when the runtime is absent.

Tests 1–6 need a runtime that is not in the Xcode archive; the loader side is
already capable of loading it.
