//go:build windows
// +build windows

package edge

import (
	"unsafe"
	"unspok3n/beatportdl/internal/webview2/w32"
)

func (e *Chromium) Resize() {
	if e.controller == nil {
		return
	}
	var bounds w32.Rect
	w32.User32GetClientRect.Call(e.hwnd, uintptr(unsafe.Pointer(&bounds)))
	if e.inset > 0 && bounds.Right-bounds.Left > e.inset*2 && bounds.Bottom-bounds.Top > e.inset*2 {
		bounds.Left += e.inset
		bounds.Top += e.inset
		bounds.Right -= e.inset
		bounds.Bottom -= e.inset
	}
	e.controller.vtbl.PutBounds.Call(
		uintptr(unsafe.Pointer(e.controller)),
		uintptr(bounds.Left),
		uintptr(bounds.Top),
		uintptr(bounds.Right),
		uintptr(bounds.Bottom),
	)
}

// SetInset leaves a small host-painted resize border around the WebView.
func (e *Chromium) SetInset(px int32) {
	e.inset = px
	e.Resize()
}
