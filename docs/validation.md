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

独立项目版本未重新部署到实际游戏；旧测试包成功运行属于迁移来源的历史证据，不替代新版本目标程序验收。未完成其他 Windows/驱动组合、真实 Present、Deferred CommandList 展开、完整帧回放、移动端与其他图形 API 验证。没有测量该代理的 GPU 性能开销，不能用这些功能测试宣称无开销。
