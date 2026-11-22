# BeatportDL · WebView2 桌面版

**v1.1.4** · Windows 10/11 x64 便携版 · Chromium / WebView2 界面 · 下载队列控制 · 代理 · 曲风分类 · GitHub 自动更新

> 版本号规则：以后只递增最后一位（`1.1.0 → 1.1.1 → 1.1.2`），不自动跳 minor/major。

Beatport & Beatsource downloader (FLAC, AAC)

*Requires an active [Beatport](https://stream.beatport.com/) or [Beatsource](https://stream.beatsource.com/) streaming plan.*

![Screenshot](/screenshots/main.png?raw=true "Screenshot")

## 下载 / Download

从 [最新 Release](https://github.com/ichiryu0021/beatportdl/releases/latest) 下载：

- `bpdl-v1.1.4-full.zip` — 完整便携包（主程序 + 后端 + 运行库），推荐新用户。
- `bpdl-hotfix-v1.1.4.zip` — 最小更新包，供已有 v1.1.0 之后文件结构的客户端使用。

解压到普通文件夹后运行 `启动.bat` 或 `beatportdl_gui.exe`。便携包**不包含**账号、配置、`data/` 或 `logs/`，首次运行按提示登录。

> **升级提示**：v1.0.0 及更早版本没有内置更新器，需要手动安装本版一次；从 v1.1.0 之后发布的版本支持应用内自动更新。

## 功能

- Chromium/WebView2 原生窗口，界面资源走应用内虚拟站点，不额外开放本地 UI 端口。
- 下载队列：勾选/全选、暂停、继续、停止全部、删除所选，稳定排序和原地进度刷新。
- **界面主题**：默认「机能 · 工业终端」（石墨/钢灰 + 柠檬黄信号色、全直角、编号侧栏、工程网格），可切换「机能 · 白灰」（纸白/灰 + 石墨灰信号 + 黑字浮起阴影）或「经典 · 深色工作室」，选择本地保存。
- **无原生标题栏**：窗口栏统一进页面内，支持拖动、最小化、最大化/还原、关闭与边缘缩放。
- **发现最新**：每页可选 `50 / 100 / 200` 首，下滑加载更多按同一页大小递增；曲目名左侧显示真实专辑封面。
- **子曲风单独表格**：发现页曲风与子曲风分开展示，子曲风表包含所属曲风；两者二选一筛选，子曲风通过 Beatport `sub_genre_id` 精确过滤。
- 定时任务使用同样的曲风/子曲风表格，支持两者多选；宿主按选择组合扫描，并把发现的新曲目加入下载队列。
- 启动时曲风加载失败或返回空列表会自动重试，顶栏不再提供“刷新曲风”按钮。
- 网络代理：http / https / socks5，API 与下载请求同时走代理。
- 曲风分类：按主曲风或子曲风优先自动建立文件夹。
- GitHub 自动更新：启动检查 Release，右上角提示新版本，支持 hotfix 与完整包回退。
- 账号登录、定时任务、下载目录、命名模板、已存在文件策略等原有能力。

## 从源码构建

```powershell
# 清洁程序运行时；保留 dist/ 里已有的 config、credentials、data/、logs/
.\build-desktop.ps1 -Package -Version 1.1.0

# 生成 GitHub Release 的 hotfix / full 资产（资产内不含用户配置/凭据/数据）
.\build-update.ps1 -Version 1.1.0
```

- `build-desktop.ps1` 只替换程序文件；如果 `dist/` 正在运行会拒绝打包，避免锁文件导致半成品。
- GitHub 发布包由 `build-update.ps1` 生成，明确排除 `beatportdl-config.yml`、凭据、`data/`、`logs/`。
- `build-desktop.ps1` 依赖 `bin\beatportdl.exe`（CGO/TagLib 后端）；Release 完整包已内置。
- 更新器与发布细节见 [WORKLOG.md](WORKLOG.md)。
- 前端自检：`index.html?selftest=1`（mock 桥）。

## CLI / 后端配置

原始 CLI/JSON-RPC 后端仍保留，可用于无界面运行与脚本调用。

Setup
---
1. [Download](https://github.com/ichiryu0021/beatportdl/releases/latest) or [build](#从源码构建) BeatportDL.

     *The portable Windows desktop build is attached to the Releases page.* \
     *Don't forget to set the execute permission on unix systems, e.g., chmod +x beatportdl-darwin-arm64*

2. Run beatportdl (e.g. `./beatportdl-darwin-arm64`), then specify the:
   - Beatport username
   - Beatport password
   - Downloads directory
   - Audio quality

3. OPTIONAL: Customize a config file. Create a new config file by running:
```shell
./beatportdl
```
This will create a new `beatportdl-config.yml` file. You can put the following options and values into the config file:

---
| Option                        | Default Value                             | Type       | Description                                                                                                                                                                               |
|-------------------------------|-------------------------------------------|------------|-------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------|
| `username`                    |                                           | String     | Beatport username                                                                                                                                                                         |
| `password`                    |                                           | String     | Beatport password                                                                                                                                                                         |
| `quality`                     | lossless                                  | String     | Download quality *(medium-hls, medium, high, lossless)*                                                                                                                                   |
| `show_progress`               | true                                      | Boolean    | Enable progress bars                                                                                                                                                                      |
| `write_error_log`             | false                                     | Boolean    | Write errors to `error.log`                                                                                                                                                               |
| `max_download_workers`        | 15                                        | Integer    | Concurrent download jobs limit                                                                                                                                                            |
| `max_global_workers`          | 15                                        | Integer    | Concurrent global jobs limit                                                                                                                                                              |
| `downloads_directory`         |                                           | String     | Location for the downloads directory                                                                                                                                                      |
| `sort_by_context`             | false                                     | Boolean    | Create a directory for each release, playlist, chart, label, or artist                                                                                                                    |
| `sort_by_label`               | false                                     | Boolean    | Use label names as parent directories for releases (requires `sort_by_context`)                                                                                                           |
| `force_release_directories`   | false                                     | Boolean    | Create release directories inside chart and playlist folders (requires `sort_by_context`)                                                                                                 |
| `track_exists`                | update                                    | String     | Behavior when track file already exists                                                                                                                                                   |
| `track_number_padding`        | 2                                         | Integer    | Track number padding for filenames and tag mappings (when using `track_number_with_padding` or `release_track_count_with_padding`)<br/> Set to 0 for dynamic padding based on track count |
| `cover_size`                  | 1400x1400                                 | String     | Cover art size for `keep_cover` and track metadata (if `fix_tags` is enabled)  *[max: 1400x1400]*                                                                                         |
| `keep_cover`                  | false                                     | Boolean    | Download cover art file (cover.jpg) to the context directory (requires `sort_by_context`)                                                                                                 |
| `fix_tags`                    | true                                      | Boolean    | Enable tag writing capabilities                                                                                                                                                           |
| `tag_mappings`                | *Listed below*                            | String Map | Custom tag mappings                                                                                                                                                                       |
| `track_file_template`         | {number}. {artists} - {name} ({mix_name}) | String     | Track filename template                                                                                                                                                                   |
| `release_directory_template`  | [{catalog_number}] {artists} - {name}     | String     | Release directory template                                                                                                                                                                |
| `playlist_directory_template` | {name} [{created_date}]                   | String     | Playlist directory template                                                                                                                                                               |
| `chart_directory_template`    | {name} [{published_date}]                 | String     | Chart directory template                                                                                                                                                                  |
| `label_directory_template`    | {name} [{updated_date}]                   | String     | Label directory template                                                                                                                                                                  |
| `artist_directory_template`   | {name}                                    | String     | Artist directory template                                                                                                                                                                 |
| `whitespace_character`        |                                           | String     | Whitespace character for track filenames and release directories                                                                                                                          |
| `artists_limit`               | 3                                         | Integer    | Maximum number of artists allowed before replacing with `artists_short_form` (affects directories, filenames, and search results)                                                         |
| `artists_short_form`          | VA                                        | String     | Custom string to represent "Various Artists"                                                                                                                                              |
| `key_system`                  | standard-short                            | String     | Music key system used in filenames and tags                                                                                                                                               |
| `proxy`                       |                                           | String     | Proxy URL                                                                                                                                                                                 |

If the Beatport credentials are correct, you should also see the file `beatportdl-credentials.json` appear in the BeatportDL directory.
*If you accidentally entered an incorrect password and got an error, you can always manually edit the config file*

Download quality options, per Beatport/Beatsource subscription type:

| Option       | Description                                                                                                  | Requires at least              | Notes                                                                   |
|--------------|--------------------------------------------------------------------------------------------------------------|--------------------------------|-------------------------------------------------------------------------|
| `medium-hls` | 128 kbps AAC through `/stream` endpoint (IMPORTANT: requires [ffmpeg](https://www.ffmpeg.org/download.html)) | Essential / Beatsource         | Same as `medium` on Advanced but uses a slightly slower download method |
| `medium`     | 128 kbps AAC                                                                                                 | Advanced / Beatsource Pro+     |                                                                         |
| `high`       | 256 kbps AAC                                                                                                 | Professional / Beatsource Pro+ |                                                                         |
| `lossless`   | 44.1 kHz FLAC                                                                                                | Professional / Beatsource Pro+ |                                                                         |

Available `track_exists` options:
* `error` Log error and skip
* `skip` Skip silently
* `overwrite` Re-download
* `update` Update tags

Available template keywords for filenames and directories (`*_template`):
* Track: `id`,`name`,`mix_name`,`slug`,`artists`,`remixers`,`number`,`length`,`key`,`bpm`,`genre`,`subgenre`,`genre_with_subgenre`,`subgenre_or_genre`,`isrc`,`label`
* Release: `id`,`name`,`slug`,`artists`,`remixers`,`date`,`year`,`track_count`,`bpm_range`,`catalog_number`,`upc`,`label`
* Playlist: `id`,`name`,`first_genre`,`track_count`,`bpm_range`,`length`,`created_date`,`updated_date`
* Chart: `id`,`name`,`slug`,`first_genre`,`track_count`,`creator`,`created_date`,`published_date`,`updated_date`
* Artist: `id`, `name`, `slug`
* Label: `id`, `name`, `slug`, `created_date`, `updated_date`

Default `tag_mappings` config:
```yaml
tag_mappings:
   flac:
      track_name: "TITLE"
      track_artists: "ARTIST"
      track_number: "TRACKNUMBER"
      track_subgenre_or_genre: "GENRE"
      track_key: "KEY"
      track_bpm: "BPM"
      track_isrc: "ISRC"
   
      release_name: "ALBUM"
      release_artists: "ALBUMARTIST"
      release_date: "DATE"
      release_track_count: "TOTALTRACKS"
      release_catalog_number: "CATALOGNUMBER"
      release_label: "LABEL"
   m4a:
      track_name: "TITLE"
      track_artists: "ARTIST"
      track_number: "TRACKNUMBER"
      track_genre: "GENRE"
      track_key: "KEY"
      track_bpm: "BPM"
      track_isrc: "ISRC"
   
      release_name: "ALBUM"
      release_artists: "ALBUMARTIST"
      release_date: "DATE"
      release_track_count: "TOTALTRACKS"
      release_catalog_number: "CATALOGNUMBER"
      release_label: "LABEL"
```

As you can see, each key here represents a predefined value from either a release or a track that you can use to customize what is written to which tags. When you add an entry in the mappings for any format (for e.g., `flac`), only the tags that you specify will be written.

All tags by default are converted to uppercase, but since some M4A players might not recognize it, you can write the tag in lowercase and add the `_raw` suffix to bypass the conversion. *(This applies to M4A tags only)*

For e.g., Traktor doesn't recognize the track key tag in uppercase, so you have to add:
```yaml
tag_mappings:
   m4a:
      track_key: "initialkey_raw"
```

Available `tag_mappings` keys: `track_id`,`track_url`,`track_name`,`track_artists`,`track_artists_limited`,`track_remixers`,`track_remixers_limited`,`track_number`,`track_number_with_padding`,`track_number_with_total`,`track_genre`,`track_subgenre`,`track_genre_with_subgenre`,`track_subgenre_or_genre`,`track_key`,`track_bpm`,`track_isrc`,`release_id`,`release_url`,`release_name`,`release_artists`,`release_artists_limited`,`release_remixers`,`release_remixers_limited`,`release_date`,`release_year`,`release_track_count`,`release_track_count_with_padding`,`release_catalog_number`,`release_upc`,`release_label`,`release_label_url`

Available `key_system` options:

| System           | Example           |
|------------------|-------------------|
| `standard`       | Eb Minor, F Major |
| `standard-short` | Ebm, F            |
| `openkey`        | 7m, 12d           |
| `camelot`        | 2A, 7B            |

Proxy URL format example: `http://username:password@127.0.0.1:8080`

Usage
---

Run BeatportDL and enter Beatport or Beatsource URL or search query:
```shell
./beatportdl
Enter url or search query:
```
By default, search returns the results from beatport, if you want to search on beatsource instead, include `@beatsource` tag in the query

...or specify the URL using positional arguments:
```shell
./beatportdl https://www.beatport.com/track/strobe/1696999 https://www.beatport.com/track/move-for-me/591753
```
...or provide a text file with urls (separated by a newline)
```shell
./beatportdl file.txt file2.txt
```

URL types that are currently supported: **Tracks, Releases, Playlists, Charts, Labels, Artists**

Building
---
Required dependencies:
* [TagLib](https://github.com/taglib/taglib) >= 2.0
* [zlib](https://github.com/madler/zlib) >= 1.2.3
* [Zig C/C++ Toolchain](https://github.com/ziglang/zig) >= 0.14.0

BeatportDL uses [TagLib](https://taglib.org/) C bindings to handle audio metadata and therefore requires [CGO](https://go.dev/wiki/cgo)

Makefile is adapted for cross-compilation and uses [Zig toolchain](https://github.com/ziglang/zig)

To compile BeatportDL with Zig using Makefile, you must specify the paths to the C/C++ libraries folder and headers folder for the desired OS and architecture with `-L` (for libraries) and `-I` (for headers) flags using environment variables: `MACOS_ARM64_LIB_PATH`, `MACOS_AMD64_LIB_PATH`, `LINUX_AMD64_LIB_PATH`, `LINUX_ARM64_LIB_PATH`, `WINDOWS_AMD64_LIB_PATH`

One line example *(for unix and unix-like os)*
```shell
MACOS_ARM64_LIB_PATH="-L/usr/local/lib -I/usr/local/include" \
make darwin-arm64
```

You can also create an `.env` file in the project folder and specify all environment variables in it:
```
MACOS_ARM64_LIB_PATH=-L/libraries/for/macos-arm64 -I/headers/for/macos-arm64
MACOS_AMD64_LIB_PATH=-L/libraries/for/macos-amd64 -I/headers/for/macos-amd64
LINUX_AMD64_LIB_PATH=-L/libraries/for/linux-amd64 -I/headers/for/linux-amd64
LINUX_ARM64_LIB_PATH=-L/libraries/for/linux-arm64 -I/headers/for/linux-arm64
WINDOWS_AMD64_LIB_PATH=-L/libraries/for/windows-amd64 -I/headers/for/windows-amd64
```

