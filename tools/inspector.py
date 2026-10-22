"""D3D11 Render Inspector: local deployment, bounded capture and analysis."""
import argparse
from collections import Counter
import ctypes
import hashlib
import html
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

from generate_exports import pe_exports

PROJECT = Path(__file__).resolve().parents[1]


class MissingProxyError(ValueError):
    """A valid managed installation has lost its proxy DLL."""


def read_install_manifest(directory):
    manifest = directory / '.d3d11-inspector-install.json'
    try:
        raw = manifest.read_bytes()
        info = json.loads(raw.decode('utf8'))
    except (UnicodeError, json.JSONDecodeError) as error:
        raise ValueError(f'安装清单无法解析：{manifest}') from error
    if not isinstance(info, dict) or type(info.get('version')) is not int or info['version'] != 1:
        raise ValueError(f'不支持的安装清单格式：{manifest}')
    name, digest = info.get('filename'), info.get('sha256')
    if (not isinstance(name, str) or name != Path(name).name or
            any(c in name for c in '/\\:') or not name.lower().endswith('.dll')):
        raise ValueError(f'安装清单中的 DLL 文件名无效：{manifest}')
    if (not isinstance(digest, str) or len(digest) != 64 or
            any(c not in '0123456789abcdef' for c in digest) or not isinstance(info.get('source'), str)):
        raise ValueError(f'安装清单缺少有效的哈希或来源：{manifest}')
    return info, raw


def repair(directory, binary):
    """Restore a missing managed DLL; preserve the old manifest and never replace a DLL."""
    directory = directory.resolve(strict=True)
    system = Path(os.environ['SystemRoot']).resolve()
    if directory == system or system in directory.parents:
        raise ValueError('Do not deploy into Windows directories')
    manifest = directory / '.d3d11-inspector-install.json'
    info, original = read_install_manifest(directory)
    target = directory / info['filename']
    if os.path.lexists(target):
        raise ValueError(f'目标 DLL 已存在，拒绝覆盖或替换：{target}')
    # Snapshot the source before validation so a concurrent build cannot change
    # the bytes between export verification, copying, and manifest hashing.
    payload = binary.read_bytes()
    digest = hashlib.sha256(payload).hexdigest()
    fd, temporary = tempfile.mkstemp(prefix='inspector-repair-', suffix='.tmp', dir=directory)
    created = False
    try:
        with os.fdopen(fd, 'wb') as out:
            out.write(payload)
        if pe_exports(Path(temporary)) != pe_exports(system / 'System32/d3d11.dll'):
            raise ValueError('Build exports do not match this Windows runtime; rebuild locally')
        fd, backup = tempfile.mkstemp(prefix=manifest.name + '.backup-', dir=directory)
        with os.fdopen(fd, 'wb') as out:
            out.write(original)
        if manifest.read_bytes() != original:
            raise ValueError('安装清单在修复期间发生变化，已停止；不会覆盖其他修改。')
        with target.open('xb') as out:
            created = True
            out.write(payload)
        write_json(temporary, dict(version=1, filename=info['filename'], sha256=digest,
                                   source=str(binary.resolve())))
        if manifest.read_bytes() != original or sha256(target) != digest:
            raise ValueError('安装文件在修复期间发生变化，已停止。')
        os.replace(temporary, manifest)
    except Exception:
        # Only undo our new copy, never an existing or subsequently modified DLL.
        if created and target.is_file() and sha256(target) == digest:
            target.unlink()
        raise
    finally:
        if os.path.exists(temporary):
            os.unlink(temporary)
    print(f'Restored missing proxy: {target}')
    print(f'Previous manifest preserved: {backup}')
    return Path(backup)


def default_output():
    return Path(os.environ.get('D3D11_PROXY_OUTPUT') or
                str(Path(os.environ.get('LOCALAPPDATA', tempfile.gettempdir())) / 'D3D11RenderInspector/captures'))


def sha256(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def write_json(path, value):
    Path(path).write_text(json.dumps(value, ensure_ascii=False, indent=2), encoding='utf8')


def capture(session, count):
    session = session.resolve(strict=True)
    if not session.is_dir() or not (session/'proxy.log').is_file():
        raise ValueError('Choose a session directory containing proxy.log')
    request = session/'capture.request'
    if request.exists(): raise ValueError('A capture request is already pending')
    if not 1 <= count <= 10000: raise ValueError('Event count must be 1..10000')
    fd, temporary = tempfile.mkstemp(prefix='request-', suffix='.tmp', dir=session)
    try:
        with os.fdopen(fd, 'w', encoding='ascii') as file: file.write(str(count))
        # Windows rename refuses to overwrite an existing request.
        os.rename(temporary, request)
    finally:
        if os.path.exists(temporary): os.unlink(temporary)
    print(f'Requested {count} events: {request}')
    print('Consumption starts on the next observed draw/dispatch; an end record confirms completion.')


def read_capture(path):
    rows, errors = [], []
    for number, line in enumerate(path.read_text(encoding='utf8', errors='replace').splitlines(), 1):
        if not line.strip(): continue
        try: rows.append(json.loads(line))
        except json.JSONDecodeError as error: errors.append(f'line {number}: {error.msg}')
    events = [row for row in rows if 'event' in row]
    complete = bool(rows and rows[0].get('type') == 'header' and rows[-1].get('type') == 'end'
                    and rows[-1].get('events') == len(events) and not errors
                    and [e['event'] for e in events] == list(range(1, len(events)+1)))
    return events, complete, errors


def views(items):
    result = []
    for item in items:
        resource = item.get('resource') or {}
        size = f"{resource['width']}×{resource['height']}" if 'width' in resource else f"{resource.get('bytes','?')} B"
        result.append(f"s{item['slot']}: {resource.get('id','?')} · {size} · fmt {item.get('view_format',resource.get('format','?'))}")
    return '; '.join(result)


def analyze(session):
    session = session.resolve(strict=True)
    summaries = []
    shader_index = {p.name: {'sha256': sha256(p), 'bytes': p.stat().st_size} for p in session.glob('*.dxbc')}
    for path in sorted(session.glob('capture_*.jsonl')):
        events, complete, errors = read_capture(path)
        summary = {'file': path.name, 'complete': complete, 'events': len(events), 'parse_errors': errors,
                   'calls': dict(Counter(e['call'] for e in events)),
                   'shader_names': dict(Counter(e.get('shader_name') or e.get('shader_file','unknown') for e in events)),
                   'context_types': dict(Counter(str(e.get('context_type')) for e in events))}
        summaries.append(summary)
        write_json(path.with_suffix('.summary.json'), summary)
        lines = ['event\tcall\tshader_file\tshader_name\tinputs\toutputs']
        table = []
        for e in events:
            columns = [str(e['event']), e['call'], e.get('shader_file',''), e.get('shader_name',''), views(e.get('inputs',[])), views(e.get('outputs',[]))]
            lines.append('\t'.join(s.replace('\t',' ').replace('\n',' ') for s in columns))
            table.append('<tr>'+''.join('<td>'+html.escape(s)+'</td>' for s in columns)+'</tr>')
        path.with_suffix('.topology.tsv').write_text('\n'.join(lines), encoding='utf8')
        title = html.escape(path.name)
        report = f'''<!doctype html><html lang="zh-CN"><meta charset="utf-8"><title>{title}</title>
<style>body{{font:14px/1.6 system-ui;background:#0d1421;color:#d9e5f6;margin:32px}}h1{{color:#81d4fc}}.note{{padding:16px;background:#18243a;border-left:4px solid #81d4fc}}table{{border-collapse:collapse;width:100%;margin-top:24px}}th,td{{text-align:left;padding:10px;border-bottom:1px solid #29354b;vertical-align:top;overflow-wrap:anywhere}}th{{background:#18243a;position:sticky;top:0}}tr:hover{{background:#17233a}}td:first-child{{color:#81d4fc}}</style>
<h1>D3D11 调用与资源绑定 · {title}</h1><div class="note">完整：{complete} · 调用数：{len(events)}<br>
这是按调用顺序记录的立即上下文状态快照，包含 Draw / Dispatch 及绑定关系。没有 Present 帧边界、GPU 耗时、纹理像素或常量值；不可据此直接判断一帧开销或重放。资源地址仅作本次采样中的关联线索。</div>
<table><thead><tr><th>事件</th><th>调用</th><th>字节码</th><th>调试名称</th><th>输入资源</th><th>输出资源</th></tr></thead><tbody>{''.join(table)}</tbody></table></html>'''
        path.with_suffix('.html').write_text(report, encoding='utf8')
    write_json(session/'analysis.json', {'captures': summaries, 'shaders': shader_index})
    print(json.dumps({'session': str(session), 'captures': summaries, 'shader_count': len(shader_index)}, ensure_ascii=False, indent=2))
    return summaries


def disassemble(session):
    if os.name != 'nt': raise ValueError('DXBC disassembly requires Windows')
    library = ctypes.WinDLL(str(Path(os.environ['SystemRoot'])/'System32/d3dcompiler_47.dll'))
    function = library.D3DDisassemble
    function.argtypes = [ctypes.c_void_p, ctypes.c_size_t, ctypes.c_uint, ctypes.c_char_p, ctypes.POINTER(ctypes.c_void_p)]
    function.restype = ctypes.c_long
    output = session/'disassembly'
    output.mkdir(exist_ok=True)
    count = 0
    for path in sorted(session.glob('*.dxbc')):
        data = path.read_bytes()
        buffer = ctypes.create_string_buffer(data)
        blob = ctypes.c_void_p()
        result = function(buffer, len(data), 0, None, ctypes.byref(blob))
        if result < 0: raise ValueError(f'{path.name}: D3DDisassemble failed 0x{result & 0xffffffff:08x}')
        table = ctypes.cast(blob, ctypes.POINTER(ctypes.POINTER(ctypes.c_void_p))).contents
        try:
            pointer = ctypes.WINFUNCTYPE(ctypes.c_void_p, ctypes.c_void_p)(table[3])(blob)
            size = ctypes.WINFUNCTYPE(ctypes.c_size_t, ctypes.c_void_p)(table[4])(blob)
            text = ctypes.string_at(pointer, size).rstrip(b'\0').decode('utf8', errors='replace')
            (output/path.with_suffix('.txt').name).write_text(text, encoding='utf8')
            count += 1
        finally:
            ctypes.WINFUNCTYPE(ctypes.c_ulong, ctypes.c_void_p)(table[2])(blob)
    print(f'Disassembled {count} shaders to {output}')


def install(directory, binary, name):
    directory = directory.resolve(strict=True)
    if not directory.is_dir(): raise ValueError('Target must be a directory')
    if name != Path(name).name or not name.lower().endswith('.dll') or any(c in name for c in '/\\:'):
        raise ValueError('DLL name must be a plain filename ending in .dll')
    system = Path(os.environ['SystemRoot']).resolve()
    if directory == system or system in directory.parents: raise ValueError('Do not deploy into Windows directories')
    expected = pe_exports(system/'System32/d3d11.dll')
    if pe_exports(binary) != expected: raise ValueError('Build exports do not match this Windows runtime; rebuild locally')
    manifest = directory/'.d3d11-inspector-install.json'
    target = directory/name
    if manifest.exists() or target.exists(): raise ValueError('Existing deployment / DLL found; refusing to overwrite')
    created = False
    try:
        with target.open('xb') as out:
            created = True
            with binary.open('rb') as source: shutil.copyfileobj(source, out)
        with manifest.open('x', encoding='utf8') as out:
            json.dump({'version':1, 'filename':name, 'sha256':sha256(target), 'source':str(binary.resolve())}, out, indent=2)
    except Exception:
        # Only remove a file created by this invocation and matching the source.
        if created and target.is_file(): target.unlink()
        raise
    print(f'Installed {target}; no executable or engine binary was patched.')


def uninstall(directory):
    directory = directory.resolve(strict=True)
    manifest = directory/'.d3d11-inspector-install.json'
    info = json.loads(manifest.read_text(encoding='utf8'))
    name = info['filename']
    if name != Path(name).name or any(c in name for c in '/\\:'): raise ValueError('Invalid manifest filename')
    target = directory/name
    if not target.is_file() or sha256(target) != info['sha256']:
        raise ValueError('Installed DLL missing or changed; refusing removal')
    target.unlink()
    manifest.unlink()
    print(f'Removed verified proxy: {target}')


def launch(executable, output, mode, arguments, detach_output=False):
    executable = executable.resolve(strict=True)
    pe_exports(executable)  # Also verifies x64; exports are not required for an EXE.
    env = dict(os.environ, D3D11_PROXY_OUTPUT=str(output.resolve()), D3D11_PROXY_MODE=mode)
    streams = dict(stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL,
                   stderr=subprocess.DEVNULL) if detach_output else {}
    return subprocess.Popen([str(executable), *arguments], cwd=executable.parent, env=env, **streams)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest='command', required=True)
    sessions = commands.add_parser('sessions', help='List local capture sessions')
    sessions.add_argument('--output', type=Path, default=default_output())
    request = commands.add_parser('capture', help='Request 1..10000 events from one session')
    request.add_argument('session', type=Path)
    request.add_argument('--events', type=int, default=2000)
    for name in ['analyze', 'disassemble']:
        sub = commands.add_parser(name)
        sub.add_argument('session', type=Path)
    deploy = commands.add_parser('install', help='Copy built proxy without replacing an existing DLL')
    deploy.add_argument('directory', type=Path)
    deploy.add_argument('--binary', type=Path, default=PROJECT/'build/d3d11.dll')
    deploy.add_argument('--name', default='d3d11.dll')
    remove = commands.add_parser('uninstall', help='Remove only the installed hash-matching DLL')
    remove.add_argument('directory', type=Path)
    restore = commands.add_parser('repair', help='Restore a missing managed DLL, preserving the old manifest')
    restore.add_argument('directory', type=Path)
    restore.add_argument('--binary', type=Path, default=PROJECT/'build/d3d11.dll')
    run = commands.add_parser('run', help='Launch target with per-process inspector environment')
    run.add_argument('--exe', type=Path, required=True)
    run.add_argument('--output', type=Path, default=default_output())
    run.add_argument('--mode', choices=['observe','forward','device-only'], default='observe')
    run.add_argument('arguments', nargs=argparse.REMAINDER)
    args = parser.parse_args()
    if args.command == 'sessions':
        for path in sorted(args.output.glob('pid_*'), key=lambda p:p.stat().st_mtime, reverse=True):
            print(f'{path}  captures={len(list(path.glob("capture_*.jsonl")))}')
    elif args.command == 'capture': capture(args.session, args.events)
    elif args.command == 'analyze': analyze(args.session)
    elif args.command == 'disassemble': disassemble(args.session.resolve(strict=True))
    elif args.command == 'install': install(args.directory, args.binary, args.name)
    elif args.command == 'uninstall': uninstall(args.directory)
    elif args.command == 'repair': repair(args.directory, args.binary)
    elif args.command == 'run':
        arguments = args.arguments[1:] if args.arguments[:1] == ['--'] else args.arguments
        process = launch(args.exe, args.output, args.mode, arguments)
        print(f'Launched PID {process.pid}. DLL loading is target-dependent; check sessions and proxy.log.')


if __name__ == '__main__':
    try: main()
    except (ValueError, OSError, KeyError) as error:
        print(f'ERROR: {error}', file=sys.stderr)
        raise SystemExit(1)
