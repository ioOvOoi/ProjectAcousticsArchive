# ProjectAcousticsArchive / Project Acoustics 存档

一个 [Project Acoustics](https://github.com/microsoft/ProjectAcoustics) 及其处理器的**非官方存档**（Unofficial Archive）。

An **Unofficial Archive** for [Project Acoustics](https://github.com/microsoft/ProjectAcoustics) and its processors.

参见微软研究院 Triton 项目：https://www.microsoft.com/en-us/research/project/project-triton/

See Microsoft research project Triton: https://www.microsoft.com/en-us/research/project/project-triton/

我会尽量保持更新，但不做承诺。

I'll try to keep it updated, but no promises.

---

## 图文教程 / Step-by-Step Guide

### 第零步：准备本地烘焙工具 / Step 0: Prepare Local Bake Tools

> ⚠️ 官方 **Download Local Bake Tools** 下载链接已不可用。请直接使用仓库 `Processors` 文件夹下自带的本地烘焙工具。 / The official download link is no longer available. Use the local bake tools bundled in the repo's `Processors` folder instead.

👉 [本地烘焙工具说明文档 / Local Bake Tool Guide](Processors/Windows/README.md)

### 第一步：切换至 Bake Acoustics 模式 / Step 1: Switch to Bake Acoustics Mode

<img src="./File/01-BakeMode.jpg" alt="Bake Acoustics 模式" width="300" />

在关卡编辑器工具栏中，点击 **Modes** 下拉菜单，选择 **Bake Acoustics** 模式。此时视口右侧会显示声学烘焙面板，包含 **Probes**、**Materials**、**Objects** 和 **Bake** 四个选项卡。

In the level editor toolbar, click the **Modes** dropdown and select **Bake Acoustics** mode. The acoustics bake panel will appear on the right side of the viewport with four tabs: **Probes**, **Materials**, **Objects**, and **Bake**.

---

## 教程与资源 / Tutorials & Resources

### 官方教程直播录像 / Official Tutorial Livestream

- [**Microsoft Project Acoustics UE5 Marketplace Plugin | Inside Unreal**](https://www.youtube.com/watch?v=3uocCX0AMIg) — 官方 UE5 插件教程直播，涵盖 Marketplace 插件安装、场景标记、材质分配、探针布局、Azure 烘焙、以及 Source Data Override 接口和 MetaSounds 集成。

  Official UE5 plugin walkthrough livestream, covering Marketplace plugin installation, scene markup, material assignment, probe layout, Azure baking, Source Data Override interface, and MetaSounds integration.

### 官方文档 / Official Documentation

- [Project Acoustics Unreal 插件概述 (Unreal Plugins Overview)](https://learn.microsoft.com/en-us/gaming/acoustics/unreal-overview)
- [Project Acoustics 文档中心 (Documentation Hub)](https://aka.ms/acoustics)
- [GitHub 官方仓库 (Official Repository)](https://github.com/microsoft/ProjectAcoustics)
- [UE Marketplace 插件页面 (Marketplace Plugin)](https://www.unrealengine.com/marketplace/en-US/product/project-acoustics-for-unreal-audio)

### 其他视频资源 / Additional Video Resources

- [**Microsoft Project Acoustics in Unreal Engine 5 | GameSoundCon 2022**](https://www.youtube.com/watch?v=MAMz9dSHU04) — UE5 集成走查，含 Lyra 示例 + MetaSounds
- [**Project Acoustics | GDC 2019**](https://www.youtube.com/watch?v=uY4G-GUAQIE) — 技术概览，波物理引擎原理与设计理念

### 示例项目 / Sample Project

- [Project Acoustics Sample for Unreal Engine](https://github.com/viayulo/AcousticsGameUE) — UE5 官方示例项目

## 测试验证 / Verified Environments

| 引擎版本 / Engine Version | 状态 / Status | 备注 / Notes |
|---|---|---|
| **Unreal Engine 5.7.4** | ✅ 编译通过 / Compiled | 发行版（Shipping），插件正常编译并启用运行 |
| **Unreal Engine 5.7.4** | ✅ Compiled Successfully | Shipping build; plugin compiles and enables without errors |

---

## 更新日志 / Changelog

### 2026-05-29 — `2d317032bbc47cb4612ebfff54229a4d4094801e`

**提交标题：** `✨ feat(acoustics): 实现双运行时 ACE 交叉淡入淡出`

本次提交为 Project Acoustics Unreal 插件增加了 **双 Triton 运行时（dual Triton runtime）/ 双 ACE 交叉淡入淡出（crossfade）** 原型。目标是应对静态建筑状态切换，例如“完整建筑 ACE → 坍塌建筑 ACE”，在运行时平滑混合两套 `.ACE` 查询结果，避免声学参数突然跳变。

#### 功能概览

- 新增 `IAcoustics` 接口：`LoadAceFileForCrossfade(...)`、`TickAceCrossfade(...)`、`IsAceCrossfadeActive()`。
- `FProjectAcousticsModule` 支持同时持有当前 `m_Triton` 和旧 `m_PreviousTriton`。
- 交叉淡入期间，声源查询会同时查询旧/新 Triton runtime，并混合 `TritonAcousticParameters`。
- 新增 `AcousticsParameterBlend.h`：dB 响度先转能量域再混合，方向向量混合后归一化，路径长度、扩散角、衰减时间线性混合。
- `AAcousticsSpace` 增加蓝图入口：`AceCrossfadeDurationSeconds`、`LoadAcousticsDataWithCrossfade(...)`、`IsAceCrossfadeActive()`。
- 动态开口（dynamic opening）增加注册表：新 runtime 加载后自动重放已注册门窗开口；交叉淡入期间新增、删除、更新 opening 会同步到旧/新 runtime。
- 交叉淡入结束后等待后台查询任务完成，再释放旧 runtime。
- 新增自动化测试 `ProjectAcoustics.Crossfade.BlendsAcousticParameters`，验证参数混合逻辑。

#### 关键代码片段

新增运行时接口：

```cpp
virtual bool LoadAceFileForCrossfade(const FString& filePath, const float cacheScale, const float durationSeconds) = 0;
virtual void TickAceCrossfade(float deltaSeconds) = 0;
virtual bool IsAceCrossfadeActive() const = 0;
```

新增蓝图入口：

```cpp
UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Acoustics|Crossfade")
float AceCrossfadeDurationSeconds;

UFUNCTION(BlueprintCallable, Category = "Acoustics|Crossfade")
bool LoadAcousticsDataWithCrossfade(UAcousticsData* newBake, float durationSeconds);

UFUNCTION(BlueprintCallable, Category = "Acoustics|Crossfade")
bool IsAceCrossfadeActive() const;
```

声学参数混合逻辑：

```cpp
inline float BlendDb(float FromDb, float ToDb, float Alpha)
{
    Alpha = ClampAlpha(Alpha);
    return PowerToDb(FMath::Lerp(DbToPower(FromDb), DbToPower(ToDb), Alpha));
}

inline TritonAcousticParameters Blend(
    const TritonAcousticParameters& From,
    const TritonAcousticParameters& To,
    float Alpha)
{
    Alpha = ClampAlpha(Alpha);

    TritonAcousticParameters Result = To;
    Result.Dry.PathLengthMeters = FMath::Lerp(From.Dry.PathLengthMeters, To.Dry.PathLengthMeters, Alpha);
    Result.Dry.LoudnessDb = BlendDb(From.Dry.LoudnessDb, To.Dry.LoudnessDb, Alpha);
    Result.Dry.ArrivalDirection = BlendDirection(From.Dry.ArrivalDirection, To.Dry.ArrivalDirection, Alpha);

    Result.Wet.LoudnessDb = BlendDb(From.Wet.LoudnessDb, To.Wet.LoudnessDb, Alpha);
    Result.Wet.ArrivalDirection = BlendDirection(From.Wet.ArrivalDirection, To.Wet.ArrivalDirection, Alpha);
    Result.Wet.AngularSpreadDegrees = FMath::Lerp(From.Wet.AngularSpreadDegrees, To.Wet.AngularSpreadDegrees, Alpha);
    Result.Wet.DecayTimeSeconds = FMath::Lerp(From.Wet.DecayTimeSeconds, To.Wet.DecayTimeSeconds, Alpha);

    return Result;
}
```

双 runtime 切换核心：

```cpp
bool FProjectAcousticsModule::LoadAceFileForCrossfade(
    const FString& filePath, const float cacheScale, const float durationSeconds)
{
    if (!m_AceFileLoaded || durationSeconds <= 0.0f)
    {
        return LoadAceFile(filePath, cacheScale);
    }

    WaitForRunningTasks();
    ClearPreviousRuntime();

    m_PreviousTriton = m_Triton;
    m_PreviousAceFileLoaded = m_AceFileLoaded;
    m_PreviousTritonIOHook = MoveTemp(m_TritonIOHook);
    m_PreviousTritonTaskHook = MoveTemp(m_TritonTaskHook);

    m_Triton = TritonAcoustics::CreateInstance();
    m_AceFileLoaded = false;

    if (!m_Triton || !LoadAceFileIntoCurrentRuntime(filePath, cacheScale))
    {
        m_Triton = m_PreviousTriton;
        m_AceFileLoaded = m_PreviousAceFileLoaded;
        m_TritonIOHook = MoveTemp(m_PreviousTritonIOHook);
        m_TritonTaskHook = MoveTemp(m_PreviousTritonTaskHook);
        m_PreviousTriton = nullptr;
        m_PreviousAceFileLoaded = false;
        return false;
    }

    ReplayDynamicOpeningsToCurrentRuntime();

    m_IsAceCrossfadeActive = true;
    m_AceCrossfadeDurationSeconds = durationSeconds;
    m_AceCrossfadeElapsedSeconds = 0.0f;
    return true;
}
```

查询阶段混合旧/新 ACE 参数：

```cpp
if (m_IsAceCrossfadeActive && m_PreviousTriton && m_PreviousAceFileLoaded)
{
    bool previousSuccess = GetAcousticParametersFromRuntime(
        m_PreviousTriton, sourceLocation, listenerLocation, previousParams, previousOpeningInfo, interpConfig);

    bool currentSuccess = GetAcousticParametersFromRuntime(
        m_Triton, sourceLocation, listenerLocation, currentParams, currentOpeningInfo, interpConfig, queryDebugInfoPtr);

    if (previousSuccess && currentSuccess)
    {
        const float alpha = FMath::Clamp(m_AceCrossfadeElapsedSeconds / m_AceCrossfadeDurationSeconds, 0.0f, 1.0f);
        acousticParams = AcousticsParameterBlend::Blend(previousParams, currentParams, alpha);
        openingInfo = alpha < 0.5f ? previousOpeningInfo : currentOpeningInfo;
        querySuccess = true;
    }
}
```

动态开口注册表与重放：

```cpp
struct FDynamicOpeningRegistration
{
    FVector Center = FVector::ZeroVector;
    FVector Normal = FVector::ForwardVector;
    TArray<FVector> Vertices;
    float DryAttenuationDb = 0.0f;
    float WetAttenuationDb = 0.0f;
};

TMap<class UAcousticsDynamicOpening*, FDynamicOpeningRegistration> m_DynamicOpeningRegistrations;
```

```cpp
void FProjectAcousticsModule::ReplayDynamicOpeningsToCurrentRuntime()
{
    for (const auto& registrationPair : m_DynamicOpeningRegistrations)
    {
        UAcousticsDynamicOpening* opening = registrationPair.Key;
        const FDynamicOpeningRegistration& registration = registrationPair.Value;
        if (AddDynamicOpeningToRuntime(
                m_Triton, opening, registration.Center, registration.Normal, registration.Vertices))
        {
            m_Triton->UpdateDynamicOpening(
                reinterpret_cast<uint64_t>(opening), registration.DryAttenuationDb, registration.WetAttenuationDb);
        }
    }
}
```

过渡结束释放旧 runtime：

```cpp
void FProjectAcousticsModule::TickAceCrossfade(float deltaSeconds)
{
    if (!m_IsAceCrossfadeActive)
    {
        return;
    }

    m_AceCrossfadeElapsedSeconds += FMath::Max(0.0f, deltaSeconds);
    if (m_AceCrossfadeElapsedSeconds >= m_AceCrossfadeDurationSeconds)
    {
        WaitForRunningTasks();
        ClearPreviousRuntime();
    }
}
```

#### 修改文件

- `.gitignore`
- `ProjectAcousticsNative/Source/ProjectAcoustics/Private/AcousticsParameterBlend.h`
- `ProjectAcousticsNative/Source/ProjectAcoustics/Private/AcousticsSpace.cpp`
- `ProjectAcousticsNative/Source/ProjectAcoustics/Private/ProjectAcoustics.cpp`
- `ProjectAcousticsNative/Source/ProjectAcoustics/Private/Tests/AcousticsParameterBlendTests.cpp`
- `ProjectAcousticsNative/Source/ProjectAcoustics/Public/AcousticsSpace.h`
- `ProjectAcousticsNative/Source/ProjectAcoustics/Public/IAcoustics.h`
- `ProjectAcousticsNative/Source/ProjectAcoustics/Public/ProjectAcoustics.h`
- `docs/superpowers/plans/2026-05-28-ray-tracing-audio-ddgi/findings.md`
- `docs/superpowers/plans/2026-05-28-ray-tracing-audio-ddgi/progress.md`

#### 验证记录

编译验证：

```powershell
 Win64 Development E:\WorkSpace\VillaBeta\VillaBeta.uproject -WaitMutex -NoHotReloadFromIDE -NoUBTMakefiles -NoXGE
```

结果：

```text
Result: Succeeded
```

自动化测试：

```powershell
UnrealEditor-Cmd.exe -ExecCmds="Automation RunTests ProjectAcoustics.Crossfade.BlendsAcousticParameters; Quit" -unattended -nop4 -nosplash
```

结果：

```text
Test Completed. Result={成功}
**** TEST COMPLETE. EXIT CODE: 0 ****
```

#### 使用方式

在蓝图或 C++ 中加载目标 ACE 数据时，调用：

```cpp
AcousticsSpace->LoadAcousticsDataWithCrossfade(NewAcousticsData, 2.0f);
```

其中 `2.0f` 表示旧 ACE 到新 ACE 的声学参数过渡时间，单位为秒。

#### 注意事项

- 该功能不是运行时重新烘焙，也不会修改 Triton 内部算法。
- 它适合“多个预烘焙静态状态之间平滑切换”，例如完整建筑 ACE 与坍塌建筑 ACE。
- crossfade 期间会同时查询两个 Triton runtime，查询成本和内存占用会临时升高。
- 真实听感仍需要使用两个有效 `.ACE` 文件在关卡中测试。

---

## TODO / 待办
- [ ] **创建一个 Demo 关卡** / **Create a Demo Level**
- [ ] **添加使用说明** / **Add Instructions for Usage**

---

## Issues / Bugfixes / 问题与修复

### 问题：Python 插件错误 / Issue: Python Plugin Error

如果你看到以下错误信息：

> **"python must be installed"**

（而 Python 插件实际已正确安装），且日志中出现如下错误：

If you see the error message:

> **"python must be installed"**

(while the Python plugin is correctly installed), and an error in the logs like this:

```
[2025.02.13-22.58.58:436][  0] LogPython: Display: Running start-up script C:/Projects/PA_Demo/Plugins/ProjectAcousticsNative/Content/Python/init_unreal.py... started...
[2025.02.13-22.58.58:587][  0] LogSourceControl: Uncontrolled asset enumeration finished in 0.190773 seconds (Found 7711 uncontrolled assets)
[2025.02.13-22.58.59:337][  0] LogPython: Error: System.NotSupportedException: An attempt was made to load an assembly from a network location which would have caused the assembly to be sandboxed in previous versions of the .NET Framework. This release of the .NET Framework does not enable CAS policy by default, so this load may be dangerous. If this load is not intended to sandbox the assembly, please enable the loadFromRemoteSources switch. See http://go.microsoft.com/fwlink/?LinkId=155569 for more information.
[2025.02.13-22.58.59:337][  0] LogPython: Error: The above exception was the direct cause of the following exception:
[2025.02.13-22.58.59:337][  0] LogPython: Error: Traceback (most recent call last):
```

---

### 修复方案 / Fix

1. 打开以下文件（Open the file）：
   ```
   C:\Windows\Microsoft.NET\Framework64\[你的 .NET 版本，如 4.0.x]\Config\machine.config
   ```

2. 找到这一行（Find this line）：
   ```xml
   <runtime/>
   ```

3. 将其替换为（Replace it with）：
   ```xml
   <runtime>
       <loadFromRemoteSources enabled="true"/>
   </runtime>
   ```

这应该可以解决 Unreal Engine 中沙箱程序集（sandboxed assembly）的错误。

This should resolve the issue with the sandboxed assembly error in Unreal Engine.
