//go:build windows

package desktop

import (
	"fmt"
	"log"
	"os"
	"strings"
	"sync/atomic"
	"time"

	"unspok3n/beatportdl/internal/webview2/edge"
	"unspok3n/beatportdl/internal/webview2/loader"
	"unspok3n/beatportdl/internal/winui"
)

// Config controls the desktop shell.
type Config struct {
	// Addr is the backend HTTP address (default 127.0.0.1:8765).
	Addr string
	// Debug enables DevTools and the browser context menu.
	Debug bool
	// BackendPath overrides backend executable discovery.
	BackendPath string
	// AttachOnly does not start the backend; it only connects if one is up.
	AttachOnly bool
	// LowMemory forces software rendering and Chromium's low-end mode.
	LowMemory bool
}

// App owns the window, the WebView2 control, the bridge and the backend child
// process for one GUI session.
type App struct {
	cfg   Config
	paths Paths

	log       *log.Logger
	logCloser interface{ Close() error }

	window   *winui.Window
	chromium *edge.Chromium
	bridge   *bridge
	backend  *backend
	stream   *eventStream

	shown atomic.Bool
}

// Run starts the desktop shell and blocks until the window closes.
func Run(cfg Config) error {
	if cfg.Addr == "" {
		cfg.Addr = "127.0.0.1:8765"
	}
	paths, err := ResolvePaths()
	if err != nil {
		return err
	}
	logger, closer := openLogger(paths.GUILog)
	defer closer.Close()
	winui.SetLogger(func(msg string) { logger.Printf("%s", msg) })
	logger.Printf("Beatport 下载器 %s 启动：exeDir=%s dataDir=%s portable=%v", Version, paths.ExeDir, paths.DataDir, paths.Portable())

	// Icon is loaded by Win32 from a file path, so extract the embedded copy.
	if data, err := IconBytes(); err == nil {
		_ = os.WriteFile(paths.IconPath, data, 0o644)
	}
	uiDir, assetVersion, err := ExtractAssets(paths.UIDir)
	if err != nil {
		return err
	}
	logger.Printf("界面资源：%s (版本 %s)", uiDir, assetVersion)

	webviewVersion, err := loader.RuntimeVersion()
	if err != nil {
		if err == loader.ErrRuntimeMissing {
			return fmt.Errorf("未检测到 Microsoft Edge WebView2 运行时。\n\nWindows 10/11 通常已自带；若缺失，请安装后重试：\nhttps://go.microsoft.com/fwlink/p/?LinkId=2124703")
		}
		return fmt.Errorf("WebView2 初始化失败：%w", err)
	}
	logger.Printf("WebView2 运行时版本：%s", webviewVersion)
	performanceMode := tuneWebViewMemory(cfg.LowMemory, logger)

	backendProc := newBackend(paths, cfg.Addr)
	if cfg.BackendPath != "" {
		backendProc.exePath = cfg.BackendPath
	}
	rpc := newRPCClient(cfg.Addr)
	br := newBridge(rpc, backendProc, paths, logger, cfg.Debug, webviewVersion, performanceMode)

	app := &App{
		cfg:       cfg,
		paths:     paths,
		log:       logger,
		logCloser: closer,
		bridge:    br,
		backend:   backendProc,
	}

	if err := app.createWindow(uiDir); err != nil {
		return err
	}

	app.startServices()
	defer app.shutdown()

	app.window.Show()
	app.window.Run()
	return nil
}

func (a *App) createWindow(uiDir string) error {
	opts := winui.Options{
		Title:     "Beatport 下载器",
		Width:     1380,
		Height:    880,
		MinW:      1080,
		MinH:      680,
		IconPath:  a.paths.IconPath,
		StateFile: a.paths.StateFile,
		OnSize: func(w, h int) {
			if a.chromium != nil {
				a.chromium.Resize()
			}
		},
		OnDpi: func(scale float64) {
			if a.chromium != nil {
				a.chromium.Resize()
			}
		},
		OnFocus: func() {
			if a.chromium != nil {
				a.chromium.Focus()
			}
		},
		OnClose: func() bool { return true },
	}

	window, err := winui.NewWindow(opts)
	if err != nil {
		return fmt.Errorf("创建窗口失败：%w", err)
	}
	a.window = window

	chromium := edge.NewChromium()
	chromium.DataPath = a.paths.WebViewData
	chromium.MessageCallback = func(raw string) { a.bridge.handleMessage(raw) }
	chromium.NavigationStartingCallback = func(sender *edge.ICoreWebView2, args *edge.ICoreWebView2NavigationStartingEventArgs) {
		uri, err := args.URI()
		if err != nil {
			return
		}
		a.log.Printf("导航开始：%s", uri)
		if strings.HasPrefix(uri, "https://bpdl.app/") || uri == "about:blank" || strings.HasPrefix(uri, "data:") {
			return
		}
		args.Cancel()
		if strings.HasPrefix(uri, "http://") || strings.HasPrefix(uri, "https://") {
			_ = winui.ShellOpen(uri)
		}
	}
	chromium.NewWindowRequestedCallback = func(sender *edge.ICoreWebView2, args *edge.ICoreWebView2NewWindowRequestedEventArgs) {
		uri, _ := args.URI()
		args.SetHandled(true)
		if strings.HasPrefix(uri, "http://") || strings.HasPrefix(uri, "https://") {
			_ = winui.ShellOpen(uri)
		}
	}
	chromium.NavigationCompletedCallback = func(sender *edge.ICoreWebView2, args *edge.ICoreWebView2NavigationCompletedEventArgs) {
		success, _ := args.IsSuccess()
		status, _ := args.WebErrorStatus()
		a.log.Printf("导航完成：success=%v webErrorStatus=%d", success, status)
		a.showWindow()
	}
	a.chromium = chromium

	if !chromium.Embed(window.Hwnd()) {
		err := chromium.LastError()
		window.Close()
		if err != nil {
			return fmt.Errorf("WebView2 控件创建失败：%w", err)
		}
		return fmt.Errorf("WebView2 控件创建失败")
	}

	if settings, err := chromium.GetSettings(); err == nil {
		_ = settings.PutAreDefaultContextMenusEnabled(a.cfg.Debug)
		_ = settings.PutAreDevToolsEnabled(a.cfg.Debug)
		_ = settings.PutIsStatusBarEnabled(false)
		_ = settings.PutIsZoomControlEnabled(false)
		_ = settings.PutAreHostObjectsAllowed(false)
		_ = settings.PutIsBuiltInErrorPageEnabled(false)
		_ = settings.PutAreDefaultScriptDialogsEnabled(false)
	}
	_ = chromium.PutDefaultBackgroundColor(11, 15, 20)
	_ = chromium.SetMonitorScaleAutoDetect(true)

	if err := chromium.SetVirtualHostMapping("bpdl.app", uiDir); err != nil {
		a.log.Printf("虚拟主机映射失败：%v", err)
	} else {
		a.log.Printf("界面内部地址：https://bpdl.app/index.html -> %s", uiDir)
	}

	a.bridge.setWebView(chromium, window)
	_ = chromium.Show()
	chromium.Resize()
	chromium.Navigate("https://bpdl.app/index.html")

	// Never leave the user staring at an invisible process if navigation
	// stalls (for example a corrupted ui folder).
	time.AfterFunc(3*time.Second, a.showWindow)
	return nil
}

func (a *App) showWindow() {
	if a.shown.CompareAndSwap(false, true) {
		if a.window != nil {
			a.window.Post(a.window.Show)
		}
	}
}

func (a *App) startServices() {
	// Event stream: reconnect loop handles a backend that is not up yet.
	a.stream = newEventStream(a.cfg.Addr,
		func(ev map[string]any) {
			if a.bridge.eventBatch != nil {
				a.bridge.eventBatch.Push(ev)
			}
		},
		func(connected bool, message string) {
			a.bridge.send(map[string]any{
				"kind":      "events.status",
				"connected": connected,
				"message":   message,
			})
		},
	)
	a.stream.Start()

	if a.cfg.AttachOnly {
		a.bridge.setBackendState("ready", "")
		a.log.Printf("附加模式：不启动后端")
		return
	}

	// Start (or attach to) the backend without blocking the window.
	go func() {
		a.bridge.setBackendState("starting", "")
		if a.backend.Healthy(1200 * time.Millisecond) {
			a.bridge.setBackendState("ready", "")
			a.log.Printf("已连接到运行中的后端 %s", a.cfg.Addr)
			return
		}
		if err := a.backend.EnsureRunning(a.paths.DataDir, 30*time.Second); err != nil {
			a.log.Printf("后端启动失败：%v", err)
			a.bridge.setBackendState("down", err.Error())
			return
		}
		a.log.Printf("后端已就绪：%s", a.cfg.Addr)
		a.bridge.setBackendState("ready", "")
	}()

	// Health watchdog keeps the UI banner honest if the backend dies.
	go func() {
		ticker := time.NewTicker(6 * time.Second)
		defer ticker.Stop()
		for range ticker.C {
			healthy := a.backend.Healthy(2 * time.Second)
			state, msg := a.bridge.currentBackendState()
			switch {
			case healthy && state != "ready":
				a.bridge.setBackendState("ready", "")
			case !healthy && state == "ready":
				a.bridge.setBackendState("down", "后端连接已断开，可点击重连")
				_ = msg
			}
		}
	}()
}

func (a *App) shutdown() {
	a.log.Printf("窗口已关闭，开始清理")
	if a.stream != nil {
		a.stream.Stop()
	}
	if a.bridge != nil && a.bridge.eventBatch != nil {
		a.bridge.eventBatch.Close()
	}
	if a.backend != nil {
		a.backend.Stop()
	}
	if a.chromium != nil {
		_ = a.chromium.Hide()
	}
}
