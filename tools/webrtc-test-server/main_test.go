package main

import (
	"bytes"
	"encoding/binary"
	"encoding/json"
	"fmt"
	"io"
	"net"
	"net/http"
	"net/http/httptest"
	"strings"
	"sync"
	"sync/atomic"
	"testing"
	"time"

	"github.com/pion/stun/v3"
	"github.com/pion/webrtc/v4"
)

func TestICEModes(t *testing.T) {
	tests := []struct {
		name     string
		udp      bool
		tcp      bool
		networks []webrtc.NetworkType
	}{
		{name: "udp", udp: true, networks: []webrtc.NetworkType{webrtc.NetworkTypeUDP4}},
		{name: "tcp", tcp: true, networks: []webrtc.NetworkType{webrtc.NetworkTypeTCP4}},
		{name: "mixed", udp: true, tcp: true, networks: []webrtc.NetworkType{webrtc.NetworkTypeUDP4, webrtc.NetworkTypeTCP4}},
		{name: "mixed-drop-udp", udp: true, tcp: true, networks: []webrtc.NetworkType{webrtc.NetworkTypeUDP4, webrtc.NetworkTypeTCP4}},
	}
	for _, test := range tests {
		t.Run(test.name, func(t *testing.T) {
			mode, err := parseICEMode(test.name)
			if err != nil {
				t.Fatal(err)
			}
			if mode.usesUDP() != test.udp || mode.usesTCP() != test.tcp {
				t.Fatalf("mode=%s udp=%v tcp=%v", mode, mode.usesUDP(), mode.usesTCP())
			}
			got := mode.networkTypes()
			if len(got) != len(test.networks) {
				t.Fatalf("networks=%v", got)
			}
			for i := range got {
				if got[i] != test.networks[i] {
					t.Fatalf("networks=%v", got)
				}
			}
		})
	}
	if _, err := parseICEMode("invalid"); err == nil {
		t.Fatal("invalid ICE mode was accepted")
	}
}

func TestInboundDroppingPacketConnPreservesAddressAndCounts(t *testing.T) {
	listener, err := net.ListenPacket("udp4", "127.0.0.1:0")
	if err != nil {
		t.Fatal(err)
	}
	defer listener.Close()
	var drops atomic.Uint64
	dropping := &inboundDroppingPacketConn{PacketConn: listener, drops: &drops}
	if dropping.LocalAddr().String() != listener.LocalAddr().String() {
		t.Fatalf("local address changed: %s", dropping.LocalAddr())
	}
	sender, err := net.ListenPacket("udp4", "127.0.0.1:0")
	if err != nil {
		t.Fatal(err)
	}
	defer sender.Close()
	readDone := make(chan error, 1)
	go func() {
		buffer := make([]byte, 32)
		_, _, readErr := dropping.ReadFrom(buffer)
		readDone <- readErr
	}()
	if _, err := sender.WriteTo([]byte("drop"), listener.LocalAddr()); err != nil {
		t.Fatal(err)
	}
	deadline := time.Now().Add(time.Second)
	for drops.Load() != 1 && time.Now().Before(deadline) {
		time.Sleep(time.Millisecond)
	}
	if drops.Load() != 1 {
		t.Fatalf("inbound packet was not dropped: drops=%d", drops.Load())
	}
	if err := listener.Close(); err != nil {
		t.Fatal(err)
	}
	select {
	case err := <-readDone:
		if err == nil {
			t.Fatal("closed packet connection returned no read error")
		}
	case <-time.After(time.Second):
		t.Fatal("dropping ReadFrom did not unblock after close")
	}
}

func newTestServer() *server {
	return &server{sessions: make(map[string]*session)}
}

type performanceTestChannel struct {
	mu     sync.Mutex
	writes [][]byte
	block  chan struct{}
	start  chan struct{}
	once   sync.Once
}

func (channel *performanceTestChannel) Read(buffer []byte) (int, error) {
	return 0, io.EOF
}

func (channel *performanceTestChannel) Write(payload []byte) (int, error) {
	return channel.WriteDataChannel(payload, false)
}

func (channel *performanceTestChannel) ReadDataChannel(buffer []byte) (int, bool, error) {
	return 0, false, io.EOF
}

func (channel *performanceTestChannel) WriteDataChannel(payload []byte, isString bool) (int, error) {
	if isString {
		return 0, fmt.Errorf("performance frame was text")
	}
	channel.mu.Lock()
	channel.writes = append(channel.writes, append([]byte(nil), payload...))
	channel.mu.Unlock()
	if channel.start != nil {
		channel.once.Do(func() { close(channel.start) })
	}
	if channel.block != nil {
		<-channel.block
	}
	return len(payload), nil
}

func (channel *performanceTestChannel) Close() error { return nil }

func (channel *performanceTestChannel) snapshot() [][]byte {
	channel.mu.Lock()
	defer channel.mu.Unlock()
	writes := make([][]byte, len(channel.writes))
	copy(writes, channel.writes)
	return writes
}

func TestPerformanceChannelProtocol(t *testing.T) {
	raw := &performanceTestChannel{}
	channel := &performanceChannel{raw: raw}

	request := performanceHeader(performanceRequest, 0, 7, 0, 0)
	if !channel.handle(request) || len(raw.snapshot()) != 1 ||
		!bytes.Equal(raw.snapshot()[0], request) {
		t.Fatal("request did not receive an exact echo")
	}
	invalid := append(performanceHeader(performanceUpload, performanceLast, 0, 1, 0), 0x01)
	if !channel.handle(invalid) || len(raw.snapshot()) != 1 || channel.uploaded != 0 {
		t.Fatal("invalid upload payload changed channel state")
	}

	first := append(performanceHeader(performanceUpload, 0, 8, 5, 0), 0xa5, 0xa5, 0xa5)
	last := append(performanceHeader(performanceUpload, performanceLast, 9, 5, 3), 0xa5, 0xa5)
	if !channel.handle(first) || !channel.handle(last) {
		t.Fatal("upload frames were not recognized")
	}
	writes := raw.snapshot()
	if len(writes) != 2 || writes[1][4] != performanceUpload ||
		writes[1][5]&performanceLast == 0 ||
		binary.BigEndian.Uint32(writes[1][12:16]) != 5 {
		t.Fatalf("unexpected upload acknowledgement: %x", writes)
	}

	invalidDownloads := []struct {
		name     string
		flags    byte
		sequence uint16
		total    uint32
		offset   uint32
	}{
		{name: "flags", flags: performanceLast, total: performanceTransferSize},
		{name: "sequence", sequence: 1, total: performanceTransferSize},
		{name: "total", total: performanceChunkSize + 3},
		{name: "offset", total: performanceTransferSize, offset: 1},
	}
	for _, invalid := range invalidDownloads {
		if !channel.handle(performanceHeader(
			performanceDownload, invalid.flags, invalid.sequence,
			invalid.total, invalid.offset,
		)) || len(raw.snapshot()) != 2 {
			t.Fatalf("invalid download %s started a transfer", invalid.name)
		}
	}
	const downloadSize = performanceTransferSize
	if !channel.handle(performanceHeader(
		performanceDownload, 0, 0, downloadSize, 0,
	)) {
		t.Fatal("download request was not recognized")
	}
	deadline := time.Now().Add(time.Second)
	wantFrames := 2 + downloadSize/performanceChunkSize
	for len(raw.snapshot()) != wantFrames && time.Now().Before(deadline) {
		time.Sleep(time.Millisecond)
	}
	writes = raw.snapshot()
	if len(writes) != wantFrames {
		t.Fatalf("unexpected download frames: count=%d want=%d", len(writes), wantFrames)
	}
	lastDownload := writes[len(writes)-1]
	if len(writes[2]) != performanceHeaderSize+performanceChunkSize ||
		len(lastDownload) != performanceHeaderSize+performanceChunkSize ||
		lastDownload[5]&performanceLast == 0 ||
		binary.BigEndian.Uint32(lastDownload[12:16]) != downloadSize-performanceChunkSize {
		t.Fatalf("unexpected download frames: count=%d", len(writes))
	}
	for index, value := range lastDownload[performanceHeaderSize:] {
		if value != byte(downloadSize-performanceChunkSize+index) {
			t.Fatalf("download payload[%d]=%d", index, value)
		}
	}

	if channel.handle([]byte("ordinary")) {
		t.Fatal("ordinary payload was consumed as performance traffic")
	}
}

func TestPerformanceChannelRejectsConcurrentDownload(t *testing.T) {
	raw := &performanceTestChannel{
		block: make(chan struct{}),
		start: make(chan struct{}),
	}
	channel := &performanceChannel{raw: raw}
	request := performanceHeader(
		performanceDownload, 0, 0, performanceTransferSize, 0,
	)
	if !channel.handle(request) {
		t.Fatal("first download request was not recognized")
	}
	select {
	case <-raw.start:
	case <-time.After(time.Second):
		t.Fatal("first download did not start")
	}
	if !channel.handle(request) {
		t.Fatal("concurrent download request was not recognized")
	}
	time.Sleep(10 * time.Millisecond)
	if got := len(raw.snapshot()); got != 1 {
		t.Fatalf("concurrent download wrote %d frames before release", got)
	}
	close(raw.block)
	deadline := time.Now().Add(time.Second)
	for time.Now().Before(deadline) {
		channel.downloadMu.Lock()
		active := channel.downloadActive
		channel.downloadMu.Unlock()
		if !active {
			return
		}
		time.Sleep(time.Millisecond)
	}
	t.Fatal("download did not complete after release")
}

func TestSDPCandidateSummary(t *testing.T) {
	summary := sdpCandidateSummary(
		"v=0\r\n" +
			"a=candidate:1 1 UDP 1 127.0.0.1 5000 typ host ufrag credential\r\n" +
			"a=candidate:2 1 TCP 1 127.0.0.1 9 typ host tcptype active ufrag credential\r\n",
	)
	if summary != "count=2 udp/ipv4:5000/host/-,tcp/ipv4:9/host/active" {
		t.Fatalf("unexpected SDP summary %q", summary)
	}
	if strings.Contains(summary, "credential") {
		t.Fatalf("summary leaked ICE credential %q", summary)
	}
}

func TestICEMuxListenAddressMatchesAdvertisedReachability(t *testing.T) {
	tests := []struct {
		name        string
		candidateIP net.IP
		want        string
	}{
		{name: "discover non-loopback", want: "0.0.0.0:0"},
		{name: "loopback", candidateIP: net.ParseIP("127.0.0.1"), want: "127.0.0.1:0"},
		{name: "lan", candidateIP: net.ParseIP("192.168.1.7"), want: "192.168.1.7:0"},
	}
	for _, test := range tests {
		t.Run(test.name, func(t *testing.T) {
			if got := iceMuxListenAddress(test.candidateIP); got != test.want {
				t.Fatalf("iceMuxListenAddress() = %q, want %q", got, test.want)
			}
		})
	}
}

func TestSTUNBinding(t *testing.T) {
	listener, err := net.ListenPacket("udp4", "127.0.0.1:0")
	if err != nil {
		t.Fatal(err)
	}
	defer listener.Close()
	go serveSTUN(listener)

	client, err := net.ListenPacket("udp4", "127.0.0.1:0")
	if err != nil {
		t.Fatal(err)
	}
	defer client.Close()
	request, err := stun.Build(
		stun.TransactionID,
		stun.BindingRequest,
		stun.Fingerprint,
	)
	if err != nil {
		t.Fatal(err)
	}
	if _, err := client.WriteTo(request.Raw, listener.LocalAddr()); err != nil {
		t.Fatal(err)
	}
	if err := client.SetReadDeadline(time.Now().Add(time.Second)); err != nil {
		t.Fatal(err)
	}
	buffer := make([]byte, 2048)
	n, _, err := client.ReadFrom(buffer)
	if err != nil {
		t.Fatal(err)
	}
	response := new(stun.Message)
	if err := stun.Decode(buffer[:n], response); err != nil {
		t.Fatal(err)
	}
	if response.Type != stun.BindingSuccess {
		t.Fatalf("unexpected STUN response %s", response.Type)
	}
	var mapped stun.XORMappedAddress
	if err := mapped.GetFrom(response); err != nil {
		t.Fatal(err)
	}
	if mapped.Port != client.LocalAddr().(*net.UDPAddr).Port {
		t.Fatalf("mapped port %d does not match client", mapped.Port)
	}
}

func TestRelayOnlyOfferRequiresRelayCandidate(t *testing.T) {
	s := newTestServer()
	request := httptest.NewRequest(http.MethodPost, "/offer", bytes.NewBufferString("v=0\r\n"))
	request.Header.Set("X-H2-Relay-Only", "1")
	response := httptest.NewRecorder()
	s.handleOffer(response, request)
	if response.Code != http.StatusBadRequest {
		t.Fatalf("status=%d body=%q", response.Code, response.Body.String())
	}
}

func TestRelayOnlyOfferFiltersDirectCandidates(t *testing.T) {
	offer := "v=0\r\n" +
		"a=candidate:1 1 udp 1 192.0.2.1 10000 typ host\r\n" +
		"a=candidate:2 1 udp 1 198.51.100.1 20000 typ srflx\r\n" +
		"a=candidate:3 1 udp 1 203.0.113.1 30000 typ relay\r\n" +
		"a=end-of-candidates\r\n"
	filtered, err := relayOnlyOffer(offer)
	if err != nil {
		t.Fatal(err)
	}
	if strings.Contains(filtered, " typ host") ||
		strings.Contains(filtered, " typ srflx") {
		t.Fatalf("direct candidates remain in %q", filtered)
	}
	if !strings.Contains(filtered, " typ relay") ||
		!strings.Contains(filtered, "a=end-of-candidates") {
		t.Fatalf("relay candidate or SDP metadata missing from %q", filtered)
	}
}

func TestOfferBodyIsBounded(t *testing.T) {
	s := newTestServer()
	request := httptest.NewRequest(
		http.MethodPost,
		"/offer",
		bytes.NewReader(bytes.Repeat([]byte{'x'}, 256*1024+1)),
	)
	response := httptest.NewRecorder()
	s.handleOffer(response, request)
	if response.Code != http.StatusBadRequest {
		t.Fatalf("status=%d body=%q", response.Code, response.Body.String())
	}
}

func TestRoutesEnforceMethodsAndShutdown(t *testing.T) {
	s := newTestServer()
	shutdown := make(chan struct{}, 1)
	handler := s.routes(func() { shutdown <- struct{}{} })

	response := httptest.NewRecorder()
	handler.ServeHTTP(response, httptest.NewRequest(http.MethodGet, "/shutdown", nil))
	if response.Code != http.StatusMethodNotAllowed {
		t.Fatalf("GET /shutdown status=%d", response.Code)
	}

	response = httptest.NewRecorder()
	handler.ServeHTTP(response, httptest.NewRequest(http.MethodPost, "/shutdown", nil))
	if response.Code != http.StatusNoContent {
		t.Fatalf("POST /shutdown status=%d", response.Code)
	}
	select {
	case <-shutdown:
	default:
		t.Fatal("shutdown callback was not invoked")
	}
}

func TestTURNStatsSnapshot(t *testing.T) {
	s := newTestServer()
	s.turnStats.allocationsCreated.Store(2)
	s.turnStats.allocationsDeleted.Store(1)
	s.turnStats.permissionsCreated.Store(3)
	s.turnStats.relayIngress.Store(4)
	s.turnStats.relayEgress.Store(5)

	response := httptest.NewRecorder()
	s.handleTURNStats(response, httptest.NewRequest(http.MethodGet, "/turn-stats", nil))
	var snapshot turnStatsSnapshot
	if err := json.Unmarshal(response.Body.Bytes(), &snapshot); err != nil {
		t.Fatal(err)
	}
	if snapshot.AllocationsCreated != 2 || snapshot.AllocationsDeleted != 1 ||
		snapshot.PermissionsCreated != 3 || snapshot.RelayIngress != 4 ||
		snapshot.RelayEgress != 5 {
		t.Fatalf("unexpected TURN stats: %+v", snapshot)
	}
}

func TestChannelStatsSnapshot(t *testing.T) {
	s := newTestServer()
	item := &session{}
	item.channels.created.Store(512)
	item.channels.opened.Store(512)
	item.channels.closed.Store(511)
	item.channels.current.Store(1)
	item.channels.maxCurrent.Store(2)
	s.sessions["reuse"] = item

	response := httptest.NewRecorder()
	s.routes(func() {}).ServeHTTP(
		response,
		httptest.NewRequest(http.MethodGet, "/session/reuse/channel-stats", nil),
	)
	var snapshot channelStatsSnapshot
	if err := json.Unmarshal(response.Body.Bytes(), &snapshot); err != nil {
		t.Fatal(err)
	}
	if snapshot.Created != 512 || snapshot.Opened != 512 || snapshot.Closed != 511 ||
		snapshot.Current != 1 || snapshot.MaxCurrent != 2 {
		t.Fatalf("unexpected channel stats: %+v", snapshot)
	}
}

func TestChannelStatsAreSessionScopedAndConcurrentSafe(t *testing.T) {
	s := newTestServer()
	first := &session{}
	second := &session{}
	s.sessions["first"] = first
	s.sessions["second"] = second

	const workers = 64
	var group sync.WaitGroup
	for range workers {
		group.Add(1)
		go func() {
			defer group.Done()
			first.channels.onCreated()
			first.channels.onOpen()
			first.channels.onClose()
		}()
	}
	group.Wait()
	firstSnapshot := first.channels.snapshot()
	if firstSnapshot.Created != workers || firstSnapshot.Opened != workers ||
		firstSnapshot.Closed != workers || firstSnapshot.Current != 0 ||
		firstSnapshot.MaxCurrent == 0 {
		t.Fatalf("unexpected concurrent stats: %+v", firstSnapshot)
	}
	if second.channels.snapshot() != (channelStatsSnapshot{}) {
		t.Fatal("second session counters were modified")
	}

	handler := s.routes(func() {})
	response := httptest.NewRecorder()
	handler.ServeHTTP(
		response,
		httptest.NewRequest(http.MethodPost, "/session/first/channel-stats", nil),
	)
	if response.Code != http.StatusMethodNotAllowed {
		t.Fatalf("POST channel-stats status=%d", response.Code)
	}
	response = httptest.NewRecorder()
	handler.ServeHTTP(
		response,
		httptest.NewRequest(http.MethodGet, "/session/missing/channel-stats", nil),
	)
	if response.Code != http.StatusNotFound {
		t.Fatalf("missing channel-stats status=%d", response.Code)
	}
}

func TestCleanupClosesOnlyExpiredSessions(t *testing.T) {
	s := newTestServer()
	oldPC, err := webrtc.NewPeerConnection(webrtc.Configuration{})
	if err != nil {
		t.Fatal(err)
	}
	currentPC, err := webrtc.NewPeerConnection(webrtc.Configuration{})
	if err != nil {
		t.Fatal(err)
	}
	defer currentPC.Close()
	s.sessions["old"] = &session{
		id:        "old",
		pc:        oldPC,
		createdAt: time.Now().Add(-sessionTTL - time.Second),
	}
	s.sessions["current"] = &session{
		id:        "current",
		pc:        currentPC,
		createdAt: time.Now(),
	}

	s.cleanup()
	if _, ok := s.sessions["old"]; ok {
		t.Fatal("expired session remains")
	}
	if _, ok := s.sessions["current"]; !ok {
		t.Fatal("current session was removed")
	}
}
