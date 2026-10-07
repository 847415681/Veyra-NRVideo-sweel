# VFG 继续优化（2026-10-07）

用户：“VFG的优化反响非常好，看看能不能继续尝试优化”。从正式2.0.6后的main 96a7c8de隔离到codex/vfg-next-20261007；工作树E:/项目/Veyra/worktrees/vfg-next-20261007。只改VFG backend及针对性测试/构建/诊断/本地候选。DLSS/XeSS/FSR、NR算法、显示策略、时钟和用户配置不变；不派Agent、不做压力/竞争负载、不merge/push/Release/关机。

1. 独立start.json与source-before.bundle保护34既有工作树、495脏文件及3691项发布原件/证据；每轮运行scripts/acceptance/vfg-next-control.py，不改旧guard或baseline。
2. 用原GTA6 4K30、原版单层1080NR/强度1/零运动/调控关、实际GPU普通、Auto同步/限帧关，复测Medium6、High3和Medium4。记录软件提交FPS、GPU各阶段、降档及错误，不把提交FPS当物理显示/端到端延迟。
3. 假说：同一输入对的每个插帧都重复producer signal/CUDA wait，导致D3D消费拷贝与下一张CUDA工作串行。尝试每对输入只同步一次；保留逐输出completion wait/独立槽/租约/有界worker/失败排空。capture revision、parity、reset、sub1都必须重建依赖。环境变量仅用于同EXE旧模式对照，不改变用户设置。
4. 必须与原2.0.6及同EXE旧模式相邻对照，8/10-bit全部质量/2X–8X像素逐位核对，重复输入地址更新、反向parity/reset、中途起始subframe、失败回退、退出/seek/切换和完整导出计数。实测无收益或画面变化则撤回产品改动并保留报告。
5. 只有证据支持才处理额外VFG专属瓶颈；禁止靠放宽过期门槛、丢更多帧、降低画质或放慢时钟伪造收益。所有产物E:/项目/Veyra下同名任务目录，测试每进程≤300秒、构建≤900秒。正常GPU串行；已有SDK/DLL/模型原字节保持且不进源码Git。

当前：开工保护完成，优化尚未实现，收益待测。NVIDIA显存增长及其它硬件长稳不由本轮短测视为解决。
