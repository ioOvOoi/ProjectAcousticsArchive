# 进度记录

## 2026-05-28

- 用户确认已切换到 `RayTracing` 分支，并希望把插件改成类似 DDGI 的 Ray Tracing 音频插件。
- 已初始化伴随状态目录：`docs/superpowers/plans/2026-05-28-ray-tracing-audio-ddgi/`。
- 下一步：先给出可行架构选项，并只问一个关键范围问题。
- 用户确认：目标平台只面向支持硬件 Ray Tracing 的电脑端。
- 用户纠正方向：不要新增 AcousticProbeGrid / 声学 DDGI 探针，不要改核心体素、探针或算法；目标是用硬件 Ray Tracing 替代 CPU 侧计算。
- 已只读复核查询链：`UpdateObjectParameters -> GetAcousticQueryResults -> GetAcousticParameters -> Triton::QueryAcoustics`。
- 已只读复核距离链：`UpdateDistances/QueryDistance -> Triton::UpdateDistancesForListener/QueryDistanceForListener`。
- 发现 Triton 公开 hook 只有 IO、内存、异步任务、日志；无计算替换 hook。
- 当前结论：不改 Triton 核心/无 Triton 源码时，无法直接把 `QueryAcoustics` 内部 CPU 算法替换成硬件 Ray Tracing；只能做外围新增或替换插件层可控功能。
- 用户要求使用 GrepMCP 查公开代码。已尝试查询 `class TritonAcousticsImpl`、`UpdateDistancesForListener QueryDistanceForListener`、`QueryAcoustics TritonAcoustics`、`Triton.Runtime ProjectAcoustics`，GrepMCP 返回 rate limit。等待 20 秒后单查仍 rate limit。
- 用户选择改用联网搜索。已查 Microsoft ProjectAcoustics 仓库、Microsoft Research Triton 页面、UE Marketplace、官方 issue #193、Project Acoustics 3.0 发布文章、SonoTraceUE、Meta XR Acoustic Ray Tracing 文档。
- 联网结果未找到 Triton Runtime 内部公开源码；外部证据支持“Project Acoustics 是离线 wave acoustics bake + 轻量 runtime 查询，不是实时 ray tracing 核心”的判断。
- 用户询问 Team Pinpoint Audio Tracing。已读取文档并确认：该插件走 Unreal Spatialization Plugin + Source Data Override 插件链，依赖 UE 硬件 Ray Tracing/Lumen 场景，使用 Sound Material + Custom Primitive Data，不使用 Project Acoustics / ACE / Triton。
- 用户询问 Audio Tracing 是否有开源链接。已联网检索，未找到 Team Pinpoint 官方源码；找到 SonoTraceUE、RSAP、Spatial Audio Plugin 等可参考开源项目。
- 用户提出动态物体挂组件、只计算该 Actor 体素和探针的想法。当前判断：不能直接作为 Triton 局部探针/体素注入；可以转化为动态声学代理组件，做硬件光追/局部近似并后处理 PA 输出参数。
- 用户询问 IceMoonAcousticField。已读取 README，判断它是低成本运行时网格/异步射线采样声学场，适合动态环境响应参考，但不是 Project Acoustics/Triton 替换，也非 UE 硬件 RT 插件。
- 用户提供 triton-lang / microsoft triton-shared / triton-san 链接。已确认这些是深度学习 Triton 编译器生态，不是 Project Triton 音频声学 runtime 源码。
- 用户决定回到 Project Acoustics 基础路线：先做“双 PA runtime / 双 ACE 高质量平滑切换”，用于静态建筑形态变化；动态修正层后续再做。
- 已更新本状态目录目标与阶段。planning-with-files catchup 首次使用 `.claude` 路径失败，已记录；当前直接根据已读计划文件继续。
