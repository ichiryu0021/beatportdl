//go:build windows

package desktop

import (
	"archive/zip"
	"context"
	"encoding/json"
	"errors"
	"flag"
	"fmt"
	"io"
	"log"
	"net/http"
	"net/url"
	"os"
	"os/exec"
	"path/filepath"
	"strconv"
	"strings"
	"sync"
	"syscall"
	"time"

	"golang.org/x/sys/windows"
	"unspok3n/beatportdl/internal/winui"
)

// Update source and protocol. The release repo is deliberately a constant so a
// copied/renamed installation cannot be tricked into downloading from an
// arbitrary host. Change it here (and publish there) if the project moves.
const (
	DefaultUpdateRepo = "ichiryu0021/beatportdl"
	updateAPIBase     = "https://api.github.com"
	updateUserAgent   = "BeatportDL-Updater"
	updateCacheFor    = 10 * time.Minute
	maxUpdateDownload = 256 << 20
	maxUpdateExtract  = 512 << 20
)

// UpdateInfo is the JSON shape sent to the renderer.
type UpdateInfo struct {
	Current     string `json:"current"`
	Latest      string `json:"latest"`
	HasUpdate   bool   `json:"hasUpdate"`
	Name        string `json:"name"`
	Notes       string `json:"notes"`
	PublishedAt string `json:"publishedAt"`
	ReleaseURL  string `json:"releaseUrl"`
	Repo        string `json:"repo"`
	Asset       string `json:"asset"`
	FullAsset   string `json:"fullAsset"`
	CheckedAt   string `json:"checkedAt"`
}

// UpdateProgress is streamed to the renderer while an update is applied.
type UpdateProgress struct {
	Stage    string `json:"stage"`
	Percent  int    `json:"percent"`
	Received int64  `json:"received"`
	Total    int64  `json:"total"`
	Message  string `json:"message"`
}

// ApplyResult mirrors the adjacent updater's "ok/canFull" shape so the UI can
// offer the full package after a failed minimal update.
type ApplyResult struct {
	OK         bool   `json:"ok"`
	Version    string `json:"version,omitempty"`
	Restarting bool   `json:"restarting,omitempty"`
	Code       string `json:"code,omitempty"`
	Message    string `json:"message,omitempty"`
	CanFull    bool   `json:"canFull,omitempty"`
}

type updateAsset struct {
	Name string
	URL  string
	Size int64
}

type ghAsset struct {
	Name               string `json:"name"`
	BrowserDownloadURL string `json:"browser_download_url"`
	Size               int64  `json:"size"`
}

type ghRelease struct {
	TagName     string    `json:"tag_name"`
	Name        string    `json:"name"`
	Body        string    `json:"body"`
	HTMLURL     string    `json:"html_url"`
	PublishedAt string    `json:"published_at"`
	Assets      []ghAsset `json:"assets"`
}

type updateManifest struct {
	App     string   `json:"app"`
	Version string   `json:"version"`
	BuiltAt string   `json:"builtAt"`
	Files   []string `json:"files"`
}

type updateManager struct {
	mu     sync.Mutex
	paths  Paths
	log    *log.Logger
	client *http.Client
	repo   string
	info   UpdateInfo
	hotfix updateAsset
	full   updateAsset

	checkedAt time.Time
	lastErr   error
	applying  bool
}

func newUpdateManager(paths Paths, logger *log.Logger) *updateManager {
	repo := DefaultUpdateRepo
	cleanupOldUpdateDirs()
	return &updateManager{
		paths:  paths,
		log:    logger,
		client: newUpdateHTTPClient(paths),
		repo:   repo,
		info:   UpdateInfo{Current: Version, Repo: repo},
	}
}

func newUpdateHTTPClient(paths Paths) *http.Client {
	transport := http.DefaultTransport.(*http.Transport).Clone()
	raw := readConfigString(paths.ConfigFile, "proxy")
	normalized, err := normalizeProxy(raw)
	if err == nil && normalized != "" {
		if u, parseErr := url.Parse(normalized); parseErr == nil {
			transport.Proxy = http.ProxyURL(u)
		}
	} else {
		transport.Proxy = http.ProxyFromEnvironment
	}
	return &http.Client{
		Transport: transport,
		// No global timeout: a full package can be ~30 MB on a slow link.
		CheckRedirect: func(req *http.Request, via []*http.Request) error {
			if len(via) >= 10 {
				return errors.New("更新下载重定向次数过多")
			}
			host := strings.ToLower(req.URL.Hostname())
			if host == "github.com" || host == "api.github.com" || strings.HasSuffix(host, ".githubusercontent.com") {
				return nil
			}
			return fmt.Errorf("更新下载被重定向到不受信任的主机：%s", host)
		},
	}
}

func updateAPIURL(repo string) string {
	return fmt.Sprintf("%s/repos/%s/releases/latest", updateAPIBase, repo)
}

// Check queries the latest GitHub release. Results are cached for ten minutes
// unless force is set.
func (u *updateManager) Check(force bool) (UpdateInfo, error) {
	u.mu.Lock()
	defer u.mu.Unlock()
	if u.applying {
		return u.info, errors.New("正在更新，请稍候")
	}
	if !force && !u.checkedAt.IsZero() && time.Since(u.checkedAt) < updateCacheFor {
		return u.info, u.lastErr
	}
	// Rebuild the client so a proxy saved in Settings takes effect immediately.
	u.client = newUpdateHTTPClient(u.paths)

	ctx, cancel := context.WithTimeout(context.Background(), 25*time.Second)
	defer cancel()
	req, err := http.NewRequestWithContext(ctx, http.MethodGet, updateAPIURL(u.repo), nil)
	if err != nil {
		return u.info, err
	}
	req.Header.Set("User-Agent", updateUserAgent)
	req.Header.Set("Accept", "application/vnd.github+json")
	resp, err := u.client.Do(req)
	if err != nil {
		u.lastErr = fmt.Errorf("连接 GitHub 失败：%w", err)
		return u.info, u.lastErr
	}
	defer resp.Body.Close()
	body, _ := io.ReadAll(io.LimitReader(resp.Body, 4<<20))
	if resp.StatusCode != http.StatusOK {
		u.lastErr = fmt.Errorf("GitHub 返回 HTTP %d（检查更新失败）", resp.StatusCode)
		if snippet := strings.TrimSpace(string(body)); snippet != "" && len(snippet) < 300 {
			u.lastErr = fmt.Errorf("%w：%s", u.lastErr, snippet)
		}
		return u.info, u.lastErr
	}
	var release ghRelease
	if err := json.Unmarshal(body, &release); err != nil {
		u.lastErr = fmt.Errorf("解析 GitHub Release 失败：%w", err)
		return u.info, u.lastErr
	}
	latest := strings.TrimSpace(strings.TrimPrefix(strings.TrimSpace(release.TagName), "v"))
	if latest == "" {
		latest = strings.TrimSpace(strings.TrimPrefix(strings.TrimSpace(release.Name), "v"))
	}
	if latest == "" {
		u.lastErr = errors.New("最新 Release 没有版本号")
		return u.info, u.lastErr
	}

	hotfix, full := selectUpdateAssets(release.Assets)
	info := UpdateInfo{
		Current:     Version,
		Latest:      latest,
		Name:        release.Name,
		Notes:       truncateText(strings.TrimSpace(release.Body), 24000),
		PublishedAt: release.PublishedAt,
		ReleaseURL:  release.HTMLURL,
		Repo:        u.repo,
		Asset:       hotfix.Name,
		FullAsset:   full.Name,
		CheckedAt:   time.Now().Format(time.RFC3339),
	}
	if info.Asset == "" {
		info.Asset = full.Name
	}
	info.HasUpdate = compareVersions(latest, Version) > 0

	u.info = info
	u.hotfix = hotfix
	u.full = full
	u.checkedAt = time.Now()
	u.lastErr = nil
	if info.HasUpdate && hotfix.Name == "" && full.Name == "" {
		u.lastErr = fmt.Errorf("最新版本 v%s 没有对应的 BPDL 更新包", latest)
	}
	if u.lastErr != nil {
		return u.info, u.lastErr
	}
	return u.info, nil
}

func selectUpdateAssets(assets []ghAsset) (updateAsset, updateAsset) {
	var hotfix, full updateAsset
	for _, a := range assets {
		name := strings.ToLower(strings.TrimSpace(a.Name))
		if !strings.HasSuffix(name, ".zip") || a.BrowserDownloadURL == "" {
			continue
		}
		if !strings.Contains(name, "bpdl") && !strings.Contains(name, "beatportdl") {
			continue
		}
		asset := updateAsset{Name: a.Name, URL: a.BrowserDownloadURL, Size: a.Size}
		switch {
		case strings.Contains(name, "hotfix") || strings.Contains(name, "minimal") || strings.Contains(name, "-min-"):
			if hotfix.Name == "" {
				hotfix = asset
			}
		case strings.Contains(name, "full"):
			if full.Name == "" {
				full = asset
			}
		}
	}
	// A single generic BPDL zip is treated as a full package: replacing the
	// runtime files is always safer than trying to guess a partial layout.
	if hotfix.Name == "" && full.Name == "" {
		for _, a := range assets {
			name := strings.ToLower(a.Name)
			if strings.HasSuffix(name, ".zip") && (strings.Contains(name, "bpdl") || strings.Contains(name, "beatportdl")) {
				full = updateAsset{Name: a.Name, URL: a.BrowserDownloadURL, Size: a.Size}
				break
			}
		}
	}
	return hotfix, full
}

// Apply downloads, verifies, backs up and stages an update, then starts a
// detached copy of this executable that swaps the files after the app exits.
func (u *updateManager) Apply(full bool, progress func(UpdateProgress)) ApplyResult {
	u.mu.Lock()
	defer u.mu.Unlock()
	if u.applying {
		return ApplyResult{OK: false, Code: "UPDATE_BUSY", Message: "正在更新，请稍候"}
	}
	if !u.info.HasUpdate || u.info.Latest == "" {
		return ApplyResult{OK: false, Code: "NO_UPDATE", Message: "没有可用的新版本，请先检查更新"}
	}

	asset := u.hotfix
	canFull := false
	if full {
		asset = u.full
	} else {
		canFull = u.hotfix.Name != "" && u.full.Name != ""
	}
	if asset.Name == "" {
		asset = u.full
	}
	if asset.Name == "" {
		return ApplyResult{OK: false, Code: "NO_FULL", Message: "Release 中没有可用的更新包"}
	}
	if !writableDirectory(u.paths.ExeDir) {
		return ApplyResult{
			OK:      false,
			Code:    "READONLY",
			Message: "程序目录不可写，无法自动更新。请把程序放在普通文件夹（不要放在 Program Files 或只读 U 盘）。",
		}
	}
	u.client = newUpdateHTTPClient(u.paths)

	u.applying = true
	defer func() { u.applying = false }()
	report := func(stage string, percent int, received, total int64, message string) {
		if progress != nil {
			progress(UpdateProgress{Stage: stage, Percent: percent, Received: received, Total: total, Message: message})
		}
	}
	failCode := "HOTFIX_FAILED"
	if full {
		failCode = "FULL_UPDATE_FAILED"
	}
	fail := func(err error) ApplyResult {
		u.log.Printf("更新失败：%v", err)
		return ApplyResult{OK: false, Code: failCode, Message: err.Error(), CanFull: canFull}
	}

	report("preparing", 1, 0, 0, "正在准备更新…")
	work, err := os.MkdirTemp("", "bpdl-update-")
	if err != nil {
		return fail(fmt.Errorf("创建更新临时目录失败：%w", err))
	}
	keepWork := false
	defer func() {
		if !keepWork {
			_ = os.RemoveAll(work)
		}
	}()

	zipPath := filepath.Join(work, "update.zip")
	report("downloading", 3, 0, asset.Size, "正在下载更新包…")
	if err := u.downloadAsset(asset, zipPath, func(received, total int64) {
		percent := 5
		if total > 0 {
			percent = 5 + int(received*70/total)
		}
		if percent > 75 {
			percent = 75
		}
		report("downloading", percent, received, total, "正在下载更新包…")
	}); err != nil {
		return fail(err)
	}

	report("verifying", 78, 0, 0, "正在校验更新包…")
	extractDir := filepath.Join(work, "extract")
	if err := extractUpdateZip(zipPath, extractDir); err != nil {
		return fail(err)
	}
	root, err := findUpdateRoot(extractDir)
	if err != nil {
		return fail(err)
	}
	stage := filepath.Join(work, "stage")
	manifest, files, err := stageUpdateFiles(root, stage, u.info.Latest)
	if err != nil {
		return fail(err)
	}
	u.log.Printf("更新包校验通过：版本=%s 文件=%d 清单=%v", u.info.Latest, len(files), manifest.Files)

	report("backing-up", 85, 0, 0, "正在备份当前版本…")
	backupDir, err := u.backupCurrent(files, u.info.Latest)
	if err != nil {
		return fail(fmt.Errorf("备份当前版本失败，已取消更新：%w", err))
	}

	report("preparing", 92, 0, 0, "正在准备更新程序…")
	self, err := os.Executable()
	if err != nil {
		return fail(fmt.Errorf("定位当前程序失败：%w", err))
	}
	updaterExe := filepath.Join(work, "bpdl-updater.exe")
	if err := copyFile(self, updaterExe); err != nil {
		return fail(fmt.Errorf("准备更新程序失败：%w", err))
	}
	if st, err := os.Stat(updaterExe); err != nil || st.Size() < 1<<20 {
		return fail(errors.New("更新程序副本不完整"))
	}

	args := []string{
		"--apply-update",
		"--pid", strconv.Itoa(os.Getpid()),
		"--app-dir", u.paths.ExeDir,
		"--data-dir", u.paths.DataDir,
		"--stage", stage,
		"--backup", backupDir,
		"--work", work,
		"--version", u.info.Latest,
	}
	cmd := exec.Command(updaterExe, args...)
	cmd.Dir = work
	cmd.SysProcAttr = &syscall.SysProcAttr{HideWindow: true}
	if err := cmd.Start(); err != nil {
		return fail(fmt.Errorf("启动更新程序失败：%w", err))
	}
	_ = cmd.Process.Release()
	keepWork = true
	report("restarting", 100, 0, 0, "更新已就绪，正在重启程序…")
	u.log.Printf("更新已交给后台程序：版本=%s 更新包=%s", u.info.Latest, asset.Name)
	return ApplyResult{OK: true, Version: u.info.Latest, Restarting: true}
}

func (u *updateManager) downloadAsset(asset updateAsset, dest string, progress func(received, total int64)) error {
	parsed, err := url.Parse(asset.URL)
	if err != nil || parsed.Scheme != "https" {
		return errors.New("更新包地址不是 HTTPS")
	}
	host := strings.ToLower(parsed.Hostname())
	if host != "github.com" && host != "api.github.com" && !strings.HasSuffix(host, ".githubusercontent.com") {
		return fmt.Errorf("更新包来自不受信任的主机：%s", host)
	}
	ctx, cancel := context.WithTimeout(context.Background(), 30*time.Minute)
	defer cancel()
	req, err := http.NewRequestWithContext(ctx, http.MethodGet, asset.URL, nil)
	if err != nil {
		return err
	}
	req.Header.Set("User-Agent", updateUserAgent)
	resp, err := u.client.Do(req)
	if err != nil {
		return fmt.Errorf("下载更新包失败：%w", err)
	}
	defer resp.Body.Close()
	if resp.StatusCode != http.StatusOK {
		return fmt.Errorf("下载更新包失败：HTTP %d", resp.StatusCode)
	}
	total := resp.ContentLength
	if total <= 0 {
		total = asset.Size
	}
	if total > maxUpdateDownload {
		return fmt.Errorf("更新包过大：%d 字节", total)
	}
	file, err := os.OpenFile(dest, os.O_CREATE|os.O_WRONLY|os.O_TRUNC, 0o600)
	if err != nil {
		return fmt.Errorf("创建更新包文件失败：%w", err)
	}
	defer file.Close()

	buf := make([]byte, 128<<10)
	var received int64
	lastReport := time.Time{}
	for {
		n, readErr := resp.Body.Read(buf)
		if n > 0 {
			received += int64(n)
			if received > maxUpdateDownload {
				return errors.New("更新包超过大小上限")
			}
			if _, err := file.Write(buf[:n]); err != nil {
				return fmt.Errorf("写入更新包失败：%w", err)
			}
			if time.Since(lastReport) > 150*time.Millisecond {
				lastReport = time.Now()
				progress(received, total)
			}
		}
		if readErr == io.EOF {
			break
		}
		if readErr != nil {
			return fmt.Errorf("读取更新包失败：%w", readErr)
		}
	}
	progress(received, total)
	if total > 0 && received != total {
		return fmt.Errorf("更新包下载不完整（%d/%d 字节）", received, total)
	}
	return nil
}

func extractUpdateZip(zipPath, dest string) error {
	reader, err := zip.OpenReader(zipPath)
	if err != nil {
		return fmt.Errorf("更新包不是有效的 zip：%w", err)
	}
	defer reader.Close()
	if err := os.MkdirAll(dest, 0o755); err != nil {
		return err
	}
	var total uint64
	for _, file := range reader.File {
		name := filepath.FromSlash(file.Name)
		clean := filepath.Clean(name)
		if clean == "." || clean == "" {
			continue
		}
		if filepath.IsAbs(clean) || strings.HasPrefix(clean, "..") || strings.Contains(clean, ".."+string(os.PathSeparator)) {
			return fmt.Errorf("更新包包含非法路径：%s", file.Name)
		}
		target := filepath.Join(dest, clean)
		if !strings.HasPrefix(target, filepath.Clean(dest)+string(os.PathSeparator)) {
			return fmt.Errorf("更新包路径越界：%s", file.Name)
		}
		if file.FileInfo().IsDir() {
			if err := os.MkdirAll(target, 0o755); err != nil {
				return err
			}
			continue
		}
		if !file.Mode().IsRegular() {
			continue
		}
		total += file.UncompressedSize64
		if total > maxUpdateExtract {
			return errors.New("更新包解压后过大")
		}
		if err := os.MkdirAll(filepath.Dir(target), 0o755); err != nil {
			return err
		}
		src, err := file.Open()
		if err != nil {
			return err
		}
		dst, err := os.OpenFile(target, os.O_CREATE|os.O_WRONLY|os.O_TRUNC, 0o644)
		if err != nil {
			src.Close()
			return err
		}
		_, copyErr := io.Copy(dst, io.LimitReader(src, int64(maxUpdateExtract)))
		src.Close()
		closeErr := dst.Close()
		if copyErr != nil {
			return copyErr
		}
		if closeErr != nil {
			return closeErr
		}
	}
	return nil
}

// findUpdateRoot accepts a zip with files at its root or one wrapper folder.
func findUpdateRoot(dir string) (string, error) {
	type candidate struct {
		path  string
		depth int
	}
	var queue []candidate
	queue = append(queue, candidate{dir, 0})
	for len(queue) > 0 {
		cur := queue[0]
		queue = queue[1:]
		if st, err := os.Stat(filepath.Join(cur.path, "beatportdl_gui.exe")); err == nil && !st.IsDir() {
			return cur.path, nil
		}
		if cur.depth >= 3 {
			continue
		}
		entries, err := os.ReadDir(cur.path)
		if err != nil {
			continue
		}
		for _, entry := range entries {
			if entry.IsDir() {
				queue = append(queue, candidate{filepath.Join(cur.path, entry.Name()), cur.depth + 1})
			}
		}
	}
	return "", errors.New("更新包中找不到 beatportdl_gui.exe")
}

// stageUpdateFiles copies only runtime files to an isolated staging directory.
// User data, credentials and the YAML config are never part of an update.
func stageUpdateFiles(root, stage, latest string) (updateManifest, []string, error) {
	if err := os.MkdirAll(stage, 0o755); err != nil {
		return updateManifest{}, nil, err
	}
	entries, err := os.ReadDir(root)
	if err != nil {
		return updateManifest{}, nil, err
	}
	var files []string
	for _, entry := range entries {
		if entry.IsDir() || !isRuntimeFileName(entry.Name()) {
			continue
		}
		if err := copyFile(filepath.Join(root, entry.Name()), filepath.Join(stage, entry.Name())); err != nil {
			return updateManifest{}, nil, fmt.Errorf("复制更新文件 %s 失败：%w", entry.Name(), err)
		}
		files = append(files, entry.Name())
	}
	exePath := filepath.Join(stage, "beatportdl_gui.exe")
	st, err := os.Stat(exePath)
	if err != nil {
		return updateManifest{}, nil, errors.New("更新包缺少 beatportdl_gui.exe")
	}
	if st.Size() < 1<<20 {
		return updateManifest{}, nil, errors.New("更新包中的主程序过小，可能已损坏")
	}
	head := make([]byte, 2)
	f, err := os.Open(exePath)
	if err != nil {
		return updateManifest{}, nil, err
	}
	_, readErr := io.ReadFull(f, head)
	f.Close()
	if readErr != nil || string(head) != "MZ" {
		return updateManifest{}, nil, errors.New("更新包中的主程序不是有效的 Windows 可执行文件")
	}

	var manifest updateManifest
	if data, err := os.ReadFile(filepath.Join(root, "update.json")); err == nil {
		if err := json.Unmarshal(data, &manifest); err != nil {
			return updateManifest{}, nil, fmt.Errorf("update.json 格式错误：%w", err)
		}
		if manifest.App != "" && manifest.App != "beatportdl" {
			return updateManifest{}, nil, errors.New("更新包不是 BeatportDL")
		}
		if manifest.Version != "" && compareVersions(manifest.Version, latest) < 0 {
			return updateManifest{}, nil, fmt.Errorf("更新包版本 %s 低于 Release %s", manifest.Version, latest)
		}
	}
	return manifest, files, nil
}

func isRuntimeFileName(name string) bool {
	base := filepath.Base(name)
	lower := strings.ToLower(base)
	if strings.Contains(lower, "config") || strings.Contains(lower, "credential") || strings.Contains(lower, "password") {
		return false
	}
	switch lower {
	case "beatportdl_gui.exe", "beatportdl.exe", "webview2loader.dll", "启动.bat":
		return true
	}
	switch strings.ToLower(filepath.Ext(base)) {
	case ".exe", ".dll":
		return true
	}
	return false
}

func (u *updateManager) backupCurrent(files []string, latest string) (string, error) {
	stamp := time.Now().Format("20060102-150405")
	backupDir := filepath.Join(u.paths.DataDir, "backups", "update-"+sanitizeVersion(latest)+"-"+stamp)
	if err := os.MkdirAll(backupDir, 0o755); err != nil {
		return "", err
	}
	for _, name := range files {
		src := filepath.Join(u.paths.ExeDir, name)
		if st, err := os.Stat(src); err != nil || st.IsDir() {
			continue
		}
		if err := copyFile(src, filepath.Join(backupDir, name)); err != nil {
			return "", fmt.Errorf("备份 %s 失败：%w", name, err)
		}
	}
	meta, _ := json.MarshalIndent(map[string]any{
		"version": Version,
		"latest":  latest,
		"time":    time.Now().Format(time.RFC3339),
	}, "", "  ")
	_ = os.WriteFile(filepath.Join(backupDir, "backup.json"), meta, 0o644)
	pruneBackups(filepath.Join(u.paths.DataDir, "backups"), 8)
	return backupDir, nil
}

func pruneBackups(root string, keep int) {
	entries, err := os.ReadDir(root)
	if err != nil {
		return
	}
	var dirs []os.DirEntry
	for _, entry := range entries {
		if entry.IsDir() && strings.HasPrefix(entry.Name(), "update-") {
			dirs = append(dirs, entry)
		}
	}
	if len(dirs) <= keep {
		return
	}
	for i := 0; i < len(dirs)-keep; i++ {
		_ = os.RemoveAll(filepath.Join(root, dirs[i].Name()))
	}
}

func sanitizeVersion(v string) string {
	v = strings.TrimSpace(v)
	var b strings.Builder
	for _, r := range v {
		if (r >= '0' && r <= '9') || (r >= 'a' && r <= 'z') || (r >= 'A' && r <= 'Z') || r == '.' || r == '-' || r == '_' {
			b.WriteRune(r)
		}
	}
	if b.Len() == 0 {
		return "unknown"
	}
	return b.String()
}

func copyFile(src, dst string) error {
	in, err := os.Open(src)
	if err != nil {
		return err
	}
	defer in.Close()
	if err := os.MkdirAll(filepath.Dir(dst), 0o755); err != nil {
		return err
	}
	out, err := os.OpenFile(dst, os.O_CREATE|os.O_WRONLY|os.O_TRUNC, 0o644)
	if err != nil {
		return err
	}
	_, copyErr := io.Copy(out, in)
	closeErr := out.Close()
	if copyErr != nil {
		return copyErr
	}
	return closeErr
}

func writableDirectory(dir string) bool {
	probe := filepath.Join(dir, ".bpdl-update-write-test")
	f, err := os.OpenFile(probe, os.O_CREATE|os.O_WRONLY|os.O_TRUNC, 0o600)
	if err != nil {
		return false
	}
	_ = f.Close()
	_ = os.Remove(probe)
	return true
}

func truncateText(value string, limit int) string {
	if len(value) <= limit {
		return value
	}
	return value[:limit] + "\n…"
}

func compareVersions(a, b string) int {
	pa := versionParts(a)
	pb := versionParts(b)
	length := len(pa)
	if len(pb) > length {
		length = len(pb)
	}
	for i := 0; i < length; i++ {
		var av, bv int
		if i < len(pa) {
			av = pa[i]
		}
		if i < len(pb) {
			bv = pb[i]
		}
		if av != bv {
			return av - bv
		}
	}
	return 0
}

func versionParts(value string) []int {
	text := strings.ToLower(strings.TrimSpace(value))
	text = strings.TrimPrefix(text, "v")
	if idx := strings.IndexAny(text, "-+"); idx >= 0 {
		text = text[:idx]
	}
	parts := strings.Split(text, ".")
	result := make([]int, 0, len(parts))
	for _, part := range parts {
		n, _ := strconv.Atoi(strings.TrimSpace(part))
		if n < 0 {
			n = 0
		}
		result = append(result, n)
	}
	if len(result) == 0 {
		result = []int{0}
	}
	return result
}

func cleanupOldUpdateDirs() {
	root := os.TempDir()
	entries, err := os.ReadDir(root)
	if err != nil {
		return
	}
	cutoff := time.Now().Add(-48 * time.Hour)
	for _, entry := range entries {
		if !entry.IsDir() || !strings.HasPrefix(entry.Name(), "bpdl-update-") {
			continue
		}
		info, err := entry.Info()
		if err != nil || info.ModTime().After(cutoff) {
			continue
		}
		_ = os.RemoveAll(filepath.Join(root, entry.Name()))
	}
}

// ---- detached updater helper ------------------------------------------------

// UpdaterArgs is the command line used by the copied updater executable.
type UpdaterArgs struct {
	PID     int
	AppDir  string
	DataDir string
	Stage   string
	Backup  string
	Work    string
	Version string
}

// IsUpdaterInvocation is checked before Win32/WebView2 initialization so the
// copied helper can run without single-instance or COM side effects.
func IsUpdaterInvocation(args []string) bool {
	for _, arg := range args {
		if arg == "--apply-update" || arg == "-apply-update" {
			return true
		}
	}
	return false
}

// RunUpdaterFromArgs runs the helper and returns an exit code for main().
func RunUpdaterFromArgs(args []string) int {
	fs := flag.NewFlagSet("bpdl-updater", flag.ContinueOnError)
	fs.SetOutput(io.Discard)
	apply := fs.Bool("apply-update", false, "")
	pid := fs.Int("pid", 0, "")
	appDir := fs.String("app-dir", "", "")
	dataDir := fs.String("data-dir", "", "")
	stage := fs.String("stage", "", "")
	backup := fs.String("backup", "", "")
	work := fs.String("work", "", "")
	version := fs.String("version", "", "")
	if err := fs.Parse(args); err != nil || !*apply {
		winui.Message("Beatport 下载器更新", "更新参数不正确，更新已取消。", winui.MBOK|winui.MBIconError)
		return 2
	}
	ua := UpdaterArgs{
		PID: *pid, AppDir: *appDir, DataDir: *dataDir,
		Stage: *stage, Backup: *backup, Work: *work, Version: *version,
	}
	return runUpdater(ua)
}

func runUpdater(ua UpdaterArgs) int {
	appDir, err := filepath.Abs(ua.AppDir)
	if err != nil || appDir == "" {
		winui.Message("Beatport 下载器更新", "更新失败：程序目录无效。", winui.MBOK|winui.MBIconError)
		return 1
	}
	logger := openUpdateLogger(appDir)
	logger.Printf("更新程序启动：pid=%d app=%s stage=%s version=%s", ua.PID, appDir, ua.Stage, ua.Version)

	if err := waitForProcessExit(ua.PID, 90*time.Second); err != nil {
		logger.Printf("等待主程序退出失败：%v", err)
		winui.Message("Beatport 下载器更新", "更新失败：主程序没有正常退出，请关闭程序后重试。\n\n"+err.Error(), winui.MBOK|winui.MBIconError)
		return 1
	}
	time.Sleep(1500 * time.Millisecond)

	stage, _ := filepath.Abs(ua.Stage)
	if err := verifyStage(stage); err != nil {
		logger.Printf("更新包校验失败：%v", err)
		winui.Message("Beatport 下载器更新", "更新失败：更新包校验未通过，原版本保持不变。\n\n"+err.Error(), winui.MBOK|winui.MBIconError)
		launchApp(appDir, logger)
		return 1
	}
	writeUpdateState(ua.DataDir, "applying", ua.Version, "")
	if err := copyStageToApp(stage, appDir); err != nil {
		logger.Printf("替换文件失败：%v", err)
		_ = restoreBackup(ua.Backup, appDir)
		writeUpdateState(ua.DataDir, "failed", ua.Version, err.Error())
		winui.Message("Beatport 下载器更新", "更新失败，已经恢复旧版本。\n\n"+err.Error(), winui.MBOK|winui.MBIconError)
		launchApp(appDir, logger)
		return 1
	}

	exePath := filepath.Join(appDir, "beatportdl_gui.exe")
	if st, err := os.Stat(exePath); err != nil || st.Size() < 1<<20 {
		logger.Printf("新版本主程序异常：%v", err)
		_ = restoreBackup(ua.Backup, appDir)
		writeUpdateState(ua.DataDir, "failed", ua.Version, "新版本主程序异常")
		winui.Message("Beatport 下载器更新", "更新失败，已经恢复旧版本。", winui.MBOK|winui.MBIconError)
		launchApp(appDir, logger)
		return 1
	}

	writeUpdateState(ua.DataDir, "ok", ua.Version, "")
	logger.Printf("更新完成，启动新版本：%s", exePath)
	if err := launchApp(appDir, logger); err != nil {
		logger.Printf("启动新版本失败：%v", err)
		_ = restoreBackup(ua.Backup, appDir)
		writeUpdateState(ua.DataDir, "failed", ua.Version, err.Error())
		winui.Message("Beatport 下载器更新", "更新已完成，但启动新版本失败，已恢复旧版本。\n\n"+err.Error(), winui.MBOK|winui.MBIconError)
		launchApp(appDir, logger)
		return 1
	}
	// Remove everything except this running updater executable; the temp copy
	// itself is cleaned up by cleanupOldUpdateDirs on a later launch.
	_ = os.RemoveAll(filepath.Join(ua.Work, "extract"))
	_ = os.RemoveAll(filepath.Join(ua.Work, "stage"))
	_ = os.Remove(filepath.Join(ua.Work, "update.zip"))
	return 0
}

func openUpdateLogger(appDir string) *log.Logger {
	logDir := filepath.Join(appDir, "logs")
	_ = os.MkdirAll(logDir, 0o755)
	f, err := os.OpenFile(filepath.Join(logDir, "update.log"), os.O_CREATE|os.O_APPEND|os.O_WRONLY, 0o644)
	if err != nil {
		return log.New(io.Discard, "", 0)
	}
	return log.New(f, "", log.LstdFlags)
}

func waitForProcessExit(pid int, timeout time.Duration) error {
	if pid <= 0 {
		return nil
	}
	handle, err := windows.OpenProcess(windows.SYNCHRONIZE, false, uint32(pid))
	if err != nil {
		// The process is already gone or the handle expired; a short grace is
		// safer than failing the update outright.
		time.Sleep(1200 * time.Millisecond)
		return nil
	}
	defer windows.CloseHandle(handle)
	event, err := windows.WaitForSingleObject(handle, uint32(timeout/time.Millisecond))
	if err != nil {
		return err
	}
	// WAIT_TIMEOUT is 258; x/sys exposes it as a syscall.Errno constant.
	if event == 258 {
		return errors.New("等待主程序退出超时")
	}
	return nil
}

func verifyStage(stage string) error {
	exePath := filepath.Join(stage, "beatportdl_gui.exe")
	st, err := os.Stat(exePath)
	if err != nil {
		return errors.New("更新包缺少 beatportdl_gui.exe")
	}
	if st.Size() < 1<<20 {
		return errors.New("更新包主程序过小")
	}
	f, err := os.Open(exePath)
	if err != nil {
		return err
	}
	defer f.Close()
	head := make([]byte, 2)
	if _, err := io.ReadFull(f, head); err != nil || string(head) != "MZ" {
		return errors.New("更新包主程序不是有效的 Windows 可执行文件")
	}
	return nil
}

func copyStageToApp(stage, appDir string) error {
	entries, err := os.ReadDir(stage)
	if err != nil {
		return err
	}
	for _, entry := range entries {
		if entry.IsDir() || !isRuntimeFileName(entry.Name()) {
			continue
		}
		src := filepath.Join(stage, entry.Name())
		dst := filepath.Join(appDir, entry.Name())
		var lastErr error
		for attempt := 0; attempt < 40; attempt++ {
			if err := copyFile(src, dst); err == nil {
				lastErr = nil
				break
			} else {
				lastErr = err
			}
			time.Sleep(250 * time.Millisecond)
		}
		if lastErr != nil {
			return fmt.Errorf("写入 %s 失败：%w", entry.Name(), lastErr)
		}
	}
	return nil
}

func restoreBackup(backupDir, appDir string) error {
	entries, err := os.ReadDir(backupDir)
	if err != nil {
		return err
	}
	for _, entry := range entries {
		if entry.IsDir() || entry.Name() == "backup.json" {
			continue
		}
		if err := copyFile(filepath.Join(backupDir, entry.Name()), filepath.Join(appDir, entry.Name())); err != nil {
			return err
		}
	}
	return nil
}

func launchApp(appDir string, logger *log.Logger) error {
	exePath := filepath.Join(appDir, "beatportdl_gui.exe")
	cmd := exec.Command(exePath)
	cmd.Dir = appDir
	cmd.SysProcAttr = &syscall.SysProcAttr{HideWindow: true}
	if err := cmd.Start(); err != nil {
		return err
	}
	_ = cmd.Process.Release()
	if logger != nil {
		logger.Printf("已启动：%s", exePath)
	}
	return nil
}

func writeUpdateState(dataDir, status, version, message string) {
	if dataDir == "" {
		return
	}
	if err := os.MkdirAll(dataDir, 0o755); err != nil {
		return
	}
	payload, _ := json.MarshalIndent(map[string]string{
		"status":  status,
		"version": version,
		"message": message,
		"time":    time.Now().Format(time.RFC3339),
	}, "", "  ")
	_ = os.WriteFile(filepath.Join(dataDir, "update-state.json"), payload, 0o644)
}
