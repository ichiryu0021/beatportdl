//go:build windows

package desktop

import (
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"log"
	"net/http"
	"net/url"
	"os"
	"path/filepath"
	"sort"
	"strconv"
	"strings"
	"sync"
	"time"
)

// The legacy backend exposes only a flat genre list. Beatport's catalog API
// additionally has first-class sub-genres and supports filtering latest
// tracks/releases by sub_genre_id, so the desktop host reads the cached OAuth
// token and provides a small read-only catalog/filter path. It never returns
// the token to the renderer.

type CatalogSubgenre struct {
	ID        int64  `json:"id"`
	Name      string `json:"name"`
	Slug      string `json:"slug"`
	GenreID   int64  `json:"genreId"`
	GenreName string `json:"genreName"`
	GenreSlug string `json:"genreSlug"`
}

type CatalogGenre struct {
	ID            int64  `json:"id"`
	Name          string `json:"name"`
	Slug          string `json:"slug"`
	Category      string `json:"category"`
	SubgenreCount int    `json:"subgenreCount"`
}

type GenreCatalog struct {
	Genres    []CatalogGenre    `json:"genres"`
	Subgenres []CatalogSubgenre `json:"subgenres"`
	CachedAt  string            `json:"cachedAt"`
}

type subgenreLatestParams struct {
	IDs      []int64 `json:"ids"`
	Kind     string  `json:"kind"`
	Page     int     `json:"page"`
	PerPage  int     `json:"per_page"`
	DateFrom string  `json:"date_from"`
	DateTo   string  `json:"date_to"`
}

type catalogManager struct {
	mu       sync.Mutex
	paths    Paths
	log      *log.Logger
	client   *http.Client
	cached   GenreCatalog
	cachedAt time.Time
}

func newCatalogManager(paths Paths, logger *log.Logger) *catalogManager {
	return &catalogManager{
		paths:  paths,
		log:    logger,
		client: newBeatportHTTPClient(paths),
	}
}

func newBeatportHTTPClient(paths Paths) *http.Client {
	transport := http.DefaultTransport.(*http.Transport).Clone()
	raw := readConfigString(paths.ConfigFile, "proxy")
	normalized, err := normalizeProxy(raw)
	if err == nil && normalized != "" {
		if u, parseErr := url.Parse(normalized); parseErr == nil {
			transport.Proxy = http.ProxyURL(u)
		}
	} else {
		transport.Proxy = http.ProxyFromEnvironment
	}
	return &http.Client{Transport: transport, Timeout: 30 * time.Second}
}

func (m *catalogManager) credentialsPath() string {
	return filepath.Join(filepath.Dir(m.paths.ConfigFile), "beatportdl-credentials.json")
}

func (m *catalogManager) accessToken() (string, error) {
	data, err := os.ReadFile(m.credentialsPath())
	if err != nil {
		return "", errors.New("没有找到 Beatport 登录凭据，请先在设置页登录")
	}
	var creds struct {
		AccessToken string `json:"access_token"`
	}
	if err := json.Unmarshal(data, &creds); err != nil || strings.TrimSpace(creds.AccessToken) == "" {
		return "", errors.New("Beatport 登录凭据无效，请重新登录")
	}
	return creds.AccessToken, nil
}

func (m *catalogManager) Catalog(force bool) (GenreCatalog, error) {
	m.mu.Lock()
	defer m.mu.Unlock()
	if !force && !m.cachedAt.IsZero() && time.Since(m.cachedAt) < time.Hour {
		return m.cached, nil
	}
	token, err := m.accessToken()
	if err != nil {
		return GenreCatalog{}, err
	}
	ctx, cancel := context.WithTimeout(context.Background(), 25*time.Second)
	defer cancel()
	req, err := http.NewRequestWithContext(ctx, http.MethodGet, "https://api.beatport.com/v4/catalog/genres/?per_page=200", nil)
	if err != nil {
		return GenreCatalog{}, err
	}
	req.Header.Set("Authorization", "Bearer "+token)
	req.Header.Set("User-Agent", updateUserAgent)
	req.Header.Set("Accept", "application/json")
	resp, err := m.client.Do(req)
	if err != nil {
		return GenreCatalog{}, fmt.Errorf("读取 Beatport 子曲风失败：%w", err)
	}
	defer resp.Body.Close()
	body, _ := io.ReadAll(io.LimitReader(resp.Body, 4<<20))
	if resp.StatusCode == http.StatusUnauthorized {
		return GenreCatalog{}, errors.New("Beatport 登录已过期，请在设置页重新登录")
	}
	if resp.StatusCode != http.StatusOK {
		return GenreCatalog{}, fmt.Errorf("Beatport 返回 HTTP %d", resp.StatusCode)
	}
	var payload struct {
		Results []struct {
			ID       int64  `json:"id"`
			Name     string `json:"name"`
			Slug     string `json:"slug"`
			Category *struct {
				Name string `json:"name"`
			} `json:"category"`
			SubGenres []struct {
				ID   int64  `json:"id"`
				Name string `json:"name"`
				Slug string `json:"slug"`
			} `json:"sub_genres"`
		} `json:"results"`
	}
	if err := json.Unmarshal(body, &payload); err != nil {
		return GenreCatalog{}, fmt.Errorf("解析子曲风失败：%w", err)
	}
	var catalog GenreCatalog
	seen := make(map[int64]bool)
	for _, genre := range payload.Results {
		item := CatalogGenre{ID: genre.ID, Name: genre.Name, Slug: genre.Slug}
		if genre.Category != nil {
			item.Category = genre.Category.Name
		}
		for _, sub := range genre.SubGenres {
			if seen[sub.ID] {
				continue
			}
			seen[sub.ID] = true
			item.SubgenreCount++
			catalog.Subgenres = append(catalog.Subgenres, CatalogSubgenre{
				ID: sub.ID, Name: sub.Name, Slug: sub.Slug,
				GenreID: genre.ID, GenreName: genre.Name, GenreSlug: genre.Slug,
			})
		}
		catalog.Genres = append(catalog.Genres, item)
	}
	sort.Slice(catalog.Genres, func(i, j int) bool { return catalog.Genres[i].Name < catalog.Genres[j].Name })
	sort.Slice(catalog.Subgenres, func(i, j int) bool {
		if catalog.Subgenres[i].Name == catalog.Subgenres[j].Name {
			return catalog.Subgenres[i].GenreName < catalog.Subgenres[j].GenreName
		}
		return catalog.Subgenres[i].Name < catalog.Subgenres[j].Name
	})
	catalog.CachedAt = time.Now().Format(time.RFC3339)
	m.cached = catalog
	m.cachedAt = time.Now()
	return catalog, nil
}

func (m *catalogManager) Latest(p subgenreLatestParams) (map[string]any, error) {
	if len(p.IDs) == 0 {
		return nil, errors.New("没有选择子曲风")
	}
	if p.Page <= 0 {
		p.Page = 1
	}
	if p.PerPage <= 0 || p.PerPage > 100 {
		p.PerPage = 60
	}
	if p.Kind != "releases" {
		p.Kind = "tracks"
	}
	token, err := m.accessToken()
	if err != nil {
		return nil, err
	}
	type mergedResult struct {
		Results []map[string]any
		Count   int
		HasMore bool
	}
	seen := make(map[string]bool)
	merged := mergedResult{Results: []map[string]any{}}
	for _, id := range p.IDs {
		res, err := m.fetchSubgenre(token, id, p)
		if err != nil {
			return nil, err
		}
		merged.Count += res.Count
		merged.HasMore = merged.HasMore || res.HasMore
		for _, item := range res.Results {
			key, _ := item["url"].(string)
			if key == "" || seen[key] {
				continue
			}
			seen[key] = true
			merged.Results = append(merged.Results, item)
		}
	}
	return map[string]any{
		"results":  merged.Results,
		"page":     p.Page,
		"per_page": p.PerPage,
		"count":    merged.Count,
		"has_more": merged.HasMore,
	}, nil
}

func (m *catalogManager) fetchSubgenre(token string, subgenreID int64, p subgenreLatestParams) (struct {
	Results []map[string]any
	Count   int
	HasMore bool
}, error) {
	out := struct {
		Results []map[string]any
		Count   int
		HasMore bool
	}{Results: []map[string]any{}}
	base := "https://api.beatport.com/v4/catalog/"
	if p.Kind == "releases" {
		base += "releases/"
	} else {
		base += "tracks/"
	}
	q := url.Values{}
	if p.Kind == "releases" {
		q.Set("order_by", "-new_release_date")
	} else {
		q.Set("order_by", "-publish_date")
	}
	q.Set("page", strconv.Itoa(p.Page))
	q.Set("per_page", strconv.Itoa(p.PerPage))
	q.Set("is_available_for_streaming", "true")
	q.Set("sub_genre_id", strconv.FormatInt(subgenreID, 10))
	if p.DateFrom != "" || p.DateTo != "" {
		q.Set("new_release_date", p.DateFrom+":"+p.DateTo)
	}
	ctx, cancel := context.WithTimeout(context.Background(), 25*time.Second)
	defer cancel()
	req, err := http.NewRequestWithContext(ctx, http.MethodGet, base+"?"+q.Encode(), nil)
	if err != nil {
		return out, err
	}
	req.Header.Set("Authorization", "Bearer "+token)
	req.Header.Set("User-Agent", updateUserAgent)
	req.Header.Set("Accept", "application/json")
	resp, err := m.client.Do(req)
	if err != nil {
		return out, fmt.Errorf("读取子曲风曲目失败：%w", err)
	}
	defer resp.Body.Close()
	body, _ := io.ReadAll(io.LimitReader(resp.Body, 12<<20))
	if resp.StatusCode == http.StatusUnauthorized {
		return out, errors.New("Beatport 登录已过期，请在设置页重新登录")
	}
	if resp.StatusCode != http.StatusOK {
		return out, fmt.Errorf("Beatport 返回 HTTP %d", resp.StatusCode)
	}
	var payload struct {
		Count   int              `json:"count"`
		Next    *string          `json:"next"`
		Results []map[string]any `json:"results"`
	}
	if err := json.Unmarshal(body, &payload); err != nil {
		return out, err
	}
	out.Count = payload.Count
	out.HasMore = payload.Next != nil && *payload.Next != ""
	for _, raw := range payload.Results {
		item := normalizeCatalogItem(raw, p.Kind)
		if item != nil {
			out.Results = append(out.Results, item)
		}
	}
	return out, nil
}

func normalizeCatalogItem(raw map[string]any, kind string) map[string]any {
	id := jsonInt64(raw["id"])
	if id == 0 {
		return nil
	}
	slug, _ := raw["slug"].(string)
	name, _ := raw["name"].(string)
	if name == "" {
		return nil
	}
	entity := "track"
	if kind == "releases" {
		entity = "release"
	}
	item := map[string]any{
		"id":   id,
		"name": name,
		"type": entity,
		"url":  fmt.Sprintf("https://www.beatport.com/%s/%s/%d", entity, strings.TrimSpace(slug), id),
	}
	if mix, ok := raw["mix_name"].(string); ok {
		item["mix_name"] = mix
	}
	if artists, ok := raw["artists"]; ok {
		item["artists"] = artists
	}
	if genre, ok := raw["genre"].(map[string]any); ok {
		item["genre"] = map[string]any{"name": genre["name"]}
	}
	if sub, ok := raw["sub_genre"].(map[string]any); ok {
		item["subgenre"] = map[string]any{"id": jsonInt64(sub["id"]), "name": sub["name"]}
	}
	return item
}

func jsonInt64(value any) int64 {
	switch n := value.(type) {
	case float64:
		return int64(n)
	case int64:
		return n
	case json.Number:
		v, _ := n.Int64()
		return v
	case string:
		v, _ := strconv.ParseInt(n, 10, 64)
		return v
	default:
		return 0
	}
}
