#!/usr/bin/env python3
"""Tests for iosbuild.py.

Two layers, deliberately separated:

  * fixture tests synthesise Mach-O bytes, TBD text and bundles, so they run on
    any machine with python3 and cover the parsers that decide what ships;
  * one integration test builds examples/winprobe with a real iPhoneOS SDK and
    skips cleanly when none is present (CI on Linux has no SDK, the machine this
    was written on does).

    python3 tests/iosbuild/test_iosbuild.py [-v]
"""
import json
import os
import plistlib
import struct
import subprocess
import sys
import tempfile
import unittest
import zipfile

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, ROOT)
import iosbuild  # noqa: E402


def macho(platform=2, minos=(15, 0), filetype=2, flags=0x200000, dylibs=(),
          bits=64, endian="<", fat=False):
    """Synthesise a Mach-O with the load commands iosbuild actually reads."""
    cmds = b""
    ncmds = 0
    for lib in dylibs:
        name = lib.encode() + b"\0"
        name += b"\0" * ((8 - len(name) % 8) % 8)
        cmds += struct.pack(endian + "IIIIII", 0xC, 24 + len(name), 24, 0, 0, 0) + name
        ncmds += 1
    if platform is not None:
        def v(major, minor):
            return (major << 16) | (minor << 8)
        cmds += struct.pack(endian + "IIIIII", 0x32, 24, platform, v(*minos), v(27, 0), 0)
        ncmds += 1
    head = struct.pack(endian + "IiiIIIII", 0xFEEDFACF, 0x0100000C, 0, filetype,
                       ncmds, len(cmds), flags, 0)
    body = head + cmds
    if not fat:
        return body
    off = 4096
    fathead = struct.pack(">II", 0xCAFEBABE, 1) + struct.pack(
        ">iiIII", 0x0100000C, 0, off, len(body), 12)
    return fathead + b"\0" * (off - len(fathead)) + body


def write(tmp, name, data, mode=0o755):
    p = os.path.join(tmp, name)
    os.makedirs(os.path.dirname(p), exist_ok=True) if os.path.dirname(p) else None
    with open(p, "wb") as fh:
        fh.write(data)
    os.chmod(p, mode)
    return p


class MachOTests(unittest.TestCase):
    def test_ios_platform_detected(self):
        with tempfile.TemporaryDirectory() as tmp:
            p = write(tmp, "app", macho(platform=2, minos=(16, 4)))
            info = iosbuild.macho_info(p)
        self.assertEqual(info["platform"], "ios")
        self.assertEqual(info["minos"], "16.4.0")
        self.assertEqual(info["arch"], "arm64")
        self.assertEqual(info["kind"], "executable")
        self.assertTrue(info["pie"])

    def test_macos_platform_is_not_ios(self):
        with tempfile.TemporaryDirectory() as tmp:
            p = write(tmp, "app", macho(platform=1, minos=(13, 0)))
            info = iosbuild.macho_info(p)
        self.assertEqual(info["platform"], "macos")
        self.assertNotEqual(info["platform"], "ios")

    def test_imports_parsed(self):
        with tempfile.TemporaryDirectory() as tmp:
            p = write(tmp, "app", macho(dylibs=("/usr/lib/libSystem.B.dylib",
                                                "/System/Library/Frameworks/UIKit.framework/UIKit")))
            info = iosbuild.macho_info(p)
        self.assertEqual(info["needed"],
                         ["/usr/lib/libSystem.B.dylib",
                          "/System/Library/Frameworks/UIKit.framework/UIKit"])

    def test_fat_binary_flagged(self):
        with tempfile.TemporaryDirectory() as tmp:
            p = write(tmp, "app", macho(fat=True))
            info = iosbuild.macho_info(p)
        self.assertTrue(info["fat"])
        self.assertEqual(len(info["slices"]), 1)

    def test_rejects_non_macho(self):
        with tempfile.TemporaryDirectory() as tmp:
            p = write(tmp, "notmacho", b"\x7fELF" + b"\0" * 60)
            with self.assertRaises(SystemExit):
                iosbuild.macho_info(p)


class TBDTests(unittest.TestCase):
    def test_arm64e_targets_rewritten(self):
        text = ("--- !tapi-tbd\ntbd-version: 4\ntargets: [ arm64e-ios, arm64e.x1-ios ]\n"
                "install-name: '/usr/lib/libSystem.B.dylib'\n"
                "exports:\n  - targets: [ arm64e-ios ]\n    symbols: [ _malloc ]\n")
        out = iosbuild._rewrite_tbd(text)
        self.assertNotIn("arm64e", out)
        self.assertIn("targets: [ arm64-ios ]", out)
        self.assertIn("symbols: [ _malloc ]", out)

    def test_duplicates_collapsed(self):
        out = iosbuild._rewrite_tbd("targets: [ arm64e-ios, arm64e.x1-ios, arm64-ios ]\n")
        self.assertEqual(out.strip(), "targets: [ arm64-ios ]")

    def test_plain_tbd_untouched(self):
        text = "targets: [ arm64-ios ]\nsymbols: [ _x ]\n"
        self.assertEqual(iosbuild._rewrite_tbd(text), text)


class ManifestTests(unittest.TestCase):
    def _manifest(self, tmp, data):
        p = os.path.join(tmp, "ipa.json")
        with open(p, "w") as fh:
            json.dump(data, fh)
        return p

    def test_defaults_filled(self):
        with tempfile.TemporaryDirectory() as tmp:
            write(tmp, "main.m", b"int main(void){return 0;}")
            m = iosbuild.load_manifest(self._manifest(tmp, {"sources": ["main.m"]}))
            self.assertEqual(m["min_ios"], "15.0")
            self.assertTrue(m["bundle_id"].startswith("com.example."))
            self.assertEqual(len(m["sources"]), 1)

    def test_directory_without_manifest_is_built(self):
        with tempfile.TemporaryDirectory() as tmp:
            write(tmp, "a.m", b"int main(void){return 0;}")
            m = iosbuild.load_manifest(tmp)
        self.assertEqual(len(m["sources"]), 1)

    def test_swift_sources_rejected_with_reason(self):
        with tempfile.TemporaryDirectory() as tmp:
            write(tmp, "main.swift", b"print(1)")
            with self.assertRaises(SystemExit) as cm:
                iosbuild.load_manifest(self._manifest(tmp, {"sources": ["main.swift"]}))
            self.assertEqual(cm.exception.code, 1)

    def test_empty_source_list_is_a_usage_error(self):
        with tempfile.TemporaryDirectory() as tmp:
            with self.assertRaises(SystemExit):
                iosbuild.load_manifest(self._manifest(tmp, {"sources": []}))

    def test_glob_that_matches_nothing_fails(self):
        with tempfile.TemporaryDirectory() as tmp:
            with self.assertRaises(SystemExit):
                iosbuild.load_manifest(self._manifest(tmp, {"sources": ["nope*.m"]}))


class BundleTests(unittest.TestCase):
    def _app(self, tmp, platform=2, bundle_id="com.example.t", exe="T"):
        appdir = os.path.join(tmp, "Payload", "T.app")
        os.makedirs(appdir)
        write(appdir, exe, macho(platform=platform))
        m = dict(iosbuild.DEFAULT_MANIFEST, name="T", bundle_id=bundle_id,
                 version="1.0", build="1", min_ios="15.0", _dir=tmp)
        iosbuild.write_plist(m, appdir)
        return appdir

    def test_valid_app_has_no_problems(self):
        with tempfile.TemporaryDirectory() as tmp:
            self.assertEqual(iosbuild.validate_app(self._app(tmp), "T"), [])

    def test_macos_platform_rejected(self):
        with tempfile.TemporaryDirectory() as tmp:
            problems = iosbuild.validate_app(self._app(tmp, platform=1), "T")
        self.assertTrue(any("platform" in p for p in problems), problems)

    def test_bad_bundle_id_rejected(self):
        with tempfile.TemporaryDirectory() as tmp:
            problems = iosbuild.validate_app(self._app(tmp, bundle_id="nodots"), "T")
        self.assertTrue(any("reverse-DNS" in p for p in problems), problems)

    def test_plist_roundtrip(self):
        with tempfile.TemporaryDirectory() as tmp:
            appdir = self._app(tmp)
            pl = iosbuild.read_plist(os.path.join(appdir, "Info.plist"))
        self.assertEqual(pl["CFBundleExecutable"], "T")
        self.assertEqual(pl["CFBundleIdentifier"], "com.example.t")
        self.assertEqual(pl["MinimumOSVersion"], "15.0")
        self.assertTrue(pl["LSRequiresIPhoneOS"])

    def test_ipa_roundtrip(self):
        with tempfile.TemporaryDirectory() as tmp:
            appdir = self._app(tmp)
            ipa = os.path.join(tmp, "T.ipa")
            iosbuild._zip_dir(appdir, ipa, "Payload/T.app")
            info = iosbuild.inspect_ipa(ipa)
        self.assertEqual(info["kind"], "ipa")
        self.assertEqual(info["bundle_id"], "com.example.t")
        self.assertEqual(info["executable"]["platform"], "ios")
        self.assertEqual(info["problems"], [])

    def test_ipa_without_payload_rejected(self):
        with tempfile.TemporaryDirectory() as tmp:
            ipa = os.path.join(tmp, "bad.ipa")
            with zipfile.ZipFile(ipa, "w") as z:
                z.writestr("nope/x", b"1")
            with self.assertRaises(SystemExit):
                iosbuild.inspect_ipa(ipa)


class IntegrationTest(unittest.TestCase):
    @unittest.skipUnless(os.environ.get("IPHONEOS_SDK") or
                         any(os.path.exists(p) for p in iosbuild.SDK_CANDIDATES) or
                         os.path.isdir("/root/iphoneos27.3-compat-sdk-with-headers"),
                         "no iPhoneOS SDK on this machine")
    def test_window_probe_ipa(self):
        sdk = iosbuild.find_sdk(None)
        with tempfile.TemporaryDirectory() as tmp:
            out = subprocess.run(
                [sys.executable, os.path.join(ROOT, "iosbuild.py"), "ipa",
                 os.path.join(ROOT, "examples", "winprobe"), "--sdk", sdk,
                 "--out", tmp], capture_output=True, text=True)
            self.assertEqual(out.returncode, 0, out.stderr[-2000:])
            report = json.loads(out.stdout)
            # Everything that touches report["ipa"] has to happen before the
            # TemporaryDirectory above is collected - the first version of this
            # test asserted a size on a file that had already been deleted.
            info = iosbuild.inspect_ipa(report["ipa"])
            ipa_size = os.path.getsize(report["ipa"])
            self.assertTrue(os.path.exists(report["ipa"]))
        self.assertEqual(info["executable"]["platform"], "ios")
        self.assertEqual(info["executable"]["arch"], "arm64")
        self.assertEqual(info["problems"], [])
        self.assertGreater(ipa_size, 1000)


if __name__ == "__main__":
    unittest.main(verbosity=2)
