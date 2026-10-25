# Bilibili 完整 Windows 构建（2026-10-25）

用户选择补齐全部依赖，生成完整功能的 NVIDIA 便携版。供体是用户提供的
`D:/SoftWares_portable/Veyra-2.1.0-test-NVIDIA-win64-portable`，源代码位于
`D:/Ai_Projects/Veyra-NRVideo-sweel`，分支 `codex/bilibili-playback`。

## 构建环境

所有依赖、编译产物、测试数据和最终包位于 `E:/项目/Veyra`；ASCII 联接
`E:/Veyra-bilibili-20261025` 指向该目录。只给构建子进程设置 PATH、INCLUDE、LIB、TEMP/TMP，
没有安装显卡驱动或更改系统全局环境。每次构建最多 900 秒、并发 2；测试每次最多 300 秒。

- MSVC 14.44.35207（编译器 19.44.35229）、Windows SDK 10.0.26100.0；官方微软载荷的便携部署。
- Qt 6.8.3 `msvc2022_64`，与原版包匹配。
- vcpkg `30ef65cad98f08e7197c9a1656fbd871bcb72f2d`；`x64-windows-static`，Release、静态 CRT。
- Moonlight `f900dd4767759c7b9d0e93bcea666b55c69ea62f`，包含固定 ENet/nanors 子模块。
- Chiaki `0e16950165f06e5c3291537c2eeba6e852be7120`，完整子模块及仓库已有两个审核补丁；保留独立干净检出用于校验。
- NVENC 头文件 n13.0.19.0；XeSS v3.0.2；FidelityFX 2.3.0；FidelityFX 1.1.4 光流静态库。
- NVOF 5.0 D3D12 头文件来自 `mbucchia/Optical-Flow-SDK` 固定提交
  `54e68293b4898a530bc07e4d7df71efbc5d30f9b`，头文件保留许可，仅供本机编译。
- DeckLink IDL 来自现有 notices 记录的 OBS 提交；Magewell 3.3.1.1596 官方 SDK 仅提取头文件，未安装驱动。
- FFmpeg 开发头文件与导入库放在独立开发 prefix；最终 DLL 使用供体中的 PS5 slices256 / dav1d / zlib 版本及独立 avfilter-12。

依赖根目录：`E:/项目/Veyra/deps/bilibili-exe-20261025`。
主程序构建：`E:/项目/Veyra/build/bilibili-exe-20261025`。
详细日志：`E:/项目/Veyra/logs/bilibili-20261025`。

本机辅助脚本位于 `C:/Users/HUYEZ/Documents/ChatGPT/Rea projects/bilibili-dev/build_exe.py`，
使用 Codex 随附 Python 运行，支持 `deps`、`ffx`、`configure`、`build`、`tests`、`provenance`。
完整配置启用 PS5、Moonlight、Xbox、DLSS NR/SR/FG、NVOF、去隔行和 QML，并提供
XeSS、FidelityFX、libass、Magewell、DeckLink 的实际开发依赖。
`cmake/VeyraShaders.cmake` 支持显式 `VEYRA_DXC_EXECUTABLE`，用于便携 Windows SDK。

## 遇到的问题与修复

- Meson 误选另一套 w64devkit 的 `patch.exe`，运行失败。构建进程 PATH 排除了该目录，
  CMake/Ninja 使用显式路径。
- 自定义 MSVC 工具链未继承 vcpkg 的 CRT 配置，导致六个库以 `/MD` 构建。
  链式加载标准 Windows toolchain 后重建 brotli/bzip2/zlib/libpng/freetype/curl；
  HarfBuzz 的相关 CRT 链接错误随之解决，没有添加忽略冲突或空库。
- FidelityFX 1.1.4 自带 VS generator 平台假设不适用于 Ninja；本机 SDK 的 PIX CMake
  路径改用已计算出的 `FFX_PLATFORM_NAME`，其余功能与着色器保留。

## 打包约束与证据

供体只读。按其文件清单选取运行依赖，不复制个人配置、日志、旧程序、旧应用 QML 或旧着色器。
复用的 1,439 个文件和 50 项运行组件全部与原清单 SHA-256 一致，报告：
`E:/项目/Veyra/reports/bilibili-20261025/donor-audit.json`。
供体 EXE SHA-256：`5b8a8920c4c67a4e1760e26b6c4f197ed532d429cca8effd59526642d7a03a65`。

输出以新的 EXE、当前源码 QML/着色器、许可和新清单组成独立 NVIDIA 便携目录。
SDK、PDB、LIB、用户账号、测试媒体不进入交付包。未授权线上发布，不创建 GitHub Release。

## 完整构建与验收结果

已生成 `2.1.0-bilibili-20261025` MSVC x64 Release 完整功能 EXE。
目录：`E:/Veyra-bilibili-20261025/test-packages/bilibili-2.1.0-20261025/Veyra-2.1.0-bilibili-20261025-NVIDIA-win64-portable`。
EXE：`veyra_qml_ui.exe`，SHA-256：`936b7f04721ff3872e75bc81b08b6e49f7dd25a8ff8a779a76e2c53cf1ea417f`。
同目录上一级保存便携 ZIP、Veyra 应用源码快照、`.sha256` 和 `DELIVERY.json`。
应用源码快照不包含开发 SDK/运行库，也不声称是完整第三方依赖对应源码发行。

实际编译命令记录在 `logs/bilibili-20261025/main-configure-options.json` 与 `main-build*.log`。
首次完整构建 540 个步骤成功，分段兼容修复后增量重建成功。
`reports/bilibili-20261025/build-dependencies.json` 从真实 compile_commands 检查了
XeSS、FSR、AMD OF、Magewell、DeckLink、libass、PS5、Moonlight、Xbox，全部启用。
NR/SR/FG/NVOF/去隔行均保持启用。不是关闭缺失依赖的精简版。

| 最终检查 | 结果 |
| --- | --- |
| MSVC / Qt 6.8.3 模块测试 | 12 passed / 0 failed |
| MSVC / Qt 6.8.3 QML 测试 | 3 passed / 0 failed |
| 完整播放器：BV1xx411c7mD | 512×384 H.264，15 fps，AAC；连续播放超过 20 秒，seek 30 秒后继续至 42.334 秒；呈现 493 个真实帧 |
| 完整播放器：ep84340 | 24 集列表，852×480 H.264，24 fps，AAC；连续播放超过 20 秒，seek 后继续至 42.042 秒；呈现 794 个真实帧 |
| 完整播放器：直播间 6 | 当次 854×480 H.264，30 fps，AAC；实际时间戳推进 20.467 秒；呈现 638 个真实帧 |
| 原生画面与界面 | 番剧实际画面/字幕/播放控件已目视检查；最终便携目录直接启动后，首页 Bilibili 入口、真实链接解析、分 P/画质选择布局正常 |
| DLL 依赖 | dumpbin 直接依赖无缺失；最终 SHA 与验收记录绑定 |

本机是 NVIDIA GeForce RTX 5070 Ti，驱动 32.0.16.2101（621.01）。三类流均实际使用
D3D12VA，日志包含音频 endpoint 打开与呈现计数；不是只读取 metadata。
`submitFps` 是应用呈现提交速率，不冒充面板物理刷新率。两类 VOD 分段播放和 seek 均已跨过首段。
最终证据：`reports/bilibili-20261025/validation.json`；每次测试有独立目录及 EXE SHA。
逐项模块结果：`logs/bilibili-20261025/veyra_bilibili_tests-msvc-result.txt` 与
`veyra_bilibili_qml_tests-msvc-result.txt`。

## 播放兼容修复

完整播放器最初在首个约 5 秒分段后 EOF。以原发行包 FFmpeg DLL 单独读取复现：
默认参数、应用参数和关闭 HTTP 持久连接均只获得 75 个视频包/215 个音频包。
debug 日志显示同 URI 的连续 BYTERANGE 被合并读取。
通过本地转发器让每段 URI 不同后，三组参数都获得至少 375 个视频包和 1,786 个音频包，
PTS 跨越 25 秒；因此给 HLS 每段加入本地 `?segment=<offset>` 标识，网关仍向原 CDN
地址转发同一 Range。未修改 FFmpeg DLL、编解码数据或 GPU 引擎。
失败证据保留在 `reports/bilibili-20261025/full-player/video/` 和 `donor-continuation/debug.log`。
最终三类主程序回归证据在 `validation.json` 指向的带日期子目录。

测试也修正了两个独立问题：`displayFps` 是明确未知的旧字段，不是出帧证据；
改为真实呈现提交计数/媒体位置和引擎日志。直播初始 PTS 可以非零，持续时长按位置差计算。
FidelityFX 1.1.4 shader helper 的输出变量名被错误加入依赖列表，导致 Ninja 缺少
`OPTICALFLOW_PERMUTATION_OUTPUTS`；本机外部 SDK 修正与 PIX 路径修正一起保存在
`reports/bilibili-20261025/fidelityfx-ninja-local.patch`。CMake RC 显式路径统一为正斜杠。

## 本机复现命令

```powershell
$biliPython = 'C:/Users/HUYEZ/.cache/codex-runtimes/codex-primary-runtime/dependencies/python/python.exe'
$biliHelper = 'C:/Users/HUYEZ/Documents/ChatGPT/Rea projects/bilibili-dev'
& $biliPython -B -X utf8 "$biliHelper/build_exe.py" build
& $biliPython -B -X utf8 "$biliHelper/build_exe.py" tests
& $biliPython -B -X utf8 "$biliHelper/full_player_smoke.py" --link BV1xx411c7mD --case video
& $biliPython -B -X utf8 "$biliHelper/full_player_smoke.py" --link ep84340 --case pgc
& $biliPython -B -X utf8 "$biliHelper/full_player_smoke.py" --link https://live.bilibili.com/6 --case live
& $biliPython -B -X utf8 "$biliHelper/final_checks.py" validation
```

辅助脚本归档在 `reports/bilibili-20261025/local-build-tools/`。修改源码再构建后，应把新 EXE
复制到一个新的候选目录并重新验证；上述播放测试默认测试这次固定候选目录。
最终封包命令是 `package_exe.py seal`，仅在验证哈希匹配后制作 ZIP，并逐文件读取 ZIP 核对 SHA。
封包脚本拒绝复用已有 ZIP。开发依赖、构建和日志保留供二次开发，未清理；
占用明细在 `reports/bilibili-20261025/storage-full.json`，逻辑大小含硬链接重复计数。

## 使用与实测边界

完整解压，运行 `veyra_qml_ui.exe`；首页 → Bilibili → 粘贴链接 → 解析 → 选择剧集/画质 → 播放。
配置默认位于便携目录的 `runtime_local/user-data-2.0.0`，这次包内不带个人数据。
扫码会话使用 Windows 当前用户 DPAPI 加密；扫码确认由用户在手机上操作。
未实测真实账号的会员高清/4K/HDR/杜比，未实测真实 PS5/Moonlight/Xbox/采集硬件；
本轮 GPU 测试验证基础硬解/呈现，未对增强运行库执行 Create/Evaluate 回归，不能将编译开启当作逐项实机验收。
AMD/Intel 实卡、长时间播放与网络故障恢复未验。长期暂停导致 CDN 地址过期时需要重新解析。
原供体目录、显卡驱动和用户真实配置未改；源码未提交、未合并或推送，未发布 GitHub Release。
