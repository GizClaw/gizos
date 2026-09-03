package pion

import (
	"bytes"
	"errors"
	"testing"
	"time"

	"github.com/pion/webrtc/v4"
	"github.com/pion/webrtc/v4/pkg/media"
)

// Tests collect events through the same retry-aware dispatch used by C.
func pollBackend(backend *Backend, timeoutMS int) ([]Event, bool) {
	deadline := time.Now().Add(time.Duration(timeoutMS) * time.Millisecond)
	var events []Event
	overflow := false
	for {
		before := len(events)
		full, err := backend.Dispatch(func(event Event) error {
			events = append(events, event)
			return nil
		})
		overflow = overflow || full
		if err != nil {
			return events, overflow
		}
		if full || len(events) != before {
			continue // drain bounded worker slices, including after overflow
		}
		if len(events) != 0 || overflow || timeoutMS <= 0 || !time.Now().Before(deadline) {
			return events, overflow
		}
		timer := time.NewTimer(time.Until(deadline))
		select {
		case <-backend.ready:
			timer.Stop()
		case <-timer.C:
			return events, overflow
		}
	}
}

func TestDispatchRetainsBackpressuredEventAndOrder(t *testing.T) {
	backend := &Backend{ready: make(chan struct{}, 1)}
	backend.enqueue(Event{Kind: EventOpusFrame, Data: []byte{1}})
	backend.enqueue(Event{Kind: EventChannelMessage, Data: []byte{2}})
	for attempt := 0; attempt < 3; attempt++ {
		calls := 0
		overflow, err := backend.Dispatch(func(event Event) error {
			calls++
			if !bytes.Equal(event.Data, []byte{1}) {
				t.Fatalf("retry reordered/lost payload: %v", event.Data)
			}
			return ErrWouldBlock
		})
		if overflow || !errors.Is(err, ErrWouldBlock) || calls != 1 {
			t.Fatalf("retry: overflow=%v err=%v calls=%d", overflow, err, calls)
		}
	}
	var got []byte
	overflow, err := backend.Dispatch(func(event Event) error {
		got = append(got, event.Data...)
		return nil
	})
	if overflow || err != nil || !bytes.Equal(got, []byte{1, 2}) || backend.pending != nil {
		t.Fatalf("drain: got=%v overflow=%v err=%v", got, overflow, err)
	}
}

func TestDispatchIsBoundedAndReportsOverflow(t *testing.T) {
	backend := &Backend{ready: make(chan struct{}, 1)}
	for n := 0; n < maxQueuedEvents; n++ {
		backend.enqueue(Event{Kind: EventChannelMessage})
	}
	calls := 0
	deliver := func(Event) error { calls++; return nil }
	if overflow, err := backend.Dispatch(deliver); overflow || err != nil || calls != 64 {
		t.Fatalf("worker slice unbounded: calls=%d overflow=%v err=%v", calls, overflow, err)
	}
	backend.enqueue(Event{Kind: EventChannelMessage, Data: make([]byte, maxQueuedBytes+1)})
	if overflow, err := backend.Dispatch(deliver); !overflow || err != nil || calls != 64 {
		t.Fatalf("overflow not surfaced before delivery: calls=%d overflow=%v err=%v", calls, overflow, err)
	}
}

func TestOpusQueueOverflowDoesNotSilentlyReplaceOlderPacket(t *testing.T) {
	backend := &Backend{ready: make(chan struct{}, 1)}
	for n := 0; n <= maxQueuedOpus; n++ {
		backend.enqueue(Event{Kind: EventOpusFrame, Data: []byte{byte(n)}})
	}
	events, overflow := pollBackend(backend, 0)
	if !overflow || len(events) != maxQueuedOpus {
		t.Fatalf("opus overflow: count=%d overflow=%v", len(events), overflow)
	}
	for n, event := range events {
		if event.Data[0] != byte(n) {
			t.Fatalf("replaced packet at %d: %v", n, event.Data)
		}
	}
}

func TestCloseStopsPendingDelivery(t *testing.T) {
	backend := &Backend{ready: make(chan struct{}, 1)}
	backend.enqueue(Event{Kind: EventChannelMessage, Data: []byte{42}})
	_, _ = backend.Dispatch(func(Event) error { return ErrWouldBlock })
	backend.Close()
	_, err := backend.Dispatch(func(Event) error {
		t.Fatal("called consumer after close")
		return nil
	})
	if !errors.Is(err, ErrClosed) || backend.pending != nil {
		t.Fatalf("closed backend retained pending delivery: %v", err)
	}
}

func TestBackendDataChannelsAndOpus(t *testing.T) {
	backend, err := New()
	if err != nil {
		t.Fatal(err)
	}
	defer backend.Close()

	if err := backend.CreateDataChannel(1, "local-rpc", false, 0, true, true); err != nil {
		t.Fatal(err)
	}
	offer, err := backend.StartOffer()
	if err != nil {
		t.Fatal(err)
	}

	remote, err := webrtc.NewPeerConnection(webrtc.Configuration{})
	if err != nil {
		t.Fatal(err)
	}
	defer remote.Close()

	remote.OnDataChannel(func(channel *webrtc.DataChannel) {
		channel.OnMessage(func(message webrtc.DataChannelMessage) {
			if message.IsString {
				_ = channel.SendText(string(message.Data))
			} else {
				_ = channel.Send(message.Data)
			}
		})
	})
	if err := remote.SetRemoteDescription(webrtc.SessionDescription{
		Type: webrtc.SDPTypeOffer,
		SDP:  offer,
	}); err != nil {
		t.Fatal(err)
	}
	remoteChannel, err := remote.CreateDataChannel("remote-event", nil)
	if err != nil {
		t.Fatal(err)
	}
	remoteChannel.OnOpen(func() {
		_ = remoteChannel.SendText("server-push")
	})
	remoteTrack, err := webrtc.NewTrackLocalStaticSample(webrtc.RTPCodecCapability{
		MimeType:  webrtc.MimeTypeOpus,
		ClockRate: 48000,
		Channels:  2,
	}, "remote-opus", "remote")
	if err != nil {
		t.Fatal(err)
	}
	sender, err := remote.AddTrack(remoteTrack)
	if err != nil {
		t.Fatal(err)
	}
	go func() {
		for {
			if _, _, readErr := sender.ReadRTCP(); readErr != nil {
				return
			}
		}
	}()
	gatheringDone := webrtc.GatheringCompletePromise(remote)
	answer, err := remote.CreateAnswer(nil)
	if err != nil {
		t.Fatal(err)
	}
	if err := remote.SetLocalDescription(answer); err != nil {
		t.Fatal(err)
	}
	<-gatheringDone
	if err := backend.SetRemoteAnswer(remote.LocalDescription().SDP); err != nil {
		t.Fatal(err)
	}

	wantEcho := []byte("rpc-echo")
	localOpen := false
	remoteOpen := false
	echoSeen := false
	pushSeen := false
	opusSeen := false
	writableSeen := false
	remoteOpus := []byte{0xf8, 0xff, 0xfe}
	deadline := time.Now().Add(10 * time.Second)
	for time.Now().Before(deadline) {
		events, overflow := pollBackend(backend, 100)
		if overflow {
			t.Fatal("event queue overflowed")
		}
		for _, event := range events {
			switch event.Kind {
			case EventChannelOpen:
				switch event.Label {
				case "local-rpc":
					localOpen = true
					if err := backend.Send(event.ChannelKey, wantEcho, false); err != nil {
						t.Fatal(err)
					}
				case "remote-event":
					remoteOpen = event.Remote
				}
			case EventChannelMessage:
				if bytes.Equal(event.Data, wantEcho) {
					echoSeen = true
				}
				if string(event.Data) == "server-push" {
					pushSeen = true
				}
			case EventOpusFrame:
				opusSeen = bytes.Equal(event.Data, remoteOpus)
			case EventWritable:
				writableSeen = writableSeen || event.ChannelKey == 1
			}
		}
		if localOpen && remoteOpen && !opusSeen {
			if err := remoteTrack.WriteSample(media.Sample{
				Data: remoteOpus, Duration: 20 * time.Millisecond,
			}); err != nil {
				t.Fatal(err)
			}
		}
		if localOpen && remoteOpen && echoSeen && pushSeen && opusSeen && writableSeen {
			return
		}
	}
	t.Fatalf(
		"timed out: local_open=%t remote_open=%t echo=%t push=%t opus=%t writable=%t",
		localOpen, remoteOpen, echoSeen, pushSeen, opusSeen, writableSeen,
	)
}

func TestExplicitChannelCloseSuppressesTerminalEvent(t *testing.T) {
	backend, err := New()
	if err != nil {
		t.Fatal(err)
	}
	defer backend.Close()
	if err := backend.CreateDataChannel(7, "closed", false, 0, true, true); err != nil {
		t.Fatal(err)
	}
	backend.CloseChannel(7)
	events, _ := pollBackend(backend, 0)
	for _, event := range events {
		if event.ChannelKey == 7 && event.Kind == EventChannelState {
			t.Fatalf("unexpected async terminal event after explicit close: %+v", event)
		}
	}
}

func TestOpusDurationFromTOC(t *testing.T) {
	tests := []struct {
		packet []byte
		want   time.Duration
	}{
		{packet: nil, want: 20 * time.Millisecond},
		{packet: []byte{0x00}, want: 10 * time.Millisecond},
		{packet: []byte{0x18}, want: 60 * time.Millisecond},
		{packet: []byte{0x78}, want: 20 * time.Millisecond},
		{packet: []byte{0x80}, want: 2500 * time.Microsecond},
		{packet: []byte{0x98}, want: 20 * time.Millisecond},
		{packet: []byte{0x99}, want: 40 * time.Millisecond},
		{packet: []byte{0x9b, 0x03}, want: 60 * time.Millisecond},
	}
	for _, test := range tests {
		if got := opusDuration(test.packet); got != test.want {
			t.Fatalf("opusDuration(%x) = %s, want %s", test.packet, got, test.want)
		}
	}
}

func TestBufferedAmountIncludesPendingMessage(t *testing.T) {
	tests := []struct {
		buffered   uint64
		payload    int
		wouldBlock bool
	}{
		{buffered: 0, payload: maxBufferedAmount, wouldBlock: false},
		{buffered: 1, payload: maxBufferedAmount, wouldBlock: true},
		{buffered: maxBufferedAmount - 1, payload: 1, wouldBlock: false},
		{buffered: maxBufferedAmount, payload: 1, wouldBlock: true},
		{buffered: 0, payload: maxBufferedAmount + 1, wouldBlock: true},
	}
	for _, test := range tests {
		if got := exceedsBufferedAmount(test.buffered, test.payload); got != test.wouldBlock {
			t.Fatalf(
				"exceedsBufferedAmount(%d, %d) = %t, want %t",
				test.buffered, test.payload, got, test.wouldBlock,
			)
		}
	}
}

func TestEventQueueHasByteBudget(t *testing.T) {
	backend := &Backend{ready: make(chan struct{}, 1)}
	backend.enqueue(Event{Kind: EventChannelMessage, Data: make([]byte, maxQueuedBytes)})
	backend.enqueue(Event{Kind: EventChannelMessage, Data: []byte{1}})

	events, overflow := pollBackend(backend, 0)
	if !overflow {
		t.Fatal("event byte budget overflow was not reported")
	}
	if len(events) != 1 || len(events[0].Data) != maxQueuedBytes {
		t.Fatalf("queued events = %d, want one budget-sized event", len(events))
	}
	if backend.queuedEventBytes != 0 {
		t.Fatalf("queuedEventBytes = %d after poll, want 0", backend.queuedEventBytes)
	}
}

func TestOversizedEventSignalsPoller(t *testing.T) {
	backend := &Backend{ready: make(chan struct{}, 1)}
	backend.enqueue(Event{
		Kind: EventChannelMessage,
		Data: make([]byte, maxQueuedBytes+1),
	})

	select {
	case <-backend.ready:
	default:
		t.Fatal("oversized event did not signal the poller")
	}
	events, overflow := pollBackend(backend, 0)
	if !overflow || len(events) != 0 {
		t.Fatalf("poll = (%d events, overflow=%t), want (0, true)", len(events), overflow)
	}
}
