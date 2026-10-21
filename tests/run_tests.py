"""Real D3D11 rendering/readback + recording validation; no target game is modified."""
from datetime import datetime
import json
import os
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT/'tools'))
from generate_exports import pe_exports
from inspector import analyze, disassemble, read_capture, sha256


def main():
    output = ROOT/'test-results'/datetime.now().strftime('%Y%m%d_%H%M%S')
    output.mkdir(parents=True)
    proxy = ROOT/'build/d3d11.dll'
    report = {'proxy_sha256': sha256(proxy), 'output': str(output), 'tests': []}
    def run(name, args, env=None):
        result = subprocess.run(args, cwd=ROOT, env=env, capture_output=True, timeout=90)
        (output/f'{name}.log').write_bytes(result.stdout+b'\n'+result.stderr)
        report['tests'].append({'name':name, 'returncode':result.returncode})
        print(f'{name}: {"PASS" if result.returncode == 0 else "FAIL"}', flush=True)
        if result.returncode:
            print((result.stdout+result.stderr).decode(errors='replace'))
            raise RuntimeError(name)
    try:
        expected = pe_exports(Path(os.environ['SystemRoot'])/'System32/d3d11.dll')
        assert pe_exports(proxy) == expected
        report['export_count'] = len(expected)
        run('forwarding_abi', [str(ROOT/'build/abi_test.exe')])
        run('python_tools', [sys.executable, '-m', 'unittest', 'discover', '-s', 'tests', '-p', 'test_*.py'])
        for mode in ['forward','device-only','observe']:
            directory = output/mode
            env = dict(os.environ, D3D11_PROXY_OUTPUT=str(directory), D3D11_PROXY_MODE=mode)
            run(mode, [str(ROOT/'build/render_smoke.exe'), str(proxy)], env)
            sessions = list(directory.glob('pid_*'))
            assert len(sessions) == 1, sessions
            session = sessions[0]
            shaders = list(session.glob('ps_*.dxbc'))
            captures = sorted(session.glob('capture_*.jsonl'))
            assert len(shaders) == (0 if mode == 'forward' else 6), (mode,len(shaders))
            assert len(captures) == (6 if mode == 'observe' else 0), (mode,len(captures))
            if mode == 'observe':
                for path in captures:
                    events, complete, errors = read_capture(path)
                    assert complete and not errors and len(events) == 1
                    event = events[0]
                    assert event['call'] == 'Draw' and event['context_type'] == 0
                    assert event['outputs'][0]['resource']['width'] == 4
                    assert event['outputs'][0]['resource']['height'] == 4
                    assert (session/event['shader_file']).is_file()
                analyze(session)
                disassemble(session)
                report['observe_session'] = str(session)
        compute_root = output/'compute'
        run('compute', [str(ROOT/'build/compute_smoke.exe'), str(proxy)],
            dict(os.environ, D3D11_PROXY_OUTPUT=str(compute_root), D3D11_PROXY_MODE='observe'))
        compute_session = next(compute_root.glob('pid_*'))
        events, complete, errors = read_capture(compute_session/'capture_1.jsonl')
        assert complete and not errors and len(events) == 1
        assert events[0]['call'] == 'Dispatch'
        assert events[0]['outputs'][0]['resource']['width'] == 4
        assert events[0]['shader_file'].startswith('cs_')
        assert (compute_session/events[0]['shader_file']).is_file()
        analyze(compute_session)
        disassemble(compute_session)
        report['compute_session'] = str(compute_session)
        report['passed'] = True
    except Exception as error:
        report['passed'] = False
        report['error'] = str(error)
        raise
    finally:
        (output/'result.json').write_text(json.dumps(report, indent=2), encoding='utf8')
        (ROOT/'test-results/latest.txt').write_text(str(output), encoding='utf8')
        print(f'Evidence: {output}', flush=True)


if __name__ == '__main__': main()
