# Bilibili 播放接入（2026-10-25）

本轮用户要求参考 frostnotfall/BilibiliPotPlayer，在 `D:/Ai_Projects/Veyra-NRVideo-sweel` 增加 Bilibili 播放，并明确选择扫码登录。

开发分支 `codex/bilibili-playback`，基线 `c5253796`。当前请求授权这项功能必要的 UI、网络适配、构建和测试接点；仓库历史其他机器的 UI 冻结、发布授权不作为本轮任务或授权。源码留 D 盘，开发依赖、构建、日志和测试放 `E:/项目/Veyra/{deps,build,tests,logs,tmp,reports}/bilibili-20261025`。未授权推送、合并或发布。

## 接入方案

- 首页 Bilibili 入口；播放地址和拖放入口识别 BV/AV、视频链接、`?p=`、ep/ss 番剧、直播间及 b23.tv 短链。
- 原生 Qt 异步获取 metadata、WBI 签名、账号允许的播放地址；多 P/分集、画质选择；默认选择可用的最高 SDR 画质，HDR/杜比需手动选择。
- 官方二维码生成/轮询接口，二维码由本地 MIT qrcodegen 绘制；关闭窗口停止轮询。登录 Cookie 使用 Windows 当前用户 DPAPI 加密保存到独立应用数据目录；退出清除；凭据和带签名 CDN URL 不写日志。
- 现有发行包 FFmpeg 的 `avformat_configuration()` 显示 `--disable-libxml2`，`av_find_input_format("dash")` 为空、HLS 存在。因此将 Bilibili DASH 的 ISO BMFF SIDX 解析成带 EXT-X-MAP/EXT-X-BYTERANGE 的 HLS VOD 播放列表。音频和视频各自保留原始分段、时长与字节范围；没有转码和完整视频下载。
- 专用 Qt 线程上的 loopback HTTP 服务只代理当前播放会话白名单 URL，补充 Referer/User-Agent；随机会话路径、Host/Origin 检查、连接数/请求头/流量缓冲上限。媒体请求不带账号 Cookie。旧会话在换源/停止时失效。
- 将适配后的媒体交给现有 `openPath` / `DemuxSession`，沿用解码、音频时钟、缓冲、seek、增强和呈现；不修改 GPU 算法与运行库。直播使用官方 HTTP-FLV 流。

## 参考与许可证

- 行为/API 参考：<https://github.com/frostnotfall/BilibiliPotPlayer>，提交 `76b89f0f6a30afc31014a61afbe45c93a6074ef8`。主 AngelScript 播放脚本未找到明确许可证，未复制该脚本、网页浏览脚本、DLL 或资源。新 Qt/C++ 代码为独立实现。播放器嵌入网页、弹幕、推荐流、观看记录上传不属于此次链接播放接入。
- QR Code generator：<https://github.com/nayuki/QR-Code-generator>，提交 `3c6d0b3cefb4e049dc337e82237c9644399716a8`，MIT。仅复制原样 `cpp/qrcodegen.cpp`、`cpp/qrcodegen.hpp`，保留文件内完整许可。
- 访问权限由 Bilibili 返回的账号/地区/会员权限决定，不增加解锁或绕过逻辑。

## 验证

独立 Qt 测试入口：`tests/bilibili/CMakeLists.txt`，无需完整 GPU SDK 即可构建核心解析、网络服务和 UI 模型。下表保留最初的模块与外部读取测试；随后已完成 MSVC 完整 EXE、原生 GPU 播放和跨分段/seek 验证，最终结果见 [完整构建记录](BUILD_BILIBILI_FULL_2026-10-25.md)。

| 检查 | 实际结果 |
| --- | --- |
| Qt 6.8.3 / MinGW 13.1 Debug 模块构建 | `veyra_bilibili`、单元测试、QML 测试、实网 probe 编译链接通过，`--parallel 2` |
| `veyra_bilibili_tests` | 12 passed / 0 failed（含初始化、清理）；覆盖短链/分 P、WBI、SIDX v0/v1/错误边界、CDN 白名单/备用地址、loopback 隔离、取消、权限、缺失剧集、DRM 拒绝、试看/无音轨、模拟直播、模拟扫码确认及真实 DPAPI 保存/恢复/退出 |
| `veyra_bilibili_qml_tests` | 3 passed / 0 failed（含初始化、清理）；真实 QML 面板实例化、分 P 操作、画质和 QR；无 QML warning。系统字体发现有 `FT_New_Face` debug 输出，面板已正常渲染并检查截图 |
| 普通视频 `BV1xx411c7mD` | 游客 480P 档，实际 512×384 H.264 + AAC，2055.27 秒；解析 1.203 秒；FFmpeg 从 30 秒处解码 5 秒音视频，exit 0，stderr 为空 |
| 番剧 `ep84340` / `ss425` | 24 集，游客实际 852×480 H.264 + AAC，1494.993 秒；解析 0.922 秒；同样从 30 秒处实际解码 5 秒音视频通过 |
| 直播 `https://live.bilibili.com/6` | 测试时开播，实际 1280×720 H.264 + AAC；解析 0.390 秒；HTTP-FLV 实际解码 5 秒通过；直播不执行 seek |
| 原 Veyra FFmpeg 兼容性 | 对上述三类真实流分别加载便携包原 `avformat-63.dll`，找到音频/视频并读取 128 包；两类点播各 seek 到 30 秒后继续读包通过。DLL 原件未修改 |
| 官方扫码接口 | 实际 generate + poll 成功；兼容当前 `account.bilibili.com` 与 `passport.bilibili.com` HTTPS QR 地址；本地绘制后等待扫码并取消。未使用用户账号扫码确认 |
| 四语目录 | 新增 54 条繁中/英文/日文翻译；`scripts/i18n/extract.py --check` 显示 missing 全 0、placeholder problems 0 |

证据位于 `E:/项目/Veyra/logs/bilibili-20261025/{unit-tests.txt,qml-tests.txt}` 与 `E:/项目/Veyra/reports/bilibili-20261025/`：`live-smoke.json`（普通视频）、`pgc/live-smoke.json`、`live/live-smoke.json`、`bilibili-panel.png`。面板截图使用测试内容与测试二维码，无真实登录凭据。公开接口与直播状态可能随时间变化，上述为本次执行结果。

### 复现命令

在 PowerShell 中，设定本轮子进程环境；命令均在项目根目录执行：

```powershell
$env:PATH = 'E:\Veyra-bilibili-tools\mingw1310_64\bin;E:\Veyra-bilibili-20261025\deps\bilibili-20261025\qt\6.8.3\mingw_64\bin;D:\ai_tools\notepad4-build\w64devkit\bin;' + $env:PATH
$env:TEMP = 'E:\Veyra-bilibili-20261025\tmp\bilibili-20261025'
$env:TMP = $env:TEMP
$env:QT_QPA_PLATFORM = 'offscreen'
$env:QT_QUICK_BACKEND = 'software'
$env:QT_QPA_FONTDIR = 'C:/Windows/Fonts'
cmake -S tests/bilibili -B E:/Veyra-bilibili-20261025/build/bilibili-20261025 -G Ninja -DCMAKE_CXX_COMPILER=E:/Veyra-bilibili-tools/mingw1310_64/bin/g++.exe -DCMAKE_PREFIX_PATH=E:/Veyra-bilibili-20261025/deps/bilibili-20261025/qt/6.8.3/mingw_64 -DCMAKE_BUILD_TYPE=Debug
cmake --build E:/Veyra-bilibili-20261025/build/bilibili-20261025 --parallel 2
& 'E:/Veyra-bilibili-20261025/build/bilibili-20261025/veyra_bilibili_tests.exe' -o 'E:/Veyra-bilibili-20261025/logs/bilibili-20261025/unit-tests.txt,txt'
& 'E:/Veyra-bilibili-20261025/build/bilibili-20261025/veyra_bilibili_qml_tests.exe' -o 'E:/Veyra-bilibili-20261025/logs/bilibili-20261025/qml-tests.txt,txt'
```

`E:/Veyra-bilibili-20261025` 是指向 `E:/项目/Veyra` 的目录联接。MinGW 链接器在含中文的实际安装路径下失败，因此本轮编译器单独保留在 E 盘 ASCII 路径 `E:/Veyra-bilibili-tools/mingw1310_64`；未修改全局 PATH。构建命令写入 ASCII 路径，产物实际仍在任务目录。

真实流回归：

```powershell
$biliPython = 'C:/Users/HUYEZ/.cache/codex-runtimes/codex-primary-runtime/dependencies/python/python.exe'
& $biliPython -X utf8 tests/bilibili/live_smoke.py --probe E:/Veyra-bilibili-20261025/build/bilibili-20261025/veyra_bilibili_probe.exe --ffmpeg D:/Ai_Projects/.android-tools/ffmpeg/ffmpeg-n9.0-latest-win64-gpl-shared-9.0/bin/ffmpeg.exe --runtime D:/SoftWares_portable/Veyra-2.1.0-test-NVIDIA-win64-portable --output E:/Veyra-bilibili-20261025/reports/bilibili-20261025
# 番剧：同上，加 --video ep84340，输出目录加 /pgc
# 直播：同上，加 --video https://live.bilibili.com/6 --live，输出目录加 /live
& 'E:/Veyra-bilibili-20261025/build/bilibili-20261025/veyra_bilibili_probe.exe' --qr
& $biliPython -B -X utf8 scripts/i18n/extract.py --check
git diff --check
```

### 使用与未验范围

完整项目构建时，启用现有 `VEYRA_BUILD_QML_UI=ON` 即包含 `veyra_bilibili`，依赖 Qt Core/Gui/Network 与 Windows Crypt32；源码携带 MIT QR 生成器，不新增需随包分发的第三方 DLL。随后已补齐 MSVC x64 和完整 Windows/GPU/FFmpeg/串流/采集开发依赖，生成 `2.1.0-bilibili-20261025` NVIDIA 完整功能便携包。原始包未覆盖，构建和最终实测证据见 [完整构建记录](BUILD_BILIBILI_FULL_2026-10-25.md)。

重建后的入口：**首页 → Bilibili → 粘贴链接 → 解析 → 选择分 P/剧集和画质 → 播放**。也可在“播放地址…”或单链接拖放中打开 B 站链接。需要账号权限时点“扫码登录”，使用 Bilibili App 扫码并在手机确认；退出会删除独立加密会话文件。解析页面和二维码关闭后取消未完成的请求，已经开始的播放继续运行；导入本地文件夹不会中断当前 B 站流。

完整播放器的普通视频/番剧/直播硬解、音频输出及原生 GPU 呈现已验证。未验证：用户账号确认登录后的会员高清/4K/HDR/杜比、真实地区限制场景、长期播放/网络故障恢复。账号请求和文件加密已有自动回归，不能据此宣称会员实播通过。签名媒体地址到期后需重新解析；不会自动刷新长期暂停的旧 CDN 地址。当前 PGC 以接口主剧集列表为准，特殊分区剧集不在列表时明确提示。没有弹幕、搜索/推荐、自动连播或观看历史上传。新版独立便携包已包含新功能；原版 EXE 保持不变。

### 开发中修正

- 根据真实二维码响应补充 `account.bilibili.com`，避免旧 host 假设导致生成失败。
- 独立 Python 验证后修正 WBI 测试向量的预期摘要；算法输出与真实接口均通过。
- 显式使用 Qt Quick Controls Basic 的可定制输入框/选择框，消除 Windows 原生样式定制警告。
- 新建播放服务改为首次播放才启动线程/listener；目录导入不释放现有 B 站播放；缺失指定剧集、DRM、音轨损坏均明确报错。

- 完整播放器验证发现同 URI 的 HLS 连续 BYTERANGE 在原 FFmpeg 中首段后停止；每段加入本地独立 URI 标识后，跨分段和 seek 实播通过。完整失败/修复证据见构建记录。
