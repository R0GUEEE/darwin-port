import contextlib
import importlib.util
import io
import json
import os
from pathlib import Path
import subprocess
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('setup', ROOT / 'scripts/setup.py')
setup = importlib.util.module_from_spec(spec)
spec.loader.exec_module(setup)


class SetupTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.path = Path(self.tmp.name)
        self.config = self.path / 'config.env'

    def invoke(self, *args):
        with patch.dict(os.environ, {}, clear=True), contextlib.redirect_stdout(io.StringIO()) as out:
            code = setup.main([*args, '--config', str(self.config)])
        return code, out.getvalue()

    def test_auto_profile_on_x64(self):
        with patch.object(setup.platform, 'machine', return_value='x86_64'):
            self.assertEqual(setup.resolve_profile('auto'), 'tools')
            with self.assertRaisesRegex(ValueError, 'ARM64 Linux'):
                setup.resolve_profile('full')

    def test_ipa_profile_available_on_x64(self):
        with patch.object(setup.platform, 'machine', return_value='x86_64'), patch.object(setup.platform, 'system', return_value='Linux'):
            self.assertEqual(setup.resolve_profile('ipa'), 'ipa')

    def test_auto_profile_on_arm_linux(self):
        with patch.object(setup.platform, 'machine', return_value='aarch64'), patch.object(setup.platform, 'system', return_value='Linux'):
            self.assertEqual(setup.resolve_profile('auto'), 'full')

    def test_plan_is_read_only(self):
        with patch.object(setup, 'execute') as run, patch.object(setup, 'write_config') as write:
            code, out = self.invoke('plan', '--profile', 'tools', '--json')
        self.assertEqual(code, 0)
        self.assertEqual(json.loads(out)['profile'], 'tools')
        run.assert_not_called()
        write.assert_not_called()
        self.assertFalse(self.config.exists())

    def test_check_only_install_never_mutates(self):
        with patch.object(setup, 'execute') as run, patch.object(setup, 'write_config') as write:
            self.invoke('install', '--profile', 'tools', '--check-only')
        run.assert_not_called()
        write.assert_not_called()

    def test_config_created_and_shell_roundtrip(self):
        sdk = self.path / "SDK with spaces and 'quote'"
        for directory in setup.SDK_DIRS:
            (sdk / directory).mkdir(parents=True)
        self.invoke('configure', '--profile', 'tools', '--sdk', str(sdk))
        values = setup.read_config(self.config)
        self.assertEqual(values['DARWIN_SDK'], str(sdk))
        out = subprocess.check_output(['sh', '-c', '. "$1"; printf %s "$DARWIN_SDK"', 'sh', str(self.config)], text=True)
        self.assertEqual(out, str(sdk))

    def test_idempotent_and_backup(self):
        setup.write_config(self.config, {'DARWIN_MINOS': '14.0'})
        stamp = self.config.stat().st_mtime_ns
        setup.write_config(self.config, {'DARWIN_MINOS': '14.0'})
        self.assertEqual(stamp, self.config.stat().st_mtime_ns)
        setup.write_config(self.config, {'DARWIN_MINOS': '15.0'})
        self.assertIn('14.0', Path(str(self.config) + '.bak').read_text())

    def test_invalid_sdk_does_not_write(self):
        with self.assertRaisesRegex(ValueError, 'SDK must contain'):
            self.invoke('configure', '--profile', 'tools', '--sdk', str(self.path))
        self.assertFalse(self.config.exists())

    def test_no_sdk_removes_saved_value(self):
        setup.write_config(self.config, {'DARWIN_SDK': '/nonexistent'})
        self.invoke('configure', '--profile', 'tools', '--no-sdk')
        self.assertNotIn('DARWIN_SDK', setup.read_config(self.config))

    def test_config_cannot_execute_shell(self):
        marker = self.path / 'executed'
        self.config.write_text(f'DARWIN_SDK=$(touch {marker})\n')
        with self.assertRaises(ValueError):
            setup.read_config(self.config)
        self.assertFalse(marker.exists())

    def test_unknown_setting_rejected(self):
        with self.assertRaisesRegex(ValueError, 'unsupported setting'):
            self.invoke('configure', '--set', 'PATH=/tmp')

    def test_newline_rejected(self):
        with self.assertRaises(ValueError):
            self.invoke('configure', '--set', 'DS_SYSROOT=a\nb')

    def test_doctor_missing_tool_returns_failure(self):
        with patch.object(setup.shutil, 'which', return_value=None):
            code, _ = self.invoke('doctor', '--profile', 'tools')
        self.assertEqual(code, 1)

    def test_package_failure_does_not_claim_success_or_write(self):
        with patch.object(setup, 'manager', return_value='apt-get'), patch.object(setup.os, 'geteuid', return_value=0), patch.object(setup, 'execute', side_effect=subprocess.CalledProcessError(9, ['apt-get'])):
            with self.assertRaises(subprocess.CalledProcessError):
                self.invoke('install', '--profile', 'tools', '--yes')
        self.assertFalse(self.config.exists())

    def test_tools_install_does_not_build(self):
        with patch.object(setup.shutil, 'which', return_value='/bin/tool'), patch.object(setup, 'execute') as run:
            code, _ = self.invoke('install', '--profile', 'tools', '--no-packages')
        self.assertEqual(code, 0)
        run.assert_not_called()
        self.assertTrue(self.config.exists())

    def test_ipa_profile_installs_cross_compiler_without_native_build(self):
        with patch.object(setup, 'manager', return_value='apt-get'), patch.object(setup.shutil, 'which', return_value='/bin/tool'), patch.object(setup, 'execute') as run:
            code, out = self.invoke('plan', '--profile', 'ipa', '--json')
        report = json.loads(out)
        self.assertEqual(code, 0)
        self.assertFalse(report['build'])
        packages = report['packages'][-1]
        self.assertIn('clang', packages)
        self.assertIn('lld', packages)

    def test_config_propagates_to_build_and_tests(self):
        with patch.object(setup.platform, 'system', return_value='Linux'), patch.object(setup.platform, 'machine', return_value='aarch64'), patch.object(setup.shutil, 'which', return_value='/bin/tool'), patch.object(setup, 'execute') as run:
            self.invoke('install', '--profile', 'full', '--no-packages', '--set', 'DS_SYSROOT=/guest', '--log', str(self.path / 'log'))
        self.assertEqual(len(run.call_args_list), 2)
        self.assertEqual(run.call_args_list[1].args[0], ['make', 'test'])
        self.assertEqual(run.call_args_list[1].args[1]['DS_SYSROOT'], '/guest')

    def test_each_package_manager_has_core_and_full_packages(self):
        for manager in setup.PACKAGES:
            for profile in ['tools', 'ipa', 'core', 'full', 'sdk']:
                self.assertTrue(setup.package_commands(manager, profile, True))
                self.assertTrue(setup.package_commands(manager, profile, False))

    def test_run_preserves_arguments_and_saved_environment(self):
        setup.write_config(self.config, {'DS_SYSROOT': '/guest with spaces'})
        with patch.object(setup, 'execute') as run:
            with contextlib.redirect_stdout(io.StringIO()):
                setup.main(['run', '--config', str(self.config), '--', 'python3', '-c', 'print(42)'])
        self.assertEqual(run.call_args.args[0], ['python3', '-c', 'print(42)'])
        self.assertEqual(run.call_args.args[1]['DS_SYSROOT'], '/guest with spaces')


if __name__ == '__main__':
    unittest.main()
