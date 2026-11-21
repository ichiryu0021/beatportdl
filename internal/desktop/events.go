//go:build windows

package desktop

import (
	"bufio"
	"context"
	"encoding/json"
	"fmt"
	"io"
	"log"
	"net/http"
	"os"
	"strings"
	"sync"
	"time"
)

// openLogger returns a logger that writes to both the GUI log file and stderr
// (stderr is useful when the app is launched from a console during development).
func openLogger(path string) (*log.Logger, io.Closer) {
	if err := os.MkdirAll(dirOf(path), 0o755); err == nil {
		if f, err := os.OpenFile(path, os.O_CREATE|os.O_WRONLY|os.O_APPEND, 0o600); err == nil {
			fmt.Fprintf(f, "\n===== GUI 启动 %s =====\n", time.Now().Format("2006-01-02 15:04:05"))
			return log.New(io.MultiWriter(f), "", log.LstdFlags|log.Lmicroseconds), f
		}
	}
	return log.New(os.Stderr, "", log.LstdFlags|log.Lmicroseconds), io.NopCloser(nil)
}

func dirOf(path string) string {
	if i := strings.LastIndexAny(path, `\/`); i > 0 {
		return path[:i]
	}
	return "."
}

// eventStream subscribes to the backend's Server-Sent Events endpoint and
// reconnects with backoff. Events are forwarded to the page through the
// WebView2 message bridge, never through a socket opened by the page.
type eventStream struct {
	addr    string
	onEvent func(map[string]any)
	onState func(connected bool, message string)

	mu     sync.Mutex
	cancel context.CancelFunc
	wg     sync.WaitGroup
}

func newEventStream(addr string, onEvent func(map[string]any), onState func(bool, string)) *eventStream {
	return &eventStream{addr: addr, onEvent: onEvent, onState: onState}
}

func (e *eventStream) Start() {
	e.mu.Lock()
	defer e.mu.Unlock()
	if e.cancel != nil {
		return
	}
	ctx, cancel := context.WithCancel(context.Background())
	e.cancel = cancel
	e.wg.Add(1)
	go func() {
		defer e.wg.Done()
		e.loop(ctx)
	}()
}

func (e *eventStream) Stop() {
	e.mu.Lock()
	cancel := e.cancel
	e.cancel = nil
	e.mu.Unlock()
	if cancel != nil {
		cancel()
		e.wg.Wait()
	}
}

func (e *eventStream) loop(ctx context.Context) {
	backoff := 500 * time.Millisecond
	for {
		if ctx.Err() != nil {
			return
		}
		err := e.consume(ctx)
		if ctx.Err() != nil {
			return
		}
		if e.onState != nil {
			msg := "与后端的事件通道已断开"
			if err != nil {
				msg = err.Error()
			}
			e.onState(false, msg)
		}
		select {
		case <-ctx.Done():
			return
		case <-time.After(backoff):
		}
		if backoff < 5*time.Second {
			backoff *= 2
			if backoff > 5*time.Second {
				backoff = 5 * time.Second
			}
		}
	}
}

func (e *eventStream) consume(ctx context.Context) error {
	req, err := http.NewRequestWithContext(ctx, http.MethodGet, "http://"+e.addr+"/events", nil)
	if err != nil {
		return err
	}
	req.Header.Set("Accept", "text/event-stream")
	req.Header.Set("Cache-Control", "no-cache")

	client := &http.Client{}
	resp, err := client.Do(req)
	if err != nil {
		return fmt.Errorf("连接事件通道失败: %w", err)
	}
	defer resp.Body.Close()
	if resp.StatusCode != http.StatusOK {
		return fmt.Errorf("事件通道返回 HTTP %d", resp.StatusCode)
	}
	if e.onState != nil {
		e.onState(true, "")
	}

	scanner := bufio.NewScanner(resp.Body)
	scanner.Buffer(make([]byte, 0, 64*1024), 1<<22)
	for scanner.Scan() {
		line := strings.TrimRight(scanner.Text(), "\r")
		if !strings.HasPrefix(line, "data:") {
			continue
		}
		payload := strings.TrimSpace(strings.TrimPrefix(line, "data:"))
		if payload == "" {
			continue
		}
		var ev map[string]any
		if err := json.Unmarshal([]byte(payload), &ev); err != nil {
			continue
		}
		if e.onEvent != nil {
			e.onEvent(ev)
		}
	}
	if err := scanner.Err(); err != nil && ctx.Err() == nil {
		return fmt.Errorf("事件通道中断: %w", err)
	}
	return nil
}
