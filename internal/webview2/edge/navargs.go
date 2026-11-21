package edge

import (
	"fmt"
	"unsafe"
)

// IsSuccess reports whether the navigation completed successfully.
func (i *ICoreWebView2NavigationCompletedEventArgs) IsSuccess() (bool, error) {
	var success int32
	hr, _, _ := i.vtbl.GetIsSuccess.Call(
		uintptr(unsafe.Pointer(i)),
		uintptr(unsafe.Pointer(&success)),
	)
	if int32(hr) < 0 {
		return false, fmt.Errorf("webview2: get_IsSuccess failed: HRESULT 0x%08X", uint32(hr))
	}
	return success != 0, nil
}

// WebErrorStatus returns the COREWEBVIEW2_WEB_ERROR_STATUS value (0 = none).
func (i *ICoreWebView2NavigationCompletedEventArgs) WebErrorStatus() (int32, error) {
	var status int32
	hr, _, _ := i.vtbl.GetWebErrorStatus.Call(
		uintptr(unsafe.Pointer(i)),
		uintptr(unsafe.Pointer(&status)),
	)
	if int32(hr) < 0 {
		return 0, fmt.Errorf("webview2: get_WebErrorStatus failed: HRESULT 0x%08X", uint32(hr))
	}
	return status, nil
}
