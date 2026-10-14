package main

import (
	"encoding/json"
	"errors"
	"golang.org/x/net/ipv4"
	"net"
	"net/http"
	"net/http/httptest"
	"runtime"
	"sync"
	"testing"
	"time"
)

type qosTestConn struct {
	net.PacketConn
	write func([]byte, net.Addr) (int, error)
}

func (c qosTestConn) WriteTo(p []byte, a net.Addr) (int, error) { return c.write(p, a) }
func TestRTPDSCPMultiplexedWrites(t *testing.T) {
	mark, packets := 0, 0
	c := &rtpDSCPPacketConn{dscp: 46, setTOS: func(v int) error { mark = v; runtime.Gosched(); return nil }}
	c.PacketConn = qosTestConn{write: func(p []byte, _ net.Addr) (int, error) {
		want := 0
		if len(p) > 0 && p[0] >= 128 && p[0] <= 191 {
			want = 184
		}
		if mark != want {
			t.Errorf("packet %x TOS=%d want=%d", p, mark, want)
		}
		packets++
		return len(p), nil
	}}
	var wg sync.WaitGroup
	// Cover the RFC 7983 boundaries and concurrent media/control writers.
	inputs := [][]byte{nil, {0}, {3}, {19}, {20}, {63}, {64}, {127}, {128}, {191}, {192}, {255}}
	for i := 0; i < 20; i++ {
		for _, p := range inputs {
			wg.Add(1)
			go func(p []byte) {
				defer wg.Done()
				if _, err := c.WriteTo(p, nil); err != nil {
					t.Error(err)
				}
			}(p)
		}
	}
	wg.Wait()
	got := c.snapshot()
	if packets != 240 || got.MediaPackets != 40 || got.OtherPackets != 200 || got.WriteErrors != 0 {
		t.Fatalf("packets=%d stats=%+v", packets, got)
	}
}
func TestRTPDSCPWriteFailures(t *testing.T) {
	cause := errors.New("test socket failure")
	writes, markCalls := 0, 0
	failMark := true
	c := &rtpDSCPPacketConn{dscp: 46, setTOS: func(int) error {
		markCalls++
		if failMark {
			return cause
		}
		return nil
	}}
	c.PacketConn = qosTestConn{write: func([]byte, net.Addr) (int, error) { writes++; return 0, cause }}
	if n, err := c.WriteTo([]byte{128}, nil); n != 0 || !errors.Is(err, cause) {
		t.Fatalf("mark error %d %v", n, err)
	}
	if writes != 0 {
		t.Fatal("sent packet after failed mark")
	}
	failMark = false
	// A failed marking call invalidates the cached value, including for DF.
	if _, err := c.WriteTo([]byte{20}, nil); !errors.Is(err, cause) {
		t.Fatalf("write error %v", err)
	}
	if markCalls != 2 || writes != 1 || c.snapshot().WriteErrors != 2 || c.snapshot().OtherPackets != 0 {
		t.Fatalf("calls=%d writes=%d stats=%+v", markCalls, writes, c.snapshot())
	}
}
func TestRTPDSCPRealSocket(t *testing.T) {
	if runtime.GOOS == "windows" {
		t.Skip("x/net IPv4 TOS unsupported on Windows; opt-in startup rejects it")
	}
	receiver, err := net.ListenPacket("udp4", "127.0.0.1:0")
	if err != nil {
		t.Fatal(err)
	}
	defer receiver.Close()
	sender, err := net.ListenPacket("udp4", "127.0.0.1:0")
	if err != nil {
		t.Fatal(err)
	}
	defer sender.Close()
	for _, invalid := range []int{-1, 0, 64} {
		if _, err := newRTPDSCPPacketConn(sender, invalid); err == nil {
			t.Fatalf("accepted DSCP %d", invalid)
		}
	}
	c, err := newRTPDSCPPacketConn(sender, 46)
	if err != nil {
		t.Fatal(err)
	}
	for _, p := range []byte{128, 20, 191, 0} {
		if _, err := c.WriteTo([]byte{p}, receiver.LocalAddr()); err != nil {
			t.Fatal(err)
		}
		want := 0
		if p >= 128 && p <= 191 {
			want = 184
		}
		got, err := ipv4.NewPacketConn(sender).TOS()
		if err != nil || got != want {
			t.Fatalf("TOS=%d want=%d err=%v", got, want, err)
		}
		if err := receiver.SetReadDeadline(time.Now().Add(time.Second)); err != nil {
			t.Fatal(err)
		}
		buf := make([]byte, 16)
		n, _, err := receiver.ReadFrom(buf)
		if err != nil || n != 1 || buf[0] != p {
			t.Fatalf("received=%x err=%v", buf[:n], err)
		}
	}
	sender.Close()
	if _, err := newRTPDSCPPacketConn(sender, 46); err == nil {
		t.Fatal("accepted closed socket")
	}
}
func TestQoSWitnessDisabledAndEnabled(t *testing.T) {
	for _, enabled := range []bool{false, true} {
		s := &server{}
		if enabled {
			s.udpQoS = &rtpDSCPPacketConn{dscp: 46}
			s.udpQoS.mediaPackets.Add(200)
			s.udpQoS.otherPackets.Add(300)
		}
		w := httptest.NewRecorder()
		s.routes(func() {}).ServeHTTP(w, httptest.NewRequest(http.MethodGet, "/qos-stats", nil))
		var got qosSnapshot
		if err := json.Unmarshal(w.Body.Bytes(), &got); err != nil {
			t.Fatal(err)
		}
		if w.Code != http.StatusOK || got.Enabled != enabled || (enabled && (got.RTPDSCP != 46 || got.MediaPackets != 200 || got.OtherPackets != 300)) {
			t.Fatalf("witness=%+v", got)
		}
	}
}
