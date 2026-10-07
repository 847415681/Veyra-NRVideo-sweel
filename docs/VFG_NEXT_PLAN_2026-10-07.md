# VFG 继续优化（2026-10-07）

用户：“VFG的优化反响非常好，看看能不能继续尝试优化”。从正式2.0.6后的main 96a7c8de隔离到codex/vfg-next-20261007；工作树E:/项目/Veyra/worktrees/vfg-next-20261007。只改VFG backend及针对性测试/构建/诊断/本地候选。DLSS/XeSS/FSR、NR算法、显示策略、时钟和用户配置不变；不派Agent、不做压力/竞争负载、不merge/push/Release/关机。

1. 独立start.json与source-before.bundle保护34既有工作树、495脏文件及3691项发布原件/证据；每轮运行scripts/acceptance/vfg-next-control.py，不改旧guard或baseline。
2. 用原GTA6 4K30、原版单层1080NR/强度1/零运动/调控关、实际GPU普通、Auto同步/限帧关，复测Medium6、High3和Medium4。记录软件提交FPS、GPU各阶段、降档及错误，不把提交FPS当物理显示/端到端延迟。
3. 假说：同一输入对的每个插帧都重复producer signal/CUDA wait，导致D3D消费拷贝与下一张CUDA工作串行。尝试每对输入只同步一次；保留逐输出completion wait/独立槽/租约/有界worker/失败排空。capture revision、parity、reset、sub1都必须重建依赖。环境变量仅用于同EXE旧模式对照，不改变用户设置。
4. 必须与原2.0.6及同EXE旧模式相邻对照，8/10-bit全部质量/2X–8X像素逐位核对，重复输入地址更新、反向parity/reset、中途起始subframe、失败回退、退出/seek/切换和完整导出计数。实测无收益或画面变化则撤回产品改动并保留报告。
5. 只有证据支持才处理额外VFG专属瓶颈；禁止靠放宽过期门槛、丢更多帧、降低画质或放慢时钟伪造收益。所有产物E:/项目/Veyra下同名任务目录，测试每进程≤300秒、构建≤900秒。正常GPU串行；已有SDK/DLL/模型原字节保持且不进源码Git。

当前：开工保护完成，优化尚未实现，收益待测。NVIDIA显存增长及其它硬件长稳不由本轮短测视为解决。


## 第二节点：VFG排队预算修正

同步减量实验已测：同EXE High3 68/68.5fps、Medium8 210/210fps、Medium6 180/180fps，未证明收益，撤回VfgBackend.cpp；实验ff8fdf4及独立EXE保留在tests/vfg-next-20261007/bin/producer-v1。SDK1.3官方CudaGraph selector实查get/set均-5，不实施CUDA图假说。能力探针首次未建CUDA context创建失败，补齐产品相同初始化后成功，原失败记录不作SDK不支持证据。

具体发现：EngineController文件来源pendingCompletions的GPU成本，对fgReduced=true且fgEvaluated=1的VFG组仍用fgBudget.predicted的请求倍率完整成本。只在VFG这一分支按baseCost + Fg1 GPU P95核算一张插帧，并以完整成本为上界；无测量/非1次/历史恢复沿用原估计。其它补帧、admitFile、截止时间/宽限/倍率/reset/画质不改。必要接点扩展到src/engine/EngineController.cpp、独立include/veyra/engine/VfgQueueBudget.h和tests/unit/VfgSettingsTests.cpp，guard独立范围同步扩展，不改start或旧保护。环境VEYRA_TEST_VFG_FULL_REDUCED_QUEUE_COST仅作旧估计对照。先相邻High3、Medium8/7，若无收益或增加过期/跳源帧则撤回；已有全像素合同和生命周期/导出在保留候选后验收。
