// Package scheduler provides a periodic scan-and-download scheduler that
// repeatedly polls the Beatport latest-tracks endpoint at a fixed interval and
// hands each batch of discovered tracks to a caller-supplied handler.
package scheduler

import (
	"fmt"
	"sync"
	"time"
	"unspok3n/beatportdl/internal/beatport"
)

type Scheduler struct {
	bp        *beatport.Beatport
	interval  time.Duration
	genreName string
	perPage   int
	dateFrom  string
	dateTo    string
	onResult  func([]beatport.Track) error
	stop      chan struct{}
	logger    func(string)
	once      sync.Once
}

type Option func(*Scheduler)

func WithGenre(name string) Option {
	return func(s *Scheduler) {
		s.genreName = name
	}
}

func WithPerPage(n int) Option {
	return func(s *Scheduler) {
		s.perPage = n
	}
}

func WithDateRange(from, to string) Option {
	return func(s *Scheduler) {
		s.dateFrom = from
		s.dateTo = to
	}
}

func WithResultHandler(h func([]beatport.Track) error) Option {
	return func(s *Scheduler) {
		s.onResult = h
	}
}

func WithLogger(l func(string)) Option {
	return func(s *Scheduler) {
		s.logger = l
	}
}

func NewScheduler(bp *beatport.Beatport, interval time.Duration, opts ...Option) *Scheduler {
	s := &Scheduler{
		bp:       bp,
		interval: interval,
		perPage:  50,
		stop:     make(chan struct{}),
	}
	for _, opt := range opts {
		opt(s)
	}
	return s
}

func (s *Scheduler) Start() {
	go func() {
		for {
			s.scan()
			select {
			case <-time.After(s.interval):
			case <-s.stop:
				return
			}
		}
	}()
}

func (s *Scheduler) scan() {
	defer func() {
		if r := recover(); r != nil {
			if s.logger != nil {
				s.logger(fmt.Sprintf("scheduler: recovered from panic: %v", r))
			}
		}
	}()

	if s.logger != nil {
		s.logger("scheduler: starting scan")
	}

	res, err := s.bp.GetLatestTracks(s.genreName, 1, s.perPage, s.dateFrom, s.dateTo)
	if err != nil {
		if s.logger != nil {
			s.logger(fmt.Sprintf("scheduler: fetch failed: %v", err))
		}
		return
	}

	tracks := res.Results
	if s.logger != nil {
		s.logger(fmt.Sprintf("scheduler: scan returned %d tracks", len(tracks)))
	}

	if s.onResult != nil {
		if err := s.onResult(tracks); err != nil {
			if s.logger != nil {
				s.logger(fmt.Sprintf("scheduler: result handler error: %v", err))
			}
		}
	}
}

func (s *Scheduler) Stop() {
	s.once.Do(func() {
		close(s.stop)
	})
}
