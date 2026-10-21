# D3D11RenderInspector · Codex 项目指引

## 定位与阅读顺序

- 全程中文交流。此仓库是独立的 Windows x64 / D3D11 渲染观察工具，不继承其他 Unity 工程的规则。
- 开始开发先读 `README.md`，修改代理或采样逻辑前读 `docs/architecture.md`；历史验证范围见 `docs/validation.md`，最新本机证据由 `test-results/latest.txt` 指向。
- 当前是有界调用流与绑定状态采样，不是完整帧抓取/回放工具。不得将调用数、CPU tick 或采样耗时称为 GPU 帧耗时。
- 先检查 `git status --short`，保留已有改动。修改范围围绕当前任务，避免把单次游戏排障工具混入通用库。

## 目录与环境

| 路径 | 职责 |
|---|---|
| `src/proxy.cpp` | 加载真实系统 D3D11、转发导出、设备创建入口 |
| `src/observer.cpp` | 对象虚表观察、Shader 导出、Draw/Dispatch 状态采样 |
| `src/config.*` | 模式、输出目录、进程会话隔离 |
| `tools/generate_exports.py` | 本机 PE 导出表解析与 x64 MASM 转发桩生成/验证 |
| `tools/inspector.py` | 部署、卸载、启动、触发采样、分析和 DXBC 反汇编 |
| `tests/` | ABI、真实图形/计算读回、采样完整性、Python 工具测试 |
| `docs/` | 架构、来源、验证范围 |

- Windows 原生 PowerShell/CMD；C++17，MSVC x64 + Windows SDK；Python 3.10+ x64。
- `build.cmd` 用 vswhere 查找工具链；不依赖固定 VS 版本路径、不依赖旧交接目录、不手改生成的导出表。
- Python 工具维持标准库方案；需要新依赖时先说明具体用途。
- C++ 风格遵循 `.clang-format`；文件使用 UTF-8，换行遵循 `.gitattributes`。

## 改动后的验证

代理、构建或原生测试修改：

```powershell
.\build.cmd
python tests/run_tests.py
```

仅 Python 工具修改且已有本机构建产物：

```powershell
python -m unittest discover -s tests -p "test_*.py"
```

- 改到采样格式、分析或反汇编链路时，还应跑 `python tests/run_tests.py` 验证真实数据；未构建过先执行 `build.cmd`。
- 纯文档修改检查链接、命令与差异，不重复跑 GPU 测试。
- 检查每条命令退出码与 `result.json` 的 `passed`，不要只凭存在 DLL 或某条 PASS 日志下结论。
- 区分构建通过、独立 GPU 测试通过、目标游戏实际验证；只报告当前证据覆盖的部分。
- 变更支持范围或公开命令时同步 README；更新验证记录时保留 DLL 哈希和证据路径。

## 代理实现约束

- 真实 D3D11 从系统目录绝对路径加载；DllMain 保持最小化，不在加载器锁中初始化观察器、做文件扫描或调用复杂库。
- 保持导出名称/序号、x64 参数寄存器与栈参数、HRESULT、COM 引用计数及真实调用语义。修改转发桩必须验证 ABI。
- 不裁掉虚表私有尾部；当前尾部探测有兼容性风险，修改前先理解历史 GetFeatureLevel 崩溃原因。未知接口布局跳过观察并记录日志。
- 虚表副本和钩子函数地址必须覆盖被观察对象的生命周期；不得在对象存活时释放副本或卸载代理。
- 多进程输出/请求继续按会话隔离；采样保留数量上限和结束记录；不完整日志不能被分析器标记为完整。
- Deferred Context 不支持可靠 Get* 快照，不伪造其绑定关系；ExecuteCommandList 内部尚未展开，应明确记录边界。
- 资源指针可能重用，调试名称可能缺失；Shader/资源关联结论必须附事件与数据依据。

## 项目与目标程序边界

- 开发/构建/独立测试默认只操作本项目；接入目标游戏按用户指定的程序和目录执行，既有明确授权可继续，无需反复确认。
- 安装不覆盖已有 DLL；卸载核对安装清单与 SHA-256。不得自动删除其他注入工具、系统 DLL 或用户备份。
- 不在通用脚本中写死游戏进程名、磁盘路径、加载器补丁或强制结束游戏的逻辑。特定目标需要适配时把适配范围和恢复方式写清楚。
- 输出数据、游戏二进制、DXBC、纹理、崩溃转储、认证信息和个人环境配置不默认纳入版本控制。

## Git

- 主分支 `main`；远端地址以用户给定的为准，不自行创建远端项目或推送。
- 未要求提交时做到可审阅改动即可，不主动 commit / push；要求提交时按改动范围暂存，检查 `git diff --cached --check`。
- 提交标题使用 `feat/fix/refactor/test/docs/build/chore(scope): 具体改动`；AI 署名采用当前会话可确认的真实模型信息，不猜模型版本。
- 不执行破坏性 reset/clean，不回退他人的工作。生成物保持在忽略目录。

## Code Review Rules

- 优先检查 ABI/引用计数/线程安全/虚表寿命、输出隔离、采样有界性和卸载校验；指出实际故障条件与对应代码位置。
- 对“支持某 API/驱动/游戏”“还原了某算法”“性能提升”等结论核对实测范围，不以编译或历史报告代替目标验收。
