package main

import (
	"bytes"
	"context"
	"encoding/json"
	"errors"
	"flag"
	"fmt"
	"io"
	"log"
	"net/http"
	"net/http/httptest"
	"reflect"
	"strconv"
	"strings"
	"sync"
	"testing"
	"time"

	"github.com/pion/rtp"
	"github.com/pion/webrtc/v4"
)

func timingSnapshot(t *testing.T, timing *rtpTiming) rtpTimingSnapshot {
	t.Helper()
	snapshot, retained := timing.snapshot("test")
	if !retained {
		t.Fatal("recording unexpectedly evicted")
	}
	return snapshot
}

func timingRequest(s *server, method, id, action string) *httptest.ResponseRecorder {
	response := httptest.NewRecorder()
	s.routes(func() {}).ServeHTTP(response,
		httptest.NewRequest(method, "/session/"+id+"/"+action, nil))
	return response
}

func TestRTPTimingDefaultsAndCapacityValidation(t *testing.T) {
	flags := flag.NewFlagSet("test", flag.ContinueOnError)
	capacity := registerRTPTimingFlag(flags)
	if err := flags.Parse(nil); err != nil || *capacity != 0 {
		t.Fatalf("timing must default off: capacity=%d error=%v", *capacity, err)
	}
	s := newTestServer()
	item, err := s.newSession()
	if err != nil || item.timing != nil || s.rtpTimings != nil {
		t.Fatalf("disabled session allocated a recorder: item=%+v error=%v", item, err)
	}
	id := s.track(item)
	response := timingRequest(s, http.MethodGet, id, "rtp-timing")
	var disabled rtpTimingSnapshot
	if err := json.Unmarshal(response.Body.Bytes(), &disabled); err != nil {
		t.Fatal(err)
	}
	if response.Code != http.StatusOK || disabled.Enabled || disabled.Capacity != 0 ||
		disabled.Observed != 0 || disabled.Overflow != 0 || len(disabled.Records) != 0 {
		t.Fatalf("default timing snapshot: %s", response.Body.String())
	}
	if err := flags.Parse([]string{"--rtp-timing-capacity=128"}); err != nil || *capacity != 128 {
		t.Fatalf("explicit capacity: %d %v", *capacity, err)
	}
	for _, value := range []int{-1, 0, 1, maxRTPTimingCapacity, maxRTPTimingCapacity + 1} {
		s.rtpTimingCapacity = value
		item, err := s.newSession()
		valid := value >= 0 && value <= maxRTPTimingCapacity
		if (err == nil) != valid {
			t.Fatalf("capacity=%d accepted=%t", value, err == nil)
		}
		if valid && value > 0 {
			if got := timingSnapshot(t, item.timing).Capacity; got != value {
				t.Fatalf("capacity=%d got=%d", value, got)
			}
			s.discardRTPTiming(item)
		}
	}
	for _, method := range []string{http.MethodPost, http.MethodDelete} {
		if got := timingRequest(s, method, id, "rtp-timing").Code; got != http.StatusMethodNotAllowed {
			t.Fatalf("method=%s status=%d", method, got)
		}
	}
	if got := timingRequest(s, http.MethodGet, "missing", "rtp-timing").Code; got != http.StatusNotFound {
		t.Fatalf("unknown recording status=%d", got)
	}
}

func runEchoPackets(packets []*rtp.Packet, write func(*rtp.Packet) error, witness *mediaWitness, timing *rtpTiming) {
	index := 0
	echoOpusPackets(func() (*rtp.Packet, error) {
		if index == len(packets) {
			return nil, io.EOF
		}
		packet := packets[index]
		index++
		return packet, nil
	}, write, witness, timing)
}

func TestRTPTimingCapacityOverflowAndDefaultEchoSemantics(t *testing.T) {
	packets := make([]*rtp.Packet, 0, 5)
	for _, sequence := range []uint16{65533, 65535, 65534, 1, 2} {
		packets = append(packets, &rtp.Packet{Header: rtp.Header{
			Version: 2, SequenceNumber: sequence, Timestamp: uint32(sequence) * 960,
			Marker: true, SSRC: 987, PayloadType: 109,
		}, Payload: []byte("private-Opus-data")})
	}
	var baseline []*rtp.Packet
	var baselineWitness mediaWitness
	runEchoPackets(packets, func(packet *rtp.Packet) error {
		baseline = append(baseline, packet)
		return nil
	}, &baselineWitness, nil)

	timing := newRTPTiming(3)
	var observed []*rtp.Packet
	var witness mediaWitness
	runEchoPackets(packets, func(packet *rtp.Packet) error {
		observed = append(observed, packet)
		return nil
	}, &witness, timing)
	timing.finish()
	if !reflect.DeepEqual(baseline, observed) || witness.echoed.Load() != 5 ||
		baselineWitness.echoed.Load() != 5 {
		t.Fatal("recording or overflow changed default echo output")
	}
	for i, packet := range observed {
		if packet.SequenceNumber != packets[i].SequenceNumber || packet.Timestamp != packets[i].Timestamp ||
			!packet.Marker || packet.PayloadType != 111 || !bytes.Equal(packet.Payload, packets[i].Payload) {
			t.Fatalf("echo changed source timeline or payload at %d", i)
		}
	}
	snapshot := timingSnapshot(t, timing)
	if !snapshot.Complete || snapshot.Capacity != 3 || len(snapshot.Records) != 3 ||
		snapshot.Observed != 5 || snapshot.Overflow != 2 {
		t.Fatalf("bounded recording: %+v", snapshot)
	}
	var previous int64
	for i, record := range snapshot.Records {
		if record.InputSequence != packets[i].SequenceNumber || record.InputSSRC != 987 ||
			record.InputPayloadType != 109 || record.InputLengthBytes != packets[i].MarshalSize() ||
			record.ReceiveNS < previous || record.EchoSubmitNS < record.ReceiveNS ||
			record.EchoCompleteNS < record.EchoSubmitNS || record.Outcome != "echoed" {
			t.Fatalf("non-monotonic or incorrect input metadata: %+v", record)
		}
		previous = record.EchoCompleteNS
	}
	snapshot.Records[0].InputSSRC = 0
	if timingSnapshot(t, timing).Records[0].InputSSRC != 987 {
		t.Fatal("snapshot aliases recorder storage")
	}
}

func TestRTPTimingDropsAndWriteErrors(t *testing.T) {
	packets := []*rtp.Packet{
		{Header: rtp.Header{Version: 2, SequenceNumber: 1}},
		{Header: rtp.Header{Version: 2, SequenceNumber: 2}},
	}
	timing := newRTPTiming(2)
	var witness mediaWitness
	witness.dropAt.Store(1)
	writes := 0
	runEchoPackets(packets, func(packet *rtp.Packet) error {
		writes++
		return errors.New("private write failure")
	}, &witness, timing)
	timing.finish()
	snapshot := timingSnapshot(t, timing)
	first, second := snapshot.Records[0], snapshot.Records[1]
	if writes != 1 || first.Outcome != "dropped" || first.EchoSubmitNS != 0 || first.EchoCompleteNS != 0 ||
		second.Outcome != "write_error" || second.EchoCompleteNS < second.EchoSubmitNS ||
		witness.dropped.Load() != 1 || witness.writeErrors.Load() != 1 {
		t.Fatalf("incorrect drop/write outcome: %+v", snapshot)
	}
}

func newTrackedTimingSession(t *testing.T, s *server) *session {
	t.Helper()
	item, err := s.newSession()
	if err != nil {
		t.Fatal(err)
	}
	item.pc, err = webrtc.NewPeerConnection(webrtc.Configuration{})
	if err != nil {
		s.discardRTPTiming(item)
		t.Fatal(err)
	}
	s.track(item)
	return item
}

func TestRTPTimingEndpointAfterCloseWaitsForEchoAndContainsOnlyMetadata(t *testing.T) {
	s := newTestServer()
	s.rtpTimingCapacity = 4
	item := newTrackedTimingSession(t, s)
	defer s.closeAll()
	packet := &rtp.Packet{Header: rtp.Header{Version: 2, SequenceNumber: 42,
		SSRC: 123456, PayloadType: 109, Timestamp: 7654321}, Payload: []byte("private-payload-must-not-appear")}
	writing := make(chan struct{})
	releaseWrite := make(chan struct{})
	var releaseOnce sync.Once
	release := func() { releaseOnce.Do(func() { close(releaseWrite) }) }
	defer release()
	echoDone := make(chan struct{})
	go func() {
		defer close(echoDone)
		runEchoPackets([]*rtp.Packet{packet}, func(echo *rtp.Packet) error {
			// Simulate the SSRC/PT binding performed by Pion WriteRTP.
			echo.SSRC, echo.PayloadType = 654321, 111
			close(writing)
			<-releaseWrite
			return nil
		}, &item.media, item.timing)
	}()
	select {
	case <-writing:
	case <-time.After(5 * time.Second):
		t.Fatal("echo did not reach WriteRTP")
	}
	closed := make(chan *httptest.ResponseRecorder, 1)
	go func() { closed <- timingRequest(s, http.MethodPost, item.id, "close") }()
	deadline := time.Now().Add(5 * time.Second)
	for {
		item.timing.mu.Lock()
		closing := item.timing.closing
		item.timing.mu.Unlock()
		if closing {
			break
		}
		if time.Now().After(deadline) {
			t.Fatal("close did not begin finalizing recording")
		}
		time.Sleep(time.Millisecond)
	}
	response := timingRequest(s, http.MethodGet, item.id, "rtp-timing")
	var inFlight rtpTimingSnapshot
	if err := json.Unmarshal(response.Body.Bytes(), &inFlight); err != nil {
		t.Fatal(err)
	}
	if response.Code != http.StatusOK || inFlight.Complete || len(inFlight.Records) != 1 ||
		inFlight.Records[0].EchoSubmitNS == 0 || inFlight.Records[0].EchoCompleteNS != 0 {
		t.Fatalf("in-flight snapshot: %s", response.Body.String())
	}
	select {
	case <-closed:
		t.Fatal("close completed before accepted echo finished")
	default:
	}
	release()
	select {
	case response = <-closed:
		if response.Code != http.StatusNoContent {
			t.Fatalf("close status=%d", response.Code)
		}
	case <-time.After(5 * time.Second):
		t.Fatal("close failed to join echo worker")
	}
	<-echoDone
	response = timingRequest(s, http.MethodGet, item.id, "rtp-timing")
	var snapshot rtpTimingSnapshot
	if err := json.Unmarshal(response.Body.Bytes(), &snapshot); err != nil {
		t.Fatal(err)
	}
	if response.Code != http.StatusOK || !snapshot.Complete || snapshot.SessionID != item.id ||
		snapshot.Records[0].InputSSRC != packet.SSRC || snapshot.Records[0].InputPayloadType != packet.PayloadType ||
		snapshot.Records[0].Outcome != "echoed" {
		t.Fatalf("post-close recording: %s", response.Body.String())
	}
	// Whitelist the serialized schema rather than searching source code.
	var document map[string]json.RawMessage
	if err := json.Unmarshal(response.Body.Bytes(), &document); err != nil {
		t.Fatal(err)
	}
	for _, key := range []string{"session_id", "enabled", "complete", "capacity", "observed", "overflow", "records"} {
		if _, present := document[key]; !present {
			t.Fatalf("missing output field %s", key)
		}
		delete(document, key)
	}
	if len(document) != 0 {
		t.Fatalf("unexpected session output fields: %v", document)
	}
	var records []map[string]json.RawMessage
	if err := json.Unmarshal(response.Body.Bytes(), &struct {
		Records *[]map[string]json.RawMessage `json:"records"`
	}{Records: &records}); err != nil {
		t.Fatal(err)
	}
	for _, record := range records {
		for _, key := range []string{"input_sequence", "input_ssrc", "input_payload_type", "input_length_bytes",
			"receive_ns", "echo_submit_ns", "echo_complete_ns", "outcome"} {
			if _, present := record[key]; !present {
				t.Fatalf("missing record field %s", key)
			}
			delete(record, key)
		}
		if len(record) != 0 {
			t.Fatalf("unexpected packet output fields: %v", record)
		}
	}
	if strings.Contains(response.Body.String(), string(packet.Payload)) {
		t.Fatal("payload leaked into timing response")
	}
	called := false
	echoOpusPackets(func() (*rtp.Packet, error) { called = true; return nil, io.EOF },
		func(*rtp.Packet) error { return nil }, &item.media, item.timing)
	if called {
		t.Fatal("a new echo worker started after recording completion")
	}
}

func TestRTPTimingSessionBoundTTLAndBufferRelease(t *testing.T) {
	s := newTestServer()
	s.rtpTimingCapacity = 2
	items := make([]*session, 0, maxRTPTimingSessions)
	for range maxRTPTimingSessions {
		item, err := s.newSession()
		if err != nil {
			t.Fatal(err)
		}
		s.track(item)
		items = append(items, item)
	}
	if _, err := s.newSession(); err == nil {
		t.Fatal("live recording limit was exceeded")
	}
	items[0].timing.finish()
	replacement, err := s.newSession()
	if err != nil {
		t.Fatal(err)
	}
	s.track(replacement)
	if len(s.rtpTimings) != maxRTPTimingSessions {
		t.Fatalf("recording bound changed: %d", len(s.rtpTimings))
	}
	if got := timingRequest(s, http.MethodGet, items[0].id, "rtp-timing").Code; got != http.StatusNotFound {
		t.Fatalf("evicted recording still served: %d", got)
	}
	if _, retained := items[0].timing.snapshot(items[0].id); retained {
		t.Fatal("evicted buffer is still retained through the live session")
	}
	for _, item := range items[1:] {
		if got := timingRequest(s, http.MethodGet, item.id, "rtp-timing").Code; got != http.StatusOK {
			t.Fatalf("live recording evicted: %d", got)
		}
	}
	items[1].timing.finish()
	items[1].timing.mu.Lock()
	items[1].timing.completedAt = time.Now().Add(-rtpTimingTTL - time.Second)
	items[1].timing.mu.Unlock()
	if got := timingRequest(s, http.MethodGet, items[1].id, "rtp-timing").Code; got != http.StatusNotFound {
		t.Fatalf("expired recording still served: %d", got)
	}
	if _, retained := items[1].timing.snapshot(items[1].id); retained {
		t.Fatal("expired recording buffer was not released")
	}
}

func TestRTPTimingConcurrentSnapshotsCleanupAndCompletion(t *testing.T) {
	s := newTestServer()
	s.rtpTimingCapacity = 64
	item := newTrackedTimingSession(t, s)
	other := newTrackedTimingSession(t, s)
	defer s.closeAll()
	var workers sync.WaitGroup
	for stream := range 4 {
		workers.Add(1)
		go func() {
			defer workers.Done()
			packets := make([]*rtp.Packet, 100)
			for i := range packets {
				packets[i] = &rtp.Packet{Header: rtp.Header{Version: 2,
					SequenceNumber: uint16(i), SSRC: uint32(stream + 1)}}
			}
			var witness mediaWitness
			runEchoPackets(packets, func(*rtp.Packet) error { return nil }, &witness, item.timing)
		}()
	}
	for range 4 {
		workers.Add(1)
		go func() {
			defer workers.Done()
			for range 100 {
				response := timingRequest(s, http.MethodGet, item.id, "rtp-timing")
				var snapshot rtpTimingSnapshot
				if response.Code != http.StatusOK || json.Unmarshal(response.Body.Bytes(), &snapshot) != nil ||
					len(snapshot.Records) > 64 || snapshot.Observed != uint64(len(snapshot.Records))+snapshot.Overflow {
					t.Error("inconsistent concurrent snapshot")
					return
				}
				var previous int64
				for _, record := range snapshot.Records {
					if record.ReceiveNS < previous || (record.EchoSubmitNS != 0 && record.EchoSubmitNS < record.ReceiveNS) ||
						(record.EchoCompleteNS != 0 && record.EchoCompleteNS < record.EchoSubmitNS) {
						t.Error("non-monotonic concurrent record")
						return
					}
					previous = record.ReceiveNS
				}
				s.cleanup()
			}
		}()
	}
	workers.Wait()
	if otherSnapshot := timingSnapshot(t, other.timing); otherSnapshot.Observed != 0 {
		t.Fatal("recording crossed session boundary")
	}
	item.createdAt = time.Now().Add(-sessionTTL - time.Second)
	s.cleanup()
	snapshot := timingSnapshot(t, item.timing)
	if !snapshot.Complete || snapshot.Observed != 400 || snapshot.Overflow != 336 || len(snapshot.Records) != 64 {
		t.Fatalf("completion lost records: %+v", snapshot)
	}
	if got := timingRequest(s, http.MethodGet, item.id, "rtp-timing").Code; got != http.StatusOK {
		t.Fatalf("cleanup lost completed recording: %d", got)
	}
	s.closeAll()
	if got := timingRequest(s, http.MethodGet, item.id, "rtp-timing").Code; got != http.StatusNotFound {
		t.Fatalf("shutdown retained recording: %d", got)
	}
	if _, retained := item.timing.snapshot(item.id); retained {
		t.Fatal("shutdown did not release retained buffer")
	}
}

func TestRTPTimingFailedOffersReleaseReservation(t *testing.T) {
	s := newTestServer()
	s.rtpTimingCapacity = 1
	s.api = webrtc.NewAPI()
	for range maxRTPTimingSessions + 1 {
		if _, _, err := s.createAnswer(context.Background(), "invalid SDP", 0, nil); err == nil {
			t.Fatal("invalid offer accepted")
		}
		if len(s.rtpTimings) != 0 {
			t.Fatal("failed offer retained a recording reservation")
		}
	}
}

type timeoutTestTransport struct {
	bufferedCalls int
	statsCalls    int
}

func (transport *timeoutTestTransport) BufferedAmount() int {
	transport.bufferedCalls++
	return 262144
}

func (transport *timeoutTestTransport) Stats() webrtc.SCTPTransportStats {
	transport.statsCalls++
	return webrtc.SCTPTransportStats{
		ID: "private metadata", CongestionWindow: 12345, ReceiverWindow: 23456,
		MTU: 1200, SmoothedRoundTripTime: 0.0125, BytesSent: 34567, BytesReceived: 45678,
		// This public field is not populated by the pinned Pion Stats getter.
		UNACKData: 999,
	}
}

func TestDownloadBackpressureTimeoutSamplesSafeStatsOnce(t *testing.T) {
	var output bytes.Buffer
	previousOutput, previousFlags := log.Writer(), log.Flags()
	log.SetOutput(&output)
	log.SetFlags(0)
	defer func() { log.SetOutput(previousOutput); log.SetFlags(previousFlags) }()
	transport := &timeoutTestTransport{}
	logDownloadBackpressureTimeout(transport)
	if transport.bufferedCalls != 1 || transport.statsCalls != 1 {
		t.Fatalf("timeout must sample once: %+v", transport)
	}
	expected := "H2_WEBRTC_TEST_SERVER_PERF download_backpressure_timeout buffered=262144 cwnd=12345 rwnd=23456 mtu=1200 srtt_ms=12.500 bytes_sent=34567 bytes_received=45678\n"
	if output.String() != expected {
		t.Fatalf("timeout snapshot: %q", output.String())
	}
}

func TestRTPTimingFlagsRejectMalformedCapacity(t *testing.T) {
	for _, value := range []string{"-1", strconv.Itoa(maxRTPTimingCapacity + 1), "many"} {
		t.Run(value, func(t *testing.T) {
			flags := flag.NewFlagSet("test", flag.ContinueOnError)
			flags.SetOutput(io.Discard)
			capacity := registerRTPTimingFlag(flags)
			err := flags.Parse([]string{fmt.Sprintf("--rtp-timing-capacity=%s", value)})
			if err == nil {
				err = validateRTPTimingCapacity(*capacity)
			}
			if err == nil {
				t.Fatal("invalid capacity accepted")
			}
		})
	}
}
