# D3D11 Render Inspector

Windows x64 / Direct3D 11 渲染调用观察代理。从内部测试包分析时使用的 D3D11 proxy 独立整理，保留真实 D3D11 转发和有界调用采样。用于查明后处理调用顺序、渲染目标尺寸、输入输出关联、材质/后处理 Shader 字节码。

**当前版本是调用与状态观察工具，不是完整抓帧重放器，也不是 RenderDoc 替代品。**

## 快速开始

### 图形界面（推荐）

双击根目录的 **`start_gui.cmd`**（或已关联 Python 的 `inspector.pyw`）即可打开中文窗口，无需输入命令。也可运行 `python tools/inspector_gui.py`。界面使用 Python 自带的 Tkinter，无需安装第三方包；Python 安装时需勾选 Tcl/Tk。

1. 选择游戏实际渲染进程的 **EXE** 和采样输出目录。尚无 `build/d3d11.dll` 时先点“构建代理”，需要下述 C++ 工具链。
2. 退出游戏，点击“**部署并启动**”。默认调用观察模式；首次接入可先选“纯转发”验证兼容性，退出游戏后改回“调用观察”再次启动。游戏须使用 DX11；Unity 可点“Unity DX11”添加参数。
3. 会话列表自动刷新，显示运行状态、观察器状态和采样文件数。只有唯一一个运行中会话与所选 EXE 路径匹配时才自动选择；多个实例或启动器子进程须手动选择。PID 与进程创建时间一起核对，历史会话不会被误当作存活进程。
4. 进入目标场景后点“**采样并打开报告**”，默认倒计时 5 秒，切回游戏并保持渲染。默认记录 2000 个调用事件；完整结束后自动分析并打开浏览器报告，最多等待 120 秒。超时保留请求和数据，不会伪报成功；之后可用“分析已有采样”查看结果。
5. “分析已有采样”“反汇编 Shader”“代理日志”“会话文件夹”可直接查看输出。“打开历史会话”选择 `pid_…` 目录即可分析已有数据，无需运行游戏。
6. 用完退出游戏，再点“卸载代理”。关闭 GUI 不退出游戏、不卸载代理。任务执行期间先等待完成再关闭窗口。

部署复用必须同时通过安装清单、已安装 DLL 和当前构建 DLL 的哈希校验；遇到已有第三方 DLL、被修改的代理或旧版本会停止并提示，不覆盖文件。界面只记住 EXE 和输出路径，设置存于 `%LOCALAPPDATA%\D3D11RenderInspector\gui.json`；不保存启动参数。

如果安装清单有效、但清单指定的代理 DLL 缺失，“部署并启动”会先备份旧清单为 `.d3d11-inspector-install.json.backup-*`，再用当前本机构建补齐 DLL 并更新清单。已有 DLL（包括被修改的 DLL）不会被覆盖；清单损坏会明确报错。若 DLL 再次消失，请检查文件移除原因，工具不会循环补写。也可在退出游戏后手动修复：

```powershell
python tools/inspector.py repair "D:\Games\YourTestGame"
```

游戏启动返回 **WinError 740（需要提升权限）** 时，GUI 会提供“以管理员权限重启并重试”。确认 Windows UAC 后，管理员窗口会保留游戏、输出目录、模式、启动参数及采样设置，并重试启动；启动参数只通过此次进程命令行传递，不写入设置文件。取消 UAC 会保留原窗口。提权的是工具本身，由工具重新设置游戏的采样环境；不会修改系统环境变量，也不会重复覆盖已安装的代理。更新工具后须关闭旧窗口、重新打开 `start_gui.cmd` 才能加载此逻辑。

此版本提供可双击启动的桌面界面，仍依赖本机 Python/Tk，并非脱离 Python 的单文件 EXE。构建通过不等于目标游戏验证通过；完整采样也不是完整帧抓取或 GPU 耗时测量。

### 命令行

前置条件：Windows x64、Python 3.10+ x64、Visual Studio 2022 或 Build Tools（安装“使用 C++ 的桌面开发”、MSVC x64 工具和 Windows SDK）。Python 工具仅使用标准库，无需 pip 安装。无需 CMake。

在项目目录的 PowerShell 中：

```powershell
.\build.cmd
python tests/run_tests.py
```

- 代理 DLL：`build/d3d11.dll`
- 完整测试结果：`test-results/latest.txt` 指向的目录，内含 `result.json`、各组测试日志、DXBC、采样与分析结果。
- 构建时从本机系统 D3D11 读取导出表并生成 x64 MASM 转发桩；构建后校验所有名称与序号。分发到另一套 Windows 环境时应重新构建并验证。

## 接入目标程序

先关闭目标程序。在其 EXE 所在目录部署代理，再启动：

```powershell
python tools/inspector.py install "D:\Games\YourTestGame"
python tools/inspector.py run --exe "D:\Games\YourTestGame\Game.exe" --output "D:\Captures\MyGame" --mode forward -- -force-d3d11
```

`-force-d3d11` 是 Unity 示例参数，其他引擎按自身启动方式填写，或省略 `--` 及其后参数。先以 `forward` 验证转发能启动；退出程序后改用 `--mode observe` 启动，正常进入目标场景。

部署只复制代理并创建 `.d3d11-inspector-install.json`；已有同名 DLL 时直接拒绝，不覆盖，不修改 EXE / UnityPlayer / 驱动。部署和文件修复不代表目标游戏启动或采样验证通过，当前证据范围见 [验证记录](docs/validation.md)。

### 代理是否真的加载

```powershell
python tools/inspector.py sessions --output "D:\Captures\MyGame"
```

每个实际加载并调用代理的进程有独立的 `pid_<PID>_<进程创建时间>` 目录。检查其中的 `proxy.log`：

- `initialized real=...System32\d3d11.dll`：已转调系统 D3D11。
- `observer Device5 methods installed`、`observer Context4 methods installed`：观察器安装成功。
- `observer mode=forward`：当前仅转发，不记录 Shader / Draw。
- `skipped ...`：该接口布局不满足当前版本要求，不能把“程序正常运行”当成“已完成观察”。

没有会话/日志不代表没有渲染，可能是程序没有加载此 DLL、采用 D3D12/Vulkan、加载 DLL 后尚未调用导出，或输出目录不可写。部分程序显式加载 System32 的 D3D11，本地同名代理不会自动接管。

旧内部测试包采用过专门的加载名调整；这里没有打包该游戏二进制或通用二进制修补器。对显式加载系统 DLL 的程序，优先在自有测试程序/引擎加载入口指定代理 DLL。`install --name custom.dll` 仅支持使用已配置好的其他加载名，本身不会改变程序的加载行为。当前也不提供附加到已运行进程的功能。

GUI 会自动遵循有效安装清单中的自定义 DLL 名称，包括复用、缺失修复和卸载；首次 GUI 安装仍使用 `d3d11.dll`。已有旧代理不能直接覆盖，应退出目标程序，明确备份和恢复方式后再迁移。启动后若检测到本次 PID 的旧版 `D3D11Proxy_<PID>.log`，界面会明确提示旧代理不使用当前会话目录，不再只显示等待会话；旧版固定输出目录和共享采样请求不会自动导入或触发。

## 触发一次短采样

进入目标场景后，指定上一步找到的**完整会话目录**：

```powershell
python tools/inspector.py capture "D:\Captures\MyGame\pid_12345_134000000000000000" --events 2000
```

请求以文件原子写入，目标进程在绘制/计算调用上轮询，最多约 500 ms 的空闲轮询间隔。事件数范围 1–10000，默认 2000；它是**调用数，不是帧数或采样秒数**。停止绘制/切到后台可能使采样尚未完成，脚本发出请求也不等于采样完成。

输出：

| 文件 | 用途 |
|---|---|
| `proxy.log` | 转发与观察器安装状态 |
| `ps_*.dxbc`、`cs_*.dxbc` | 从观察器安装后创建的像素/计算 Shader 导出的原始字节码 |
| `capture_N.jsonl` | 按调用顺序记录的立即上下文状态 |
| `capture.request` | 待消费请求，开始采样后删除 |

JSONL 最后的 `{"type":"end","events":N}` 与实际事件数一致，才算完整。程序异常退出时，缓冲区中的尾部数据可能丢失；分析器会标记不完整采样。

## 分析与反汇编

```powershell
python tools/inspector.py analyze "D:\Captures\MyGame\pid_12345_134000000000000000"
python tools/inspector.py disassemble "D:\Captures\MyGame\pid_12345_134000000000000000"
```

`analyze` 生成：

- `capture_N.html`：可在浏览器打开的调用/资源绑定表。
- `capture_N.topology.tsv`：方便进一步过滤和统计的调用明细。
- `capture_N.summary.json`：完整性、调用计数、Shader 名称计数。
- `analysis.json`：整个会话摘要、Shader 文件 SHA-256。

`disassemble` 使用 Windows 自带的 `d3dcompiler_47.dll`，输出到 `disassembly/`。可结合 Shader 调试名称、资源分辨率链和采样指令判断降采样、模糊、合成关系。调试名称可能缺失；调用次数不是 GPU 耗时，不能据此直接给出性能结论。

## 模式与输出位置

| 环境变量 | 值 |
|---|---|
| `D3D11_PROXY_MODE` | `forward`：纯转发；`device-only`：只记录新建 PS/CS；`observe`：加上调用状态采样，默认值 |
| `D3D11_PROXY_OUTPUT` | 输出根目录，默认 `%LOCALAPPDATA%\D3D11RenderInspector\captures` |

`run` 为启动的进程及其继承环境的子进程设置这些变量，不改系统环境变量。使用外部启动器时须确认目标渲染进程继承环境，且代理安装在实际加载位置。日志和 Shader 创建记录在进入采样前就会写入，长期运行要注意输出目录大小。

## 卸载

退出目标程序后：

```powershell
python tools/inspector.py uninstall "D:\Games\YourTestGame"
```

仅删除安装清单中 SHA-256 一致的代理 DLL 与清单；文件被替换/修改时拒绝删除。采样数据保留。不会结束进程或替换系统 DLL。

## 支持边界

- 当前覆盖 Windows x64 / D3D11，观察器要求 Device5 / Context4 与原接口指针一致；其他接口布局会跳过观察。D3D9、D3D12、Vulkan、OpenGL、x86、移动端不在本版本范围内。
- 记录 Draw 系列、Dispatch 系列调用时的 PS/CS、SRV、RTV / CS UAV、常量缓冲区身份与大小，以及部分混合状态。常量值、纹理像素、Draw 参数、完整管线状态尚未采集；CS UAV 只覆盖前 8 槽。
- 没有 Present / 帧边界、GPU 时间戳、资源内容、回放器；不是 `.rdc`。
- Deferred Context 不采绑定快照；`ExecuteCommandList` 只留提交标记，不展开内部命令。不能据此证明命令列表内部的实际执行顺序。
- 资源 ID 是进程地址，可能复用；没有生命周期跟踪。PS/CS 字节码只覆盖安装观察器后经所钩入口创建的对象，不覆盖所有新版本创建入口或更早创建的 Shader。
- 采样时会同步查询状态、序列化并写盘，可能明显影响帧率。关闭采样后也仍有转发/Shader 记录开销，不用于正式性能基准。
- 为兼容先前遇到的运行时私有虚表尾部，保留了有上限的尾部探测。它依赖非公开实现，不能保证任意 Windows / 驱动 / 第三方封装均兼容。先用纯转发和独立测试验证，再验证目标程序；详细机制见 [架构说明](docs/architecture.md)。

## 项目结构

```text
src/                  真实 D3D11 转发、观察器、输出配置
tools/                导出生成、部署/卸载、启动、采样与分析 CLI
tools/inspector_gui.py 中文 Tkinter 桌面界面
tools/gui_support.py   会话身份检查、部署复用和采样完成等待
inspector.pyw          无控制台 GUI 入口
start_gui.cmd          双击启动 GUI
tests/                ABI、真实绘制/计算/读回、记录完整性、工具测试
docs/                 架构与验证范围
build.cmd             自动发现 VS C++ 工具链并从源码构建
build/                生成物，不入版本控制
test-results/         本机验证证据，不入版本控制
```

这是从已验证实验代码整理的独立工程。当前版本的重建与验收结果见 [验证记录](docs/validation.md)，后续修改应重新运行 `build.cmd` 和 `python tests/run_tests.py`。

## 使用 Codex 继续开发

项目指引位于根目录 [AGENTS.md](AGENTS.md)，包含源码导航、构建测试、兼容性限制和 Git 约定。从项目根目录开始会话：

```powershell
codex -C D:\Projects\D3D11RenderInspector
```

也可以在 Codex 应用中选择此目录作为项目。采用官方的 [AGENTS.md 项目指引机制](https://learn.chatgpt.com/docs/agent-configuration/agents-md)，无需另建账号或 API Key。新会话可先要求“读取 AGENTS.md 和 README，概述当前实现与验证范围”。

按项目所有者要求，`.codex/config.toml` 默认启用 **Full Access + YOLO**：`sandbox_mode = "danger-full-access"`，`approval_policy = "never"`。配置仅作用于此项目，模型沿用个人设置。项目本地配置需要将该目录标为 trusted；本机已配置该目录的信任项，其他机器首次打开时需信任项目。新建会话加载配置，显式启动参数或管理策略可能覆盖项目默认值。
