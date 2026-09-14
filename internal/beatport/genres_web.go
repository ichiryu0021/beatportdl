package beatport

import (
	"fmt"
	"io"
	"net/http"
	"regexp"
	"sort"
	"strings"
)

// 官网导航里曲风链接的两种常见形态：
//
//	<a href="/genre/deep-house/7766">Deep House</a>
//	<a href="/genre/deep-house/7766" title="Deep House">...</a>
//
// 用宽松正则容错，页面结构小改也能命中；完全匹配不到时由调用方回退 API。
var (
	genreLinkRe  = regexp.MustCompile(`href="/genre/([a-z0-9-]+)/(\d+)"[^>]*>([^<]{2,60})<`)
	genreTitleRe = regexp.MustCompile(`href="/genre/([a-z0-9-]+)/\d+"[^>]*title="([^"]{2,60})"`)
)

// ScrapeGenres 从 Beatport 官网首页抓取曲风导航，返回 {name, slug} 列表。
// 走普通网页请求（不带认证），因此登录前也能用。页面结构变化导致一个
// 曲风链接都找不到时返回错误，由调用方回退到 catalog API。
func (b *Beatport) ScrapeGenres() ([]Genre, error) {
	req, err := http.NewRequest(http.MethodGet, "https://www.beatport.com/", nil)
	if err != nil {
		return nil, fmt.Errorf("build genre request: %w", err)
	}
	req.Header.Set("user-agent", b.headers["user-agent"])
	req.Header.Set("accept", b.headers["accept"])
	req.Header.Set("accept-language", b.headers["accept-language"])

	resp, err := b.client.Do(req)
	if err != nil {
		return nil, fmt.Errorf("fetch genre page: %w", err)
	}
	defer resp.Body.Close()
	if resp.StatusCode != http.StatusOK {
		return nil, fmt.Errorf("genre page status: %s", resp.Status)
	}

	body, err := io.ReadAll(io.LimitReader(resp.Body, 8<<20))
	if err != nil {
		return nil, fmt.Errorf("read genre page: %w", err)
	}
	return parseGenresHTML(string(body))
}

func parseGenresHTML(html string) ([]Genre, error) {
	bySlug := make(map[string]Genre)

	for _, m := range genreLinkRe.FindAllStringSubmatch(html, -1) {
		slug := m[1]
		name := strings.TrimSpace(m[3])
		if name == "" {
			continue
		}
		if _, ok := bySlug[slug]; !ok {
			bySlug[slug] = Genre{Name: name, Slug: slug}
		}
	}
	for _, m := range genreTitleRe.FindAllStringSubmatch(html, -1) {
		slug := m[1]
		name := strings.TrimSpace(m[2])
		if name == "" {
			continue
		}
		if _, ok := bySlug[slug]; !ok {
			bySlug[slug] = Genre{Name: name, Slug: slug}
		}
	}

	if len(bySlug) == 0 {
		return nil, fmt.Errorf("no genre links found on beatport homepage")
	}

	out := make([]Genre, 0, len(bySlug))
	for _, g := range bySlug {
		out = append(out, g)
	}
	sort.Slice(out, func(i, j int) bool { return out[i].Name < out[j].Name })
	return out, nil
}

// ParseGenresHTMLForTest exposes the HTML parser for offline unit tests.
// Production code uses the unexported parseGenresHTML directly.
func ParseGenresHTMLForTest(html string) ([]Genre, error) {
	return parseGenresHTML(html)
}
