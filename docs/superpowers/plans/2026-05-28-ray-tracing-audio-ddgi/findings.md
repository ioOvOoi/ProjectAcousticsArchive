# 发现记录

## 已知现有结构

- `AAcousticsSpace` 当前负责加载单个 `UAcousticsData` / `.ACE`，每帧更新监听者位置、流式区域、outdoorness、距离图。
- `FProjectAcousticsModule::LoadAceFile()` 会先 `UnloadAceFile(false)`，再 `InitLoad(...)` 新 `.ACE`，当前不是多状态混合架构。
- `UAcousticsDynamicOpening` 支持固定开口几何 + 每帧动态更新干声/湿声衰减，适合门窗，不适合任意动态破坏几何。
- Triton Runtime 查询输出是声学参数，不是可直接 Lerp 的 `.ACE` 数据。

## 初步结论

- Ray Tracing 的目标已收窄：不新增声学探针场，不改核心声学算法，只评估把 CPU 侧射线/可见性/距离类计算迁移到硬件 Ray Tracing。
- 用户确认目标平台只需支持硬件 Ray Tracing 的电脑端，因此可以优先使用 UE 硬件光追路径，不必设计无光追兜底。

## 纠偏记录

- `AcousticProbeGrid` 原意是“新增类似 DDGI 的声学探针缓存”，但这会改变插件架构，偏离用户要求。
- 正确方向：保留 Triton / Project Acoustics 现有体素、探针、`.ACE` 与查询语义，只寻找可替换的 CPU 计算路径。

## 2026-05-28 只读代码复核

### 运行时声学查询路径

- `FProjectAcousticsModule::UpdateObjectParameters(...)` 负责每个声源的运行时参数更新。
- 它主要做异步调度与结果缓存：首帧可同步查询，之后把查询放到后台任务。
- 真正声学查询进入 `GetAcousticQueryResults(...)`，再进入 `GetAcousticParameters(...)`。
- `GetAcousticParameters(...)` 只做坐标转换，然后直接调用：
  - 非 Shipping：`GetTritonDebugInstance()->QueryAcoustics(...)`
  - Shipping：`m_Triton->QueryAcoustics(...)`
- 结论：主要声学 CPU 计算在 `Triton.Runtime.lib` 黑盒内，不在插件 C++ 源码中。

### 距离图路径

- `FProjectAcousticsModule::UpdateDistances(...)` 只做坐标转换，然后调用 `m_Triton->UpdateDistancesForListener(listener)`。
- `FProjectAcousticsModule::QueryDistance(...)` 只做方向转换，然后调用 `m_Triton->QueryDistanceForListener(dir)`。
- `TritonPublicInterface.h` 注释明确：`QueryDistanceForListener` 使用预计算数据，不是实时 ray tracing，只消耗少量三角函数。
- 结论：距离图也在 Triton 黑盒/预计算数据中，插件层没有 CPU 射线循环可直接替换。

### Triton hook 能力

- `TritonHooks.h` 只有：
  - `ITritonIOHook`：读取预计算数据。
  - `ITritonMemHook`：内存分配。
  - `ITritonAsyncTaskHook`：异步加载任务。
  - `ITritonLogHook`：日志。
- 未发现 ray tracing、visibility、distance、occlusion 的计算 hook。
- 结论：不能通过公开 hook 把 Triton 内部 CPU 算法替换成硬件 Ray Tracing。

### 模块依赖

- `ProjectAcoustics.Build.cs` 当前只依赖 `Core/CoreUObject/Engine/Projects`，链接 `Triton.Runtime.lib`、`Triton.Codec.lib`、`zfp.lib`。
- 未见 `RHI`、`RenderCore`、`Renderer`、`RayTracing` 运行时依赖。
- 结论：Ray Tracing 支持目前不是插件运行时模块的一部分，需要新增 UE 渲染/RHI 侧桥接模块或依赖。

## 2026-05-28 联网检索结论

- Microsoft Research 的 Project Triton 页面说明：Triton 核心是预计算波声学，运行时使用专有参数压缩数据做快速查询；这支持“核心算法/数据不是实时 ray tracing 替换”的判断。
- Unreal Marketplace 页面说明：Project Acoustics 避免 CPU intensive raytracing，类似静态光照，离线 bake 物理基线，运行时 lightweight 查询；也说明 Unreal 插件驱动 occlusion、arrival direction、convolution reverb 参数。
- Microsoft ProjectAcoustics GitHub 仓库是问题反馈/发布说明仓库，已归档；检索未发现公开 `TritonAcousticsImpl` 或 `Triton.Runtime` 内部源码。
- 官方 issue #193 中 Microsoft 回复建议：动态门通常 bake 时开门，运行时用 hit testing 判断门开关；Unity binary 无法拦截底层 acoustics runtime 查询，只能禁用声源或调 occlusion multiplier。此点与本地判断一致：无 runtime 内部 hook 时只能做外围修正。
- 官方发布说明提到 2022.1：Triton encoder 4.0、新 voxel-free interpolator、voxels only kept around for debug visuals、可同时加载多个 ACE。这提示“体素”在新版主要为 debug，可见核心查询仍在 Triton 参数场/插值黑盒内。
- 找到参考项目 SonoTraceUE：UE5 硬件加速 Ray Tracing 声学仿真插件，可作为 UE 光追音频实现参考，但它是另一套声学系统，不是 Project Acoustics 内部算法替换。
- 找到 Meta XR Acoustic Ray Tracing 文档：动态物体在 baked acoustics 中可影响 direct sound occlusion，但不影响 late reverb/reflections/diffracted paths；这与“外围 ray tracing 修正只能改直达遮挡等，不等价替换 Triton 传播”的边界一致。

## 2026-05-28 Team Pinpoint Audio Tracing 文档分析

- 文档地址：`https://teampinpoint.gitbook.io/teampinpoint-docs/`。
- 它不是 Project Acoustics / ACE / Triton 改造路线；文档未提 `.ACE`、Triton 或 Project Acoustics。
- 集成方式是 Unreal 原生音频插件链：
  - Windows Audio 设置里把 `Spatialization Plugin` 设为 `Audio Tracing Spatializer`。
  - 把 `Source Data Override Plugin` 设为 `Audio Tracing Audio Source Data Override`。
  - Sound Attenuation 中启用 `Plugin-Spatialized` 和 `Enable Source Data Override`。
- 它要求开启 UE 硬件 Ray Tracing，并要求 Lumen GI 或 Reflections 至少一个使用 Lumen；说明它依赖 UE 渲染侧 Ray Tracing 场景。
- 它通过 `Audio Tracing Sound Material Component` 和 `Sound Material Asset` 给几何表面提供声学材质参数：scattering、reflection、absorption。
- 文档说明 Sound Material 数据编码到 `Custom Primitive Data`，供 Ray Tracing 使用。
- 它有自己的参数：ray count、max reflection、max trace time、max active sound sources、sound speed、head width、early reflection max count、reverb time multiplier。
- 结论：这是“另写一套 UE 音频光追插件”，使用 Unreal 音频扩展点和硬件光追场景；不是把 Project Acoustics 的 Triton CPU 核心换成 GPU。

## 2026-05-28 Audio Tracing 开源性检索

- 检索 `Team Pinpoint Audio Tracing Unreal plugin source GitHub`、`Audio Tracing Spatializer` 等关键词，未找到 Team Pinpoint / Audio Tracing 官方开源仓库。
- 搜到第三方资源站 `gfx-hub.co` 的 `Audio Tracing v5.6` 页面，疑似非官方资产转载/下载，不作为可用源码来源。
- 找到可参考的开源/公开项目：
  - `https://github.com/Cosys-Lab/SonoTraceUE`：UE5 硬件加速 Ray Tracing 声学仿真插件，MIT，C++ 源码，使用 Unreal RHI / DXR / compute shader 路线；偏超声/传感器研究，不是游戏音频替代品，但实现路径有参考价值。
  - `https://github.com/StelleBrink/RSAP`：Realistic Sound Attenuation and Pathfinding，UE5，C++，声学路径/衰减系统，使用 ray tracing/SVO，规模小但可参考。
  - `https://github.com/TobiasGrothmann/Spatial-Audio-Plugin-for-Unreal-Engine`：UE 空间化插件示例，非光追声学，但可参考 Spatializer 插件结构。
- 结论：Team Pinpoint Audio Tracing 本身看起来是闭源商店插件；若要学习实现，优先看 SonoTraceUE 的 RHI/DXR 管线和 UE 音频插件接入。

## 2026-05-28 动态物体组件化方案讨论

- 用户提出：给动态物体挂组件，只计算该 Actor 的体素和探针，用来计算动态物体音频模拟。
- 判断：作为“Project Acoustics/Triton 体素/探针局部增量注入”不可直接做。原因是 Triton 的探针/参数场来自全场景 bake，声传播不是单个 Actor 独立贡献，公开接口也没有运行时注入局部体素/探针的 API。
- 可行变体：把组件做成“动态声学代理组件”，只保存该 Actor 的声学材质、包围体/网格、硬件 Ray Tracing 可见性与厚度结果，再在 `Source Data Override` 输出前修正直达遮挡、低通、早期反射或湿声发送。
- 中等难度变体：组件维护 Actor 本地 voxel/SDF/BVH 仅用于快速近场遮挡/厚度/简化反射，不试图写回 Triton，也不声称是 PA 探针。

## 2026-05-28 IceMoonAcousticField 评估

- 仓库：`https://github.com/ioOvOoi/IceMoonAcousticField`，fork 自 `IceMoonTech/IceMoonAcousticField`。
- 定位：UE5.6 运行时动态声学场，低成本网格化系统，不是 Project Acoustics / Triton 替代，也不是硬件 Ray Tracing 插件。
- 核心：三层 Multi-LOD Sparse Grid、Z 轴高度钳制、声源驱动 `AsyncFireProbes`、Fibonacci sphere 分布、异步射线检测、材质映射、平滑查询、自适应状态机。
- 采样范围：README 写明异步 ray tracing 只采 Static/Stationary geometry，非 UE 硬件 RT/DXR；更接近 UE collision/raycast 环境采样。
- 输出参数：Wet、Delay、Decay、Density、Diffusion、Dampening，主要用于 reverb/环境响应，不是完整传播路径/波声学。
- 性能方向：每格约 140 bytes，稀疏分配；活跃每帧约 0.01ms tick/query；probe firing 异步隐藏开销。
- 构建限制：依赖 `IceMoonDataInterface`、`IceMoonBlueprintGPUMathUtilities`；README 明确外部编译不支持，但中文说明称可删掉关键依赖点（stat 与 GetCameraPosition 工具）。
- 结论：适合做“低成本动态环境响应/混响估计”的参考，不适合替代 Project Acoustics 的传播/绕射/portaling；可作为当前插件外围修正层或动态声学代理组件的设计参考。

## 2026-05-28 triton-lang / microsoft triton-shared 辨析

- 用户提供 `https://github.com/microsoft/triton-shared`、`https://github.com/triton-lang/triton`、`triton-san` 文档。
- 这些项目不是 Project Triton / Project Acoustics 的音频声学 Triton。
- `triton-lang/triton` 是深度学习 GPU kernel 语言与编译器，用于写高性能自定义 GPU/CPU 算子。
- `microsoft/triton-shared` 是 Triton compiler 的 MLIR middle-layer / reference CPU backend，仓库说明“Shared Middle-Layer for Triton Compilation”，且已不再维护。
- `triton-san` 是 Triton 程序动态分析工具，通过 triton-shared CPU backend + LLVM sanitizers 检测 Triton kernels 的 buffer overflow / data race。
- 这些仓库可用于写 GPU 计算 kernel，但与 `Triton.Runtime.lib` 声学运行时、`.ACE`、`QueryAcoustics`、Project Acoustics 波声学算法无直接关系。

## 2026-05-28 新优先级：双 PA runtime 平滑切换

- 用户确认先做“高质量方案：双 PA runtime”，用于 PA 数据平滑切换，应对静态建筑状态变化。
- 目标不是让 GPU 数据进入 `Triton.Runtime.lib`，而是同时保留旧/新 PA runtime 查询结果，在插件层混合参数。
- 最小原型应先解决生命周期和查询路径：当前 ACE、过渡来源 ACE、过渡进度、结束释放旧 runtime。
- 主要风险：Triton 是否支持同进程多个 runtime 实例、`UAcousticsData`/`FProjectAcousticsModule` 是否天然单例化、异步查询缓存是否可携带双数据来源、线程安全、内存/查询成本翻倍。
