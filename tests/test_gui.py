"""Workflow regression tests; no actual game directory is touched."""
import ctypes
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import threading
import time
import tkinter as tk
import unittest
from unittest.mock import MagicMock, patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
import gui_support as support
import inspector
from inspector_gui import InspectorApp, parse_options


def write_capture(path, complete=True):
    rows = [{'type': 'header'}, {'event': 1, 'call': 'Draw'}]
    if complete:
        rows.append({'type': 'end', 'events': 1})
    path.write_text('\n'.join(json.dumps(row) for row in rows), encoding='utf8')


class WorkflowTests(unittest.TestCase):
    def test_managed_custom_name_is_reused_without_touching_default_dll(self):
        with tempfile.TemporaryDirectory() as directory:
            folder = Path(directory)
            binary = folder / 'source.dll'
            binary.write_bytes(b'current build')
            custom = folder / 'custom.dll'
            custom.write_bytes(binary.read_bytes())
            default = folder / 'd3d11.dll'
            default.write_bytes(b'other tool')
            manifest = folder / '.d3d11-inspector-install.json'
            manifest.write_text(json.dumps(dict(version=1, filename=custom.name,
                                                sha256=inspector.sha256(custom), source=str(binary))))
            self.assertFalse(support.deployment_needed(folder, binary))
            custom.unlink()
            with self.assertRaises(inspector.MissingProxyError):
                support.deployment_needed(folder, binary)
            self.assertEqual(default.read_bytes(), b'other tool')

    def test_legacy_diagnostic_requires_current_pid_and_fresh_legacy_marker(self):
        with tempfile.TemporaryDirectory() as directory:
            folder = Path(directory)
            path = folder / 'D3D11Proxy_123.log'
            started = time.time() - 1
            self.assertIsNone(support.legacy_proxy_diagnostic(folder, 123, started))
            for text, expected in [('pid=123 build=observer-v4-private-tail', True),
                                   ('pid=124 build=observer-v4-private-tail', False),
                                   ('pid=123 build=D3D11RenderInspector-0.1-private-tail', False)]:
                path.write_text(text)
                self.assertEqual(bool(support.legacy_proxy_diagnostic(folder, 123, started)), expected)
            path.write_text('pid=123 build=observer-v4-private-tail')
            os.utime(path, (started - 60, started - 60))
            self.assertIsNone(support.legacy_proxy_diagnostic(folder, 123, started))

    def test_elevation_uses_runas_and_preserves_windows_quoted_arguments(self):
        shell, ole = MagicMock(), MagicMock()
        ole.CoInitializeEx.return_value = 0
        captured = {}
        def execute(pointer):
            info = ctypes.cast(pointer, ctypes.POINTER(support.ShellExecuteInfo)).contents
            captured.update(verb=info.lpVerb, file=info.lpFile, parameters=info.lpParameters,
                            directory=info.lpDirectory, size=info.cbSize)
            return 1
        shell.ShellExecuteExW.side_effect = execute
        arguments = ['--exe=D:\\游戏 目录\\game.exe', '--arguments=--name "a b" &', '--mode=observe']
        with patch.object(support.ctypes, 'WinDLL', side_effect=[shell, ole]):
            self.assertTrue(support.restart_elevated(arguments))
        self.assertEqual(captured['verb'], 'runas')
        self.assertEqual(Path(captured['file']).name, 'pythonw.exe')
        self.assertEqual(support.split_arguments(captured['parameters']),
                         [str(inspector.PROJECT / 'inspector.pyw'), *arguments])
        self.assertEqual(captured['size'], 112)  # Windows x64 SHELLEXECUTEINFOW ABI.
        ole.CoUninitialize.assert_called_once()

    def test_cancelled_uac_returns_false_and_other_errors_propagate(self):
        for error in [1223, 5]:
            shell, ole = MagicMock(), MagicMock()
            ole.CoInitializeEx.return_value = 0
            shell.ShellExecuteExW.return_value = 0
            with patch.object(support.ctypes, 'WinDLL', side_effect=[shell, ole]), \
                    patch.object(support.ctypes, 'get_last_error', return_value=error):
                if error == 1223:
                    self.assertFalse(support.restart_elevated([]))
                else:
                    with self.assertRaises(OSError):
                        support.restart_elevated([])
            ole.CoUninitialize.assert_called_once()

    def test_windows_arguments_roundtrip_and_no_shell_interpretation(self):
        args = ['-force-d3d11', '--path', 'D:\\含 空格\\folder\\', '', 'quoted "value"', '&', '$(echo unsafe)']
        self.assertEqual(support.split_arguments(subprocess.list2cmdline(args)), args)
        self.assertEqual(support.split_arguments('  '), [])

    def test_session_requires_matching_process_creation_time(self):
        stamp, image = support.process_identity(os.getpid())
        with tempfile.TemporaryDirectory() as directory:
            for expected_stamp, expected_alive in [(stamp, True), (stamp + 1, False)]:
                session = Path(directory) / f'pid_{os.getpid()}_{expected_stamp}'
                session.mkdir()
                (session / 'proxy.log').write_text('observer Context4 methods installed', encoding='utf8')
                info = support.session_info(session)
                self.assertEqual(info['alive'], expected_alive)
                self.assertEqual(info['observing'], True)
                if expected_alive:
                    self.assertEqual(info['executable'], image)
                    support.require_capture_ready(session)
                    write_capture(session / 'capture_1.jsonl', complete=False)
                    with self.assertRaisesRegex(ValueError, '上一份采样'):
                        support.require_capture_ready(session)
                else:
                    with self.assertRaisesRegex(ValueError, '进程已退出'):
                        support.require_capture_ready(session)

    def test_deployment_reuse_checks_manifest_and_both_hashes(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            game = root / 'game'
            game.mkdir()
            binary = root / 'source.dll'
            binary.write_bytes(b'current proxy')
            self.assertTrue(support.deployment_needed(game, binary))
            target = game / 'd3d11.dll'
            target.write_bytes(binary.read_bytes())
            with self.assertRaises(ValueError):
                support.deployment_needed(game, binary)
            manifest = game / '.d3d11-inspector-install.json'
            manifest.write_text(json.dumps({'version': 1, 'filename': target.name,
                                            'sha256': inspector.sha256(target), 'source': str(binary)}))
            self.assertFalse(support.deployment_needed(game, binary))
            binary.write_bytes(b'new version')
            with self.assertRaisesRegex(ValueError, '其他版本'):
                support.deployment_needed(game, binary)
            target.write_bytes(b'user change')
            with self.assertRaisesRegex(ValueError, '被修改'):
                support.deployment_needed(game, binary)
            self.assertEqual(target.read_bytes(), b'user change')
            target.unlink()
            with self.assertRaisesRegex(inspector.MissingProxyError, '代理 DLL 缺失'):
                support.deployment_needed(game, binary)

    def test_wait_does_not_accept_old_or_partial_capture(self):
        with tempfile.TemporaryDirectory() as directory, patch.object(support, 'session_info', return_value={'alive': True}):
            session = Path(directory)
            old = session / 'capture_9.jsonl'
            new = session / 'capture_10.jsonl'
            write_capture(old)
            write_capture(new, complete=False)
            stop = threading.Event()
            with self.assertRaises(TimeoutError):
                support.wait_capture(session, {old.name}, stop, lambda _: None, timeout=0)
            self.assertFalse(inspector.read_capture(new)[1])
            write_capture(new)
            self.assertEqual(support.wait_capture(session, {old.name}, stop, lambda _: None, timeout=0), new)
            stop.set()
            self.assertIsNone(support.wait_capture(session, set(), stop, lambda _: None))

    def test_detached_launch_keeps_arguments_and_process_environment(self):
        with tempfile.TemporaryDirectory() as directory, patch.object(inspector, 'pe_exports'), \
                patch.object(inspector.subprocess, 'Popen') as popen:
            exe = Path(directory) / 'space game.exe'
            exe.touch()
            original_mode = os.environ.get('D3D11_PROXY_MODE')
            inspector.launch(exe, Path(directory) / 'captures', 'observe', ['a b', '&'], detach_output=True)
            args, kwargs = popen.call_args
            self.assertEqual(args[0], [str(exe.resolve()), 'a b', '&'])
            self.assertEqual(kwargs['stdout'], subprocess.DEVNULL)
            self.assertEqual(kwargs['env']['D3D11_PROXY_MODE'], 'observe')
            self.assertEqual(os.environ.get('D3D11_PROXY_MODE'), original_mode)


class DesktopTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.root = tk.Tk()
        self.root.withdraw()
        self.app = InspectorApp(self.root, Path(self.directory.name) / 'gui.json')
        self.app.output.set(self.directory.name)

    def tearDown(self):
        self.app.busy = False
        self.app.close()
        self.directory.cleanup()

    def pump_until_idle(self):
        deadline = time.monotonic() + 10
        while self.app.busy and time.monotonic() < deadline:
            self.root.update()
            time.sleep(0.01)
        self.assertFalse(self.app.busy, 'GUI worker did not return')

    def test_multiple_sessions_require_explicit_selection(self):
        self.app.exe.set(str(Path(self.directory.name) / 'game.exe'))
        items = [dict(path=Path(self.directory.name) / f'pid_{pid}_1', pid=pid, alive=True,
                      executable=self.app.exe.get(), status='可采样', captures=0) for pid in [10, 20]]
        self.app._show_sessions(items)
        self.assertEqual(self.app.tree.selection(), ())
        self.app.tree.selection_set(str(items[1]['path']))
        self.app._show_sessions(items)
        self.assertEqual(self.app.selected(), items[1]['path'])

    def test_launch_740_offers_elevation_without_reinstalling(self):
        exe = Path(self.directory.name) / 'game.exe'
        exe.touch()
        self.app.exe.set(str(exe))
        self.app.admin = False
        with patch('inspector_gui.pe_exports', return_value=[]), \
                patch('inspector_gui.deployment_needed', return_value=False), \
                patch('inspector_gui.launch', side_effect=ctypes.WinError(740)), \
                patch('inspector_gui.run_cli') as cli, \
                patch('inspector_gui.messagebox.askyesno', return_value=False) as prompt, \
                patch('inspector_gui.messagebox.showerror') as error:
            self.app.launch()
            self.pump_until_idle()
            prompt.assert_called_once()
            error.assert_not_called()
            cli.assert_not_called()
        self.assertIn('尚未启动', self.app.status.get())

    def test_launch_repairs_missing_dll_before_start_and_stops_on_failed_repair(self):
        exe = Path(self.directory.name) / 'game.exe'
        exe.touch()
        self.app.exe.set(str(exe))
        for failure in [None, RuntimeError('repair failed')]:
            calls = []
            def cli(arguments, log):
                calls.append(arguments[0])
                if failure:
                    raise failure
            def start(*args, **kwargs):
                calls.append('launch')
                return MagicMock(pid=123)
            with patch('inspector_gui.pe_exports', return_value=[]), \
                    patch('inspector_gui.deployment_needed', side_effect=[inspector.MissingProxyError('missing'), False]), \
                    patch('inspector_gui.run_cli', side_effect=cli), \
                    patch('inspector_gui.launch', side_effect=start), \
                    patch('inspector_gui.messagebox.showerror') as error:
                self.app.launch()
                self.pump_until_idle()
                self.assertEqual(calls, ['repair'] if failure else ['repair', 'launch'])
                self.assertEqual(error.call_count, 1 if failure else 0)

    def test_elevated_restart_preserves_configuration_and_retries_launch(self):
        self.app.admin = False
        self.app.exe.set('D:\\游戏 目录\\game.exe')
        self.app.output.set('D:\\采样 目录')
        self.app.arguments.set('--flag "a b" &')
        self.app.mode.set('纯转发（首次兼容性检查）')
        self.app.events.set('123')
        self.app.delay.set('7')
        with patch('inspector_gui.messagebox.askyesno', return_value=True), \
                patch('inspector_gui.restart_elevated', return_value=True) as restart, \
                patch.object(self.app, 'close') as close:
            self.app.error(ctypes.WinError(740))
            options = parse_options(restart.call_args.args[0])
            self.assertEqual(options['exe'], self.app.exe.get())
            self.assertEqual(options['output'], self.app.output.get())
            self.assertEqual(options['arguments'], self.app.arguments.get())
            self.assertEqual(options['mode'], 'forward')
            self.assertEqual(options['events'], '123')
            self.assertEqual(options['delay'], '7')
            self.assertTrue(options['resume_launch'])
            close.assert_called_once()

    def test_new_window_restores_options_before_scheduled_launch(self):
        self.app.close()
        self.root = tk.Tk()
        self.root.withdraw()
        options = dict(exe='D:\\test game\\game.exe', output=self.directory.name,
                       arguments='-force-d3d11', mode='forward', events='123', delay='7', resume_launch=True)
        with patch.object(InspectorApp, 'launch') as launch:
            self.app = InspectorApp(self.root, Path(self.directory.name) / 'gui.json', initial=options)
            deadline = time.monotonic() + 2
            while not launch.called and time.monotonic() < deadline:
                self.root.update()
                time.sleep(0.01)
            launch.assert_called_once()
        for name in ['exe', 'output', 'arguments', 'events', 'delay']:
            self.assertEqual(getattr(self.app, name).get(), options[name])
        self.assertEqual(self.app.mode.get(), '纯转发（首次兼容性检查）')

    def test_uac_cancellation_or_failure_keeps_original_window(self):
        self.app.admin = False
        for result in [False, ctypes.WinError(5)]:
            with patch('inspector_gui.messagebox.askyesno', return_value=True), \
                    patch('inspector_gui.restart_elevated', side_effect=result if isinstance(result, OSError) else None,
                          return_value=False), \
                    patch('inspector_gui.messagebox.showerror'), patch.object(self.app, 'close') as close:
                self.app.error(ctypes.WinError(740))
                close.assert_not_called()
                self.assertIn('保留', self.app.status.get())

    def test_elevation_does_not_loop_or_handle_unrelated_errors(self):
        for admin, attempted, code in [(True, False, 740), (False, True, 740), (False, False, 5)]:
            self.app.admin = admin
            self.app.elevation_attempted = attempted
            with patch('inspector_gui.messagebox.showerror') as error, \
                    patch('inspector_gui.messagebox.askyesno') as prompt:
                self.app.error(ctypes.WinError(code))
                prompt.assert_not_called()
                error.assert_called_once()

    def test_analyze_button_runs_real_cli_and_opens_report(self):
        session = Path(self.directory.name) / 'pid_123_1'
        session.mkdir()
        (session / 'proxy.log').touch()
        write_capture(session / 'capture_1.jsonl')
        self.app._show_sessions(support.list_sessions(Path(self.directory.name)), select=str(session))
        with patch('inspector_gui.webbrowser.open', return_value=True) as opened, \
                patch('inspector_gui.messagebox.showerror') as error:
            self.app.analyze()
            self.pump_until_idle()
            error.assert_not_called()
            opened.assert_called_once_with((session / 'capture_1.html').as_uri())
        summary = json.loads((session / 'capture_1.summary.json').read_text(encoding='utf8'))
        self.assertTrue(summary['complete'])
        self.assertEqual(summary['events'], 1)

    def test_worker_failure_restores_controls_and_settings_exclude_arguments(self):
        self.app.arguments.set('--secret do-not-save')
        def failure(log, notify):
            raise ValueError('test failure')
        with patch('inspector_gui.messagebox.showerror') as error:
            self.app.task('test', failure)
            self.pump_until_idle()
            error.assert_called_once()
        self.assertTrue(all(str(widget['state']) == 'normal' for widget in self.app.actions))
        self.assertNotIn('do-not-save', self.app.settings_path.read_text(encoding='utf8'))

    def test_capture_button_waits_for_new_complete_file_before_opening(self):
        # Simulate the producer only; request writing, polling, analysis and GUI
        # callbacks are real. Native GPU coverage remains in run_tests.py.
        stamp, _ = support.process_identity(os.getpid())
        session = Path(self.directory.name) / f'pid_{os.getpid()}_{stamp}'
        session.mkdir()
        (session / 'proxy.log').write_text('observer Context4 methods installed')
        write_capture(session / 'capture_1.jsonl')
        self.app._show_sessions(support.list_sessions(Path(self.directory.name)), select=str(session))
        self.app.events.set('1')
        self.app.delay.set('0')
        produced = threading.Event()
        def producer():
            deadline = time.monotonic() + 8
            request = session / 'capture.request'
            while time.monotonic() < deadline:
                if request.exists():
                    write_capture(session / 'capture_2.jsonl', complete=False)
                    request.unlink()
                    time.sleep(0.7)
                    write_capture(session / 'capture_2.jsonl')
                    produced.set()
                    return
                time.sleep(0.02)
        worker = threading.Thread(target=producer)
        worker.start()
        with patch('inspector_gui.webbrowser.open', return_value=True) as opened, \
                patch('inspector_gui.messagebox.showerror') as error:
            self.app.capture()
            self.pump_until_idle()
            worker.join(timeout=10)
            self.assertTrue(produced.is_set())
            error.assert_not_called()
            opened.assert_called_once_with((session / 'capture_2.html').as_uri())
        self.assertIn('采样完成', self.app.status.get())


if __name__ == '__main__':
    unittest.main()
