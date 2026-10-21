import json
from pathlib import Path
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]/'tools'))
from inspector import capture, install, read_capture, uninstall


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


if __name__ == '__main__': unittest.main()
