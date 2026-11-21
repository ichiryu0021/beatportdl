//go:build windows

package desktop

import (
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"log"
	"sort"
	"strings"
	"sync"
	"time"
)

// The legacy backend scheduler accepts at most one genre and knows nothing about
// sub-genres. The desktop host runs the scheduler itself so the picker can offer
// multiple genres and multiple Beatport sub-genres, then enqueues the discovered
// URLs through the backend's normal download RPC.

type schedulerStartParams struct {
	IntervalSeconds int      `json:"interval_seconds"`
	Genres          []string `json:"genres"`
	SubgenreIDs     []int64  `json:"subgenre_ids"`
}

type schedulerManager struct {
	mu       sync.Mutex
	rpc      *rpcClient
	catalog  *catalogManager
	logger   *log.Logger
	send     func(map[string]any)
	running  bool
	stop     chan struct{}
	done     chan struct{}
	interval time.Duration
	genres   []string
	subIDs   []int64
	lastScan time.Time
	nextScan time.Time
}

func newSchedulerManager(rpc *rpcClient, catalog *catalogManager, logger *log.Logger, send func(map[string]any)) *schedulerManager {
	return &schedulerManager{rpc: rpc, catalog: catalog, logger: logger, send: send}
}

func (m *schedulerManager) Start(p schedulerStartParams) error {
	if p.IntervalSeconds < 60 {
		return errors.New("扫描间隔不能小于 60 秒")
	}
	m.Stop()
	genres := make([]string, 0, len(p.Genres))
	seenGenres := make(map[string]bool)
	for _, genre := range p.Genres {
		genre = strings.TrimSpace(genre)
		if genre == "" || seenGenres[genre] {
			continue
		}
		seenGenres[genre] = true
		genres = append(genres, genre)
	}
	ids := make([]int64, 0, len(p.SubgenreIDs))
	seenIDs := make(map[int64]bool)
	for _, id := range p.SubgenreIDs {
		if id <= 0 || seenIDs[id] {
			continue
		}
		seenIDs[id] = true
		ids = append(ids, id)
	}

	m.mu.Lock()
	m.interval = time.Duration(p.IntervalSeconds) * time.Second
	m.genres = genres
	m.subIDs = ids
	m.running = true
	m.stop = make(chan struct{})
	m.done = make(chan struct{})
	stop, done := m.stop, m.done
	m.mu.Unlock()

	// A scheduler created by an older UI may still be running in the backend.
	ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
	_, _ = m.rpc.Call(ctx, "stopScheduler", nil)
	cancel()

	go m.loop(stop, done)
	return nil
}

func (m *schedulerManager) Stop() {
	m.mu.Lock()
	if !m.running {
		m.mu.Unlock()
		return
	}
	stop, done := m.stop, m.done
	m.running = false
	m.stop = nil
	m.done = nil
	close(stop)
	m.mu.Unlock()
	select {
	case <-done:
	case <-time.After(3 * time.Second):
	}
}

func (m *schedulerManager) Status() map[string]any {
	m.mu.Lock()
	defer m.mu.Unlock()
	out := map[string]any{
		"running":   m.running,
		"genres":    len(m.genres),
		"subgenres": len(m.subIDs),
	}
	if !m.lastScan.IsZero() {
		out["last_scan"] = m.lastScan.Format(time.RFC3339)
	}
	if !m.nextScan.IsZero() {
		out["next_scan"] = m.nextScan.Format(time.RFC3339)
	}
	return out
}

func (m *schedulerManager) loop(stop <-chan struct{}, done chan<- struct{}) {
	defer close(done)
	m.scan()
	m.mu.Lock()
	interval := m.interval
	m.mu.Unlock()
	ticker := time.NewTicker(interval)
	defer ticker.Stop()
	for {
		select {
		case <-stop:
			return
		case <-ticker.C:
			m.scan()
		}
	}
}

func (m *schedulerManager) scan() {
	m.mu.Lock()
	genres := append([]string(nil), m.genres...)
	ids := append([]int64(nil), m.subIDs...)
	interval := m.interval
	m.mu.Unlock()

	ctx, cancel := context.WithTimeout(context.Background(), 180*time.Second)
	defer cancel()

	urls := make(map[string]bool)
	if len(genres) > 0 || len(ids) == 0 {
		params := map[string]any{"kind": "tracks", "page": 1, "per_page": 50}
		if len(genres) > 0 {
			params["genres"] = genres
		}
		raw, err := m.rpc.Call(ctx, "getLatest", params)
		if err != nil {
			m.emitLog("扫描失败：" + err.Error())
			m.markScan(interval)
			return
		}
		var res struct {
			Results []struct {
				URL string `json:"url"`
			} `json:"results"`
		}
		if err := json.Unmarshal(raw, &res); err != nil {
			m.emitLog("解析扫描结果失败：" + err.Error())
			m.markScan(interval)
			return
		}
		for _, item := range res.Results {
			if item.URL != "" {
				urls[item.URL] = true
			}
		}
	}
	if len(ids) > 0 {
		res, err := m.catalog.Latest(subgenreLatestParams{
			IDs: ids, Kind: "tracks", Page: 1, PerPage: 50,
		})
		if err != nil {
			m.emitLog("子曲风扫描失败：" + err.Error())
		} else if results, ok := res["results"].([]map[string]any); ok {
			for _, item := range results {
				if url, ok := item["url"].(string); ok && url != "" {
					urls[url] = true
				}
			}
		}
	}
	if len(urls) == 0 {
		m.emitLog("扫描完成：没有发现新项目")
		m.markScan(interval)
		return
	}
	list := make([]string, 0, len(urls))
	for url := range urls {
		list = append(list, url)
	}
	sort.Strings(list)
	raw, err := m.rpc.Call(ctx, "download", map[string]any{"urls": list})
	if err != nil {
		m.emitLog("加入下载队列失败：" + err.Error())
		m.markScan(interval)
		return
	}
	var dl struct {
		Skipped int `json:"skipped"`
	}
	_ = json.Unmarshal(raw, &dl)
	enqueued := len(list) - dl.Skipped
	if enqueued < 0 {
		enqueued = 0
	}
	message := fmt.Sprintf("发现 %d 个新项目，已加入下载队列", enqueued)
	if dl.Skipped > 0 {
		message += fmt.Sprintf("（跳过 %d 个重复）", dl.Skipped)
	}
	m.emitLog(message)
	m.emitFound(enqueued)
	m.markScan(interval)
}

func (m *schedulerManager) markScan(interval time.Duration) {
	now := time.Now()
	m.mu.Lock()
	m.lastScan = now
	m.nextScan = now.Add(interval)
	m.mu.Unlock()
}

func (m *schedulerManager) emitLog(message string) {
	if m.logger != nil {
		m.logger.Printf("scheduler: %s", message)
	}
	m.send(map[string]any{
		"kind":   "events.batch",
		"events": []map[string]any{{"event": "log", "message": message}},
	})
}

func (m *schedulerManager) emitFound(count int) {
	m.send(map[string]any{
		"kind":   "events.batch",
		"events": []map[string]any{{"event": "scheduler.found", "count": count}},
	})
}
