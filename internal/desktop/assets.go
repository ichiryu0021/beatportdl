//go:build windows

package desktop

import (
	"crypto/sha256"
	"embed"
	"encoding/hex"
	"fmt"
	"io/fs"
	"os"
	"path/filepath"
	"sort"
)

// webAssets is the compiled single-page UI. It is extracted into <data>/ui on
// first launch (and whenever it changes), then served to WebView2 through an
// in-process virtual host mapping: the UI gets a stable https:// origin without
// a local HTTP server or an open port.
//
//go:embed web
var webAssets embed.FS

// Version is the desktop shell version, shown in settings and the about box.
// Version is the application version reported to the UI and compared against
// GitHub Release tags. It is a var (not a const) so build-desktop.ps1 can inject
// the release version with -ldflags -X.
var Version = "1.1.6"

func assetHash() (string, error) {
	h := sha256.New()
	var names []string
	err := fs.WalkDir(webAssets, "web", func(path string, d fs.DirEntry, err error) error {
		if err != nil {
			return err
		}
		if d.IsDir() {
			return nil
		}
		names = append(names, path)
		return nil
	})
	if err != nil {
		return "", err
	}
	sort.Strings(names)
	for _, name := range names {
		data, err := webAssets.ReadFile(name)
		if err != nil {
			return "", err
		}
		h.Write([]byte(name))
		h.Write([]byte{0})
		h.Write(data)
	}
	return hex.EncodeToString(h.Sum(nil))[:16], nil
}

// ExtractAssets writes the embedded UI into uiDir when its content version
// differs from the stamp file. Returns the directory to map and the version.
func ExtractAssets(uiDir string) (string, string, error) {
	hash, err := assetHash()
	if err != nil {
		return "", "", fmt.Errorf("hash UI assets: %w", err)
	}
	if err := os.MkdirAll(uiDir, 0o755); err != nil {
		return "", "", fmt.Errorf("create UI directory: %w", err)
	}
	stampPath := filepath.Join(uiDir, ".version")
	if stamp, err := os.ReadFile(stampPath); err == nil && string(stamp) == hash {
		return uiDir, hash, nil
	}

	err = fs.WalkDir(webAssets, "web", func(path string, d fs.DirEntry, err error) error {
		if err != nil {
			return err
		}
		rel, err := filepath.Rel("web", filepath.FromSlash(path))
		if err != nil {
			return err
		}
		target := filepath.Join(uiDir, rel)
		if d.IsDir() {
			return os.MkdirAll(target, 0o755)
		}
		data, err := webAssets.ReadFile(path)
		if err != nil {
			return err
		}
		return os.WriteFile(target, data, 0o644)
	})
	if err != nil {
		return "", "", fmt.Errorf("extract UI: %w", err)
	}
	if err := os.WriteFile(stampPath, []byte(hash), 0o644); err != nil {
		return "", "", fmt.Errorf("write UI version stamp: %w", err)
	}
	return uiDir, hash, nil
}

// IconBytes returns the embedded application icon so the host can extract it to
// a stable path for LoadImageW (Win32 expects a file path, not a byte slice).
func IconBytes() ([]byte, error) {
	return webAssets.ReadFile("web/app.ico")
}

