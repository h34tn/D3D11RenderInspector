import csv
import json
import os
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
from launch_diagnostics import diagnose, loaded_modules, trace_deletions


class LaunchDiagnosticTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        self.exe = self.root / 'Any Game.exe'
        self.exe.touch()
        self.proxy = self.root / 'custom.dll'
        self.proxy.write_bytes(b'proxy')
        import hashlib
        self.digest = hashlib.sha256(b'proxy').hexdigest()
        (self.root / '.d3d11-inspector-install.json').write_text(json.dumps(dict(
            version=1, filename=self.proxy.name, sha256=self.digest, source='test')), encoding='utf8')

    def check_launch(self, modules=(), identity=None):
        identity = identity or (123, str(self.exe))
        with patch('gui_support.process_identity', return_value=identity), \
                patch('launch_diagnostics.loaded_modules', return_value=modules):
            return diagnose(self.exe, self.root, 42)

    def test_missing_dll_is_not_repaired_or_attributed_without_trace(self):
        self.proxy.unlink()
        report = self.check_launch([str(Path(os.environ['SystemRoot']) / 'System32/d3d11.dll')])
        self.assertEqual(report['status'], 'proxy_missing')
        self.assertNotIn('delete_events', report)
        self.assertFalse(self.proxy.exists())

    def test_loaded_proxy_is_distinguished_from_an_intact_unloaded_file(self):
        self.assertEqual(self.check_launch()['status'], 'not_loaded')
        self.assertEqual(self.check_launch([str(self.proxy)])['status'], 'loaded_without_session')

    def test_pid_reuse_does_not_match_old_forwarding_log(self):
        session = self.root / 'pid_42_122'
        session.mkdir()
        (session / 'proxy.log').write_text('observer mode=forward')
        self.assertEqual(self.check_launch()['status'], 'not_loaded')
        session.rename(self.root / 'pid_42_123')
        self.assertEqual(self.check_launch()['status'], 'forwarding')
        self.assertEqual(self.check_launch(identity=(123, str(self.root / 'different.exe')))['status'], 'process_unverified')

    def test_permission_denied_remains_unknown(self):
        with patch('gui_support.process_identity', side_effect=OSError('access denied')):
            report = diagnose(self.exe, self.root, 42)
        self.assertEqual(report['status'], 'process_unverified')
        self.assertNotIn('process', report)

    def test_process_change_during_enumeration_discards_module_result(self):
        with patch('gui_support.process_identity', side_effect=[(123, str(self.exe)), (124, str(self.exe))]), \
                patch('launch_diagnostics.loaded_modules', return_value=[str(self.proxy)]):
            report = diagnose(self.exe, self.root, 42)
        self.assertEqual(report['status'], 'process_unverified')
        self.assertNotIn('proxy_loaded', report)

    def test_changed_proxy_is_preserved(self):
        self.proxy.write_bytes(b'user change')
        self.assertEqual(self.check_launch()['status'], 'proxy_changed')
        self.assertEqual(self.proxy.read_bytes(), b'user change')

    def test_invalid_manifest_does_not_guess_a_default_proxy_name(self):
        (self.root / '.d3d11-inspector-install.json').write_text('invalid json')
        report = self.check_launch()
        self.assertEqual(report['status'], 'deployment_unverified')
        self.assertNotIn('proxy', report)
        self.assertEqual(self.proxy.read_bytes(), b'proxy')

    def test_trace_requires_successful_delete_for_exact_managed_path(self):
        trace = self.root / 'trace.csv'
        fields = ['Time of Day', 'Process Name', 'PID', 'Operation', 'Path', 'Result', 'Detail']
        with trace.open('w', encoding='utf-8-sig', newline='') as stream:
            writer = csv.DictWriter(stream, fieldnames=fields)
            writer.writeheader()
            for target, operation, result, detail in [
                    (self.proxy, 'CreateFile', 'SUCCESS', 'Desired Access: Delete'),
                    (self.proxy, 'SetDispositionInformationEx', 'ACCESS DENIED', 'FILE_DISPOSITION_DELETE'),
                    (self.root / 'unrelated.dll', 'SetDispositionInformationEx', 'SUCCESS', 'FILE_DISPOSITION_DELETE'),
                    (self.proxy, 'SetDispositionInformationEx', 'SUCCESS', 'FILE_DISPOSITION_DELETE')]:
                writer.writerow(dict(zip(fields, ['12:00', 'AnyGame.exe', '42', operation, str(target), result, detail])))
        events = trace_deletions(trace, self.proxy)
        self.assertEqual(len(events), 1)
        self.assertEqual(events[0]['Process Name'], 'AnyGame.exe')

    def test_native_module_enumeration_for_current_python_process(self):
        paths = loaded_modules(os.getpid())
        self.assertTrue(any(Path(p).name.lower() == Path(sys.executable).name.lower() for p in paths))


if __name__ == '__main__':
    unittest.main()
