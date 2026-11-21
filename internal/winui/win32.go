// Package winui implements the small set of native Windows pieces the
// BeatportDL desktop shell needs: a dark-themed main window, a message loop, a
// modern folder picker, single-instance handling and a kill-on-close job so the
// hidden Go backend never outlives the UI.
package winui

import (
	"syscall"
	"unsafe"

	"golang.org/x/sys/windows"
)

var (
	user32   = windows.NewLazySystemDLL("user32.dll")
	shell32  = windows.NewLazySystemDLL("shell32.dll")
	dwmapi   = windows.NewLazySystemDLL("dwmapi.dll")
	shcore   = windows.NewLazySystemDLL("shcore.dll")
	kernel32 = windows.NewLazySystemDLL("kernel32.dll")
	gdi32    = windows.NewLazySystemDLL("gdi32.dll")
	ole32    = windows.NewLazySystemDLL("ole32.dll")

	procGlobalMemoryStatusEx = kernel32.NewProc("GlobalMemoryStatusEx")

	procRegisterClassExW          = user32.NewProc("RegisterClassExW")
	procCreateWindowExW           = user32.NewProc("CreateWindowExW")
	procDefWindowProcW            = user32.NewProc("DefWindowProcW")
	procDestroyWindow             = user32.NewProc("DestroyWindow")
	procShowWindow                = user32.NewProc("ShowWindow")
	procUpdateWindow              = user32.NewProc("UpdateWindow")
	procSetWindowTextW            = user32.NewProc("SetWindowTextW")
	procGetMessageW               = user32.NewProc("GetMessageW")
	procTranslateMessage          = user32.NewProc("TranslateMessage")
	procDispatchMessageW          = user32.NewProc("DispatchMessageW")
	procPostQuitMessage           = user32.NewProc("PostQuitMessage")
	procPostThreadMessageW        = user32.NewProc("PostThreadMessageW")
	procPostMessageW              = user32.NewProc("PostMessageW")
	procSetForegroundWindow       = user32.NewProc("SetForegroundWindow")
	procFindWindowW               = user32.NewProc("FindWindowW")
	procMessageBoxW               = user32.NewProc("MessageBoxW")
	procSetWindowPos              = user32.NewProc("SetWindowPos")
	procGetWindowRect             = user32.NewProc("GetWindowRect")
	procGetClientRect             = user32.NewProc("GetClientRect")
	procGetWindowDC               = user32.NewProc("GetWindowDC")
	procReleaseDC                 = user32.NewProc("ReleaseDC")
	procFillRect                  = user32.NewProc("FillRect")
	procReleaseCapture            = user32.NewProc("ReleaseCapture")
	procSendMessageW              = user32.NewProc("SendMessageW")
	procMonitorFromWindow         = user32.NewProc("MonitorFromWindow")
	procGetMonitorInfoW           = user32.NewProc("GetMonitorInfoW")
	procSetFocus                  = user32.NewProc("SetFocus")
	procLoadImageW                = user32.NewProc("LoadImageW")
	procLoadIconW                 = user32.NewProc("LoadIconW")
	procLoadCursorW               = user32.NewProc("LoadCursorW")
	procSystemParametersInfoW     = user32.NewProc("SystemParametersInfoW")
	procGetDpiForWindow           = user32.NewProc("GetDpiForWindow")
	procGetDpiForSystem           = user32.NewProc("GetDpiForSystem")
	procCreateSolidBrush          = gdi32.NewProc("CreateSolidBrush")
	procSetProcessDpiAwarenessCtx = user32.NewProc("SetProcessDpiAwarenessContext")
	procSetProcessDPIAware        = user32.NewProc("SetProcessDPIAware")
	procAdjustWindowRectExForDpi  = user32.NewProc("AdjustWindowRectExForDpi")
	procAdjustWindowRectEx        = user32.NewProc("AdjustWindowRectEx")
	procGetWindowPlacement        = user32.NewProc("GetWindowPlacement")
	procIsZoomed                  = user32.NewProc("IsZoomed")
	procSetWindowLongPtrW         = user32.NewProc("SetWindowLongPtrW")
	procGetWindowLongPtrW         = user32.NewProc("GetWindowLongPtrW")
	procSetClassLongPtrW          = user32.NewProc("SetClassLongPtrW")
	procRedrawWindow              = user32.NewProc("RedrawWindow")
	procGetSystemMetrics          = user32.NewProc("GetSystemMetrics")
	procDwmSetWindowAttribute     = dwmapi.NewProc("DwmSetWindowAttribute")
	procDeleteObject              = gdi32.NewProc("DeleteObject")
	procShellExecuteW             = shell32.NewProc("ShellExecuteW")
	procSetProcessDpiAwareness    = shcore.NewProc("SetProcessDpiAwareness")
	procCoInitializeEx            = ole32.NewProc("CoInitializeEx")
	procCoUninitialize            = ole32.NewProc("CoUninitialize")
	procCoCreateInstance          = ole32.NewProc("CoCreateInstance")
	procCoTaskMemFree             = ole32.NewProc("CoTaskMemFree")
	procGetCurrentThreadID        = kernel32.NewProc("GetCurrentThreadId")
	procGetModuleHandleW          = kernel32.NewProc("GetModuleHandleW")
	procCreateMutexW              = kernel32.NewProc("CreateMutexW")
	procCreateJobObjectW          = kernel32.NewProc("CreateJobObjectW")
	procSetInformationJobObject   = kernel32.NewProc("SetInformationJobObject")
	procAssignProcessToJobObject  = kernel32.NewProc("AssignProcessToJobObject")
)

const (
	WSOverlappedWindow = 0x00CF0000
	WSVisible          = 0x10000000

	WSPopup       = 0x80000000
	WSThickFrame  = 0x00040000
	WSMinimizeBox = 0x00020000
	WSMaximizeBox = 0x00010000
	WSSysMenu     = 0x00080000
	WSExAppWindow = 0x00040000

	// AppWindowStyle is a frameless resizable top-level window. Windows keeps
	// the resize frame and taskbar integration, but draws no native caption.
	AppWindowStyle = WSPopup | WSThickFrame | WSMinimizeBox | WSMaximizeBox | WSSysMenu

	HTCAPTION       = 2
	WMNCLButtonDown = 0x00A1

	SWShowNormal = 1
	SWMaximize   = 3
	SWShow       = 5
	SWMinimize   = 6
	SWRestore    = 9

	WMCreate        = 0x0001
	WMDestroy       = 0x0002
	WMSize          = 0x0005
	WMActivate      = 0x0006
	WMSetFocus      = 0x0007
	WMPaint         = 0x000F
	WMClose         = 0x0010
	WMQuit          = 0x0012
	WMEraseBkgnd    = 0x0014
	WMNCCalcSize    = 0x0083
	WMNCHitTest     = 0x0084
	WMNCPaint       = 0x0085
	WMGetMinMaxInfo = 0x0024
	WMDpiChanged    = 0x02E0
	WMApp           = 0x8000

	HTClient      = 1
	HTLeft        = 10
	HTRight       = 11
	HTTop         = 12
	HTTopLeft     = 13
	HTTopRight    = 14
	HTBottom      = 15
	HTBottomLeft  = 16
	HTBottomRight = 17

	SWPNoSize       = 0x0001
	SWPNoZOrder     = 0x0004
	SWPNoActivate   = 0x0010
	SWPNoMove       = 0x0002
	SWPFrameChanged = 0x0020

	SPIGetWorkArea = 0x0030

	MonitorDefaultToNearest = 2

	LRLoadFromFile = 0x0010
	LRDefaultSize  = 0x0040
	ImageIcon      = 1
	IDIApplication = 32512
	IDCArrow       = 32512

	GWLStyle       = -16
	GCLPBackground = ^uintptr(9)

	RDWInvalidate  = 0x0001
	RDWAllChildren = 0x0080
	RDWFrame       = 0x0400

	MBOK              = 0x00000000
	MBIconError       = 0x00000010
	MBIconWarning     = 0x00000030
	MBIconInformation = 0x00000040
	MBSetForeground   = 0x00010000
	MBTopmost         = 0x00040000

	DWMWAUseImmersiveDarkMode = 20
	DWMWAWindowCorner         = 33
	DWMWACaptionColor         = 35
	DWMWABorderColor          = 34
	DWMWATextColor            = 36
	DWMWCPRound               = 2
	DWMWCPDoNotRound          = 1

	CLSCTXInprocServer      = 0x1
	COINITApartmentThreaded = 0x2

	JobObjectExtendedLimitInformation = 9
	JOBObjectLimitKillOnJobClose      = 0x2000

	// DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2 is (HANDLE)-4.
	dpiAwarenessContextPerMonitorAwareV2 = ^uintptr(3)
)

type point struct {
	X, Y int32
}

type rect struct {
	Left, Top, Right, Bottom int32
}

type minMaxInfo struct {
	PtReserved     point
	PtMaxSize      point
	PtMaxPosition  point
	PtMinTrackSize point
	PtMaxTrackSize point
}

type monitorInfo struct {
	CbSize    uint32
	RcMonitor rect
	RcWork    rect
	DwFlags   uint32
}

type wndClassExW struct {
	CbSize        uint32
	Style         uint32
	LpfnWndProc   uintptr
	CbClsExtra    int32
	CbWndExtra    int32
	HInstance     windows.Handle
	HIcon         windows.Handle
	HCursor       windows.Handle
	HbrBackground windows.Handle
	LpszMenuName  *uint16
	LpszClassName *uint16
	HIconSm       windows.Handle
}

type msg struct {
	Hwnd     windows.Handle
	Message  uint32
	WParam   uintptr
	LParam   uintptr
	Time     uint32
	Pt       point
	LPrivate uint32
}

type windowPlacement struct {
	Length           uint32
	Flags            uint32
	ShowCmd          uint32
	PtMinPosition    point
	PtMaxPosition    point
	RcNormalPosition rect
}

type jobObjectBasicLimitInformation struct {
	PerProcessUserTimeLimit int64
	PerJobUserTimeLimit     int64
	LimitFlags              uint32
	MinimumWorkingSetSize   uintptr
	MaximumWorkingSetSize   uintptr
	ActiveProcessLimit      uint32
	Affinity                uintptr
	PriorityClass           uint32
	SchedulingClass         uint32
}

type ioCounters struct {
	ReadOperationCount  uint64
	WriteOperationCount uint64
	OtherOperationCount uint64
	ReadTransferCount   uint64
	WriteTransferCount  uint64
	OtherTransferCount  uint64
}

type jobObjectExtendedLimitInformation struct {
	BasicLimitInformation jobObjectBasicLimitInformation
	IoInfo                ioCounters
	ProcessMemoryLimit    uintptr
	JobMemoryLimit        uintptr
	PeakProcessMemoryUsed uintptr
	PeakJobMemoryUsed     uintptr
}

type memoryStatusEx struct {
	Length               uint32
	MemoryLoad           uint32
	TotalPhys            uint64
	AvailPhys            uint64
	TotalPageFile        uint64
	AvailPageFile        uint64
	TotalVirtual         uint64
	AvailVirtual         uint64
	AvailExtendedVirtual uint64
}

// TotalPhysicalMemory returns installed RAM in bytes (0 when unavailable).
func TotalPhysicalMemory() uint64 {
	var status memoryStatusEx
	status.Length = uint32(unsafe.Sizeof(status))
	ok, _, _ := procGlobalMemoryStatusEx.Call(uintptr(unsafe.Pointer(&status)))
	if ok == 0 {
		return 0
	}
	return status.TotalPhys
}

func loword(v uintptr) uint32 { return uint32(v & 0xFFFF) }
func hiword(v uintptr) uint32 { return uint32((v >> 16) & 0xFFFF) }
func boolInt(v bool) uintptr {
	if v {
		return 1
	}
	return 0
}

func utf16Ptr(s string) *uint16 {
	p, err := windows.UTF16PtrFromString(s)
	if err != nil {
		empty, _ := windows.UTF16PtrFromString("")
		return empty
	}
	return p
}

// colorRef converts 8-bit RGB to a Windows COLORREF (0x00BBGGRR).
func colorRef(r, g, b uint8) uint32 {
	return uint32(r) | uint32(g)<<8 | uint32(b)<<16
}

// SetDPIAware enables per-monitor-v2 DPI awareness so the WebView2 content is
// rendered at native resolution on scaled displays.
func SetDPIAware() {
	if r, _, _ := procSetProcessDpiAwarenessCtx.Call(dpiAwarenessContextPerMonitorAwareV2); r != 0 {
		return
	}
	// Fallback for Windows < 10 1703.
	_, _, _ = procSetProcessDPIAware.Call()
}

// CoInitialize initializes COM in single-threaded apartment mode for the
// calling (UI) thread. Safe to call once per thread.
func CoInitialize() error {
	r, _, _ := procCoInitializeEx.Call(0, COINITApartmentThreaded)
	if int32(r) < 0 && uint32(r) != 0x00000001 { // S_FALSE is fine
		return syscall.Errno(uint32(r))
	}
	return nil
}

// CoUninitialize releases COM for the calling thread.
func CoUninitialize() {
	_, _, _ = procCoUninitialize.Call()
}

// Message shows a native message box. Safe to call before the window exists.
func Message(title, text string, flags uint32) {
	_, _, _ = procMessageBoxW.Call(
		0,
		uintptr(unsafe.Pointer(utf16Ptr(text))),
		uintptr(unsafe.Pointer(utf16Ptr(title))),
		uintptr(flags|MBSetForeground),
	)
}

// ShellOpen opens a file, folder or URL with the user's default handler.
func ShellOpen(target string) error {
	r, _, err := procShellExecuteW.Call(
		0,
		uintptr(unsafe.Pointer(utf16Ptr("open"))),
		uintptr(unsafe.Pointer(utf16Ptr(target))),
		0,
		0,
		SWShowNormal,
	)
	if r <= 32 {
		return err
	}
	return nil
}
