// Package sorting organizes downloaded tracks into genre-based subdirectories
// beneath the configured downloads root. Because Beatport's Release structure
// carries no genre information while each Track does, classification operates
// at the track level: every track is filed under a folder named after its own
// (sub)genre, keeping the original release directory nested inside.
package sorting

import (
	"fmt"
	"io"
	"os"
	"path/filepath"

	"unspok3n/beatportdl/internal/beatport"
)

// Mode selects which genre value is used as the folder name.
type Mode int

const (
	// ModeGenre uses track.Genre.Name (the primary genre).
	ModeGenre Mode = iota
	// ModeSubgenreOrGenre uses the subgenre when present, otherwise the genre.
	ModeSubgenreOrGenre
)

// Archiver moves completed track files into <root>/<genre>/... subdirectories.
// It does not download anything itself; it only reorganizes files that already
// exist on disk.
type Archiver struct {
	rootDir string
	mode    Mode
	// Whitespace mirrors beatport.NamingPreferences.Whitespace: when non-empty,
	// spaces in the genre folder name are replaced with it.
	whitespace string
}

// NewArchiver creates an Archiver that files tracks under rootDir using the
// given mode. whitespace mirrors the config's whitespace_character and may be "".
func NewArchiver(rootDir string, mode Mode, whitespace string) *Archiver {
	return &Archiver{rootDir: rootDir, mode: mode, whitespace: whitespace}
}

// genreFolder returns the sanitized folder name for the given track's genre.
func (a *Archiver) genreFolder(t *beatport.Track) string {
	var name string
	switch a.mode {
	case ModeSubgenreOrGenre:
		name = t.SubgenreOrGenre()
	default:
		name = t.Genre.Name
	}
	return beatport.SanitizePath(beatport.SanitizeForPath(name), a.whitespace)
}

// GenreDir returns the absolute path of the genre folder for a track, without
// moving anything. Useful for the downloader to write directly into the right
// place.
func (a *Archiver) GenreDir(t *beatport.Track) (string, error) {
	return a.ensureDir(a.genreFolder(t))
}

// ensureDir creates <root>/<sub> if missing and returns its absolute path.
func (a *Archiver) ensureDir(sub string) (string, error) {
	dir := filepath.Join(a.rootDir, sub)
	if err := os.MkdirAll(dir, 0o760); err != nil {
		return "", fmt.Errorf("create genre directory: %w", err)
	}
	return dir, nil
}

// File moves srcPath into the genre folder derived from track. The destination
// keeps the same base filename as srcPath. If the destination already exists,
// a numeric " (n)" suffix is appended before the extension (mirroring the
// duplicate-name behavior in cmd/beatportdl/downloader.go saveTrack).
//
// On success the original srcPath is removed (it has been moved). On error the
// source file is left untouched.
func (a *Archiver) File(track *beatport.Track, srcPath string) (string, error) {
	if track == nil {
		return "", fmt.Errorf("nil track")
	}
	dir, err := a.GenreDir(track)
	if err != nil {
		return "", err
	}

	base := filepath.Base(srcPath)
	dst := filepath.Join(dir, base)

	// Disambiguate collisions: "name.ext" -> "name (1).ext", "name (2).ext", ...
	if _, err := os.Stat(dst); err == nil {
		ext := filepath.Ext(base)
		stem := base[:len(base)-len(ext)]
		for i := 1; ; i++ {
			candidate := filepath.Join(dir, fmt.Sprintf("%s (%d)%s", stem, i, ext))
			if _, err := os.Stat(candidate); os.IsNotExist(err) {
				dst = candidate
				break
			}
		}
	}

	if err := os.Rename(srcPath, dst); err != nil {
		// Rename across volumes can fail; fall back to copy+delete.
		if copyErr := copyFile(srcPath, dst); copyErr != nil {
			return "", fmt.Errorf("move file: rename failed (%v) and copy fallback failed (%v)", err, copyErr)
		}
		if rmErr := os.Remove(srcPath); rmErr != nil {
			return dst, fmt.Errorf("move file: copied but failed to remove source: %w", rmErr)
		}
	}
	return dst, nil
}

// copyFile is a fallback for cross-volume renames.
func copyFile(src, dst string) error {
	in, err := os.Open(src)
	if err != nil {
		return err
	}
	defer in.Close()

	out, err := os.Create(dst)
	if err != nil {
		return err
	}
	defer out.Close()

	if _, err := io.Copy(out, in); err != nil {
		return err
	}
	return out.Sync()
}
