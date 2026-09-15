# BPDownloader — Beatport/Beatsource Downloader (C++/Qt)

**English** | [简体中文](#简体中文)

> A C++17 / Qt 6 rewrite and fork of [**unspok3n/beatportdl**](https://github.com/unspok3n/beatportdl) (Go), rebuilt as a native Windows desktop application with a graphical interface, bilingual UI, light/dark themes, and a fully static single-file executable.
>
> This repository is a GitHub fork of the original project and therefore keeps its commit history and the GPL-3.0 license. All original copyright belongs to **unspok3n** and the beatportdl contributors.

---

## Features

- 🎧 Browse and download from **Beatport** and **Beatsource** with your own account
- 🎵 **FLAC** (lossless) and **AAC** (HLS) audio
- ⚡ **Multi-threaded concurrent downloads** with configurable global / per-task worker counts
- 🗂️ **Organize downloads into genre folders** — by genre, or by sub-genre with genre fallback
- 🏷️ Automatic metadata tagging and embedded cover art (TagLib)
- 🕒 **Scheduled download tasks** per genre (daily / weekly / monthly / every 3 months)
- 🌐 Three proxy modes: direct connection, system proxy, or custom HTTP proxy
- 🎨 Light and dark themes
- 🌍 Bilingual interface: **English / 简体中文**, switchable in Settings
- 🪟 Single static `.exe` for Windows — no runtime or installer required
- 🖥️ Three run modes built from one codebase: GUI (default), `--serve` (HTTP/JSON-RPC), and `--cli` (interactive/batch)

## Requirements

- Windows 10 / 11 (x64)
- A valid Beatport or Beatsource account. You can only download content your account is entitled to access.

## Usage

1. Download `BPDownloader.exe` from the [Releases](../../releases) page.
2. Run it and log in with your Beatport/Beatsource credentials in the login dialog.
3. Use the tabs:
   - **Discover** — browse the latest releases by genre
   - **Search** — search tracks/releases
   - **Scheduled Tasks** — set up recurring downloads for a chosen genre
   - **Downloads** — monitor download progress and history
   - **Settings** — download folder, format, naming scheme, proxy, concurrency, genre folders, theme and language

Settings are stored next to the executable in `bpdownloader-config.yml`.

> ⚠️ Never share your config file or `*-credentials.json` — they contain your login data.

### Headless modes

```text
BPDownloader.exe              # GUI (default)
BPDownloader.exe --serve      # embedded HTTP / JSON-RPC server
BPDownloader.exe --cli        # interactive / batch command line
```

## Building from source

Toolchain: Windows + [MSYS2](https://www.msys2.org/) (MinGW-w64), CMake 3.21+, Qt 6 (Widgets, Network), OpenSSL 1.1/3, TagLib 2.x.

```bash
# Inside an MSYS2 MINGW64 shell
pacman -S --needed mingw-w64-x86_64-gcc mingw-w64-x86_64-cmake \
  mingw-w64-x86_64-qt6-base mingw-w64-x86_64-openssl mingw-w64-x86_64-taglib

cmake -S . -B build -G "MinGW Makefiles"
cmake --build build -j
```

### Static single-file build

Use a static Qt 6 prefix and enable `BP_STATIC_LINK`:

```bash
cmake -S . -B build-static -G "MinGW Makefiles" \
  -DCMAKE_PREFIX_PATH="/path/to/qt6-static;/mingw64" \
  -DBP_STATIC_LINK=ON
cmake --build build-static -j
```

### Updating translations

The UI source language is English; Simplified Chinese lives in [i18n/bpdownloader_zh_CN.ts](i18n/bpdownloader_zh_CN.ts).
The `.ts` file is compiled to an embedded `.qm` resource automatically at build time.

```bash
cmake --build build --target update_translations   # refresh the .ts file with lupdate
# translate the new strings in Qt Linguist, then rebuild
```

## Disclaimer

This software is provided for personal use only. You are responsible for complying with Beatport's/Beatsource's Terms of Service and the copyright laws applicable in your jurisdiction. Only download content that you have legally purchased or are otherwise entitled to access. The authors accept no responsibility for misuse.

## License

Licensed under the **GNU General Public License v3.0 or later**, per the original project. See [LICENSE](LICENSE) for the full text and [NOTICE](NOTICE) for attribution.

Beatport® and Beatsource® are trademarks of their respective owners; this project is an independent, unofficial work and is not affiliated with them.

---

<a id="简体中文"></a>

# BPDownloader —— Beatport / Beatsource 下载器（C++/Qt）

[English](#bpdownloader--beatportbeatsource-downloader-cqt) | **简体中文**

> 本项目是 [**unspok3n/beatportdl**](https://github.com/unspok3n/beatportdl)（Go 语言）的 C++17 / Qt 6 重写与 fork，以原生 Windows 桌面应用的形式重新实现，提供图形界面、中英双语、浅色/深色主题，以及全静态单文件可执行程序。
>
> 本仓库是原项目的 GitHub fork，保留了其提交历史与 GPL-3.0 协议。原始版权归 **unspok3n** 及 beatportdl 贡献者所有。

## 功能特性

- 🎧 使用你自己的账号浏览并下载 **Beatport** 和 **Beatsource**
- 🎵 支持 **FLAC**（无损）与 **AAC**（HLS）音频
- ⚡ **多线程并发下载**，可分别配置全局/每任务并发数
- 🗂️ **按曲风分类存放下载文件** —— 按曲风，或按子曲风（无则回退到曲风）
- 🏷️ 自动写入元数据标签并内嵌封面（TagLib）
- 🕒 按曲风设置 **定时下载任务**（每天 / 每周 / 每月 / 每三个月）
- 🌐 代理三态：直连、跟随系统、自定义 HTTP 代理
- 🎨 浅色 / 深色主题
- 🌍 **中英双语界面**，在设置页随时切换
- 🪟 Windows 单文件静态 `.exe`，无需运行库、无需安装
- 🖥️ 同一套代码三种运行模式：GUI（默认）、`--serve`（HTTP/JSON-RPC）、`--cli`（交互式/批处理）

## 运行要求

- Windows 10 / 11（x64）
- 有效的 Beatport 或 Beatsource 账号，且只能下载你的账号有权访问的内容

## 使用方法

1. 在 [Releases](../../releases) 页面下载 `BPDownloader.exe`。
2. 运行后在登录窗口输入 Beatport/Beatsource 账号登录。
3. 各标签页说明：
   - **发现最新**：按曲风浏览最新发行
   - **搜索**：搜索单曲/专辑
   - **定时任务**：为指定曲风设置周期性下载
   - **下载**：查看下载进度与历史
   - **设置**：下载目录、格式、命名规则、代理、并发数、曲风文件夹、主题与语言

配置保存在可执行文件旁的 `bpdownloader-config.yml` 中。

> ⚠️ 切勿将配置文件或 `*-credentials.json` 分享给他人，其中包含你的登录数据。

### 无界面模式

```text
BPDownloader.exe              # 图形界面（默认）
BPDownloader.exe --serve      # 内嵌 HTTP / JSON-RPC 服务
BPDownloader.exe --cli        # 交互式 / 批处理命令行
```

## 从源码构建

工具链：Windows + [MSYS2](https://www.msys2.org/)（MinGW-w64）、CMake 3.21+、Qt 6（Widgets、Network）、OpenSSL、TagLib 2.x。

```bash
# 在 MSYS2 MINGW64 终端中
pacman -S --needed mingw-w64-x86_64-gcc mingw-w64-x86_64-cmake \
  mingw-w64-x86_64-qt6-base mingw-w64-x86_64-openssl mingw-w64-x86_64-taglib

cmake -S . -B build -G "MinGW Makefiles"
cmake --build build -j
```

### 静态单文件构建

使用静态 Qt 6 prefix 并开启 `BP_STATIC_LINK`：

```bash
cmake -S . -B build-static -G "MinGW Makefiles" \
  -DCMAKE_PREFIX_PATH="/path/to/qt6-static;/mingw64" \
  -DBP_STATIC_LINK=ON
cmake --build build-static -j
```

### 更新翻译

界面源语言为英文，简体中文翻译位于 [i18n/bpdownloader_zh_CN.ts](i18n/bpdownloader_zh_CN.ts)，
构建时 `.ts` 会自动编译为内嵌的 `.qm` 资源。

```bash
cmake --build build --target update_translations   # 使用 lupdate 刷新 .ts
# 在 Qt Linguist 中翻译新增条目后重新构建
```

## 免责声明

本软件仅供个人使用。你需要自行遵守 Beatport/Beatsource 的服务条款以及所在地的版权法律法规，仅下载你已合法购买或有权访问的内容。因不当使用造成的一切后果由使用者自行承担，作者概不负责。

## 许可证

依照原项目，采用 **GNU General Public License v3.0 或更高版本**。全文见 [LICENSE](LICENSE)，版权归属说明见 [NOTICE](NOTICE)。

Beatport® 与 Beatsource® 为其各自所有者的商标，本项目为独立的非官方作品，与上述公司无任何关联。
