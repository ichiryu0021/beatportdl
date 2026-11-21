//go:build windows

package desktop

import (
	"encoding/json"
	"testing"
)

func TestNormalizeCatalogTrack(t *testing.T) {
	raw := map[string]any{
		"id":       float64(31063654),
		"slug":     "come-out-with-me",
		"name":     "Come Out With Me",
		"mix_name": "VIP Remix",
		"artists":  []any{map[string]any{"name": "Kinetic1"}},
		"genre":    map[string]any{"name": "House"},
		"sub_genre": map[string]any{
			"id": float64(143), "name": "Acid",
		},
	}
	item := normalizeCatalogItem(raw, "tracks")
	if item == nil {
		t.Fatal("normalizeCatalogItem returned nil")
	}
	if item["url"] != "https://www.beatport.com/track/come-out-with-me/31063654" {
		t.Fatalf("url = %v", item["url"])
	}
	if item["type"] != "track" || item["mix_name"] != "VIP Remix" {
		t.Fatalf("item = %+v", item)
	}
	sub := item["subgenre"].(map[string]any)
	if sub["name"] != "Acid" || sub["id"] != int64(143) {
		t.Fatalf("subgenre = %+v", sub)
	}
}

func TestNormalizeCatalogRelease(t *testing.T) {
	raw := map[string]any{
		"id":   json.Number("5226729"),
		"slug": "techno-techno",
		"name": "Techno Techno",
	}
	item := normalizeCatalogItem(raw, "releases")
	if item == nil {
		t.Fatal("normalizeCatalogItem returned nil")
	}
	if item["url"] != "https://www.beatport.com/release/techno-techno/5226729" {
		t.Fatalf("url = %v", item["url"])
	}
	if item["type"] != "release" {
		t.Fatalf("type = %v", item["type"])
	}
}

func TestJSONInt64(t *testing.T) {
	cases := []struct {
		in   any
		want int64
	}{
		{float64(12), 12},
		{json.Number("34"), 34},
		{"56", 56},
		{int64(78), 78},
		{nil, 0},
	}
	for _, tc := range cases {
		if got := jsonInt64(tc.in); got != tc.want {
			t.Errorf("jsonInt64(%v) = %d, want %d", tc.in, got, tc.want)
		}
	}
}
