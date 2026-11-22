package winui

import (
	"encoding/json"
	"os"
	"path/filepath"
	"sync"
	"time"
	"unsafe"

	"golang.org/x/sys/windows"
)

// className is the window class queried by the single-instance logic.
const className = "BeatportDL.MainWindow"

// Options configures a Window.
type Options struct {
	Title  string
	Width  int // logical pixels (96 dpi)
	Height int
	MinW   int
	MinH   int
	// IconPath is an .ico file loaded for the title bar and taskbar.
	IconPath string
	// StateFile persists size/position between launches (may be empty).
	StateFile string

	OnSize  func(w, h int)
	OnDpi   func(scale float64)
	OnFocus func()
	// OnClose may return false to veto the close (e.g. while a download is
	// shutting down).
	OnClose func() bool
}

// Window is a native top-level window hosting the WebView2 control.
type Window struct {
	hwnd    uintptr
	scale   float64
	minW    int
	minH    int
	state   windowState
	options Options

	normalRect rect
	maximized  bool
	frameBrush uintptr

	threadID uintptr
	queueMu  sync.Mutex
	queue    []func()
}

type windowState struct {
	X, Y, W, H int32
	Maximized  bool
	Valid      bool
}

var (
	registryMu sync.RWMutex
	registry   = map[uintptr]*Window{}

	classOnce sync.Once
	classErr  error
	wndProcCb uintptr
	bgBrush   uintptr
)

func lookupWindow(hwnd uintptr) *Window {
	registryMu.RLock()
	defer registryMu.RUnlock()
	return registry[hwnd]
}

func registerWindow(hwnd uintptr, w *Window) {
	registryMu.Lock()
	registry[hwnd] = w
	registryMu.Unlock()
}

func unregisterWindow(hwnd uintptr) {
	registryMu.Lock()
	delete(registry, hwnd)
	registryMu.Unlock()
}

func wndProc(hwnd, message, wparam, lparam uintptr) uintptr {
	w := lookupWindow(hwnd)
	switch message {
	case WMGetMinMaxInfo:
		if w != nil {
			mmi := (*minMaxInfo)(unsafe.Pointer(lparam))
			mmi.PtMinTrackSize = point{
				X: int32(float64(w.minW) * w.scale),
				Y: int32(float64(w.minH) * w.scale),
			}
			monitor, _, _ := procMonitorFromWindow.Call(hwnd, MonitorDefaultToNearest)
			if monitor != 0 {
				mi := monitorInfo{CbSize: uint32(unsafe.Sizeof(monitorInfo{}))}
				if ok, _, _ := procGetMonitorInfoW.Call(monitor, uintptr(unsafe.Pointer(&mi))); ok != 0 {
					mmi.PtMaxPosition = point{X: mi.RcWork.Left, Y: mi.RcWork.Top}
					mmi.PtMaxSize = point{
						X: mi.RcWork.Right - mi.RcWork.Left,
						Y: mi.RcWork.Bottom - mi.RcWork.Top,
					}
				}
			}
			return 0
		}
	case WMSize:
		if w != nil && w.options.OnSize != nil {
			w.options.OnSize(int(loword(lparam)), int(hiword(lparam)))
		}
		return 0
	case WMDpiChanged:
		if w != nil {
			w.scale = float64(loword(wparam)) / 96.0
			r := (*rect)(unsafe.Pointer(lparam))
			_, _, _ = procSetWindowPos.Call(
				hwnd, 0,
				uintptr(r.Left), uintptr(r.Top),
				uintptr(r.Right-r.Left), uintptr(r.Bottom-r.Top),
				SWPNoZOrder|SWPNoActivate,
			)
			if w.options.OnDpi != nil {
				w.options.OnDpi(w.scale)
			}
		}
		return 0
	case WMActivate:
		if w != nil && w.options.OnFocus != nil && (wparam == 1 || wparam == 2) {
			w.options.OnFocus()
		}
	case WMSetFocus:
		if w != nil && w.options.OnFocus != nil {
			w.options.OnFocus()
		}
	case WMEraseBkgnd:
		if w != nil {
			brush := w.frameBrush
			if brush == 0 {
				brush = bgBrush
			}
			var cr rect
			_, _, _ = procGetClientRect.Call(hwnd, uintptr(unsafe.Pointer(&cr)))
			_, _, _ = procFillRect.Call(wparam, uintptr(unsafe.Pointer(&cr)), brush)
		}
		return 1
	case WMNCCalcSize:
		if wparam != 0 {
			return 0
		}
	case WMNCHitTest:
		if w != nil {
			if w.maximized {
				return HTClient
			}
			x := int32(int16(loword(lparam)))
			y := int32(int16(hiword(lparam)))
			var wr rect
			if ok, _, _ := procGetWindowRect.Call(hwnd, uintptr(unsafe.Pointer(&wr))); ok != 0 {
				border := int32(6 * w.scale)
				left := x < wr.Left+border
				right := x >= wr.Right-border
				top := y < wr.Top+border
				bottom := y >= wr.Bottom-border
				switch {
				case top && left:
					return HTTopLeft
				case top && right:
					return HTTopRight
				case bottom && left:
					return HTBottomLeft
				case bottom && right:
					return HTBottomRight
				case left:
					return HTLeft
				case right:
					return HTRight
				case top:
					return HTTop
				case bottom:
					return HTBottom
				}
			}
		}
		return HTClient
	case WMNCPaint:
		if w != nil {
			brush := w.frameBrush
			if brush == 0 {
				brush = bgBrush
			}
			if hdc, _, _ := procGetWindowDC.Call(hwnd); hdc != 0 {
				var wr rect
				_, _, _ = procGetWindowRect.Call(hwnd, uintptr(unsafe.Pointer(&wr)))
				fill := rect{0, 0, wr.Right - wr.Left, wr.Bottom - wr.Top}
				_, _, _ = procFillRect.Call(hdc, uintptr(unsafe.Pointer(&fill)), brush)
				_, _, _ = procReleaseDC.Call(hwnd, hdc)
			}
			return 0
		}
	case WMClose:
		if w != nil && w.options.OnClose != nil && !w.options.OnClose() {
			return 0
		}
		if w != nil {
			w.saveState()
		}
		_, _, _ = procDestroyWindow.Call(hwnd)
		return 0
	case WMDestroy:
		unregisterWindow(hwnd)
		_, _, _ = procPostQuitMessage.Call(0)
		return 0
	}
	r, _, _ := procDefWindowProcW.Call(hwnd, message, wparam, lparam)
	return r
}

func ensureClass(icon windows.Handle) error {
	classOnce.Do(func() {
		hInst, _, _ := procGetModuleHandleW.Call(0)
		cursor, _, _ := procLoadCursorW.Call(0, uintptr(IDCArrow))
		bgBrush, _, _ = procCreateSolidBrush.Call(uintptr(colorRef(11, 15, 20)))
		wndProcCb = windows.NewCallback(wndProc)

		wc := wndClassExW{
			CbSize:        uint32(unsafe.Sizeof(wndClassExW{})),
			LpfnWndProc:   wndProcCb,
			HInstance:     windows.Handle(hInst),
			HIcon:         icon,
			HIconSm:       icon,
			HCursor:       windows.Handle(cursor),
			HbrBackground: windows.Handle(bgBrush),
			LpszClassName: utf16Ptr(className),
		}
		atom, _, err := procRegisterClassExW.Call(uintptr(unsafe.Pointer(&wc)))
		if atom == 0 {
			classErr = err
		}
	})
	return classErr
}

func loadWindowState(path string) windowState {
	if path == "" {
		return windowState{}
	}
	data, err := os.ReadFile(path)
	if err != nil {
		return windowState{}
	}
	var st windowState
	if err := json.Unmarshal(data, &st); err != nil {
		return windowState{}
	}
	if st.W <= 400 || st.H <= 300 {
		st.Valid = false
	}
	return st
}

func systemDPI() float64 {
	if r, _, _ := procGetDpiForSystem.Call(); r > 0 {
		return float64(r) / 96.0
	}
	return 1
}

func workArea() rect {
	var r rect
	if ok, _, _ := procSystemParametersInfoW.Call(SPIGetWorkArea, 0, uintptr(unsafe.Pointer(&r)), 0); ok == 0 {
		return rect{0, 0, 1920, 1080}
	}
	return r
}

func rectHasUsableArea(r rect) bool {
	if r.Right <= r.Left || r.Bottom <= r.Top {
		return false
	}
	wa := workArea()
	left := r.Left
	if left < wa.Left {
		left = wa.Left
	}
	top := r.Top
	if top < wa.Top {
		top = wa.Top
	}
	right := r.Right
	if right > wa.Right {
		right = wa.Right
	}
	bottom := r.Bottom
	if bottom > wa.Bottom {
		bottom = wa.Bottom
	}
	return right-left >= 120 && bottom-top >= 80
}

func adjustWindowRect(r *rect, dpi uint32) {
	if ok, _, _ := procAdjustWindowRectExForDpi.Call(
		uintptr(unsafe.Pointer(r)),
		AppWindowStyle, 0, WSExAppWindow, uintptr(dpi),
	); ok != 0 {
		return
	}
	_, _, _ = procAdjustWindowRectEx.Call(
		uintptr(unsafe.Pointer(r)),
		AppWindowStyle, 0, WSExAppWindow,
	)
}

// NewWindow creates the native frame. The window is created hidden; call Show
// once the WebView2 content is ready to avoid a flash of empty window.
func NewWindow(opts Options) (*Window, error) {
	if opts.Width <= 0 {
		opts.Width = 1280
	}
	if opts.Height <= 0 {
		opts.Height = 820
	}
	if opts.MinW <= 0 {
		opts.MinW = 1024
	}
	if opts.MinH <= 0 {
		opts.MinH = 640
	}

	var icon windows.Handle
	if opts.IconPath != "" {
		h, _, _ := procLoadImageW.Call(
			0,
			uintptr(unsafe.Pointer(utf16Ptr(opts.IconPath))),
			ImageIcon, 0, 0,
			LRLoadFromFile|LRDefaultSize,
		)
		icon = windows.Handle(h)
	}
	if icon == 0 {
		h, _, _ := procLoadIconW.Call(0, uintptr(IDIApplication))
		icon = windows.Handle(h)
	}

	if err := ensureClass(icon); err != nil {
		return nil, err
	}

	state := loadWindowState(opts.StateFile)
	scale := systemDPI()
	dpi := uint32(scale * 96)

	// Compute the outer window size from the desired client size.
	clientW := int32(float64(opts.Width) * scale)
	clientH := int32(float64(opts.Height) * scale)
	if state.Valid {
		// Remembered sizes are outer-window sizes (GetWindowPlacement returns
		// screen coordinates), so use them verbatim to avoid growing the frame
		// on every launch.
		clientW = state.W
		clientH = state.H
	}
	winW, winH := clientW, clientH
	if !state.Valid {
		wr := rect{0, 0, clientW, clientH}
		adjustWindowRect(&wr, dpi)
		winW = wr.Right - wr.Left
		winH = wr.Bottom - wr.Top
	}

	var x, y int32
	if state.Valid {
		x, y = state.X, state.Y
	} else {
		wa := workArea()
		x = wa.Left + (wa.Right-wa.Left-winW)/2
		y = wa.Top + (wa.Bottom-wa.Top-winH)/2
	}

	hInst, _, _ := procGetModuleHandleW.Call(0)
	hwnd, _, err := procCreateWindowExW.Call(
		WSExAppWindow,
		uintptr(unsafe.Pointer(utf16Ptr(className))),
		uintptr(unsafe.Pointer(utf16Ptr(opts.Title))),
		AppWindowStyle,
		uintptr(x), uintptr(y),
		uintptr(winW), uintptr(winH),
		0, 0, hInst, 0,
	)
	if hwnd == 0 {
		return nil, err
	}

	w := &Window{
		hwnd:     hwnd,
		scale:    scale,
		minW:     opts.MinW,
		minH:     opts.MinH,
		state:    state,
		options:  opts,
		threadID: currentThreadID(),
	}
	w.state.X, w.state.Y = int32(x), int32(y)
	registerWindow(hwnd, w)
	w.applyDarkFrame()
	return w, nil
}

func currentThreadID() uintptr {
	r, _, _ := procGetCurrentThreadID.Call()
	return r
}

// Hwnd returns the native window handle.
func (w *Window) Hwnd() uintptr { return w.hwnd }

// Scale returns the current DPI scale factor (1.0 at 96 dpi).
func (w *Window) Scale() float64 { return w.scale }

// ClientSize returns the current client area size in physical pixels.
func (w *Window) ClientSize() (int, int) {
	var r rect
	_, _, _ = procGetClientRect.Call(w.hwnd, uintptr(unsafe.Pointer(&r)))
	return int(r.Right - r.Left), int(r.Bottom - r.Top)
}

// SetTitle updates the window caption.
func (w *Window) SetTitle(title string) {
	_, _, _ = procSetWindowTextW.Call(w.hwnd, uintptr(unsafe.Pointer(utf16Ptr(title))))
}

// applyDarkFrame asks DWM for a dark caption matching the app surface.
func (w *Window) applyDarkFrame() {
	dark := uint32(1)
	_, _, _ = procDwmSetWindowAttribute.Call(
		w.hwnd, DWMWAUseImmersiveDarkMode,
		uintptr(unsafe.Pointer(&dark)), unsafe.Sizeof(dark),
	)
	// Older Windows 10 builds used attribute 19 for the same feature.
	_, _, _ = procDwmSetWindowAttribute.Call(
		w.hwnd, 19,
		uintptr(unsafe.Pointer(&dark)), unsafe.Sizeof(dark),
	)

	caption := colorRef(11, 15, 20)
	border := colorRef(31, 41, 55)
	text := colorRef(230, 237, 247)
	_, _, _ = procDwmSetWindowAttribute.Call(w.hwnd, DWMWACaptionColor, uintptr(unsafe.Pointer(&caption)), 4)
	_, _, _ = procDwmSetWindowAttribute.Call(w.hwnd, DWMWABorderColor, uintptr(unsafe.Pointer(&border)), 4)
	_, _, _ = procDwmSetWindowAttribute.Call(w.hwnd, DWMWATextColor, uintptr(unsafe.Pointer(&text)), 4)

	corner := uint32(DWMWCPDoNotRound)
	_, _, _ = procDwmSetWindowAttribute.Call(w.hwnd, DWMWAWindowCorner, uintptr(unsafe.Pointer(&corner)), 4)
}

// Show makes the window visible (respecting the remembered maximized state)
// and brings it to the foreground.
func (w *Window) Show() {
	if w.state.Valid && !rectHasUsableArea(rect{
		Left: w.state.X, Top: w.state.Y,
		Right: w.state.X + w.state.W, Bottom: w.state.Y + w.state.H,
	}) {
		// A monitor was removed or a previous automated resize left the saved
		// normal rect off-screen. Fall back to the default window position
		// instead of restoring an invisible maximized window.
		w.state.Valid = false
		w.state.Maximized = false
	}
	if w.state.Valid && w.state.Maximized {
		_, _, _ = procShowWindow.Call(w.hwnd, SWShowNormal)
		w.normalRect = rect{w.state.X, w.state.Y, w.state.X + w.state.W, w.state.Y + w.state.H}
		w.maximizeToWorkArea()
	} else {
		_, _, _ = procShowWindow.Call(w.hwnd, SWShowNormal)
	}
	_, _, _ = procUpdateWindow.Call(w.hwnd)
	_, _, _ = procSetForegroundWindow.Call(w.hwnd)
}

// Focus requests keyboard focus for the window.
func (w *Window) Focus() {
	_, _, _ = procSetFocus.Call(w.hwnd)
}

// Close posts a WM_CLOSE to the window.
func (w *Window) Close() {
	_, _, _ = procPostMessageW.Call(w.hwnd, WMClose, 0, 0)
}

// StartDrag begins a native window move from the custom title bar. The call is
// marshalled to the UI thread because WM_NCLBUTTONDOWN enters a modal move loop.
func (w *Window) StartDrag() {
	w.Post(func() {
		if w.maximized && !w.waitForDragMove() {
			return
		}
		_, _, _ = procReleaseCapture.Call()
		_, _, _ = procSendMessageW.Call(w.hwnd, WMNCLButtonDown, HTCAPTION, 0)
	})
}

func (w *Window) waitForDragMove() bool {
	var start point
	if ok, _, _ := procGetCursorPos.Call(uintptr(unsafe.Pointer(&start))); ok == 0 {
		return false
	}
	for i := 0; i < 80; i++ {
		time.Sleep(10 * time.Millisecond)
		down, _, _ := procGetAsyncKeyState.Call(0x01) // VK_LBUTTON
		if uint16(down)&0x8000 == 0 {
			return false
		}
		var current point
		if ok, _, _ := procGetCursorPos.Call(uintptr(unsafe.Pointer(&current))); ok == 0 {
			continue
		}
		if abs32(current.X-start.X) >= 4 || abs32(current.Y-start.Y) >= 4 {
			w.restoreForDrag()
			return true
		}
	}
	return false
}

func abs32(v int32) int32 {
	if v < 0 {
		return -v
	}
	return v
}

func (w *Window) restoreForDrag() {
	var cursor point
	if ok, _, _ := procGetCursorPos.Call(uintptr(unsafe.Pointer(&cursor))); ok == 0 {
		return
	}
	var maximizedRect rect
	if ok, _, _ := procGetWindowRect.Call(w.hwnd, uintptr(unsafe.Pointer(&maximizedRect))); ok == 0 {
		return
	}
	normal := w.normalRect
	wa := workArea()
	if !rectHasUsableArea(normal) {
		width := w.options.Width
		height := w.options.Height
		if width <= 0 {
			width = 1200
		}
		if height <= 0 {
			height = 800
		}
		left := wa.Left + (wa.Right-wa.Left-int32(width))/2
		top := wa.Top + (wa.Bottom-wa.Top-int32(height))/2
		normal = rect{left, top, left + int32(width), top + int32(height)}
	}
	width := normal.Right - normal.Left
	height := normal.Bottom - normal.Top
	if width <= 0 || height <= 0 {
		return
	}
	if width > wa.Right-wa.Left {
		width = wa.Right - wa.Left
	}
	if height > wa.Bottom-wa.Top {
		height = wa.Bottom - wa.Top
	}
	maxWidth := maximizedRect.Right - maximizedRect.Left
	fractionX := 0.5
	if maxWidth > 0 {
		fractionX = float64(cursor.X-maximizedRect.Left) / float64(maxWidth)
	}
	if fractionX < 0 {
		fractionX = 0
	}
	if fractionX > 1 {
		fractionX = 1
	}
	left := cursor.X - int32(float64(width)*fractionX)
	grabY := cursor.Y - maximizedRect.Top
	if grabY < 8 {
		grabY = 8
	}
	if grabY > 38 {
		grabY = 38
	}
	top := cursor.Y - grabY
	if left < wa.Left {
		left = wa.Left
	}
	if left+width > wa.Right {
		left = wa.Right - width
	}
	if top < wa.Top {
		top = wa.Top
	}
	if top+height > wa.Bottom {
		top = wa.Bottom - height
	}
	_, _, _ = procSetWindowPos.Call(
		w.hwnd, 0,
		uintptr(left), uintptr(top),
		uintptr(width), uintptr(height),
		SWPNoZOrder,
	)
	w.maximized = false
	w.normalRect = rect{left, top, left + width, top + height}
}

// SetAppearance switches the native frame between the dark and paper themes.
// The popup window still has a resize frame, so its DWM border and class brush
// must follow the in-page theme instead of staying dark.
func (w *Window) SetAppearance(light bool) {
	w.Post(func() {
		dark := uint32(1)
		border := colorRef(31, 41, 55)
		caption := colorRef(11, 15, 20)
		text := colorRef(230, 237, 247)
		brushColor := colorRef(11, 15, 20)
		if light {
			dark = 0
			border = colorRef(206, 204, 194)
			caption = colorRef(246, 244, 236)
			text = colorRef(16, 18, 15)
			brushColor = colorRef(238, 237, 231)
		}
		_, _, _ = procDwmSetWindowAttribute.Call(
			w.hwnd, DWMWAUseImmersiveDarkMode,
			uintptr(unsafe.Pointer(&dark)), unsafe.Sizeof(dark),
		)
		_, _, _ = procDwmSetWindowAttribute.Call(w.hwnd, DWMWABorderColor, uintptr(unsafe.Pointer(&border)), 4)
		_, _, _ = procDwmSetWindowAttribute.Call(w.hwnd, DWMWACaptionColor, uintptr(unsafe.Pointer(&caption)), 4)
		_, _, _ = procDwmSetWindowAttribute.Call(w.hwnd, DWMWATextColor, uintptr(unsafe.Pointer(&text)), 4)
		corner := uint32(DWMWCPDoNotRound)
		_, _, _ = procDwmSetWindowAttribute.Call(w.hwnd, DWMWAWindowCorner, uintptr(unsafe.Pointer(&corner)), 4)

		brush, _, _ := procCreateSolidBrush.Call(uintptr(brushColor))
		if brush != 0 {
			if w.frameBrush != 0 {
				_, _, _ = procDeleteObject.Call(w.frameBrush)
			}
			w.frameBrush = brush
			_, _, _ = procSetClassLongPtrW.Call(w.hwnd, GCLPBackground, brush)
		}
		_, _, _ = procSetWindowPos.Call(
			w.hwnd, 0, 0, 0, 0, 0,
			SWPNoMove|SWPNoSize|SWPNoZOrder|SWPNoActivate|SWPFrameChanged,
		)
		_, _, _ = procRedrawWindow.Call(
			w.hwnd, 0, 0,
			RDWInvalidate|RDWFrame|RDWAllChildren,
		)
	})
}

// StartResize starts a native sizing loop in the requested direction. The
// invisible HTML edge handles call this instead of relying on a painted frame.
func (w *Window) StartResize(hitTest uintptr) {
	w.Post(func() {
		if w.maximized {
			return
		}
		_, _, _ = procReleaseCapture.Call()
		_, _, _ = procSendMessageW.Call(w.hwnd, WMNCLButtonDown, hitTest, 0)
	})
}

// ToggleMaximize restores a maximized window or maximizes a normal one.
func (w *Window) ToggleMaximize() {
	w.Post(func() {
		if w.maximized {
			r := w.normalRect
			if r.Right > r.Left && r.Bottom > r.Top {
				_, _, _ = procSetWindowPos.Call(
					w.hwnd, 0,
					uintptr(r.Left), uintptr(r.Top),
					uintptr(r.Right-r.Left), uintptr(r.Bottom-r.Top),
					SWPNoZOrder,
				)
			}
			w.maximized = false
			return
		}
		w.maximizeToWorkArea()
	})
}

func (w *Window) maximizeToWorkArea() {
	var wr rect
	if ok, _, _ := procGetWindowRect.Call(w.hwnd, uintptr(unsafe.Pointer(&wr))); ok == 0 {
		return
	}
	monitor, _, _ := procMonitorFromWindow.Call(w.hwnd, MonitorDefaultToNearest)
	if monitor == 0 {
		return
	}
	mi := monitorInfo{CbSize: uint32(unsafe.Sizeof(monitorInfo{}))}
	if ok, _, _ := procGetMonitorInfoW.Call(monitor, uintptr(unsafe.Pointer(&mi))); ok == 0 {
		return
	}
	w.normalRect = wr
	w.maximized = true
	_, _, _ = procSetWindowPos.Call(
		w.hwnd, 0,
		uintptr(mi.RcWork.Left), uintptr(mi.RcWork.Top),
		uintptr(mi.RcWork.Right-mi.RcWork.Left),
		uintptr(mi.RcWork.Bottom-mi.RcWork.Top),
		SWPNoZOrder,
	)
}

// Minimize minimizes the window.
func (w *Window) Minimize() {
	w.Post(func() {
		_, _, _ = procShowWindow.Call(w.hwnd, SWMinimize)
	})
}

// Post schedules fn on the UI thread. Safe to call from any goroutine.
func (w *Window) Post(fn func()) {
	if fn == nil {
		return
	}
	w.queueMu.Lock()
	w.queue = append(w.queue, fn)
	w.queueMu.Unlock()
	_, _, _ = procPostThreadMessageW.Call(w.threadID, WMApp, 0, 0)
}

func (w *Window) drainQueue() {
	w.queueMu.Lock()
	q := w.queue
	w.queue = nil
	w.queueMu.Unlock()
	for _, fn := range q {
		fn()
	}
}

// Run pumps the message loop until the window closes. It must be called on the
// thread that created the window.
func (w *Window) Run() {
	var m msg
	for {
		r, _, _ := procGetMessageW.Call(uintptr(unsafe.Pointer(&m)), 0, 0, 0)
		if int32(r) <= 0 {
			break
		}
		if m.Message == WMApp {
			w.drainQueue()
			continue
		}
		_, _, _ = procTranslateMessage.Call(uintptr(unsafe.Pointer(&m)))
		_, _, _ = procDispatchMessageW.Call(uintptr(unsafe.Pointer(&m)))
	}
	w.saveState()
}

func (w *Window) saveState() {
	if w.options.StateFile == "" {
		return
	}
	if w.maximized && w.normalRect.Right > w.normalRect.Left && w.normalRect.Bottom > w.normalRect.Top {
		data, err := json.Marshal(windowState{
			X:         w.normalRect.Left,
			Y:         w.normalRect.Top,
			W:         w.normalRect.Right - w.normalRect.Left,
			H:         w.normalRect.Bottom - w.normalRect.Top,
			Maximized: true,
			Valid:     true,
		})
		if err == nil {
			_ = os.MkdirAll(filepath.Dir(w.options.StateFile), 0o755)
			_ = os.WriteFile(w.options.StateFile, data, 0o644)
		}
		return
	}
	var wp windowPlacement
	wp.Length = uint32(unsafe.Sizeof(windowPlacement{}))
	if ok, _, _ := procGetWindowPlacement.Call(w.hwnd, uintptr(unsafe.Pointer(&wp))); ok == 0 {
		return
	}
	maximized, _, _ := procIsZoomed.Call(w.hwnd)
	st := windowState{
		X:         wp.RcNormalPosition.Left,
		Y:         wp.RcNormalPosition.Top,
		W:         wp.RcNormalPosition.Right - wp.RcNormalPosition.Left,
		H:         wp.RcNormalPosition.Bottom - wp.RcNormalPosition.Top,
		Maximized: maximized != 0,
		Valid:     true,
	}
	if st.W <= 0 || st.H <= 0 {
		return
	}
	data, err := json.Marshal(st)
	if err != nil {
		return
	}
	_ = os.MkdirAll(filepath.Dir(w.options.StateFile), 0o755)
	_ = os.WriteFile(w.options.StateFile, data, 0o644)
}

// FocusExistingWindow brings an already-running instance to the foreground.
func FocusExistingWindow() bool {
	hwnd, _, _ := procFindWindowW.Call(
		uintptr(unsafe.Pointer(utf16Ptr(className))), 0,
	)
	if hwnd == 0 {
		return false
	}
	_, _, _ = procShowWindow.Call(hwnd, SWRestore)
	_, _, _ = procSetForegroundWindow.Call(hwnd)
	return true
}

// AcquireSingleInstance creates a named mutex. The returned bool reports
// whether this process is the first instance; the handle must stay alive for
// the process lifetime.
func AcquireSingleInstance(name string) (windows.Handle, bool) {
	h, _, err := procCreateMutexW.Call(0, 0, uintptr(unsafe.Pointer(utf16Ptr(name))))
	if h == 0 {
		return 0, true // mutex creation failed: do not block startup
	}
	if err == windows.ERROR_ALREADY_EXISTS {
		return windows.Handle(h), false
	}
	return windows.Handle(h), true
}

// KillOnCloseJob creates a job object that terminates its processes when the
// handle is closed (i.e. when this process exits, even abnormally).
func KillOnCloseJob() (windows.Handle, error) {
	h, _, err := procCreateJobObjectW.Call(0, 0)
	if h == 0 {
		return 0, err
	}
	var info jobObjectExtendedLimitInformation
	info.BasicLimitInformation.LimitFlags = JOBObjectLimitKillOnJobClose
	ok, _, err := procSetInformationJobObject.Call(
		h,
		JobObjectExtendedLimitInformation,
		uintptr(unsafe.Pointer(&info)),
		unsafe.Sizeof(info),
	)
	if ok == 0 {
		windows.CloseHandle(windows.Handle(h))
		return 0, err
	}
	return windows.Handle(h), nil
}

// AssignProcessToJob adds pid to job. Failures are non-fatal: the app also
// kills the child explicitly on shutdown.
func AssignProcessToJob(job windows.Handle, pid int) error {
	const processSetQuota = 0x0100
	const processTerminate = 0x0001
	ph, err := windows.OpenProcess(processSetQuota|processTerminate, false, uint32(pid))
	if err != nil {
		return err
	}
	defer windows.CloseHandle(ph)
	ok, _, callErr := procAssignProcessToJobObject.Call(uintptr(job), uintptr(ph))
	if ok == 0 {
		return callErr
	}
	return nil
}

// WorkArea returns the primary monitor work area in physical pixels.
func WorkArea() (x, y, w, h int) {
	wa := workArea()
	return int(wa.Left), int(wa.Top), int(wa.Right - wa.Left), int(wa.Bottom - wa.Top)
}
