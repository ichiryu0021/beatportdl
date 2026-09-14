// Package server exposes the Beatport downloader as a local HTTP/JSON-RPC
// service so that a GUI front-end (e.g. a Qt/C++ client) can drive the same
// download pipeline that the CLI uses. It wraps an *application instance,
// reusing its Beatport client, config and download workers instead of
// duplicating any logic.
//
// The server speaks a tiny JSON-RPC 2.0 subset over POST /rpc and streams
// progress events over GET /events (Server-Sent Events). It binds to
// 127.0.0.1 only; it is not intended to be exposed on a network.
package server

import (
	"context"
	"encoding/json"
	"fmt"
	"net"
	"net/http"
	"strings"
	"sync"
	"time"

	"unspok3n/beatportdl/config"
	"unspok3n/beatportdl/internal/beatport"
	"unspok3n/beatportdl/internal/integrity"
	"unspok3n/beatportdl/internal/scheduler"
	"unspok3n/beatportdl/internal/sorting"
)

// App is the minimal subset of *application (cmd/beatportdl) the server needs.
// Defining it as an interface lets the server live in its own package without
// an import cycle on package main.
type App interface {
	Beatport() *beatport.Beatport
	Config() *config.AppConfig
	// AuthReady 返回当前是否已认证。--serve 模式启动时若缓存失效且无凭据，
	// 返回 false，等前端调 Login 提交凭据。
	AuthReady() bool
	// Login 用前端提交的凭据做 Beatport 认证，成功后写回配置与缓存。
	Login(username, password string) error
	// Logout 清除配置中的凭据与本地认证缓存，并置为未认证。
	// 前端"退出登录"按钮调用，之后需重新 Login。
	Logout() error
	// SetDownloadsDirectory 校验并切换到新的下载目录（可写则生效并持久化，
	// 重建按曲风归档的 archiver）。目录不可写（如 Access denied）时返回错误。
	SetDownloadsDirectory(path string) error
	// DownloadsDirectory 返回当前配置的下载目录路径（用于设置页显示）。
	DownloadsDirectory() string
	// CheckDownloadsDirectory 探测目录是否可写（创建 + 写探针文件），
	// 不可写（如 Access denied）时返回错误；不修改配置，纯检查。
	CheckDownloadsDirectory(path string) error
	// Quality 返回当前下载音质（lossless/high/medium/medium-hls）。
	Quality() string
	// SetQuality 校验并持久化下载音质，对后续下载立即生效。
	SetQuality(quality string) error
	// TrackExists 返回本地文件已存在时的处理策略
	// （error/skip/overwrite/update）。
	TrackExists() string
	// SetTrackExists 校验并持久化"文件已存在"策略，对后续下载立即生效。
	SetTrackExists(mode string) error
	// TrackFileTemplate 返回当前曲目文件名模板（如 "{artists} - {name}"）。
	TrackFileTemplate() string
	// SetTrackFileTemplate 校验并持久化曲目文件名模板，对后续下载立即生效。
	SetTrackFileTemplate(template string) error
	// EnqueueURLs schedules the given beatport URLs/queries for download using
	// the existing worker pool. URLs already pending or currently being
	// downloaded are deduplicated. It must be safe to call repeatedly.
	// Returns the number of newly enqueued URLs and the number of duplicates
	// rejected (each broadcast as a download.skipped/reason=duplicate event).
	EnqueueURLs(urls []string) (enqueued, skipped int)
	// HandleQuery resolves a search query (non-URL input) the same way the
	// CLI's mainPrompt/search does, returning the URLs to enqueue. This is
	// optional; if nil, queries are enqueued verbatim and the CLI search
	// behavior is used.
}

// Server is the HTTP/JSON-RPC server.
type Server struct {
	app       App
	addr      string
	mux       *http.ServeMux
	srv       *http.Server
	checker   *integrity.Checker
	scheduler *scheduler.Scheduler
	schedMu   sync.Mutex

	// Progress event broker: clients connect to /events and receive JSON
	// objects describing download progress, completion and errors.
	broker *broker
}

// Option configures a Server.
type Option func(*Server)

// WithAddr overrides the default listen address 127.0.0.1:8765.
func WithAddr(addr string) Option {
	return func(s *Server) { s.addr = addr }
}

// New constructs a Server wrapping app.
func New(app App, opts ...Option) *Server {
	s := &Server{
		app:     app,
		addr:    "127.0.0.1:8765",
		checker: integrity.NewChecker(),
		broker:  newBroker(),
	}
	for _, o := range opts {
		o(s)
	}
	s.mux = http.NewServeMux()
	s.mux.HandleFunc("/rpc", s.handleRPC)
	s.mux.HandleFunc("/events", s.broker.serveHTTP)
	s.mux.HandleFunc("/health", func(w http.ResponseWriter, r *http.Request) {
		w.WriteHeader(http.StatusOK)
		_, _ = w.Write([]byte("ok"))
	})
	return s
}

// Addr returns the configured listen address.
func (s *Server) Addr() string { return s.addr }

// ListenAndServe starts the HTTP server. It blocks until the server stops.
func (s *Server) ListenAndServe() error {
	s.srv = &http.Server{
		Addr:              s.addr,
		Handler:           s.mux,
		ReadHeaderTimeout: 10 * time.Second,
	}
	return s.srv.ListenAndServe()
}

// Serve serves on an already-bound listener (used when the caller wants to
// fail fast on a busy port before spawning the download loop).
func (s *Server) Serve(ln net.Listener) error {
	s.srv = &http.Server{
		Addr:              s.addr,
		Handler:           s.mux,
		ReadHeaderTimeout: 10 * time.Second,
	}
	return s.srv.Serve(ln)
}

// Shutdown stops the HTTP server and any running scheduler.
func (s *Server) Shutdown(ctx context.Context) error {
	s.schedMu.Lock()
	if s.scheduler != nil {
		s.scheduler.Stop()
	}
	s.schedMu.Unlock()
	if s.srv != nil {
		return s.srv.Shutdown(ctx)
	}
	return nil
}

// --- JSON-RPC ---

type rpcRequest struct {
	Method string          `json:"method"`
	Params json.RawMessage `json:"params,omitempty"`
	ID     json.RawMessage `json:"id,omitempty"`
}

type rpcResponse struct {
	Result interface{}     `json:"result,omitempty"`
	Error  *rpcError       `json:"error,omitempty"`
	ID     json.RawMessage `json:"id"`
}

type rpcError struct {
	Code    int    `json:"code"`
	Message string `json:"message"`
}

func newErr(id json.RawMessage, code int, msg string) rpcResponse {
	return rpcResponse{Error: &rpcError{Code: code, Message: msg}, ID: id}
}

func (s *Server) handleRPC(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodPost {
		http.Error(w, "method not allowed", http.StatusMethodNotAllowed)
		return
	}
	var req rpcRequest
	if err := json.NewDecoder(r.Body).Decode(&req); err != nil {
		writeJSON(w, rpcResponse{Error: &rpcError{Code: -32700, Message: "parse error: " + err.Error()}})
		return
	}

	w.Header().Set("Content-Type", "application/json")
	var resp rpcResponse
	resp.ID = req.ID

	switch req.Method {
	case "search":
		resp = s.rpcSearch(req)
	case "getLatest":
		resp = s.rpcGetLatest(req)
	case "getGenres":
		resp = s.rpcGetGenres(req)
	case "download":
		resp = s.rpcDownload(req)
	case "setDownloadsDirectory":
		resp = s.rpcSetDownloadsDirectory(req)
	case "getDownloadsDirectory":
		resp = s.rpcGetDownloadsDirectory(req)
	case "getSettings":
		resp = s.rpcGetSettings(req)
	case "setQuality":
		resp = s.rpcSetQuality(req)
	case "setTrackExists":
		resp = s.rpcSetTrackExists(req)
	case "setTrackFileTemplate":
		resp = s.rpcSetTrackFileTemplate(req)
	case "login":
		resp = s.rpcLogin(req)
	case "logout":
		resp = s.rpcLogout(req)
	case "authStatus":
		resp = s.rpcAuthStatus(req)
	case "startScheduler":
		resp = s.rpcStartScheduler(req)
	case "stopScheduler":
		s.schedMu.Lock()
		if s.scheduler != nil {
			s.scheduler.Stop()
			s.scheduler = nil
		}
		s.schedMu.Unlock()
		resp = rpcResponse{Result: true, ID: req.ID}
	case "schedulerStatus":
		s.schedMu.Lock()
		running := s.scheduler != nil
		s.schedMu.Unlock()
		resp = rpcResponse{Result: map[string]bool{"running": running}, ID: req.ID}
	default:
		resp = newErr(req.ID, -32601, "method not found: "+req.Method)
	}
	writeJSON(w, resp)
}

func writeJSON(w http.ResponseWriter, v interface{}) {
	_ = json.NewEncoder(w).Encode(v)
}

// --- RPC method implementations ---

type searchParams struct {
	Query string `json:"query"`
}

type searchResultItem struct {
	Type    string `json:"type"`
	ID      int64  `json:"id"`
	Title   string `json:"title"`
	Artists string `json:"artists"`
	URL     string `json:"url"`
	Genre   string `json:"genre,omitempty"`
}

func (s *Server) rpcSearch(req rpcRequest) rpcResponse {
	var p searchParams
	if len(req.Params) > 0 {
		if err := json.Unmarshal(req.Params, &p); err != nil {
			return newErr(req.ID, -32602, "invalid params: "+err.Error())
		}
	}
	if p.Query == "" {
		return newErr(req.ID, -32602, "query is required")
	}
	results, err := s.app.Beatport().Search(p.Query)
	if err != nil {
		return newErr(req.ID, -32000, "search failed: "+err.Error())
	}
	out := make([]searchResultItem, 0, len(results.Tracks)+len(results.Releases)+len(results.Labels))
	for _, t := range results.Tracks {
		out = append(out, searchResultItem{
			Type: "track", ID: t.ID,
			Title:   t.Name.String() + " (" + t.MixName.String() + ")",
			Artists: t.Artists.Display(0, ""),
			URL:     t.URL,
			Genre:   t.Genre.Name,
		})
	}
	for _, r := range results.Releases {
		out = append(out, searchResultItem{
			Type: "release", ID: r.ID,
			Title:   r.Name.String(),
			Artists: r.Artists.Display(0, ""),
			URL:     r.URL,
		})
	}
	for _, l := range results.Labels {
		out = append(out, searchResultItem{
			Type:  "label",
			ID:    l.ID,
			Title: l.Name,
			URL:   l.StoreUrl(),
		})
	}
	return rpcResponse{Result: out, ID: req.ID}
}

type getLatestParams struct {
	GenreName string `json:"genre,omitempty"`
	// Genres 支持多选曲风（前端勾选多个时传入）；非空时优先于 GenreName，
	// 后端分别请求后按 URL 去重合并。
	Genres   []string `json:"genres,omitempty"`
	Page     int      `json:"page,omitempty"`
	PerPage  int      `json:"per_page,omitempty"`
	Kind     string   `json:"kind,omitempty"` // "tracks" (default) or "releases"
	DateFrom string   `json:"date_from,omitempty"`
	DateTo   string   `json:"date_to,omitempty"`
}

// latestResult 是 getLatest 的返回形态：results 为本页曲目，附带分页元信息，
// 供前端无限滚动判断是否还有下一页。
type latestResult struct {
	Results []map[string]interface{} `json:"results"`
	Page    int                      `json:"page"`
	PerPage int                      `json:"per_page"`
	Count   int                      `json:"count"`
	HasMore bool                     `json:"has_more"`
}

func (s *Server) rpcGetLatest(req rpcRequest) rpcResponse {
	var p getLatestParams
	if len(req.Params) > 0 {
		if err := json.Unmarshal(req.Params, &p); err != nil {
			return newErr(req.ID, -32602, "invalid params: "+err.Error())
		}
	}
	if p.Page <= 0 {
		p.Page = 1
	}
	if p.PerPage <= 0 || p.PerPage > 100 {
		p.PerPage = 50
	}
	genres := p.Genres
	if len(genres) == 0 && p.GenreName != "" {
		genres = []string{p.GenreName}
	}

	fetch := func(genre string) (*latestResult, error) {
		out := &latestResult{Page: p.Page, PerPage: p.PerPage, Results: []map[string]interface{}{}}
		if p.Kind == "releases" {
			res, err := s.app.Beatport().GetLatestReleases(genre, p.Page, p.PerPage, p.DateFrom, p.DateTo)
			if err != nil {
				return nil, err
			}
			out.Count = res.Count
			out.HasMore = res.Next != nil
			for _, r := range res.Results {
				out.Results = append(out.Results, toTrackMap(r.URL, r))
			}
			return out, nil
		}
		res, err := s.app.Beatport().GetLatestTracks(genre, p.Page, p.PerPage, p.DateFrom, p.DateTo)
		if err != nil {
			return nil, err
		}
		out.Count = res.Count
		out.HasMore = res.Next != nil
		for _, t := range res.Results {
			out.Results = append(out.Results, toTrackMap(t.URL, t))
		}
		return out, nil
	}

	if len(genres) <= 1 {
		genre := ""
		if len(genres) == 1 {
			genre = genres[0]
		}
		results, err := fetch(genre)
		if err != nil {
			return newErr(req.ID, -32000, "getLatest failed: "+err.Error())
		}
		return rpcResponse{Result: results, ID: req.ID}
	}

	// 多曲风：同一页码分别请求，按 URL 去重合并；任一曲风还有下一页则
	// has_more=true（count 为各曲风总数之和，仅作参考）。
	merged := &latestResult{Page: p.Page, PerPage: p.PerPage, Results: []map[string]interface{}{}}
	seen := make(map[string]struct{})
	for _, g := range genres {
		results, err := fetch(g)
		if err != nil {
			return newErr(req.ID, -32000, "getLatest failed for genre "+g+": "+err.Error())
		}
		merged.Count += results.Count
		merged.HasMore = merged.HasMore || results.HasMore
		for _, m := range results.Results {
			key, _ := m["url"].(string)
			if _, dup := seen[key]; dup {
				continue
			}
			seen[key] = struct{}{}
			merged.Results = append(merged.Results, m)
		}
	}
	return rpcResponse{Result: merged, ID: req.ID}
}

// toTrackMap converts a beatport Track/Release into the map shape the GUI
// expects ({url, name, artists:[{name}], genre, mix_name, ...}). Values are
// left as their original struct fields where possible.
func toTrackMap(url string, v interface{}) map[string]interface{} {
	switch t := v.(type) {
	case beatport.Track:
		return map[string]interface{}{
			"id":       t.ID,
			"url":      url,
			"name":     t.Name.String(),
			"mix_name": t.MixName.String(),
			"artists":  []map[string]interface{}{{"name": t.Artists.Display(0, "")}},
			"genre":    map[string]interface{}{"name": t.Genre.Name},
		}
	case beatport.Release:
		return map[string]interface{}{
			"id":      t.ID,
			"url":     url,
			"name":    t.Name.String(),
			"artists": []map[string]interface{}{{"name": t.Artists.Display(0, "")}},
		}
	}
	return map[string]interface{}{"url": url}
}

func (s *Server) rpcGetGenres(req rpcRequest) rpcResponse {
	// 优先爬官网曲风导航（无需登录，列表与官网一致）；失败回退 catalog API。
	genres, err := s.app.Beatport().ScrapeGenres()
	if err == nil && len(genres) > 0 {
		return rpcResponse{Result: genres, ID: req.ID}
	}
	res, err := s.app.Beatport().GetGenres()
	if err != nil {
		return newErr(req.ID, -32000, "getGenres failed: "+err.Error())
	}
	return rpcResponse{Result: res.Results, ID: req.ID}
}

type loginParams struct {
	Username string `json:"username"`
	Password string `json:"password"`
}

// rpcLogin 接收前端提交的凭据，调 App.Login 做 Beatport OAuth 登录。
// 成功后凭据写回配置文件，缓存写回同目录，后续请求即视为已认证。
// 失败时返回详细错误（凭据错误/网络/2FA 等），前端可提示用户重试。
func (s *Server) rpcLogin(req rpcRequest) rpcResponse {
	var p loginParams
	if len(req.Params) > 0 {
		if err := json.Unmarshal(req.Params, &p); err != nil {
			return newErr(req.ID, -32602, "invalid params: "+err.Error())
		}
	}
	if p.Username == "" || p.Password == "" {
		return newErr(req.ID, -32602, "username and password are required")
	}
	if err := s.app.Login(p.Username, p.Password); err != nil {
		return newErr(req.ID, -32000, "login failed: "+err.Error())
	}
	return rpcResponse{Result: map[string]bool{"ok": true}, ID: req.ID}
}

// rpcLogout 清除本地凭据与缓存并置为未认证。前端"退出登录"按钮调用，
// 之后 GUI 应回到登录对话框。广播 auth.loggedOut 事件以便前端刷新状态。
func (s *Server) rpcLogout(req rpcRequest) rpcResponse {
	if err := s.app.Logout(); err != nil {
		return newErr(req.ID, -32000, "logout failed: "+err.Error())
	}
	s.broker.broadcast(map[string]interface{}{"event": "auth.loggedOut"})
	return rpcResponse{Result: map[string]bool{"ok": true}, ID: req.ID}
}

// rpcAuthStatus 返回当前认证状态，前端启动时先查这个决定是否显示登录页。
type authStatusResult struct {
	Ready bool   `json:"ready"`
	Err   string `json:"error,omitempty"`
}

func (s *Server) rpcAuthStatus(req rpcRequest) rpcResponse {
	st := authStatusResult{Ready: s.app.AuthReady()}
	return rpcResponse{Result: st, ID: req.ID}
}

type downloadParams struct {
	URLs     []string `json:"urls"`
	GenreDir bool     `json:"genre_dir,omitempty"` // when true, file completed tracks into genre subfolders
}

type setDownloadsDirectoryParams struct {
	Path string `json:"path"`
}

// rpcSetDownloadsDirectory 切换下载目录。后端会先尝试创建并写探针文件，
// 目录不可写（如 Access denied）时返回明确错误，前端据此提示用户换目录。
func (s *Server) rpcSetDownloadsDirectory(req rpcRequest) rpcResponse {
	var p setDownloadsDirectoryParams
	if len(req.Params) > 0 {
		if err := json.Unmarshal(req.Params, &p); err != nil {
			return newErr(req.ID, -32602, "invalid params: "+err.Error())
		}
	}
	if strings.TrimSpace(p.Path) == "" {
		return newErr(req.ID, -32602, "path is required")
	}
	if err := s.app.SetDownloadsDirectory(strings.TrimSpace(p.Path)); err != nil {
		return newErr(req.ID, -32000, "set downloads directory: "+err.Error())
	}
	return rpcResponse{Result: map[string]string{"path": p.Path}, ID: req.ID}
}

// rpcGetDownloadsDirectory 返回当前配置的下载目录及其可写性。前端设置页
// 启动时用它显示当前目录；不可写（Access denied 等）时 writable=false 并
// 附带原因，GUI 据此预警并引导用户换目录。
func (s *Server) rpcGetDownloadsDirectory(req rpcRequest) rpcResponse {
	path := s.app.DownloadsDirectory()
	result := map[string]interface{}{"path": path, "writable": true}
	if path != "" {
		if err := s.app.CheckDownloadsDirectory(path); err != nil {
			result["writable"] = false
			result["error"] = err.Error()
		}
	}
	return rpcResponse{Result: result, ID: req.ID}
}

// rpcGetSettings 返回设置页需要的全部持久化选项（目前是下载目录、可写性、
// 音质、文件已存在策略），GUI 一次调用即可填充设置页。
func (s *Server) rpcGetSettings(req rpcRequest) rpcResponse {
	path := s.app.DownloadsDirectory()
	result := map[string]interface{}{
		"downloads_directory": path,
		"writable":            true,
		"quality":             s.app.Quality(),
		"track_exists":        s.app.TrackExists(),
		"track_file_template": s.app.TrackFileTemplate(),
	}
	if path != "" {
		if err := s.app.CheckDownloadsDirectory(path); err != nil {
			result["writable"] = false
			result["error"] = err.Error()
		}
	}
	return rpcResponse{Result: result, ID: req.ID}
}

type setQualityParams struct {
	Quality string `json:"quality"`
}

// rpcSetQuality 切换下载音质（lossless FLAC / high AAC / medium AAC /
// medium-hls AAC）。校验合法值并持久化配置，后续下载立即按新音质进行。
func (s *Server) rpcSetQuality(req rpcRequest) rpcResponse {
	var p setQualityParams
	if len(req.Params) > 0 {
		if err := json.Unmarshal(req.Params, &p); err != nil {
			return newErr(req.ID, -32602, "invalid params: "+err.Error())
		}
	}
	if err := s.app.SetQuality(strings.TrimSpace(p.Quality)); err != nil {
		return newErr(req.ID, -32000, "set quality: "+err.Error())
	}
	return rpcResponse{Result: map[string]string{"quality": p.Quality}, ID: req.ID}
}

type setTrackExistsParams struct {
	Mode string `json:"mode"`
}

// rpcSetTrackExists 切换本地文件已存在时的处理策略（error/skip/overwrite/
// update）。校验合法值并持久化配置，后续下载立即按新策略处理。
func (s *Server) rpcSetTrackExists(req rpcRequest) rpcResponse {
	var p setTrackExistsParams
	if len(req.Params) > 0 {
		if err := json.Unmarshal(req.Params, &p); err != nil {
			return newErr(req.ID, -32602, "invalid params: "+err.Error())
		}
	}
	mode := strings.TrimSpace(p.Mode)
	if mode == "" {
		return newErr(req.ID, -32602, "mode is required")
	}
	if err := s.app.SetTrackExists(mode); err != nil {
		return newErr(req.ID, -32000, "set track exists: "+err.Error())
	}
	return rpcResponse{Result: map[string]string{"mode": mode}, ID: req.ID}
}

type setTrackFileTemplateParams struct {
	Template string `json:"template"`
}

// rpcSetTrackFileTemplate 持久化曲目文件名模板（由设置页的命名段勾选生成），
// 后续下载立即按新模板命名。模板必须包含 {name}。
func (s *Server) rpcSetTrackFileTemplate(req rpcRequest) rpcResponse {
	var p setTrackFileTemplateParams
	if len(req.Params) > 0 {
		if err := json.Unmarshal(req.Params, &p); err != nil {
			return newErr(req.ID, -32602, "invalid params: "+err.Error())
		}
	}
	if err := s.app.SetTrackFileTemplate(p.Template); err != nil {
		return newErr(req.ID, -32000, "set track file template: "+err.Error())
	}
	return rpcResponse{Result: map[string]string{"template": p.Template}, ID: req.ID}
}

// download is fire-and-forget: it enqueues URLs on the app's existing worker
// pool and returns immediately. Progress is streamed via /events.
func (s *Server) rpcDownload(req rpcRequest) rpcResponse {
	var p downloadParams
	if len(req.Params) > 0 {
		if err := json.Unmarshal(req.Params, &p); err != nil {
			return newErr(req.ID, -32602, "invalid params: "+err.Error())
		}
	}
	if len(p.URLs) == 0 {
		return newErr(req.ID, -32602, "urls is required")
	}
	enqueued, skipped := s.app.EnqueueURLs(p.URLs)
	return rpcResponse{Result: map[string]int{"enqueued": enqueued, "skipped": skipped}, ID: req.ID}
}

type startSchedulerParams struct {
	GenreName       string `json:"genre,omitempty"`
	IntervalSeconds int    `json:"interval_seconds,omitempty"`
	PerPage         int    `json:"per_page,omitempty"`
	DateFrom        string `json:"date_from,omitempty"`
	DateTo          string `json:"date_to,omitempty"`
}

func (s *Server) rpcStartScheduler(req rpcRequest) rpcResponse {
	var p startSchedulerParams
	if len(req.Params) > 0 {
		if err := json.Unmarshal(req.Params, &p); err != nil {
			return newErr(req.ID, -32602, "invalid params: "+err.Error())
		}
	}
	if p.IntervalSeconds <= 0 {
		p.IntervalSeconds = 3600
	}
	s.schedMu.Lock()
	if s.scheduler != nil {
		s.scheduler.Stop()
	}
	opts := []scheduler.Option{
		scheduler.WithLogger(func(msg string) { s.broker.broadcast(map[string]interface{}{"event": "log", "message": msg}) }),
	}
	if p.GenreName != "" {
		opts = append(opts, scheduler.WithGenre(p.GenreName))
	}
	if p.PerPage > 0 {
		opts = append(opts, scheduler.WithPerPage(p.PerPage))
	}
	if p.DateFrom != "" || p.DateTo != "" {
		opts = append(opts, scheduler.WithDateRange(p.DateFrom, p.DateTo))
	}
	opts = append(opts, scheduler.WithResultHandler(func(tracks []beatport.Track) error {
		urls := make([]string, 0, len(tracks))
		for _, t := range tracks {
			if t.URL != "" {
				urls = append(urls, t.URL)
			}
		}
		if len(urls) > 0 {
			s.broker.broadcast(map[string]interface{}{"event": "scheduler.found", "count": len(urls)})
			s.app.EnqueueURLs(urls)
		}
		return nil
	}))
	s.scheduler = scheduler.NewScheduler(s.app.Beatport(), time.Duration(p.IntervalSeconds)*time.Second, opts...)
	s.scheduler.Start()
	s.schedMu.Unlock()
	return rpcResponse{Result: map[string]bool{"started": true}, ID: req.ID}
}

// --- SSE broker ---

type broker struct {
	mu      sync.RWMutex
	clients map[chan []byte]struct{}
}

func newBroker() *broker {
	return &broker{clients: make(map[chan []byte]struct{})}
}

func (b *broker) broadcast(payload map[string]interface{}) {
	data, err := json.Marshal(payload)
	if err != nil {
		return
	}
	b.mu.RLock()
	defer b.mu.RUnlock()
	for ch := range b.clients {
		select {
		case ch <- data:
		default: // drop event if client is slow
		}
	}
}

func (b *broker) serveHTTP(w http.ResponseWriter, r *http.Request) {
	w.Header().Set("Content-Type", "text/event-stream")
	w.Header().Set("Cache-Control", "no-cache")
	w.Header().Set("Connection", "keep-alive")
	flusher, ok := w.(http.Flusher)
	if !ok {
		http.Error(w, "streaming unsupported", http.StatusInternalServerError)
		return
	}
	ch := make(chan []byte, 16)
	b.mu.Lock()
	b.clients[ch] = struct{}{}
	b.mu.Unlock()
	defer func() {
		b.mu.Lock()
		delete(b.clients, ch)
		b.mu.Unlock()
	}()
	fmt.Fprint(w, ": connected\n\n")
	flusher.Flush()
	for {
		select {
		case <-r.Context().Done():
			return
		case data, ok := <-ch:
			if !ok {
				return
			}
			fmt.Fprintf(w, "data: %s\n\n", data)
			flusher.Flush()
		}
	}
}

// --- helpers used by the App adapter to push progress ---

// PublishProgress is called by the App adapter (in cmd/beatportdl) to push a
// download progress event to all connected GUI clients. The payload shape is
// agreed between front-end and back-end; see the GUI docs.
func (s *Server) PublishProgress(payload map[string]interface{}) {
	s.broker.broadcast(payload)
}

// GenreArchiver builds a sorting.Archiver rooted at the app's downloads dir.
// Exposed so the App adapter can file finished tracks into genre folders when
// the user opted into that behavior.
func (s *Server) GenreArchiver(mode sorting.Mode) *sorting.Archiver {
	cfg := s.app.Config()
	ws := ""
	if cfg != nil {
		ws = cfg.WhitespaceCharacter
	}
	return sorting.NewArchiver(cfg.DownloadsDirectory, mode, ws)
}

// Checker exposes the integrity checker so the App adapter can verify files.
func (s *Server) Checker() *integrity.Checker { return s.checker }
