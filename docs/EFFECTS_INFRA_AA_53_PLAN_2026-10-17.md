# 效果器扩展基础 E0 与 #53 空间抗锯齿：施工方案（2026-10-17）

分支 `claude/aa-53-20261017`（worktree `E:/项目/Veyra/worktrees/aa-53-20261017`），基线 main `26c0d61`。上位计划：`E:/项目/Veyra/reports/claude-plan-48-53-beauty-20261017/PLAN_48_53_BEAUTY_2026-10-17.md` 第 3、4 节。授权与用户原话见 `AGENTS.md` 顶部同日条目。

存档：`E:/项目/Veyra/archives/claude-aa-53-20261017/`（`start.json`：88 工作树 / 100 分支 / 主检出 64 个脏文件逐文件 SHA256；`source-before.bundle` verify 通过，76,295,892 字节，SHA256 `f22a36f998b4d0b7eaa181537fc68b2c32fb2a75229a508f0af65d4803041095`）。产物目录：`{build,tests,logs,tmp,reports,archives}/claude-aa-53-20261017`。

## 1. 范围

两件事，按顺序，分开提交：

1. **E0 效果器扩展基础**（抗锯齿与美颜共用，只在本会话做一次；美颜会话合入 `claude/effects-infra-20261017`）。
2. **#53 空间抗锯齿**：FXAA 中/高/极高、SMAA 低/中/高/极高、MLAA（阈值/强度）、实验“时域 SMAA”（T2x/4x NoJitter，默认关）。

不做：带抖动的 SMAA T2x/4x、会改分辨率的 Anime4K 3D AA 放大与 SGSR、整套 MagpieFX 通用运行时。

## 2. 现状（main `26c0d61`）要点

- 调色在图里有三类位置：①列表/节点首个调色融合进输入转换，列表额外调色在 `srcRgba_` 上逐个原位执行（`additionalColorInstances_`）；②节点模式的“槽”调色（SR 前/NR 前/NR 层间/链尾），由 `EnhanceGraphDesc::preSrColor/preNrColor/interNrColorTarget/tailColor` 按**调色参数序号**判定，`EnhanceGraph` 里各有一份维护计数、输出指针、视图和派发代码；③这些判定同时驱动 `validateFixedExecutionPlan` 的次序校验。
- `kMaxChainNodes=16`，`toChain()` 满载静默覆盖最后一格；`EffectType` 6 项；`GpuStage` 25 项，`GpuTimer` 用 32 位掩码；导出共享协议 9、预设库最高 13、链会话最高 8。

## 3. E0 设计

### 3.1 数据与格式
- `EffectType` 末尾追加 `AntiAliasing`(6)、`Beauty`(7)，既有数值不变；`effectTypeCount` 与所有定长数组（目录、计划资源计数、列表顺序表）随之 8 项。目录项新增 `hidden`：`Beauty` 在 E0 起即隐藏；`AntiAliasing` 在 E0 提交内隐藏，抗锯齿提交中放开。
- `kMaxChainNodes` 16→32；`toChain()` 满载不再覆盖，返回明确失败（`EffectChain::overflow` 标记，`validateChain` 拒绝）；`NodeGraphLayout` 等相关数组随常量增长。
- `AntiAliasSettings`（方式、质量档、MLAA 阈值/强度、时域实验与历史权重）进入 `ChainNode`/`EnhancementSettings`（实例 0 为 `antiAlias`，第 2 实例为 `additionalAntiAlias`，与调色同构），含校验、`activeVideoSettings`、`sameVideoConfiguration`、`requiresGraphRebuild`。
- 导出共享协议 9→10（`bytes=sizeof(Shared)` 校验保留）；预设库最高 v14（只有含新类型/超过 16 节点的内容写 v14，旧内容仍写最低够用版本）；链会话最高 v9。旧版读到更高版本明确拒绝并保留原文件；导出 worker 版本/字节不符拒绝。
- `GpuStage` 增加 `AaNode0-1`、`BeautyNode0-5`、`BeautyDetect`（共 34 项），`GpuTimer` 掩码 64 位；`enhancementProcessingMs` 把抗锯齿区间并入增强链耗时；旧 Win32 遥测名表同步。

### 3.2 插入槽通用化（图内）
- 新接口 `SlotPass`（`createViews/dispatch/reset`，同尺寸输入→输出的简单 pass）；`ColorGradeInstance` 实现它，`AntiAliasPass`、以后的美颜 pass 也实现它。
- `EnhanceGraphDesc` 用**按计划步骤**的 `placementOfStep(step)` 取代四个按调色序号的判定：返回 `Ingress / SourcePost / PreSr / PreNr / InterNr(target) / Tail`。判定条件逐字沿用旧四个函数，仅把“邻接的调色”推广为“邻接的简单 pass”（调色、抗锯齿、美颜）；旧函数保留为薄封装，`EffectChainTests` 里的既有断言继续成立。
- 列表模式（无 `runtimeNodeOrder`）按类型固定：调色=Ingress，抗锯齿=SourcePost，美颜=Tail。节点模式头部区域（第一个 SR/NR/保护之前）的前导调色仍走输入转换融合；其后的简单 pass 进 SourcePost，其间的调色（含参数 0）作为槽内节点调色，保证“按链顺序执行”。
- 图内每个槽一张 `SlotList`（按计划顺序的 `{pass, output, 计划步骤, 尺寸, 计时阶段}`），统一做资源创建、视图绑定、派发与计时；原调色四类派发/视图/计数/日志代码改为遍历槽列表，调色输出逐字节不变。
- 新槽：`SourcePost`（输入转换与列表额外调色之后、任何放大之前，源分辨率）供抗锯齿；`Tail` 在列表模式预留给美颜（NR 组含保护收尾之后、RTX HDR 之前，NR 先行时在 SR 之后）。
- 对比视图“原画”仍取抗锯齿之前的 `srcRgba_`；`workRgba_` 与 SR/NR 的“阶段输入”改走 `stageInput` 访问器。

### 3.3 界面接点
- `effectCatalog/effectAvailability` 新 id `aa`、`beauty`（隐藏项不进入菜单）；节点页编辑器分发、色标；列表处理顺序条；`nodeTimings`/`stageTimings`；四语言翻译。

## 4. 抗锯齿设计（E0 之后）

- 着色器：`shaders/MagpieCompat.hlsli`（`GetInputPt/GetInputSize/GetOutputSize/Rmp8x8/GetFrameCount` 与 PS 风格 pass→CS 包装）、`shaders/Fxaa.hlsli`、`shaders/Smaa.hlsli`、`shaders/Mlaa.hlsli`；CMake 增 `veyra_add_compute_shader_variant`（同源多预设，`-D` 宏）。`ComputePass::create` 增可选静态采样器。
- 颜色域：线性 FP16 工作纹理；先用一个小 pass 把线性颜色转成感知亮度纹理（SDR sRGB 曲线，HDR=PQ 归一化亮度，负值/超范围夹到 0 后编码）；边缘检测只读它；混合与邻域重采样仍作用在线性纹理，HDR 高光与负分量保留。
- `AntiAliasPass`：持有各变体 PSO、`edges/blend` 中间纹理、SMAA 查找纹理、时域历史；seek/切镜/断点/尺寸变化/重建重置历史。
- 位置：列表 `调色→抗锯齿→SR→NR→保护→(美颜)→RTX HDR→补帧`，NR 先行 `调色→抗锯齿→NR→保护→SR→…`；节点模式最多 2 个实例，RTX HDR 之后禁止。
- 界面：“画质”页最上方（超分辨率之上）新卡片“抗锯齿”；节点“添加节点”新增“抗锯齿”；预设/会话/继续上次/导出带参数。
- 许可：补回 FXAA 的 NVIDIA 2011 声明，保留 SMAA/MLAA MIT 声明与 Magpie GPLv3；`THIRD_PARTY_NOTICES.md` 逐文件记录来源提交、改动与 SHA256；SMAA 查找表 `AreaTex.dds`/`SearchTex.dds` 取自 Magpie 同一提交，随构建复制到 shaders 目录。

## 5. 测试与回归门槛

- **调色逐字节回归（E0 门槛）**：`veyra_effect_slot_regression_tests` 每配置一个进程渲染一帧，输出 8 位结果、FP16 线性输入、对比参考的 SHA-256。配置：列表默认（SR+NR+3 调色）、NR 先行、6 调色、双 NR+保护、节点 SR 前/NR 前/NR 层间/链尾各一组、混合（SR 先行与 NR 先行）、仅 SR 链尾、拆分 NR。在**未改产品代码**的基线上先录制，E0 后逐项对比必须全部相同；基线先自比对两次确认确定性。
- 单元：`veyra_effect_chain_tests`（链校验与上限、满载拒绝、计划与槽判定、会话/预设往返、旧版拒绝）、预设库/会话/导出协议测试、GpuTimer 掩码测试、UI 契约/翻译检查。
- 抗锯齿：合成锯齿图（多角度细线、文字/UI）边缘阶梯指标、平坦区几乎不变、HDR 无 NaN/截断/负值丢失；时域实验的重置与拖影；1080p/1440p/4K 各档 GPU 耗时实测。
- 约束：单测试 ≤300 秒、构建 ≤900 秒、每类配置只测一次、不做压力测试；GPU 计时前确认无他人 Veyra 进程。

## 6. 提交计划

1. `tests:` 切片回归 harness + 本方案 + AGENTS 授权（未改产品代码，用于录制基线）。
2. `E0:` 效果器扩展基础（回归通过后建 `claude/effects-infra-20261017` 指向它，不再移动；后续修正用 `[E0-fix]` 前缀单独提交并记入 WORKLOG）。
3. `AA:` 抗锯齿（着色器、AntiAliasPass、图接入、桥接/QML/翻译、预设/会话/导出、许可、测试与报告）。

## 7. 未验证项（预先声明）
AMD/Intel 实卡、用户 Switch 素材、长时间运行、光学延迟均不在本轮验证范围；本机 RTX 结果不外推其它硬件。
