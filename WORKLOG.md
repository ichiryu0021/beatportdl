# BPDL 二次开发工作记录（AI 交接文档）

> 最后更新：2026-11-21
> 本次主线：**Qt6/C++ 桌面端 → WebView2（Chromium 内核）桌面壳**，界面整体重做；
> Go 下载后端保持原样（本机无 MSYS2/CGO 工具链，未重编）。
> 上游通用说明仍以 [README.md](README.md) 为准，本文件只记录二次开发内容。

## 1. 架构现状

```
用户双击  beatportdl_gui.exe  (Go + WebView2 壳, 约 7.6 MB)
   │
   ├─ Win32 原生窗口（暗色标题栏、DPI 感知、单实例、窗口位置记忆）
   ├─ WebView2 控件（系统 Edge Chromium 运行时，EdgeWebView 157.x）
   │    └─ 内部虚拟站点 https://bpdl.app/  ⇐ 资源来自 <exe>\data\ui
   ├─ postMessage 消息桥（进程内，不走端口、不走 fetch）
   │    ├─ rpc  : 由宿主进程 POST 127.0.0.1:8765/rpc（复用现有后端）
   │    └─ host : 选择目录 / 打开文件 / 资源管理器定位 / 诊断/重启后端
   └─ 后端守护：探测 /health，必要时无窗口拉起 beatportdl.exe --serve
```

关键点：

- **界面没有 URL 端口**。UI 由 WebView2 的虚拟主机映射（`SetVirtualHostNameToFolderMapping`）
  以 `https://bpdl.app/` 载入，前端与宿主通过 `window.chrome.webview.postMessage` 通信。
- **后端仍是原来的进程**：`beatportdl.exe --serve`，只监听 `127.0.0.1:8765`，
  提供已有的 JSON-RPC（`POST /rpc`）、SSE（`GET /events`）与 `/health`。
  这是目前唯一的回环 HTTP 连接，由宿主进程发起，页面本身不接触网络。
- 宿主退出会 `Kill` 自己拉起的后端；同时用 Job Object（`KILL_ON_JOB_CLOSE`）兜底，
  避免宿主崩溃后残留后端占用 8765。
- 单实例：命名互斥量 `Local\BeatportDL.Desktop.SingleInstance`，重复启动会把已有窗口置前。

## 2. 新增/修改的文件

新增（Go 桌面壳）：

| 路径 | 说明 |
|---|---|
| `cmd/bpdl-ui/main.go` | 入口：DPI、COM/STA（`runtime.LockOSThread`）、单实例、错误弹窗 |
| `cmd/bpdl-ui/app.manifest` / `versioninfo.json` / `resource_windows_amd64.syso` | 清单（PerMonitorV2）、版本信息、exe 图标资源 |
| `internal/desktop/` | 宿主逻辑：窗口编排、后端守护、RPC/SSE 客户端、消息桥、资源解包、性能模式 |
| `internal/desktop/update.go` | GitHub Release 检查、zip 下载/校验、备份、更新 helper、自动重启 |
| `internal/desktop/update_test.go` | 更新器纯逻辑单测（版本/资产/白名单/zip 安全/清单） |
| `internal/desktop/catalog.go` | 读取缓存 OAuth token，拉取 Beatport 子曲风表并按 `sub_genre_id` 查询最新曲目 |
| `internal/desktop/catalog_test.go` | 子曲风条目转换与数字解析单测 |
| `internal/desktop/scheduler.go` | 宿主侧定时任务：多曲风 + 多子曲风扫描并投递下载队列 |
| `internal/desktop/web/mecha.css` | 机能 · 工业终端主题覆盖层（默认主题） |
| `internal/winui/` | Win32 窗口、暗色标题栏/DWM、IFileOpenDialog、ShellExecute、Job、单实例 |
| `internal/webview2/` | 从 `jchv/go-webview2`（MIT）裁剪适配的 WebView2 COM 层；新增虚拟主机、消息桥、DPI、导航策略等 |
| `internal/webview2/loader/` | 纯 Go 动态加载 `WebView2Loader.dll`（无 CGO） |
| `third_party/webview2/` | `WebView2Loader.dll` x64（NuGet 1.0.4370.26）+ LICENSE/NOTICE |
| `internal/desktop/web/` | 前端：`index.html` + `app.css` + `app.js`（无第三方依赖） |
| `build-desktop.ps1` | 构建外壳；`-Package` 生成便携 `dist/`；`-Version` 注入版本 |
| `build-update.ps1` | 从 `dist/` 生成 hotfix / full 两个 GitHub Release 资产 |
| `screenshots/desktop-*.png` | 新界面实机截图 |

修改：`run-gui.bat`（改为启动新壳）、`.gitignore`（忽略 `/.tmp-*/`）。

未改：Go 后端（`cmd/beatportdl`、`internal/*`、`config/`）、`gui/` Qt 源码
（保留作回退参考，不再构建/部署）。

## 3. 构建、打包、测试

```powershell
# 仅构建外壳（纯 Go，无 CGO）
.\build-desktop.ps1

# 构建并生成便携目录 dist\（需要 bin\beatportdl.exe 已存在）
.\build-desktop.ps1 -Package

# 发布版：注入版本号（version.txt 记录同一版本）
.\build-desktop.ps1 -Package -Version 1.1.0

# 生成 hotfix / full 两个 Release 资产
.\build-update.ps1 -Version 1.1.0

# 等价的手工命令
go build -trimpath -ldflags "-H windowsgui -s -w" -o bin\bpdl-ui.exe .\cmd\bpdl-ui
```

- 后端仍是 CGO 构建（TagLib），需要 MSYS2 + `setenv.ps1`：
  `go build -o bin\beatportdl.exe .\cmd\beatportdl`。**本机当前没有 MSYS2**，
  本次沿用已有的 `bin\beatportdl.exe`（18 MB，2025 年构建）。
- WebView2 运行时：Windows 10/11 通常自带（本机 `C:\Program Files (x86)\Microsoft\EdgeWebView\Application\157.0.4341.60`）。
  `WebView2Loader.dll` 必须与 exe 同目录；打包脚本会自动复制。
- `resource_windows_amd64.syso` 由 goversioninfo 生成（图标 + 版本信息 + manifest），已提交；
  重新生成时把生成的 `resource_windows_amd64.syso` 放进 `cmd/bpdl-ui/`（其余架构文件删掉）。
- `go vet` 会在 `internal/winui/window.go` 报 2 处 `unsafeptr`（WndProc 的 lparam 转指针），
  这是 Win32 回调的固有限制/误报，`go build` 不受影响。
- 前端自检（无 WebView2 也能跑，mock 桥）：
  ```powershell
  msedge --headless=new --disable-gpu --window-size=1600,1000 `
    --virtual-time-budget=20000 --screenshot=selftest.png `
    "file:///D:/Project/BPDL/internal/desktop/web/index.html?selftest=1"
  ```
  截图左下角应显示 `SELFTEST OK`；`?view=discover|search|scheduler|downloads|settings`
  可直接预览某一页。
- 实机截图（硬件加速下全屏 GDI 截图会发白）：用 `PrintWindow(hwnd, hdc, PW_RENDERFULLCONTENT=2)`。
- 真实联调可用 Windows UIAutomation 点击界面（`AutomationElement.FromHandle` +
  `InvokePattern`），已验证发现页/搜索/设置/定时页均可用；HTML 元素的 `AutomationId`
  就是 DOM id（如 `proxy-input`/`proxy-save`/`genre-sort-save`）。
- 纯 Go 测试：`go test ./internal/desktop ./test/sorting/`（配置写入/代理校验/曲风归档/
  更新器版本与包校验）。

## 4. 桥协议（宿主 ↔ 前端）

页面 → 宿主：`window.chrome.webview.postMessage(JSON.stringify({...}))`

| 字段 | 说明 |
|---|---|
| `{kind:"rpc", id, method, params}` | 调后端 JSON-RPC；宿主返回 `rpc.result` / `rpc.error` |
| `{kind:"host", id, action, ...}` | 原生操作，见下表 |
| `{kind:"ui", action:"ready"}` | 页面就绪；宿主回 `hello` + 当前后端状态并开始转发 SSE |
| `{kind:"ui", action:"log", level, message}` | 前端异常/诊断写入 `logs/gui.log` |
| `{kind:"ui", action:"retryBackend"}` | 重启后端 |

宿主 `host` 动作：`pickFolder`、`openPath`、`openExternal`、`revealPath`、`openLogs`、
`openDataDir`、`backendTail`、`info`、`restartBackend`、`getNetworkSettings`、
`setProxy`、`setGenreSort`、`stopDownloads`、`checkUpdate`、`applyUpdate`。

其中 `getNetworkSettings` 读取配置里的 `proxy` / `sort_by_genre` / `genre_sort_mode`；
`setProxy` / `setGenreSort` 由宿主**直接改写 YAML 配置并重启后端**（不依赖后端 RPC）；
`stopDownloads` 同样是“杀掉后端 → 重新拉起”，用于立即停止整个下载队列。
`checkUpdate` / `applyUpdate` 走 GitHub Release 更新器，进度通过 `{kind:"update"}` 消息推送。
`getGenreCatalog` / `getLatestBySubgenres` 由宿主读取本地 OAuth token 直连 Beatport
子曲风接口；token 不返回前端。子曲风筛选使用 `sub_genre_id`，不经过旧后端的单曲风参数。

宿主 → 页面：

- `{kind:"hello", data:{appVersion, webviewVersion, backendAddr, username, performanceMode, …}}`
- `{kind:"backend", state:"ready|starting|down", message}`
- `{kind:"events.status", connected, message}`
- `{kind:"events.batch", events:[SSE payload, …]}`（70 ms 合批，最多 200 条）
- `{kind:"update", state, percent, stage, message, data}`（检查/下载/安装/重启）
- `{kind:"rpc.result"/"rpc.error"/"host.result"/"host.error", id, …}`

后端 SSE 事件形状未改动：`download.started/progress/done/error/skipped`、`log`、
`scheduler.found`、`auth.loggedOut`。**下载事件键仍必须是用户提交的原始 URL**
（见旧版本 §4.6 的坑：API URL 与 StoreUrl 不一致会导致两行任务）。

## 5. 界面功能（全中文）

- 左侧导航：发现最新 / 搜索 / 定时任务 / 下载队列 / 设置；可收起为图标栏（Ctrl+B）。
- 顶栏：启动异步检查 GitHub 最新 Release；发现新版本时右上角显示
  「新版本 vX」药丸，点击查看更新说明并一键更新。
- 发现：曲风多选（显示名，支持搜索）、曲目/专辑、日期预设（最新/一周/一月/半年/自定义/今日）、
  无限滚动、虚拟列表、全选/清空/Shift 连选、底部悬浮“加入下载队列”。
- 搜索：关键词搜索曲目/专辑/厂牌，空状态提供快捷词；粘贴 Beatport 链接直接入队。
- 定时任务：1 天/1 周/1 月/3 月间隔、限定曲风、启动/停止、实时日志。
- 下载队列：统计卡、状态筛选、**全选/勾选、暂停队列、继续、停止全部、删除所选**；
  真实进度/速度（原地更新不重排）、失败原因、跳过原因、完成后资源管理器定位、
  重试、打开浏览器；已结束记录存 localStorage（最多 400 条），后端队列仍是内存态。
  - UI 侧本地调度：只向仍无取消 API 的后端提交最多约 10 首；其余在本地 `waiting`。
    因此暂停/删除对“尚未提交”的任务是真实有效的；已提交的下载中任务无法单条撤销，
    只能“停止全部”（杀后端重启，可能留下半文件，UI 会二次确认并提示）。
  - 队列顺序按“加入序号 seq”稳定排序：进行中/排队在前，已结束在后；进度事件绝不改变顺序。
    行内容在进度更新时只原地修改进度条/文本，不重建 DOM，避免进度条重置与视觉跳动。
- 设置：音频格式（4 档）、已存在文件策略、文件名勾选（序号/艺人/标题/版本）+ 实时示例、
  下载目录（原生选择器/打开）、**网络代理**（http/https/socks5，写配置并重启后端；
  同时注入 HTTP(S)_PROXY 给后端的下载请求）、**曲风分类**（启用/主曲风/子曲风优先，
  写 `sort_by_genre`/`genre_sort_mode` 并重启后端）、**软件更新**（GitHub 检查/立即更新/
  发布页）、账号登录/退出、后端诊断、关于。
  - 曲风分类只影响**之后完成**的下载；已经平铺在下载目录根部的旧文件不会自动移动。
    后端启动时读取该配置，因此修改后必须重启后端才生效（UI 已自动处理）。
- 登录：启动时 `authStatus` 未认证则弹登录框；用户名保留在配置里并显示于侧栏
  （宿主只读取 `username:`，不读密码），密码只写入配置文件。
- 键盘：`Ctrl+K` / `/` 搜索、`Ctrl+B` 收起侧栏、`Ctrl+A` 全选结果或下载行、`Esc` 关闭弹层/清空选择、
  `Enter` 搜索。

## 6. 性能相关的设计

- **无 UI 端口**：虚拟主机 + postMessage，页面不发 HTTP 请求（也就没有 CORS 问题）。
- 虚拟列表：只渲染可视区 ±5 行；数据/选择变化同步重排，滚动用 rAF 节流。
- SSE 合批：宿主 70 ms 聚合事件后再发；前端每帧合并渲染（`markDirty` + `requestAnimationFrame`）。
- 下载列表稳定排序：`seq` 固定；进度/`updatedAt` 不参与排序；行内进度条原地更新。
- 进度写入 localStorage 做 900 ms 防抖；topbar 按签名缓存，避免每批进度重建 DOM。
- 选中数 O(1)；虚拟列表渲染同步，避免“数据到了但列表短暂空白”。
- WebView2 浏览器参数在启动时设置：
  - 默认 GPU：`--no-first-run --disable-background-networking --disable-component-update
    --disable-sync --disable-extensions --disable-breakpad --js-flags=--max-old-space-size=128
    --disk-cache-size=33554432`
  - 物理内存 < 9 GB 或 `--low-memory`：追加 `--disable-gpu --enable-low-end-device-mode
    --disk-cache-size=8388608`（软件渲染，内存明显降低）。
  - 若环境已有 `WEBVIEW2_ADDITIONAL_BROWSER_ARGUMENTS`，尊重用户设置（诊断页显示“自定义”）。

本机实测（3440×1440 / 32 GB / EdgeWebView 157）：

| 指标 | WebView2 壳 | 旧 Qt 壳 |
|---|---|---|
| 启动到窗口可见 | 约 0.25–0.35 s | 约 0.3–0.5 s |
| 内存（UI 部分私有） | GPU 模式约 345 MB；`--low-memory` 约 235 MB | 约 43 MB |
| 程序目录 | dist 约 29.2 MB（含后端/DLL） | test-deploy 曾约 100 MB |

> Chromium 多进程本身比 Qt Widgets 占内存，这是内核选择的代价；界面观感、滚动流畅度、
> 列表规模与可维护性明显提升。介意内存时用 `--low-memory` 启动。

## 7. 本版踩过/修好的关键坑

1. **UI 线程必须固定**：WebView2 与 Win32 要求同一 STA 线程。入口已 `runtime.LockOSThread()`；
   去掉后曾出现“窗口不出、导航不触发、进程假死”的间歇性 hang。
2. **窗口尺寸持久化**：保存的是窗口外框尺寸，恢复时必须原样使用，不能再按 client 尺寸加边框，
   否则每次启动窗口都会变大。
3. **全屏 GDI 截图发白**：WebView2 硬件合成下 `CopyFromScreen` 可能抓不到内容，改用
   `PrintWindow(..., 2)`；这不是界面 bug。
4. **艺人是数组**：后端 `artists` 返回 `[{name:"A, B"}]`，前端需 `joinArtists` 展开去重，
   否则显示 `[object Object]`。
5. **专辑/厂牌链接不要建乐观行**：后端展开子曲目后用 StoreUrl 发事件，父级 URL 永远等不到
   更新；仅 `*/track/*` 或 `*/catalog/tracks/*` 才预建“排队中”行。
6. **旧 Qt 部署目录**：`test-deploy/legacy-qt/` 放旧 Qt exe + Qt DLL/插件，可回退；
   `test-deploy/` 根目录现在放新壳 + 后端 + TagLib DLL + `WebView2Loader.dll`。
   旧 Qt 需要的 `libgcc/libstdc++/libwinpthread` 已复制一份到 `legacy-qt/`。
7. **配置自动创建**：`<exe>/beatportdl-config.yml` 不存在时，宿主会写一份最小配置
   （下载目录默认 `%USERPROFILE%\Downloads\BeatportDL`），避免隐藏控制台的后端进入
   交互式首次配置而卡死。若 exe 目录不可写，则写 `data/` 并用 `--config` 指定。
8. **后端关闭**：正常退出会显式 Kill 子进程 + Job 兜底；实测关窗后 GUI 与后端均退出。
   强杀宿主时 Job 生效；若用户自己启动的后端则不会动。
9. **原生目录选择器**：`IFileOpenDialog` 放在独立 STA 线程上执行（`internal/winui/dialog.go`），
   避免重入 WebView2 的 COM apartment；实测可正常打开、取消返回 `0x800704C7`（path 为空）、
   宿主日志会打印 `picker:` 诊断行。选择结果经 `setDownloadsDirectory` RPC 立即生效。
   UI 侧 `pickFolder` 通过 `window.Post` 转发并在 goroutine 中等待，不阻塞 WebView2 消息回调。

## 8. 当前部署

- `dist/`：本地清洁运行时 **v1.1.0**：`beatportdl_gui.exe`、`beatportdl.exe`、
  `WebView2Loader.dll`、6 个 TagLib/运行时 DLL、`启动.bat`、`version.txt`；
  **保留已有的** `beatportdl-config.yml`、凭据、`data/`、`logs/`。GitHub Release
  资产由 `build-update.ps1` 生成，资产内不包含这些用户文件。
  首次运行会生成 `data/`（UI 资源、WebView2 用户数据、窗口状态）与 `logs/`。
- `test-deploy/`：已换成新壳，保留用户原来的配置/凭据并恢复自动登录；
  `legacy-qt/` 为旧版（约 75 MB，可整体删除；根目录新壳 + 后端约 29.2 MB）。
- 自检：`?selftest=1` 当前输出 `SELFTEST OK`（mock：发现 60 首 / 勾选 / 入队 / 下载页 5 行 /
  顺序稳定 / 暂停切换 / 停止全部 / 删除所选 / 设置 / 曲风 / 搜索 4 条）；
  真实后端联调（副屏 DISPLAY6）：发现 60 首、搜索 Anyma 返回 21 条、
  设置页读回下载目录与 GPU 模式、原生目录选择器可开可取消；
  代理保存（`http://127.0.0.1:9`）→ 配置 `proxy` 行写入、后端重启日志出现该代理；
  改回直连 → `proxy: ''`、后端重启为“直连”；曲风分类保存 → 后端按
  `sort_by_genre: true / genre_sort_mode: 'genre'` 重启。
- **曲风归档实测通过**：临时把下载目录指到 `dl-test/`、质量切到 AAC 128k 下载 1 首，
  文件最终落在 `dl-test/House/You Lit Me Up (Original Mix).m4a`，后端日志出现
  `filed into genre: House`；测试后已恢复 `downloads_directory: D:\DJMusLib` 与
  `quality: lossless`，并删除测试文件。
- 用户机器 `D:\DJMusLib` 根部的旧 flac 是曲风分类生效前的产物；后端已在 20:55 用
  `sort_by_genre: true` 重启，之后的新下载会进入 `<曲风>/` 子目录，旧文件不会自动移动。
- 配置/凭据都不进 git；**不要把 dist 直接公开**（内含账号凭据缓存）。
- 真实下载链路的最终验证仍建议用户双击 GUI 自己操作一次（特别是曲风分类与暂停/停止）。

## 9. 已知限制 / 后续候选

- 本机无 MSYS2/CGO，后端未重编；若要加封面、BPM、厂牌、发布日期等 RPC 元数据或后端级
  单任务取消，需要先搭回 CGO 工具链。当前列表封面是用 URL 哈希生成的渐变占位，不是真图。
- **单条“下载中”任务无法真正暂停/删除**：后端没有取消接口，宿主也不掌握每首的临时文件路径。
  已提交任务只能“停止全部”（杀后端重启）；等待中的任务可自由暂停/删除。
- 停止全部/改代理/改曲风分类都会重启后端，可能留下下载到一半的文件；UI 有明确确认提示。
- 下载队列仍是后端内存态（重启清空）；界面用 localStorage 保留“已结束”记录用于追溯。
- 曲风分类是后端启动时的配置；已有平铺文件不会自动整理（如需要可另做“按标签整理已有文件”）。
- WebView2 运行时缺失时会弹原生提示并给出安装链接；无法像 Electron 一样自带内核。
- 旧 Qt 源码保留但不再维护；若彻底切换，可后续删除 `gui/`。
- 自动化测试入口：`?selftest=1` 覆盖加载、勾选、入队、切页、顺序稳定、暂停、停止全部、
  删除所选、设置、曲风、搜索（mock）；`?preselect=1&view=downloads` 可预览多选操作栏。

## 10. 用户偏好（沟通与决策）

- 全程中文交流；UI 全简体中文。
- 不主动建文档、不主动 git commit（本次均为明确要求时例外）。
- 面向便携 U 盘部署；网站更新慢，定时类功能用大间隔。
- 需求边界克制：只做被要求的事，不自行扩展。
- 界面/窗口测试尽量放在副屏（DISPLAY6），不打扰主屏使用。

## 11. 自动更新（GitHub Releases）

这一版按 `D:\Project\二游档案\electron\updater.cjs` 的方案移植：启动检查最新 Release、
顶栏出现新版本提示、优先下载最小包、失败可回落完整包、更新前备份、校验后自动重启。
宿主是 Go/WebView2，所以下载/解压/替换用纯 Go 和 `archive/zip` 实现，不依赖 PowerShell。

### 更新源与资产

- 更新源常量：`internal/desktop/update.go` 的 `DefaultUpdateRepo`
  （当前 `ichiryu0021/beatportdl`，即用户有 admin 权限的 fork）。
- API：`https://api.github.com/repos/ichiryu0021/beatportdl/releases/latest`。
- 版本比较使用数字段（`v1.1.0` / `1.1.0` / 旧 `x.y.z` 均兼容）；当前版本由
  `internal/desktop/assets.go` 的 `Version` 提供。
- Release 资产命名固定：
  - 最小包：`bpdl-hotfix-v<version>.zip`（`beatportdl_gui.exe` + `WebView2Loader.dll` + `update.json`）
  - 完整包：`bpdl-v<version>-full.zip`（主程序 + 后端 + 运行库 + `启动.bat` + `update.json`）
  - 完整包**不含** `beatportdl-config.yml`、`beatportdl-credentials.json`、`data/`、`logs/`。
- 更新器优先用 hotfix；hotfix 安装失败且存在完整包时，UI 提示“是否下载完整版”，
  完整包仍只替换程序文件，不动用户数据。

### 运行流程

1. 启动约 0.9 s 后异步 `checkUpdate`，结果只在内存缓存 10 分钟；设置页可手动强制检查。
2. 右上角出现「新版本 vX」药丸，点击打开更新说明弹窗；设置页也有「软件更新」卡片。
3. 点「立即更新」：若本地队列还有未完成任务，先二次确认（更新会中断下载）。
4. 宿主下载 zip（显示百分比/字节），校验 zip 路径、`update.json` 版本、主程序 PE 头/大小。
5. 只把运行库白名单文件复制到隔离的 `stage/`；配置、凭据、`data/`、`logs/` 永远不进入更新包。
6. 备份当前同名运行文件到 `data/backups/update-<version>-<stamp>/`（保留最近 8 份）。
7. 把当前 exe 复制成临时 `bpdl-updater.exe`，以 `--apply-update` 启动；主程序 WM_CLOSE 退出。
8. 更新 helper 等待主进程 PID 退出，重试复制 stage 文件；校验新 exe 后启动新程序。
   复制失败则从备份恢复旧文件并重新启动旧版本；过程写入 `logs/update.log` 与
   `data/update-state.json`。
9. helper 在 `main()` 最前面（COM/WebView2/单实例之前）识别参数，不会打开第二个窗口。

### 构建/发布

```powershell
# 1. 出带版本号的便携包（Version 会通过 -ldflags -X 注入，version.txt 记录）
.\build-desktop.ps1 -Package -Version 1.1.0

# 2. 从 dist 生成两个 Release 资产
.\build-update.ps1 -Version 1.1.0

# 3. 在 GitHub 建 tag v1.1.0 + Release，上传：
#    dist\bpdl-hotfix-v1.1.0.zip
#    dist\bpdl-v1.1.0-full.zip
```

- `build-update.ps1` 会校验 `dist\version.txt` 与 `-Version` 一致，防止 exe 版本和
  Release 版本不一致造成“更新后又提示同一版本”的循环。
- **版本号规则**：只递增最后一位（`1.1.0 → 1.1.1 → 1.1.2`），不自动跳 minor/major；
  需要大版本时由用户明确指定。
- **首次升级必须手动装一次**：当前 v1.0.0 没有更新代码；装上本版（建议 1.1.0）后，
  以后的 v1.1.x 才会自动更新。GitHub 上现有最新 Release 是 v1.0.0（旧 Qt 包，且没有
  BPDL zip 资产），所以在发布 v1.1.0 之前真实界面不会出现提示。
- 预览更新 UI：`index.html?update=1`（mock 返回 v1.1.0）；`&updialog=1` 自动打开弹窗。

### 校验

- 纯 Go 单测：版本比较、资产选择、运行文件白名单、zip 路径穿越、版本清单、updater 参数。
- 更新 helper 已用隔离临时目录实测：替换主程序、保留 config/credentials/data、写
  `update-state.json=ok`、启动新程序均正常。
- 真实 GitHub API 检查已实测（当前 latest=v1.0.0，正确识别为无更新）。
- 本轮已完成 **v1.1.0 清洁打包**：只替换程序运行时，保留 `dist/` 里已有的配置/凭据/
  `data/`/`logs/`；`test-deploy/` 运行文件更新后原配置也保持不动。
  GitHub 发布资产仍明确排除这些用户文件。
- GitHub 侧：按用户确认，保留 `ichiryu0021/beatportdl` 仓库与历史，清理全部旧
  Release/tag，推送新源码，创建 `v1.1.0` Release 并上传
  `bpdl-hotfix-v1.1.0.zip` 与 `bpdl-v1.1.0-full.zip`。
- **端到端实测通过**：在隔离临时目录启动 v1.0.0（带更新器）→ 检查到 v1.1.0 →
  点击更新 → 下载 hotfix → 备份 → 替换 → 自动重启为 v1.1.0；新 exe 的 SHA256 与
  Release hotfix 内的 exe 完全一致，`beatportdl-config.yml` 与
  `beatportdl-credentials.json` 的哈希前后不变。

## 12. 子曲风独立表 / 启动自动刷新曲风

- 旧后端 `getGenres` 只有平铺的 46 个曲风，没有子曲风；而 Beatport catalog API 的
  `/catalog/genres/` 每个 genre 带 `sub_genres`。宿主新增 `catalog.go`：读取本地
  `beatportdl-credentials.json` 的 OAuth `access_token`（不下发前端），按配置代理请求
  API，得到 46 个曲风 / 127 个子曲风（按 genre 关联去重）。
- 新增宿主动作 `getGenreCatalog`、`getLatestBySubgenres`。子曲风筛选直接走 API
  `sub_genre_id`（旧后端只支持 `genre_name`，无法精确表达子曲风）；分页、日期、
  tracks/releases、URL 去重与旧 RPC 返回形状保持一致。
- UI 不做层级树：发现页曲风 picker 增加 `曲风 / 子曲风` 两个 tab；子曲风是
  **独立平铺表**，列头为“子曲风 / 所属曲风”。发现页曲风与子曲风二选一；定时页
  允许同时多选（见第 13 节）。
- 启动曲风加载：空数组/失败不再把 `genresLoaded` 永久设为 true；会按 1.2s/2.4s/3.6s
  自动重试，启动 1.5s 后在后台/登录完成后再强制检查一次。顶栏的手动“刷新曲风”
  按钮已移除，加载完全自动。
- 实测：宿主直连 API 拿到 46 个曲风 / 127 个子曲风；子曲风 `Acid` 通过
  `getLatestBySubgenres` 返回 60 首；真实 UI 中表格、选中、`获取最新` 均正常。
- 本轮只更新了 **本地 v1.1.0**（dist/test-deploy），**没有重新上传 GitHub Release**；
  GitHub 上的 v1.1.0 资产仍是上一版。要发布这版子曲风功能需要重新生成资产并上传新版本。

## 13. 定时任务多曲风 / 子曲风

- 旧后端 scheduler 只接受单个 `genre`，也不认识子曲风，无法直接做多选。新增
  `internal/desktop/scheduler.go`：桌面宿主自己按间隔扫描，选择曲风时调用旧后端
  `getLatest`（支持 `genres` 数组），选择子曲风时调用宿主 `catalog.Latest`
  （`sub_genre_id`），两路结果按 URL 去重后调用后端 `download` 加入队列。
- 宿主 scheduler 支持同时勾选曲风和子曲风，数量不设上限；每次扫描最多取 50 条，
  重复项由后端 `skipped` 处理。日志/发现事件以 `events.batch` 发给前端，复用现有
  `scheduler.found` 与日志区。
- 定时页的 picker 改成与发现页同风格：`曲风 / 子曲风` 两个 tab、独立表格、搜索、
  多选、已选数量；触发按钮显示“N 个曲风 + M 个子曲风”。启动/停止/状态全部走宿主
  `startScheduler` / `stopScheduler` / `schedulerStatus`，不再使用旧后端的单曲风
  scheduler。
- 避免了真实启动验证造成自动下载；已验证 picker 的曲风/子曲风表格、跨 tab 多选和
  “1 个曲风 + 1 个子曲风”标签。宿主 scheduler 编译通过。

## 14. 机能 · 工业终端界面

- 新增 `internal/desktop/web/mecha.css`，默认 `html[data-theme="mecha"]`；经典样式
  仍由 `app.css` 原样承担，主题只覆盖机能态，不重写功能 DOM。
- 侧栏改为编号模块：品牌块 `DL // 001`、`MODULE INDEX`、`01–05` 导航编号、等宽英文
  副标、方形图标框、黄色左标尺、底部 `SYS // LOCAL ARCHIVE`；折叠后为 76px 图标轨。
- 顶栏/页面标题：工业网格、顶部黄黑警示条、页面编号盒（01–05）、等宽 eyebrow、
  无圆角、刻度下沿。
- 卡片：全直角、发丝线、四角 L 形括号、3px 黄色标题标尺；按钮主操作为骨白实底，
  hover 转信号黄；输入框/分段控件/表格/弹窗/日志/Toast 全部直角化。
- 设置页新增「界面主题」卡（机能 / 经典），使用 `bpdl.theme` localStorage，首屏
  inline 读取避免闪主题；`?theme=classic|mecha` 可用于本地预览。
- 已在真实 WebView2 中切换机能/经典并验证显示；未启动任何定时下载测试。
- 本轮仍**没有 push**，GitHub Release 未改动；本地版本仍为 v1.1.0。

## 15. 无原生标题栏 / 侧栏精简 / 设置模块归一化

- **无原生标题栏**：`winui` 主窗口改为 `WS_POPUP | WS_THICKFRAME | ...` + `WS_EX_APPWINDOW`，
  保留系统缩放边框、任务栏和 Alt+Tab，但不绘制原生 caption。页面顶部新增 38px
  `windowbar`：终端标识 + 最小化 / 最大化还原 / 关闭按钮。
- 宿主新增 `windowDrag`、`windowMinimize`、`windowToggleMaximize`、`windowClose`
  四个 bridge 动作；拖动通过 `WM_NCLBUTTONDOWN/HTCAPTION`，拖动、双击、边缘缩放
  由 Win32 处理。关闭仍走 `WM_CLOSE`，保留正在下载时的 close veto。
- **最大化**不用 `ShowWindow(SW_MAXIMIZE)`（popup 下不稳定），改为按 `MonitorFromWindow`
  的工作区 `SetWindowPos`，并记录 normal rect；再次点击还原，关闭时保存正常尺寸与
  maximized 状态。
- **侧栏**去掉 5 个导航图标方框和“收起侧栏”按钮，保留 `01–05` 编号、中文名和等宽
  英文副标；折叠态（Ctrl+B）只显示编号。导航文字不再 `overflow:hidden`，避免中英文
  上下被裁切。
- **按钮**：机能主题主操作从骨白底改为信号黄底 + 深字，包括获取最新、搜索、开始定时、
  保存、下载队列筛选、子曲风优先等；hover 提升到亮黄。
- **设置页**改为单列 1080px 模块轨：所有卡片同宽、内容高度、14px 节奏，没有行洞、
  没有固定高度容器导致的裁切；主题预览、音质选项、segmented、按钮尺寸统一。
- 实测：无原生标题栏启动正常；自定义窗口栏拖动位置变化；最大化到 3840×2160 工作区、
  还原回 1412×958；配置/凭据哈希保持不变。
- 本轮仍**没有 push**，GitHub Release 未改动。

## 16. 机能 · 白灰主题

- 采用 `data-theme="mecha"` + `data-mecha-variant="light"`：深/白共用机能结构，
  白灰只覆盖色板、表面、边框和文字阴影；经典仍是 `data-theme="classic"`。
- 白灰：纸白 `#eeede7`、面板 `#f8f6ef`、黑字 `#10120f`、灰标签 `#5b6158`，
  信号色改为石墨灰 `#4f554f`；主按钮为浅灰底黑字，不再使用黄色。
- 文字浮起：只使用石墨色 drop shadow（不添加白色描边/白边）；正文
  `0 1px 2px rgba(16,18,15,.16)`，标题/导航/账号等
  `0 3px 6px rgba(16,18,15,.28), 0 1px 2px rgba(...,.18)`；主按钮无文字阴影。
- 侧栏字号加大：品牌 18px、导航 14px、后端状态 12.5px、账号名 14.5px。
- 无原生标题栏的黑条：移除顶部警示斜纹，改成 NCCALCSIZE 全客户区 + 6px 主题色
  WebView 内缩边框；DWM 边框/背景刷随主题切换，边缘缩放实测可用。
- 白灰下重绘：工程纸网格、窗口栏、编号侧栏、卡片四角括号、表格、弹层、日志、
  定时任务、设置主题卡；经典主题不受影响。
- 设置页主题卡现在三选：`机能 · 工业终端` / `机能 · 白灰` / `经典 · 深色工作室`；
  `bpdl.theme` 保存 `mecha|mecha-light|classic`，`?theme=light` 可预览。
- 本轮仍**没有 push**，GitHub Release 未改动。


## 17. 发现最新：每页 50/100/200 + 专辑封面

- 后端 `getLatest` 把 `per_page` 限制在 ≤100，无法直接满足 200；发现页改为宿主直连
  Beatport API 的 `getLatestDiscover`：`per_page` 支持 50/100/200，按 genre_id /
  sub_genre_id 过滤，多个筛选分别请求后按 URL 去重；分页、日期与 `has_more` 保持
  原返回形状。
- 宿主把 `release.image.dynamic_uri`（曲目封面）或 release 自身 image 处理成
  `300x300` 的 CDN URL，返回 `cover` 字段；前端 `<img loading="lazy">` 渲染，加载
  失败回退渐变字母。
- 发现页工具栏新增 `每页 50/100/200` 分段控件，选择保存到
  `bpdl.discover.perPage`；下滑加载更多使用同一 `per_page`，切换后自动从第一页重载。
- 真实 API 实测：`per_page=200` 返回 200 条且每首带 cover URL；真实 WebView2 行内
  封面正常显示，200 首分页计数正确。
- 本轮仍**没有 push**，GitHub Release 未改动。

## 18. 最终视觉收尾

- 去掉窗口外圈灰/白边框：WebView2 inset 归零，客户区铺满窗口；边缘缩放改由页面内
  8 个透明 resize handle 触发 `WM_NCLBUTTONDOWN` 原生缩放循环，不再绘制主题边框。
- 侧栏账号信息去掉头像方框和右侧图标，只保留账号名 + 已登录状态；整行可点击进入
  设置/登录，Enter/Space 也可操作。
- 发现页每页 50/100/200 与宿主直连 API 的 200 条分页、专辑封面均在真实 WebView2
  验证；封面为 300×300，懒加载，失败回退字母。
- 本轮仍**没有 push**，GitHub Release 未改动。

## 19. 专辑封面四角括号

- 每个封面加四角 L 形括号，`inset: 2px`；短臂 6px、1px 线。
- 深色主题用信号黄；白灰主题用纸白 `mix-blend-mode: difference`，在深浅封面上自动
  反相成可见的灰/黑线，不再额外加白边。
- 只随封面显示，不改变行高/布局。
- 本轮仍**没有 push**，GitHub Release 未改动。

## 20. 封面四角括号修正为外扩

- 四角括号改为封面外侧 2px：44px 外框内放 40px 封面，括号画在外框四角。
- 深色主题信号黄；白灰主题石墨灰，边框不会被封面图片遮住。
- 本轮仍**没有 push**，GitHub Release 未改动。

## 21. v1.1.1 发布

- 版本号按规则递增到 1.1.1；资源版本、exe 文件版本、`version.txt` 同步为 1.1.1。
- 发布资产：`bpdl-hotfix-v1.1.1.zip`、`bpdl-v1.1.1-full.zip`；不包含配置、凭据、
  `data/`、`logs/`。本地清洁打包保留用户文件。
- 本版包含：机能深/白灰/经典三主题、无原生标题栏与透明边缘缩放、编号侧栏、
  子曲风表、定时任务多曲风/子曲风多选、发现页 50/100/200、真实专辑封面与四角括号、
  启动静默清理封面缓存。

## 22. v1.1.2 弹层遮挡修复

- 修复机能主题下曲风/子曲风弹层被列表行和筛选底栏盖住、背景透出表格的问题。
- 弹层去掉入场透明度动画，改为不透明背景；筛选卡/定时任务卡提升为独立层级，
  弹层始终在表格与底栏之上。
- 版本号递增到 1.1.2。

## 23. v1.1.3 分段高亮与曲风页签修复

- 白灰主题分段按钮反色：选中的一段为纸白底、黑字，未选中的为石墨底、浅字；
  页大小 50/100/200 和曲目/专辑等分段控件统一。
- 页大小按钮扩大触发区域到至少 42×32，所有分段按钮补齐 pointer/touch 行为。
- 修复曲风弹层先切到子曲风、再切回曲风后无法再次切回子曲风：切页签时立即
  renderGenreList，不再依赖 loadSubgenres 的早退。
- 版本号递增到 1.1.3。

## 24. v1.1.4 曲风选择高亮修正

- 撤回上一版对白灰主题外部分段按钮的反色和大触控区改动，恢复原来的浅底/灰选中样式。
- 只修正曲风/子曲风选择弹层：选中行改为纸白底 + 左侧石墨标记，选中勾选框为
  白底深色勾；未选中的勾选框为灰色，选中状态不再比未选状态更暗。
- 版本号递增到 1.1.4。

## 25. v1.1.5 发布

- 版本号 1.1.5；资源版本、exe 文件版本、README、`version.txt` 同步。
- 干净打包：release 资产仅含程序、后端、运行库、启动脚本和 update.json；
  不含配置、凭据、`data/`、`logs/`；本地清洁打包保留这些用户文件。
- 本版包含：发现页专辑类型可用（修复 releases API 的 streaming 参数）、
  白灰主题曲风页签选中态、选择浮条白灰样式与更小主按钮、勾选符号 1px 对齐。

## 26. v1.1.6 发布

- 窗口：最大化状态拖动标题栏自动还原为普通窗口；修复离屏窗口状态导致的
  最大化/还原/缩放失效。
- 文件夹按钮改用 Shell API（SHParseDisplayName + SHOpenFolderAndSelectItems），
  不再错误打开「文档」，并支持曲风子目录和标签改名后的路径回退。
- 下载页改为 42px 矮行：仅文件名、进度条、百分比、速度、状态、文件夹/浏览器。
- 下载页顶部显示总下载速度，行内百分比旁显示当前任务速度，使用机能风数字样式。
- 删除下载页副标题。
- 干净打包：release 资产不含配置、凭据、data、logs。
