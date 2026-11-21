//go:build windows

package desktop

import (
	"os"
	"path/filepath"
	"strings"
	"testing"
)

func TestWriteConfigStringPreservesOtherKeys(t *testing.T) {
	dir := t.TempDir()
	path := filepath.Join(dir, "beatportdl-config.yml")
	original := "username: user\npassword: 'p#ss'\ndownloads_directory: D:/Music\nsort_by_genre: true\nproxy: 'http://old:8080'\ntag_mappings:\n  flac:\n    track_name: TITLE\n"
	if err := os.WriteFile(path, []byte(original), 0o600); err != nil {
		t.Fatal(err)
	}

	if err := writeConfigString(path, "proxy", "socks5://127.0.0.1:1080"); err != nil {
		t.Fatal(err)
	}
	data, _ := os.ReadFile(path)
	text := string(data)
	if !strings.Contains(text, "proxy: 'socks5://127.0.0.1:1080'") {
		t.Fatalf("proxy line not updated:\n%s", text)
	}
	for _, want := range []string{"username: user", "password: 'p#ss'", "sort_by_genre: true", "track_name: TITLE"} {
		if !strings.Contains(text, want) {
			t.Fatalf("config lost %q:\n%s", want, text)
		}
	}
	if got := readConfigString(path, "proxy"); got != "socks5://127.0.0.1:1080" {
		t.Fatalf("readConfigString = %q", got)
	}
}

func TestWriteConfigAppendsMissingKeyAndBool(t *testing.T) {
	dir := t.TempDir()
	path := filepath.Join(dir, "c.yml")
	if err := os.WriteFile(path, []byte("downloads_directory: D:/Music\n"), 0o600); err != nil {
		t.Fatal(err)
	}
	if err := writeConfigBool(path, "sort_by_genre", true); err != nil {
		t.Fatal(err)
	}
	if err := writeConfigString(path, "genre_sort_mode", "subgenre_or_genre"); err != nil {
		t.Fatal(err)
	}
	if !readConfigBool(path, "sort_by_genre") {
		t.Fatal("sort_by_genre not readable")
	}
	if got := readConfigString(path, "genre_sort_mode"); got != "subgenre_or_genre" {
		t.Fatalf("genre_sort_mode = %q", got)
	}
	// A second write must replace the value instead of duplicating the key.
	if err := writeConfigBool(path, "sort_by_genre", false); err != nil {
		t.Fatal(err)
	}
	data, _ := os.ReadFile(path)
	if strings.Count(string(data), "sort_by_genre:") != 1 {
		t.Fatalf("duplicate keys:\n%s", data)
	}
	if readConfigBool(path, "sort_by_genre") {
		t.Fatal("sort_by_genre should be false")
	}
}

func TestNormalizeProxy(t *testing.T) {
	cases := []struct {
		in      string
		want    string
		wantErr bool
	}{
		{"", "", false},
		{"127.0.0.1:7890", "http://127.0.0.1:7890", false},
		{"http://127.0.0.1:7890", "http://127.0.0.1:7890", false},
		{"https://user:pass@proxy.local:8443", "https://user:pass@proxy.local:8443", false},
		{"socks5://127.0.0.1:1080", "socks5://127.0.0.1:1080", false},
		{"socks5h://127.0.0.1:1080", "socks5://127.0.0.1:1080", false},
		{"ftp://127.0.0.1:21", "", true},
		{"http://127.0.0.1:7890/path", "", true},
	}
	for _, tc := range cases {
		got, err := normalizeProxy(tc.in)
		if tc.wantErr {
			if err == nil {
				t.Errorf("normalizeProxy(%q) = %q, want error", tc.in, got)
			}
			continue
		}
		if err != nil || got != tc.want {
			t.Errorf("normalizeProxy(%q) = %q, %v; want %q", tc.in, got, err, tc.want)
		}
	}
}
