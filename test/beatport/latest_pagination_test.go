// 无限滚动分页链路测试：用 httptest 模拟 Beatport API，验证
// GetLatestTracks/GetLatestReleases 把 page/per_page/genre/日期透传到查询串，
// 且 Paginated.Next（server 层 has_more 的来源）能正确解码。
// 不依赖外网与 CGO。
package beatport_test

import (
	"encoding/hex"
	"encoding/json"
	"fmt"
	"hash/fnv"
	"net/http"
	"net/http/httptest"
	"net/url"
	"os"
	"path/filepath"
	"testing"
	"time"

	"unspok3n/beatportdl/internal/beatport"
)

// authedClient 构造一个带着"未过期 token 缓存"的 Beatport 客户端，
// 指向 base 测试服务器（绕过真实登录与外网）。
func authedClient(t *testing.T, base string) *beatport.Beatport {
	t.Helper()
	const user, pass = "tester", "secret"
	auth := beatport.NewAuth(user, pass, filepath.Join(t.TempDir(), "token.json"))

	// login_id 算法与 auth.go loginId() 一致：fnv64a("user:pass") 的 hex。
	h := fnv.New64a()
	h.Write([]byte(user + ":" + pass))
	loginID := hex.EncodeToString(h.Sum(nil))

	token := map[string]interface{}{
		"access_token":  "fake-access-token",
		"refresh_token": "fake-refresh-token",
		"expires_in":    3600,
		"token_type":    "Bearer",
		"scope":         "",
		"login_id":      loginID,
		"issued_at":     time.Now().Unix(),
	}
	data, err := json.Marshal(token)
	if err != nil {
		t.Fatalf("marshal token: %v", err)
	}
	if err := os.WriteFile(auth.CacheFile(), data, 0600); err != nil {
		t.Fatalf("write token cache: %v", err)
	}
	if err := auth.LoadCache(); err != nil {
		t.Fatalf("LoadCache: %v", err)
	}

	b := beatport.New("", auth)
	b.SetBaseURL(base)
	return b
}

// trackJSON 构造最小可用的曲目响应对象。
func trackJSON(id int64) map[string]interface{} {
	return map[string]interface{}{
		"id":   id,
		"url":  fmt.Sprintf("https://www.beatport.com/track/t%d", id),
		"name": fmt.Sprintf("Track %d", id),
	}
}

func TestGetLatest_PaginationAndFilters(t *testing.T) {
	var seenQueries []string
	srv := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		if r.URL.Path != "/catalog/tracks/" {
			t.Errorf("unexpected path %q", r.URL.Path)
		}
		seenQueries = append(seenQueries, r.URL.RawQuery)
		q := r.URL.Query()

		resp := map[string]interface{}{
			"count":    51,
			"page":     q.Get("page"),
			"per_page": 50,
			"results":  []map[string]interface{}{},
		}
		switch q.Get("page") {
		case "1":
			next := "https://api.beatport.com/v4/catalog/tracks/?page=2&per_page=50"
			resp["next"] = &next
			resp["results"] = []map[string]interface{}{trackJSON(1), trackJSON(2)}
		case "2":
			resp["next"] = nil // 末页
			resp["results"] = []map[string]interface{}{trackJSON(51)}
		}
		w.Header().Set("Content-Type", "application/json")
		_ = json.NewEncoder(w).Encode(resp)
	}))
	defer srv.Close()

	b := authedClient(t, srv.URL)

	// 第 1 页：带曲风 + 日期过滤，应有 next。
	p1, err := b.GetLatestTracks("Tech House", 1, 50, "2026-01-01", "2026-02-01")
	if err != nil {
		t.Fatalf("GetLatestTracks page1: %v", err)
	}
	if p1.Next == nil || *p1.Next == "" {
		t.Fatalf("page1 Next = nil, want non-empty next URL")
	}
	if p1.Count != 51 || len(p1.Results) != 2 {
		t.Fatalf("page1 payload mismatch: count=%d results=%d", p1.Count, len(p1.Results))
	}
	if got := p1.Results[0].URL; got != "https://www.beatport.com/track/t1" {
		t.Fatalf("first track URL = %q", got)
	}

	// 第 2 页：next 为 nil（驱动 GUI 显示"已加载全部"）。
	p2, err := b.GetLatestTracks("Tech House", 2, 50, "2026-01-01", "2026-02-01")
	if err != nil {
		t.Fatalf("GetLatestTracks page2: %v", err)
	}
	if p2.Next != nil {
		t.Fatalf("page2 Next = %v, want nil", *p2.Next)
	}
	if len(p2.Results) != 1 || p2.Results[0].ID != 51 {
		t.Fatalf("page2 results mismatch: %+v", p2.Results)
	}

	if len(seenQueries) != 2 {
		t.Fatalf("server saw %d requests, want 2", len(seenQueries))
	}
	for i, q := range seenQueries {
		v, err := url.ParseQuery(q)
		if err != nil {
			t.Fatalf("parse query %q: %v", q, err)
		}
		wantPage := fmt.Sprintf("%d", i+1)
		if v.Get("page") != wantPage || v.Get("per_page") != "50" {
			t.Errorf("page/per_page = %s/%s, want %s/50 (raw=%s)",
				v.Get("page"), v.Get("per_page"), wantPage, q)
		}
		if v.Get("genre_name") != "Tech House" {
			t.Errorf("genre_name = %q, want Tech House (raw=%s)", v.Get("genre_name"), q)
		}
		if v.Get("new_release_date") != "2026-01-01:2026-02-01" {
			t.Errorf("new_release_date = %q (raw=%s)", v.Get("new_release_date"), q)
		}
		if v.Get("is_available_for_streaming") != "true" {
			t.Errorf("streaming flag missing (raw=%s)", q)
		}
		if v.Get("order_by") != "-publish_date" {
			t.Errorf("order_by = %q, want -publish_date", v.Get("order_by"))
		}
	}
}

func TestGetLatestReleases_Pagination(t *testing.T) {
	srv := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		if r.URL.Path != "/catalog/releases/" {
			t.Errorf("unexpected path %q", r.URL.Path)
		}
		next := ""
		if r.URL.Query().Get("page") == "1" {
			next = "https://api.beatport.com/v4/catalog/releases/?page=2"
		}
		resp := map[string]interface{}{
			"count":  1,
			"next":   next, // 空字符串解到 *string → 非 nil 但为空；末页用省略表达
			"results": []map[string]interface{}{
				{"id": 900, "url": "https://www.beatport.com/release/r900", "name": "R900"},
			},
		}
		if r.URL.Query().Get("page") == "2" {
			resp["next"] = nil
		}
		w.Header().Set("Content-Type", "application/json")
		_ = json.NewEncoder(w).Encode(resp)
	}))
	defer srv.Close()

	b := authedClient(t, srv.URL)
	p1, err := b.GetLatestReleases("", 1, 50, "", "")
	if err != nil {
		t.Fatalf("GetLatestReleases page1: %v", err)
	}
	if p1.Next == nil || *p1.Next == "" {
		t.Fatalf("releases page1 Next = %v, want non-empty", p1.Next)
	}
	if p1.Results[0].ID != 900 {
		t.Fatalf("release id = %d, want 900", p1.Results[0].ID)
	}
	p2, err := b.GetLatestReleases("", 2, 50, "", "")
	if err != nil {
		t.Fatalf("GetLatestReleases page2: %v", err)
	}
	if p2.Next != nil {
		t.Fatalf("releases page2 Next = %v, want nil", *p2.Next)
	}
}
