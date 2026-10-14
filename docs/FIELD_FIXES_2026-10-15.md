# 现场修复 2026-10-15（Claude）：2.0.7 打开采集卡闪退 + GPU 卡死后条栏图标消失

## 授权与范围

用户（2026-10-15）：“1.现在有个奇怪的bug，而且本机无法复现……他卡了之后，软件未响应，等响应了之后，没有关闭软件，重新采集了一下采集卡，就好了，说非常的丝滑，未响应恢复后的软件条变成了图1这个样子 2.logs(18).zip是另一个问题，用户反馈2.0.7会闪退……这两个问题修复好之后汇报一下，给我电脑关机”。

- 存档：标签 `checkpoint/pre-claude-field-20261015`（main 7571919，即已发布 2.0.7 的产品源码）；`E:/项目/Veyra/archives/claude-field-20261015/{main-7571919.bundle,start.json}`。分支 `claude/field-20261015`。
- 产物：`E:/项目/Veyra/{build,tests,logs,tmp,downloads}/claude-field-20261015`。现场日志按不可信数据处理（单独目录，只读解析）。
- 不 merge/push/Release；不改默认设置（GPU 优先级默认“实时”、NR 默认版本）、运行库、模型、驱动、用户配置；不派子 Agent。

## 问题二：2.0.7 打开采集卡面板后“闪退”（logs(18).zip）

### 证据
- RTX 3080；采集卡 272 个能力项，`GetFrameRateList` 每项返回 120/121 个帧率，合计 15474 个格式。2.0.7 合入的 PR34 按驱动列出全部帧率（2.0.6 每个能力项只有一个），所以只有 2.0.7、只有这类驱动的用户出现。
- 两次会话都停在 `capture-ui formats=15474 selected=false`，没有 `[crash]` 行和转储：不是异常退出，而是界面停止响应后被关闭。
- 本机复现：测试开关 `VEYRA_TEST_CAPTURE_EXTRA_RATES=160` 让 MCS 4K-T911 每项多报 1–160 fps，共 15558 个格式。打开面板后界面心跳停止，`--exit-after` 计时器不再触发，进程 372 MB，105 秒后被强制结束（`logs/claude-field-20261015/repro-160-010844`）。日志同样停在中途，因为卡住的进程不再刷写日志缓冲，用户的日志也是这样截断的。
- 主线程调用栈（dbghelp 采样，`repro-160-010844/ui-thread-stacks.txt`）：`QV4::Sequence::virtualGet → Sequence::virtualMetacall → QmlPlayerBridge::qt_static_metacall → QList<QVariant>::operator=`。Qt 6 里列表属性是“序列引用”，每访问一个元素都会重新读取整个属性；`DialogHost.qml` 的 `veyra.captureFormats.find(...)` 因此每个元素都让 C++ 重建 15k 个 QVariantMap，总量约 2.4 亿次，界面卡死。

### 修复
- `QmlPlayerBridge`：`captureFormats`、分辨率/帧率/像素格式三个选项列表按“生成它们的输入”缓存（格式列表代号、设备、已选分辨率/帧率、界面语言）。缓存不靠 `captureChanged` 清空，因为 QML 绑定先于普通连接收到通知信号。
- 新属性 `captureFormatLabel`、`captureFormatCostHint`；`DialogHost.qml` 不再在 QML 里搜索 `captureFormats`。
- `enumerateFormats` 去重改为哈希集合（原来是 O(n²)）。
- 驱动上报的全部帧率照旧列出，PR34 的行为不变。

### 验证（最终程序）
- 15558 个格式：软件界面与 GPU 界面心跳最大间隔 0.29–0.30 秒（正常抖动）；选 1920×1080 / 60 fps / NV12 分别 73 / 46 / 14 ms，标签与提示正确（`final-160-soft-014019`、`final-160-gpu-014038`）。格式枚举 0.03 秒。
- T911 原本的 378 个格式行为不变（`fix-0-gpu-011620`）。

## 问题一：用久了卡、GPU 卡死后条栏图标消失（veyra-qml(56).log）

### 证据（2.0.6，RTX 4070 SUPER，VC-X7PRO 2560×1440 60，NR SF-v2 + NVOF，GPU 优先级“实时”，D3D12 界面且 NVIDIA 浮窗注入进程）
- 共 9 次 GPU 卡死：引擎 2 秒等不到帧完成（`GPU-frame-ready-timeout`），系统事件 nvlddmkm 153；等 5 秒设备仍报告“存活”，于是会话以“视频队列或呈现失败；GPU 未报告设备移除”结束；约 13 秒后 Windows 才移除设备（0x887A0006 DEVICE_HUNG）。所以每次 `gpu-failure` 都是 `alive=true`，DRED（记录哪段 GPU 工作卡住）从来没写出来。
- 界面线程停 14–17 秒（“未响应”）。D3D12 同一进程同一显卡只有一个设备，界面和视频共用，GPU 卡住时界面一起停。随后 Qt 记录 “Graphics device lost, cleaning up scenegraph and releasing RHI”，在新设备上重建界面。
- 图 1：重建后，所有用 Qt Shape 曲线渲染器画的图标都是空的（Qt 6.8.3），文字和图片正常。本机用测试开关 `VEYRA_TEST_UI_DEVICE_REMOVE` 移除界面设备后复现：首页卡片、条栏、窗口按钮图标全部消失（`uidev-gpu-012720/shot-2.png`）。
- 卡死后会话直接结束，需要手动重连。20:14 那次用户在 20:16:32 自己重连，之后 60 fps、无停顿；10 秒后重启了软件（多半因为条栏坏了）。
- 关于“用久了卡”：刚连接时调度等待 p95≈0、回调到呈现 p95≈10 ms；运行一段时间后等待 p95 升到约 16 ms、回调到呈现约 31 ms，并出现采集丢帧（如 21:03–21:09），约每分钟一次约 50 ms 的引擎停顿；两次等待升高之后都发生了 GPU 卡死（20:16:04、21:25:41）。显存平稳（没有泄漏）。采集卡时间戳相对电脑时钟每分钟漂移 44 ms（卡按 60.00024 标时间，实际约 59.95），不开补帧时按“帧到即出”，不用这个时间戳排期，所以不是原因。20:17–20:54 的 30 fps 是用户自己开的 60→30。**这份日志定不出长时间变卡和 GPU 卡死的根因**，未写成已修复。

### 修复
1. **图标重建**：`main.cpp` 监听每个窗口的 `sceneGraphInvalidated → sceneGraphInitialized`，界面场景图在新设备上重建后，桥接的 `uiDeviceGeneration` 加一；`VIcon` 随之重新设置路径，Qt 重新生成图标几何。
2. **GPU 卡死后自动重连一次**：引擎快照新增 `gpuFailed`。采集卡会话因 GPU 失败结束时，提示后面加“将自动重新连接采集卡一次”，不跳回首页；等这次会话收尾完成再过 3 秒，按“继续上次”重连。若十分钟内又一次 GPU 失败，就不再自动重连（与原来一样交给用户）。期间打开了别的片源就取消。PS5/PC/Xbox 串流与屏幕捕获不变。
3. **收尾时补写 DRED**：GPU 失败当时设备仍“存活”的，收尾时（排空队列之后，最多再等 2 秒）再查一次；设备已被移除就写出 DRED 报告（卡住的命令列表和操作），下次日志能看出卡在 NR、光流还是呈现。

### 验证（最终程序，T911 1920×1080 60 NV12）
- 图标：移除界面设备后图标全部恢复，日志 `ui-device ... generation=1`（`final-uidev-014056/shot-2.png`）；连续两次共享设备丢失（generation 1、2）图标都恢复（`gpu-reconnect-013558/shot-3.png`）。
- 重连：`VEYRA_TEST_GPU_FAILURE=300:6000`（第 300 帧模拟帧完成超时，6 秒后移除设备）：会话 1 失败 → `automatic=true` → 收尾 3 秒后重连 → 会话 2 正常运行；会话 2 再失败 → `automatic=false`（`final-reconnect-014109`）。
- DRED：收尾时检测到移除，记录 `live-presentation-failed-at-teardown alive=false removedReason=0x887A0005`。测试用的 RemoveDevice 本身不产生面包屑（查询返回 0x887A0004），真实的 TDR 会产生。

### 建议（默认值不改，由用户决定）
- 这位用户先换 2.0.7 本测试包：2.0.7 遇到 NVIDIA 浮窗会自动改用软件界面（logs(15) 卡死修复），界面不再和视频共用 D3D12 设备。
- 还卡死的话，可以试 NR 版本换“原版/Lecram”、GPU 优先级改“高”，分别看卡死是否消失；再发日志时会带 DRED 报告。

## 测试开关（只在设置了环境变量时生效）
- `VEYRA_TEST_CAPTURE_EXTRA_RATES=N`：每个能力项多报 1..N fps。
- `VEYRA_TEST_UI_DEVICE_REMOVE=<ms>`：在界面渲染线程移除界面 D3D12 设备一次。
- `VEYRA_TEST_GPU_FAILURE=<帧>:<移除延迟ms>`：直播呈现模拟一次 GPU 帧完成超时，可选延迟移除设备（-1 不移除）。

## 构建与回归
- 构建：`py -3.11 -X utf8 -I E:/项目/Veyra/tmp/claude-field-20261015/build.py`（复用 `build/test-2.0.7-refresh-20261012` 的对象，只编改动闭包），最终日志 `logs/claude-field-20261015/build-20261015-013917`，`veyra_qml_ui.exe` SHA256 `a79714fbcee0c606c45206a5e4a0d7f329ae9a90b3a0a3f8855521492bbf0bef`，显示版本 2.0.7-test-field。
- 回归（`logs/claude-field-20261015/unit-final-013928`）：capture_format_selection 0 失败、repair_contract 259/0、ui_contract PASS、live_timing PASS、present_sink_lifecycle passed=1 debugErrors=0、settings_transaction passed=1 debugErrors=0、qml_quick 68 通过 0 失败。
- 界面脚本：`tmp/claude-field-20261015/{repro_formats.py,gpu_reconnect.py,run_app_prefs.py,stack_sample.py}`。测试前确认没有其他 Veyra 进程。

## 测试包（基于 2.0.7 r2，只替换 veyra_qml_ui.exe、DialogHost.qml、VIcon.qml，不含任何用户设置）
- 目录 `E:/项目/Veyra/test-packages/claude-field-20261015/`（DELIVERY.json，打包脚本 `tmp/claude-field-20261015/package.py`，源码 d2239c6）。
- 覆盖补丁 `Veyra-2.0.7-test-field-patch.zip`（8120432 字节，SHA256 `fa715aad4a4da09247798ef021c74e6227a4091714b7af3df19ef02def3367c1`）：解压到原 2.0.7 文件夹覆盖，NVIDIA/AMD 包通用（两包的 2.0.7 exe 相同），原设置保留。
- 完整包 `Veyra-2.0.7-test-field-NVIDIA-win64-portable.zip`（714770070 字节，SHA256 `8762448fd9ba43cc1311c84d53b10adb1fd163c259b65c1a6ea2503216d8d9c8`）、`Veyra-2.0.7-test-field-AMD-win64-portable.zip`（355706701 字节，SHA256 `5573da62692e6a4fcc23dcbd62c0d30c626803e9fe5ea6c3870f77fd507390c8`）。
- 两个完整包用独立数据目录和日志冷启动通过（退出码 0、0 条错误），包目录未写入任何文件。

## 未验证
- 用户那张 272 能力项的卡和 RTX 3080 没有实机，本机用测试开关造出同量级格式。
- 真实 TDR、4070 SUPER、VC-X7PRO 没有实机；自动重连和 DRED 只用模拟的超时和设备移除验证。
- 长时间变卡与 GPU 卡死的根因未定。
