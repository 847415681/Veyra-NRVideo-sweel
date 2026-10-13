# 更新检查（2026-10-13）

用户要求真正检查更新，每次启动自动检查；有新版本顶部短暂提示，无更新静默。起点 main7571919，隔离 codex/update-check-20261013；70旧工作树/脏文件及增量源码已存档 E:/项目/Veyra/archives/update-check-20261013。

只改 UI 更新服务、bridge、QML、翻译、必要 CMake/验证。无需修改引擎、运行库、模型、用户设置或33ms/250ms刷新节奏。新网络模块由现有 windeployqt 根据 imports 部署。产物全部 E:/项目/Veyra/{build,tests,logs,tmp,reports}/update-check-20261013。不派Agent、不合 main/push/发布或关机。

实现：界面就绪2.5秒后单次异步 GET GitHub /repos/Likely7/Veyra-NRVideo/releases/latest，无凭据；12秒总超时、128KiB响应上限、不重试。只接受非草稿/非预发布的 v数字.数字.数字，数值比较（当前 -test-r2 标识归一同版本），验证链接属于当前仓库正式标签。有更新显示8秒顶部提示，可点击发布页/关闭，设置页保留下载入口。自动无更新/失败均无提示，手动显示最新/失败/可用结果；并发点击合并为一个请求。

验收：独立本地 HTTP 服务覆盖新/同/旧版本、10与9数值比较、预发布/源码归档/无效JSON/外部URL、404/403、总超时、超大响应、自动静默/手动、并发/销毁；真实匿名 HTTPS GitHub 查询验证。Qt Quick 测顶部弹窗显示、8秒关闭、点击/窄屏；完整产品构建和私有配置启动。不得用本地夹具冒充真实远端查询，不生成完整测试包。

参考：GitHub REST releases latest https://docs.github.com/en/rest/releases/releases#get-the-latest-release；Qt6.8 QNetworkAccessManager https://doc.qt.io/qt-6.8/qnetworkaccessmanager.html。
