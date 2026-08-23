package main

import (
	"bytes"
	"context"
	"encoding/binary"
	"encoding/json"
	"flag"
	"fmt"
	"io"
	"log"
	"net"
	"net/http"
	"os"
	"os/signal"
	"strconv"
	"strings"
	"sync"
	"sync/atomic"
	"syscall"
	"time"

	"github.com/pion/datachannel"
	"github.com/pion/rtp"
	"github.com/pion/stun/v3"
	"github.com/pion/turn/v4"
	"github.com/pion/webrtc/v4"
)

const (
	defaultListen           = "127.0.0.1:0"
	defaultSTUNListen       = "127.0.0.1:0"
	defaultTURNListen       = "127.0.0.1:0"
	sessionTTL              = 30 * time.Minute
	turnRealm               = "h2peer.test"
	turnUsername            = "h2peer"
	turnCredential          = "h2peer-secret"
	performanceHeaderSize   = 16
	performanceChunkSize    = 8 * 1024
	performanceTransferSize = 10 * 1024 * 1024
)

var performanceMagic = [4]byte{'H', '2', 'P', 'F'}

const (
	performanceRequest  byte = 1
	performanceUpload   byte = 2
	performanceDownload byte = 3
	performanceEcho     byte = 4
	performanceLast     byte = 1
)

type session struct {
	id        string
	pc        *webrtc.PeerConnection
	createdAt time.Time
	channels  channelStats
}

type iceMode string

const (
	iceModeUDP          iceMode = "udp"
	iceModeTCP          iceMode = "tcp"
	iceModeMixed        iceMode = "mixed"
	iceModeMixedDropUDP iceMode = "mixed-drop-udp"
)

func parseICEMode(value string) (iceMode, error) {
	mode := iceMode(value)
	switch mode {
	case iceModeUDP, iceModeTCP, iceModeMixed, iceModeMixedDropUDP:
		return mode, nil
	default:
		return "", fmt.Errorf("invalid ICE mode %q", value)
	}
}

func (m iceMode) usesUDP() bool {
	return m != iceModeTCP
}

func (m iceMode) usesTCP() bool {
	return m != iceModeUDP
}

func (m iceMode) networkTypes() []webrtc.NetworkType {
	types := make([]webrtc.NetworkType, 0, 2)
	if m.usesUDP() {
		types = append(types, webrtc.NetworkTypeUDP4)
	}
	if m.usesTCP() {
		types = append(types, webrtc.NetworkTypeTCP4)
	}
	return types
}

type channelStats struct {
	created        atomic.Uint64
	opened         atomic.Uint64
	closed         atomic.Uint64
	current        atomic.Uint64
	maxCurrent     atomic.Uint64
	reverseReplies atomic.Uint64
}

type channelStatsSnapshot struct {
	Created        uint64 `json:"created"`
	Opened         uint64 `json:"opened"`
	Closed         uint64 `json:"closed"`
	Current        uint64 `json:"current"`
	MaxCurrent     uint64 `json:"max_current"`
	ReverseReplies uint64 `json:"reverse_replies"`
}

func (s *channelStats) onCreated() {
	s.created.Add(1)
}

func (s *channelStats) onOpen() {
	current := s.current.Add(1)
	s.opened.Add(1)
	for {
		maximum := s.maxCurrent.Load()
		if current <= maximum || s.maxCurrent.CompareAndSwap(maximum, current) {
			return
		}
	}
}

func (s *channelStats) onClose() {
	s.closed.Add(1)
	for {
		current := s.current.Load()
		if current == 0 || s.current.CompareAndSwap(current, current-1) {
			return
		}
	}
}

func (s *channelStats) snapshot() channelStatsSnapshot {
	return channelStatsSnapshot{
		Created:        s.created.Load(),
		Opened:         s.opened.Load(),
		Closed:         s.closed.Load(),
		Current:        s.current.Load(),
		MaxCurrent:     s.maxCurrent.Load(),
		ReverseReplies: s.reverseReplies.Load(),
	}
}

type server struct {
	mu          sync.Mutex
	sessions    map[string]*session
	nextSession uint64
	candidateIP net.IP
	api         *webrtc.API
	iceMode     iceMode
	udpDrops    atomic.Uint64
	turnStats   turnStats
}

type inboundDroppingPacketConn struct {
	net.PacketConn
	drops *atomic.Uint64
}

func (c *inboundDroppingPacketConn) ReadFrom(data []byte) (int, net.Addr, error) {
	for {
		n, addr, err := c.PacketConn.ReadFrom(data)
		if err != nil {
			return n, addr, err
		}
		c.drops.Add(1)
	}
}

type icePairSnapshot struct {
	Mode           iceMode `json:"mode"`
	LocalProtocol  string  `json:"local_protocol"`
	RemoteProtocol string  `json:"remote_protocol"`
	LocalType      string  `json:"local_type"`
	RemoteType     string  `json:"remote_type"`
	LocalTCPType   string  `json:"local_tcp_type"`
	RemoteTCPType  string  `json:"remote_tcp_type"`
	UDPDrops       uint64  `json:"udp_drops"`
}

type turnStats struct {
	allocationsCreated atomic.Uint64
	allocationsDeleted atomic.Uint64
	permissionsCreated atomic.Uint64
	channelsCreated    atomic.Uint64
	relayIngress       atomic.Uint64
	relayEgress        atomic.Uint64
}

type turnStatsSnapshot struct {
	AllocationsCreated uint64 `json:"allocations_created"`
	AllocationsDeleted uint64 `json:"allocations_deleted"`
	PermissionsCreated uint64 `json:"permissions_created"`
	ChannelsCreated    uint64 `json:"channels_created"`
	RelayIngress       uint64 `json:"relay_ingress"`
	RelayEgress        uint64 `json:"relay_egress"`
}

type countingPacketConn struct {
	net.PacketConn
	stats *turnStats
}

func (c *countingPacketConn) ReadFrom(data []byte) (int, net.Addr, error) {
	n, addr, err := c.PacketConn.ReadFrom(data)
	if n > 0 {
		c.stats.relayIngress.Add(1)
	}
	return n, addr, err
}

func (c *countingPacketConn) WriteTo(data []byte, addr net.Addr) (int, error) {
	n, err := c.PacketConn.WriteTo(data, addr)
	if n > 0 {
		c.stats.relayEgress.Add(1)
	}
	return n, err
}

type countingRelayGenerator struct {
	inner turn.RelayAddressGeneratorStatic
	stats *turnStats
}

func (g *countingRelayGenerator) Validate() error {
	return g.inner.Validate()
}

func (g *countingRelayGenerator) AllocatePacketConn(
	network string,
	requestedPort int,
) (net.PacketConn, net.Addr, error) {
	conn, addr, err := g.inner.AllocatePacketConn(network, requestedPort)
	if err != nil {
		return nil, nil, err
	}
	return &countingPacketConn{PacketConn: conn, stats: g.stats}, addr, nil
}

func (g *countingRelayGenerator) AllocateConn(
	network string,
	requestedPort int,
) (net.Conn, net.Addr, error) {
	return g.inner.AllocateConn(network, requestedPort)
}

func iceMuxListenAddress(candidateIP net.IP) string {
	if candidateIP != nil {
		return net.JoinHostPort(candidateIP.String(), "0")
	}
	return "0.0.0.0:0"
}

func main() {
	listen := flag.String("listen", defaultListen, "HTTP listen address")
	stunListen := flag.String("stun-listen", defaultSTUNListen, "STUN UDP listen address")
	turnListen := flag.String("turn-listen", defaultTURNListen, "TURN UDP listen address")
	candidateIP := flag.String("candidate-ip", "", "ICE host candidate IP to advertise; defaults to non-loopback IPv4 addresses")
	iceModeValue := flag.String("ice-mode", string(iceModeUDP), "ICE transport mode: udp, tcp, mixed, or mixed-drop-udp")
	dtlsKeyLog := flag.String("dtls-key-log", "", "optional DTLS key log path for local packet diagnostics")
	flag.Parse()
	mode, err := parseICEMode(*iceModeValue)
	if err != nil {
		log.Fatal(err)
	}

	s := &server{
		candidateIP: net.ParseIP(*candidateIP),
		iceMode:     mode,
		sessions:    make(map[string]*session),
	}
	if *candidateIP != "" && s.candidateIP == nil {
		log.Fatalf("invalid --candidate-ip %q", *candidateIP)
	}
	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()
	go s.cleanupLoop(ctx)
	stunConn, err := net.ListenPacket("udp4", *stunListen)
	if err != nil {
		log.Fatalf("listen STUN: %v", err)
	}
	defer stunConn.Close()
	go serveSTUN(stunConn)

	var iceUDPConn net.PacketConn
	var iceTCPListener net.Listener
	var iceUDPAddr = "-"
	var iceTCPAddr = "-"
	var setting webrtc.SettingEngine
	setting.DetachDataChannels()
	var dtlsKeyLogFile *os.File
	if *dtlsKeyLog != "" {
		dtlsKeyLogFile, err = os.OpenFile(
			*dtlsKeyLog, os.O_CREATE|os.O_WRONLY|os.O_TRUNC, 0o600,
		)
		if err != nil {
			log.Fatalf("open DTLS key log: %v", err)
		}
		defer dtlsKeyLogFile.Close()
		setting.SetDTLSKeyLogWriter(dtlsKeyLogFile)
	}
	setting.SetNetworkTypes(mode.networkTypes())
	setting.SetIncludeLoopbackCandidate(true)
	setting.SetIPFilter(func(ip net.IP) bool {
		ip4 := ip.To4()
		if ip4 == nil || ip.IsUnspecified() {
			return false
		}
		if s.candidateIP != nil {
			return ip4.Equal(s.candidateIP.To4())
		}
		return !ip.IsLoopback()
	})
	if mode.usesUDP() {
		iceUDPConn, err = net.ListenPacket("udp4", iceMuxListenAddress(s.candidateIP))
		if err != nil {
			log.Fatalf("listen ICE UDP: %v", err)
		}
		iceUDPAddr = iceUDPConn.LocalAddr().String()
		muxConn := iceUDPConn
		if mode == iceModeMixedDropUDP {
			muxConn = &inboundDroppingPacketConn{
				PacketConn: iceUDPConn,
				drops:      &s.udpDrops,
			}
		}
		udpMux := webrtc.NewICEUDPMux(nil, muxConn)
		defer udpMux.Close()
		setting.SetICEUDPMux(udpMux)
	}
	if mode.usesTCP() {
		iceTCPListener, err = net.Listen("tcp4", iceMuxListenAddress(s.candidateIP))
		if err != nil {
			log.Fatalf("listen ICE TCP: %v", err)
		}
		iceTCPAddr = iceTCPListener.Addr().String()
		tcpMux := webrtc.NewICETCPMux(nil, iceTCPListener, 8)
		defer tcpMux.Close()
		setting.SetICETCPMux(tcpMux)
	}
	s.api = webrtc.NewAPI(webrtc.WithSettingEngine(setting))
	turnConn, err := net.ListenPacket("udp4", *turnListen)
	if err != nil {
		log.Fatalf("listen TURN: %v", err)
	}
	turnServer, err := turn.NewServer(turn.ServerConfig{
		Realm: turnRealm,
		AuthHandler: func(username, realm string, _ net.Addr) ([]byte, bool) {
			if username != turnUsername || realm != turnRealm {
				return nil, false
			}
			return turn.GenerateAuthKey(username, realm, turnCredential), true
		},
		PacketConnConfigs: []turn.PacketConnConfig{{
			PacketConn: turnConn,
			RelayAddressGenerator: &countingRelayGenerator{
				inner: turn.RelayAddressGeneratorStatic{
					RelayAddress: net.ParseIP("127.0.0.1"),
					Address:      "127.0.0.1",
				},
				stats: &s.turnStats,
			},
		}},
		EventHandler: turn.EventHandler{
			OnAllocationCreated: func(_, _ net.Addr, _, _, _ string, _ net.Addr, _ int) {
				s.turnStats.allocationsCreated.Add(1)
			},
			OnAllocationDeleted: func(_, _ net.Addr, _, _, _ string) {
				s.turnStats.allocationsDeleted.Add(1)
			},
			OnPermissionCreated: func(_, _ net.Addr, _, _, _ string, _ net.Addr, _ net.IP) {
				s.turnStats.permissionsCreated.Add(1)
			},
			OnChannelCreated: func(_, _ net.Addr, _, _, _ string, _, _ net.Addr, _ uint16) {
				s.turnStats.channelsCreated.Add(1)
			},
		},
	})
	if err != nil {
		log.Fatalf("start TURN: %v", err)
	}

	httpListener, err := net.Listen("tcp4", *listen)
	if err != nil {
		log.Fatalf("listen HTTP: %v", err)
	}
	defer httpListener.Close()

	shutdown := make(chan struct{})
	var shutdownOnce sync.Once
	mux := s.routes(func() {
		shutdownOnce.Do(func() { close(shutdown) })
	})

	fmt.Fprintf(
		os.Stdout,
		"H2_WEBRTC_TEST_SERVER_READY http=%s stun=%s turn=%s ice_udp=%s ice_tcp=%s mode=%s candidate_ip=%s\n",
		httpListener.Addr(),
		stunConn.LocalAddr(),
		turnConn.LocalAddr(),
		iceUDPAddr,
		iceTCPAddr,
		mode,
		*candidateIP,
	)
	httpServer := &http.Server{Handler: mux}
	serveErr := make(chan error, 1)
	go func() { serveErr <- httpServer.Serve(httpListener) }()
	signals := make(chan os.Signal, 1)
	signal.Notify(signals, os.Interrupt, syscall.SIGTERM)
	select {
	case <-shutdown:
	case <-signals:
	case err := <-serveErr:
		if err != nil && err != http.ErrServerClosed {
			log.Printf("H2_WEBRTC_TEST_SERVER_HTTP error=%v", err)
		}
	}
	signal.Stop(signals)
	cancel()
	shutdownCtx, shutdownCancel := context.WithTimeout(context.Background(), 5*time.Second)
	defer shutdownCancel()
	_ = httpServer.Shutdown(shutdownCtx)
	s.closeAll()
	_ = turnServer.Close()
}

func (s *server) routes(shutdown func()) http.Handler {
	mux := http.NewServeMux()
	mux.HandleFunc("/offer", s.handleOffer)
	mux.HandleFunc("/session/", s.handleSession)
	mux.HandleFunc("/turn-stats", s.handleTURNStats)
	mux.HandleFunc("/healthz", func(w http.ResponseWriter, _ *http.Request) {
		w.WriteHeader(http.StatusNoContent)
	})
	mux.HandleFunc("/shutdown", func(w http.ResponseWriter, r *http.Request) {
		if r.Method != http.MethodPost {
			http.Error(w, "method not allowed", http.StatusMethodNotAllowed)
			return
		}
		w.WriteHeader(http.StatusNoContent)
		shutdown()
	})
	return mux
}

func (s *server) handleTURNStats(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodGet {
		http.Error(w, "method not allowed", http.StatusMethodNotAllowed)
		return
	}
	w.Header().Set("Content-Type", "application/json")
	_ = json.NewEncoder(w).Encode(turnStatsSnapshot{
		AllocationsCreated: s.turnStats.allocationsCreated.Load(),
		AllocationsDeleted: s.turnStats.allocationsDeleted.Load(),
		PermissionsCreated: s.turnStats.permissionsCreated.Load(),
		ChannelsCreated:    s.turnStats.channelsCreated.Load(),
		RelayIngress:       s.turnStats.relayIngress.Load(),
		RelayEgress:        s.turnStats.relayEgress.Load(),
	})
}

func serveSTUN(conn net.PacketConn) {
	buffer := make([]byte, 2048)
	for {
		n, remote, err := conn.ReadFrom(buffer)
		if err != nil {
			return
		}
		request := new(stun.Message)
		if err := stun.Decode(buffer[:n], request); err != nil || request.Type != stun.BindingRequest {
			continue
		}
		udpRemote, ok := remote.(*net.UDPAddr)
		if !ok {
			continue
		}
		response, err := stun.Build(
			request,
			stun.BindingSuccess,
			&stun.XORMappedAddress{IP: udpRemote.IP, Port: udpRemote.Port},
			stun.Fingerprint,
		)
		if err != nil {
			log.Printf("H2_WEBRTC_TEST_SERVER_STUN build_error=%v", err)
			continue
		}
		if _, err := conn.WriteTo(response.Raw, remote); err != nil {
			log.Printf("H2_WEBRTC_TEST_SERVER_STUN send_error=%v", err)
		}
	}
}

func (s *server) handleOffer(w http.ResponseWriter, r *http.Request) {
	if r.Method == http.MethodGet || r.Method == http.MethodHead {
		w.WriteHeader(http.StatusNoContent)
		return
	}
	if r.Method != http.MethodPost {
		http.Error(w, "method not allowed", http.StatusMethodNotAllowed)
		return
	}

	r.Body = http.MaxBytesReader(w, r.Body, 256*1024)
	body, err := io.ReadAll(r.Body)
	if err != nil {
		http.Error(w, "read offer", http.StatusBadRequest)
		return
	}
	offer := string(body)
	if strings.TrimSpace(offer) == "" {
		http.Error(w, "empty offer", http.StatusBadRequest)
		return
	}
	if r.Header.Get("X-H2-Relay-Only") == "1" {
		offer, err = relayOnlyOffer(offer)
		if err != nil {
			http.Error(w, err.Error(), http.StatusBadRequest)
			return
		}
	}
	log.Printf(
		"H2_WEBRTC_TEST_SERVER_OFFER mode=%s remote=%s len=%d candidates=%q",
		s.iceMode, r.RemoteAddr, len(offer), sdpCandidateSummary(offer),
	)

	reverseChannels := 0
	if value := r.Header.Get("X-H2-Reverse-Channels"); value != "" {
		if value != "0" && value != "3" {
			http.Error(w, "reverse channel count must be 0 or 3", http.StatusBadRequest)
			return
		}
		if value == "3" {
			reverseChannels = 3
		}
	}
	answer, item, err := s.createAnswer(r.Context(), offer, reverseChannels)
	if err != nil {
		log.Printf("H2_WEBRTC_TEST_SERVER_OFFER_ERROR remote=%s error=%v", r.RemoteAddr, err)
		http.Error(w, err.Error(), http.StatusBadRequest)
		return
	}
	log.Printf(
		"H2_WEBRTC_TEST_SERVER_ANSWER mode=%s remote=%s len=%d candidates=%q",
		s.iceMode, r.RemoteAddr, len(answer), sdpCandidateSummary(answer),
	)
	sessionID := s.track(item)

	w.Header().Set("Content-Type", "application/sdp")
	w.Header().Set("Content-Length", fmt.Sprint(len(answer)))
	w.Header().Set("X-H2-Session-ID", sessionID)
	_, _ = io.WriteString(w, answer)
}

func relayOnlyOffer(offer string) (string, error) {
	var filtered strings.Builder
	relayCandidates := 0
	for len(offer) != 0 {
		lineEnd := strings.IndexByte(offer, '\n')
		line := offer
		if lineEnd >= 0 {
			line = offer[:lineEnd+1]
			offer = offer[lineEnd+1:]
		} else {
			offer = ""
		}
		candidate := strings.TrimSuffix(strings.TrimSuffix(line, "\n"), "\r")
		if strings.HasPrefix(candidate, "a=candidate:") {
			if !strings.Contains(candidate, " typ relay") {
				continue
			}
			relayCandidates++
		}
		filtered.WriteString(line)
	}
	if relayCandidates == 0 {
		return "", fmt.Errorf("relay-only offer has no relay candidate")
	}
	return filtered.String(), nil
}

func (s *server) handleSession(w http.ResponseWriter, r *http.Request) {
	parts := strings.Split(strings.TrimPrefix(r.URL.Path, "/session/"), "/")
	if len(parts) != 2 || parts[0] == "" {
		http.NotFound(w, r)
		return
	}
	if parts[1] == "channel-stats" {
		if r.Method != http.MethodGet {
			http.Error(w, "method not allowed", http.StatusMethodNotAllowed)
			return
		}
		s.mu.Lock()
		item := s.sessions[parts[0]]
		s.mu.Unlock()
		if item == nil {
			http.NotFound(w, r)
			return
		}
		w.Header().Set("Content-Type", "application/json")
		_ = json.NewEncoder(w).Encode(item.channels.snapshot())
		return
	}
	if parts[1] == "ice-pair" {
		if r.Method != http.MethodGet {
			http.Error(w, "method not allowed", http.StatusMethodNotAllowed)
			return
		}
		s.mu.Lock()
		item := s.sessions[parts[0]]
		s.mu.Unlock()
		if item == nil {
			http.NotFound(w, r)
			return
		}
		transport := item.pc.SCTP().Transport().ICETransport()
		pair, err := transport.GetSelectedCandidatePair()
		if err != nil {
			http.Error(w, err.Error(), http.StatusInternalServerError)
			return
		}
		if pair == nil {
			http.Error(w, "selected ICE pair unavailable", http.StatusConflict)
			return
		}
		snapshot := icePairSnapshot{
			Mode:           s.iceMode,
			LocalProtocol:  pair.Local.Protocol.String(),
			RemoteProtocol: pair.Remote.Protocol.String(),
			LocalType:      pair.Local.Typ.String(),
			RemoteType:     pair.Remote.Typ.String(),
			LocalTCPType:   pair.Local.TCPType,
			RemoteTCPType:  pair.Remote.TCPType,
			UDPDrops:       s.udpDrops.Load(),
		}
		log.Printf(
			"H2_WEBRTC_TEST_SERVER_PAIR mode=%s local=%s/%s/%s remote=%s/%s/%s udp_drops=%d",
			snapshot.Mode,
			snapshot.LocalProtocol,
			snapshot.LocalType,
			snapshot.LocalTCPType,
			snapshot.RemoteProtocol,
			snapshot.RemoteType,
			snapshot.RemoteTCPType,
			snapshot.UDPDrops,
		)
		w.Header().Set("Content-Type", "application/json")
		_ = json.NewEncoder(w).Encode(snapshot)
		return
	}
	if parts[1] != "close" {
		http.NotFound(w, r)
		return
	}
	if r.Method != http.MethodPost {
		http.Error(w, "method not allowed", http.StatusMethodNotAllowed)
		return
	}
	s.mu.Lock()
	item := s.sessions[parts[0]]
	delete(s.sessions, parts[0])
	s.mu.Unlock()
	if item == nil {
		http.NotFound(w, r)
		return
	}
	if err := item.pc.Close(); err != nil {
		http.Error(w, err.Error(), http.StatusInternalServerError)
		return
	}
	w.WriteHeader(http.StatusNoContent)
}

func sdpCandidateSummary(sdp string) string {
	var parts []string
	for _, line := range strings.Split(sdp, "\r\n") {
		fields := strings.Fields(line)
		if len(fields) < 8 || !strings.HasPrefix(fields[0], "a=candidate:") {
			continue
		}
		family := "name"
		if ip := net.ParseIP(fields[4]); ip != nil {
			if ip.To4() != nil {
				family = "ipv4"
			} else {
				family = "ipv6"
			}
		}
		tcpType := "-"
		for index := 8; index+1 < len(fields); index += 2 {
			if strings.EqualFold(fields[index], "tcptype") {
				tcpType = strings.ToLower(fields[index+1])
				break
			}
		}
		parts = append(parts, fmt.Sprintf(
			"%s/%s:%s/%s/%s",
			strings.ToLower(fields[2]), family, fields[5],
			strings.ToLower(fields[7]), tcpType,
		))
	}
	return fmt.Sprintf("count=%d %s", len(parts), strings.Join(parts, ","))
}

func (s *server) createAnswer(
	ctx context.Context,
	offerSDP string,
	reverseChannels int,
) (string, *session, error) {
	if s.api == nil {
		return "", nil, fmt.Errorf("ICE API is not configured")
	}
	pc, err := s.api.NewPeerConnection(webrtc.Configuration{})
	if err != nil {
		return "", nil, err
	}
	item := &session{pc: pc}
	var reverseOnce sync.Once
	startReverseChannels := func() {
		if reverseChannels == 0 || item.channels.opened.Load() != 3 {
			return
		}
		reverseOnce.Do(func() {
			go func() {
				for index := 0; index < reverseChannels; index++ {
					label := fmt.Sprintf("server/reverse/%d", index)
					dc, err := pc.CreateDataChannel(label, nil)
					if err != nil {
						log.Printf(
							"H2_WEBRTC_TEST_SERVER_REVERSE create=%d error=%v",
							index, err,
						)
						return
					}
					attachReverseDataChannel(dc, &item.channels, index)
				}
			}()
		})
	}

	audioTrack, err := webrtc.NewTrackLocalStaticRTP(
		webrtc.RTPCodecCapability{
			MimeType:  webrtc.MimeTypeOpus,
			ClockRate: 48000,
			Channels:  2,
		},
		"audio",
		"h2-webrtc-compat",
	)
	if err != nil {
		_ = pc.Close()
		return "", nil, fmt.Errorf("create Opus track: %w", err)
	}
	sender, err := pc.AddTrack(audioTrack)
	if err != nil {
		_ = pc.Close()
		return "", nil, fmt.Errorf("add Opus track: %w", err)
	}
	go func() {
		buffer := make([]byte, 1500)
		for {
			if _, _, err := sender.Read(buffer); err != nil {
				return
			}
		}
	}()

	pc.OnDataChannel(func(dc *webrtc.DataChannel) {
		attachDataChannel(dc, &item.channels, startReverseChannels)
	})
	pc.OnTrack(func(remote *webrtc.TrackRemote, _ *webrtc.RTPReceiver) {
		if remote.Codec().MimeType != webrtc.MimeTypeOpus {
			return
		}
		go echoOpus(remote, audioTrack)
	})

	offer := webrtc.SessionDescription{
		Type: webrtc.SDPTypeOffer,
		SDP:  offerSDP,
	}
	if err := pc.SetRemoteDescription(offer); err != nil {
		_ = pc.Close()
		return "", nil, fmt.Errorf("set remote offer: %w", err)
	}
	answer, err := pc.CreateAnswer(nil)
	if err != nil {
		_ = pc.Close()
		return "", nil, fmt.Errorf("create answer: %w", err)
	}
	gatherComplete := webrtc.GatheringCompletePromise(pc)
	if err := pc.SetLocalDescription(answer); err != nil {
		_ = pc.Close()
		return "", nil, fmt.Errorf("set local answer: %w", err)
	}

	select {
	case <-gatherComplete:
	case <-ctx.Done():
		_ = pc.Close()
		return "", nil, ctx.Err()
	case <-time.After(10 * time.Second):
		_ = pc.Close()
		return "", nil, fmt.Errorf("ice gathering timeout")
	}

	local := pc.LocalDescription()
	if local == nil || local.SDP == "" {
		_ = pc.Close()
		return "", nil, fmt.Errorf("empty local answer")
	}
	return local.SDP, item, nil
}

func echoOpus(remote *webrtc.TrackRemote, local *webrtc.TrackLocalStaticRTP) {
	var sequence uint16
	for {
		packet, _, err := remote.ReadRTP()
		if err != nil {
			return
		}
		payload := append([]byte(nil), packet.Payload...)
		echo := &rtp.Packet{
			Header: rtp.Header{
				Version:        2,
				Marker:         packet.Marker,
				PayloadType:    111,
				SequenceNumber: sequence,
				Timestamp:      packet.Timestamp,
			},
			Payload: payload,
		}
		sequence++
		if err := local.WriteRTP(echo); err != nil {
			return
		}
	}
}

func attachDataChannel(
	dc *webrtc.DataChannel,
	stats *channelStats,
	afterOpen func(),
) {
	label := dc.Label()
	var closeOnce sync.Once
	recordClose := func() { closeOnce.Do(stats.onClose) }
	stats.onCreated()
	dc.OnOpen(func() {
		stats.onOpen()
		if afterOpen != nil {
			afterOpen()
		}
		raw, err := dc.Detach()
		if err != nil {
			log.Printf("H2_WEBRTC_TEST_SERVER_CHANNEL label=%s detach_error=%v", label, err)
			_ = dc.Close()
			return
		}
		go echoDetachedDataChannel(label, dc, raw, recordClose)
	})
	dc.OnClose(recordClose)
}

func echoDetachedDataChannel(
	label string,
	dc *webrtc.DataChannel,
	raw datachannel.ReadWriteCloser,
	recordClose func(),
) {
	defer recordClose()
	defer raw.Close()
	buffer := make([]byte, 64*1024)
	performance := &performanceChannel{raw: raw, dc: dc}
	for {
		n, isString, err := raw.ReadDataChannel(buffer)
		if err != nil {
			return
		}
		if !isString && performance.handle(buffer[:n]) {
			continue
		}
		prefix := "server-echo-binary:"
		if isString {
			prefix = "server-echo-text:"
		}
		if strings.HasPrefix(label, "giznet/v1/service/") {
			prefix = "server-service-ack-binary:"
			if isString {
				prefix = "server-service-ack-text:"
			}
		}
		payload := append([]byte(prefix), buffer[:n]...)
		if _, err := raw.WriteDataChannel(payload, isString); err != nil {
			log.Printf("H2_WEBRTC_TEST_SERVER_CHANNEL label=%s send_echo_error=%v", label, err)
			return
		}
		if label == "giznet/v1/service/0" && !isString {
			return
		}
	}
}

type performanceChannel struct {
	raw            datachannel.ReadWriteCloser
	dc             *webrtc.DataChannel
	writeMu        sync.Mutex
	downloadMu     sync.Mutex
	downloadActive bool
	uploaded       uint32
}

const (
	performanceBufferedAmountHigh = 256 * 1024
	performanceBufferedAmountLow  = 128 * 1024
)

func performanceHeader(op byte, flags byte, sequence uint16, total uint32, offset uint32) []byte {
	header := make([]byte, performanceHeaderSize)
	copy(header, performanceMagic[:])
	header[4] = op
	header[5] = flags
	binary.BigEndian.PutUint16(header[6:8], sequence)
	binary.BigEndian.PutUint32(header[8:12], total)
	binary.BigEndian.PutUint32(header[12:16], offset)
	return header
}

func (channel *performanceChannel) write(payload []byte) error {
	channel.writeMu.Lock()
	defer channel.writeMu.Unlock()
	_, err := channel.raw.WriteDataChannel(payload, false)
	return err
}

func (channel *performanceChannel) handle(message []byte) bool {
	if len(message) < performanceHeaderSize ||
		!bytes.Equal(message[:4], performanceMagic[:]) {
		return false
	}
	op := message[4]
	flags := message[5]
	sequence := binary.BigEndian.Uint16(message[6:8])
	total := binary.BigEndian.Uint32(message[8:12])
	offset := binary.BigEndian.Uint32(message[12:16])
	switch op {
	case performanceRequest, performanceEcho:
		if err := channel.write(append([]byte(nil), message...)); err != nil {
			log.Printf("H2_WEBRTC_TEST_SERVER_PERF op=%d write_error=%v", op, err)
		}
	case performanceUpload:
		payloadLen := uint32(len(message) - performanceHeaderSize)
		if offset != channel.uploaded || channel.uploaded > total ||
			payloadLen > total-channel.uploaded {
			log.Printf("H2_WEBRTC_TEST_SERVER_PERF upload_invalid offset=%d uploaded=%d total=%d payload=%d", offset, channel.uploaded, total, payloadLen)
			return true
		}
		for _, value := range message[performanceHeaderSize:] {
			if value != 0xa5 {
				log.Printf("H2_WEBRTC_TEST_SERVER_PERF upload_payload_invalid offset=%d", offset)
				return true
			}
		}
		channel.uploaded += payloadLen
		if flags&performanceLast != 0 {
			if channel.uploaded != total {
				log.Printf("H2_WEBRTC_TEST_SERVER_PERF upload_incomplete uploaded=%d total=%d", channel.uploaded, total)
				return true
			}
			channel.uploaded = 0
			if err := channel.write(performanceHeader(
				performanceUpload, performanceLast, sequence, total, total,
			)); err != nil {
				log.Printf("H2_WEBRTC_TEST_SERVER_PERF upload_ack_error=%v", err)
			}
		}
	case performanceDownload:
		if flags != 0 || sequence != 0 || total != performanceTransferSize || offset != 0 {
			log.Printf("H2_WEBRTC_TEST_SERVER_PERF download_invalid flags=%d sequence=%d total=%d offset=%d", flags, sequence, total, offset)
			return true
		}
		channel.downloadMu.Lock()
		if channel.downloadActive {
			channel.downloadMu.Unlock()
			log.Printf("H2_WEBRTC_TEST_SERVER_PERF download_busy")
			return true
		}
		channel.downloadActive = true
		channel.downloadMu.Unlock()
		go func() {
			defer func() {
				channel.downloadMu.Lock()
				channel.downloadActive = false
				channel.downloadMu.Unlock()
			}()
			channel.sendDownload(total)
		}()
	default:
		log.Printf("H2_WEBRTC_TEST_SERVER_PERF unknown_op=%d", op)
	}
	return true
}

func (channel *performanceChannel) sendDownload(total uint32) {
	started := time.Now()
	nextProgress := uint32(1024 * 1024)
	log.Printf("H2_WEBRTC_TEST_SERVER_PERF download_start total=%d", total)
	var offset uint32
	var sequence uint16
	for offset < total {
		payloadLen := uint32(performanceChunkSize)
		if total-offset < payloadLen {
			payloadLen = total - offset
		}
		flags := byte(0)
		if offset+payloadLen == total {
			flags = performanceLast
		}
		message := performanceHeader(
			performanceDownload, flags, sequence, total, offset,
		)
		for index := uint32(0); index < payloadLen; index++ {
			message = append(message, byte(offset+index))
		}
		if err := channel.write(message); err != nil {
			log.Printf("H2_WEBRTC_TEST_SERVER_PERF download_error=%v", err)
			return
		}
		if channel.dc != nil && channel.dc.Transport() != nil &&
			channel.dc.Transport().BufferedAmount() > performanceBufferedAmountHigh {
			deadline := time.Now().Add(60 * time.Second)
			for channel.dc.Transport().BufferedAmount() >
				performanceBufferedAmountLow {
				if time.Now().After(deadline) {
					log.Printf(
						"H2_WEBRTC_TEST_SERVER_PERF download_backpressure_timeout buffered=%d",
						channel.dc.Transport().BufferedAmount(),
					)
					return
				}
				time.Sleep(time.Millisecond)
			}
		}
		offset += payloadLen
		sequence++
		if offset >= nextProgress || offset == total {
			elapsed := time.Since(started)
			bytesPerSecond := uint64(0)
			bufferedAmount := 0
			var congestionWindow uint32
			var receiverWindow uint32
			var mtu uint32
			var srttMilliseconds float64
			if elapsed > 0 {
				bytesPerSecond = uint64(offset) * uint64(time.Second) /
					uint64(elapsed)
			}
			if channel.dc != nil && channel.dc.Transport() != nil {
				transport := channel.dc.Transport()
				bufferedAmount = transport.BufferedAmount()
				stats := transport.Stats()
				congestionWindow = stats.CongestionWindow
				receiverWindow = stats.ReceiverWindow
				mtu = stats.MTU
				srttMilliseconds = stats.SmoothedRoundTripTime * 1000
			}
			log.Printf(
				"H2_WEBRTC_TEST_SERVER_PERF download_progress bytes=%d total=%d elapsed_ms=%d Bps=%d buffered=%d cwnd=%d rwnd=%d mtu=%d srtt_ms=%.3f",
				offset, total, elapsed.Milliseconds(), bytesPerSecond,
				bufferedAmount, congestionWindow, receiverWindow, mtu,
				srttMilliseconds,
			)
			nextProgress += 1024 * 1024
		}
	}
}

func attachReverseDataChannel(
	dc *webrtc.DataChannel,
	stats *channelStats,
	index int,
) {
	var closeOnce sync.Once
	recordClose := func() { closeOnce.Do(stats.onClose) }
	stats.onCreated()
	dc.OnOpen(func() {
		stats.onOpen()
		raw, err := dc.Detach()
		if err != nil {
			log.Printf("H2_WEBRTC_TEST_SERVER_REVERSE label=%s detach_error=%v", dc.Label(), err)
			_ = dc.Close()
			return
		}
		go handleDetachedReverseDataChannel(
			dc.Label(), raw, stats, index, recordClose,
		)
	})
	dc.OnClose(recordClose)
}

func handleDetachedReverseDataChannel(
	label string,
	raw datachannel.ReadWriteCloser,
	stats *channelStats,
	index int,
	recordClose func(),
) {
	defer recordClose()
	defer raw.Close()
	buffer := make([]byte, 1024)
	for {
		n, isString, err := raw.ReadDataChannel(buffer)
		if err != nil {
			return
		}
		expected := fmt.Sprintf("client-reverse-probe:%d", index)
		if isString && string(buffer[:n]) == expected {
			stats.reverseReplies.Or(1 << index)
			ack := []byte(fmt.Sprintf("server-reverse-ack:%d", index))
			if _, err := raw.WriteDataChannel(ack, true); err != nil {
				log.Printf("H2_WEBRTC_TEST_SERVER_REVERSE label=%s send_ack_error=%v", label, err)
				return
			}
			continue
		}
		log.Printf(
			"H2_WEBRTC_TEST_SERVER_REVERSE label=%s unexpected_reply text=%t len=%d",
			label, isString, n,
		)
	}
}

func (s *server) track(item *session) string {
	s.mu.Lock()
	defer s.mu.Unlock()
	s.nextSession++
	id := strconv.FormatUint(s.nextSession, 10)
	item.id = id
	item.createdAt = time.Now()
	s.sessions[id] = item
	return id
}

func (s *server) cleanupLoop(ctx context.Context) {
	ticker := time.NewTicker(15 * time.Second)
	defer ticker.Stop()
	for {
		select {
		case <-ctx.Done():
			return
		case <-ticker.C:
			s.cleanup()
		}
	}
}

func (s *server) cleanup() {
	now := time.Now()
	s.mu.Lock()
	defer s.mu.Unlock()
	for id, item := range s.sessions {
		if now.Sub(item.createdAt) >= sessionTTL {
			_ = item.pc.Close()
			delete(s.sessions, id)
		}
	}
}

func (s *server) closeAll() {
	s.mu.Lock()
	sessions := s.sessions
	s.sessions = make(map[string]*session)
	s.mu.Unlock()
	for _, item := range sessions {
		_ = item.pc.Close()
	}
}
