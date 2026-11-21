package winui

import (
	"fmt"
	"runtime"
	"syscall"
	"unsafe"

	"golang.org/x/sys/windows"
)

// This file implements the modern IFileOpenDialog-based folder picker. The
// legacy SHBrowseForFolder dialog looks dated; this one matches the explorer
// visuals of Windows 10/11 and is what the "更改…" button in settings opens.
//
// The dialog is created and shown on a dedicated STA thread so it never
// re-enters the WebView2 apartment (the UI thread runs a CoreWebView2 message
// callback when the page asks for a folder).

type comProc uintptr

func (p comProc) call(args ...uintptr) uintptr {
	r, _, _ := syscall.SyscallN(uintptr(p), args...)
	return r
}

type guid struct {
	Data1 uint32
	Data2 uint16
	Data3 uint16
	Data4 [8]byte
}

var (
	clsidFileOpenDialog = guid{0xDC1C5A9C, 0xE88A, 0x4dde, [8]byte{0xA5, 0xA1, 0x60, 0xF8, 0x2A, 0x20, 0xAE, 0xF7}}
	iidFileOpenDialog   = guid{0xd57c7288, 0xd4ad, 0x4768, [8]byte{0xbe, 0x02, 0x9d, 0x96, 0x95, 0x32, 0xd9, 0x60}}
	iidShellItem        = guid{0x43826d1e, 0xe718, 0x42ee, [8]byte{0xbc, 0x55, 0xa1, 0xe2, 0x61, 0xc3, 0x7b, 0xfe}}
)

var dialogLog = func(string) {}

// SetLogger installs an optional diagnostic logger for shell dialogs.
func SetLogger(f func(string)) {
	if f != nil {
		dialogLog = f
	}
}

const (
	fosPickFolders     = 0x00000020
	fosForceFileSystem = 0x00000040
	fosNoChangeDir     = 0x00000008
	fosPathMustExist   = 0x00000800
	sigdnFileSysPath   = 0x80058000
)

type iFileDialogVtbl struct {
	QueryInterface      comProc
	AddRef              comProc
	Release             comProc
	Show                comProc
	SetFileTypes        comProc
	SetFileTypeIndex    comProc
	GetFileTypeIndex    comProc
	Advise              comProc
	Unadvise            comProc
	SetOptions          comProc
	GetOptions          comProc
	SetDefaultFolder    comProc
	SetFolder           comProc
	GetFolder           comProc
	GetCurrentSelection comProc
	SetFileName         comProc
	GetFileName         comProc
	SetTitle            comProc
	SetOkButtonLabel    comProc
	SetFileNameLabel    comProc
	GetResult           comProc
	AddPlace            comProc
	SetDefaultExtension comProc
	Close               comProc
	SetClientGuid       comProc
	ClearClientData     comProc
	SetFilter           comProc
	GetResults          comProc
	GetSelectedItems    comProc
}

type iFileOpenDialog struct {
	vtbl *iFileDialogVtbl
}

func (d *iFileOpenDialog) release() {
	if d != nil && d.vtbl != nil {
		d.vtbl.Release.call(uintptr(unsafe.Pointer(d)))
	}
}

func (d *iFileOpenDialog) setOptions(options uint32) {
	d.vtbl.SetOptions.call(uintptr(unsafe.Pointer(d)), uintptr(options))
}

func (d *iFileOpenDialog) getOptions() uint32 {
	var opts uint32
	d.vtbl.GetOptions.call(uintptr(unsafe.Pointer(d)), uintptr(unsafe.Pointer(&opts)))
	return opts
}

func (d *iFileOpenDialog) setTitle(title string) {
	if title == "" {
		return
	}
	d.vtbl.SetTitle.call(uintptr(unsafe.Pointer(d)), uintptr(unsafe.Pointer(utf16Ptr(title))))
}

func (d *iFileOpenDialog) show(owner uintptr) uintptr {
	return d.vtbl.Show.call(uintptr(unsafe.Pointer(d)), owner)
}

func (d *iFileOpenDialog) getResult() (*iShellItem, uintptr) {
	var item *iShellItem
	hr := d.vtbl.GetResult.call(
		uintptr(unsafe.Pointer(d)),
		uintptr(unsafe.Pointer(&item)),
	)
	return item, hr
}

type iShellItemVtbl struct {
	QueryInterface comProc
	AddRef         comProc
	Release        comProc
	BindToHandler  comProc
	GetParent      comProc
	GetDisplayName comProc
	GetAttributes  comProc
	Compare        comProc
}

type iShellItem struct {
	vtbl *iShellItemVtbl
}

func (s *iShellItem) release() {
	if s != nil && s.vtbl != nil {
		s.vtbl.Release.call(uintptr(unsafe.Pointer(s)))
	}
}

func (s *iShellItem) displayName() (string, uintptr) {
	var p *uint16
	hr := s.vtbl.GetDisplayName.call(
		uintptr(unsafe.Pointer(s)),
		uintptr(sigdnFileSysPath),
		uintptr(unsafe.Pointer(&p)),
	)
	if int32(hr) < 0 || p == nil {
		return "", hr
	}
	defer windows.CoTaskMemFree(unsafe.Pointer(p))
	return windows.UTF16PtrToString(p), hr
}

// PickFolder shows the Windows folder picker. It returns "" with no error when
// the user cancels. owner may be 0 for an unowned dialog.
func PickFolder(owner uintptr, title string) (string, error) {
	type result struct {
		path string
		err  error
	}
	done := make(chan result, 1)
	go func() {
		// Dedicated STA thread: never re-enter WebView2's apartment.
		runtime.LockOSThread()
		defer runtime.UnlockOSThread()
		dialogLog("picker: thread started")
		hr, _, _ := procCoInitializeEx.Call(0, COINITApartmentThreaded)
		switch {
		case hr == 0: // S_OK: this thread owns the apartment reference
			defer procCoUninitialize.Call()
		case int32(hr) < 0:
			dialogLog(fmt.Sprintf("picker: CoInitializeEx HRESULT 0x%08X", uint32(hr)))
		}
		p, err := pickFolderOnThread(owner, title)
		done <- result{path: p, err: err}
	}()
	r := <-done
	return r.path, r.err
}

func pickFolderOnThread(owner uintptr, title string) (path string, err error) {
	defer func() {
		if rec := recover(); rec != nil {
			err = fmt.Errorf("文件夹选择器异常: %v", rec)
		}
	}()

	dialogLog("picker: CoCreateInstance")
	var dlg *iFileOpenDialog
	hr, _, _ := procCoCreateInstance.Call(
		uintptr(unsafe.Pointer(&clsidFileOpenDialog)),
		0,
		CLSCTXInprocServer,
		uintptr(unsafe.Pointer(&iidFileOpenDialog)),
		uintptr(unsafe.Pointer(&dlg)),
	)
	dialogLog(fmt.Sprintf("picker: CoCreateInstance hr=0x%08X dlg=%v", uint32(hr), dlg != nil))
	if int32(hr) < 0 || dlg == nil {
		return "", fmt.Errorf("无法创建文件夹选择窗口 (HRESULT 0x%08X)", uint32(hr))
	}
	defer dlg.release()

	opts := dlg.getOptions()
	dialogLog(fmt.Sprintf("picker: options=0x%08X", opts))
	dlg.setOptions(opts | fosPickFolders | fosForceFileSystem | fosPathMustExist | fosNoChangeDir)
	dlg.setTitle(title)

	dialogLog("picker: Show")
	showHR := dlg.show(owner)
	dialogLog(fmt.Sprintf("picker: Show returned 0x%08X", uint32(showHR)))
	if uint32(showHR) == 0x800704C7 { // HRESULT_FROM_WIN32(ERROR_CANCELLED)
		return "", nil
	}
	if int32(showHR) < 0 {
		return "", fmt.Errorf("文件夹选择被中断 (HRESULT 0x%08X)", uint32(showHR))
	}

	item, resHR := dlg.getResult()
	dialogLog(fmt.Sprintf("picker: GetResult hr=0x%08X item=%v", uint32(resHR), item != nil))
	if int32(resHR) < 0 || item == nil {
		return "", fmt.Errorf("无法读取选择的文件夹 (HRESULT 0x%08X)", uint32(resHR))
	}
	defer item.release()

	name, hrName := item.displayName()
	dialogLog(fmt.Sprintf("picker: displayName hr=0x%08X name=%q", uint32(hrName), name))
	if int32(hrName) < 0 {
		return "", fmt.Errorf("无法解析文件夹路径 (HRESULT 0x%08X)", uint32(hrName))
	}
	return name, nil
}
