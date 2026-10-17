# #48 网络播放稳定性与服务器视频缓冲：实施方案与记录

日期：2026-10-17　分支：`claude/network-buffer-48-20261017`（基线 main `26c0d61`）　作者：Claude（子会话）

授权与范围见 `AGENTS.md` 顶部 2026-10-17 “#48 网络稳定与服务器缓冲授权”。用户原话：“http点播你随便找个Twitch的试试吧，我也没试过这玩意，然后服务器视频也可能是他的服务器速率太慢了，跑那种超大MKV根本跑不动，我自己手里也没有服务器不能测试，所以你就看着弄吧……开工吧”。#48 报错原文：`The source video contains corrupt data; processing stopped`（`源视频包含损坏数据，已停止处理`）。

存档：`checkpoint/pre-claude-network-buffer-48-20261017`（main 26c0d61）；`E:/项目/Veyra/archives/claude-network-buffer-48-20261017/{source-before.bundle,start.json}`（全引用 bundle 已 verify，75,282,603 字节，记录 88 个工作树 / 108 个脏文件）。产物目录统一 `E:/项目/Veyra/{build,tests,logs,tmp,reports,archives}/claude-network-buffer-48-20261017`。

---

## 1. 现状核对（main 26c0d61 实际代码）

| 事实 | 位置 |
| --- | --- |
| 损坏包 / 损坏帧 / 读包失败一律停止 | `src/source/MediaFileSource.cpp` `read()`：`AV_PKT_FLAG_CORRUPT` → “源视频包含损坏数据，已停止处理”；解码帧 `AV_FRAME_FLAG_CORRUPT`/`decode_error_flags` → “源视频包含损坏画面”；`readVideoPacket` 失败 → “源视频读取失败” |
| 网络期限 | `include/veyra/media/NetworkInput.h`：`rw_timeout`/`timeout`=5 s，`InputInterrupt::arm()` 每次操作 5 s；打开、探测、读包共用 |
| 视频、音频各开一个 `avformat` 连接 | 视频 `FFmpegDemuxer::open`（同步读，丢弃非视频包）；音频 `AudioPipeline::open`（自带读包线程与 2.5 秒包队列） |
| 内嵌字幕扫描 | `SubtitleLoader` 整流读取；**网络地址不扫描**（`QmlPlayerBridge::openPath` 对网络传空路径），UNC/映射盘按本地文件整文件扫描 |
| 直播暂停恢复 | `QmlPlayerBridge::togglePlayPause`：`liveKind=="url"` 且时长未知时恢复 = 重新打开地址 |
| 引擎读帧 | `EngineController::run`：首帧预读 `read()` 只接受 Frame/Error；主循环 `Waiting` 路径对所有源通用（`advanceLive(); waitLive(); continue;`） |
| 音频主时钟 | 文件播放以 WASAPI 设备时钟为主；断粮补静音且静音计入媒体时间 |

计划文档 §1.2 “字幕加载器对 http/https/rtsp 也会再开第三个连接”与实际不符：网络地址已不扫描字幕（见上表）。C5 的实质是 NAS 路径与网络点播的内嵌字幕获取方式。

## 2. 与计划的差异及理由（实现细节按代码实际调整）

计划 C4 写成两套机制：点播/NAS 用“内存字节缓存 + 每个读者一个自定义 AVIO”，直播用“单解复用线程 + 包队列”。核对代码后改为**一套**：

- **网络与 NAS 输入统一走“共享解复用会话”（`media::DemuxSession`）**：一个 `avformat` 上下文、一个解复用线程，按流把包放进有界队列；视频（`FFmpegDemuxer`）和音频（`AudioPipeline`）都从该会话取包。点播与直播不再区分机制，只是点播额外支持 seek。
- 理由：
  1. 字节缓存只能挂在“顶层 URL 的字节流”上。Twitch 回放等 HLS 点播的分片由 HLS 解复用器内部开连接，字节缓存无法让两个 `avformat` 共用分片下载（带宽仍翻倍）；RTSP 同理。包级共享对 HTTP 文件、HLS、RTSP、HTTP-TS 全部有效，保证“每会话一个连接”。
  2. 包队列天然给出“缓冲秒数”“下载速度”“码率”“暂停缓冲”的判据（对应 mpv 的 demuxer cache + cache-pause），比字节窗口更直接。
  3. 重连时队列里已有的包仍可播放，足够长的缓冲可完全掩盖短断流；点播重连后按“最后入队包的 DTS”续读并去重，解码器无需重置。
  4. 本地 SSD 文件完全不进会话，保持 2.0.9 的直读路径逐字节不变。
- 未实现 / 推迟：字节级回看窗口（小幅回退 seek 仍重新定位底层连接，与 2.0.9 相同）；记入“已知限制”。
- 会话使用范围：`http/https/rtsp` 地址，UNC 路径（`\\server\share\…`，不含 `\\?\C:\` 本地扩展路径）与 `GetDriveType==DRIVE_REMOTE` 的映射盘。测试开关 `VEYRA_TEST_FORCE_DEMUX_SESSION=1` 可让本地文件也走会话，用于与直读路径逐帧对照。
- 测试用慢速 IO：`VEYRA_TEST_SLOW_IO=<MB/s>[,<ms 延迟>]` 在会话的自定义文件 AVIO 上限速并加读延迟，模拟 NAS；不新建系统共享。

## 3. 设计

### 3.1 新增 / 修改的代码

| 文件 | 变更 |
| --- | --- |
| `include/veyra/media/NetworkInput.h` | 拆分连接/读超时；`NetworkSettings`（档位、起播预缓冲、自动重连、损坏画面策略）；`NetworkStats`；`remoteMediaPath()`（UNC/映射盘）；HTTP `reconnect*` 选项 |
| `include/veyra/media/DemuxSession.h`、`src/media/DemuxSession.cpp`（新） | 共享解复用会话：线程、包队列、seek、重连、码率与下载速度测算、缓冲状态机、慢速 IO 测试文件 AVIO |
| `src/media/FFmpegDemuxer.cpp`、`include/…/FFmpegDemuxer.h` | 会话模式：流参数读自会话上下文，`readVideoPacket` 取队列包，新增非阻塞结果 |
| `src/source/MediaFileSource.cpp`、`include/…/MediaFileSource.h` | 网络容错（C1）、会话接入、`Waiting`、重连不连续标记、统计出口 |
| `include/veyra/source/IFrameSource.h` | `SourceOpenDesc` 增加 `NetworkSettings`、`nonBlocking`、`tolerateCorruption` |
| `src/sink/WasapiAudioSink.cpp`、`include/…/WasapiAudioSink.h` | `AudioPipeline::open(session)`：从会话音频队列取包；网络缓冲暂停时挂起端点（时钟冻结） |
| `src/engine/EngineController.cpp`、`include/…/EngineController.h` | 首帧预读等待缓冲；`Waiting` 状态文案；`PlayerSnapshot::network`；`PlayerOptions::network` |
| `src/engine/Subtitles.cpp` 等 | C5：NAS 扫描延迟与限速 |
| `src/ui/QmlPlayerBridge.cpp`、`include/…/QmlPlayerBridge.h`、`qml/Veyra/SettingsPage.qml`、`qml/Veyra/ProPage.qml`、`i18n/catalog.json` | C6：设置项、信号面板“网络”行、四语翻译 |
| `tests/network/slow_server.py`、`tests/integration/NetworkBufferTests.cpp`（新） | 本地测试服务端与回归 |

### 3.2 解复用会话

- 解复用线程循环：`av_read_frame` → 视频包进视频队列，所选音轨的包进音频队列，字幕包进（有上限的）字幕旁路，其它丢弃。每次读包前设读期限，打开/探测用连接期限；用户停止或切源（`cancelled`）、会话销毁、seek 请求都会立即打断阻塞读。
- 队列上限：软目标 S（档位：小 8 s / 中·自动 30 s / 大 120 s；直播抖动缓冲 0.5 / 1 / 1.5 / 3 s），按“最短一路相对播放位置的前瞻”判断；同时有字节硬上限（64 / 256 / 1024 MB）取小。任一路前瞻 < S 就继续读，另一路最多超出到硬上限，避免交错偏移饿死另一路。
- 播放位置由引擎每轮写入（音频时钟或最后呈现 PTS）；起播/seek 后的前瞻 = 队列跨度。
- 码率估计：容器 `bit_rate` 或队列字节 / PTS 跨度（≥3 s 样本后取实测）；下载速度 = 解复用线程“读包中”的字节 / 读包耗时（EMA），另报 2 秒窗口平均。
- 状态机：`Prebuffering`（起播或 seek 后，前瞻 ≥ 起播目标或 EOF 才放行）→ `Playing`（前瞻 < 低水位 0.25 s 且未 EOF → `Buffering`）→ `Buffering`（前瞻 ≥ 恢复目标或 EOF → `Playing`）；`Reconnecting` 叠加在读线程上，消费方继续用队列里的包，队列见底才进入 `Buffering`。
- 自适应恢复目标（C7）：持续（≥5 s 读包时间）下载速度 < 码率 × 0.95 时置 `bandwidthLow`，恢复目标 = clamp(60 × (1 − 速度/码率), 起播目标, 队列上限)，使每次放行至少连续播放约一分钟；界面提示“服务器速度 x MB/s 低于视频码率 y MB/s，建议加大缓冲或下载后播放”。绝不降速播放、不报错黑屏。
- 暂停缓冲：会话告知 `MediaFileSource::read()` 返回 `Waiting`（引擎主循环已按活源处理，画面停在当前帧）；音频线程因会话 hold 而暂停端点，设备时钟冻结，恢复后继续，不丢 PCM、不拿静音冒充同步。

### 3.3 重连（C2）

- 触发：读包返回非 EOF 错误、读期限超时、连续 10 秒只有损坏数据。关闭用户设置“断线自动重连”时直接报错（带原因）。
- 退避 1 / 2 / 4 / 8 s 封顶 8 s，总时长上限 60 s（可测试覆盖）；用户停止立即中断；每次尝试用连接期限 15 s。
- 点播（可 seek 且时长已知）：重新打开后 `av_seek_frame(最后入队视频 DTS, BACKWARD)`，每路丢弃 DTS ≤ 该路最后入队 DTS 的重复包，队列已有的包照常播放，解码器不重置。HLS 点播同理（按分片重新定位）。
- 直播（不可 seek）：重新打开，丢弃首个视频关键帧之前的包，时间戳平移使新连接首个关键帧紧接上一连接结束时间（连续时间线，音视频同一平移量），队列首包带“不连续”标记，解码器清空，帧带 `Discontinuity` 使图历史重置。
- HTTP 同时打开 FFmpeg `reconnect`、`reconnect_streamed`、`reconnect_on_network_error`、`reconnect_delay_max`（内层快速重试），HLS 设 `seg_max_retry`。

### 3.4 损坏容错（C1）

- `SourceOpenDesc::tolerateCorruption`：引擎对网络源与 `mpegts` 容器默认置真；导出、测试工具、`MediaFileSource` 默认值保持严格。
- 损坏包照常送解码器（FFmpeg 自带隐藏/修补）；解码失败的单个包记数后继续；带 `AV_FRAME_FLAG_CORRUPT`/`decode_error_flags` 的帧按“损坏画面”策略：`显示修补结果`（默认）或 `等下一个关键帧`（`recoverAtKeyframe` 丢弃至关键帧）。
- 计数与限频日志（≥1 秒一条）；状态“检测到网络丢包，已自动跳过 n 处”；连续 10 秒无好帧 → C2 重连。

### 3.5 字幕（C5）

- 网络地址：不开第二、三连接。会话把字幕包旁路给字幕加载器（有上限、可丢弃）；点播在起播缓冲达到目标后才读取，不与播放抢带宽。
- NAS / 映射盘：整文件扫描延后到起播缓冲达标之后，限速（受 `VEYRA_TEST_SLOW_IO` 同样影响），播放缓冲低于低水位时让出读取。

### 3.6 设置与状态（C6）

- 设置→播放：`网络缓冲`（自动/关闭/小/中/大，默认自动）、`起播预缓冲`（自动/不等待/3/5/10/20/30 秒）、`断线自动重连`（默认开）、`损坏画面`（显示修补结果/等下一个关键帧）。均为保存的偏好，下次打开地址生效。
- 信号面板（ProPage 的“信号”卡）新增“网络”行：`缓冲 18.2 s · 下载 12.4 MB/s · 重连 0`；缓冲中显示进度而不是黑屏报错。四语翻译进 `i18n/catalog.json`。

## 4. 测试计划与验收（每类只测一次，不做压力测试）

测试服务端 `tests/network/slow_server.py`（Python 标准库）：Range、限速（低于/高于码率两档）、定时卡顿 8–12 s、中途断开、按比例破坏 TS 包、连接数统计。素材：本机已有高码率大文件（≥60 Mbps 4K，不改原件）。

| 指标 | 方法 |
| --- | --- |
| 1% TS 丢包播放 10 分钟不停止 | 服务端按 1% 破坏 TS 包；“10 分钟”用环回加速读取（按内容时间 10 分钟计）并如实说明 |
| 8 秒断流后 5 秒内恢复 | 服务端卡顿/断开 8 s，量测恢复出画时间 |
| 点播每会话连接数 = 1 | 服务端连接计数 |
| 缓存不超上限 | 会话字节统计 |
| 音画偏差 < 40 ms | 引擎遥测 / 音频时钟与视频 PTS 对照 |
| 带宽低于码率 | 限速低于码率档：进入暂停缓冲，提示含原因，无报错 |
| RTSP 单会话 | mediamtx 连接计数 |
| 本地文件回放与 2.0.9 一致 | 同一素材：直读路径输出与 2.0.9 构建逐帧 PTS/像素 SHA 对照；强制会话路径与直读路径对照 |
| 真实 HTTP 点播（用户指定） | Twitch 回放 HLS / 剪辑 MP4 在 Veyra 内试播；令牌脱敏；拿不到则改用 Mux/Apple HLS 测试流与 W3C Sintel，如实记录 |

真实 NAS 与用户的直播源本机无法验证，列为现场待验。

## 5. 进度记录

（随实施逐项更新；详细命令、日志路径见 `docs/WORKLOG.md` 与 `reports/claude-network-buffer-48-20261017`。）

- [x] 存档、AGENTS.md 授权、方案文档（本文件）
