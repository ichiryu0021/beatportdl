// 集成测试目录 test/ 与源代码物理隔离，按被测包分子目录组织。
// 本子目录测试 internal/sorting 的按曲风分类逻辑。仅依赖纯 Go 包
// (sorting → beatport)，无需 CGO/TagLib，因此仅需 Go 工具链即可运行：
//
//	go test ./test/sorting/
package sorting_test

import (
	"os"
	"path/filepath"
	"testing"

	"unspok3n/beatportdl/internal/beatport"
	"unspok3n/beatportdl/internal/sorting"
)

// makeTrack 构造一个最小可用的 Track，仅填充 genre 相关字段。
func makeTrack(genre, subgenre string) *beatport.Track {
	t := &beatport.Track{Genre: beatport.Genre{Name: genre}}
	if subgenre != "" {
		t.Subgenre = &beatport.Genre{Name: subgenre}
	}
	return t
}

// writeFile 在 dir 下创建一个名为 name、内容为 data 的文件并返回其绝对路径。
func writeFile(t *testing.T, dir, name, data string) string {
	t.Helper()
	p := filepath.Join(dir, name)
	if err := os.WriteFile(p, []byte(data), 0o644); err != nil {
		t.Fatalf("write %s: %v", p, err)
	}
	return p
}

func TestGenreDir_CreatesFolderUnderRoot(t *testing.T) {
	root := t.TempDir()
	a := sorting.NewArchiver(root, sorting.ModeGenre, "")

	dir, err := a.GenreDir(makeTrack("House", ""))
	if err != nil {
		t.Fatalf("GenreDir: %v", err)
	}

	want := filepath.Join(root, "House")
	if dir != want {
		t.Fatalf("dir = %q, want %q", dir, want)
	}
	if info, err := os.Stat(dir); err != nil || !info.IsDir() {
		t.Fatalf("genre dir not created on disk: %v", err)
	}
}

func TestGenreDir_SubgenreMode(t *testing.T) {
	root := t.TempDir()
	a := sorting.NewArchiver(root, sorting.ModeSubgenreOrGenre, "")

	dir, err := a.GenreDir(makeTrack("House", "Deep House"))
	if err != nil {
		t.Fatalf("GenreDir: %v", err)
	}
	if filepath.Base(dir) != "Deep House" {
		t.Fatalf("subgenre mode used %q, want Deep House", filepath.Base(dir))
	}

	// 无 subgenre 时回退到主 genre。
	dir2, _ := a.GenreDir(makeTrack("Techno", ""))
	if filepath.Base(dir2) != "Techno" {
		t.Fatalf("fallback genre = %q, want Techno", filepath.Base(dir2))
	}
}

func TestGenreDir_WhitespaceReplacement(t *testing.T) {
	root := t.TempDir()
	a := sorting.NewArchiver(root, sorting.ModeGenre, "_")

	dir, _ := a.GenreDir(makeTrack("Tech House", ""))
	if filepath.Base(dir) != "Tech_House" {
		t.Fatalf("whitespace replacement = %q, want Tech_House", filepath.Base(dir))
	}
}

func TestFile_MovesIntoGenreDir(t *testing.T) {
	root := t.TempDir()
	srcDir := t.TempDir()
	src := writeFile(t, srcDir, "track.flac", "audio")

	a := sorting.NewArchiver(root, sorting.ModeGenre, "")
	dst, err := a.File(makeTrack("House", ""), src)
	if err != nil {
		t.Fatalf("File: %v", err)
	}

	// 源文件应已消失。
	if _, err := os.Stat(src); !os.IsNotExist(err) {
		t.Fatalf("source still exists after move: %v", err)
	}
	// 目标应在 root/House/ 下且内容一致。
	wantDir := filepath.Join(root, "House")
	if filepath.Dir(dst) != wantDir {
		t.Fatalf("dst dir = %q, want %q", filepath.Dir(dst), wantDir)
	}
	got, err := os.ReadFile(dst)
	if err != nil {
		t.Fatalf("read dst: %v", err)
	}
	if string(got) != "audio" {
		t.Fatalf("content lost: %q", got)
	}
}

func TestFile_CollisionAppendsNumericSuffix(t *testing.T) {
	root := t.TempDir()
	a := sorting.NewArchiver(root, sorting.ModeGenre, "")
	track := makeTrack("House", "")

	first := writeFile(t, t.TempDir(), "x.flac", "v1")
	if _, err := a.File(track, first); err != nil {
		t.Fatalf("File #1: %v", err)
	}

	second := writeFile(t, t.TempDir(), "x.flac", "v2")
	dst2, err := a.File(track, second)
	if err != nil {
		t.Fatalf("File #2: %v", err)
	}
	if filepath.Base(dst2) != "x (1).flac" {
		t.Fatalf("collision suffix = %q, want x (1).flac", filepath.Base(dst2))
	}

	third := writeFile(t, t.TempDir(), "x.flac", "v3")
	dst3, err := a.File(track, third)
	if err != nil {
		t.Fatalf("File #3: %v", err)
	}
	if filepath.Base(dst3) != "x (2).flac" {
		t.Fatalf("collision suffix = %q, want x (2).flac", filepath.Base(dst3))
	}
}

func TestFile_NilTrackReturnsError(t *testing.T) {
	root := t.TempDir()
	a := sorting.NewArchiver(root, sorting.ModeGenre, "")
	src := writeFile(t, t.TempDir(), "x.flac", "x")

	if _, err := a.File(nil, src); err == nil {
		t.Fatal("expected error for nil track, got nil")
	}
}
