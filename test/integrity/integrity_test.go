//go:build cgo

// 本子目录测试 internal/integrity 的下载完整性校验。integrity 包传递依赖
// internal/taglib（CGO），因此本文件带 build tag `cgo`：仅在有 CGO 与
// TagLib C 库的环境下编译运行，其余环境自动跳过，不阻塞 `go test ./test/...`。
//
// 运行：CGO_ENABLED=1 go test ./test/integrity/
package integrity_test

import (
	"testing"

	"unspok3n/beatportdl/internal/integrity"
)

func TestVerify_MissingFile_ReturnsErrorWithoutPanic(t *testing.T) {
	c := integrity.NewChecker()
	res := c.Verify("/no/such/file/missing.flac", -1)

	if res.Valid {
		t.Fatal("missing file should not be valid")
	}
	if res.Error == "" {
		t.Fatal("expected non-empty error for missing file")
	}
	if res.FilePath != "/no/such/file/missing.flac" {
		t.Fatalf("FilePath = %q", res.FilePath)
	}
}

func TestVerifyAudio_MissingFile(t *testing.T) {
	c := integrity.NewChecker()
	res := c.VerifyAudio("/no/such/file/missing.m4a")
	if res.Valid {
		t.Fatal("missing file should not be valid")
	}
}

// TestVerifyDownload_NilChecker_SkipsVerification 验证：当调用方未提供
// Checker (c==nil) 时，VerifyDownload 仅执行下载并报告 valid=true，不触发
// 任何 TagLib 调用。
func TestVerifyDownload_NilChecker_SkipsVerification(t *testing.T) {
	called := false
	dl := func(url, dest, prefix string) error {
		called = true
		return nil
	}
	res, err := integrity.VerifyDownload(dl, "http://x/y.flac", "/tmp/y.flac", "", -1, nil)
	if err != nil {
		t.Fatalf("unexpected error: %v", err)
	}
	if !called {
		t.Fatal("download func was not invoked")
	}
	if !res.Valid {
		t.Fatal("nil checker should report valid=true (skip verification)")
	}
}

type assertErr string

func (e assertErr) Error() string { return string(e) }

// TestVerifyDownload_DownloadError_Propagates 验证：下载失败时错误透传，
// 且结果标记为无效，FilePath 指向传入的 destination。
func TestVerifyDownload_DownloadError_Propagates(t *testing.T) {
	dl := func(url, dest, prefix string) error { return assertErr("download failed") }
	res, err := integrity.VerifyDownload(dl, "u", "d", "p", -1, integrity.NewChecker())
	if err == nil {
		t.Fatal("expected error to propagate")
	}
	if res.Valid {
		t.Fatal("failed download should not be valid")
	}
	if res.FilePath != "d" {
		t.Fatalf("FilePath = %q, want d", res.FilePath)
	}
}
