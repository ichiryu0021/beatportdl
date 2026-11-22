//go:build windows

package winui

import (
	"fmt"
	"path/filepath"
	"strings"
	"syscall"
	"time"
	"unsafe"
)

var (
	procEnumWindows           = user32.NewProc("EnumWindows")
	procGetWindowTextW        = user32.NewProc("GetWindowTextW")
	procGetClassNameW         = user32.NewProc("GetClassNameW")
	procSHParseDisplayName    = shell32.NewProc("SHParseDisplayName")
	procSHOpenFolderAndSelect = shell32.NewProc("SHOpenFolderAndSelectItems")
)

// RevealInExplorer opens Explorer with the given file or folder selected.
func RevealInExplorer(path string) error {
	path = strings.TrimSpace(path)
	if path == "" {
		return fmt.Errorf("路径为空")
	}
	folder := filepath.Base(filepath.Dir(path))
	folderPath := filepath.Dir(path)
	if filepath.Ext(path) == "" {
		folder = filepath.Base(path)
		folderPath = path
	}
	_ = CoInitialize()
	if err := revealWithShell(path); err == nil {
		go focusExplorerEventually(folder)
		return nil
	}
	if err := ShellOpen(folderPath); err != nil {
		return fmt.Errorf("无法打开资源管理器: %w", err)
	}
	go focusExplorerEventually(folder)
	return nil
}

func revealWithShell(path string) error {
	name, err := syscall.UTF16PtrFromString(path)
	if err != nil {
		return err
	}
	var pidl uintptr
	hr, _, _ := procSHParseDisplayName.Call(
		uintptr(unsafe.Pointer(name)), 0,
		uintptr(unsafe.Pointer(&pidl)), 0, 0,
	)
	if int32(hr) < 0 || pidl == 0 {
		return fmt.Errorf("无法解析路径")
	}
	defer procCoTaskMemFree.Call(pidl)
	hr, _, _ = procSHOpenFolderAndSelect.Call(pidl, 0, 0, 0)
	if int32(hr) < 0 {
		return fmt.Errorf("无法在资源管理器中定位")
	}
	return nil
}

func focusExplorerEventually(folder string) {
	for i := 0; i < 15; i++ {
		time.Sleep(100 * time.Millisecond)
		if focusExplorerWindow(folder) {
			return
		}
	}
}

func focusExplorerWindow(folder string) bool {
	var found uintptr
	callback := syscall.NewCallback(func(hwnd uintptr, _ uintptr) uintptr {
		var class [64]uint16
		_, _, _ = procGetClassNameW.Call(hwnd, uintptr(unsafe.Pointer(&class[0])), uintptr(len(class)))
		if syscall.UTF16ToString(class[:]) != "CabinetWClass" {
			return 1
		}
		var title [512]uint16
		_, _, _ = procGetWindowTextW.Call(hwnd, uintptr(unsafe.Pointer(&title[0])), uintptr(len(title)))
		if folder != "" && !strings.Contains(strings.ToLower(syscall.UTF16ToString(title[:])), strings.ToLower(folder)) {
			return 1
		}
		found = hwnd
		return 0
	})
	_, _, _ = procEnumWindows.Call(callback, 0)
	if found == 0 {
		return false
	}
	_, _, _ = procShowWindow.Call(found, SWRestore)
	_, _, _ = procSetForegroundWindow.Call(found)
	return true
}
