import json
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1]/'tools'))
from inspector import capture, install, read_capture, repair, sha256, uninstall


class AnalysisTests(unittest.TestCase):
    def test_truncated_capture_never_reports_complete(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory)/'capture_1.jsonl'
            path.write_text('{"type":"header"}\n{"event":1,"call":"Draw"}\n{"event":', encoding='utf8')
            events, complete, errors = read_capture(path)
            self.assertFalse(complete)
            self.assertEqual(len(events), 1)
            self.assertEqual(len(errors), 1)

    def test_end_count_and_sequence_must_match(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory)/'capture_1.jsonl'
            for event_number, end_count, expected in [(1,1,True),(1,2,False),(2,1,False)]:
                rows = [{'type':'header'}, {'event':event_number,'call':'Draw'}, {'type':'end','events':end_count}]
                path.write_text('\n'.join(json.dumps(r) for r in rows), encoding='utf8')
                self.assertEqual(read_capture(path)[1], expected)

    def test_capture_does_not_replace_pending_request(self):
        with tempfile.TemporaryDirectory() as directory:
            session = Path(directory)
            (session/'proxy.log').touch()
            capture(session, 10)
            with self.assertRaises(ValueError): capture(session, 20)
            self.assertEqual((session/'capture.request').read_text(), '10')
            with self.assertRaises(ValueError): capture(session, 10001)

    def test_install_and_uninstall_preserve_changed_or_existing_dll(self):
        binary = Path(__file__).resolve().parents[1]/'build/d3d11.dll'
        with tempfile.TemporaryDirectory() as directory:
            folder = Path(directory)
            target = folder/'d3d11.dll'
            target.write_bytes(b'pre-existing')
            with self.assertRaises(ValueError): install(folder, binary, 'd3d11.dll')
            self.assertEqual(target.read_bytes(), b'pre-existing')
            target.unlink()
            install(folder, binary, 'd3d11.dll')
            original = target.read_bytes()
            target.write_bytes(b'changed')
            with self.assertRaises(ValueError): uninstall(folder)
            self.assertEqual(target.read_bytes(), b'changed')
            target.write_bytes(original)
            uninstall(folder)
            self.assertFalse(target.exists())


class DeploymentRepairTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.folder = Path(self.temporary.name)
        self.binary = Path(__file__).resolve().parents[1] / 'build/d3d11.dll'
        self.target = self.folder / 'd3d11.dll'
        self.manifest = self.folder / '.d3d11-inspector-install.json'
        # A valid manifest left by an older build, whose DLL has disappeared.
        self.original = json.dumps(dict(version=1, filename='d3d11.dll', sha256='0' * 64,
                                        source=str(self.binary))).encode('utf8')
        self.manifest.write_bytes(self.original)

    def tearDown(self):
        self.temporary.cleanup()

    def test_restore_missing_old_build_preserves_manifest_and_can_uninstall(self):
        backup = repair(self.folder, self.binary)
        self.assertEqual(backup.read_bytes(), self.original)
        self.assertEqual(sha256(self.target), sha256(self.binary))
        info = json.loads(self.manifest.read_text(encoding='utf8'))
        self.assertEqual(info['sha256'], sha256(self.target))
        uninstall(self.folder)
        self.assertTrue(backup.is_file())
        self.assertFalse(self.target.exists())

    def test_existing_dll_is_never_overwritten(self):
        self.target.write_bytes(b'user-owned file')
        with self.assertRaisesRegex(ValueError, '拒绝覆盖'):
            repair(self.folder, self.binary)
        self.assertEqual(self.target.read_bytes(), b'user-owned file')
        self.assertEqual(self.manifest.read_bytes(), self.original)
        self.assertEqual(list(self.folder.glob('*.backup-*')), [])

    def test_corrupt_or_unsafe_manifest_is_not_repaired(self):
        for data in [b'not JSON', b'[]', b'{"version":99}',
                     json.dumps(dict(version=1, filename='../other.dll', sha256='0'*64, source='x')).encode()]:
            self.manifest.write_bytes(data)
            with self.assertRaises(ValueError):
                repair(self.folder, self.binary)
            self.assertEqual(self.manifest.read_bytes(), data)
            self.assertFalse(self.target.exists())

    def test_custom_managed_name_repairs_and_uninstalls_only_named_file(self):
        self.target.write_bytes(b'unrelated default DLL')
        info = json.loads(self.original)
        info['filename'] = 'custom.dll'
        self.manifest.write_text(json.dumps(info), encoding='utf8')
        backup = repair(self.folder, self.binary)
        self.assertEqual(sha256(self.folder / 'custom.dll'), sha256(self.binary))
        uninstall(self.folder)
        self.assertEqual(self.target.read_bytes(), b'unrelated default DLL')
        self.assertTrue(backup.is_file())

    def test_failed_manifest_publish_rolls_back_only_new_dll(self):
        with patch('inspector.os.replace', side_effect=PermissionError('test publish failure')):
            with self.assertRaises(PermissionError):
                repair(self.folder, self.binary)
        self.assertEqual(self.manifest.read_bytes(), self.original)
        self.assertFalse(self.target.exists())
        backups = list(self.folder.glob('*.backup-*'))
        self.assertEqual(len(backups), 1)
        self.assertEqual(backups[0].read_bytes(), self.original)
        self.assertEqual(list(self.folder.glob('inspector-repair-*.tmp')), [])


if __name__ == '__main__': unittest.main()
