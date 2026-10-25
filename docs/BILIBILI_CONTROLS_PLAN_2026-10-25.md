# Bilibili 播放栏与直播时移 · 2026-10-25

用户要求：先提交上一版，然后开发播放中选集/清晰度/音轨、B 站服务器直播时移，修复播放前下拉列表不可见，并生成完整功能 EXE。

上一版提交 **8c9ef4393f022015d3f1e7abe36ce9a44e6ed06b** 已推送至用户仓库 `847415681/Veyra-NRVideo-sweel` 的 `codex/bilibili-playback` 分支。
本次开发与本地提交在 `codex/bilibili-controls-timeshift`。版本 `2.1.0-bilibili-controls-20261025`；未合并 main、未推送本次新分支、未发布 GitHub Release。

## 行为与实现

- 极简、全屏和专业模式的播放栏新增选集、当前清晰度；音符菜单显示 API 返回的音轨。只有一个分 P 时选集不可点击；直播不显示选集。
- 点播切换清晰度/音轨恢复进度、播放/暂停状态和倍速；换集从头开始，保持暂停状态和倍速。切换会重新打开流，有短暂加载，不是无缝零等待。
- `BilibiliModel` 分离正在播放的会话与对话框浏览信息。切换请求成功准备后才替换播放连接；失败保持当前会话，取消/停止用请求 epoch 使旧回调失效。Bridge 用引擎 sessionId 限定进度恢复。
- 播放前 ComboBox 已改用项目的 VSelect/VMenu 深色弹层，避免默认浅色背景与浅色文字冲突。全屏新增菜单接入原生覆盖与输入区域；长列表可滚动。
- AAC 音轨按实际码率列出；杜比/FLAC 在接口确实提供时加入，默认优先 AAC。未凭空生成不存在的轨道。非 B 站片源继续使用原有音轨菜单。
- 直播清晰度显示服务器实际 `current_qn`，权限导致降档时提示。账号能否获取高画质由 B 站决定。

## 直播时移协议与边界

验证对象为用户提供的 `https://live.bilibili.com/6`，当时真实房间为 7734200。
协议依据为官方公开播放器 `https://s1.hdslb.com/bfs/blive-engineer/live-web-player/room-player.prod.min.js` 的请求行为，未复制其无独立许可证的实现。

1. `api.live.bilibili.com/xlive/web-room/v2/index/getRoomPlayInfo` 请求 `protocol=0,1`、`format=0,2`、`codec=0,1`、`platform=web`、`ptype=8`；`all_special_types` 包含 19 时继续探测。
2. `api.live.bilibili.com/xlive/web-room/v1/index/getProgressBarInfo?roomid=7734200&platform=web` 返回 `bar_status=ENABLE`、`bar_special_time=3600`、`bar_start_time=0`。以开播时间、允许起始点和服务器窗口限制进度条。
3. fMP4 HLS URL 加 `tmshift=<向后秒数>`；实测 TS HLS 忽略此参数，因此不把 TS/普通 FLV 本地 seek 当成服务器回看。正常直播优先 FLV，回看采用已验证 fMP4。
4. 本地随机会话路径代理 HLS，子清单/分片只允许 B 站 CDN 白名单；清单 URI 改写为 SHA-256 资源键，分片不盲目追加 tmshift。每清单 2 MiB、资源映射 8192 项、并发连接 24、媒体转发缓冲 256 KiB，均有上限；不转发账号 Cookie，不支持加密 HLS。
5. CDN 可能声明一小时窗口，却对部分旧片段返回 404。替换旧会话前检查实际媒体片段；不可用则保留当前播放并提示换时间点或回到直播。不能保证整个声明窗口都有可用片段。
6. 原生直播 PTS 可能从数日后的时间值起步；回看时间用第一帧实际呈现 PTS 建立锚点，忽略启动期零位置，避免回看突然显示在直播端点。

拖动只在释放时提交回看请求。播放栏左侧负时间表示相对直播的落后时长，右侧为“直播”；回看期间显示“回到直播”。不支持服务器回看的房间不开放这条时移进度条。

## 验证结果

最终 EXE SHA-256：`25c12d90248ef431fff00eeedf4da84e13a8e98a0a0656b7e470455ea981484b`。
所有证据根目录：`E:/Veyra-bilibili-20261025/reports/bilibili-controls-20261025`；日志在同根 `logs/bilibili-controls-20261025`。

| 检查 | 结果与证据 |
| --- | --- |
| MSVC 模块测试 | 15 passed / 0 failed；覆盖会话隔离、清晰度/音轨/选集、取消、直播参数、非零 PTS、片段缺失保持原流和 HLS 地址限制 |
| QML 测试 | 3 passed / 0 failed；真实点击播放前选集/清晰度，检查文字可见及正确选中；`panel.png.parts.png`、`panel.png.quality.png` |
| 公开视频 BV1xx411c7mD | `full-player/video/20261025-161356`：480P↔360P、暂停切清晰度和 AAC；暂停位置 16.934 秒保持，1.25 倍速保持；恢复后 22.134 秒，真实呈现累计 345 帧、音频端点打开 |
| 番剧 ep84340 | `full-player/pgc/20261025-161206`：24 集、同样的切换/暂停/倍速；暂停位置 20.812 秒保持，换到第二集从头播放；真实呈现累计 962 帧、音频端点打开 |
| 用户直播间 6 | `full-player/live/20261025-161033`：画质切换；120 秒回看实际落后 124.79 秒，90 秒回看落后 95.62 秒；300 秒分片不可用时原会话继续；回到直播成功，累计 1551 帧、音频端点打开 |
| 原生 UI | `full-player/gui/20261025-161521`：播放前两列表及极简选集/画质/音轨；`20261025-161948`：专业模式三菜单、全屏画质菜单可见，并实际选择 360P，恢复位置 112.779 秒 |
| 直播 UI | `full-player/gui/20261025-162305`：实际鼠标拖动进度条出现约 −2:30 回看、回到直播按钮，点击后实时流恢复 |
| 完整依赖 | 现有全功能 MSVC Release 构建增量编译成功；XeSS/FSR/AMD OF、Magewell/DeckLink、libass、PS5/Moonlight/Xbox 构建宏保持开启；供体 1439 项文件/50 项运行组件哈希一致 |

硬件为 RTX 5070 Ti，驱动 32.0.16.2101。呈现计数与 FPS 是应用提交证据，不是光学显示测量。直播 120/90 秒实际落后差异包含获取和打开流的耗时。
回看流启动日志出现过单次 D3D12VA `send_packet code=-1`，随后 0.4–1.3 秒内获得第一帧并持续硬解呈现；测试没有停止或转入失败状态。记录原始错误，不将通过结果表述为日志零错误。
详细验证索引为 `validation.json`，完整包内复制到 `docs/bilibili-validation.json`。

## 已定位并修正的失败

- 第一轮回看由于启动阶段零位置被当成 PTS 锚点而显示在直播端点；改为等待正的实际呈现位置，模块和完整播放器已复验。
- 5 分钟前清单可获取但分片 404，旧逻辑进入打开超时；新增分片预检并验证保持原流。
- 一轮自动回看验证期间日志出现额外 `ui-menu` 选择 2K/自动，改变了被测会话。加自动验证标题并独立重跑后通过；没有将此干扰误记为产品失败。
- 测试脚本加标题时曾重复定义 Main 的 Component.onCompleted，导致测试实例未启动；改为独立 Timer 后修复。未进入产品 QML。

## 本机命令与交付

```powershell
$biliPython = 'C:/Users/HUYEZ/.cache/codex-runtimes/codex-primary-runtime/dependencies/python/python.exe'
$biliHelper = 'C:/Users/HUYEZ/Documents/ChatGPT/Rea projects/bilibili-dev'
& $biliPython -B -X utf8 "$biliHelper/build_controls.py" configure
& $biliPython -B -X utf8 "$biliHelper/build_controls.py" build
& $biliPython -B -X utf8 "$biliHelper/build_controls.py" tests
& $biliPython -B -X utf8 "$biliHelper/controls_smoke.py" --case live --link https://live.bilibili.com/6
& $biliPython -B -X utf8 "$biliHelper/controls_smoke.py" --case pgc --link ep84340
& $biliPython -B -X utf8 "$biliHelper/controls_smoke.py" --case video --link BV1xx411c7mD
& $biliPython -B -X utf8 "$biliHelper/package_controls.py" stage
& $biliPython -B -X utf8 "$biliHelper/controls_finalize.py"
& $biliPython -B -X utf8 "$biliHelper/package_controls.py" seal
```

构建 -j2、单次 ≤900 秒；测试单进程 ≤300 秒，完整播放器自动退出限制 190 秒。各测试使用独立配置；完整播放器测试共用隔离测试 app，必须顺序运行。
构建器沿用 `build/bilibili-exe-20261025` 和已补齐的依赖，未修改原版/上一版密封包、运行库、模型、驱动或真实用户配置。未派子 Agent。
辅助脚本归档在本报告目录 `local-build-tools/`。`stage` 和 `seal` 拒绝覆盖已存在的目标/ZIP。
最终便携目录及 ZIP 位于 `E:/Veyra-bilibili-20261025/test-packages/bilibili-controls-20261025/`，以 `Veyra-2.1.0-bilibili-controls-20261025-NVIDIA-win64-portable` 命名。
封包检查最终 EXE 与三个真实播放结果哈希一致、QML 与测试源一致、依赖完整、运行库/模型身份未变，并逐文件读取 ZIP 核对 SHA-256。交付索引为 `DELIVERY.json`，另附 Veyra 源码快照与 SHA-256 文件。
包内不包含测试注入、个人配置、Cookie、日志或开发 SDK。源码快照不是完整的公开发行依赖源码集合，本次不创建公开 Release。

## 使用与未验证范围

运行整个便携目录中的 `veyra_qml_ui.exe`；首页 → Bilibili → 解析 → 播放。播放栏中字幕按钮旁为选集/清晰度，音符按钮为音轨。直播回看直接拖动进度条。
本轮以访客公开内容验证。扫码账号会员高清、4K/HDR、杜比/FLAC和多语言轨道仍需实际有权限账号验证；长时间播放和网络故障恢复未覆盖。
完整功能均保留编译及部署，但 PS5/PC/Xbox 串流、实体采集卡、全部增强效果 Create/Evaluate、AMD/Intel 实卡未在本轮逐项实机重验。
