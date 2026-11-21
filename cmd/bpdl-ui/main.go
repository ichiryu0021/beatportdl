//go:build windows

// Command bpdl-ui is the Chromium-kernel desktop shell for BeatportDL.
//
// It hosts the embedded web UI inside Microsoft Edge WebView2, serves the UI
// from an app-internal virtual host (no local HTTP port is opened for the
// interface) and talks to the existing Go downloader backend (`beatportdl.exe
// --serve`) through an in-process JSON-RPC/SSE client plus the WebView2 message
// bridge.
package main

import (
	"flag"
	"fmt"
	"os"
	"runtime"

	"unspok3n/beatportdl/internal/desktop"
	"unspok3n/beatportdl/internal/winui"
)

func main() {
	// A copied updater helper is launched with --apply-update while the normal
	// app is still shutting down. It must run before COM/WebView2 and before the
	// single-instance mutex, otherwise it would either join the wrong session or
	// try to become a second UI window.
	if desktop.IsUpdaterInvocation(os.Args[1:]) {
		os.Exit(desktop.RunUpdaterFromArgs(os.Args[1:]))
	}

	debug := flag.Bool("debug", false, "启用开发者工具与浏览器右键菜单")
	addr := flag.String("addr", "127.0.0.1:8765", "后端 RPC 监听地址")
	backendPath := flag.String("backend", "", "指定 beatportdl.exe 路径")
	attachOnly := flag.Bool("attach", false, "只连接已在运行的后端，不自动启动")
	lowMemory := flag.Bool("low-memory", false, "低内存模式：软件渲染并压低 Chromium 缓存")
	flag.Parse()

	// WebView2 and Win32 both require a single STA thread: the window procedure,
	// COM apartment and every CoreWebView2 call must stay on the same OS thread.
	// Without this lock the Go scheduler may move main between threads, which
	// makes environment/controller creation hang intermittently.
	runtime.LockOSThread()

	winui.SetDPIAware()
	if err := winui.CoInitialize(); err != nil {
		winui.Message("Beatport 下载器", "COM 初始化失败："+err.Error(), winui.MBOK|winui.MBIconError)
		return
	}
	defer winui.CoUninitialize()

	// One window per session: focus the running instance instead of opening a
	// second copy (the backend port is a single-instance resource anyway).
	_, first := winui.AcquireSingleInstance(`Local\BeatportDL.Desktop.SingleInstance`)
	if !first {
		if winui.FocusExistingWindow() {
			return
		}
		winui.Message("Beatport 下载器", "程序已经在运行中。", winui.MBOK|winui.MBIconInformation)
		return
	}

	err := desktop.Run(desktop.Config{
		Addr:        *addr,
		Debug:       *debug,
		BackendPath: *backendPath,
		AttachOnly:  *attachOnly,
		LowMemory:   *lowMemory,
	})
	if err != nil {
		msg := err.Error()
		fmt.Fprintln(os.Stderr, msg)
		winui.Message("Beatport 下载器", msg, winui.MBOK|winui.MBIconError)
	}
}
