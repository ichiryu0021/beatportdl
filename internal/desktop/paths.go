//go:build windows

package desktop

import (
	"fmt"
	"os"
	"path/filepath"
)

// Paths collects every filesystem location the desktop shell uses. The layout
// is portable: everything lives next to the executable unless that directory is
// read-only (e.g. a locked USB drive), in which case user-writable fallbacks
// under %LOCALAPPDATA% are used.
type Paths struct {
	ExeDir        string
	DataDir       string
	UIDir         string
	LogsDir       string
	WebViewData   string
	StateFile     string
	IconPath      string
	BackendLog    string
	GUILog        string
	ConfigFile    string
	BackendBinary string
	portable      bool
}

func writable(dir string) bool {
	if err := os.MkdirAll(dir, 0o755); err != nil {
		return false
	}
	probe := filepath.Join(dir, ".bpdl-ui-write-test")
	f, err := os.OpenFile(probe, os.O_CREATE|os.O_WRONLY|os.O_TRUNC, 0o600)
	if err != nil {
		return false
	}
	_ = f.Close()
	_ = os.Remove(probe)
	return true
}

func localAppData() string {
	if dir := os.Getenv("LOCALAPPDATA"); dir != "" {
		return dir
	}
	home, _ := os.UserHomeDir()
	return filepath.Join(home, "AppData", "Local")
}

// ResolvePaths computes the portable layout and creates the directories.
func ResolvePaths() (Paths, error) {
	exe, err := os.Executable()
	if err != nil {
		return Paths{}, fmt.Errorf("locate executable: %w", err)
	}
	exeDir := filepath.Dir(exe)

	p := Paths{ExeDir: exeDir, portable: true}
	p.DataDir = filepath.Join(exeDir, "data")
	p.LogsDir = filepath.Join(exeDir, "logs")
	if !writable(p.DataDir) || !writable(p.LogsDir) {
		p.portable = false
		base := filepath.Join(localAppData(), "BeatportDL")
		p.DataDir = filepath.Join(base, "data")
		p.LogsDir = filepath.Join(base, "logs")
		if err := os.MkdirAll(p.DataDir, 0o755); err != nil {
			return Paths{}, fmt.Errorf("create data directory: %w", err)
		}
		if err := os.MkdirAll(p.LogsDir, 0o755); err != nil {
			return Paths{}, fmt.Errorf("create logs directory: %w", err)
		}
	}

	p.UIDir = filepath.Join(p.DataDir, "ui")
	p.WebViewData = filepath.Join(p.DataDir, "webview2")
	p.StateFile = filepath.Join(p.DataDir, "window.json")
	p.IconPath = filepath.Join(p.DataDir, "app.ico")
	p.BackendLog = filepath.Join(p.LogsDir, "backend.log")
	p.GUILog = filepath.Join(p.LogsDir, "gui.log")
	p.ConfigFile = filepath.Join(exeDir, "beatportdl-config.yml")
	p.BackendBinary = findBackend(exeDir)
	return p, nil
}

// findBackend looks for the Go backend next to the UI binary first (deployment
// layout), then in the repository's bin directory (development layout).
func findBackend(exeDir string) string {
	candidates := []string{
		filepath.Join(exeDir, "beatportdl.exe"),
		filepath.Join(exeDir, "..", "bin", "beatportdl.exe"),
		filepath.Join(exeDir, "beatportdl"),
	}
	for _, candidate := range candidates {
		abs, err := filepath.Abs(candidate)
		if err != nil {
			continue
		}
		if st, err := os.Stat(abs); err == nil && !st.IsDir() {
			return abs
		}
	}
	return ""
}

// Portable reports whether data lives next to the executable.
func (p Paths) Portable() bool { return p.portable }
