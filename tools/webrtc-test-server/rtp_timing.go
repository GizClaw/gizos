package main

import (
	"flag"
	"fmt"
	"sync"
	"time"

	"github.com/pion/rtp"
)

const (
	maxRTPTimingCapacity = 65536
	maxRTPTimingSessions = 8
	rtpTimingTTL         = 5 * time.Minute
)

func registerRTPTimingFlag(flags *flag.FlagSet) *int {
	return flags.Int("rtp-timing-capacity", 0, "optional input RTP timing records per session (0 disables; maximum 65536)")
}

func validateRTPTimingCapacity(capacity int) error {
	if capacity < 0 || capacity > maxRTPTimingCapacity {
		return fmt.Errorf("rtp-timing-capacity must be between 0 and %d", maxRTPTimingCapacity)
	}
	return nil
}

// Only scalar input packet metadata is retained. Pion may bind a different
// SSRC/PT on the echoed packet. Times are monotonic elapsed nanoseconds from
// recorder creation, not wall-clock timestamps or UDP wire-send observations.
type rtpTimingRecord struct {
	InputSequence    uint16 `json:"input_sequence"`
	InputSSRC        uint32 `json:"input_ssrc"`
	InputPayloadType uint8  `json:"input_payload_type"`
	InputLengthBytes int    `json:"input_length_bytes"`
	ReceiveNS        int64  `json:"receive_ns"`
	EchoSubmitNS     int64  `json:"echo_submit_ns,omitempty"`
	EchoCompleteNS   int64  `json:"echo_complete_ns,omitempty"`
	Outcome          string `json:"outcome"`
}

type rtpTimingSnapshot struct {
	SessionID string            `json:"session_id"`
	Enabled   bool              `json:"enabled"`
	Complete  bool              `json:"complete"`
	Capacity  int               `json:"capacity"`
	Observed  uint64            `json:"observed"`
	Overflow  uint64            `json:"overflow"`
	Records   []rtpTimingRecord `json:"records"`
}

type rtpTiming struct {
	mu          sync.Mutex
	started     time.Time
	completedAt time.Time
	closing     bool
	workers     sync.WaitGroup
	finishOnce  sync.Once
	records     []rtpTimingRecord
	observed    uint64
	overflow    uint64
}

func newRTPTiming(capacity int) *rtpTiming {
	return &rtpTiming{started: time.Now(), records: make([]rtpTimingRecord, 0, capacity)}
}

// No worker can register after finish starts, so Wait cannot race with Add.
func (timing *rtpTiming) startEcho() bool {
	timing.mu.Lock()
	defer timing.mu.Unlock()
	if timing.closing {
		return false
	}
	timing.workers.Add(1)
	return true
}

func (timing *rtpTiming) finish() {
	if timing == nil {
		return
	}
	timing.finishOnce.Do(func() {
		timing.mu.Lock()
		timing.closing = true
		timing.mu.Unlock()
		timing.workers.Wait()
		timing.mu.Lock()
		timing.completedAt = time.Now()
		timing.mu.Unlock()
	})
}

func (timing *rtpTiming) receive(packet *rtp.Packet) int {
	timing.mu.Lock()
	defer timing.mu.Unlock()
	timing.observed++
	if len(timing.records) == cap(timing.records) {
		timing.overflow++
		return -1
	}
	index := len(timing.records)
	timing.records = append(timing.records, rtpTimingRecord{
		InputSequence: packet.SequenceNumber, InputSSRC: packet.SSRC,
		InputPayloadType: packet.PayloadType, InputLengthBytes: packet.MarshalSize(),
		ReceiveNS: time.Since(timing.started).Nanoseconds(), Outcome: "received",
	})
	return index
}

func (timing *rtpTiming) submit(index int) {
	if index < 0 {
		return
	}
	timing.mu.Lock()
	timing.records[index].EchoSubmitNS = time.Since(timing.started).Nanoseconds()
	timing.mu.Unlock()
}

func (timing *rtpTiming) complete(index int, success bool) {
	if index < 0 {
		return
	}
	timing.mu.Lock()
	record := &timing.records[index]
	record.EchoCompleteNS = time.Since(timing.started).Nanoseconds()
	record.Outcome = "echoed"
	if !success {
		record.Outcome = "write_error"
	}
	timing.mu.Unlock()
}

func (timing *rtpTiming) drop(index int) {
	if index < 0 {
		return
	}
	timing.mu.Lock()
	timing.records[index].Outcome = "dropped"
	timing.mu.Unlock()
}

func (timing *rtpTiming) snapshot(id string) (rtpTimingSnapshot, bool) {
	if timing == nil {
		return rtpTimingSnapshot{SessionID: id, Records: []rtpTimingRecord{}}, true
	}
	timing.mu.Lock()
	defer timing.mu.Unlock()
	if timing.records == nil {
		return rtpTimingSnapshot{}, false
	}
	records := make([]rtpTimingRecord, len(timing.records))
	copy(records, timing.records)
	return rtpTimingSnapshot{SessionID: id, Enabled: true,
		Complete: !timing.completedAt.IsZero(), Capacity: cap(timing.records),
		Observed: timing.observed, Overflow: timing.overflow, Records: records}, true
}

// Release only after completion, when no echo worker can touch the buffer. A
// live session may still hold the recorder itself after its records expire.
func (timing *rtpTiming) release() {
	timing.mu.Lock()
	timing.records = nil
	timing.mu.Unlock()
}

// The registry bounds pending, live, and completed recordings together. It owns
// recorders only, not PeerConnections; evicting completed metadata cannot close
// an otherwise live session. Call with the server lock held.
func (s *server) pruneRTPTimings(now time.Time) {
	for timing := range s.rtpTimings {
		timing.mu.Lock()
		completedAt := timing.completedAt
		timing.mu.Unlock()
		if !completedAt.IsZero() && now.Sub(completedAt) >= rtpTimingTTL {
			timing.release()
			delete(s.rtpTimings, timing)
		}
	}
}

func (s *server) newSession() (*session, error) {
	if err := validateRTPTimingCapacity(s.rtpTimingCapacity); err != nil {
		return nil, err
	}
	if s.rtpTimingCapacity == 0 {
		return &session{}, nil
	}
	s.mu.Lock()
	defer s.mu.Unlock()
	s.pruneRTPTimings(time.Now())
	if len(s.rtpTimings) >= maxRTPTimingSessions {
		var oldest *rtpTiming
		var oldestTime time.Time
		for timing := range s.rtpTimings {
			timing.mu.Lock()
			completedAt := timing.completedAt
			timing.mu.Unlock()
			if !completedAt.IsZero() && (oldest == nil || completedAt.Before(oldestTime)) {
				oldest, oldestTime = timing, completedAt
			}
		}
		if oldest == nil {
			return nil, fmt.Errorf("RTP timing session limit reached (%d)", maxRTPTimingSessions)
		}
		oldest.release()
		delete(s.rtpTimings, oldest)
	}
	if s.rtpTimings == nil {
		s.rtpTimings = make(map[*rtpTiming]string)
	}
	timing := newRTPTiming(s.rtpTimingCapacity)
	s.rtpTimings[timing] = ""
	return &session{timing: timing}, nil
}

func (s *server) discardRTPTiming(item *session) {
	if item.timing == nil {
		return
	}
	item.timing.finish()
	item.timing.release()
	s.mu.Lock()
	delete(s.rtpTimings, item.timing)
	s.mu.Unlock()
}

func (s *server) rtpTimingForSession(id string) (*rtpTiming, bool) {
	s.mu.Lock()
	defer s.mu.Unlock()
	s.pruneRTPTimings(time.Now())
	for timing, sessionID := range s.rtpTimings {
		if sessionID == id {
			return timing, true
		}
	}
	// Disabled live sessions still report an explicit disabled snapshot. An
	// enabled recording that expired/was evicted must instead return 404.
	if item := s.sessions[id]; item != nil && item.timing == nil {
		return nil, true
	}
	return nil, false
}
