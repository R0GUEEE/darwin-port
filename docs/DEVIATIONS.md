# DEVIATIONS — where this layer deliberately differs from macOS

Each entry: what macOS does, what we do, and what a guest can observe.
Anything that could silently corrupt memory is marked **CORRUPTION** and has
either been fixed or is refused explicitly.

## Fixed in this pass (were silent corruption)

| area | macOS | before | now |
|---|---|---|---|
| `setjmp`/`longjmp` | `jmp_buf` is `int[48]` = **192 B** on arm64 | musl's `setjmp` wrote **312 B** — 120 bytes past the guest's buffer on every call | own arm64 implementation writing exactly 168 B, test **T7** |
| `dirent` | `d_seekoff@8`, `d_namlen@18`, `d_name@21[1024]`, 1048 B | passthrough returned a 256-byte-name struct with different offsets | translated into a 1048 B buffer, test **T5** |
| `statfs` | 2168-byte struct, `f_fstypename@72`, `f_mntfromname@1112` | musl's 120-byte `statvfs` written into the guest's 2168-byte buffer, leaving 2048 bytes of stack garbage | translated and zero-filled, test **T8** |
| `LC_DYLD_INFO` weak binds | executed | ignored — `coremlc` appeared to import nothing and was counted "clean" | executed; it now reports the 3,083 weak imports it always had |

## Behavioural deviations (documented, not corrupting)

1. **No lazy binding.** Every import is bound before the entry point runs;
   `dyld_stub_binder` is a no-op. A guest cannot observe lazy-resolution order,
   and `dyld_stub_binder` address comparisons see a fixed function.
2. **`_NSGetExecutablePath`** returns `argv[0]` verbatim (relative if invoked
   relatively) instead of a resolved absolute path.
3. **mach traps are constants.** `mach_task_self_` = 0x203, `mach_thread_self()`
   = 0x403, `mach_host_self()` = 0x303. `host_info` returns `KERN_SUCCESS` with
   a zero-filled structure. Code that inspects ports sees fake values.
4. **`os_log*` is a no-op.** Nothing reaches a unified log; `os_log_type_enabled`
   returns 0 so `if (os_log_type_enabled(...))` blocks are skipped entirely.
5. **`sysctl` answers only `hw.memsize` and `hw.ncpu`** (from constants). Every
   other MIB returns ENOTSUP. `CTL_KERN` queries fail rather than guess.
6. **TLS is process-global.** `__tlv_bootstrap` allocates one block per
   descriptor, not per thread. Multi-threaded guests would share TLV storage.
7. **Blocks**: `_Block_object_assign/dispose` are no-ops; only non-capturing
   blocks are safe. A block that captures an object will not retain it.
8. **`__cxa_atexit`** accumulates into one global list run at process exit in
   reverse order. `__cxa_finalize(dso)` does nothing, so per-image teardown
   ordering is not honoured.
9. **`sigaltstack`**: sizes below 8 KiB are raised to 8 KiB (musl rejects
   smaller); `SS_ONSTACK` is never reported back in `old`.
10. **`statfs` reports a synthetic filesystem**: `f_fstypename` is always
    `"apfs"`, `f_mntfromname` is `/dev/disk1s1`, `f_flags`/`f_type` are 0.
    Sizes and free space are real.
11. **`struct stat`**: `st_birthtimespec`, `st_flags`, `st_gen`, `st_qspare`
    are zero (Linux has no birth time and no BSD flags).
12. **`open()`**: `O_SHLOCK`, `O_EXLOCK`, `O_EVTONLY`, `O_SYMLINK` are dropped.
    `O_DIRECTORY`, `O_CLOEXEC`, `O_NOFOLLOW` are translated.
13. **`@rpath` resolution** uses `$DS_LIB_PATH` and the loading image's
    directory; `LC_RPATH` entries are not parsed in order.
14. **No dyld shared cache.** `dyld_get_shared_cache_range` returns length 0;
    `dyld_for_each_installed_shared_cache` never calls back.
15. **`xcselect_get_developer_dir_path`** returns `$DEVELOPER_DIR`, defaulting
    to the extraction path this project was built against — not to whatever
    `xcode-select` configured.
16. **`exit()`** from a guest terminates the loader process, exactly as a native
    binary would. There is no interpreter unwinding, so host-side cleanup after
    the guest exits does not run.
17. **`execvp`** of a Mach-O re-enters `darwinld` as a child process; the guest
    sees a real `exec` semantic (the PID is preserved by `execv`), but the
    child's `argv[0]` is the loader path, not the tool path.
18. **`getopt`** is musl's. Option permutation differs from BSD's in the
    POSIXLY_CORRECT corner, which is why `unifdef` rejects its options while a
    synthetic test program parses the same options correctly.
19. **Signals**: `sigaction` handlers are installed on the host with translated
    flags, so a Darwin `SA_ONSTACK`/`SA_SIGINFO` handler is invoked with the
    Linux `siginfo_t`/`ucontext_t` layouts. Guests that read `si_addr` are fine;
    guests that walk `ucontext_t` registers are not.
20. **Thread names**: `pthread_setname_np` is a no-op.

## Explicitly refused rather than faked

| call | behaviour |
|---|---|
| `fsgetpath`, `proc_pidinfo`, `terminate_with_payload` | return `-1` with `ENOSYS` — never a plausible-looking answer |
| FairPlay image (`cryptid != 0`) | loader exits 3 before mapping anything |
| unresolved import | loader exits 4 and names the symbol (test **T4**); it never jumps to NULL |

## Not implemented at all (call is MISSING, `check` reports it)

`termios`/`tcgetattr`/`ioctl` translation (Darwin `termios` is 72 bytes with a
different flag set), `locale_t`/`duplocale`, `getmntinfo`, `statvfs`,
`pthread_setname_np` semantics, `os_unfair_lock` (falls through to musl's
`pthread_mutex`), `dispatch_*`, and the ObjC/Swift/CoreFoundation runtimes.
