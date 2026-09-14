package beatport_test

import (
	"strings"
	"testing"

	"unspok3n/beatportdl/internal/beatport"
)

// TestParseGenresHTML verifies the homepage genre-link parser handles the
// anchor formats seen on beatport.com, dedupes by slug and returns sorted,
// non-empty results. Pure string parsing — no network.
func TestParseGenresHTML(t *testing.T) {
	const html = `
<html><body>
<a href="/genre/deep-house/7766">Deep House</a>
<a href="/genre/techno/11894" title="Techno">Techno</a>
<a href="/genre/techno/11894" title="Techno (dup)">Techno</a>
<a href="/genre/drum-and-bass/8017">Drum &amp; Bass</a>
<span>ignore me</span>
</body></html>
`
	genres, err := beatport.ParseGenresHTMLForTest(html)
	if err != nil {
		t.Fatalf("parse: %v", err)
	}
	if len(genres) != 3 {
		t.Fatalf("want 3 genres, got %d: %+v", len(genres), genres)
	}

	bySlug := map[string]string{}
	for _, g := range genres {
		bySlug[g.Slug] = g.Name
	}
	if bySlug["deep-house"] != "Deep House" {
		t.Errorf("deep-house name = %q", bySlug["deep-house"])
	}
	if bySlug["techno"] != "Techno" {
		t.Errorf("techno name = %q (should dedupe keeping first)", bySlug["techno"])
	}
	if !strings.Contains(bySlug["drum-and-bass"], "Drum") {
		t.Errorf("drum-and-bass name = %q", bySlug["drum-and-bass"])
	}

	// sorted by name
	for i := 1; i < len(genres); i++ {
		if genres[i-1].Name > genres[i].Name {
			t.Fatalf("not sorted: %+v", genres)
		}
	}
}

// TestParseGenresHTML_NoLinks reports an error when nothing matches, so the
// server falls back to the catalog API.
func TestParseGenresHTML_NoLinks(t *testing.T) {
	if _, err := beatport.ParseGenresHTMLForTest("<html><body>no links here</body></html>"); err == nil {
		t.Fatal("want error for page without genre links")
	}
}
