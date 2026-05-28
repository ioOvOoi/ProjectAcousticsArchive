# Ray Tracing 音频计算替换伴随状态

## 目标

在不改 Project Acoustics 核心声学算法、不改既有体素/探针/`.ACE` 数据语义的前提下，基于 Project Acoustics 增加可落地的动态能力。当前优先目标切换为：实现双 PA runtime / 双 ACE 高质量平滑切换原型，用于静态建筑状态变化（如完整/坍塌）时平滑过渡声学参数；后续再叠加大面积动态修正层。

## 当前分支

- 分支：`RayTracing`

## 阶段

### 阶段 1：范围确认与现有架构复核

- 状态：进行中
- 输出：明确哪些 CPU 计算可替换为硬件 Ray Tracing，哪些 Triton 核心算法不能碰。

### 阶段 2：提出 2-3 个架构方案

- 状态：未开始
- 输出：对比 UE 硬件光追查询替换 CPU 射线、GPU 加速批量查询、保持 Triton 不变只替换外围计算。

### 阶段 3：用户确认设计方向

- 状态：未开始
- 输出：用户批准一个设计方向后，再写正式设计文档与实施计划。

### 阶段 4：正式实施计划

- 状态：被新优先级替代
- 输出：使用 `writing-plans` 生成 `docs/superpowers/plans/YYYY-MM-DD-<slug>.md`。

### 阶段 5：双 PA runtime / 双 ACE 平滑切换原型

- 状态：进行中
- 输出：最小可用原型：切换期间保留旧 ACE runtime，同时加载新 ACE runtime；每帧查询两份声学参数并按权重混合；过渡结束释放旧 runtime。
- 验收：能从 ACE A 切到 ACE B，切换过程中参数连续、无突变；不破坏原有单 ACE 加载路径。

## 约束

- 不创建 `.opencode/atlas/`。
- 编辑函数/类/方法前必须做 GitNexus impact analysis。
- 先设计，用户批准后再实现。
- 优先小步增量，避免一次性重写整个插件。
- 目标平台只面向支持硬件 Ray Tracing 的电脑端；无需无光追兜底。
- 明确不新增“声学 DDGI 探针场”，不改 Triton 的体素、探针、`.ACE` 核心数据结构与核心查询算法。
- 目标是“不要用 CPU 算可迁移的射线/可见性/距离类工作”，不是重写声学模型。
- 双 runtime 原型优先保持小步增量：先做功能和生命周期验证，不同时引入硬件 Ray Tracing 动态修正层。

## 错误记录

- 方向误判：曾将目标理解成新增 AcousticProbeGrid / 声学 DDGI。用户已纠正：不希望改核心算法、体素、探针，只想用硬件 Ray Tracing 替代 CPU 计算。
- GrepMCP rate limit：批量查询与 20 秒后单次重试均失败。下一步可稍后重试，或用户同意后改用其他联网搜索方式定位公开源码。
- planning-with-files catchup 第一次尝试失败：命令使用了 `C:\Users\Administrator\.claude\skills\planning-with-files\scripts\session-catchup.py`，该路径不存在；本机 skill 实际在 `.config/opencode/skills/` 下。下一步如需要追赶，改用正确脚本路径。
