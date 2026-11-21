WebView2Loader.dll
==================

Source : Microsoft.Web.WebView2 NuGet package, version 1.0.4370.26
         runtimes/win-x64/native/WebView2Loader.dll
License: see LICENSE.txt and NOTICE.txt in this directory.

The loader is redistributed together with the application so the desktop shell
can create a WebView2 environment on machines that only have the Evergreen
Microsoft Edge WebView2 Runtime installed (the default on Windows 10/11).
No Edge/Chromium binaries are bundled; the runtime is provided by Windows.

The COM interop layer under internal/webview2/edge is derived from
github.com/jchv/go-webview2 (MIT, see internal/webview2/LICENSE-go-webview2.txt)
and has been adapted for the BeatportDL desktop shell (virtual host mapping,
message bridge, DPI handling, navigation policy).
