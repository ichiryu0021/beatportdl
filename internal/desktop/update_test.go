//go:build windows

package desktop

import (
	"archive/zip"
	"os"
	"path/filepath"
	"strings"
	"testing"
)

func TestCompareVersions(t *testing.T) {
	cases := []struct {
		a, b string
		want int
	}{
		{"1.0.0", "1.0.0", 0},
		{"v1.0.1", "1.0.0", 1},
		{"1.0.0", "v1.1.0", -1},
		{"1.0.10", "1.0.9", 1},
		{"1.0", "1.0.0", 0},
		{"1.0.0-beta", "1.0.0", 0},
		{"2.0.0", "1.9.9", 1},
	}
	for _, tc := range cases {
		got := compareVersions(tc.a, tc.b)
		if (got > 0) != (tc.want > 0) || (got < 0) != (tc.want < 0) || (got == 0) != (tc.want == 0) {
			t.Errorf("compareVersions(%q,%q)=%d, want sign %d", tc.a, tc.b, got, tc.want)
		}
	}
}

func TestSelectUpdateAssets(t *testing.T) {
	assets := []ghAsset{
		{Name: "BPDownloader-1.0.0-windows-x64.exe", BrowserDownloadURL: "https://github.com/x/y/releases/download/v1.0.0/app.exe"},
		{Name: "bpdl-hotfix-v1.1.0.zip", BrowserDownloadURL: "https://github.com/x/y/releases/download/v1.1.0/hot.zip"},
		{Name: "bpdl-v1.1.0-full.zip", BrowserDownloadURL: "https://github.com/x/y/releases/download/v1.1.0/full.zip"},
	}
	hotfix, full := selectUpdateAssets(assets)
	if hotfix.Name != "bpdl-hotfix-v1.1.0.zip" {
		t.Fatalf("hotfix = %q", hotfix.Name)
	}
	if full.Name != "bpdl-v1.1.0-full.zip" {
		t.Fatalf("full = %q", full.Name)
	}

	_, generic := selectUpdateAssets([]ghAsset{{Name: "bpdl-v1.1.0.zip", BrowserDownloadURL: "https://github.com/x"}})
	if generic.Name != "bpdl-v1.1.0.zip" {
		t.Fatalf("generic zip not treated as full: %+v", generic)
	}
	if hotfix, full := selectUpdateAssets([]ghAsset{{Name: "notes.zip", BrowserDownloadURL: "https://github.com/x"}}); hotfix.Name != "" || full.Name != "" {
		t.Fatalf("unrelated zip selected: %+v %+v", hotfix, full)
	}
}

func TestRuntimeFileFilter(t *testing.T) {
	for _, name := range []string{"beatportdl_gui.exe", "beatportdl.exe", "WebView2Loader.dll", "libtag-2.dll", "启动.bat"} {
		if !isRuntimeFileName(name) {
			t.Errorf("%q should be a runtime file", name)
		}
	}
	for _, name := range []string{"beatportdl-config.yml", "beatportdl-credentials.json", "data", "logs", "settings.json"} {
		if isRuntimeFileName(name) {
			t.Errorf("%q must never be copied by the updater", name)
		}
	}
}

func TestStageUpdateFiles(t *testing.T) {
	root := t.TempDir()
	writeTestExe(t, filepath.Join(root, "beatportdl_gui.exe"), 1<<20+128)
	writeTestFile(t, filepath.Join(root, "WebView2Loader.dll"), []byte("dll"))
	writeTestFile(t, filepath.Join(root, "beatportdl-config.yml"), []byte("username: secret\n"))
	writeTestFile(t, filepath.Join(root, "update.json"), []byte(`{"app":"beatportdl","version":"1.1.0","files":["beatportdl_gui.exe"]}`))
	stage := filepath.Join(t.TempDir(), "stage")
	manifest, files, err := stageUpdateFiles(root, stage, "1.1.0")
	if err != nil {
		t.Fatal(err)
	}
	if manifest.Version != "1.1.0" {
		t.Fatalf("manifest version = %q", manifest.Version)
	}
	if len(files) != 2 {
		t.Fatalf("staged files = %v", files)
	}
	for _, name := range []string{"beatportdl_gui.exe", "WebView2Loader.dll"} {
		if _, err := os.Stat(filepath.Join(stage, name)); err != nil {
			t.Errorf("missing staged %s: %v", name, err)
		}
	}
	if _, err := os.Stat(filepath.Join(stage, "beatportdl-config.yml")); !os.IsNotExist(err) {
		t.Fatalf("config was copied into update stage")
	}
	if _, err := os.Stat(filepath.Join(stage, "update.json")); !os.IsNotExist(err) {
		t.Fatalf("update.json should not be installed")
	}
}

func TestStageRejectsVersionMismatch(t *testing.T) {
	root := t.TempDir()
	writeTestExe(t, filepath.Join(root, "beatportdl_gui.exe"), 1<<20+128)
	writeTestFile(t, filepath.Join(root, "update.json"), []byte(`{"version":"1.0.0"}`))
	if _, _, err := stageUpdateFiles(root, filepath.Join(t.TempDir(), "stage"), "1.1.0"); err == nil {
		t.Fatal("expected version mismatch error")
	}
}

func TestExtractZipRejectsTraversal(t *testing.T) {
	zipPath := filepath.Join(t.TempDir(), "bad.zip")
	f, err := os.Create(zipPath)
	if err != nil {
		t.Fatal(err)
	}
	zw := zip.NewWriter(f)
	w, err := zw.Create("../evil.txt")
	if err != nil {
		t.Fatal(err)
	}
	_, _ = w.Write([]byte("bad"))
	_ = zw.Close()
	_ = f.Close()
	if err := extractUpdateZip(zipPath, t.TempDir()); err == nil {
		t.Fatal("expected path traversal to be rejected")
	}
}

func TestExtractZipNormal(t *testing.T) {
	zipPath := filepath.Join(t.TempDir(), "ok.zip")
	f, err := os.Create(zipPath)
	if err != nil {
		t.Fatal(err)
	}
	zw := zip.NewWriter(f)
	w, _ := zw.Create("BPDL/beatportdl_gui.exe")
	_, _ = w.Write([]byte("MZtest"))
	_ = zw.Close()
	_ = f.Close()
	dest := t.TempDir()
	if err := extractUpdateZip(zipPath, dest); err != nil {
		t.Fatal(err)
	}
	if _, err := os.Stat(filepath.Join(dest, "BPDL", "beatportdl_gui.exe")); err != nil {
		t.Fatal(err)
	}
}

func TestUpdaterInvocation(t *testing.T) {
	if !IsUpdaterInvocation([]string{"--apply-update", "--pid", "1"}) {
		t.Fatal("expected updater invocation")
	}
	if IsUpdaterInvocation([]string{"--debug"}) {
		t.Fatal("normal app invocation misdetected")
	}
}

func TestSanitizeVersion(t *testing.T) {
	if got := sanitizeVersion("v1.1.0 / beta"); strings.ContainsAny(got, `/\ `) || got == "" {
		t.Fatalf("sanitizeVersion = %q", got)
	}
}

func writeTestFile(t *testing.T, path string, data []byte) {
	t.Helper()
	if err := os.WriteFile(path, data, 0o644); err != nil {
		t.Fatal(err)
	}
}

func writeTestExe(t *testing.T, path string, size int) {
	t.Helper()
	data := make([]byte, size)
	copy(data, "MZ")
	if err := os.WriteFile(path, data, 0o644); err != nil {
		t.Fatal(err)
	}
}
