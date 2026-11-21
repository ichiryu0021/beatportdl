//go:build windows

package desktop

import (
	"log"
	"os"
	"strings"

	"unspok3n/beatportdl/internal/winui"
)

// Chromium memory/perf tuning. WebView2 inherits flags from
// WEBVIEW2_ADDITIONAL_BROWSER_ARGUMENTS at environment creation time. On
// machines with little RAM we fall back to software rendering plus Chromium's
// low-end memory mode; otherwise the GPU process is kept for the smoothest
// scrolling and animation.
const commonChromiumFlags = "--no-first-run --disable-background-networking " +
	"--disable-component-update --disable-sync --disable-extensions " +
	"--disable-breakpad --js-flags=--max-old-space-size=128"

const gpuChromiumFlags = commonChromiumFlags + " --disk-cache-size=33554432"

const lowMemoryChromiumFlags = commonChromiumFlags +
	" --disable-gpu --enable-low-end-device-mode --disk-cache-size=8388608"

// tuneWebViewMemory configures browser flags and reports the active mode:
// "gpu", "low-memory" or "custom" (an existing environment variable wins).
func tuneWebViewMemory(forceLow bool, logger *log.Logger) string {
	if existing := strings.TrimSpace(os.Getenv("WEBVIEW2_ADDITIONAL_BROWSER_ARGUMENTS")); existing != "" {
		logger.Printf("WebView2 使用已有的浏览器参数：%s", existing)
		return "custom"
	}
	total := winui.TotalPhysicalMemory()
	low := forceLow || (total > 0 && total < 9<<30)
	flags := gpuChromiumFlags
	mode := "gpu"
	if low {
		flags = lowMemoryChromiumFlags
		mode = "low-memory"
	}
	_ = os.Setenv("WEBVIEW2_ADDITIONAL_BROWSER_ARGUMENTS", flags)
	logger.Printf("WebView2 性能模式=%s（物理内存 %.1f GB）\n  flags: %s", mode, float64(total)/(1<<30), flags)
	return mode
}
