package main

import (
	"context"
	"errors"
	"fmt"
	"github.com/google/uuid"
	"os"
	"path/filepath"
	"resenje.org/singleflight"
	"strconv"
	"strings"
	"sync"
	"unspok3n/beatportdl/config"
	"unspok3n/beatportdl/internal/beatport"
	"unspok3n/beatportdl/internal/taglib"
)

func (app *application) errorLogWrapper(url, step string, err error) {
	app.LogError(fmt.Sprintf("[%s] %s", url, step), err)
}

// publishUrlError 记录错误日志并把 download.error 事件推给 GUI（目录创建
// 失败、Access denied 等都会在这里被前端看到）。
func (app *application) publishUrlError(url, step string, err error) {
	app.errorLogWrapper(url, step, err)
	app.publishDownload("download.error", url, "",
		map[string]interface{}{"error": step + ": " + err.Error()})
}

func (app *application) infoLogWrapper(url, message string) {
	app.LogInfo(fmt.Sprintf("[%s] %s", url, message))
}

// publishDownload streams a download.* event to the GUI (no-op in CLI mode).
// url 用 track 的商店页 URL 作键，与 getLatest/search 返回的 url 一致，
// 前端下载列表按它定位行。
func (app *application) publishDownload(event, url, title string, extra map[string]interface{}) {
	payload := map[string]interface{}{"event": event, "url": url, "title": title}
	for k, v := range extra {
		payload[k] = v
	}
	app.PublishProgress(payload)
}

func (app *application) createDirectory(baseDir string, subDir ...string) (string, error) {
	fullPath := filepath.Join(baseDir, filepath.Join(subDir...))
	err := CreateDirectory(fullPath)
	return fullPath, err
}

type DownloadsDirectoryEntity interface {
	DirectoryName(n beatport.NamingPreferences) string
}

func (app *application) setupDownloadsDirectory(baseDir string, entity DownloadsDirectoryEntity) (string, error) {
	if app.config.SortByContext {
		var subDir string
		switch castedEntity := entity.(type) {
		case *beatport.Release:
			subDir = castedEntity.DirectoryName(
				beatport.NamingPreferences{
					Template:           app.config.ReleaseDirectoryTemplate,
					Whitespace:         app.config.WhitespaceCharacter,
					ArtistsLimit:       app.config.ArtistsLimit,
					ArtistsShortForm:   app.config.ArtistsShortForm,
					TrackNumberPadding: app.config.TrackNumberPadding,
				},
			)
			if app.config.SortByLabel && entity != nil {
				baseDir = filepath.Join(baseDir, castedEntity.Label.Name)
			}
		case *beatport.Playlist:
			subDir = castedEntity.DirectoryName(
				beatport.NamingPreferences{
					Template:           app.config.PlaylistDirectoryTemplate,
					Whitespace:         app.config.WhitespaceCharacter,
					TrackNumberPadding: app.config.TrackNumberPadding,
				},
			)
		case *beatport.Chart:
			subDir = castedEntity.DirectoryName(
				beatport.NamingPreferences{
					Template:           app.config.ChartDirectoryTemplate,
					Whitespace:         app.config.WhitespaceCharacter,
					TrackNumberPadding: app.config.TrackNumberPadding,
				},
			)
		case *beatport.Label:
			subDir = castedEntity.DirectoryName(
				beatport.NamingPreferences{
					Template:   app.config.LabelDirectoryTemplate,
					Whitespace: app.config.WhitespaceCharacter,
				},
			)
		case *beatport.Artist:
			subDir = castedEntity.DirectoryName(
				beatport.NamingPreferences{
					Template:   app.config.ArtistDirectoryTemplate,
					Whitespace: app.config.WhitespaceCharacter,
				},
			)
		}
		baseDir = filepath.Join(baseDir, subDir)
	}
	return app.createDirectory(baseDir)
}

func (app *application) requireCover(respectFixTags, respectKeepCover bool) bool {
	fixTags := respectFixTags && app.config.FixTags &&
		(app.config.CoverSize != config.DefaultCoverSize || app.config.Quality != "lossless")
	keepCover := respectKeepCover && app.config.SortByContext && app.config.KeepCover
	return fixTags || keepCover
}

func (app *application) downloadCover(image beatport.Image, downloadsDir string) (string, error) {
	coverUrl := image.FormattedUrl(app.config.CoverSize)
	coverPath := filepath.Join(downloadsDir, uuid.New().String())
	err := app.downloadFile(coverUrl, coverPath, "")
	if err != nil {
		os.Remove(coverPath)
		return "", err
	}
	return coverPath, nil
}

func (app *application) handleCoverFile(path string) error {
	if path == "" {
		return nil
	}
	if app.config.KeepCover && app.config.SortByContext {
		newPath := filepath.Dir(path) + "/cover.jpg"
		if err := os.Rename(path, newPath); err != nil {
			return err
		}
	} else {
		os.Remove(path)
	}
	return nil
}

var (
	ErrTrackFileExists = errors.New("file already exists")
)

func (app *application) saveTrack(track *beatport.Track, directory string, quality string, eventURL string) (string, error) {
	if eventURL == "" {
		eventURL = track.StoreUrl()
	}
	var fileExtension string
	var displayQuality string

	var stream *beatport.TrackStream
	var download *beatport.TrackDownload

	switch app.config.Quality {
	case "medium-hls":
		trackStream, err := app.bp.StreamTrack(track.ID)
		if err != nil {
			return "", err
		}
		fileExtension = ".m4a"
		displayQuality = "AAC 128kbps - HLS"
		stream = trackStream
	default:
		trackDownload, err := app.bp.DownloadTrack(track.ID, quality)
		if err != nil {
			return "", err
		}
		switch trackDownload.StreamQuality {
		case ".128k.aac.mp4":
			fileExtension = ".m4a"
			displayQuality = "AAC 128kbps"
		case ".256k.aac.mp4":
			fileExtension = ".m4a"
			displayQuality = "AAC 256kbps"
		case ".flac":
			fileExtension = ".flac"
			displayQuality = "FLAC"
		default:
			return "", fmt.Errorf("invalid stream quality: %s", trackDownload.StreamQuality)
		}
		download = trackDownload
	}

	fileName := track.Filename(
		beatport.NamingPreferences{
			Template:           app.config.TrackFileTemplate,
			Whitespace:         app.config.WhitespaceCharacter,
			ArtistsLimit:       app.config.ArtistsLimit,
			ArtistsShortForm:   app.config.ArtistsShortForm,
			TrackNumberPadding: app.config.TrackNumberPadding,
			KeySystem:          app.config.KeySystem,
		},
	)
	filePath := fmt.Sprintf("%s/%s%s", directory, fileName, fileExtension)
	if _, err := os.Stat(filePath); err == nil {
		app.activeFilesMutex.RLock()
		_, exists := app.activeFiles[filePath]
		app.activeFilesMutex.RUnlock()

		if exists {
			i := 1
			for {
				filePath = fmt.Sprintf("%s/%s (%d)%s", directory, fileName, i, fileExtension)
				if _, err := os.Stat(filePath); os.IsNotExist(err) {
					break
				}
				i++
			}
		} else {
			switch app.config.TrackExists {
			case "skip":
				return "", nil
			case "update":
				app.infoLogWrapper(track.StoreUrl(), "updating tags")
				return filePath, nil
			case "error":
				return "", ErrTrackFileExists
			}
		}
	}

	app.activeFilesMutex.Lock()
	app.activeFiles[filePath] = struct{}{}
	app.activeFilesMutex.Unlock()

	var prefix string
	infoDisplay := fmt.Sprintf("%s (%s) [%s]", track.Name.String(), track.MixName.String(), displayQuality)
	if app.config.ShowProgress {
		prefix = infoDisplay
	} else {
		fmt.Println("Downloading " + infoDisplay)
	}

	if download != nil {
		title := infoDisplay
		if err := app.downloadFileProgress(download.Location, filePath, prefix, func(received, total int64) {
			pct := 0
			if total > 0 {
				pct = int(received * 100 / total)
			}
			app.publishDownload("download.progress", eventURL, title,
				map[string]interface{}{"percent": pct, "received": received, "total": total})
		}); err != nil {
			os.Remove(filePath)
			return "", err
		}
	} else if stream != nil {
		segments, key, err := getStreamSegments(stream.Url)
		if err != nil {
			return "", fmt.Errorf("get stream segments: %v", err)
		}
		segmentsFile, err := app.downloadSegments(directory, *segments, *key, prefix)
		defer os.Remove(segmentsFile)
		if err != nil {
			return "", fmt.Errorf("download segments: %v", err)
		}
		if err := remuxToM4A(segmentsFile, filePath); err != nil {
			os.Remove(filePath)
			return "", fmt.Errorf("remux to m4a: %v", err)
		}
	}

	if !app.config.ShowProgress {
		fmt.Printf("Finished downloading %s\n", infoDisplay)
	}

	// Integrity verification: when enabled, confirm the downloaded file is
	// decodable as audio with a positive sample rate (decode check is the
	// strongest completeness guarantee and works for both direct downloads
	// and HLS-remuxed m4a where Content-Length is unknown). On failure the
	// partial file is removed and the error is propagated.
	if app.checker != nil && app.config.VerifyIntegrity {
		result := app.checker.VerifyAudio(filePath)
		if !result.Valid {
			app.errorLogWrapper(track.StoreUrl(), "integrity check", fmt.Errorf("%s", result.Error))
			os.Remove(filePath)
			return "", fmt.Errorf("integrity check failed: %s", result.Error)
		}
		app.infoLogWrapper(track.StoreUrl(), fmt.Sprintf("verified: %d bytes, %d Hz", result.ActualBytes, result.SampleRate))
	}

	return filePath, nil
}

const (
	rawTagSuffix = "_raw"
)

func (app *application) tagTrack(location string, track *beatport.Track, coverPath string) error {
	fileExt := filepath.Ext(location)
	if !app.config.FixTags {
		return nil
	}
	file, err := taglib.Read(location)
	if err != nil {
		return err
	}
	defer file.Close()

	subgenre := ""
	if track.Subgenre != nil {
		subgenre = track.Subgenre.Name
	}
	mappingValues := map[string]string{
		"track_id":       strconv.Itoa(int(track.ID)),
		"track_url":      track.StoreUrl(),
		"track_name":     fmt.Sprintf("%s (%s)", track.Name.String(), track.MixName.String()),
		"track_artists":  track.Artists.Display(0, ""),
		"track_remixers": track.Remixers.Display(0, ""),
		"track_artists_limited": track.Artists.Display(
			app.config.ArtistsLimit,
			app.config.ArtistsShortForm,
		),
		"track_remixers_limited": track.Remixers.Display(
			app.config.ArtistsLimit,
			app.config.ArtistsShortForm,
		),
		"track_number":              strconv.Itoa(track.Number),
		"track_number_with_padding": beatport.NumberWithPadding(track.Number, track.Release.TrackCount, app.config.TrackNumberPadding),
		"track_number_with_total":   fmt.Sprintf("%d/%d", track.Number, track.Release.TrackCount),
		"track_genre":               track.Genre.Name,
		"track_subgenre":            subgenre,
		"track_genre_with_subgenre": track.GenreWithSubgenre("|"),
		"track_subgenre_or_genre":   track.SubgenreOrGenre(),
		"track_key":                 track.Key.Display(app.config.KeySystem),
		"track_bpm":                 strconv.Itoa(track.BPM),
		"track_isrc":                track.ISRC,

		"release_id":   strconv.Itoa(int(track.Release.ID)),
		"release_url":  track.Release.StoreUrl(),
		"release_name": track.Release.Name.String(),
		"release_artists": track.Release.Artists.Display(
			0,
			"",
		),
		"release_remixers": track.Release.Remixers.Display(
			0,
			"",
		),
		"release_artists_limited": track.Release.Artists.Display(
			app.config.ArtistsLimit,
			app.config.ArtistsShortForm,
		),
		"release_remixers_limited": track.Release.Remixers.Display(
			app.config.ArtistsLimit,
			app.config.ArtistsShortForm,
		),
		"release_date":        track.Release.Date,
		"release_year":        track.Release.Year(),
		"release_track_count": strconv.Itoa(track.Release.TrackCount),
		"release_track_count_with_padding": beatport.NumberWithPadding(
			track.Release.TrackCount, track.Release.TrackCount, app.config.TrackNumberPadding,
		),
		"release_catalog_number": track.Release.CatalogNumber.String(),
		"release_upc":            track.Release.UPC,
		"release_label":          track.Release.Label.Name,
		"release_label_url":      track.Release.Label.StoreUrl(),
	}

	if fileExt == ".m4a" {
		if err = file.StripMp4(); err != nil {
			return err
		}
	} else {
		existingTags, err := file.PropertyKeys()
		if err != nil {
			return fmt.Errorf("read existing tags: %v", err)
		}

		for _, tag := range existingTags {
			file.SetProperty(tag, nil)
		}
	}

	if fileExt == ".flac" {
		for field, property := range app.config.TagMappings["flac"] {
			value := mappingValues[field]
			if value != "" {
				file.SetProperty(property, &value)
			}
		}
	} else if fileExt == ".m4a" {
		rawTags := make(map[string]string)

		for field, property := range app.config.TagMappings["m4a"] {
			if strings.HasSuffix(property, rawTagSuffix) {
				if mappingValues[field] != "" {
					property = strings.TrimSuffix(property, rawTagSuffix)
					rawTags[property] = mappingValues[field]
				}
			} else {
				value := mappingValues[field]
				if value != "" {
					file.SetProperty(property, &value)
				}
			}
		}

		for tag, value := range rawTags {
			file.SetItemMp4(tag, value)
		}
	}

	if coverPath != "" && (app.config.CoverSize != config.DefaultCoverSize || fileExt == ".m4a") {
		data, err := os.ReadFile(coverPath)
		if err != nil {
			return err
		}
		picture := taglib.Picture{
			MimeType:    "image/jpeg",
			PictureType: "Front",
			Description: "Cover",
			Data:        data,
			Size:        uint(len(data)),
		}
		if err := file.SetPicture(&picture); err != nil {
			return err
		}
	}

	if err = file.Save(); err != nil {
		return err
	}

	return nil
}

// handleTrack 处理单曲下载全流程。sourceURL 是用户提交下载时使用的 URL
// （可能是 api.beatport.com 形式，与 track.StoreUrl() 的 www 形式不同）；
// SSE 事件统一用它作键，保证前端预登记的下载行能正确收到状态更新。
// 专辑/歌单子曲目没有对应的提交 URL，传空串则回退为 StoreUrl()。
func (app *application) handleTrack(track *beatport.Track, downloadsDir string, coverPath string, sourceURL string) error {
	eventURL := sourceURL
	if eventURL == "" {
		eventURL = track.StoreUrl()
	}
	title := fmt.Sprintf("%s (%s)", track.Name.String(), track.MixName.String())
	app.publishDownload("download.started", eventURL, title, nil)

	location, err := app.saveTrack(track, downloadsDir, app.config.Quality, eventURL)
	if err != nil {
		app.publishDownload("download.error", eventURL, title,
			map[string]interface{}{"error": err.Error()})
		return fmt.Errorf("save track: %v", err)
	}
	// saveTrack returns ("", nil) only when TrackExists=="skip" and the file
	// already exists on disk. Tell the GUI it was skipped and stop here: no
	// tagging, genre filing or download.done with an empty path. This covers
	// every caller (single track, release, playlist, chart, label, artist),
	// which all funnel tracks through handleTrack.
	if location == "" {
		app.publishDownload("download.skipped", eventURL, title,
			map[string]interface{}{"reason": "exists"})
		return nil
	}
	if err = app.tagTrack(location, track, coverPath); err != nil {
		app.publishDownload("download.error", eventURL, title,
			map[string]interface{}{"error": err.Error()})
		return fmt.Errorf("tag track: %v", err)
	}
	// Genre-based filing: when the archiver is configured (sort_by_genre), move
	// the finished file into <downloads>/<genre>/. The track carries its own
	// genre so this works even though releases have no genre field.
	if app.archiver != nil {
		moved, err := app.archiver.File(track, location)
		if err != nil {
			app.errorLogWrapper(eventURL, "genre filing", err)
		} else if moved != location {
			app.infoLogWrapper(eventURL, "filed into genre: "+filepath.Base(filepath.Dir(moved)))
		}
	}
	app.publishDownload("download.done", eventURL, title,
		map[string]interface{}{"path": location})
	return nil
}

func (app *application) cleanup(downloadsDir string) {
	if downloadsDir != app.config.DownloadsDirectory {
		os.Remove(downloadsDir)
	}
}

func ForPaginated[T any](
	entityId int64,
	params string,
	fetchPage func(id int64, page int, params string) (results *beatport.Paginated[T], err error),
	processItem func(item T, i int) error,
) error {
	page := 1
	for {
		paginated, err := fetchPage(entityId, page, params)
		if err != nil {
			return fmt.Errorf("fetch page: %w", err)
		}

		for i, item := range paginated.Results {
			if err := processItem(item, i); err != nil {
				return fmt.Errorf("process item: %w", err)
			}
		}

		if paginated.Next == nil {
			break
		}
		page++
	}
	return nil
}

func (app *application) handleUrl(url string) {
	link, err := app.bp.ParseUrl(url)
	if err != nil {
		app.errorLogWrapper(url, "parse url", err)
		return
	}

	switch link.Type {
	case beatport.TrackLink:
		app.handleTrackLink(link)
	case beatport.ReleaseLink:
		app.handleReleaseLink(link)
	case beatport.PlaylistLink:
		app.handlePlaylistLink(link)
	case beatport.ChartLink:
		app.handleChartLink(link)
	case beatport.LabelLink:
		app.handleLabelLink(link)
	case beatport.ArtistLink:
		app.handleArtistLink(link)
	default:
		app.LogError("handle URL", ErrUnsupportedLinkType)
	}
}

func (app *application) handleTrackLink(link *beatport.Link) {
	track, err := app.bp.GetTrack(link.ID)
	if err != nil {
		app.errorLogWrapper(link.Original, "fetch track", err)
		return
	}

	release, err := app.bp.GetRelease(track.Release.ID)
	if err != nil {
		app.errorLogWrapper(link.Original, "fetch track release", err)
		return
	}
	track.Release = *release

	downloadsDir, err := app.setupDownloadsDirectory(app.config.DownloadsDirectory, release)
	if err != nil {
	app.publishUrlError(link.Original, "setup downloads directory", err)
		return
	}

	wg := sync.WaitGroup{}
	app.downloadWorker(&wg, func() {
		var cover string
		if app.requireCover(true, true) {
			cover, err = app.downloadCover(track.Release.Image, downloadsDir)
			if err != nil {
				app.errorLogWrapper(link.Original, "download track release cover", err)
			}
		}

		if err := app.handleTrack(track, downloadsDir, cover, link.Original); err != nil {
			app.errorLogWrapper(link.Original, "handle track", err)
			os.Remove(cover)
			return
		}

		if err := app.handleCoverFile(cover); err != nil {
			app.errorLogWrapper(link.Original, "handle cover file", err)
			return
		}
	})
	wg.Wait()

	app.cleanup(downloadsDir)
}

func (app *application) handleReleaseLink(link *beatport.Link) {
	release, err := app.bp.GetRelease(link.ID)
	if err != nil {
		app.errorLogWrapper(link.Original, "fetch release", err)
		return
	}

	downloadsDir, err := app.setupDownloadsDirectory(app.config.DownloadsDirectory, release)
	if err != nil {
	app.publishUrlError(link.Original, "setup downloads directory", err)
		return
	}

	var cover string
	if app.requireCover(true, true) {
		app.semAcquire(app.downloadSem)
		cover, err = app.downloadCover(release.Image, downloadsDir)
		if err != nil {
			app.errorLogWrapper(link.Original, "download release cover", err)
		}
		app.semRelease(app.downloadSem)
	}

	wg := sync.WaitGroup{}
	for _, trackUrl := range release.TrackUrls {
		app.downloadWorker(&wg, func() {
			trackLink, err := app.bp.ParseUrl(trackUrl)
			if err != nil {
				app.errorLogWrapper(link.Original, "parse track url", err)
				return
			}

			track, err := app.bp.GetTrack(trackLink.ID)
			if err != nil {
				app.errorLogWrapper(trackUrl, "fetch release track", err)
				return
			}
			trackStoreUrl := track.StoreUrl()
			track.Release = *release

			if err := app.handleTrack(track, downloadsDir, cover, ""); err != nil {
				app.errorLogWrapper(trackStoreUrl, "handle track", err)
				return
			}
		})
	}
	wg.Wait()

	if err := app.handleCoverFile(cover); err != nil {
		app.errorLogWrapper(link.Original, "handle cover file", err)
		return
	}

	app.cleanup(downloadsDir)
}

func (app *application) handlePlaylistLink(link *beatport.Link) {
	playlist, err := app.bp.GetPlaylist(link.ID)
	if err != nil {
		app.errorLogWrapper(link.Original, "fetch playlist", err)
		return
	}

	downloadsDir, err := app.setupDownloadsDirectory(app.config.DownloadsDirectory, playlist)
	if err != nil {
	app.publishUrlError(link.Original, "setup downloads directory", err)
		return
	}

	wg := sync.WaitGroup{}
	err = ForPaginated[beatport.PlaylistItem](link.ID, "", app.bp.GetPlaylistItems, func(item beatport.PlaylistItem, i int) error {
		app.downloadWorker(&wg, func() {
			trackStoreUrl := item.Track.StoreUrl()

			release, err := app.bp.GetRelease(item.Track.Release.ID)
			if err != nil {
				app.errorLogWrapper(trackStoreUrl, "fetch track release", err)
				return
			}
			item.Track.Release = *release

			trackDownloadsDir := downloadsDir
			trackFull, err := app.bp.GetTrack(item.Track.ID)
			if err != nil {
				app.errorLogWrapper(trackStoreUrl, "fetch full track", err)
				return
			}
			item.Track.Number = trackFull.Number
			if app.config.SortByContext && app.config.ForceReleaseDirectories {
				trackDownloadsDir, err = app.setupDownloadsDirectory(downloadsDir, release)
				if err != nil {
				app.publishUrlError(trackStoreUrl, "setup track release directory", err)
					return
				}
			}

			var cover string
			if app.requireCover(true, app.config.ForceReleaseDirectories) {
				cover, err = app.downloadCover(item.Track.Release.Image, trackDownloadsDir)
				if err != nil {
					app.errorLogWrapper(trackStoreUrl, "download track release cover", err)
				} else if !app.config.ForceReleaseDirectories {
					defer os.Remove(cover)
				}
			}

			if err := app.handleTrack(&item.Track, trackDownloadsDir, cover, ""); err != nil {
				app.errorLogWrapper(trackStoreUrl, "handle track", err)
				os.Remove(cover)
				app.cleanup(trackDownloadsDir)
				return
			}

			if app.config.ForceReleaseDirectories {
				if err := app.handleCoverFile(cover); err != nil {
					app.errorLogWrapper(trackStoreUrl, "handle track release cover file", err)
					return
				}
			}

			app.cleanup(trackDownloadsDir)
		})
		return nil
	})

	if err != nil {
		app.errorLogWrapper(link.Original, "handle playlist items", err)
		return
	}

	wg.Wait()
}

func (app *application) handleChartLink(link *beatport.Link) {
	chart, err := app.bp.GetChart(link.ID)
	if err != nil {
		app.errorLogWrapper(link.Original, "fetch chart", err)
		return
	}

	downloadsDir, err := app.setupDownloadsDirectory(app.config.DownloadsDirectory, chart)
	if err != nil {
	app.publishUrlError(link.Original, "setup downloads directory", err)
		return
	}
	wg := sync.WaitGroup{}

	if app.requireCover(false, true) {
		app.downloadWorker(&wg, func() {
			cover, err := app.downloadCover(chart.Image, downloadsDir)
			if err != nil {
				app.errorLogWrapper(link.Original, "download chart cover", err)
			}
			if err := app.handleCoverFile(cover); err != nil {
				app.errorLogWrapper(link.Original, "handle cover file", err)
				return
			}
		})
	}

	err = ForPaginated[beatport.Track](link.ID, "", app.bp.GetChartTracks, func(track beatport.Track, i int) error {
		app.downloadWorker(&wg, func() {
			trackStoreUrl := track.StoreUrl()

			release, err := app.bp.GetRelease(track.Release.ID)
			if err != nil {
				app.errorLogWrapper(trackStoreUrl, "fetch track release", err)
				return
			}
			track.Release = *release

			trackDownloadsDir := downloadsDir
			trackFull, err := app.bp.GetTrack(track.ID)
			if err != nil {
				app.errorLogWrapper(trackStoreUrl, "fetch full track", err)
				return
			}
			track.Number = trackFull.Number
			if app.config.SortByContext && app.config.ForceReleaseDirectories {
				trackDownloadsDir, err = app.setupDownloadsDirectory(downloadsDir, release)
				if err != nil {
				app.publishUrlError(trackStoreUrl, "setup track release directory", err)
					return
				}
			}

			var cover string
			if app.requireCover(true, app.config.ForceReleaseDirectories) {
				cover, err = app.downloadCover(track.Release.Image, trackDownloadsDir)
				if err != nil {
					app.errorLogWrapper(trackStoreUrl, "download track release cover", err)
				} else if !app.config.ForceReleaseDirectories {
					defer os.Remove(cover)
				}
			}

			if err := app.handleTrack(&track, trackDownloadsDir, cover, ""); err != nil {
				app.errorLogWrapper(trackStoreUrl, "handle track", err)
				os.Remove(cover)
				app.cleanup(trackDownloadsDir)
				return
			}

			if app.config.ForceReleaseDirectories {
				if err := app.handleCoverFile(cover); err != nil {
					app.errorLogWrapper(trackStoreUrl, "handle track release cover file", err)
					return
				}
			}

			app.cleanup(trackDownloadsDir)
		})
		return nil
	})

	if err != nil {
		app.errorLogWrapper(link.Original, "handle playlist items", err)
		return
	}

	wg.Wait()
}

func (app *application) handleLabelLink(link *beatport.Link) {
	label, err := app.bp.GetLabel(link.ID)
	if err != nil {
		app.errorLogWrapper(link.Original, "fetch label", err)
		return
	}

	downloadsDir, err := app.setupDownloadsDirectory(app.config.DownloadsDirectory, label)
	if err != nil {
	app.publishUrlError(link.Original, "setup downloads directory", err)
		return
	}

	wg := sync.WaitGroup{}
	var relGroup singleflight.Group[int64, *beatport.Release]

	err = ForPaginated[beatport.Track](link.ID, link.Params, app.bp.GetLabelTracks, func(t beatport.Track, i int) error {
		app.downloadWorker(&wg, func() {
			trackStoreUrl := t.StoreUrl()

			track, err := app.bp.GetTrack(t.ID)
			if err != nil {
				app.errorLogWrapper(trackStoreUrl, "fetch track", err)
				return
			}

			release, _, err := relGroup.Do(context.Background(), track.Release.ID, func(ctx context.Context) (*beatport.Release, error) {
				release, err := app.bp.GetRelease(track.Release.ID)
				if err != nil {
					return nil, err
				}
				return release, nil
			})

			if err != nil {
				app.errorLogWrapper(trackStoreUrl, "fetch track release", err)
				return
			}

			track.Release = *release

			releaseDir, err := app.setupDownloadsDirectory(downloadsDir, release)
			if err != nil {
			app.publishUrlError(trackStoreUrl, "setup track release downloads directory", err)
				return
			}

			var cover string
			cover, err = app.downloadCover(release.Image, releaseDir)
			if err != nil {
				app.errorLogWrapper(trackStoreUrl, "download track release cover", err)
			}

			if err := app.handleTrack(track, releaseDir, cover, ""); err != nil {
				app.errorLogWrapper(trackStoreUrl, "handle track", err)
				app.cleanup(releaseDir)
				return
			}

			if err := app.handleCoverFile(cover); err != nil {
				app.errorLogWrapper(trackStoreUrl, "handle cover file", err)
				return
			}

			app.cleanup(releaseDir)
		})
		return nil
	})

	if err != nil {
		app.errorLogWrapper(link.Original, "handle label releases", err)
		return
	}

	wg.Wait()
}

func (app *application) handleArtistLink(link *beatport.Link) {
	artist, err := app.bp.GetArtist(link.ID)
	if err != nil {
		app.errorLogWrapper(link.Original, "fetch artist", err)
		return
	}

	downloadsDir, err := app.setupDownloadsDirectory(app.config.DownloadsDirectory, artist)
	if err != nil {
	app.publishUrlError(link.Original, "setup downloads directory", err)
		return
	}

	wg := sync.WaitGroup{}
	var relGroup singleflight.Group[int64, *beatport.Release]

	err = ForPaginated[beatport.Track](link.ID, link.Params, app.bp.GetArtistTracks, func(t beatport.Track, i int) error {
		app.downloadWorker(&wg, func() {
			trackStoreUrl := t.StoreUrl()
			track, err := app.bp.GetTrack(t.ID)
			if err != nil {
				app.errorLogWrapper(trackStoreUrl, "fetch full track", err)
				return
			}

			release, _, err := relGroup.Do(context.Background(), track.Release.ID, func(ctx context.Context) (*beatport.Release, error) {
				release, err := app.bp.GetRelease(track.Release.ID)
				if err != nil {
					return nil, err
				}
				return release, nil
			})

			if err != nil {
				app.errorLogWrapper(trackStoreUrl, "fetch track release", err)
				return
			}

			track.Release = *release

			releaseDir, err := app.setupDownloadsDirectory(downloadsDir, release)
			if err != nil {
			app.publishUrlError(trackStoreUrl, "setup track release downloads directory", err)
				return
			}

			var cover string
			if app.requireCover(true, true) {
				cover, err = app.downloadCover(release.Image, releaseDir)
				if err != nil {
					app.errorLogWrapper(trackStoreUrl, "download track release cover", err)
				}
			}

			if err := app.handleTrack(track, releaseDir, cover, ""); err != nil {
				app.errorLogWrapper(trackStoreUrl, "handle track", err)
				os.Remove(cover)
				app.cleanup(releaseDir)
				return
			}

			if err := app.handleCoverFile(cover); err != nil {
				app.errorLogWrapper(trackStoreUrl, "handle cover file", err)
				return
			}

			app.cleanup(releaseDir)
		})
		return nil
	})
	if err != nil {
		app.errorLogWrapper(link.Original, "handle artist tracks", err)
		return
	}

	wg.Wait()
}
