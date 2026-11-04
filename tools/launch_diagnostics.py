"""Read-only, bounded diagnostics for a managed proxy deployment and launch."""
import csv
import ctypes
from ctypes import wintypes
import os
from pathlib import Path

from inspector import read_install_manifest, sha256


def canonical(path):
    return os.path.normcase(str(Path(path).resolve()))


def loaded_modules(pid):
    """Toolhelp returns paths without reading target process memory or injecting code."""
    class Module(ctypes.Structure):
        _fields_ = [('dwSize', wintypes.DWORD), ('th32ModuleID', wintypes.DWORD),
                    ('th32ProcessID', wintypes.DWORD), ('GlblcntUsage', wintypes.DWORD),
                    ('ProccntUsage', wintypes.DWORD), ('modBaseAddr', ctypes.c_void_p),
                    ('modBaseSize', wintypes.DWORD), ('hModule', wintypes.HMODULE),
                    ('szModule', wintypes.WCHAR * 256), ('szExePath', wintypes.WCHAR * 260)]
    kernel = ctypes.WinDLL('kernel32', use_last_error=True)
    kernel.CreateToolhelp32Snapshot.argtypes = [wintypes.DWORD, wintypes.DWORD]
    kernel.CreateToolhelp32Snapshot.restype = wintypes.HANDLE
    kernel.Module32FirstW.argtypes = kernel.Module32NextW.argtypes = [wintypes.HANDLE, ctypes.POINTER(Module)]
    kernel.Module32FirstW.restype = kernel.Module32NextW.restype = wintypes.BOOL
    kernel.CloseHandle.argtypes = [wintypes.HANDLE]
    for _ in range(3):
        handle = kernel.CreateToolhelp32Snapshot(0x8 | 0x10, pid)
        if handle != ctypes.c_void_p(-1).value:
            break
        if ctypes.get_last_error() != 24:  # ERROR_BAD_LENGTH: module list changed.
            raise ctypes.WinError(ctypes.get_last_error())
    else:
        raise ctypes.WinError(ctypes.get_last_error())
    try:
        module = Module()
        module.dwSize = ctypes.sizeof(module)
        result = []
        ok = kernel.Module32FirstW(handle, ctypes.byref(module))
        if not ok:
            raise ctypes.WinError(ctypes.get_last_error())
        while ok:
            result.append(module.szExePath)
            if len(result) > 4096:
                raise ValueError('Module enumeration exceeded 4096 entries')
            ok = kernel.Module32NextW(handle, ctypes.byref(module))
        if ctypes.get_last_error() != 18:  # ERROR_NO_MORE_FILES
            raise ctypes.WinError(ctypes.get_last_error())
        return result
    finally:
        kernel.CloseHandle(handle)


def trace_deletions(trace, target):
    """Only successful delete disposition events prove a requesting process.

    This attributes the process, not a particular DLL or protection subsystem.
    Limit input size/event count; don't import unrelated process activity.
    """
    if trace.stat().st_size > 1024 * 1024 * 1024:
        raise ValueError('Trace CSV exceeds 1 GiB; filter it in Process Monitor first')
    events = []
    expected = canonical(target)
    with trace.open(encoding='utf-8-sig', newline='') as stream:
        reader = csv.DictReader(stream)
        required = {'Path', 'Operation', 'Result', 'Detail', 'PID', 'Process Name', 'Time of Day'}
        if not required.issubset(reader.fieldnames or []):
            raise ValueError('Expected a Process Monitor CSV with English column names')
        for row in reader:
            if (row['Result'] == 'SUCCESS'
                    and row['Operation'] in ('SetDispositionInformationFile', 'SetDispositionInformationEx')
                    and canonical(row['Path']) == expected
                    and ('Delete: True' in row['Detail'] or 'FILE_DISPOSITION_DELETE' in row['Detail'])):
                events.append({key: row[key] for key in required})
                if len(events) >= 64:
                    break
    return events


def diagnose(executable, output, pid=None, trace=None):
    # Import lazily: gui_support retains the established PID/creation-time check.
    from gui_support import process_identity
    executable = executable.resolve(strict=True)
    result = dict(executable=str(executable), pid=pid, status='not_loaded', sessions=[])
    try:
        info, _ = read_install_manifest(executable.parent)
    except (OSError, ValueError) as error:
        result.update(status='deployment_unverified', deployment_error=str(error),
                      message='安装清单缺失或无效，无法核实代理名称和来源；保留现有文件，先核对部署目录。')
        return result
    target = executable.parent / info['filename']
    result['proxy'] = str(target)
    result['expected_sha256'] = info['sha256']
    result['deployment'] = 'missing'
    if target.is_file():
        result['actual_sha256'] = sha256(target)
        result['deployment'] = 'intact' if result['actual_sha256'] == info['sha256'] else 'changed'
    if trace:
        result['delete_events'] = trace_deletions(trace, target)
    identity = None
    if pid is not None:
        try:
            identity = process_identity(pid)
            if identity is None:
                result['process'] = 'exited'
            elif canonical(identity[1]) != canonical(executable):
                result['process'] = 'different_executable'
            else:
                result['process'] = 'running'
                result['creation_time'] = identity[0]
                modules = loaded_modules(pid)
                # Recheck identity after enumeration to avoid associating a reused PID.
                if process_identity(pid) != identity:
                    result['process'] = 'changed_during_check'
                else:
                    result['proxy_loaded'] = any(canonical(p) == canonical(target) for p in modules)
                    result['graphics_modules'] = [p for p in modules if Path(p).name.lower() in
                                                  ('d3d11.dll', 'd3d12.dll', 'dxgi.dll', 'vulkan-1.dll', 'opengl32.dll')]
        except OSError as error:
            result['process_error'] = str(error)
    if identity and result.get('process') == 'running':
        session = output / f'pid_{pid}_{identity[0]}'
        log = session / 'proxy.log'
        if log.is_file():
            with log.open('rb') as stream:
                content = stream.read(131072).decode('utf8', errors='replace')
            result['sessions'] = [str(session)]
            if 'observer Context4 methods installed' in content:
                result['status'] = 'observing'
            elif 'observer mode=forward' in content:
                result['status'] = 'forwarding'
            elif 'observer mode=device-only' in content:
                result['status'] = 'shader_only'
            else:
                result['status'] = 'observer_not_ready'
        elif result.get('proxy_loaded'):
            result['status'] = 'loaded_without_session'
    if result['status'] == 'not_loaded':
        if result['deployment'] != 'intact':
            result['status'] = 'proxy_' + result['deployment']
        elif result.get('process_error') or result.get('process') != 'running':
            result['status'] = 'process_unverified'
    messages = {
        'proxy_missing': '代理文件缺失，未建立会话；停止重试补写。可用文件访问追踪确定移除者，不能仅凭文件缺失归因。',
        'proxy_changed': '代理文件哈希变化；保留现有文件，停止复用或替换。',
        'not_loaded': '代理存在但目标进程未加载；核对实际渲染进程和加载入口。系统 D3D11 路径本身不能证明使用了绝对路径加载。',
        'process_unverified': '无法核实目标进程；检查 PID、进程路径和读取权限，不将访问拒绝视为进程退出。',
        'loaded_without_session': '代理已加载但未生成会话；检查导出是否被调用、输出目录权限及环境变量继承。',
        'observer_not_ready': '已有代理会话，但观察器未就绪；检查 proxy.log 中的跳过或失败信息。',
        'observing': '观察器已就绪；采样完成仍须核对结束记录。',
        'forwarding': '纯转发接入成功；退出目标后使用 observe 模式重新启动以采样。',
        'shader_only': '仅 Shader 模式接入成功；当前不支持调用采样。',
    }
    result['message'] = messages[result['status']]
    return result
