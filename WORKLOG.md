# BPDL 二次开发工作记录（AI 交接文档）

> 最后更新：2026-09-15
> 本文件供后续接手的 AI / 开发者快速了解本仓库的 Windows 桌面化改造现状。
> 上游通用说明仍以 [README.md](README.md) 为准，本文件只记录二次开发内容。

## 1. 项目性质与架构

基于上游 beatportdl（Git 基线：`878d3f6 feat: interactive filters`）的 **Windows 桌面化二次开发**。
终端用户是中文 DJ，全部界面简体中文，最终形态是便携文件夹（计划 U 盘部署）。

- **后端** `cmd/beatportdl`：Go 1.27 + CGO（TagLib 写音频标签）。
  - CLI 模式保留；新增 `--serve` 模式：本地 HTTP 服务，仅监听 `127.0.0.1:8765`。
  - `POST /rpc`：JSON-RPC 2.0（请求体 `{jsonrpc,id,method,params}`）。
  - `GET /events`：SSE 推送下载进度，payload 形如 `{event,url,title,reason?,percent?...}`。
  - RPC 分发与 App 接口：[internal/server/server.go](internal/server/server.go)；CLI App 的适配实现：[cmd/beatportdl/server_adapter.go](cmd/beatportdl/server_adapter.go)。
- **前端** [gui/](gui)：Qt6 / C++（CMake + MinGW）。
  - [main.cpp](gui/main.cpp)：启动时探测后端，不可达则**无窗口自动拉起**同目录 `beatportdl.exe --serve`，退出时连带关闭。
  - [main_window.cpp](gui/main_window.cpp)：四个页签——发现 / 搜索 / 定时任务 / 下载，外加设置页。
  - [backend_client.cpp](gui/backend_client.cpp)：RPC 与 SSE 客户端。
  - [date_range_widget.cpp](gui/date_range_widget.cpp)：日期预设控件。
- 下载队列纯内存（`pendingURLs`，重启清空），入队自动去重，重复 URL 推 `download.skipped/reason=duplicate`。

## 2. 构建、测试、部署（PowerShell）

每个新 shell 必须先放开执行策略并载入 MSYS2/TagLib 环境：

```powershell
Set-ExecutionPolicy -Scope Process -ExecutionPolicy Bypass -Force
. .\setenv.ps1                                   # MSYS2 mingw64 gcc 16.2.0 + TagLib
go build -o bin\beatportdl.exe .\cmd\beatportdl  # 后端
go test -vet=off -count=1 -timeout 120s ./test/...
$env:Path = "C:\msys64\mingw64\bin;C:\msys64\usr\bin;" + $env:Path
& "C:\Program Files\CMake\bin\cmake.exe" --build gui\build   # 前端
```

部署目录 [test-deploy/](test-deploy)（已 gitignore）：两个 exe + Qt/MinGW DLL + plugins +
`beatportdl-config.yml` + `beatportdl-credentials.json`（均 gitignore，不进库）。
每次改完把 `bin\beatportdl.exe`、`gui\build\beatportdl_gui.exe` 拷进去；
**覆盖前必须先杀掉运行中的 `beatportdl` / `beatportdl_gui` 进程**（exe 占用无法覆盖），
最终由用户自己双击 GUI 启动验证。

调 RPC 的姿势（PowerShell 单引号会剥 JSON，须写临时文件、UTF-8 无 BOM）：

```powershell
$body = @{ jsonrpc='2.0'; id=1; method='getSettings'; params=@{} } | ConvertTo-Json -Compress
$tmp = [System.IO.Path]::GetTempFileName()
[System.IO.File]::WriteAllText($tmp, $body, (New-Object System.Text.UTF8Encoding($false)))
curl.exe -s --noproxy "*" -X POST -H "Content-Type: application/json" --data-binary "@$tmp" http://127.0.0.1:8765/rpc
```

## 3. ⚠️ TRAE 沙箱限制（极其重要，反复踩过）

AI Agent 通过 Shell 启动的**任何**进程（含 GUI 拉起的后端链）写入
`C:\Users\jason\Downloads` 都会被过滤驱动拦截：表现为 `Access is denied` 或留下 **0 字节文件**；
即使 `dangerouslyDisableSandbox: true` 也无效。NTFS ACL 本身正常（用户 FullControl）。

- **工作区目录（BPDL 仓库内）读写完全正常**：后端测试用 `setDownloadsDirectory` RPC
  指到仓库内临时目录（如 `dl-test/`），测完删除并把配置改回真实目录。
- 任何改配置的测试结束后，确认 test-deploy 配置恢复为
  `downloads_directory: C:/Users/jason/Downloads/BeatportDL`。
- **真实下载链路的最终验证只能让用户自己双击 GUI**。Agent 不要尝试替用户点 GUI 下载。
- 历史上的两个 0 字节 flac（panic 中途 `os.Create` 残留）会触发 `track_exists=skip` 误判"已下载"，
  需用户在资源管理器手动删除；截至 2026-09-15 已清理，下载目录内 4 首 FLAC 大小均正常。

## 4. 2026-09-14 ~ 15 已完成改动

### 4.1 GUI 自动托管后端，消除黑控制台（gui/main.cpp）
- `probeBackend()`：GET `/health`，1.5s 超时。
- `locateBackendExe()`：先找 GUI 同目录 `beatportdl.exe`，回退 `../../bin/`。
- `startBackendHidden()`：QProcess + `CREATE_NO_WINDOW`，MergedChannels 重定向到
  `<目录>/logs/backend.log`（Append，启动写分隔行）。
- main()：探测失败→拉起→轮询最多约 15s（进程提前退出则立即失败并弹框指明日志路径）；
  `aboutToQuit` 时只 kill **自己拉起**的后端（用户手动启动的不动）。

### 4.2 日期控件"最新"预设 +【今日】按钮（gui/date_range_widget.*）
- 下拉：最新（默认，不传日期、新到旧）/最近一周/最近一个月/最近半年/自定义范围…
- 非自定义时隐藏起止日期框；`fromIso()/toIso()` 在"最新"返回空串。
- 【今日】按钮一键把起止设为今天（QSignalBlocker 防重复信号）。
- 评估后未做键盘快捷输入：QDateEdit 原生支持逐段输入与分隔符跳段。

### 4.3 曲风过滤修复：必须传显示名而非 slug（gui/main_window.cpp）
- 发现页/定时页 genre combo 的 itemData 存**显示名**（如 `Afro House`，addItem(name, name)），
  不是 slug（`afro-house` 会返回 0 首）。实测 Afro House 50 首。

### 4.4 勾选行蓝色高亮
- 匿名 namespace `setRowChecked()`：勾选时 `setBackground(QColor(72,128,214,72))`，取消清空。
- 已覆盖：追加结果、全选/清空、Shift 连选三处所有 setCheckState。

### 4.5 serve 模式 nil pointer panic 修复
- [cmd/beatportdl/utils.go](cmd/beatportdl/utils.go)：进度条工具函数加
  `if pbPrefix != "" && app.pbp != nil`（serve 模式无 mpb 进度条实例）。
- [cmd/beatportdl/concurrency.go](cmd/beatportdl/concurrency.go)：两处 recover 改为
  打印 `panic recovered: %v` + `runtime/debug.Stack()` 完整堆栈。
- 该 panic 曾在真实下载时中断并留下 0 字节文件（见 §3）。

### 4.6 ★同一首歌下载列表出现两行（一行永远"排队中"、一行"已跳过"）
**根因——事件键不匹配**：
- getLatest/search 返回的 `url` 是 `https://api.beatport.com/v4/catalog/tracks/<ID>/`，
  GUI 用它做下载行键 `m_dlRows` 并原样提交 download RPC。
- 后端 SSE 事件却用 `track.StoreUrl()`（`https://www.beatport.com/track/<slug>/<ID>`）。
- GUI 收到找不到键的事件就 `ensureDownloadRow` 另建一行 → 两行；
  乐观插入的行永远停在"排队中"。
- "自动下载/定时任务加歌"系误解：日志证实只有一次手工 enqueue，无定时任务；
  当前交互是**单击结果行即勾选/取消**（无独立选中态），误点第一行所致。

**修复**（[cmd/beatportdl/downloader.go](cmd/beatportdl/downloader.go)）：
- `saveTrack(track, dir, quality, eventURL string)`：空串才回退 `StoreUrl()`；progress 用 eventURL。
- `handleTrack(track, dir, coverPath, sourceURL string)`：started/error/skipped/done/归档日志全用 eventURL。
- 单曲入口 `handleTrackLink` 传 `link.Original`（即用户提交的 URL，关键）；
  专辑/歌单/厂牌/艺人等子曲目传 `""`（回退 StoreUrl，GUI 对这些 URL 同样以 StoreUrl 建键）。
- 验证：API 形式 URL 提交时，预置 0 字节文件 → started/skipped 键与提交 URL 完全一致；
  正常下载 → started/done 键一致，6.87MB FLAC + 44.1kHz 校验通过。
- **铁律：以后新增任何 download.* 事件，URL 必须沿用"用户提交的原始 URL"，不得自行换 StoreUrl()。**

### 4.7 定时任务间隔改大粒度预设（gui/main_window.cpp 设置/定时页）
- 分钟 QSpinBox → QComboBox：一天 86400 / 一周 604800 / 一个月 2592000 / 三个月 7776000 秒，
  itemData 存秒数，默认"一天"。原因：Beatport 网站更新不快。

### 4.8 文件名命名方式可配置（设置页勾选）
- 后端新增 RPC `setTrackFileTemplate {template}`，并在 `getSettings` 返回 `track_file_template`。
  - 适配层 [server_adapter.go](cmd/beatportdl/server_adapter.go)：
    `TrackFileTemplate()` / `SetTrackFileTemplate()`，保存到配置并即时生效；
    **强制模板含 `{name}`**，否则报错拒绝（防止不同曲目同名互踩）。
- GUI 设置页"文件名："一行四个复选框，从左到右：**序号 / 艺人 / 标题 / 版本**；
  标题恒选且禁用取消；下方实时示例（如
  `示例：01. Conspiracy 23 - Silly Things Intro (Original Mix).flac`）。
  - 拼模板：`[{number}. ][{artists} - ]{name}[ ({mix_name})]`
  - 函数：`buildNameTemplate()` / `applyNameTemplate()`（模板反填勾选，兼容自定义模板）/ `refreshNamePreview()`。
  - 回填期间 `m_namingReady=false` 屏蔽误发 RPC；RPC 失败弹回上一次模板。
- 占位符由下载流程渲染（配置默认模板 `{number}. {artists} - {name} ({mix_name})`，
  定义见 [config/config.go](config/config.go)）。
- 已知权衡：仅 `{name}` 时按曲风平铺目录下同名异作会被 skip 跳过；建议勾"艺人"，
  需要区分同一曲不同混音再勾"版本"。用户当前选择：**仅标题**（见 §6）。

## 5. RPC 方法清单（internal/server/server.go switch）

`search` `getLatest` `getGenres` `download` `setDownloadsDirectory` `getDownloadsDirectory`
`getSettings` `setQuality` `setTrackExists` `setTrackFileTemplate`（本轮新增）
`login` `logout` `authStatus` `startScheduler` `stopScheduler` `schedulerStatus`。

SSE 事件：`download.started/progress/skipped(reason=exists|duplicate)/error/done`。
App 接口新增方法时，同步改 server.go 的 `App` interface 与 server_adapter.go 实现（目前无 mock）。

## 6. 当前运行配置（test-deploy/beatportdl-config.yml，不入库）

`quality=lossless`、`track_exists=skip`、`sort_by_genre=true`、`genre_sort_mode=genre`、
`verify_integrity=true`、`track_file_template='{name}'`、
`downloads_directory=C:/Users/jason/Downloads/BeatportDL`。
后端日志：`test-deploy/logs/backend.log`（GUI 重定向；手工调试可用 bat 把
`exe --serve` 输出重定向，直接 Start-Process 会丢输出）。

## 7. 用户偏好（沟通与决策）

- 全程中文交流；UI 全简体中文。
- 不主动建 md/文档、不主动 git commit（本次均为用户明确要求）。
- 面向便携 U 盘部署；网站更新慢，定时类功能用大间隔。
- 需求边界克制：只做被要求的事，不自行扩展。

## 8. 后续候选 / 留意点

- 单击结果行即勾选的交互容易被误以为"自动下载"，若用户再提可考虑改为复选框热区+独立选中态。
- `{name}` 单段命名的同名冲突目前未实际遇到；真出现时引导勾艺人/版本，或考虑后端自动加序号后缀。
- test-deploy 与 gui/build 已加入 .gitignore，换机器/新克隆需手动准备 DLL 与配置。
- 构建依赖 MSYS2 mingw64 + TagLib + CMake + Go 1.27；环境脚本 [setenv.ps1](setenv.ps1)，
  工具链安装见 [install-build-tools.bat](install-build-tools.bat)，便携启动 [run-gui.bat](run-gui.bat)。
