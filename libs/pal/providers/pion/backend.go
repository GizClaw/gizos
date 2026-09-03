package pion

import (
	"errors"
	"fmt"
	"strings"
	"sync"
	"sync/atomic"
	"time"

	"github.com/pion/webrtc/v4"
	"github.com/pion/webrtc/v4/pkg/media"
)

const (
	PeerNew          = 0
	PeerConnecting   = 1
	PeerConnected    = 2
	PeerDisconnected = 3
	PeerFailed       = 4
	PeerClosed       = 5

	ChannelOpen   = 1
	ChannelClosed = 2
	ChannelError  = 3

	maxQueuedEvents   = 4096
	maxQueuedOpus     = 64
	maxQueuedBytes    = 4 * 1024 * 1024
	maxBufferedAmount = 256 * 1024
	maxChannels       = 64
)

type EventKind uint8

const (
	EventPeerState EventKind = iota
	EventChannelOpen
	EventChannelState
	EventChannelMessage
	EventOpusFrame
	EventWritable
)

type Event struct {
	Kind        EventKind
	ChannelKey  uint64
	Label       string
	HasStreamID bool
	StreamID    uint16
	Ordered     bool
	Reliable    bool
	Remote      bool
	State       int
	Data        []byte
	IsText      bool
}

type Backend struct {
	deliveryMu       sync.Mutex
	pending          *Event
	mu               sync.Mutex
	pc               *webrtc.PeerConnection
	opusTrack        *webrtc.TrackLocalStaticSample
	channels         map[uint64]*channel
	events           []Event
	queuedEventBytes int
	ready            chan struct{}
	closed           bool
	overflow         bool
	remoteKeySerial  atomic.Uint64
}

type channel struct {
	dc       *webrtc.DataChannel
	terminal sync.Once
}

func New() (*Backend, error) {
	pc, err := webrtc.NewPeerConnection(webrtc.Configuration{})
	if err != nil {
		return nil, err
	}
	track, err := webrtc.NewTrackLocalStaticSample(webrtc.RTPCodecCapability{
		MimeType:  webrtc.MimeTypeOpus,
		ClockRate: 48000,
		Channels:  2,
	}, "gizclaw-opus", "gizclaw")
	if err != nil {
		_ = pc.Close()
		return nil, err
	}
	transceiver, err := pc.AddTransceiverFromTrack(track, webrtc.RTPTransceiverInit{
		Direction: webrtc.RTPTransceiverDirectionSendrecv,
	})
	if err != nil {
		_ = pc.Close()
		return nil, err
	}
	b := &Backend{
		pc:        pc,
		opusTrack: track,
		channels:  make(map[uint64]*channel),
		ready:     make(chan struct{}, 1),
	}
	b.remoteKeySerial.Store(1 << 63)
	go func() {
		for {
			if _, _, err := transceiver.Sender().ReadRTCP(); err != nil {
				return
			}
		}
	}()
	pc.OnConnectionStateChange(func(state webrtc.PeerConnectionState) {
		b.enqueue(Event{Kind: EventPeerState, State: mapPeerState(state)})
	})
	pc.OnDataChannel(b.acceptRemoteChannel)
	pc.OnTrack(func(remote *webrtc.TrackRemote, _ *webrtc.RTPReceiver) {
		if !strings.EqualFold(remote.Codec().MimeType, webrtc.MimeTypeOpus) {
			return
		}
		go b.forwardOpus(remote)
	})
	return b, nil
}

func (b *Backend) AddICEServer(url, username, credential string) error {
	b.mu.Lock()
	defer b.mu.Unlock()
	if b.closed || b.pc == nil {
		return ErrClosed
	}
	configuration := b.pc.GetConfiguration()
	configuration.ICEServers = append(configuration.ICEServers, webrtc.ICEServer{
		URLs:       []string{url},
		Username:   username,
		Credential: credential,
	})
	return b.pc.SetConfiguration(configuration)
}

func (b *Backend) StartOffer() (string, error) {
	b.mu.Lock()
	pc := b.pc
	closed := b.closed
	b.mu.Unlock()
	if closed || pc == nil {
		return "", ErrClosed
	}
	gatheringDone := webrtc.GatheringCompletePromise(pc)
	offer, err := pc.CreateOffer(nil)
	if err != nil {
		return "", err
	}
	if err := pc.SetLocalDescription(offer); err != nil {
		return "", err
	}
	<-gatheringDone
	local := pc.LocalDescription()
	if local == nil {
		return "", errors.New("missing local description")
	}
	return local.SDP, nil
}

func (b *Backend) SetRemoteAnswer(sdp string) error {
	b.mu.Lock()
	pc := b.pc
	closed := b.closed
	b.mu.Unlock()
	if closed || pc == nil {
		return ErrClosed
	}
	return pc.SetRemoteDescription(webrtc.SessionDescription{
		Type: webrtc.SDPTypeAnswer,
		SDP:  sdp,
	})
}

func (b *Backend) CreateDataChannel(
	key uint64,
	label string,
	hasStreamID bool,
	streamID uint16,
	ordered bool,
	reliable bool,
) error {
	b.mu.Lock()
	pc := b.pc
	if b.closed || pc == nil {
		b.mu.Unlock()
		return ErrClosed
	}
	if _, exists := b.channels[key]; exists {
		b.mu.Unlock()
		return fmt.Errorf("%w: duplicate channel key %d", ErrInvalidState, key)
	}
	if len(b.channels) >= maxChannels {
		b.mu.Unlock()
		return ErrNoSpace
	}
	state := &channel{}
	b.channels[key] = state
	b.mu.Unlock()

	init := &webrtc.DataChannelInit{Ordered: &ordered}
	if hasStreamID {
		init.ID = &streamID
	}
	if !reliable {
		maxRetransmits := uint16(0)
		init.MaxRetransmits = &maxRetransmits
	}
	dc, err := pc.CreateDataChannel(label, init)
	if err != nil {
		b.mu.Lock()
		if b.channels[key] == state {
			delete(b.channels, key)
		}
		b.mu.Unlock()
		return err
	}
	b.mu.Lock()
	if b.closed {
		delete(b.channels, key)
		b.mu.Unlock()
		_ = dc.Close()
		return ErrClosed
	}
	state.dc = dc
	b.mu.Unlock()
	b.attachChannel(key, state, label, ordered, reliable, false)
	return nil
}

// Dispatch delivers at most 64 events per worker slice. A rejected event stays
// owned by the backend and is retried before any later event. Neither the PAL
// event queue nor Track backpressure may silently consume received data.
func (b *Backend) Dispatch(deliver func(Event) error) (bool, error) {
	b.deliveryMu.Lock()
	defer b.deliveryMu.Unlock()
	for n := 0; n < 64; n++ {
		b.mu.Lock()
		if b.closed {
			b.pending = nil
			b.mu.Unlock()
			return false, ErrClosed
		}
		if b.overflow {
			b.overflow = false
			b.mu.Unlock()
			return true, nil
		}
		if b.pending == nil && len(b.events) != 0 {
			event := b.events[0]
			b.events[0] = Event{}
			b.events = b.events[1:]
			b.queuedEventBytes -= len(event.Data)
			b.pending = &event
		}
		b.mu.Unlock()
		if b.pending == nil {
			return false, nil
		}
		if err := deliver(*b.pending); err != nil {
			return false, err
		}
		b.pending = nil
	}
	return false, nil
}

func (b *Backend) Send(key uint64, data []byte, isText bool) error {
	state, err := b.channel(key)
	if err != nil {
		return err
	}
	if len(data) > maxBufferedAmount {
		return ErrNoSpace
	}
	if exceedsBufferedAmount(state.dc.BufferedAmount(), len(data)) {
		return ErrWouldBlock
	}
	if isText {
		return state.dc.SendText(string(data))
	}
	return state.dc.Send(data)
}

func (b *Backend) SendOpus(opus []byte) error {
	b.mu.Lock()
	track := b.opusTrack
	closed := b.closed
	b.mu.Unlock()
	if closed || track == nil {
		return ErrClosed
	}
	return track.WriteSample(media.Sample{
		Data:     append([]byte(nil), opus...),
		Duration: opusDuration(opus),
	})
}

func (b *Backend) CloseChannel(key uint64) {
	b.mu.Lock()
	state := b.channels[key]
	delete(b.channels, key)
	b.mu.Unlock()
	if state != nil && state.dc != nil {
		state.terminal.Do(func() {})
		_ = state.dc.Close()
	}
}

func (b *Backend) Close() {
	b.mu.Lock()
	if b.closed {
		b.mu.Unlock()
		return
	}
	b.closed = true
	pc := b.pc
	b.pc = nil
	b.opusTrack = nil
	b.channels = nil
	b.events = nil
	b.queuedEventBytes = 0
	b.mu.Unlock()
	select {
	case b.ready <- struct{}{}:
	default:
	}
	if pc != nil {
		_ = pc.Close()
	}
}

var ErrWouldBlock = errors.New("data channel send would block")
var ErrNoSpace = errors.New("data channel capacity exhausted")
var ErrClosed = errors.New("peer or data channel is closed")
var ErrInvalidState = errors.New("invalid WebRTC state")

func (b *Backend) channel(key uint64) (*channel, error) {
	b.mu.Lock()
	defer b.mu.Unlock()
	if b.closed {
		return nil, ErrClosed
	}
	state := b.channels[key]
	if state == nil || state.dc == nil {
		return nil, fmt.Errorf("%w: channel %d", ErrClosed, key)
	}
	return state, nil
}

func (b *Backend) acceptRemoteChannel(dc *webrtc.DataChannel) {
	if dc == nil {
		return
	}
	key := b.remoteKeySerial.Add(1)
	state := &channel{dc: dc}
	b.mu.Lock()
	if b.closed {
		b.mu.Unlock()
		_ = dc.Close()
		return
	}
	if len(b.channels) >= maxChannels {
		b.overflow = true
		b.mu.Unlock()
		_ = dc.Close()
		b.notifyReady()
		return
	}
	b.channels[key] = state
	b.mu.Unlock()
	b.attachChannel(
		key,
		state,
		dc.Label(),
		dc.Ordered(),
		dc.MaxRetransmits() == nil && dc.MaxPacketLifeTime() == nil,
		true,
	)
}

func (b *Backend) attachChannel(
	key uint64,
	state *channel,
	label string,
	ordered bool,
	reliable bool,
	remote bool,
) {
	// At zero, even the largest accepted message can be retried. A nonzero
	// threshold can notify too early and leave a maximum-sized send stranded.
	state.dc.SetBufferedAmountLowThreshold(0)
	state.dc.OnBufferedAmountLow(func() {
		b.enqueue(Event{Kind: EventWritable, ChannelKey: key})
	})
	state.dc.OnOpen(func() {
		b.enqueue(Event{
			Kind:        EventChannelOpen,
			ChannelKey:  key,
			Label:       label,
			HasStreamID: state.dc.ID() != nil,
			StreamID:    valueOrZero(state.dc.ID()),
			Ordered:     ordered,
			Reliable:    reliable,
			Remote:      remote,
		})
	})
	state.dc.OnMessage(func(message webrtc.DataChannelMessage) {
		b.enqueue(Event{
			Kind:       EventChannelMessage,
			ChannelKey: key,
			Data:       append([]byte(nil), message.Data...),
			IsText:     message.IsString,
		})
	})
	state.dc.OnError(func(_ error) {
		b.terminal(key, state, ChannelError)
	})
	state.dc.OnClose(func() {
		b.terminal(key, state, ChannelClosed)
	})
}

func (b *Backend) terminal(key uint64, state *channel, terminalState int) {
	state.terminal.Do(func() {
		b.mu.Lock()
		if b.channels[key] == state {
			delete(b.channels, key)
		}
		b.mu.Unlock()
		b.enqueue(Event{Kind: EventChannelState, ChannelKey: key, State: terminalState})
	})
}

func (b *Backend) forwardOpus(track *webrtc.TrackRemote) {
	for {
		packet, _, err := track.ReadRTP()
		if err != nil {
			return
		}
		if len(packet.Payload) != 0 {
			b.enqueue(Event{Kind: EventOpusFrame, Data: append([]byte(nil), packet.Payload...)})
		}
	}
}

func (b *Backend) enqueue(event Event) {
	b.mu.Lock()
	if b.closed {
		b.mu.Unlock()
		return
	}
	if event.Kind == EventOpusFrame {
		opusCount := 0
		for index := range b.events {
			if b.events[index].Kind == EventOpusFrame {
				opusCount++
			}
		}
		if opusCount >= maxQueuedOpus {
			b.overflow = true
			b.mu.Unlock()
			b.notifyReady()
			return
		}
	}
	eventBytes := len(event.Data)
	if eventBytes > maxQueuedBytes ||
		b.queuedEventBytes > maxQueuedBytes-eventBytes {
		b.overflow = true
		b.mu.Unlock()
		b.notifyReady()
		return
	}
	if len(b.events) >= maxQueuedEvents {
		b.overflow = true
		b.mu.Unlock()
		b.notifyReady()
		return
	}
	b.events = append(b.events, event)
	b.queuedEventBytes += eventBytes
	b.mu.Unlock()
	b.notifyReady()
}

func (b *Backend) notifyReady() {
	select {
	case b.ready <- struct{}{}:
	default:
	}
}

func exceedsBufferedAmount(buffered uint64, payloadLen int) bool {
	return payloadLen > maxBufferedAmount ||
		buffered > uint64(maxBufferedAmount-payloadLen)
}

func mapPeerState(state webrtc.PeerConnectionState) int {
	switch state {
	case webrtc.PeerConnectionStateNew:
		return PeerNew
	case webrtc.PeerConnectionStateConnecting:
		return PeerConnecting
	case webrtc.PeerConnectionStateConnected:
		return PeerConnected
	case webrtc.PeerConnectionStateDisconnected:
		return PeerDisconnected
	case webrtc.PeerConnectionStateFailed:
		return PeerFailed
	case webrtc.PeerConnectionStateClosed:
		return PeerClosed
	default:
		return PeerFailed
	}
}

func valueOrZero(value *uint16) uint16 {
	if value == nil {
		return 0
	}
	return *value
}

func opusDuration(packet []byte) time.Duration {
	if len(packet) == 0 {
		return 20 * time.Millisecond
	}
	configuration := packet[0] >> 3
	var ticks uint32
	switch {
	case configuration < 12:
		switch configuration % 4 {
		case 0:
			ticks = 480
		case 1:
			ticks = 960
		case 2:
			ticks = 1920
		default:
			ticks = 2880
		}
	case configuration < 16:
		if configuration%2 == 0 {
			ticks = 480
		} else {
			ticks = 960
		}
	default:
		switch configuration % 4 {
		case 0:
			ticks = 120
		case 1:
			ticks = 240
		case 2:
			ticks = 480
		default:
			ticks = 960
		}
	}
	frames := 1
	switch packet[0] & 3 {
	case 1, 2:
		frames = 2
	case 3:
		if len(packet) > 1 {
			frames = int(packet[1] & 0x3f)
		}
	}
	if frames <= 0 {
		frames = 1
	}
	return time.Duration(ticks*uint32(frames)) * time.Second / 48000
}
