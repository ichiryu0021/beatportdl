//go:build windows

package desktop

import (
	"context"
	"encoding/json"
	"fmt"
	"log"
	"net/url"
	"os"
	"path/filepath"
	"strings"
	"sync"
	"sync/atomic"
	"time"

	"unspok3n/beatportdl/internal/webview2/edge"
	"unspok3n/beatportdl/internal/winui"
)

func contextWithTimeout(d time.Duration) (context.Context, context.CancelFunc) {
	return context.WithTimeout(context.Background(), d)
}

// The bridge is the app-internal channel between the WebView2 page and Go.
// The page calls window.chrome.webview.postMessage(JSON) and receives Go
// messages through the same object's "message" event. No port, no CORS.

type inboundMessage struct {
	ID      *int64          `json:"id"`
	Kind    string          `json:"kind"`
	Method  string          `json:"method"`
	Action  string          `json:"action"`
	Params  json.RawMessage `json:"params"`
	URL     string          `json:"url"`
	Title   string          `json:"title"`
	Path    string          `json:"path"`
	Level   string          `json:"level"`
	Message string          `json:"message"`
	Proxy   string          `json:"proxy"`
	Enabled *bool           `json:"enabled"`
	Mode    string          `json:"mode"`
	Full    *bool           `json:"full"`
}

type bridge struct {
	rpc             *rpcClient
	backend         *backend
	paths           Paths
	log             *log.Logger
	debug           bool
	webviewVersion  string
	performanceMode string

	mu      sync.Mutex
	webview *edge.Chromium
	window  *winui.Window
	loaded  bool
	queue   []map[string]any

	eventBatch *eventBatcher

	backendStateMu sync.Mutex
	backendState   string
	backendMsg     string

	restarting atomic.Bool
	updates    *updateManager
	catalog    *catalogManager
	scheduler  *schedulerManager
}

func newBridge(rpc *rpcClient, b *backend, paths Paths, log *log.Logger, debug bool, webviewVersion, performanceMode string) *bridge {
	br := &bridge{
		rpc:             rpc,
		backend:         b,
		paths:           paths,
		log:             log,
		debug:           debug,
		webviewVersion:  webviewVersion,
		performanceMode: performanceMode,
		backendState:    "starting",
	}
	br.updates = newUpdateManager(paths, log)
	br.catalog = newCatalogManager(paths, log)
	br.scheduler = newSchedulerManager(br.rpc, br.catalog, log, br.send)
	br.eventBatch = newEventBatcher(br)
	return br
}

func (b *bridge) setWebView(c *edge.Chromium, w *winui.Window) {
	b.mu.Lock()
	b.webview = c
	b.window = w
	b.mu.Unlock()
}

// pageLoaded flushes messages queued before the page could receive them.
func (b *bridge) pageLoaded() {
	b.mu.Lock()
	b.loaded = true
	queue := b.queue
	b.queue = nil
	b.mu.Unlock()
	for _, msg := range queue {
		b.post(msg)
	}
	b.send(map[string]any{"kind": "hello", "data": b.helloPayload()})
	state, message := b.currentBackendState()
	b.sendBackendStateFull(state, message)
}

// send marshals and posts a message to the page, queuing it until the document
// is loaded.
func (b *bridge) send(msg map[string]any) {
	b.mu.Lock()
	loaded := b.loaded
	webview := b.webview
	if !loaded || webview == nil {
		if len(b.queue) < 200 {
			b.queue = append(b.queue, msg)
		}
		b.mu.Unlock()
		return
	}
	window := b.window
	b.mu.Unlock()
	b.postVia(window, webview, msg)
}

func (b *bridge) post(msg map[string]any) {
	b.mu.Lock()
	webview := b.webview
	window := b.window
	b.mu.Unlock()
	b.postVia(window, webview, msg)
}

func (b *bridge) postVia(window *winui.Window, webview *edge.Chromium, msg map[string]any) {
	if window == nil || webview == nil {
		return
	}
	data, err := json.Marshal(msg)
	if err != nil {
		return
	}
	window.Post(func() {
		if err := webview.PostWebMessageJSON(string(data)); err != nil {
			b.log.Printf("bridge: %v", err)
		}
	})
}

func (b *bridge) helloPayload() map[string]any {
	return map[string]any{
		"appVersion":      Version,
		"webviewVersion":  b.webviewVersion,
		"backendAddr":     b.backend.addr,
		"backendLog":      b.paths.BackendLog,
		"guiLog":          b.paths.GUILog,
		"dataDir":         b.paths.DataDir,
		"exeDir":          b.paths.ExeDir,
		"configFile":      b.paths.ConfigFile,
		"username":        configUsername(b.paths.ConfigFile),
		"portable":        b.paths.Portable(),
		"performanceMode": b.performanceMode,
		"debug":           b.debug,
	}
}

// configUsername extracts the configured Beatport username for display only.
// It never touches the password or the credential cache.
func configUsername(path string) string {
	data, err := os.ReadFile(path)
	if err != nil {
		return ""
	}
	for _, line := range strings.Split(string(data), "\n") {
		line = strings.TrimSpace(line)
		if !strings.HasPrefix(line, "username:") {
			continue
		}
		value := strings.TrimSpace(strings.TrimPrefix(line, "username:"))
		value = strings.Trim(value, `"'`)
		if value == "null" {
			return ""
		}
		return value
	}
	return ""
}

// ---- inbound messages -----------------------------------------------------

func (b *bridge) handleMessage(raw string) {
	var msg inboundMessage
	if err := json.Unmarshal([]byte(raw), &msg); err != nil {
		b.log.Printf("bridge: invalid message: %v", err)
		return
	}
	if b.debug {
		b.log.Printf("bridge <- kind=%s method=%s action=%s", msg.Kind, msg.Method, msg.Action)
	}
	switch msg.Kind {
	case "rpc":
		b.handleRPC(msg)
	case "host":
		b.handleHost(msg)
	case "ui":
		b.handleUI(msg)
	default:
		b.log.Printf("bridge: unknown message kind %q", msg.Kind)
	}
}

func (b *bridge) reply(id *int64, kind string, value any, err error) {
	if id == nil {
		return
	}
	if err != nil {
		b.log.Printf("bridge: %s 失败：%v", kind, err)
		b.send(map[string]any{
			"kind": kind + ".error",
			"id":   *id,
			"error": map[string]any{
				"message": err.Error(),
			},
		})
		return
	}
	b.send(map[string]any{
		"kind":   kind + ".result",
		"id":     *id,
		"result": value,
	})
}

func (b *bridge) handleRPC(msg inboundMessage) {
	if msg.Method == "" {
		b.reply(msg.ID, "rpc", nil, fmt.Errorf("缺少方法名"))
		return
	}
	timeout := 90 * time.Second
	switch msg.Method {
	case "getLatest", "search":
		timeout = 180 * time.Second
	case "download", "setQuality", "setTrackExists", "setTrackFileTemplate",
		"getSettings", "getGenres", "authStatus", "login", "logout",
		"schedulerStatus", "startScheduler", "stopScheduler":
		timeout = 45 * time.Second
	}
	go func() {
		ctx, cancel := contextWithTimeout(timeout)
		defer cancel()
		result, err := b.rpc.Call(ctx, msg.Method, msg.Params)
		if err != nil {
			b.log.Printf("rpc %s 失败：%v", msg.Method, err)
		}
		b.reply(msg.ID, "rpc", json.RawMessage(result), err)
	}()
}

func (b *bridge) handleUI(msg inboundMessage) {
	switch msg.Action {
	case "ready":
		b.pageLoaded()
	case "retryBackend":
		b.restartBackend(msg.ID)
	case "log":
		level := msg.Level
		if level == "" {
			level = "info"
		}
		b.log.Printf("[ui:%s] %s", level, msg.Message)
		b.reply(msg.ID, "ui", map[string]any{"ok": true}, nil)
	case "openDevTools":
		if b.debug {
			if wv := b.webview; wv != nil {
				wv.OpenDevTools()
			}
		}
	case "quit":
		if w := b.window; w != nil {
			w.Close()
		}
	default:
		b.log.Printf("bridge: unknown ui action %q", msg.Action)
	}
}

func (b *bridge) handleHost(msg inboundMessage) {
	switch msg.Action {
	case "windowDrag":
		if b.window == nil {
			b.reply(msg.ID, "host", nil, fmt.Errorf("窗口未就绪"))
			return
		}
		b.window.StartDrag()
		b.reply(msg.ID, "host", map[string]any{"ok": true}, nil)
	case "windowResizeStart":
		var p struct {
			Direction string `json:"direction"`
		}
		if err := json.Unmarshal(msg.Params, &p); err != nil {
			b.reply(msg.ID, "host", nil, fmt.Errorf("缩放方向错误：%w", err))
			return
		}
		hit := map[string]uintptr{
			"left": winui.HTLeft, "right": winui.HTRight,
			"top": winui.HTTop, "bottom": winui.HTBottom,
			"top-left": winui.HTTopLeft, "top-right": winui.HTTopRight,
			"bottom-left": winui.HTBottomLeft, "bottom-right": winui.HTBottomRight,
		}[p.Direction]
		if hit == 0 || b.window == nil {
			b.reply(msg.ID, "host", nil, fmt.Errorf("缩放方向无效"))
			return
		}
		b.window.StartResize(hit)
		b.reply(msg.ID, "host", map[string]any{"ok": true}, nil)
	case "windowMinimize":
		if b.window != nil {
			b.window.Minimize()
		}
		b.reply(msg.ID, "host", map[string]any{"ok": true}, nil)
	case "windowToggleMaximize":
		if b.window != nil {
			b.window.ToggleMaximize()
		}
		b.reply(msg.ID, "host", map[string]any{"ok": true}, nil)
	case "windowClose":
		if b.window != nil {
			b.window.Close()
		}
		b.reply(msg.ID, "host", map[string]any{"ok": true}, nil)
	case "setWindowAppearance":
		var p struct {
			Light bool `json:"light"`
		}
		if err := json.Unmarshal(msg.Params, &p); err != nil {
			b.reply(msg.ID, "host", nil, fmt.Errorf("窗口外观参数错误：%w", err))
			return
		}
		if b.window != nil {
			b.window.SetAppearance(p.Light)
		}
		b.reply(msg.ID, "host", map[string]any{"ok": true}, nil)
	case "pickFolder":
		if b.window == nil {
			b.reply(msg.ID, "host", nil, fmt.Errorf("窗口未就绪"))
			return
		}
		// Run the modal picker from the message loop instead of inside the
		// WebView2 message callback, so the browser callback returns promptly.
		id := msg.ID
		title := msg.Title
		if title == "" {
			title = "选择文件夹"
		}
		window := b.window
		b.window.Post(func() {
			// PickFolder uses its own STA thread; the reply is posted back to
			// the UI thread so the dialog never blocks the WebView2 loop.
			go func() {
				b.log.Printf("打开文件夹选择器：%q", title)
				path, err := winui.PickFolder(window.Hwnd(), title)
				b.log.Printf("文件夹选择器返回：path=%q err=%v", path, err)
				b.reply(id, "host", map[string]any{"path": path}, err)
			}()
		})
	case "openPath", "openExternal":
		target := msg.Path
		if target == "" {
			target = msg.URL
		}
		if err := b.openTarget(target, msg.Action == "openExternal"); err != nil {
			b.reply(msg.ID, "host", nil, err)
			return
		}
		b.reply(msg.ID, "host", map[string]any{"ok": true}, nil)
	case "revealPath":
		if err := b.reveal(msg.Path); err != nil {
			b.reply(msg.ID, "host", nil, err)
			return
		}
		b.reply(msg.ID, "host", map[string]any{"ok": true}, nil)
	case "openLogs":
		_ = winui.ShellOpen(b.paths.LogsDir)
		b.reply(msg.ID, "host", map[string]any{"ok": true}, nil)
	case "openDataDir":
		_ = winui.ShellOpen(b.paths.DataDir)
		b.reply(msg.ID, "host", map[string]any{"ok": true}, nil)
	case "backendTail":
		b.reply(msg.ID, "host", map[string]any{"text": b.backend.TailLog(40)}, nil)
	case "info":
		b.reply(msg.ID, "host", b.helloPayload(), nil)
	case "restartBackend":
		b.restartBackend(msg.ID)
	case "getNetworkSettings":
		path := b.backend.ConfigPath()
		mode := readConfigString(path, "genre_sort_mode")
		if mode == "" {
			mode = "genre"
		}
		b.reply(msg.ID, "host", map[string]any{
			"proxy":           readConfigString(path, "proxy"),
			"sort_by_genre":   readConfigBool(path, "sort_by_genre"),
			"genre_sort_mode": mode,
			"configPath":      path,
			"ownsBackend":     b.backend.OwnsProcess(),
		}, nil)
	case "setProxy":
		normalized, err := normalizeProxy(msg.Proxy)
		if err != nil {
			b.reply(msg.ID, "host", nil, err)
			return
		}
		path := b.backend.ConfigPath()
		if err := writeConfigString(path, "proxy", normalized); err != nil {
			b.reply(msg.ID, "host", nil, err)
			return
		}
		b.backend.SetProxy(normalized)
		b.log.Printf("代理已更新：%s", maskProxy(normalized))
		b.restartForChange(msg.ID, map[string]any{
			"proxy": normalized, "restarted": true,
		})
	case "setGenreSort":
		enabled := false
		if msg.Enabled != nil {
			enabled = *msg.Enabled
		}
		mode := strings.TrimSpace(msg.Mode)
		if mode == "" {
			mode = "genre"
		}
		if mode != "genre" && mode != "subgenre_or_genre" {
			b.reply(msg.ID, "host", nil, fmt.Errorf("不支持的曲风归档方式：%s", mode))
			return
		}
		path := b.backend.ConfigPath()
		if err := writeConfigBool(path, "sort_by_genre", enabled); err != nil {
			b.reply(msg.ID, "host", nil, err)
			return
		}
		if err := writeConfigString(path, "genre_sort_mode", mode); err != nil {
			b.reply(msg.ID, "host", nil, err)
			return
		}
		b.log.Printf("曲风归档：enabled=%v mode=%s", enabled, mode)
		b.restartForChange(msg.ID, map[string]any{
			"sort_by_genre": enabled, "genre_sort_mode": mode, "restarted": true,
		})
	case "stopDownloads":
		b.log.Printf("请求停止全部下载并重启后端")
		b.restartForChange(msg.ID, map[string]any{"stopped": true, "restarted": true})
	case "checkUpdate":
		b.startUpdateCheck(msg.ID)
	case "applyUpdate":
		full := msg.Full != nil && *msg.Full
		b.startUpdateApply(msg.ID, full)
	case "getGenreCatalog":
		go func() {
			catalog, err := b.catalog.Catalog(false)
			if err != nil {
				b.reply(msg.ID, "host", nil, err)
				return
			}
			b.reply(msg.ID, "host", catalog, nil)
		}()
	case "getLatestBySubgenres":
		var p subgenreLatestParams
		if err := json.Unmarshal(msg.Params, &p); err != nil {
			b.reply(msg.ID, "host", nil, fmt.Errorf("子曲风参数错误：%w", err))
			return
		}
		go func() {
			result, err := b.catalog.Latest(p)
			if err != nil {
				b.reply(msg.ID, "host", nil, err)
				return
			}
			b.reply(msg.ID, "host", result, nil)
		}()
	case "getLatestDiscover":
		var p discoverLatestParams
		if err := json.Unmarshal(msg.Params, &p); err != nil {
			b.reply(msg.ID, "host", nil, fmt.Errorf("发现参数错误：%w", err))
			return
		}
		go func() {
			result, err := b.catalog.LatestDiscover(p)
			if err != nil {
				b.reply(msg.ID, "host", nil, err)
				return
			}
			b.reply(msg.ID, "host", result, nil)
		}()
	case "startScheduler":
		var p schedulerStartParams
		if err := json.Unmarshal(msg.Params, &p); err != nil {
			b.reply(msg.ID, "host", nil, fmt.Errorf("定时任务参数错误：%w", err))
			return
		}
		go func() {
			if err := b.scheduler.Start(p); err != nil {
				b.reply(msg.ID, "host", nil, err)
				return
			}
			b.reply(msg.ID, "host", map[string]any{"ok": true}, nil)
		}()
	case "stopScheduler":
		go func() {
			b.scheduler.Stop()
			b.reply(msg.ID, "host", map[string]any{"ok": true}, nil)
		}()
	case "schedulerStatus":
		b.reply(msg.ID, "host", b.scheduler.Status(), nil)
	default:
		b.reply(msg.ID, "host", nil, fmt.Errorf("未知操作: %s", msg.Action))
	}
}

func (b *bridge) openTarget(target string, external bool) error {
	target = strings.TrimSpace(target)
	if target == "" {
		return fmt.Errorf("路径为空")
	}
	if external {
		u, err := url.Parse(target)
		if err != nil || (u.Scheme != "http" && u.Scheme != "https") {
			return fmt.Errorf("只允许打开 http/https 链接")
		}
		return winui.ShellOpen(target)
	}
	abs, err := filepath.Abs(target)
	if err != nil {
		return err
	}
	if _, err := os.Stat(abs); err != nil {
		return fmt.Errorf("路径不存在: %s", abs)
	}
	return winui.ShellOpen(abs)
}

func (b *bridge) reveal(path string) error {
	abs, err := filepath.Abs(strings.TrimSpace(path))
	if err != nil {
		return err
	}
	if _, err := os.Stat(abs); err != nil {
		return fmt.Errorf("路径不存在: %s", abs)
	}
	return winui.RevealInExplorer(abs)
}

func (b *bridge) restartBackend(id *int64) {
	b.restartForChange(id, map[string]any{"ok": true})
}

// restartForChange restarts the backend (for proxy/genre/stop flows) and
// replies once it answers /health again.
func (b *bridge) restartForChange(id *int64, value map[string]any) {
	if !b.restarting.CompareAndSwap(false, true) {
		b.reply(id, "host", nil, fmt.Errorf("后端正在重启，请稍候"))
		return
	}
	b.setBackendState("starting", "")
	go func() {
		defer b.restarting.Store(false)
		err := b.backend.Restart(b.paths.DataDir, 30*time.Second)
		if err != nil {
			b.setBackendState("down", err.Error())
			b.reply(id, "host", nil, err)
			return
		}
		b.setBackendState("ready", "")
		b.reply(id, "host", value, nil)
	}()
}

// ---- GitHub update ---------------------------------------------------------

func (b *bridge) startUpdateCheck(id *int64) {
	go func() {
		b.sendUpdate(map[string]any{"state": "checking", "message": "正在检查更新…"})
		info, err := b.updates.Check(true)
		if err != nil {
			b.sendUpdate(map[string]any{
				"state": "error", "message": err.Error(),
			})
			b.reply(id, "host", nil, err)
			return
		}
		state := "none"
		if info.HasUpdate {
			state = "available"
		}
		b.reply(id, "host", info, nil)
		b.sendUpdate(map[string]any{"state": state, "data": info})
	}()
}

func (b *bridge) startUpdateApply(id *int64, full bool) {
	go func() {
		result := b.updates.Apply(full, func(p UpdateProgress) {
			state := "applying"
			if p.Stage == "downloading" {
				state = "downloading"
			}
			b.sendUpdate(map[string]any{
				"state": state, "stage": p.Stage, "percent": p.Percent,
				"received": p.Received, "total": p.Total, "message": p.Message,
			})
		})
		b.reply(id, "host", result, nil)
		if result.OK {
			b.sendUpdate(map[string]any{
				"state": "restarting", "percent": 100, "message": "更新已就绪，正在重启程序…",
			})
			b.mu.Lock()
			w := b.window
			b.mu.Unlock()
			if w != nil {
				// Give the WebView a moment to receive the result and paint the
				// "restarting" state before WM_CLOSE tears the bridge down.
				time.AfterFunc(1200*time.Millisecond, w.Close)
			}
		}
	}()
}

func (b *bridge) sendUpdate(payload map[string]any) {
	payload["kind"] = "update"
	b.send(payload)
}

// ---- backend state notifications ------------------------------------------

func (b *bridge) currentBackendState() (string, string) {
	b.backendStateMu.Lock()
	defer b.backendStateMu.Unlock()
	return b.backendState, b.backendMsg
}

func (b *bridge) setBackendState(state, message string) {
	b.backendStateMu.Lock()
	changed := b.backendState != state || b.backendMsg != message
	b.backendState = state
	b.backendMsg = message
	b.backendStateMu.Unlock()
	if changed {
		b.sendBackendStateFull(state, message)
	}
}

func (b *bridge) sendBackendStateFull(state, message string) {
	b.send(map[string]any{
		"kind":    "backend",
		"state":   state,
		"message": message,
	})
}

// ---- event batching -------------------------------------------------------

// eventBatcher coalesces backend SSE events into small batches so the renderer
// is not woken up thousands of times during parallel downloads.
type eventBatcher struct {
	b      *bridge
	mu     sync.Mutex
	events []map[string]any
	timer  *time.Timer
	closed bool
}

func newEventBatcher(b *bridge) *eventBatcher {
	return &eventBatcher{b: b}
}

func (e *eventBatcher) Push(ev map[string]any) {
	e.mu.Lock()
	if e.closed {
		e.mu.Unlock()
		return
	}
	e.events = append(e.events, ev)
	flushNow := len(e.events) >= 200
	if !flushNow && e.timer == nil {
		e.timer = time.AfterFunc(70*time.Millisecond, e.flush)
	}
	e.mu.Unlock()
	if flushNow {
		e.flush()
	}
}

func (e *eventBatcher) flush() {
	e.mu.Lock()
	events := e.events
	e.events = nil
	e.timer = nil
	e.mu.Unlock()
	if len(events) == 0 {
		return
	}
	e.b.send(map[string]any{"kind": "events.batch", "events": events})
}

func (e *eventBatcher) Close() {
	e.mu.Lock()
	e.closed = true
	if e.timer != nil {
		e.timer.Stop()
		e.timer = nil
	}
	e.events = nil
	e.mu.Unlock()
}
