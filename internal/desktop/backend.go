//go:build windows

package desktop

import (
	"bufio"
	"context"
	"fmt"
	"net/http"
	"os"
	"os/exec"
	"path/filepath"
	"strings"
	"sync"
	"syscall"
	"time"

	"golang.org/x/sys/windows"

	"unspok3n/beatportdl/internal/winui"
)

const (
	createNoWindow = 0x08000000
)

// backend supervises the existing Go downloader process: it probes /health,
// starts `beatportdl.exe --serve` hidden when needed, funnels its output into
// logs/backend.log and ties the child's lifetime to this UI process via a
// kill-on-close job object.
type backend struct {
	addr     string
	exePath  string
	exeDir   string
	config   string
	logPath  string
	extraArg []string

	mu      sync.Mutex
	cmd     *exec.Cmd
	waitCh  chan error
	job     windows.Handle
	logFile *os.File
	// proxy mirrors the config file value and is exported to the child process
	// as HTTP(S)_PROXY so downloads (which use http.Get) also honor it.
	proxy string

	ownsProcess bool
}

func newBackend(paths Paths, addr string) *backend {
	b := &backend{
		addr:    addr,
		exePath: paths.BackendBinary,
		exeDir:  paths.ExeDir,
		config:  paths.ConfigFile,
		logPath: paths.BackendLog,
	}
	b.proxy = readConfigString(b.config, "proxy")
	return b
}

// ConfigPath returns the config file used by the backend (the portable one, or
// the fallback under data/ when the executable directory is read-only).
func (b *backend) ConfigPath() string {
	b.mu.Lock()
	defer b.mu.Unlock()
	return b.configPathLocked()
}

func (b *backend) configPathLocked() string {
	for i := 0; i+1 < len(b.extraArg); i++ {
		if b.extraArg[i] == "--config" {
			return b.extraArg[i+1]
		}
	}
	return b.config
}

// Proxy returns the currently applied proxy URL ("" means direct).
func (b *backend) Proxy() string {
	b.mu.Lock()
	defer b.mu.Unlock()
	return b.proxy
}

// SetProxy updates the proxy used for the next backend process start.
func (b *backend) SetProxy(proxy string) {
	b.mu.Lock()
	b.proxy = proxy
	b.mu.Unlock()
}

// OwnsProcess reports whether this UI started the backend process.
func (b *backend) OwnsProcess() bool {
	b.mu.Lock()
	defer b.mu.Unlock()
	return b.ownsProcess
}

// commandEnvLocked builds the environment for the backend child. The caller
// must hold b.mu. Proxy settings are injected here (in addition to the YAML
// `proxy` value used by the Beatport API client) so plain http.Get download
// calls also go through the proxy.
func (b *backend) commandEnvLocked() []string {
	proxy := strings.TrimSpace(b.proxy)
	if proxy == "" {
		return os.Environ()
	}
	drop := map[string]bool{
		"http_proxy": true, "https_proxy": true, "all_proxy": true, "no_proxy": true,
	}
	env := make([]string, 0, len(os.Environ())+4)
	for _, entry := range os.Environ() {
		name := entry
		if idx := strings.IndexByte(entry, '='); idx >= 0 {
			name = entry[:idx]
		}
		if drop[strings.ToLower(name)] {
			continue
		}
		env = append(env, entry)
	}
	env = append(env,
		"HTTP_PROXY="+proxy,
		"HTTPS_PROXY="+proxy,
		"ALL_PROXY="+proxy,
		"NO_PROXY=127.0.0.1,localhost,::1",
	)
	return env
}

// Healthy reports whether a backend answers /health quickly.
func (b *backend) Healthy(timeout time.Duration) bool {
	if timeout <= 0 {
		timeout = 2 * time.Second
	}
	client := &http.Client{Timeout: timeout}
	ctx, cancel := context.WithTimeout(context.Background(), timeout)
	defer cancel()
	req, err := http.NewRequestWithContext(ctx, http.MethodGet, "http://"+b.addr+"/health", nil)
	if err != nil {
		return false
	}
	resp, err := client.Do(req)
	if err != nil {
		return false
	}
	defer resp.Body.Close()
	return resp.StatusCode == http.StatusOK
}

// ensureConfig makes sure the backend has a config to read. A missing file
// would make the backend fall into its interactive first-run prompt, which
// cannot work when its console is hidden.
func (b *backend) ensureConfig(dataDir string) error {
	if b.config != "" {
		if _, err := os.Stat(b.config); err == nil {
			b.extraArg = nil
			return nil
		}
	}
	// The portable layout keeps the config next to the executable. If that
	// directory is read-only, fall back to the data directory and point the
	// backend at it with --config (credentials cache follows the config).
	target := b.config
	if err := os.MkdirAll(filepath.Dir(target), 0o755); err != nil {
		target = filepath.Join(dataDir, "beatportdl-config.yml")
	}
	if target == "" {
		return fmt.Errorf("无法确定配置文件位置")
	}
	if _, err := os.Stat(target); err == nil {
		if target != b.config {
			b.extraArg = []string{"--config", target}
		}
		return nil
	}

	downloads := defaultDownloadsDir()
	content := fmt.Sprintf(`# BeatportDL 配置文件（由桌面版自动生成）
# 首次运行已为你填好下载目录，可在界面「设置」中随时更改。
downloads_directory: %s
quality: lossless
track_exists: update
show_progress: false
`, downloads)
	if err := os.WriteFile(target, []byte(content), 0o600); err != nil {
		if target == b.config {
			alt := filepath.Join(dataDir, "beatportdl-config.yml")
			if werr := os.WriteFile(alt, []byte(content), 0o600); werr == nil {
				b.extraArg = []string{"--config", alt}
				return nil
			}
		}
		return fmt.Errorf("写入配置文件失败: %w", err)
	}
	if target != b.config {
		b.extraArg = []string{"--config", target}
	}
	return nil
}

func defaultDownloadsDir() string {
	home, err := os.UserHomeDir()
	if err != nil || home == "" {
		return "C:/BeatportDL"
	}
	path := filepath.Join(home, "Downloads", "BeatportDL")
	return filepath.ToSlash(path)
}

// EnsureRunning probes the backend and starts it when it is not already
// listening. It returns after the backend answers /health or after timeout.
func (b *backend) EnsureRunning(dataDir string, timeout time.Duration) error {
	if b.Healthy(1500 * time.Millisecond) {
		return nil
	}
	b.mu.Lock()
	defer b.mu.Unlock()
	if b.cmd != nil && b.waitCh != nil {
		// Already starting: wait for readiness below instead of spawning twice.
	} else {
		if b.cmd != nil && b.waitCh == nil {
			// The previous attempt exited on its own; release its handles before
			// spawning a replacement (used by restart flows).
			if b.job != 0 {
				windows.CloseHandle(b.job)
				b.job = 0
			}
			if b.logFile != nil {
				b.logFile.Close()
				b.logFile = nil
			}
			b.cmd = nil
			b.ownsProcess = false
		}
		if b.exePath == "" {
			return fmt.Errorf("未找到 beatportdl.exe（应与本程序放在同一目录）")
		}
		if err := b.ensureConfig(dataDir); err != nil {
			return err
		}
		if err := b.startLocked(); err != nil {
			return err
		}
	}

	deadline := time.Now().Add(timeout)
	for time.Now().Before(deadline) {
		select {
		case err := <-b.waitCh:
			b.waitCh = nil
			return fmt.Errorf("后端进程已退出（%v）。日志：%s", err, b.logPath)
		default:
		}
		if b.Healthy(900 * time.Millisecond) {
			return nil
		}
		time.Sleep(250 * time.Millisecond)
	}
	return fmt.Errorf("后端在 %s 内未能就绪。日志：%s", timeout, b.logPath)
}

func (b *backend) startLocked() error {
	// The YAML file is the source of truth (the UI writes it before a restart).
	b.proxy = readConfigString(b.configPathLocked(), "proxy")

	if err := os.MkdirAll(filepath.Dir(b.logPath), 0o755); err != nil {
		return fmt.Errorf("创建日志目录失败: %w", err)
	}
	f, err := os.OpenFile(b.logPath, os.O_CREATE|os.O_WRONLY|os.O_APPEND, 0o600)
	if err != nil {
		return fmt.Errorf("打开后端日志失败: %w", err)
	}
	b.logFile = f
	fmt.Fprintf(f, "\n===== GUI 启动后端 %s =====\n", time.Now().Format("2006-01-02 15:04:05"))
	fmt.Fprintf(f, "exe: %s\naddr: %s\nproxy: %s\n", b.exePath, b.addr, maskProxy(b.proxy))

	args := []string{"--serve", "--addr", b.addr}
	args = append(args, b.extraArg...)
	cmd := exec.Command(b.exePath, args...)
	cmd.Dir = filepath.Dir(b.exePath)
	cmd.Stdout = f
	cmd.Stderr = f
	cmd.Env = b.commandEnvLocked()
	cmd.SysProcAttr = &syscall.SysProcAttr{HideWindow: true, CreationFlags: createNoWindow}

	if err := cmd.Start(); err != nil {
		f.Close()
		b.logFile = nil
		return fmt.Errorf("启动后端失败: %w", err)
	}

	wait := make(chan error, 1)
	go func() { wait <- cmd.Wait() }()

	job, jobErr := winui.KillOnCloseJob()
	if jobErr == nil && cmd.Process != nil {
		_ = winui.AssignProcessToJob(job, cmd.Process.Pid)
	}

	b.cmd = cmd
	b.waitCh = wait
	b.job = job
	b.ownsProcess = true
	return nil
}

// Stop terminates a backend this UI started. A backend the user launched
// manually is left untouched.
func (b *backend) Stop() {
	b.mu.Lock()
	defer b.mu.Unlock()
	if !b.ownsProcess || b.cmd == nil || b.cmd.Process == nil {
		return
	}
	_ = b.cmd.Process.Kill()
	if b.waitCh != nil {
		select {
		case <-b.waitCh:
		case <-time.After(4 * time.Second):
		}
	}
	if b.job != 0 {
		windows.CloseHandle(b.job)
		b.job = 0
	}
	if b.logFile != nil {
		b.logFile.Close()
		b.logFile = nil
	}
	b.cmd = nil
	b.waitCh = nil
	b.ownsProcess = false
}

// Restart stops the backend this UI started and launches it again with the
// current config/proxy. An externally started backend is left alone and an
// error is returned so the UI can tell the user to restart it manually.
func (b *backend) Restart(dataDir string, timeout time.Duration) error {
	if !b.OwnsProcess() && b.Healthy(1500*time.Millisecond) {
		return fmt.Errorf("后端不是由界面启动的，无法自动重启；请手动重启 beatportdl.exe")
	}
	b.Stop()
	return b.EnsureRunning(dataDir, timeout)
}

// TailLog returns up to the last n lines of the backend log, used for failure
// dialogs and the in-app diagnostics panel.
func (b *backend) TailLog(n int) string {
	if n <= 0 {
		n = 20
	}
	f, err := os.Open(b.logPath)
	if err != nil {
		return ""
	}
	defer f.Close()
	scanner := bufio.NewScanner(f)
	scanner.Buffer(make([]byte, 0, 64*1024), 1<<20)
	var lines []string
	for scanner.Scan() {
		lines = append(lines, scanner.Text())
		if len(lines) > n*4 {
			lines = lines[len(lines)-n*2:]
		}
	}
	if len(lines) > n {
		lines = lines[len(lines)-n:]
	}
	return strings.Join(lines, "\n")
}

// LogPath exposes the backend log location for the settings page.
func (b *backend) LogPath() string { return b.logPath }
