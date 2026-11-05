"""Real pixel/compute readback through explicit System32 loads and startup hooks."""
from datetime import datetime
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
from inspector import read_capture, sha256, analyze, disassemble
from state_args_checks import validate_state_args
from readback_checks import validate_readback


def main():
    output = ROOT / 'test-results' / ('startup_' + datetime.now().strftime('%Y%m%d_%H%M%S'))
    output.mkdir(parents=True)
    report = dict(hook_sha256=sha256(ROOT / 'build/inspector_hook.dll'),
                  launcher_sha256=sha256(ROOT / 'build/startup_launcher.exe'), tests=[], passed=False)
    # The tested app and its argument both use ordinary Windows quoting; the app
    # loads System32/d3d11.dll, so the directory proxy cannot satisfy this test.
    space = output / 'path with spaces'
    space.mkdir()
    render = space / 'render smoke.exe'
    shutil.copyfile(ROOT / 'build/render_smoke.exe', render)
    system = str(Path(os.environ['SystemRoot']) / 'System32/d3d11.dll')
    try:
        rejected = subprocess.run([str(ROOT / 'build/startup_launcher.exe'), str(ROOT / 'build/d3d11.dll'),
                                   str(render), system], capture_output=True, timeout=10)
        report['tests'].append(dict(name='reject_non_hook_dll', returncode=rejected.returncode,
                                    expected_failure=True))
        assert rejected.returncode == 3, rejected
        for name, executable, mode, count in [
                ('forward', render, 'forward', 0),
                ('device-only', render, 'device-only', 0),
                ('observe', render, 'observe', 6),
                ('compute', ROOT / 'build/compute_smoke.exe', 'observe', 1)]:
            result = subprocess.run([sys.executable, str(ROOT / 'tools/inspector.py'), 'run-startup',
                                     '--exe', str(executable), '--output', str(output / name), '--mode', mode,
                                     '--wait', '--', system], capture_output=True, timeout=110)
            (output / (name + '.log')).write_bytes(result.stdout + result.stderr)
            report['tests'].append(dict(name=name, returncode=result.returncode))
            assert result.returncode == 0, (name, result.stdout, result.stderr)
            sessions = list((output / name).glob('pid_*'))
            assert len(sessions) == 1, (name, sessions)
            session = sessions[0]
            assert 'startup hooks result=0' in (session / 'proxy.log').read_text()
            captures = list(session.glob('capture_*.jsonl'))
            assert len(captures) == count, (name, len(captures))
            shaders = list(session.glob('*.dxbc'))
            # render_smoke creates one VS and one PS per device variant; compute_smoke only a CS.
            assert len(shaders) == (0 if mode == 'forward' else 1 if name == 'compute' else 12), (name, shaders)
            for capture in captures:
                events, complete, errors = read_capture(capture)
                assert complete and not errors and len(events) == 1
                assert events[0]['call'] == ('Dispatch' if name == 'compute' else 'Draw')
                assert events[0]['outputs'][0]['resource']['width'] == 4
                if name == 'compute':
                    assert events[0]['args'] == {'x': 1, 'y': 1, 'z': 1}, events[0]['args']
                else:
                    assert events[0]['args'] == {'vertex_count': 3, 'start_vertex': 0}, events[0]['args']
                    assert events[0]['depth_state']['depth_func'] == 4, events[0]['depth_state']
                    assert events[0]['depth_target']['resource']['width'] == 4
                assert (session / events[0]['shader_file']).is_file()
            if count:
                analyze(session)
                disassemble(session)
            print(name, 'PASS', flush=True)
        report['state_args_sessions'] = {}
        for driver in ['hardware', 'warp']:
            state_root = output / ('state_args_' + driver)
            result = subprocess.run([sys.executable, str(ROOT / 'tools/inspector.py'), 'run-startup',
                                     '--exe', str(ROOT / 'build/state_args_smoke.exe'), '--output', str(state_root),
                                     '--mode', 'observe', '--wait', '--', system, driver],
                                    capture_output=True, timeout=110)
            (output / ('state_args_' + driver + '.log')).write_bytes(result.stdout + result.stderr)
            report['tests'].append(dict(name='state_args_' + driver, returncode=result.returncode))
            assert result.returncode == 0, (driver, result.stdout, result.stderr)
            sessions = list(state_root.glob('pid_*'))
            assert len(sessions) == 1, sessions
            validate_state_args(sessions[0])
            report['state_args_sessions'][driver] = str(sessions[0])
            print('state_args_' + driver, 'PASS', flush=True)
        report['readback_sessions'] = {}
        for driver in ['hardware', 'warp']:
            readback_root = output / ('readback_' + driver)
            result = subprocess.run([sys.executable, str(ROOT / 'tools/inspector.py'), 'run-startup',
                                     '--exe', str(ROOT / 'build/readback_smoke.exe'), '--output', str(readback_root),
                                     '--mode', 'observe', '--wait', '--', system, driver],
                                    capture_output=True, timeout=110)
            (output / ('readback_' + driver + '.log')).write_bytes(result.stdout + result.stderr)
            report['tests'].append(dict(name='readback_' + driver, returncode=result.returncode))
            assert result.returncode == 0, (driver, result.stdout, result.stderr)
            sessions = list(readback_root.glob('pid_*'))
            assert len(sessions) == 1, sessions
            validate_readback(sessions[0])
            report['readback_sessions'][driver] = str(sessions[0])
            print('readback_' + driver, 'PASS', flush=True)
        report['passed'] = True
    finally:
        (output / 'result.json').write_text(json.dumps(report, indent=2), encoding='utf8')
        (ROOT / 'test-results/startup-latest.txt').write_text(str(output), encoding='utf8')
        print('Evidence:', output)


if __name__ == '__main__':
    main()
