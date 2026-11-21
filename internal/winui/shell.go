//go:build windows

package winui

import (
	"fmt"
	"os/exec"
	"strings"
	"syscall"
)

// RevealInExplorer opens Explorer with the given file or folder selected.
func RevealInExplorer(path string) error {
	path = strings.TrimSpace(path)
	if path == "" {
		return fmt.Errorf("路径为空")
	}
	cmd := exec.Command("explorer.exe", "/select,"+path)
	cmd.SysProcAttr = &syscall.SysProcAttr{HideWindow: true}
	if err := cmd.Start(); err != nil {
		return fmt.Errorf("无法打开资源管理器: %w", err)
	}
	go func() { _ = cmd.Wait() }()
	return nil
}
