package main

import (
	"context"
	"fmt"
	"net"
	"os"
	"path/filepath"
	"strings"
	"sync"
	"time"

	"unspok3n/beatportdl/config"
	"unspok3n/beatportdl/internal/beatport"
	"unspok3n/beatportdl/internal/server"
	"unspok3n/beatportdl/internal/sorting"
)

// serverApp adapts *application to the server.App interface. It lives in
// package main so it can read application's unexported fields directly.
//
// EnqueueURLS is concurrency-safe: pendingURLs and the main loop's active
// batch (app.urls) are both guarded by urlsMu, so URLs pushed from RPC
// handlers are deduplicated against queued/in-flight work and picked up on
// the next loop iteration without racing the loop's app.urls reset.
type serverApp struct {
	app *application
	srv *server.Server

	urlsMu sync.Mutex
	// pendingURLS accumulates URLs enqueued by RPC handlers between main-loop
	// iterations. The loop drains it via drainAndActivate under urlsMu.
	pendingURLs []string
}

// Compile-time check that serverApp implements server.App.
var _ server.App = (*serverApp)(nil)

func (a *serverApp) Beatport() *beatport.Beatport { return a.app.bp }
func (a *serverApp) Config() *config.AppConfig    { return a.app.config }

// AuthReady 返回认证状态。--serve 模式启动时若缓存失效且无凭据，会保持
// false，等前端调 Login 提交凭据。
func (a *serverApp) AuthReady() bool {
	a.app.authMu.RLock()
	defer a.app.authMu.RUnlock()
	return a.app.authReady
}

// Login 用前端提交的用户名/密码做 Beatport 认证。成功后写回配置文件，
// 这样进程重启后能从缓存直接读。线程安全：auth.Init 内部有锁。
func (a *serverApp) Login(username, password string) error {
	if username == "" || password == "" {
		return fmt.Errorf("username and password are required")
	}
	// 更新 Auth 的凭据并尝试登录。NewAuth 会把凭据存到 auth 结构里，
	// Init 用它走 OAuth 登录流程。
	auth := beatport.NewAuth(username, password, a.app.auth.CacheFile())
	if err := auth.Init(a.app.bp); err != nil {
		return fmt.Errorf("login failed: %w", err)
	}
	// 成功：把新 auth 装回 application 与 bp，写回配置。
	a.app.authMu.Lock()
	a.app.auth = auth
	a.app.bp.SetAuth(auth)
	a.app.config.Username = username
	a.app.config.Password = password
	a.app.authReady = true
	a.app.authMu.Unlock()

	// 持久化到配置文件，重启后可从缓存直接登录。
	if a.app.configPath != "" {
		if err := a.app.config.Save(a.app.configPath); err != nil {
			// 写不回去不影响当前会话，只记录。
			a.app.LogInfo("server: save config after login: " + err.Error())
		}
	}
	return nil
}

// Logout 清除配置凭据与本地认证缓存并置为未认证。之后 authStatus 返回
// ready=false，前端应回到登录对话框。
func (a *serverApp) Logout() error {
	a.app.authMu.Lock()
	defer a.app.authMu.Unlock()

	a.app.config.Username = ""
	a.app.config.Password = ""
	if a.app.configPath != "" {
		if err := a.app.config.Save(a.app.configPath); err != nil {
			// 写不回去只记录；当前会话已按未认证处理。
			a.app.LogInfo("server: save config after logout: " + err.Error())
		}
	}

	// 删除本地认证缓存（OAuth token），下次启动需重新登录。
	if a.app.auth != nil {
		cachePath := a.app.auth.CacheFile()
		if cachePath != "" {
			if err := os.Remove(cachePath); err != nil && !os.IsNotExist(err) {
				a.app.LogInfo("server: remove auth cache: " + err.Error())
			}
		}
	}

	a.app.authReady = false
	return nil
}

// SetDownloadsDirectory 切换下载目录：先创建并写探针文件验证可写性
// （Access denied 等权限问题在此暴露为明确错误），成功后更新配置并重建
// 按曲风归档的 archiver。
func (a *serverApp) SetDownloadsDirectory(path string) error {
	if err := os.MkdirAll(path, 0700); err != nil {
		return fmt.Errorf("cannot create %s: %w", path, err)
	}
	probe := filepath.Join(path, ".bpdl-write-test")
	if err := os.WriteFile(probe, []byte("ok"), 0600); err != nil {
		return fmt.Errorf("not writable: %w", err)
	}
	_ = os.Remove(probe)

	a.app.config.DownloadsDirectory = path
	if a.app.configPath != "" {
		if err := a.app.config.Save(a.app.configPath); err != nil {
			// 写不回去只记录；当前会话已生效。
			a.app.LogInfo("server: save config after set downloads dir: " + err.Error())
		}
	}
	if a.app.config.SortByGenre {
		mode := sorting.ModeGenre
		if a.app.config.GenreSortMode == "subgenre_or_genre" {
			mode = sorting.ModeSubgenreOrGenre
		}
		a.app.archiver = sorting.NewArchiver(path, mode, a.app.config.WhitespaceCharacter)
	}
	return nil
}

// DownloadsDirectory 返回当前配置的下载目录路径。
func (a *serverApp) DownloadsDirectory() string {
	return a.app.config.DownloadsDirectory
}

// Quality 返回当前下载音质。
func (a *serverApp) Quality() string {
	return a.app.config.Quality
}

// SetQuality 校验并切换下载音质（lossless/high/medium/medium-hls），
// 持久化到配置文件；下载流程在每次下载时实时读取，切换立即生效。
func (a *serverApp) SetQuality(quality string) error {
	switch quality {
	case "lossless", "high", "medium", "medium-hls":
	default:
		return fmt.Errorf("unsupported quality %q (want lossless/high/medium/medium-hls)", quality)
	}
	if quality == "medium-hls" && !config.FFMPEGInstalled() {
		return fmt.Errorf("quality medium-hls requires ffmpeg, which was not found in PATH")
	}
	a.app.config.Quality = quality
	if a.app.configPath != "" {
		if err := a.app.config.Save(a.app.configPath); err != nil {
			return fmt.Errorf("save config after set quality: %w", err)
		}
	}
	return nil
}

// TrackExists 返回本地音频文件已存在时的处理策略
// （error/skip/overwrite/update）。
func (a *serverApp) TrackExists() string {
	return a.app.config.TrackExists
}

// SetTrackExists 校验并切换"文件已存在"策略，持久化到配置文件；下载流程在
// 每次 saveTrack 时实时读取，切换立即生效。
func (a *serverApp) SetTrackExists(mode string) error {
	for _, opt := range config.SupportedTrackExistsOptions {
		if mode != opt {
			continue
		}
		a.app.config.TrackExists = mode
		if a.app.configPath != "" {
			if err := a.app.config.Save(a.app.configPath); err != nil {
				return fmt.Errorf("save config after set track exists: %w", err)
			}
		}
		return nil
	}
	return fmt.Errorf("unsupported track_exists mode %q (want one of %s)",
		mode, strings.Join(config.SupportedTrackExistsOptions, ", "))
}

// TrackFileTemplate 返回当前曲目文件名模板。
func (a *serverApp) TrackFileTemplate() string {
	return a.app.config.TrackFileTemplate
}

// SetTrackFileTemplate 校验并持久化曲目文件名模板。必须包含 {name}（曲目标题），
// 否则不同曲目会得到相同文件名而互相覆盖/跳过。下载流程实时读取，切换立即生效。
func (a *serverApp) SetTrackFileTemplate(tmpl string) error {
	tmpl = strings.TrimSpace(tmpl)
	if tmpl == "" {
		return fmt.Errorf("track file template is empty")
	}
	if !strings.Contains(tmpl, "{name}") {
		return fmt.Errorf("track file template %q must contain {name}", tmpl)
	}
	a.app.config.TrackFileTemplate = tmpl
	if a.app.configPath != "" {
		if err := a.app.config.Save(a.app.configPath); err != nil {
			return fmt.Errorf("save config after set track file template: %w", err)
		}
	}
	return nil
}

// CheckDownloadsDirectory 只做可写性探测（创建目录 + 写探针文件），不改配置。
// 与 SetDownloadsDirectory 的校验逻辑一致，供 getDownloadsDirectory RPC 用。
func (a *serverApp) CheckDownloadsDirectory(path string) error {
	if err := os.MkdirAll(path, 0700); err != nil {
		return fmt.Errorf("cannot create %s: %w", path, err)
	}
	probe := filepath.Join(path, ".bpdl-write-test")
	if err := os.WriteFile(probe, []byte("ok"), 0600); err != nil {
		return fmt.Errorf("not writable: %w", err)
	}
	_ = os.Remove(probe)
	return nil
}

// EnqueueURLs hands URLs to the download pipeline. It never blocks on the
// worker pool; the main loop picks them up on its next iteration.
//
// URLs are deduplicated against three sources: duplicates inside this call,
// URLs already waiting in pendingURLs, and URLs of the batch currently being
// downloaded (app.urls). A URL is downloadable again once its whole batch has
// finished and the loop has reset app.urls — there is no session-wide
// blacklist. Rejected duplicates are announced via download.skipped events.
// Returns (newly enqueued count, duplicate count).
func (a *serverApp) EnqueueURLs(urls []string) (int, int) {
	if len(urls) == 0 {
		return 0, 0
	}
	skipped := make([]string, 0)
	enqueued := 0

	// All dedup decisions are made under urlsMu together with the pending
	// append, so concurrent RPC calls and the main loop's drain/reset of
	// app.urls can never interleave with this check.
	a.urlsMu.Lock()
	seen := make(map[string]struct{}, len(a.pendingURLs)+len(a.app.urls)+len(urls))
	for _, u := range a.pendingURLs {
		seen[u] = struct{}{}
	}
	for _, u := range a.app.urls {
		seen[u] = struct{}{}
	}
	for _, u := range urls {
		if _, dup := seen[u]; dup {
			skipped = append(skipped, u)
			continue
		}
		seen[u] = struct{}{}
		a.pendingURLs = append(a.pendingURLs, u)
		enqueued++
	}
	a.urlsMu.Unlock()

	// Broadcast outside the lock: the SSE broker only touches its own mutex,
	// but keeping the publish out of the critical section avoids any lock
	// ordering surprises and holding urlsMu during client I/O.
	for _, u := range skipped {
		a.app.publishDownload("download.skipped", u, "",
			map[string]interface{}{"reason": "duplicate"})
	}
	if enqueued > 0 {
		a.app.LogInfo(fmt.Sprintf("server: enqueued %d urls (%d duplicates skipped)", enqueued, len(skipped)))
	}
	return enqueued, len(skipped)
}

// drainAndActivate moves RPC-enqueued URLs into the active batch (app.urls)
// under urlsMu and returns a snapshot copy the loop can dispatch on. Doing
// the merge and snapshot under the same lock serializes them against
// EnqueueURLs' dedup reads. Must not be called while urlsMu is already held.
func (a *serverApp) drainAndActivate() []string {
	a.urlsMu.Lock()
	defer a.urlsMu.Unlock()
	a.app.urls = append(a.app.urls, a.pendingURLs...)
	a.pendingURLs = nil
	return append([]string(nil), a.app.urls...)
}

// resetActiveURLs clears the finished batch so its URLs become enqueueable
// again. Only the main loop calls it, always after the batch's workers have
// fully returned (wg.Wait), and it races EnqueueURLs safely via urlsMu.
func (a *serverApp) resetActiveURLs() {
	a.urlsMu.Lock()
	a.app.urls = a.app.urls[:0]
	a.urlsMu.Unlock()
}

// runServer starts the JSON-RPC/SSE server and a serve-mode download loop.
// Unlike the CLI loop, when there are no URLs to process it sleeps briefly
// instead of prompting on stdin, so it can run headless alongside the GUI.
func runServer(app *application, addr string) error {
	adapter := &serverApp{app: app}
	srv := server.New(adapter, server.WithAddr(addr))
	adapter.srv = srv
	app.srv = srv // expose so downloader hooks can PublishProgress

	// 同步绑定端口：若已有实例占用（如用户重复双击 run-gui.bat），立即返回
	// 错误让进程退出，避免出现一个 RPC 不可用、却仍空跑下载循环的僵尸进程。
	ln, err := net.Listen("tcp", addr)
	if err != nil {
		return fmt.Errorf("cannot listen on %s: %w (another beatportdl instance is already running)", addr, err)
	}

	go func() {
		app.LogInfo("server: listening on " + srv.Addr())
		if err := srv.Serve(ln); err != nil && err.Error() != "http: Server closed" {
			app.LogInfo("server: " + err.Error())
		}
	}()

	ctx := app.ctx
	for {
		// Pull any URLs enqueued via RPC into the active list. The merge and
		// snapshot happen under urlsMu so EnqueueURLs' dedup check against
		// app.urls/pendingURLs cannot race this loop.
		urls := adapter.drainAndActivate()

		if len(urls) == 0 {
			select {
			case <-ctx.Done():
				return ctx.Err()
			case <-time.After(250 * time.Millisecond):
			}
			continue
		}

		app.activeFiles = make(map[string]struct{}, len(urls))
		for _, u := range urls {
			u := u
			app.globalWorker(func() { app.handleUrl(u) })
		}
		app.wg.Wait()

		// Batch fully done: release its URLs so the same URLs may be enqueued
		// again. URLs that arrived during processing stay in pendingURLs and
		// are merged on the next iteration.
		adapter.resetActiveURLs()

		select {
		case <-ctx.Done():
			return ctx.Err()
		default:
		}
	}
}

// PublishProgress forwards a progress event to connected GUI clients. Safe to
// call when no server is running (no-op then).
func (app *application) PublishProgress(payload map[string]interface{}) {
	if app.srv != nil {
		app.srv.PublishProgress(payload)
	}
}

// serverShutdown stops the HTTP server (if running). Called from the main
// shutdown path.
func (app *application) serverShutdown(ctx context.Context) {
	if app.srv != nil {
		_ = app.srv.Shutdown(ctx)
	}
}
