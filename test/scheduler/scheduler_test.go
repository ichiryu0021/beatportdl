// 本子目录测试 internal/scheduler 的周期扫描调度逻辑。仅依赖纯 Go 包
// (scheduler → beatport)，无需 CGO/TagLib。
//
//	go test ./test/scheduler/
package scheduler_test

import (
	"sync"
	"testing"
	"time"

	"unspok3n/beatportdl/internal/beatport"
	"unspok3n/beatportdl/internal/scheduler"
)

func TestNewScheduler_Defaults(t *testing.T) {
	s := scheduler.NewScheduler(nil, time.Hour)
	if s == nil {
		t.Fatal("nil scheduler")
	}
	// 构造不应 panic，Stop 也不应 panic。
	s.Stop()
}

func TestOptions_Applied(t *testing.T) {
	var logs []string
	var mu sync.Mutex
	s := scheduler.NewScheduler(
		nil,
		time.Hour,
		scheduler.WithGenre("House"),
		scheduler.WithPerPage(10),
		scheduler.WithDateRange("2020-01-01", "2020-12-31"),
		scheduler.WithLogger(func(msg string) {
			mu.Lock()
			defer mu.Unlock()
			logs = append(logs, msg)
		}),
		scheduler.WithResultHandler(func([]beatport.Track) error { return nil }),
	)
	if s == nil {
		t.Fatal("nil scheduler with options")
	}
}

// TestStartStop_WithNilBpRecovers 验证：即使 bp 为 nil 导致首次 scan 出错，
// scan 内部的 defer recover 会捕获 panic，scheduler goroutine 不会崩溃，
// 且 Stop 幂等（调用两次不 panic）。
func TestStartStop_WithNilBpRecovers(t *testing.T) {
	var (
		mu   sync.Mutex
		logs []string
	)
	s := scheduler.NewScheduler(
		nil, // nil Beatport client → scan 调用 GetLatestTracks 时 nil deref
		50*time.Millisecond,
		scheduler.WithLogger(func(msg string) {
			mu.Lock()
			defer mu.Unlock()
			logs = append(logs, msg)
		}),
	)
	s.Start()

	// 给 scan 一次执行时间。
	time.Sleep(120 * time.Millisecond)

	s.Stop()
	s.Stop() // 幂等：第二次 Stop 不得 panic

	mu.Lock()
	defer mu.Unlock()
	if len(logs) == 0 {
		// 至少应有 "starting scan" 或 recover 日志；一条都没有说明 logger 未生效或 scan 未执行。
		t.Fatal("expected at least one log line from scheduler, got none")
	}
}
