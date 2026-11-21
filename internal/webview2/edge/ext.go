package edge

import (
	"fmt"
	"unsafe"

	"golang.org/x/sys/windows"
)

// This file adds the WebView2 features the BeatportDL desktop shell needs on
// top of the vendored go-webview2 COM layer: virtual-host mapping (the UI is
// served from an app-internal https origin instead of a local HTTP port), a
// JSON message bridge, dark default background, DPI auto-detection, external
// link handling and window title sync.

// ---- ICoreWebView2 extras -------------------------------------------------

const (
	CoreWebView2Controller3IID = "{f9614724-5d2b-41dc-aef7-73d62b51543b}"
)

// PostWebMessageJSON posts a JSON string to the page. The page receives it via
// window.chrome.webview's "message" event.
func (e *Chromium) PostWebMessageJSON(payload string) error {
	if e.webview == nil {
		return fmt.Errorf("webview2: PostWebMessageJSON before controller ready")
	}
	p, err := windows.UTF16PtrFromString(payload)
	if err != nil {
		return err
	}
	hr, _, _ := e.webview.vtbl.PostWebMessageAsJSON.Call(
		uintptr(unsafe.Pointer(e.webview)),
		uintptr(unsafe.Pointer(p)),
	)
	if int32(hr) < 0 {
		return fmt.Errorf("webview2: PostWebMessageAsJSON failed: HRESULT 0x%08X", uint32(hr))
	}
	return nil
}

// SetVirtualHostMapping maps a synthetic host name to a local folder so the UI
// loads from https://<host>/ without opening any TCP port. Access kind ALLOW is
// used; the page and its assets are same-origin by construction.
func (e *Chromium) SetVirtualHostMapping(host, folder string) error {
	v3 := e.GetICoreWebView2_3()
	if v3 == nil {
		return fmt.Errorf("webview2: ICoreWebView2_3 is unavailable")
	}
	return v3.SetVirtualHostNameToFolderMapping(host, folder, COREWEBVIEW2_HOST_RESOURCE_ACCESS_KIND_ALLOW)
}

// PutDefaultBackgroundColor sets the surface color WebView2 paints before the
// page renders, avoiding a white flash on startup.
func (e *Chromium) PutDefaultBackgroundColor(r, g, b uint8) error {
	if e.controller == nil {
		return fmt.Errorf("webview2: controller is not ready")
	}
	c2 := e.controller.GetICoreWebView2Controller2()
	if c2 == nil {
		return fmt.Errorf("webview2: ICoreWebView2Controller2 is unavailable")
	}
	return c2.PutDefaultBackgroundColor(COREWEBVIEW2_COLOR{A: 255, R: r, G: g, B: b})
}

// Controller3 exposes monitor scale auto-detection which keeps the WebView
// crisp when the window is moved between displays with different DPI.
type iCoreWebView2Controller3Vtbl struct {
	_ICoreWebView2Controller2Vtbl
	GetRasterizationScale              ComProc
	PutRasterizationScale              ComProc
	GetShouldDetectMonitorScaleChanges ComProc
	PutShouldDetectMonitorScaleChanges ComProc
	AddRasterizationScaleChanged       ComProc
	RemoveRasterizationScaleChanged    ComProc
	GetBoundsMode                      ComProc
	PutBoundsMode                      ComProc
}

type ICoreWebView2Controller3 struct {
	vtbl *iCoreWebView2Controller3Vtbl
}

// SetMonitorScaleAutoDetect asks WebView2 to track per-monitor DPI changes.
func (e *Chromium) SetMonitorScaleAutoDetect(enabled bool) error {
	if e.controller == nil {
		return fmt.Errorf("webview2: controller is not ready")
	}
	iid := NewGUID(CoreWebView2Controller3IID)
	var c3 *ICoreWebView2Controller3
	hr, _, _ := e.controller.vtbl.QueryInterface.Call(
		uintptr(unsafe.Pointer(e.controller)),
		uintptr(unsafe.Pointer(iid)),
		uintptr(unsafe.Pointer(&c3)),
	)
	if int32(hr) < 0 || c3 == nil {
		return fmt.Errorf("webview2: ICoreWebView2Controller3 is unavailable: HRESULT 0x%08X", uint32(hr))
	}
	value := uintptr(0)
	if enabled {
		value = 1
	}
	hr, _, _ = c3.vtbl.PutShouldDetectMonitorScaleChanges.Call(
		uintptr(unsafe.Pointer(c3)),
		value,
	)
	if int32(hr) < 0 {
		return fmt.Errorf("webview2: put_ShouldDetectMonitorScaleChanges failed: HRESULT 0x%08X", uint32(hr))
	}
	return nil
}

// OpenDevTools opens the Chromium DevTools window for the current page.
func (e *Chromium) OpenDevTools() {
	if e.webview == nil {
		return
	}
	_, _, _ = e.webview.vtbl.OpenDevToolsWindow.Call(uintptr(unsafe.Pointer(e.webview)))
}

// Reload reloads the current document.
func (e *Chromium) Reload() {
	if e.webview == nil {
		return
	}
	_, _, _ = e.webview.vtbl.Reload.Call(uintptr(unsafe.Pointer(e.webview)))
}

// DocumentTitle returns the document title reported by Chromium.
func (i *ICoreWebView2) DocumentTitle() (string, error) {
	var title *uint16
	hr, _, _ := i.vtbl.GetDocumentTitle.Call(
		uintptr(unsafe.Pointer(i)),
		uintptr(unsafe.Pointer(&title)),
	)
	if int32(hr) < 0 {
		return "", fmt.Errorf("webview2: get_DocumentTitle failed: HRESULT 0x%08X", uint32(hr))
	}
	defer windows.CoTaskMemFree(unsafe.Pointer(title))
	if title == nil {
		return "", nil
	}
	return windows.UTF16PtrToString(title), nil
}

// ---- navigation starting --------------------------------------------------

type iCoreWebView2NavigationStartingEventArgsVtbl struct {
	_IUnknownVtbl
	GetUri             ComProc
	GetIsUserInitiated ComProc
	GetIsRedirected    ComProc
	GetRequestHeaders  ComProc
	GetCancel          ComProc
	PutCancel          ComProc
	GetNavigationID    ComProc
}

// ICoreWebView2NavigationStartingEventArgs is passed to navigation events.
type ICoreWebView2NavigationStartingEventArgs struct {
	vtbl *iCoreWebView2NavigationStartingEventArgsVtbl
}

// URI returns the target URI of the navigation.
func (a *ICoreWebView2NavigationStartingEventArgs) URI() (string, error) {
	var uri *uint16
	hr, _, _ := a.vtbl.GetUri.Call(
		uintptr(unsafe.Pointer(a)),
		uintptr(unsafe.Pointer(&uri)),
	)
	if int32(hr) < 0 {
		return "", fmt.Errorf("webview2: get_Uri failed: HRESULT 0x%08X", uint32(hr))
	}
	defer windows.CoTaskMemFree(unsafe.Pointer(uri))
	if uri == nil {
		return "", nil
	}
	return windows.UTF16PtrToString(uri), nil
}

// Cancel stops the navigation when called from a NavigationStarting handler.
func (a *ICoreWebView2NavigationStartingEventArgs) Cancel() {
	_, _, _ = a.vtbl.PutCancel.Call(uintptr(unsafe.Pointer(a)), 1)
}

type iCoreWebView2NavigationStartingEventHandlerVtbl struct {
	_IUnknownVtbl
	Invoke ComProc
}

// ICoreWebView2NavigationStartingEventHandler receives navigation events.
type ICoreWebView2NavigationStartingEventHandler struct {
	vtbl *iCoreWebView2NavigationStartingEventHandlerVtbl
	impl iCoreWebView2NavigationStartingEventHandlerImpl
}

type iCoreWebView2NavigationStartingEventHandlerImpl interface {
	_IUnknownImpl
	NavigationStarting(sender *ICoreWebView2, args *ICoreWebView2NavigationStartingEventArgs) uintptr
}

func _navStartingQI(this *ICoreWebView2NavigationStartingEventHandler, refiid, object uintptr) uintptr {
	return this.impl.QueryInterface(refiid, object)
}
func _navStartingAddRef(this *ICoreWebView2NavigationStartingEventHandler) uintptr {
	return this.impl.AddRef()
}
func _navStartingRelease(this *ICoreWebView2NavigationStartingEventHandler) uintptr {
	return this.impl.Release()
}
func _navStartingInvoke(this *ICoreWebView2NavigationStartingEventHandler, sender *ICoreWebView2, args *ICoreWebView2NavigationStartingEventArgs) uintptr {
	return this.impl.NavigationStarting(sender, args)
}

var iCoreWebView2NavigationStartingEventHandlerFn = iCoreWebView2NavigationStartingEventHandlerVtbl{
	_IUnknownVtbl{
		NewComProc(_navStartingQI),
		NewComProc(_navStartingAddRef),
		NewComProc(_navStartingRelease),
	},
	NewComProc(_navStartingInvoke),
}

func newICoreWebView2NavigationStartingEventHandler(impl iCoreWebView2NavigationStartingEventHandlerImpl) *ICoreWebView2NavigationStartingEventHandler {
	return &ICoreWebView2NavigationStartingEventHandler{
		vtbl: &iCoreWebView2NavigationStartingEventHandlerFn,
		impl: impl,
	}
}

// AddNavigationStarting registers a navigation-starting event handler.
func (i *ICoreWebView2) AddNavigationStarting(h *ICoreWebView2NavigationStartingEventHandler, token *_EventRegistrationToken) error {
	hr, _, _ := i.vtbl.AddNavigationStarting.Call(
		uintptr(unsafe.Pointer(i)),
		uintptr(unsafe.Pointer(h)),
		uintptr(unsafe.Pointer(token)),
	)
	if int32(hr) < 0 {
		return fmt.Errorf("webview2: add_NavigationStarting failed: HRESULT 0x%08X", uint32(hr))
	}
	return nil
}

// ---- new window requested -------------------------------------------------

type iCoreWebView2NewWindowRequestedEventArgsVtbl struct {
	_IUnknownVtbl
	GetUri             ComProc
	PutNewWindow       ComProc
	GetNewWindow       ComProc
	PutHandled         ComProc
	GetHandled         ComProc
	GetIsUserInitiated ComProc
	GetDeferral        ComProc
	GetWindowFeatures  ComProc
}

// ICoreWebView2NewWindowRequestedEventArgs is passed to new-window requests.
type ICoreWebView2NewWindowRequestedEventArgs struct {
	vtbl *iCoreWebView2NewWindowRequestedEventArgsVtbl
}

// URI returns the requested URI.
func (a *ICoreWebView2NewWindowRequestedEventArgs) URI() (string, error) {
	var uri *uint16
	hr, _, _ := a.vtbl.GetUri.Call(
		uintptr(unsafe.Pointer(a)),
		uintptr(unsafe.Pointer(&uri)),
	)
	if int32(hr) < 0 {
		return "", fmt.Errorf("webview2: get_Uri failed: HRESULT 0x%08X", uint32(hr))
	}
	defer windows.CoTaskMemFree(unsafe.Pointer(uri))
	if uri == nil {
		return "", nil
	}
	return windows.UTF16PtrToString(uri), nil
}

// SetHandled marks the request as handled so WebView2 opens no popup.
func (a *ICoreWebView2NewWindowRequestedEventArgs) SetHandled(handled bool) {
	v := uintptr(0)
	if handled {
		v = 1
	}
	_, _, _ = a.vtbl.PutHandled.Call(uintptr(unsafe.Pointer(a)), v)
}

type iCoreWebView2NewWindowRequestedEventHandlerVtbl struct {
	_IUnknownVtbl
	Invoke ComProc
}

// ICoreWebView2NewWindowRequestedEventHandler receives popup requests.
type ICoreWebView2NewWindowRequestedEventHandler struct {
	vtbl *iCoreWebView2NewWindowRequestedEventHandlerVtbl
	impl iCoreWebView2NewWindowRequestedEventHandlerImpl
}

type iCoreWebView2NewWindowRequestedEventHandlerImpl interface {
	_IUnknownImpl
	NewWindowRequested(sender *ICoreWebView2, args *ICoreWebView2NewWindowRequestedEventArgs) uintptr
}

func _newWindowQI(this *ICoreWebView2NewWindowRequestedEventHandler, refiid, object uintptr) uintptr {
	return this.impl.QueryInterface(refiid, object)
}
func _newWindowAddRef(this *ICoreWebView2NewWindowRequestedEventHandler) uintptr {
	return this.impl.AddRef()
}
func _newWindowRelease(this *ICoreWebView2NewWindowRequestedEventHandler) uintptr {
	return this.impl.Release()
}
func _newWindowInvoke(this *ICoreWebView2NewWindowRequestedEventHandler, sender *ICoreWebView2, args *ICoreWebView2NewWindowRequestedEventArgs) uintptr {
	return this.impl.NewWindowRequested(sender, args)
}

var iCoreWebView2NewWindowRequestedEventHandlerFn = iCoreWebView2NewWindowRequestedEventHandlerVtbl{
	_IUnknownVtbl{
		NewComProc(_newWindowQI),
		NewComProc(_newWindowAddRef),
		NewComProc(_newWindowRelease),
	},
	NewComProc(_newWindowInvoke),
}

func newICoreWebView2NewWindowRequestedEventHandler(impl iCoreWebView2NewWindowRequestedEventHandlerImpl) *ICoreWebView2NewWindowRequestedEventHandler {
	return &ICoreWebView2NewWindowRequestedEventHandler{
		vtbl: &iCoreWebView2NewWindowRequestedEventHandlerFn,
		impl: impl,
	}
}

// AddNewWindowRequested registers a popup request handler.
func (i *ICoreWebView2) AddNewWindowRequested(h *ICoreWebView2NewWindowRequestedEventHandler, token *_EventRegistrationToken) error {
	hr, _, _ := i.vtbl.AddNewWindowRequested.Call(
		uintptr(unsafe.Pointer(i)),
		uintptr(unsafe.Pointer(h)),
		uintptr(unsafe.Pointer(token)),
	)
	if int32(hr) < 0 {
		return fmt.Errorf("webview2: add_NewWindowRequested failed: HRESULT 0x%08X", uint32(hr))
	}
	return nil
}

// ---- document title changed -----------------------------------------------

type iCoreWebView2DocumentTitleChangedEventHandlerVtbl struct {
	_IUnknownVtbl
	Invoke ComProc
}

// ICoreWebView2DocumentTitleChangedEventHandler receives title changes.
type ICoreWebView2DocumentTitleChangedEventHandler struct {
	vtbl *iCoreWebView2DocumentTitleChangedEventHandlerVtbl
	impl iCoreWebView2DocumentTitleChangedEventHandlerImpl
}

type iCoreWebView2DocumentTitleChangedEventHandlerImpl interface {
	_IUnknownImpl
	DocumentTitleChanged(sender *ICoreWebView2) uintptr
}

func _titleQI(this *ICoreWebView2DocumentTitleChangedEventHandler, refiid, object uintptr) uintptr {
	return this.impl.QueryInterface(refiid, object)
}
func _titleAddRef(this *ICoreWebView2DocumentTitleChangedEventHandler) uintptr {
	return this.impl.AddRef()
}
func _titleRelease(this *ICoreWebView2DocumentTitleChangedEventHandler) uintptr {
	return this.impl.Release()
}
func _titleInvoke(this *ICoreWebView2DocumentTitleChangedEventHandler, sender *ICoreWebView2) uintptr {
	return this.impl.DocumentTitleChanged(sender)
}

var iCoreWebView2DocumentTitleChangedEventHandlerFn = iCoreWebView2DocumentTitleChangedEventHandlerVtbl{
	_IUnknownVtbl{
		NewComProc(_titleQI),
		NewComProc(_titleAddRef),
		NewComProc(_titleRelease),
	},
	NewComProc(_titleInvoke),
}

func newICoreWebView2DocumentTitleChangedEventHandler(impl iCoreWebView2DocumentTitleChangedEventHandlerImpl) *ICoreWebView2DocumentTitleChangedEventHandler {
	return &ICoreWebView2DocumentTitleChangedEventHandler{
		vtbl: &iCoreWebView2DocumentTitleChangedEventHandlerFn,
		impl: impl,
	}
}

// AddDocumentTitleChanged registers a document title handler.
func (i *ICoreWebView2) AddDocumentTitleChanged(h *ICoreWebView2DocumentTitleChangedEventHandler, token *_EventRegistrationToken) error {
	hr, _, _ := i.vtbl.AddDocumentTitleChanged.Call(
		uintptr(unsafe.Pointer(i)),
		uintptr(unsafe.Pointer(h)),
		uintptr(unsafe.Pointer(token)),
	)
	if int32(hr) < 0 {
		return fmt.Errorf("webview2: add_DocumentTitleChanged failed: HRESULT 0x%08X", uint32(hr))
	}
	return nil
}
