// 本子目录用真实 Beatport API 响应样本（脱敏 JSON fixture，存于
// testdata/）端到端验证两件事：
//   1. Paginated[Track]/Paginated[Genre] 能正确解码真实 API 响应（字段
//      映射、genre/subgenre 嵌套、artists 列表等）；
//   2. internal/beatport/latest.go 的查询参数构造 (buildLatestParams) 与
//      interactions.go 中已验证的 Beatport 过滤参数格式一致
//      (genre_name=, new_release_date=FROM:TO, per_page=)。
//
// 不依赖网络、CGO、TagLib，只需 Go 工具链：
//
//	go test ./test/beatport/
package beatport_test

import (
	"encoding/json"
	"net/url"
	"os"
	"path/filepath"
	"strings"
	"testing"

	"unspok3n/beatportdl/internal/beatport"
)

// loadFixture 把 testdata/<name> 读入并解码到 out。
func loadFixture(t *testing.T, name string, out interface{}) {
	t.Helper()
	path := filepath.Join("testdata", name)
	data, err := os.ReadFile(path)
	if err != nil {
		t.Fatalf("read %s: %v (cwd=%s)", path, err, mustGetwd())
	}
	if err := json.Unmarshal(data, out); err != nil {
		t.Fatalf("decode %s: %v", path, err)
	}
}

func mustGetwd() string {
	wd, _ := os.Getwd()
	return wd
}

// TestDecode_LatestTracks_Fixture 端到端验证：真实 API 的 latest-tracks 响应
// 能被 Paginated[Track] 正确解码，且关键字段（genre、subgenre、artists、
// release.label、publish_date）映射正确。这是 GetLatestTracks 之后解析路径
// 的端到端覆盖。
func TestDecode_LatestTracks_Fixture(t *testing.T) {
	var page beatport.Paginated[beatport.Track]
	loadFixture(t, "latest_tracks.json", &page)

	if page.Count != 1234 {
		t.Fatalf("Count = %d, want 1234", page.Count)
	}
	if len(page.Results) != 2 {
		t.Fatalf("Results len = %d, want 2", len(page.Results))
	}

	first := page.Results[0]
	if first.ID != 1696999 {
		t.Fatalf("ID = %d, want 1696999", first.ID)
	}
	if first.Name.String() != "Strobe" {
		t.Fatalf("Name = %q, want Strobe", first.Name.String())
	}
	if first.Genre.Name != "Progressive House" {
		t.Fatalf("Genre = %q", first.Genre.Name)
	}
	if first.Subgenre == nil || first.Subgenre.Name != "Melodic Progressive" {
		t.Fatalf("Subgenre mismatch: %+v", first.Subgenre)
	}
	if first.SubgenreOrGenre() != "Melodic Progressive" {
		t.Fatalf("SubgenreOrGenre = %q", first.SubgenreOrGenre())
	}
	if first.Release.Label.Name != "mau5trap" {
		t.Fatalf("Release.Label = %q", first.Release.Label.Name)
	}
	if first.Release.Date != "2009-09-22" {
		t.Fatalf("Release.Date = %q, want 2009-09-22", first.Release.Date)
	}
	if first.PublishDate != "2008-09-01" {
		t.Fatalf("PublishDate = %q", first.PublishDate)
	}
	if first.Release.TrackCount != 10 {
		t.Fatalf("TrackCount = %d", first.Release.TrackCount)
	}
	// 第二首的 subgenre 为 null。
	if page.Results[1].Subgenre != nil {
		t.Fatalf("expected nil subgenre for track #2, got %v", page.Results[1].Subgenre)
	}
	if page.Results[1].SubgenreOrGenre() != "Progressive House" {
		t.Fatalf("SubgenreOrGenre fallback = %q", page.Results[1].SubgenreOrGenre())
	}
	// facets 端到端解码（interactions.go 的 entityStats 依赖此结构）。
	if len(page.Facets.Fields["genre"]) != 1 || page.Facets.Fields["genre"][0].Name != "Progressive House" {
		t.Fatalf("genre facet mismatch: %+v", page.Facets.Fields)
	}
}

// TestDecode_Genres_Fixture 验证 GetGenres 的响应解码。
func TestDecode_Genres_Fixture(t *testing.T) {
	var page beatport.Paginated[beatport.Genre]
	loadFixture(t, "genres.json", &page)
	if len(page.Results) != 3 {
		t.Fatalf("Results len = %d, want 3", len(page.Results))
	}
	if page.Results[0].Name != "Progressive House" || page.Results[0].Slug != "progressive-house" {
		t.Fatalf("first genre = %+v", page.Results[0])
	}
}

// TestBuildLatestParams_QueryFormat 端到端验证 latest.go 构造的查询参数
// 与 Beatport 实际接受的格式（interactions.go:270-284 已验证）一致。
// 这是 GetLatestTracks/GetLatestReleases 内部 URL 构造的端到端覆盖。
func TestBuildLatestParams_QueryFormat(t *testing.T) {
	// 全参数：含曲风、日期范围。
	got := beatport.BuildLatestParams("Tech House", 1, 25, "2024-01-01", "2024-12-31")
	// 验证已 URL 编码（空格→+）。
	if !strings.Contains(got, "genre_name=Tech+House") && !strings.Contains(got, "genre_name=Tech%20House") {
		t.Fatalf("genre_name not URL-encoded properly: %q", got)
	}
	if !strings.Contains(got, "page=1") || !strings.Contains(got, "per_page=25") {
		t.Fatalf("pagination missing: %q", got)
	}
	if !strings.Contains(got, "is_available_for_streaming=true") {
		t.Fatalf("streaming filter missing: %q", got)
	}
	// date range 必须是 new_release_date=FROM:TO 形式。
	if !strings.Contains(got, "new_release_date=2024-01-01:2024-12-31") {
		t.Fatalf("date range format wrong: %q", got)
	}

	// 无曲风、无日期：参数中不得出现 genre_name 或 new_release_date。
	got2 := beatport.BuildLatestParams("", 1, 50, "", "")
	if strings.Contains(got2, "genre_name=") {
		t.Fatalf("unexpected genre_name in empty case: %q", got2)
	}
	if strings.Contains(got2, "new_release_date=") {
		t.Fatalf("unexpected new_release_date in empty case: %q", got2)
	}

	// 仅曲风、无日期。
	got3 := beatport.BuildLatestParams("Techno", 2, 100, "", "")
	// 解析成 query 以便断言。
	vals, err := url.ParseQuery(got3)
	if err != nil {
		t.Fatalf("ParseQuery: %v", err)
	}
	if vals.Get("genre_name") != "Techno" {
		t.Fatalf("genre_name = %q, want Techno", vals.Get("genre_name"))
	}
	if vals.Get("page") != "2" || vals.Get("per_page") != "100" {
		t.Fatalf("pagination = page=%s per_page=%s", vals.Get("page"), vals.Get("per_page"))
	}
	// 含特殊字符的曲风必须被 URL 编码。
	got4 := beatport.BuildLatestParams("Drum & Bass", 1, 10, "", "")
	if !strings.Contains(got4, "Drum+%26+Bass") && !strings.Contains(got4, "Drum+%26+Bass") {
		// & 必须编码为 %26。
		if strings.Contains(got4, "Drum & Bass") {
			t.Fatalf("genre with & not encoded: %q", got4)
		}
	}
}
