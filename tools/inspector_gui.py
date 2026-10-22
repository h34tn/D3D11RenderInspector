"""Chinese Tk desktop frontend for the existing inspector CLI."""
import argparse
import json
import os
from pathlib import Path
import queue
import subprocess
import threading
import time
import tkinter as tk
from tkinter import filedialog, messagebox, ttk
import webbrowser

from generate_exports import pe_exports
from inspector import PROJECT, MissingProxyError, default_output, launch, read_capture
from gui_support import (deployment_needed, latest_capture, list_sessions, require_capture_ready,
                         run_cli, split_arguments, wait_capture, is_administrator, restart_elevated,
                         legacy_proxy_diagnostic)

MODES = {'调用观察（采样 + Shader）': 'observe', '纯转发（首次兼容性检查）': 'forward',
         '仅导出 Shader': 'device-only'}
SETTINGS = Path(os.environ.get('LOCALAPPDATA', str(Path.home()))) / 'D3D11RenderInspector/gui.json'


class InspectorApp:
    def __init__(self, root, settings_path=SETTINGS, initial=None):
        self.root = root
        self.settings_path = settings_path
        self.messages = queue.Queue()
        self.stop = threading.Event()
        self.busy = False
        self.refreshing = False
        self.closed = False
        self.sessions = {}
        self.processes = []
        self.launch_watch = None
        self.actions = []
        self.inputs = []
        self.admin = is_administrator()
        self.elevation_attempted = bool(initial and initial.get('resume_launch'))
        self.exe = tk.StringVar()
        self.output = tk.StringVar(value=str(default_output()))
        self.arguments = tk.StringVar()
        self.mode = tk.StringVar(value=next(iter(MODES)))
        self.events = tk.StringVar(value='2000')
        self.delay = tk.StringVar(value='5')
        self.status = tk.StringVar(value='选择游戏 EXE，然后部署并启动。首次接入可先选择“纯转发”。')
        self.selection_text = tk.StringVar(value='暂无会话。游戏启动后会自动刷新；也可打开历史会话。')
        self._load_settings()
        if initial:
            for name in ['exe', 'output', 'arguments', 'events', 'delay']:
                if initial.get(name) is not None:
                    getattr(self, name).set(initial[name])
            if initial.get('mode'):
                self.mode.set(next(label for label, code in MODES.items() if code == initial['mode']))
        self._build()
        self.root.protocol('WM_DELETE_WINDOW', self.close)
        self.poll_timer = self.root.after(100, self._poll)
        self.refresh_timer = self.root.after(200, self.refresh)
        if initial and initial.get('resume_launch'):
            self.launch_timer = self.root.after(300, lambda: self.guard(self.launch))

    def _build(self):
        self.root.title('D3D11 Render Inspector' + (' · 管理员' if self.admin else ''))
        self.root.geometry('1080x820')
        self.root.minsize(920, 640)
        style = ttk.Style()
        style.theme_use('clam')
        style.configure('.', font=('Microsoft YaHei UI', 10))
        style.configure('TFrame', background='#f3f6fa')
        style.configure('TLabel', background='#f3f6fa')
        style.configure('TLabelframe', background='#f3f6fa')
        style.configure('TLabelframe.Label', background='#f3f6fa', foreground='#1f3d63')
        style.configure('TButton', padding=(12, 7))
        style.configure('Primary.TButton', background='#245bb2', foreground='white')
        style.map('Primary.TButton', background=[('disabled', '#a1aec2'), ('active', '#17488f')])
        style.configure('Treeview', rowheight=30)
        # Preserve usable controls on small / high-DPI displays instead of squeezing
        # the session table to zero height. The whole page can scroll when needed.
        shell = ttk.Frame(self.root)
        shell.pack(fill='both', expand=True)
        shell.rowconfigure(0, weight=1)
        shell.columnconfigure(0, weight=1)
        canvas = tk.Canvas(shell, highlightthickness=0, background='#f3f6fa')
        canvas.grid(row=0, column=0, sticky='nsew')
        vertical = ttk.Scrollbar(shell, orient='vertical', command=canvas.yview)
        vertical.grid(row=0, column=1, sticky='ns')
        horizontal = ttk.Scrollbar(shell, orient='horizontal', command=canvas.xview)
        horizontal.grid(row=1, column=0, sticky='ew')
        canvas.configure(yscrollcommand=vertical.set, xscrollcommand=horizontal.set)
        frame = ttk.Frame(canvas, padding=22)
        page = canvas.create_window(0, 0, anchor='nw', window=frame)
        def layout(_event=None):
            canvas.itemconfigure(page, width=max(frame.winfo_reqwidth(), canvas.winfo_width()),
                                 height=max(frame.winfo_reqheight(), canvas.winfo_height()))
            canvas.configure(scrollregion=canvas.bbox('all'))
        canvas.bind('<Configure>', layout)
        frame.bind('<Configure>', layout)
        def wheel(event):
            if not isinstance(event.widget, (tk.Text, ttk.Treeview, ttk.Spinbox, ttk.Combobox)):
                canvas.yview_scroll(-int(event.delta / 120), 'units')
        self.root.bind('<MouseWheel>', wheel)
        frame.columnconfigure(0, weight=1)
        frame.rowconfigure(2, weight=2)
        frame.rowconfigure(5, weight=1)
        header = ttk.Frame(frame)
        header.grid(row=0, column=0, sticky='ew', pady=(0, 14))
        ttk.Label(header, text='D3D11 Render Inspector', font=('Segoe UI', 22, 'bold')).pack(anchor='w')
        ttk.Label(header, text='选择游戏 → 启动观察 → 回到场景采样 → 查看报告', foreground='#52647b').pack(anchor='w')

        config = ttk.LabelFrame(frame, text=' 1  游戏与启动 ', padding=12)
        config.grid(row=1, column=0, sticky='ew', pady=(0, 12))
        config.columnconfigure(1, weight=1)
        for row, label, variable, callback in [(0, '游戏 EXE', self.exe, self.choose_exe),
                                                (1, '输出目录', self.output, self.choose_output)]:
            ttk.Label(config, text=label).grid(row=row, column=0, sticky='w', padx=(0, 12))
            entry = ttk.Entry(config, textvariable=variable)
            entry.grid(row=row, column=1, sticky='ew', pady=4)
            self.inputs.append(entry)
            self.button(config, '浏览…', callback).grid(row=row, column=2, padx=(8, 0))
        ttk.Label(config, text='启动参数').grid(row=2, column=0, sticky='w')
        args = ttk.Entry(config, textvariable=self.arguments)
        args.grid(row=2, column=1, sticky='ew', pady=4)
        self.inputs.append(args)
        self.button(config, 'Unity DX11', self.unity_arguments).grid(row=2, column=2, padx=(8, 0))
        row = ttk.Frame(config)
        row.grid(row=3, column=0, columnspan=3, sticky='ew', pady=(8, 0))
        combo = ttk.Combobox(row, textvariable=self.mode, values=list(MODES), state='readonly', width=29)
        combo.pack(side='left', padx=(0, 12))
        self.inputs.append(combo)
        self.button(row, '部署并启动', self.launch, primary=True).pack(side='left')
        self.button(row, '卸载代理', self.uninstall).pack(side='left', padx=8)
        self.button(row, '构建代理', self.build).pack(side='right')
        ttk.Label(config, text='安装或卸载前请退出游戏；选择实际渲染 EXE。仅支持 Windows x64 / D3D11。',
                  foreground='#52647b').grid(row=4, column=0, columnspan=3, sticky='w', pady=(8, 0))

        sessions = ttk.LabelFrame(frame, text=' 2  游戏会话 ', padding=12)
        sessions.grid(row=2, column=0, sticky='nsew', pady=(0, 12))
        sessions.columnconfigure(0, weight=1)
        sessions.rowconfigure(0, weight=1)
        self.tree = ttk.Treeview(sessions, columns=('pid', 'status', 'captures', 'directory'), show='headings', height=4)
        for name, label, width in [('pid', '进程 PID', 90), ('status', '观察状态', 210),
                                   ('captures', '采样文件', 80), ('directory', '会话目录', 480)]:
            self.tree.heading(name, text=label)
            self.tree.column(name, width=width, minwidth=60, stretch=name == 'directory')
        self.tree.grid(row=0, column=0, sticky='nsew')
        scroll = ttk.Scrollbar(sessions, orient='vertical', command=self.tree.yview)
        scroll.grid(row=0, column=1, sticky='ns')
        self.tree.configure(yscrollcommand=scroll.set)
        self.tree.bind('<<TreeviewSelect>>', self._selection_changed)
        row = ttk.Frame(sessions)
        row.grid(row=1, column=0, columnspan=2, sticky='ew', pady=(8, 0))
        self.button(row, '刷新会话', self.refresh).pack(side='left')
        self.button(row, '打开历史会话…', self.open_session).pack(side='left', padx=8)
        self.button(row, '代理日志', lambda: self.open_selected('proxy.log')).pack(side='right')
        self.button(row, '会话文件夹', lambda: self.open_selected()).pack(side='right', padx=8)
        ttk.Label(sessions, textvariable=self.selection_text, foreground='#52647b', wraplength=940).grid(
            row=2, column=0, columnspan=2, sticky='w', pady=(6, 0))

        capture = ttk.LabelFrame(frame, text=' 3  采样与报告 ', padding=12)
        capture.grid(row=3, column=0, sticky='ew', pady=(0, 12))
        ttk.Label(capture, text='调用数').pack(side='left')
        count = ttk.Spinbox(capture, from_=1, to=10000, textvariable=self.events, width=7)
        count.pack(side='left', padx=(8, 14))
        ttk.Label(capture, text='延迟 / 秒').pack(side='left')
        delay = ttk.Spinbox(capture, from_=0, to=60, textvariable=self.delay, width=4)
        delay.pack(side='left', padx=(8, 14))
        self.inputs.extend([count, delay])
        self.button(capture, '采样并打开报告', self.capture, primary=True).pack(side='left')
        self.button(capture, '分析已有采样', self.analyze).pack(side='left', padx=8)
        self.button(capture, '反汇编 Shader', self.disassemble).pack(side='left')
        ttk.Label(frame, textvariable=self.status, wraplength=990, foreground='#17488f').grid(
            row=4, column=0, sticky='ew', pady=(0, 8))
        self.log = tk.Text(frame, height=5, wrap='word', state='disabled', font=('Consolas', 9),
                           background='#132238', foreground='#dce8f7', relief='flat', padx=12, pady=8)
        self.log.grid(row=5, column=0, sticky='nsew')
        ttk.Label(frame, text='采样记录调用与资源绑定，无帧回放或 GPU 耗时；采样期间可能影响帧率。',
                  foreground='#52647b').grid(row=6, column=0, sticky='w', pady=(10, 0))
        self.root.update_idletasks()
        width = min(max(1080, frame.winfo_reqwidth() + 20), self.root.winfo_screenwidth() - 80)
        height = min(max(820, frame.winfo_reqheight() + 20), self.root.winfo_screenheight() - 100)
        self.root.geometry(f'{width}x{height}')

    def button(self, parent, label, command, primary=False):
        widget = ttk.Button(parent, text=label, command=lambda: self.guard(command),
                            style='Primary.TButton' if primary else 'TButton')
        self.actions.append(widget)
        return widget

    def guard(self, callback):
        try:
            callback()
        except Exception as error:
            self.error(error)

    def error(self, error):
        if getattr(error, 'winerror', None) == 740 and not self.admin and not self.elevation_attempted:
            self.offer_elevation()
            return
        self.status.set('操作未完成，请查看提示。')
        self.write_log(str(error))
        messagebox.showerror('操作未完成', str(error), parent=self.root)

    def offer_elevation(self):
        self.status.set('游戏要求管理员权限，当前尚未启动。已安装的代理保留。')
        self.write_log('启动被 Windows 拒绝（740）：需要管理员权限。')
        if not messagebox.askyesno('游戏需要管理员权限',
                                  '游戏要求管理员权限。是否以管理员权限重启本工具并重试启动？\n\n'
                                  'Windows 将显示 UAC 授权窗口。当前游戏、输出目录、模式和参数会保留。',
                                  parent=self.root):
            return
        arguments = [f'--{name}={getattr(self, name).get()}'
                     for name in ['exe', 'output', 'arguments', 'events', 'delay']]
        arguments += [f'--mode={MODES[self.mode.get()]}', '--resume-launch']
        try:
            started = restart_elevated(arguments, int(self.root.frame(), 16))
        except OSError as error:
            self.status.set('管理员窗口未能启动，当前窗口已保留。')
            self.write_log(str(error))
            messagebox.showerror('提权未完成', str(error), parent=self.root)
            return
        if started:
            self.close()
        else:
            self.status.set('已取消 Windows 管理员授权，游戏未启动；当前窗口和配置已保留。')

    def write_log(self, text):
        self.log.configure(state='normal')
        self.log.insert('end', text + '\n')
        if int(self.log.index('end-1c').split('.')[0]) > 800:
            self.log.delete('1.0', '200.0')
        self.log.see('end')
        self.log.configure(state='disabled')

    def _load_settings(self):
        try:
            settings = json.loads(self.settings_path.read_text(encoding='utf8'))
            if not isinstance(settings, dict):
                return
            # Launch arguments may contain credentials; deliberately do not persist them.
            for name in ['exe', 'output']:
                if isinstance(settings.get(name), str):
                    getattr(self, name).set(settings[name])
        except (OSError, ValueError):
            pass

    def _save_settings(self):
        try:
            self.settings_path.parent.mkdir(parents=True, exist_ok=True)
            temporary = self.settings_path.with_suffix('.tmp')
            temporary.write_text(json.dumps({'exe': self.exe.get(), 'output': self.output.get()},
                                            ensure_ascii=False), encoding='utf8')
            temporary.replace(self.settings_path)
        except OSError as error:
            self.write_log(f'未能保存界面设置：{error}')

    def choose_exe(self):
        path = filedialog.askopenfilename(parent=self.root, title='选择游戏的实际渲染 EXE',
                                          filetypes=[('Windows 程序', '*.exe')])
        if path:
            self.exe.set(path)

    def choose_output(self):
        path = filedialog.askdirectory(parent=self.root, title='选择采样输出目录', mustexist=False)
        if path:
            self.output.set(path)
            self.refresh()

    def unity_arguments(self):
        if '-force-d3d11' not in split_arguments(self.arguments.get()):
            self.arguments.set((self.arguments.get() + ' -force-d3d11').strip())

    def executable(self):
        if not self.exe.get().strip():
            raise ValueError('请先选择游戏 EXE。')
        path = Path(self.exe.get().strip()).resolve(strict=True)
        if path.suffix.lower() != '.exe' or not path.is_file():
            raise ValueError('请选择有效的 Windows x64 EXE 文件。')
        return path

    def selected(self):
        selection = self.tree.selection()
        if not selection:
            raise ValueError('请先在会话列表中选择目标游戏进程。')
        return Path(selection[0])

    def _selection_changed(self, _event=None):
        selected = self.tree.selection()
        if selected and selected[0] in self.sessions:
            info = self.sessions[selected[0]]
            self.selection_text.set(info['executable'] or str(info['path']))

    def open_selected(self, name=''):
        path = self.selected() / name
        if not path.exists():
            raise ValueError(f'文件尚未生成：{path}')
        os.startfile(path)

    def open_session(self):
        path = filedialog.askdirectory(parent=self.root, title='选择包含 proxy.log 的 pid_… 会话目录')
        if path:
            from gui_support import session_info
            info = session_info(Path(path))
            self.output.set(str(Path(path).parent))
            self._show_sessions([info], select=str(info['path']))
            self.refresh()

    def refresh(self):
        if self.closed or self.refreshing:
            return
        root = self.output.get().strip()
        if not root:
            return
        self.refreshing = True
        watch = self.launch_watch
        def work():
            try:
                items = list_sessions(Path(root))
                self.messages.put(('sessions', (root, items)))
                if (watch and root == watch['output'] and
                        not any(item['pid'] == watch['pid'] for item in items)):
                    diagnostic = legacy_proxy_diagnostic(watch['directory'], watch['pid'], watch['started'])
                    if diagnostic:
                        self.messages.put(('launch_diagnostic', (watch, diagnostic)))
            except Exception as error:
                self.messages.put(('refresh_error', str(error)))
        threading.Thread(target=work, daemon=True).start()

    def _show_sessions(self, items, select=None):
        selection = select or next(iter(self.tree.selection()), None)
        self.sessions = {str(item['path']): item for item in items}
        for iid in self.tree.get_children():
            if iid not in self.sessions:
                self.tree.delete(iid)
        for index, (iid, item) in enumerate(self.sessions.items()):
            values = (item['pid'], item['status'], item['captures'], item['path'].name)
            if self.tree.exists(iid):
                self.tree.item(iid, values=values)
                self.tree.move(iid, '', index)
            else:
                self.tree.insert('', index, iid=iid, values=values)
        if selection not in self.sessions:
            # Never silently choose another live process. Auto-select only an exact EXE match.
            matches = [iid for iid, item in self.sessions.items() if item['alive'] and item['executable']
                       and os.path.normcase(os.path.abspath(item['executable'])) ==
                       os.path.normcase(os.path.abspath(self.exe.get()))]
            selection = matches[0] if len(matches) == 1 else None
        if selection:
            self.tree.selection_set(selection)
            self._selection_changed()
        else:
            self.selection_text.set('请选择目标会话；没有会话时，检查游戏是否使用 DX11、代理是否实际加载。')

    def task(self, title, work):
        if self.busy:
            return
        self.busy = True
        self.status.set(title)
        self._save_settings()
        for widget in self.actions + self.inputs:
            widget.configure(state='disabled')
        def run():
            try:
                result = work(lambda text: self.messages.put(('log', text)),
                              lambda text: self.messages.put(('status', text)))
                self.messages.put(('done', result))
            except Exception as error:
                self.messages.put(('error', error))
        threading.Thread(target=run, daemon=True).start()

    def launch(self):
        executable = self.executable()
        arguments = split_arguments(self.arguments.get())
        if not self.output.get().strip():
            raise ValueError('请选择输出目录。')
        output = Path(self.output.get()).resolve()
        mode = MODES[self.mode.get()]
        def work(log, notify):
            pe_exports(executable)
            binary = PROJECT / 'build/d3d11.dll'
            if not binary.is_file():
                raise ValueError('尚未生成代理 DLL。请先点击“构建代理”（需要 MSVC C++ 工具与 Windows SDK）。')
            if pe_exports(binary) != pe_exports(Path(os.environ['SystemRoot']) / 'System32/d3d11.dll'):
                raise ValueError('代理导出表与本机系统不一致，请重新构建。')
            try:
                needed = deployment_needed(executable.parent, binary)
            except MissingProxyError as error:
                log(str(error))
                notify('代理 DLL 缺失，正在备份清单并补齐…')
                run_cli(['repair', executable.parent], log)
                needed = deployment_needed(executable.parent, binary)
            if needed:
                notify('正在安装代理…')
                run_cli(['install', executable.parent], log)
            else:
                log('已核对安装清单和 DLL 哈希，复用当前版本代理。')
            notify('正在启动游戏…')
            started = time.time()
            process = launch(executable, output, mode, arguments, detach_output=True)
            self.messages.put(('process', process))
            self.messages.put(('launch_watch', dict(pid=process.pid, directory=executable.parent,
                                                    started=started, output=str(output))))
            log(f'已启动 PID {process.pid}；请在会话列表确认代理是否加载。')
            return ('游戏已启动，等待会话出现。能否观察以会话状态和代理日志为准。', None)
        self.tree.selection_remove(*self.tree.selection())
        self.task('准备部署并启动…', work)

    def uninstall(self):
        directory = self.executable().parent
        if messagebox.askyesno('卸载代理', f'请先退出游戏。\n\n将核验并卸载：\n{directory}\n\n采样数据保留。继续？', parent=self.root):
            self.task('正在核验并卸载…', lambda log, notify:
                      (run_cli(['uninstall', directory], log) and '代理已卸载，采样数据已保留。', None))

    def build(self):
        def work(log, notify):
            result = subprocess.run(['cmd.exe', '/d', '/c', str(PROJECT / 'build.cmd')], cwd=PROJECT,
                                    capture_output=True, creationflags=subprocess.CREATE_NO_WINDOW)
            log((result.stdout + result.stderr).decode('mbcs', errors='replace'))
            if result.returncode:
                raise RuntimeError(f'构建失败（退出码 {result.returncode}）。请检查 MSVC C++ 工具与 Windows SDK。')
            return ('代理构建完成。可选择游戏并启动；构建成功不代表目标游戏已验证。', None)
        self.task('正在构建代理，窗口可正常响应…', work)

    def capture(self):
        session = self.selected()
        count, delay = int(self.events.get()), int(self.delay.get())
        if not 1 <= count <= 10000 or not 0 <= delay <= 60:
            raise ValueError('调用数须为 1–10000，延迟须为 0–60 秒。')
        def work(log, notify):
            require_capture_ready(session)
            for seconds in range(delay, 0, -1):
                notify(f'{seconds} 秒后采样，请切回游戏并保持渲染…')
                if self.stop.wait(1):
                    return None
            require_capture_ready(session)
            previous = {p.name for p in session.glob('capture_*.jsonl')}
            run_cli(['capture', session, '--events', count], log)
            path = wait_capture(session, previous, self.stop, notify)
            if path is None:
                return None
            notify('采样完整，正在生成报告…')
            run_cli(['analyze', session], log)
            return (f'采样完成：{path.name}，报告已打开。', path.with_suffix('.html'))
        self.task('准备采样…', work)

    def analyze(self):
        session = self.selected()
        def work(log, notify):
            path = latest_capture(session)
            if path is None:
                raise ValueError('此会话尚无采样文件。')
            run_cli(['analyze', session], log)
            complete = read_capture(path)[1]
            return (('完整采样，报告已打开。' if complete else '采样不完整：报告仅供排查，不能作为完整结果。'),
                    path.with_suffix('.html'))
        self.task('正在分析已有采样…', work)

    def disassemble(self):
        session = self.selected()
        def work(log, notify):
            if not any(session.glob('*.dxbc')):
                raise ValueError('此会话尚无导出的 Shader 字节码。')
            run_cli(['disassemble', session], log)
            return ('Shader 反汇编完成。', session / 'disassembly')
        self.task('正在反汇编 Shader…', work)

    def _poll(self):
        if self.closed:
            return
        while True:
            try:
                kind, value = self.messages.get_nowait()
            except queue.Empty:
                break
            if kind == 'sessions':
                self.refreshing = False
                output, items = value
                if output == self.output.get().strip():
                    self._show_sessions(items)
            elif kind == 'refresh_error':
                self.refreshing = False
                self.write_log('刷新会话失败：' + value)
            elif kind == 'log':
                self.write_log(value)
            elif kind == 'process':
                self.processes.append(value)
            elif kind == 'launch_watch':
                self.launch_watch = value
            elif kind == 'launch_diagnostic':
                watch, diagnostic = value
                if watch == self.launch_watch:
                    self.launch_watch = None
                    self.write_log(diagnostic)
                    if not self.busy:
                        self.status.set(diagnostic)
            elif kind == 'status':
                self.status.set(value)
            elif kind in ('done', 'error'):
                self.busy = False
                for widget in self.actions + self.inputs:
                    widget.configure(state='readonly' if isinstance(widget, ttk.Combobox) else 'normal')
                if kind == 'error':
                    self.error(value)
                    if self.closed:
                        return
                elif value:
                    status, path = value
                    self.status.set(status)
                    if path:
                        self.guard(lambda: os.startfile(path) if path.is_dir() else webbrowser.open(path.as_uri()))
                self.refresh()
        self.poll_timer = self.root.after(150, self._poll)
        # Poll by elapsed Tk time without creating a second recurring callback chain.
        if not hasattr(self, '_refresh_ticks'):
            self._refresh_ticks = 0
        self._refresh_ticks += 1
        if self._refresh_ticks % 14 == 0:
            self.processes = [process for process in self.processes if process.poll() is None]
            self.refresh()

    def close(self):
        if self.closed:
            return
        if self.busy:
            messagebox.showinfo('操作进行中', '请等待当前操作结束再关闭。采样最多等待 120 秒；关闭界面不会退出游戏或卸载代理。', parent=self.root)
            return
        self.closed = True
        self.stop.set()
        for name in ['poll_timer', 'refresh_timer', 'launch_timer']:
            timer = getattr(self, name, None)
            if timer:
                self.root.after_cancel(timer)
        self._save_settings()
        self.root.destroy()


def parse_options(arguments=None):
    parser = argparse.ArgumentParser(description='D3D11 Render Inspector GUI')
    for name in ['exe', 'output', 'arguments', 'events', 'delay']:
        parser.add_argument('--' + name)
    parser.add_argument('--mode', choices=list(MODES.values()))
    parser.add_argument('--resume-launch', action='store_true')
    return vars(parser.parse_args(arguments))


def main():
    import ctypes
    try:
        ctypes.windll.shcore.SetProcessDpiAwareness(1)
    except (AttributeError, OSError):
        pass
    options = parse_options()
    root = tk.Tk()
    InspectorApp(root, initial=options)
    root.mainloop()


if __name__ == '__main__':
    main()
