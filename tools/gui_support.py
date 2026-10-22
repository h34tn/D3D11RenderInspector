"""Windows desktop workflow helpers. No UI calls and no third-party dependencies."""
import ctypes
from ctypes import wintypes
import os
from pathlib import Path
import re
import subprocess
import sys
import time

from inspector import PROJECT, MissingProxyError, read_capture, read_install_manifest, sha256


def is_administrator():
    return bool(ctypes.WinDLL('shell32').IsUserAnAdmin())


class ShellExecuteInfo(ctypes.Structure):
    _fields_ = [('cbSize', wintypes.DWORD), ('fMask', wintypes.ULONG),
                ('hwnd', wintypes.HWND), ('lpVerb', wintypes.LPCWSTR),
                ('lpFile', wintypes.LPCWSTR), ('lpParameters', wintypes.LPCWSTR),
                ('lpDirectory', wintypes.LPCWSTR), ('nShow', ctypes.c_int),
                ('hInstApp', wintypes.HINSTANCE), ('lpIDList', ctypes.c_void_p),
                ('lpClass', wintypes.LPCWSTR), ('hkeyClass', wintypes.HKEY),
                ('dwHotKey', wintypes.DWORD), ('hIconOrMonitor', wintypes.HANDLE),
                ('hProcess', wintypes.HANDLE)]


def restart_elevated(arguments, owner=None):
    """Request normal Windows UAC consent; cancellation leaves the current GUI open.

    Elevate the GUI, not the game: the new GUI recreates the game's per-process
    capture environment instead of relying on environment inheritance through UAC.
    """
    executable = Path(sys.executable).with_name('pythonw.exe').resolve(strict=True)
    shell = ctypes.WinDLL('shell32', use_last_error=True)
    ole = ctypes.WinDLL('ole32', use_last_error=True)
    shell.ShellExecuteExW.argtypes = [ctypes.POINTER(ShellExecuteInfo)]
    shell.ShellExecuteExW.restype = wintypes.BOOL
    ole.CoInitializeEx.argtypes = [ctypes.c_void_p, wintypes.DWORD]
    ole.CoInitializeEx.restype = ctypes.c_long
    initialized = ole.CoInitializeEx(None, 2)
    # RPC_E_CHANGED_MODE means COM was already initialized in another apartment.
    if initialized < 0 and initialized != -2147417850:
        raise OSError(f'COM 初始化失败：0x{initialized & 0xffffffff:08x}')
    try:
        info = ShellExecuteInfo()
        info.cbSize = ctypes.sizeof(info)
        info.fMask = 0x100 | 0x400  # SEE_MASK_NOASYNC | SEE_MASK_FLAG_NO_UI
        info.hwnd = owner
        info.lpVerb = 'runas'
        info.lpFile = str(executable)
        info.lpParameters = subprocess.list2cmdline([str(PROJECT / 'inspector.pyw'), *arguments])
        info.lpDirectory = str(PROJECT)
        info.nShow = 1
        if not shell.ShellExecuteExW(ctypes.byref(info)):
            error = ctypes.get_last_error()
            if error == 1223:  # ERROR_CANCELLED
                return False
            raise ctypes.WinError(error)
        return True
    finally:
        if initialized >= 0:
            ole.CoUninitialize()


def split_arguments(text):
    """Use Windows quoting rules, without invoking a command shell."""
    if not text.strip():
        return []
    shell = ctypes.WinDLL('shell32', use_last_error=True)
    kernel = ctypes.WinDLL('kernel32', use_last_error=True)
    shell.CommandLineToArgvW.argtypes = [wintypes.LPCWSTR, ctypes.POINTER(ctypes.c_int)]
    shell.CommandLineToArgvW.restype = ctypes.POINTER(wintypes.LPWSTR)
    kernel.LocalFree.argtypes = [ctypes.c_void_p]
    kernel.LocalFree.restype = ctypes.c_void_p
    count = ctypes.c_int()
    pointer = shell.CommandLineToArgvW('game.exe ' + text, ctypes.byref(count))
    if not pointer:
        raise ctypes.WinError(ctypes.get_last_error())
    try:
        return list(pointer[:count.value])[1:]
    finally:
        kernel.LocalFree(pointer)


def process_identity(pid):
    """Return (creation FILETIME, image) for a live process, or None.

    Access denied is unknown, not proof that the process exited.
    """
    kernel = ctypes.WinDLL('kernel32', use_last_error=True)
    kernel.OpenProcess.argtypes = [wintypes.DWORD, wintypes.BOOL, wintypes.DWORD]
    kernel.OpenProcess.restype = wintypes.HANDLE
    kernel.CloseHandle.argtypes = [wintypes.HANDLE]
    kernel.GetProcessTimes.argtypes = [wintypes.HANDLE] + [ctypes.POINTER(wintypes.FILETIME)] * 4
    kernel.WaitForSingleObject.argtypes = [wintypes.HANDLE, wintypes.DWORD]
    kernel.WaitForSingleObject.restype = wintypes.DWORD
    kernel.QueryFullProcessImageNameW.argtypes = [wintypes.HANDLE, wintypes.DWORD,
                                               wintypes.LPWSTR, ctypes.POINTER(wintypes.DWORD)]
    handle = kernel.OpenProcess(0x1000 | 0x100000, False, pid)
    if not handle:
        error = ctypes.get_last_error()
        if error == 87:  # PID no longer exists.
            return None
        raise ctypes.WinError(error)
    try:
        state = kernel.WaitForSingleObject(handle, 0)
        if state == 0:
            return None
        if state != 258:
            raise ctypes.WinError(ctypes.get_last_error())
        stamps = [wintypes.FILETIME() for _ in range(4)]
        if not kernel.GetProcessTimes(handle, *(ctypes.byref(t) for t in stamps)):
            raise ctypes.WinError(ctypes.get_last_error())
        size = wintypes.DWORD(32768)
        name = ctypes.create_unicode_buffer(size.value)
        if not kernel.QueryFullProcessImageNameW(handle, 0, name, ctypes.byref(size)):
            raise ctypes.WinError(ctypes.get_last_error())
        return (stamps[0].dwHighDateTime << 32 | stamps[0].dwLowDateTime, name.value)
    finally:
        kernel.CloseHandle(handle)


def session_info(path):
    match = re.fullmatch(r'pid_(\d+)_(\d+)', path.name)
    if not match or not (path / 'proxy.log').is_file():
        raise ValueError('请选择包含 proxy.log 的进程会话目录。')
    pid, stamp = map(int, match.groups())
    alive, executable = None, ''
    try:
        identity = process_identity(pid)
        alive = identity is not None and identity[0] == stamp
        if alive:
            executable = identity[1]
    except OSError:
        pass
    with (path / 'proxy.log').open('rb') as stream:
        # Installation messages precede shader / capture logging. Keep refresh bounded.
        log = stream.read(131072).decode('utf8', errors='replace')
    observing = 'observer Context4 methods installed' in log
    if observing:
        mode = '可采样'
    elif 'observer mode=forward' in log:
        mode = '纯转发'
    elif 'observer mode=device-only' in log:
        mode = '仅 Shader'
    else:
        mode = '观察器未就绪'
    inactive_mode = '观察模式' if observing else mode
    status = mode if alive else ('已退出 · ' + inactive_mode if alive is False else '进程状态未知 · ' + inactive_mode)
    return dict(path=path, pid=pid, alive=alive, observing=observing, status=status,
                executable=executable, captures=len(list(path.glob('capture_*.jsonl'))))


def list_sessions(root):
    result = []
    for path in root.glob('pid_*'):
        try:
            result.append(session_info(path))
        except (OSError, ValueError):
            continue
    return sorted(result, key=lambda item: int(item['path'].name.rsplit('_', 1)[1]), reverse=True)


def deployment_needed(directory, binary):
    """Reuse an intact managed deployment, including an explicitly installed custom name."""
    manifest = directory / '.d3d11-inspector-install.json'
    target = directory / 'd3d11.dll'
    if not manifest.exists():
        if target.exists():
            raise ValueError('游戏目录已有 d3d11.dll，且不是本工具的受管安装；不会覆盖。')
        return True
    info, _ = read_install_manifest(directory)
    target = directory / info['filename']
    if not os.path.lexists(target):
        raise MissingProxyError(f'安装清单有效，但代理 DLL 缺失：{target}')
    if not target.is_file():
        raise ValueError(f'代理位置不是普通文件：{target}')
    actual = sha256(target)
    if actual != info.get('sha256'):
        raise ValueError('已安装 DLL 被修改；不会替换或删除。')
    if actual != sha256(binary):
        raise ValueError('已安装的是其他版本。请退出游戏，卸载代理后重新启动。')
    return False


def legacy_proxy_diagnostic(directory, pid, launched_at):
    """Recognize this project's predecessor without importing its unisolated data.

    Match a fresh log for the launched PID; an old file with a reused PID is not
    evidence about the current launch. Never trigger the legacy shared request.
    """
    log = directory / f'D3D11Proxy_{pid}.log'
    try:
        if log.stat().st_mtime < launched_at:
            return None
        with log.open('rb') as stream:
            content = stream.read(16384).decode('utf8', errors='replace')
        if f'pid={pid} ' in content and 'build=observer-v4-private-tail' in content:
            return (f'检测到旧版代理日志：{log}。旧版不使用当前工具的会话目录；'
                    '需先备份并迁移旧代理，再重启游戏。更改输出目录或刷新不会转换旧会话。')
    except OSError:
        pass
    return None


def python_command():
    # pythonw has no console handles; the worker needs captured stdout/stderr.
    executable = Path(sys.executable)
    if executable.name.lower() == 'pythonw.exe':
        executable = executable.with_name('python.exe')
    return str(executable)


def run_cli(arguments, log):
    result = subprocess.run([python_command(), str(PROJECT / 'tools/inspector.py'),
                             *map(str, arguments)], cwd=PROJECT,
                            env=dict(os.environ, PYTHONIOENCODING='utf-8'),
                            capture_output=True, encoding='utf8', errors='replace',
                            creationflags=subprocess.CREATE_NO_WINDOW)
    output = (result.stdout + result.stderr).strip()
    if output:
        log(output)
    if result.returncode:
        raise RuntimeError(output or f'工具退出码：{result.returncode}')
    return output


def latest_capture(session):
    paths = [p for p in session.glob('capture_*.jsonl')
             if re.fullmatch(r'capture_\d+\.jsonl', p.name)]
    return max(paths, key=lambda p: int(p.stem.split('_')[1]), default=None)


def require_capture_ready(session):
    info = session_info(session)
    if info['alive'] is not True:
        raise ValueError('会话进程已退出或无法核实。请选择正在运行的游戏会话。')
    if not info['observing']:
        raise ValueError('此会话无法采样。请以“调用观察”模式重启，并检查代理日志。')
    if (session / 'capture.request').exists():
        raise ValueError('此会话还有待处理请求，请回到游戏继续渲染。')
    previous = latest_capture(session)
    if previous and not read_capture(previous)[1]:
        raise ValueError('上一份采样仍未完整结束，请回到游戏继续渲染或检查日志。')


def wait_capture(session, previous, stop, notify, timeout=120):
    """Never accept an old capture, a partial JSONL, or a mere request deletion."""
    deadline = time.monotonic() + timeout
    while not stop.is_set():
        candidate = latest_capture(session)
        if candidate and candidate.name not in previous:
            events, complete, _ = read_capture(candidate)
            if complete:
                return candidate
            notify(f'采样中：已写入 {len(events)} 个调用，等待结束记录…')
        else:
            notify('请求已发送，等待游戏绘制；请切回游戏…')
        info = session_info(session)
        if info['alive'] is False:
            raise RuntimeError('游戏进程已退出，本次未取得完整采样。可用“分析已有采样”检查残留数据。')
        if time.monotonic() >= deadline:
            raise TimeoutError('等待采样完成超时。请求/数据已保留；回到游戏继续渲染后，可点击“分析已有采样”。')
        stop.wait(0.5)
    return None
