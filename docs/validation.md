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
