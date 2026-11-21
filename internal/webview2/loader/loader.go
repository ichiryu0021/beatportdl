// Package loader loads the Microsoft Edge WebView2 Loader DLL and exposes the
// two exports this application needs. The loader itself is a tiny native
// shim that finds the installed Evergreen WebView2 runtime and creates the
// CoreWebView2 environment; it is shipped next to the executable so the app
// works on any Windows 10/11 machine that has Microsoft Edge installed.
package loader

import (
	"errors"
	"fmt"
	"os"
	"path/filepath"
	"sync"
	"syscall"
	"unsafe"

	"golang.org/x/sys/windows"
)

// ErrRuntimeMissing is returned when WebView2Loader.dll works but no Edge
// WebView2 runtime is installed on the machine.
var ErrRuntimeMissing = errors.New("Microsoft Edge WebView2 runtime is not installed")

var (
	once        sync.Once
	initErr     error
	dll         windows.Handle
	procCreate  uintptr
	procVersion uintptr
	procCompare uintptr
)

// exeDir returns the directory containing the running executable.
func exeDir() string {
	if p, err := os.Executable(); err == nil {
		return filepath.Dir(p)
	}
	return "."
}

// candidatePaths lists the places WebView2Loader.dll may live. Deployment is a
// portable folder, so the executable directory is checked first; a system-wide
// copy on PATH is the last resort.
func candidatePaths() []string {
	dir := exeDir()
	return []string{
		filepath.Join(dir, "WebView2Loader.dll"),
		filepath.Join(dir, "webview2", "WebView2Loader.dll"),
		filepath.Join(dir, "WebView2", "WebView2Loader.dll"),
		"WebView2Loader.dll",
	}
}

// Init locates and loads WebView2Loader.dll. It is safe to call multiple
// times; the DLL is loaded once.
func Init() error {
	once.Do(func() {
		var lastErr error
		for _, candidate := range candidatePaths() {
			h, err := windows.LoadLibrary(candidate)
			if err != nil {
				lastErr = err
				continue
			}
			create, err := windows.GetProcAddress(h, "CreateCoreWebView2EnvironmentWithOptions")
			if err != nil {
				_ = windows.FreeLibrary(h)
				lastErr = fmt.Errorf("%s: missing CreateCoreWebView2EnvironmentWithOptions: %w", candidate, err)
				continue
			}
			version, err := windows.GetProcAddress(h, "GetAvailableCoreWebView2BrowserVersionString")
			if err != nil {
				_ = windows.FreeLibrary(h)
				lastErr = fmt.Errorf("%s: missing GetAvailableCoreWebView2BrowserVersionString: %w", candidate, err)
				continue
			}
			compare, _ := windows.GetProcAddress(h, "CompareBrowserVersions")
			dll = h
			procCreate = create
			procVersion = version
			procCompare = compare
			return
		}
		if lastErr == nil {
			lastErr = errors.New("WebView2Loader.dll was not found")
		}
		initErr = fmt.Errorf("load WebView2Loader.dll: %w", lastErr)
	})
	return initErr
}

// RuntimeVersion reports the installed WebView2 Evergreen runtime version.
// It returns ErrRuntimeMissing when Edge WebView2 is not installed.
func RuntimeVersion() (string, error) {
	if err := Init(); err != nil {
		return "", err
	}
	var out *uint16
	hr, _, _ := syscall.SyscallN(procVersion, 0, uintptr(unsafe.Pointer(&out)))
	if int32(hr) < 0 {
		if uint32(hr)&0xFFFF == uint32(windows.ERROR_FILE_NOT_FOUND) {
			return "", ErrRuntimeMissing
		}
		return "", fmt.Errorf("GetAvailableCoreWebView2BrowserVersionString failed: HRESULT 0x%08X", uint32(hr))
	}
	defer windows.CoTaskMemFree(unsafe.Pointer(out))
	if out == nil {
		return "", ErrRuntimeMissing
	}
	return windows.UTF16PtrToString(out), nil
}

// CreateCoreWebView2EnvironmentWithOptions calls the loader export of the same
// name. See the WebView2 SDK documentation for argument semantics.
func CreateCoreWebView2EnvironmentWithOptions(browserExecutableFolder, userDataFolder *uint16, environmentOptions uintptr, environmentCompletedHandle uintptr) (uintptr, error) {
	if err := Init(); err != nil {
		return 0, err
	}
	hr, _, _ := syscall.SyscallN(
		procCreate,
		uintptr(unsafe.Pointer(browserExecutableFolder)),
		uintptr(unsafe.Pointer(userDataFolder)),
		environmentOptions,
		environmentCompletedHandle,
	)
	return hr, nil
}

// CompareBrowserVersions compares two WebView2 runtime version strings.
func CompareBrowserVersions(v1, v2 string) (int, error) {
	if err := Init(); err != nil {
		return 0, err
	}
	if procCompare == 0 {
		return 0, errors.New("CompareBrowserVersions is not exported by WebView2Loader.dll")
	}
	p1, err := windows.UTF16PtrFromString(v1)
	if err != nil {
		return 0, err
	}
	p2, err := windows.UTF16PtrFromString(v2)
	if err != nil {
		return 0, err
	}
	var result int32
	hr, _, _ := syscall.SyscallN(
		procCompare,
		uintptr(unsafe.Pointer(p1)),
		uintptr(unsafe.Pointer(p2)),
		uintptr(unsafe.Pointer(&result)),
	)
	if int32(hr) < 0 {
		return 0, fmt.Errorf("CompareBrowserVersions failed: HRESULT 0x%08X", uint32(hr))
	}
	return int(result), nil
}
