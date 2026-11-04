# 本机验证记录

验证日期：2026-10-21。系统：Windows 10，版本 10.0.19045。Python：3.13.1。

从当前项目源码完整重建，MSVC `/W4` 构建未报告警告。当前 DLL SHA-256：`d12abae3c88e9377fc668d17996e30a7f5e13f94700cde14e3ad92645c45bf96`。

| 验证项 | 结果与范围 |
|---|---|
| 导出表 | 51 个导出名称及序号与本机 System32/d3d11.dll 一致 |
| 转发 ABI | 整数寄存器、浮点寄存器及栈参数转发测试通过 |
| 纯转发模式 | 6 组设备创建、GetFeatureLevel、三角形绘制、4×4 纹理逐像素读回通过 |
| 仅 Shader 模式 | 同样 6 组绘制/读回通过，导出 6 个 PS，没有调用采样 |
| 调用观察模式 | 同样 6 组绘制/读回通过；6 份单事件 Draw 采样完整，PS 文件与 4×4 输出资源匹配 |
| 计算路径 | WARP Compute Shader Dispatch、4×4 UAV 写入、逐像素读回通过；1 份 Dispatch 采样完整并导出 CS |
| 反汇编与报告 | 6 个 PS + 1 个 CS 成功反汇编，生成 HTML、TSV 和 JSON 摘要 |
| 工具保护/完整性 | 4 项 unittest 通过：不完整日志、结束计数/序号、重复采样请求、已有/被修改 DLL 的部署卸载保护 |

六组设备组合是硬件 / WARP / CreateDeviceAndSwapChain 入口，分别以默认 / SINGLETHREADED 标志创建。CreateDeviceAndSwapChain 测试传入空交换链描述和输出，只验证该创建入口，不是实际交换链 Present 验证。

原绘制测试已改为先清黑再绘制目标颜色，避免把 Clear 的结果误认成 Draw 成功。这里的绘制共 18 组，另有 1 组计算 Shader 验证。

本机证据入口：[`result.json`](../test-results/20261021_201142/result.json)。原始证据位于忽略目录，不随 Git 分发；其他机器可运行测试重新生成。`test-results/latest.txt` 会指向之后最新一次验证；此页记载的是上述 DLL 哈希对应的验证。

## 未覆盖

实际游戏验证目前仅覆盖下文 2026-10-22 记录的目标程序启动场景及 32 事件短采样；旧测试包成功运行属于迁移来源的历史证据，不替代新版本目标程序验收。未完成其他游戏场景、Windows/驱动组合、真实 Present、Deferred CommandList 展开、完整帧回放、移动端与其他图形 API 验证。没有测量该代理的 GPU 性能开销，不能用这些功能测试宣称无开销。

## GUI 集成验证（2026-10-21）

新增中文 Tkinter 界面、双击启动入口和桌面流程测试。代理 C++ 未改动，复用以上 DLL，SHA-256 仍为 `d12abae3c88e9377fc668d17996e30a7f5e13f94700cde14e3ad92645c45bf96`。

- `python tests/run_tests.py` 退出码为 0，最新本次证据为 [`result.json`](../test-results/20261021_210110/result.json)，`passed: true`。ABI、三种模式绘制/读回、计算读回、真实 DXBC 反汇编和报告生成全部通过。
- Python 测试共 13 项通过，包括原有 4 项与新增 9 项：Windows 参数引用、PID 创建时间匹配、部署复用校验、完整采样等待、独立启动环境、GUI 多实例选择、实际 CLI 报告生成、错误恢复和界面采样流程。GUI 采样流程测试模拟数据生产端；不冒充 GUI 操作实际游戏的验证。
- 在本机 2560×1440 / 125% DPI 环境打开窗口检查布局；修正列表压缩，增加小窗口滚动。实际 `start_gui.cmd` 启动了响应正常的 `pythonw` 窗口；截图位于 `test-results/20261021_210110/gui-launch.png`，布局检查截图位于 `test-results/gui-preview.png`。
- 未安装到实际游戏目录，未验证目标游戏或外部启动器。GUI 仍需要 Python 3.10+ x64 / Tcl/Tk；未提供自带 Python 运行时的独立 EXE。

### GUI 权限错误修复（2026-10-21）

用户在实际游戏目录部署后提供了启动阶段 WinError 740 截图；这证明该次部署已成功，但不能证明游戏已启动或代理已加载。本次新增 740 专用的管理员重启/重试流程，取消 UAC 或提权失败时保留原窗口，重启时保留输出目录、模式和启动参数，已提权重试不再循环弹出提权请求。

`python -m unittest discover -s tests -p "test_*.py"` 退出码 0，共 20 项通过。新增验证覆盖 ShellExecuteExW x64 结构与参数、UAC 取消/失败、部署后启动报错、配置恢复和自动重试、防止重复提权。证据见 [`result.json`](../test-results/20261021_gui_elevation/result.json)。UAC 返回值使用模拟，未实际授权 UAC 或启动目标游戏；未改动代理 DLL，SHA-256 仍为 `d12abae3c88e9377fc668d17996e30a7f5e13f94700cde14e3ad92645c45bf96`。本次仅修改 GUI 启动流程，未重跑原生 GPU 测试，之前的完整测试证据继续保留。

### 缺失代理修复（2026-10-21）

后续现场检查确认：目标目录的默认安装清单仍有效，但 `d3d11.dll` 缺失；当前本机构建已更新为 SHA-256 `f6853556e578226db9c94c27ff14ea7266b45f3d08181b8a48aaf01012963d72`。未确定 DLL 被移除的原因。GUI 现在区分文件缺失、清单损坏、自定义加载名及哈希变化；对有效清单下的缺失 DLL 自动调用 `repair`，备份旧清单、排他创建 DLL，并原子更新清单。任何已存在的 DLL 均拒绝覆盖。

- `python tests/run_tests.py` 退出码 0，[本次 result.json](../test-results/20261021_220203/result.json) 的 `passed` 为 `true`，所有子命令退出码为 0。25 项 Python 测试通过；同时通过 ABI、三种模式绘制/读回、计算读回、真实反汇编和报告生成。
- 新增回归覆盖缺失旧版本恢复、备份及卸载、已有 DLL 保护、损坏/自定义清单拒绝、更新失败回滚，以及 GUI 先修复再启动和修复失败时停止启动。
- 已在用户指定的目标目录补齐代理，核验旧清单备份、当前 DLL SHA-256 和新清单一致，GUI 部署复用检查通过。现场证据见 [deployment-repair.json](../test-results/20261021_220203/deployment-repair.json)。未启动游戏，未将文件修复当作实际游戏兼容性验收。

## 旧代理迁移与实际游戏验证（2026-10-22）

用户反馈游戏可启动但 GUI 无会话。现场 PID 36296 的 `D3D11Proxy_36296.log` 标记为 `observer-v4-private-tail`；旧 `bld11.dll` 的对应标记、旧源代码的固定输出路径和实际旧输出目录均吻合。该版不读取新版输出环境变量，数据进入原工具目录，不能通过 GUI 刷新转换为新版会话。默认 `d3d11.dll` 再次缺失的原因仍未确定，此现象与旧版代理仍在使用的证据分别记录。

GUI 现已遵循安装清单中的自定义 DLL 名称，并能识别本次启动 PID 的新写入旧版日志，明确提示迁移；不会触发旧版共享请求文件。28 项 Python 回归通过，包括自定义名称复用/修复/卸载、默认同名文件保留及旧日志时间/PID 检查。新版 DLL 改名后的独立计算读回、会话及单事件完整采样也通过，见 [custom-name-result.json](../test-results/20261022_legacy_proxy/custom-name-result.json)。DLL SHA-256 为 `f6853556e578226db9c94c27ff14ea7266b45f3d08181b8a48aaf01012963d72`。

经用户明确确认并退出游戏后，在 `G:\Games\BH3` 完成迁移：旧 DLL 备份为 `bld11.dll.before-inspector-20261022.bak`，旧安装清单备份为 `.d3d11-inspector-install.json.before-custom-20261022.bak`，新版代理以 `bld11.dll` 安装。备份旧 DLL SHA-256 为 `f9ea59aceeb5a6ac74a4495fd15fbb4ed355ac436da942dedcd21bfc1fb722ab`。本次不修改游戏 EXE、UnityPlayer 或原工具输出。迁移证据见 [migration-result.json](../test-results/20261022_legacy_proxy/migration-result.json)。此适配仅沿用该目标原有的自定义加载名，不代表其他游戏会加载此名称。

恢复方式：退出游戏，通过工具卸载当前清单中哈希匹配的新版代理；确认目标 DLL 和清单位置已空，再将上述两份备份分别改回 `bld11.dll` 和 `.d3d11-inspector-install.json`。不要覆盖现有文件；恢复旧代理后仍使用旧输出机制。

随后通过新版管理员 GUI 启动游戏，确认新会话 `pid_94700_134371093607459850` 出现，日志标记为 `D3D11RenderInspector-0.1-private-tail`，Device5 / Context4 观察器安装成功。启动场景的 32 个 DrawIndexed 事件采样具有完整结束记录、连续序号和正确事件数，生成 HTML 报告；分析时索引了 3054 个 Shader 文件。证据见 [live-game-result.json](../test-results/20261022_legacy_proxy/live-game-result.json) 和本地 `test-results/20261022_legacy_proxy/live-gui.png`。这些证据覆盖此次游戏启动和短采样，不代表整帧、所有场景或 GPU 性能验收。

## 通用接入诊断与另一目标的接入失败（2026-11-04）

代理未修改，SHA-256 仍为 `f6853556e578226db9c94c27ff14ea7266b45f3d08181b8a48aaf01012963d72`。本轮先运行 `python tests/run_tests.py`，退出码 0，[result.json](../test-results/20261104_105751/result.json) 的 `passed` 为 `true`；这是独立测试证据。

新增通用 `diagnose` 命令与 GUI 无会话诊断，仅修改 Python 工具、测试及文档。`python -m unittest discover -s tests -p "test_*.py"` 退出码 0，37 项通过，包含实际当前 Python 进程模块枚举、权限不足、PID 重用、枚举期间身份变化、自定义代理名称、哈希变化与无效清单保护及精确文件路径的成功删除事件归属。证据为 [diagnostic-result.json](../test-results/genshin-analysis/diagnostic-result.json) 与同目录 `diagnostic-tests.log`；其他进程/驱动/游戏兼容性不由这些测试证明。

在用户指定的原神客户端以 `forward` 模式进行受控启动：包头/游戏日志版本为 7.2.0，DX11 启动成功，但未建立代理会话。微软 Process Monitor 记录 PID 46524 的 `YuanShen.exe` 于 `11:04:00.3239347` 对部署的代理成功执行 `SetDispositionInformationEx`，包含 `FILE_DISPOSITION_DELETE`。因此本次可以归属删除请求的进程，但不能从进程名确定其内部具体模块或机制。原始证据为本地 `test-results/genshin-analysis/launch-trace.pml`、`launch-trace.csv`，提取结果为 [proxy-file-events.json](../test-results/genshin-analysis/proxy-file-events.json)。

新命令针对该现场返回 `proxy_missing`、退出码 2，且从追踪中识别上述删除事件，见 [diagnosis.json](../test-results/genshin-analysis/diagnosis.json)。这验证了失败诊断，未解决该客户端的代理接入，也没有取得实时地形数据；未修改游戏二进制或其加载策略。原始采样/资源与追踪继续保留在忽略目录中。

## 启动接入与原神实际短采样（2026-11-04，后续）

以上“尚未解决”描述的是同目录代理阶段。后续新增可选的 `run-startup` CLI 和独立启动器，使用 Microsoft Detours v4.0.1 固定提交 `e4bfd6b03e50de46b47abfbd1e46b384f0c5f833`，在入口前加载工具目录中的观察器并截获系统 D3D11 设备创建。未向通用代码写入目标路径、进程名或游戏补丁。`build.cmd` 和 `build_injected.cmd` 均退出码 0；本轮原代理的完整独立测试 [result.json](../test-results/20261104_121105/result.json) 的 `passed` 为 `true`。

启动模式独立测试覆盖显式加载 `System32/d3d11.dll` 的硬件/WARP/两个设备创建入口、SINGLETHREADED 标志、三种观察模式、逐像素绘制读回、计算 UAV 读回、Shader 导出、有界采样完整性和反汇编；测试还使用带空格的 EXE 路径。入口之前加载一个缺少初始化导出的普通代理会被拒绝，不启动目标。证据入口为忽略目录 `test-results/startup-latest.txt`，每次结果含各子命令退出码与 `passed`。这里仍不是游戏场景或其他驱动/游戏组合的兼容性证明。

实测加载的观察器 SHA-256：`ae6567e62723171d87c529658481bca25032045b959799569490baaca1c9345f`；启动器 SHA-256：`1217549ca00cca29377363075c5b30da4edf3590739a8c3ad908147568cfbf60`。原神先以 forward 模式启动 PID 63896，设备和交换链创建成功，模块路径为工具目录中的 `build/inspector_hook.dll`。随后正常退出并以 observe 模式启动 PID 53676，会话为 `captures/genshin-startup-observe/pid_53676_134382391903414890`；Device5/Context4 观察器安装成功，首次 32 事件采样具有完整结束记录和连续事件编号，包含 Draw、DrawIndexed、DrawIndexedInstanced。证据为 [startup-live-result.json](../test-results/genshin-analysis/startup-live-result.json)、同目录 `startup-game-live.json` 及会话中的 `proxy.log`、`capture_1.jsonl`、`capture_1.summary.json`。

该短采样主要使用 `miHoYo/Scene/Login Base`，确认的是登录场景接入与调用采样，不代表已经采集可游玩地形、完整帧或 GPU 耗时。没有修改磁盘上的游戏 EXE；调试器临时入口断点在恢复入口前还原，观察器钩子固定到进程退出，不支持运行中卸载。此前一次官方 RenderDoc 1.46 标准启动对照在设备创建阶段退出，不能将其启动返回的控制 ID 当作捕获成功。

用户随后确认已进入游戏，采集 `capture_2.jsonl`：2000 事件，完整结束记录、连续编号、无解析错误；1121 份 Shader 成功反汇编。其中实际调用 CTS Terrain、BlendStone 和 Terrain Base Map；代表事件 30 的两张 1280×720 输出与事件 109 的岩石输入资源对应，PS 反汇编可核对高度竞争及按深度差混合颜色/法线的代码。现场细节和局限见统一分析文档目录中的 [terrain-runtime-findings.txt](../../GenshinImpact/terrain-runtime-findings.txt) 与原始 [terrain-runtime-evidence.json](../test-results/genshin-analysis/terrain-runtime-evidence.json)。这些数据仍不是完整帧、纹理读回、常量读取或 GPU 性能测试。

## 采样格式版本 3：调用参数与深度状态（2026-11-04）

`capture_N.jsonl` 头部版本升到 3：每个事件新增 `args`（Draw 系列的顶点/索引/实例数与起始偏移、Dispatch 线程组数、间接调用的参数缓冲身份与偏移、ExecuteCommandList 的 restore 标志；参数先以原值存在栈上，只在采样进行中才格式化，不采样时不分配字符串），图形调用新增 `depth_target`（DSV 视图格式、只读标志、资源）与 `depth_state`（深度开关、写掩码、比较函数、模板开关与 ref；未绑定状态对象时按 D3D11 默认值记录并标 `default=1`）。间接参数缓冲内容仍不读回。`analyze` 的 TSV/HTML 新增"调用参数"和"深度"两列，旧版本采样这两列为空。

`render_smoke` 改为绑定 4×4 D24S8 DSV 与非默认深度状态（LESS_EQUAL、写入关、ref=5），测试断言记录值与设置一致；计算测试断言线程组参数且无深度字段。`build.cmd`、`build_injected.cmd` 退出码 0，MSVC `/W4` 无警告。`python tests/run_tests.py` 退出码 0，[result.json](../test-results/20261104_141000/result.json) `passed: true`，Python 回归 38 项通过；`python tests/run_startup_tests.py` 退出码 0，`test-results/startup_20261104_141019/result.json` `passed: true`。

代理 DLL SHA-256：`2128d84590f602680e1fd3e42347f8dd659535902d5dac744d936c0300554cdb`；启动观察器 `inspector_hook.dll` SHA-256：`6ef3c4e9c5e07eac91edebc8ed3be231ed3e03a8c0c9f87819d1737d1f672fa6`。以上为独立测试证据，尚未用新版本在目标游戏中采样。
