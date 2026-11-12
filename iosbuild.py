#!/usr/bin/env python3
"""iosbuild - build a real, unsigned iOS app from Linux, without Xcode.

Compiles C, Objective-C, C++, and Objective-C++ source with clang for `arm64-apple-ios`, links them with
ld64.lld against an extracted iPhoneOS SDK's text stubs, writes the bundle
Info.plist, and packages `Payload/<Name>.app` as an IPA.

  iosbuild.py plan  <dir|ipa.json> [--sdk PATH] [--json]
  iosbuild.py build <dir|ipa.json> [--sdk PATH] [--out DIR] [-v]
  iosbuild.py ipa   <dir|ipa.json> [--sdk PATH] [--out DIR] [--name Foo]
  iosbuild.py inspect <file.ipa|File.app> [--json]

Exit codes: 0 ok, 1 build error, 2 usage, 3 environment (no SDK / no linker).

What this is not: a signing tool. The IPA ships unsigned; install it through a
signing service or sign it with ldid on a jailbroken device. It is also not a
Swift compiler: no Swift runtime stubs exist in a stock SDK, so Swift sources
are rejected with that reason rather than a linker error 400 lines later.

Three facts about the iPhoneOS SDK that this tool encodes, each learned the hard
way and each invisible from the outside:

  * framework headers live in `System/Library/Frameworks`, but a growing set
    (UIUtilities, ...) live in `System/Library/SubFrameworks`; `#import
    <UIUtilities/UIDefines.h>` fails with "file not found" unless both are
    framework search paths;
  * the TBD stubs in a 27.x SDK declare arm64e/arm64e.x1, and ld64.lld 19
    rejects both, so linking needs an overlay that maps those target triples to
    plain arm64 (the bytes of the stub are unchanged, only `targets` and
    `archs` are rewritten);
  * an iOS Mach-O is indistinguishable from a macOS one by shape alone - both
    are thin arm64 - so every artifact this tool produces is checked for
    LC_BUILD_VERSION platform 2 before it is allowed into an IPA.
"""
import hashlib
import json
import os
import plistlib
import shutil
import struct
import subprocess
import sys
import tempfile
import time
import zipfile

VERSION = "1.0"
DEFAULT_MIN_IOS = "15.0"
# Platform ids as they appear in LC_BUILD_VERSION.
PLATFORM_NAMES = {1: "macos", 2: "ios", 3: "tvos", 4: "watchos",
                  6: "maccatalyst", 7: "ios-simulator"}
HERE = os.path.dirname(os.path.abspath(__file__))


# ------------------------------------------------------------------ logging --
QUIET = False


def log(msg):
    if not QUIET:
        sys.stderr.write(msg + "\n")


def die(msg, code=1):
    sys.stderr.write("iosbuild: %s\n" % msg)
    raise SystemExit(code)


# ------------------------------------------------------------------ tooling --
def which(name):
    """Find a host executable on PATH; also accept Windows .exe names."""
    names = [name]
    if os.name == "nt" and not name.lower().endswith((".exe", ".cmd", ".bat")):
        names = [name + ".exe", name + ".cmd", name + ".bat", name]
    for d in os.environ.get("PATH", "").split(os.pathsep):
        for candidate in names:
            p = os.path.join(d, candidate)
            if os.path.isfile(p) and (os.name == "nt" or os.access(p, os.X_OK)):
                return p
    return None


def run(argv, verbose=False, cwd=None):
    if verbose:
        log("$ " + " ".join(argv))
    p = subprocess.run(argv, cwd=cwd, capture_output=True, text=True)
    if p.returncode != 0:
        tail = (p.stderr or p.stdout).strip().splitlines()[-25:]
        die("command failed (%d): %s\n%s" % (p.returncode, " ".join(argv),
                                             "\n".join(tail)))
    if verbose and p.stderr.strip():
        log(p.stderr.strip())
    return p.stdout


# ------------------------------------------------------------------- mach-o --
MACHO_MAGIC = {0xFEEDFACF: ("<", 64), 0xCFFAEDFE: (">", 64),
               0xFEEDFACE: ("<", 32), 0xCEFAEDFE: (">", 32)}


def macho_info(path):
    """Parse a thin or fat Mach-O without otool. Returns a dict:
    {arch, kind, platform, minos, needed[], pie, encrypted, slices[]}"""
    with open(path, "rb") as fh:
        data = fh.read()
    if len(data) < 32:
        die("%s is too small to be a Mach-O" % path)
    magic_be = struct.unpack_from(">I", data, 0)[0]
    if magic_be in (0xCAFEBABE, 0xCAFEBABF):
        nfat = struct.unpack_from(">I", data, 4)[0]
        slices = []
        for i in range(nfat):
            cpu, sub, off, size, align = struct.unpack_from(">iiIII", data, 8 + i * 20)
            slices.append({"arch": {0x0100000C: "arm64", 0x01000007: "x86_64"}.get(cpu, "?"),
                           "offset": off, "size": size})
        first = _macho_slice(data[slices[0]["offset"]:slices[0]["offset"] + slices[0]["size"]])
        first["slices"] = slices
        first["fat"] = True
        return first
    info = _macho_slice(data)
    info["fat"] = False
    info["slices"] = [{"arch": info["arch"], "offset": 0, "size": len(data)}]
    return info


def _macho_slice(data):
    magic = struct.unpack_from("<I", data, 0)[0]
    if magic not in MACHO_MAGIC:
        die("not a Mach-O (magic 0x%08x)" % magic)
    endian, bits = MACHO_MAGIC[magic]
    cputype, cpusub, filetype, ncmds, sizeofcmds, flags, _res = struct.unpack_from(
        endian + "IIIIIII", data, 4)
    arch = {0x0100000C: "arm64", 0x01000007: "x86_64", 12: "arm", 7: "i386"}.get(cputype, "?")
    kind = {2: "executable", 6: "dylib", 8: "bundle", 1: "object", 11: "kext"}.get(filetype, "?")
    off = 32 if bits == 64 else 28
    out = {"arch": arch, "kind": kind, "bits": bits, "needed": [],
           "platform": None, "minos": None, "pie": bool(flags & 0x200000),
           "encrypted": False}
    for _ in range(ncmds):
        if off + 8 > len(data):
            break
        cmd, cmdsize = struct.unpack_from(endian + "II", data, off)
        if cmdsize < 8:
            break
        if cmd in (0xC, 0x18 | 0x80000000, 0x1F | 0x80000000, 0x23 | 0x80000000):
            name_off = struct.unpack_from(endian + "I", data, off + 8)[0]
            start = off + name_off
            end = data.find(b"\0", start)
            out["needed"].append(data[start:end].decode("utf-8", "replace"))
        elif cmd == 0x32:                                     # LC_BUILD_VERSION
            plat, minos = struct.unpack_from(endian + "II", data, off + 8)
            out["platform"] = PLATFORM_NAMES.get(plat, "platform-%d" % plat)
            out["minos"] = _ver(minos)
        elif cmd == 0x24:                                     # LC_VERSION_MIN_MACOSX
            v = struct.unpack_from(endian + "I", data, off + 8)[0]
            out["platform"] = out["platform"] or "macos"
            out["minos"] = out["minos"] or _ver(v)
        elif cmd == 0x25:                                     # LC_VERSION_MIN_IPHONEOS
            v = struct.unpack_from(endian + "I", data, off + 8)[0]
            out["platform"] = out["platform"] or "ios"
            out["minos"] = out["minos"] or _ver(v)
        elif cmd == 0x2C:                                     # LC_ENCRYPTION_INFO_64
            cryptid = struct.unpack_from(endian + "I", data, off + 16)[0]
            out["encrypted"] = cryptid != 0
        off += cmdsize
    return out


def _ver(v):
    return "%d.%d.%d" % ((v >> 16) & 0xFFFF, (v >> 8) & 0xFF, v & 0xFF)


# ---------------------------------------------------------------------- sdk --
SDK_CANDIDATES = [
    os.path.expanduser("~/iphoneos-compat-sdk"),
    os.path.expanduser("~/iphoneos27.3-compat-sdk-with-headers"),
    "/root/iphoneos-compat-sdk",
    "/root/iphoneos27.3-compat-sdk-with-headers",
]


def find_sdk(explicit=None):
    """Locate an iPhoneOS SDK. Order: --sdk, $IPHONEOS_SDK, known local
    compatibility SDK paths, then an extracted Xcode tree under /root/xc or ~/xc."""
    if explicit:
        p = os.path.abspath(explicit)
        if not os.path.isdir(p):
            die("no such SDK: %s" % p, 3)
        return p
    env = os.environ.get("IPHONEOS_SDK")
    if env:
        return os.path.abspath(env)
    for p in SDK_CANDIDATES:
        if os.path.isdir(p) and os.path.exists(os.path.join(p, "SDKSettings.json")):
            return p
    import glob
    pats = [os.path.expanduser("~/iphoneos*compat*"),
            "/root/iphoneos*compat*",
            os.path.expanduser("~/xc/*.app/Contents/Developer/Platforms/iPhoneOS.platform/Developer/SDKs/*.sdk"),
            "/root/xc/*.app/Contents/Developer/Platforms/iPhoneOS.platform/Developer/SDKs/*.sdk"]
    found = []
    for pat in pats:
        found.extend(sorted(glob.glob(pat)))
    if not found:
        die("no iPhoneOS SDK found; pass --sdk or set $IPHONEOS_SDK", 3)
    return found[0]


def sdk_version(sdk):
    p = os.path.join(sdk, "SDKSettings.json")
    if os.path.exists(p):
        try:
            with open(p) as fh:
                info = json.load(fh)
            return info.get("Version") or info.get("CanonicalName")
        except (ValueError, OSError):
            pass
    return "unknown"


def sdk_archs(sdk):
    p = os.path.join(sdk, "SDKSettings.json")
    try:
        with open(p) as fh:
            info = json.load(fh)
        for block in info.get("SupportedTargets", {}).values():
            if block.get("Archs"):
                return block["Archs"]
    except (ValueError, OSError, KeyError):
        pass
    return []


def _tbd_needs_overlay(tbd_path):
    with open(tbd_path, "r", errors="replace") as fh:
        head = fh.read(4096)
    return "arm64e" in head


# ------------------------------------------------------------------ overlay --
# ld64.lld 19 cannot resolve the arm64e / arm64e.x1 target triples that a 27.x
# SDK's TBD stubs declare, and a device link must target plain arm64 anyway.
# The overlay is a shadow tree: directories symlinked, headers untouched, TBD
# text rewritten so only `arm64` remains. Nothing else in the SDK changes, and
# the original tree is never written to.
_TGT_MAP = [
    ("arm64e.x1-", "arm64-"),
    ("arm64e-", "arm64-"),
]


def _rewrite_tbd(text):
    out = []
    for line in text.splitlines():
        if "arm64e" in line:
            for a, b in _TGT_MAP:
                line = line.replace(a, b)
            # `[ arm64-ios, arm64-ios ]` is legal YAML but pointless; collapse it
            if "[" in line and "]" in line:
                head, _, rest = line.partition("[")
                items, _, tail = rest.partition("]")
                seen, keep = set(), []
                for it in items.split(","):
                    it = it.strip()
                    if it and it not in seen:
                        seen.add(it)
                        keep.append(it)
                line = head + "[ " + ", ".join(keep) + " ]" + tail
        out.append(line)
    return "\n".join(out) + "\n"


def overlay_sdk(sdk, verbose=False):
    """Build (or reuse) an arm64-only view of `sdk`. Returns its path."""
    key = hashlib.sha256(("%s|%s|%s" % (os.path.abspath(sdk), sdk_version(sdk),
                                        VERSION)).encode()).hexdigest()[:16]
    cache = os.path.join(os.path.expanduser("~/.cache/darwin-port"), "ios-" + key)
    marker = os.path.join(cache, ".complete")
    if os.path.exists(marker):
        return cache
    log("overlay: mapping arm64e TBD targets to arm64 (%s)" % cache)
    shutil.rmtree(cache, ignore_errors=True)
    os.makedirs(cache, exist_ok=True)
    n_tbd = 0
    for root, dirs, files in os.walk(sdk):
        rel = os.path.relpath(root, sdk)
        dst_root = cache if rel == "." else os.path.join(cache, rel)
        os.makedirs(dst_root, exist_ok=True)
        for d in dirs:
            os.makedirs(os.path.join(dst_root, d), exist_ok=True)
        for f in files:
            src = os.path.join(root, f)
            dst = os.path.join(dst_root, f)
            if f.endswith(".tbd") and _tbd_needs_overlay(src):
                with open(src, "r", errors="replace") as fh:
                    text = fh.read()
                with open(dst, "w") as fh:
                    fh.write(_rewrite_tbd(text))
                n_tbd += 1
            else:
                try:
                    os.symlink(os.path.abspath(src), dst)
                except OSError:
                    shutil.copy2(src, dst)
    with open(marker, "w") as fh:
        fh.write("%s\n%d\n" % (sdk, n_tbd))
    log("overlay: %d TBD files rewritten" % n_tbd)
    return cache


# ----------------------------------------------------------------- manifest --
DEFAULT_MANIFEST = {
    "name": None,              # defaults to the directory name
    "bundle_id": None,         # defaults to com.example.<name>
    "version": "1.0",
    "build": "1",
    "min_ios": DEFAULT_MIN_IOS,
    "sources": [],             # ["*.m", "src/*.c"] relative to the manifest dir
    "frameworks": [],          # ["UIKit", "Foundation"]
    "libraries": ["objc", "System"],
    "defines": [],
    "includes": [],
    "resources": [],           # copied into <Name>.app/
    "info": {},                # extra Info.plist keys
    "required_capabilities": [],  # e.g. ["arm64"], ["UIRequiredDeviceCapabilities"]
}


def load_manifest(path):
    """Read ipa.json (or <dir>/ipa.json) and fill in the defaults."""
    base = os.path.abspath(path)
    if os.path.isdir(base):
        return load_manifest(os.path.join(base, "ipa.json")) if os.path.exists(
            os.path.join(base, "ipa.json")) else _manifest_from_dir(base)
    if not os.path.exists(base):
        die("no such manifest: %s" % base, 2)
    with open(base) as fh:
        try:
            user = json.load(fh)
        except ValueError as exc:
            die("%s is not valid JSON: %s" % (base, exc), 2)
    m = dict(DEFAULT_MANIFEST)
    m.update(user)
    m["_dir"] = os.path.dirname(base)
    return _finish_manifest(m)


def _manifest_from_dir(d):
    """No ipa.json: build everything obvious in the directory."""
    srcs = sorted(f for f in os.listdir(d)
                  if f.endswith((".m", ".c", ".mm", ".cc", ".cpp", ".cxx")))
    if not srcs:
        die("no ipa.json and no .m/.c/.mm sources in %s" % d, 2)
    m = dict(DEFAULT_MANIFEST)
    m["sources"] = srcs
    m["_dir"] = d
    return _finish_manifest(m)


def _finish_manifest(m):
    d = m.get("_dir", ".")
    name = m.get("name") or os.path.basename(os.path.abspath(d)) or "App"
    name = name.replace(" ", "")
    m["name"] = name
    m["bundle_id"] = m.get("bundle_id") or ("com.example." + name.lower())
    # glob expansion, sorted for determinism
    import glob
    srcs = []
    for pat in m.get("sources", []):
        hits = sorted(glob.glob(os.path.join(d, pat)))
        if not hits:
            die("source pattern matched nothing: %s" % pat)
        srcs.extend(hits)
    m["sources"] = srcs
    if not srcs:
        die("manifest lists no sources")
    m["_resolved_resources"] = []
    for pat in m.get("resources", []):
        hits = sorted(glob.glob(os.path.join(d, pat)))
        if not hits:
            die("resource pattern matched nothing: %s" % pat)
        m["_resolved_resources"].extend(hits)
    for s in srcs:
        if s.endswith(".swift"):
            die("Swift sources are not supported: a stock SDK ships no "
                "libswiftCore stubs, so linking them is impossible, not merely "
                "unimplemented", 1)
    return m


def sdk_framework_paths(sdk):
    """Both framework roots: the ordinary one and SubFrameworks, which UIKit
    headers reach into (#import <UIUtilities/UIDefines.h>)."""
    out = []
    for rel in ("System/Library/Frameworks", "System/Library/SubFrameworks",
                "System/Library/PrivateFrameworks", "usr/lib"):
        p = os.path.join(sdk, rel)
        if os.path.isdir(p):
            out.append(p)
    return out


# -------------------------------------------------------------------- build --
def clang_flags(sdk, m, source=None):
    flags = ["-target", "arm64-apple-ios" + m["min_ios"],
             "-isysroot", sdk,
             "-O2", "-fPIC", "-Wno-nullability-completeness",
             "-DIPHONEOS=1"]
    ext = os.path.splitext(source or "")[1].lower()
    if ext in (".m", ".mm"):
        flags += ["-fobjc-arc", "-fblocks"]
    if ext in (".mm", ".cc", ".cpp", ".cxx"):
        flags += ["-stdlib=libc++"]
    for fpath in sdk_framework_paths(sdk):
        if fpath.endswith("Framework") or fpath.endswith("Frameworks"):
            flags += ["-F", fpath]
    for inc in m.get("includes", []):
        flags.append("-I" + os.path.join(m["_dir"], inc))
    for d in m.get("defines", []):
        flags.append("-D" + d)
    return flags


def build(m, sdk, outdir, verbose=False):
    """Compile + link. Returns the path to the produced executable."""
    if not which("ld64.lld"):
        for cand in ("/usr/bin/ld64.lld", "/usr/local/bin/ld64.lld"):
            if os.path.exists(cand):
                os.environ["PATH"] = os.path.dirname(cand) + os.pathsep + os.environ["PATH"]
                break
        else:
            die("ld64.lld not found (apk add lld, or brew install llvm)", 3)
    overlay = overlay_sdk(sdk, verbose)
    os.makedirs(outdir, exist_ok=True)
    objs = []
    for src in m["sources"]:
        obj = os.path.join(outdir, os.path.basename(src) + ".o")
        log("  CC  %s" % os.path.relpath(src, m["_dir"]))
        run(["clang"] + clang_flags(sdk, m, src) + ["-c", src, "-o", obj], verbose)
        objs.append(obj)

    exe = os.path.join(outdir, m["name"])
    args = ["ld64.lld", "-arch", "arm64",
            "-platform_version", "ios", m["min_ios"], sdk_version(sdk) or "27.0",
            "-syslibroot", overlay, "-o", exe, "-e", "_main"]
    for fw in m.get("frameworks", []):
        args += ["-framework", fw]
    for lib in m.get("libraries", []):
        args += ["-l" + lib]
    args += objs
    log("  LD  %s" % m["name"])
    run(args, verbose)

    info = macho_info(exe)
    if info["fat"]:
        die("link produced a fat binary; expected thin arm64")
    if info["arch"] != "arm64":
        die("link produced %s, expected arm64" % info["arch"])
    if info["platform"] != "ios":
        die("link produced platform %r, expected ios - the SDK overlay or the "
            "-platform_version flag is wrong" % info["platform"])
    return exe


def write_plist(m, appdir):
    info = {
        "CFBundleDevelopmentRegion": "en",
        "CFBundleExecutable": m["name"],
        "CFBundleIdentifier": m["bundle_id"],
        "CFBundleInfoDictionaryVersion": "6.0",
        "CFBundleName": m["name"],
        "CFBundlePackageType": "APPL",
        "CFBundleShortVersionString": m["version"],
        "CFBundleVersion": m["build"],
        "LSRequiresIPhoneOS": True,
        "UILaunchScreen": {},
        "UIRequiredDeviceCapabilities": m.get("required_capabilities") or ["arm64"],
        "UISupportedInterfaceOrientations": [
            "UIInterfaceOrientationPortrait",
            "UIInterfaceOrientationLandscapeLeft",
            "UIInterfaceOrientationLandscapeRight",
        ],
        "MinimumOSVersion": m["min_ios"],
        "DTPlatformName": "iphoneos",
        "DTSDKName": "iphoneos" + m["min_ios"],
        "DTSDKBuild": "compat",
        "CFBundleSupportedPlatforms": ["iPhoneOS"],
    }
    info.update(m.get("info") or {})
    path = os.path.join(appdir, "Info.plist")
    with open(path, "wb") as fh:
        plistlib.dump(info, fh, fmt=plistlib.FMT_XML, sort_keys=True)
    return path


def read_plist(path):
    with open(path, "rb") as fh:
        return plistlib.load(fh)


# ---------------------------------------------------------------------- ipa --
def package_ipa(m, exe, outdir, name=None):
    """Assemble <Name>.app, validate it, and zip Payload/<Name>.app -> .ipa."""
    stem = name or m["name"]
    stage = os.path.join(outdir, "Payload")
    shutil.rmtree(stage, ignore_errors=True)
    appdir = os.path.join(stage, stem + ".app")
    os.makedirs(appdir)
    shutil.copy2(exe, os.path.join(appdir, stem))
    os.chmod(os.path.join(appdir, stem), 0o755)
    write_plist(m, appdir)
    for res in m.get("_resolved_resources", []):
        dst = os.path.join(appdir, os.path.basename(res))
        if os.path.isdir(res):
            shutil.copytree(res, dst)
        else:
            shutil.copy2(res, dst)

    problems = validate_app(appdir, stem)
    if problems:
        die("bundle validation failed:\n  - " + "\n  - ".join(problems))

    ipa = os.path.join(outdir, stem + ".ipa")
    _zip_dir(appdir, ipa, arcroot="Payload/" + stem + ".app")
    return ipa


def _zip_dir(appdir, ipa, arcroot):
    if os.path.exists(ipa):
        os.unlink(ipa)
    with zipfile.ZipFile(ipa, "w", zipfile.ZIP_DEFLATED) as z:
        for root, dirs, files in os.walk(appdir):
            dirs.sort()
            for f in sorted(files):
                full = os.path.join(root, f)
                rel = os.path.relpath(full, appdir)
                arc = arcroot + "/" + rel.replace(os.sep, "/")
                zi = zipfile.ZipInfo(arc, time.localtime(os.path.getmtime(full))[:6])
                mode = os.stat(full).st_mode & 0o777
                zi.external_attr = (mode | (0o40000 if os.path.isdir(full) else 0o100000)) << 16
                zi.compress_type = zipfile.ZIP_DEFLATED
                with open(full, "rb") as fh:
                    z.writestr(zi, fh.read())


def validate_app(appdir, stem):
    """Everything an IPA must satisfy to be worth shipping. Returns problems[]."""
    problems = []
    exe = os.path.join(appdir, stem)
    if not os.path.exists(exe):
        return ["executable %s missing" % stem]
    if not os.access(exe, os.X_OK):
        problems.append("executable is not marked executable")
    try:
        info = macho_info(exe)
    except SystemExit:
        return ["executable is not a Mach-O"]
    if info["fat"]:
        problems.append("executable is a fat binary; an App Store IPA is thin")
    if info["arch"] != "arm64":
        problems.append("arch is %s, not arm64" % info["arch"])
    if info["platform"] != "ios":
        problems.append("LC_BUILD_VERSION platform is %r, not ios" % info["platform"])
    if info["kind"] != "executable":
        problems.append("file type is %r, not an executable" % info["kind"])
    if not info["pie"]:
        problems.append("not PIE (iOS requires position independence)")
    if info["encrypted"]:
        problems.append("cryptid is set: this binary is FairPlay-encrypted")
    for lib in info["needed"]:
        if lib.startswith(("/usr/lib/", "/System/Library/", "@rpath/libswift", "@executable_path", "@loader_path")):
            continue
        problems.append("links non-system library %s" % lib)

    info_plist = os.path.join(appdir, "Info.plist")
    if not os.path.exists(info_plist):
        problems.append("Info.plist missing")
    else:
        try:
            pl = read_plist(info_plist)
        except Exception as exc:                                  # noqa: BLE001
            return problems + ["Info.plist unreadable: %s" % exc]
        for key in ("CFBundleExecutable", "CFBundleIdentifier", "CFBundleName",
                    "CFBundleVersion", "MinimumOSVersion"):
            if key not in pl:
                problems.append("Info.plist lacks %s" % key)
        if pl.get("CFBundleExecutable") not in (None, stem):
            problems.append("CFBundleExecutable %r does not match %r"
                            % (pl.get("CFBundleExecutable"), stem))
        if pl.get("CFBundleIdentifier", "").count(".") < 2:
            problems.append("CFBundleIdentifier %r is not reverse-DNS"
                            % pl.get("CFBundleIdentifier"))
    return problems


def inspect_ipa(path, as_json=False):
    """Report what a .ipa (or .app) actually contains."""
    result = {"path": os.path.abspath(path), "kind": None, "app": None,
              "executable": None, "entries": 0, "size": os.path.getsize(path)}
    tmp = None
    try:
        if os.path.isdir(path):
            appdir = path
            result["kind"] = "app"
        else:
            result["kind"] = "ipa"
            tmp = tempfile.mkdtemp(prefix="iosbuild-inspect-")
            with zipfile.ZipFile(path) as z:
                names = z.namelist()
                result["entries"] = len(names)
                tops = {n.split("/")[0] for n in names}
                if "Payload" not in tops:
                    die("not an IPA: no Payload/ directory")
                z.extractall(tmp)
            payload = os.path.join(tmp, "Payload")
            apps = [d for d in os.listdir(payload) if d.endswith(".app")]
            if not apps:
                die("not an IPA: Payload/ contains no .app")
            appdir = os.path.join(payload, apps[0])
        result["app"] = os.path.basename(appdir)
        pl = os.path.join(appdir, "Info.plist")
        if os.path.exists(pl):
            data = read_plist(pl)
            result["bundle_id"] = data.get("CFBundleIdentifier")
            result["version"] = data.get("CFBundleShortVersionString")
            result["min_os"] = data.get("MinimumOSVersion")
            exe_name = data.get("CFBundleExecutable")
        else:
            exe_name = os.path.basename(appdir)[:-4]
        exe = os.path.join(appdir, exe_name) if exe_name else None
        if exe and os.path.exists(exe):
            info = macho_info(exe)
            result["executable"] = {
                "name": exe_name, "arch": info["arch"], "platform": info["platform"],
                "minos": info["minos"], "kind": info["kind"], "pie": info["pie"],
                "needed": info["needed"], "size": os.path.getsize(exe),
                "sha256": _sha256(exe),
            }
        result["problems"] = validate_app(appdir, exe_name or "")
        return result
    finally:
        if tmp:
            shutil.rmtree(tmp, ignore_errors=True)


def _sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as fh:
        for chunk in iter(lambda: fh.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


# ---------------------------------------------------------------------- cli --
USAGE = """iosbuild - build an unsigned iOS app on Linux

  iosbuild.py plan    <dir|ipa.json> [--sdk PATH] [--json]
  iosbuild.py build   <dir|ipa.json> [--sdk PATH] [--out DIR] [-v]
  iosbuild.py ipa     <dir|ipa.json> [--sdk PATH] [--out DIR] [--name NAME]
  iosbuild.py inspect <file.ipa|File.app> [--json]

manifest keys (ipa.json, all optional except sources):
  name, bundle_id, version, build, min_ios, sources[], frameworks[],
  libraries[], defines[], includes[], resources[], info{}, required_capabilities[]

environment:
  IPHONEOS_SDK   SDK path, same as --sdk
exit codes: 0 ok, 1 build error, 2 usage, 3 missing toolchain/SDK
"""


def cmd_plan(m, sdk, as_json):
    overlay = overlay_sdk(sdk)
    plan = {
        "name": m["name"], "bundle_id": m["bundle_id"],
        "version": m["version"], "build": m["build"],
        "min_ios": m["min_ios"],
        "sdk": {"path": sdk, "version": sdk_version(sdk), "archs": sdk_archs(sdk),
                "overlay": overlay},
        "sources": m["sources"],
        "frameworks": m.get("frameworks", []),
        "libraries": m.get("libraries", []),
        "compile_flags": clang_flags(sdk, m),
        "tooling": {"clang": which("clang"), "ld64.lld": which("ld64.lld")},
    }
    if as_json:
        print(json.dumps(plan, indent=2))
    else:
        print("app        %s (%s) %s (%s)" % (m["name"], m["bundle_id"],
                                              m["version"], m["build"]))
        print("sdk        %s  [%s]" % (plan["sdk"]["path"], plan["sdk"]["version"]))
        print("archs      %s" % ", ".join(plan["sdk"]["archs"] or ["?"]))
        print("min ios    %s" % m["min_ios"])
        print("sources    %d" % len(m["sources"]))
        for s in m["sources"]:
            print("           %s" % os.path.relpath(s, m["_dir"]))
        print("frameworks %s" % (", ".join(m["frameworks"]) or "-"))
        print("libraries  %s" % (", ".join(m["libraries"]) or "-"))
    return 0


def cmd_build(m, sdk, outdir, do_ipa, name, verbose):
    t0 = time.time()
    work = os.path.join(outdir, "build")
    os.makedirs(work, exist_ok=True)
    exe = build(m, sdk, work, verbose)
    log("  ok  %s (%d bytes, arm64 ios %s)" %
        (os.path.basename(exe), os.path.getsize(exe), m["min_ios"]))
    if do_ipa:
        ipa = package_ipa(m, exe, outdir, name)
        info = inspect_ipa(ipa)
        if info["problems"]:
            die("\n".join(info["problems"]))
        print(json.dumps({"ipa": os.path.abspath(ipa),
                          "sha256": _sha256(ipa),
                          "size": os.path.getsize(ipa),
                          "bundle_id": info.get("bundle_id"),
                          "seconds": round(time.time() - t0, 1)}, indent=2))
    return 0


def main(argv):
    global QUIET
    if len(argv) < 2 or argv[1] in ("-h", "--help", "help"):
        sys.stdout.write(USAGE)
        return 0 if len(argv) > 1 else 2
    cmd = argv[1]
    args = argv[2:]
    opts = {"sdk": None, "out": None, "name": None, "json": False,
            "verbose": False, "target": None}
    positional = []
    i = 0
    while i < len(args):
        a = args[i]
        if a in ("--sdk", "--out", "--name"):
            if i + 1 >= len(args):
                die("%s needs a value" % a, 2)
            opts[a[2:]] = args[i + 1]
            i += 2
            continue
        if a in ("--json", "-j"):
            opts["json"] = True
        elif a in ("-v", "--verbose"):
            opts["verbose"] = True
            QUIET = False
        elif a.startswith("-"):
            die("unknown option %s" % a, 2)
        else:
            positional.append(a)
        i += 1
    if not positional:
        die("missing <dir|ipa.json>", 2)
    target = positional[0]

    if cmd == "inspect":
        info = inspect_ipa(target, opts["json"])
        if opts["json"]:
            print(json.dumps(info, indent=2))
        else:
            print("%s  (%s)" % (info["path"], info["kind"]))
            print("app        %s" % info.get("app"))
            if info.get("bundle_id"):
                print("bundle     %s %s (min iOS %s)" % (info["bundle_id"],
                                                         info.get("version"),
                                                         info.get("min_os")))
            ex = info.get("executable")
            if ex:
                print("executable %s: %s, platform %s, minos %s, PIE %s"
                      % (ex["name"], ex["arch"], ex["platform"], ex["minos"], ex["pie"]))
                print("imports    %s" % (", ".join(ex["needed"]) or "none"))
            if info["kind"] == "ipa":
                print("entries    %d" % info["entries"])
            print("problems   %s" % (", ".join(info["problems"]) or "none"))
        return 0 if not info.get("problems") else 1

    m = load_manifest(target)
    sdk = find_sdk(opts["sdk"])
    if cmd == "plan":
        return cmd_plan(m, sdk, opts["json"])
    if cmd in ("build", "ipa"):
        out = os.path.abspath(opts["out"] or os.path.join(m["_dir"], "build-out"))
        return cmd_build(m, sdk, out, cmd == "ipa", opts["name"], opts["verbose"])
    die("unknown command %r (plan|build|ipa|inspect)" % cmd, 2)


if __name__ == "__main__":
    try:
        sys.exit(main(sys.argv))
    except KeyboardInterrupt:
        sys.exit(130)
