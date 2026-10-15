package main

import (
	"fmt"
	"net"
	"sync"
	"sync/atomic"

	"golang.org/x/net/ipv4"
)

// rtpDSCPPacketConn marks direct ICE UDP media independently of bulk DTLS/SCTP.
// SetTOS changes the entire socket, so every write must share this lock. The
// default fixture does not wrap its socket and retains its original behavior.
type rtpDSCPPacketConn struct {
	net.PacketConn
	mu           sync.Mutex
	setTOS       func(int) error
	dscp         int
	currentTOS   int
	mediaPackets atomic.Uint64
	otherPackets atomic.Uint64
	writeErrors  atomic.Uint64
}

type qosSnapshot struct {
	Enabled      bool   `json:"enabled"`
	RTPDSCP      int    `json:"rtp_dscp"`
	OtherDSCP    int    `json:"other_dscp"`
	MediaPackets uint64 `json:"media_packets"`
	OtherPackets uint64 `json:"other_packets"`
	WriteErrors  uint64 `json:"write_errors"`
}

func newRTPDSCPPacketConn(conn net.PacketConn, dscp int) (*rtpDSCPPacketConn, error) {
	if dscp < 1 || dscp > 63 {
		return nil, fmt.Errorf("RTP DSCP must be in 1..63")
	}
	ip := ipv4.NewPacketConn(conn)
	// Check the requested mark and restore DF before admitting the fixture.
	// A platform that cannot apply/read it must not silently claim QoS evidence.
	for _, tos := range []int{dscp << 2, 0} {
		if err := ip.SetTOS(tos); err != nil {
			return nil, fmt.Errorf("set TOS %d: %w", tos, err)
		}
		got, err := ip.TOS()
		if err != nil {
			return nil, fmt.Errorf("read TOS: %w", err)
		}
		if got != tos {
			return nil, fmt.Errorf("TOS readback %d differs from %d", got, tos)
		}
	}
	return &rtpDSCPPacketConn{PacketConn: conn, setTOS: ip.SetTOS, dscp: dscp}, nil
}

func (c *rtpDSCPPacketConn) WriteTo(data []byte, addr net.Addr) (int, error) {
	// RFC 7983 demultiplexing: 128..191 is RTP/RTCP; STUN and DTLS stay DF.
	media := len(data) != 0 && data[0] >= 128 && data[0] <= 191
	tos := 0
	if media {
		tos = c.dscp << 2
	}
	c.mu.Lock()
	defer c.mu.Unlock()
	if tos != c.currentTOS {
		if err := c.setTOS(tos); err != nil {
			c.currentTOS = -1 // Do not reuse a possibly changed socket mark.
			c.writeErrors.Add(1)
			return 0, fmt.Errorf("set packet TOS: %w", err)
		}
		c.currentTOS = tos
	}
	n, err := c.PacketConn.WriteTo(data, addr)
	if err != nil {
		c.writeErrors.Add(1)
	} else if media {
		c.mediaPackets.Add(1)
	} else {
		c.otherPackets.Add(1)
	}
	return n, err
}

func (c *rtpDSCPPacketConn) snapshot() qosSnapshot {
	if c == nil {
		return qosSnapshot{}
	}
	return qosSnapshot{Enabled: true, RTPDSCP: c.dscp,
		MediaPackets: c.mediaPackets.Load(), OtherPackets: c.otherPackets.Load(),
		WriteErrors: c.writeErrors.Load()}
}
