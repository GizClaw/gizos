package main

import (
	"errors"
	"fmt"
	"net/http"
	"strconv"
	"strings"
	"sync/atomic"
	"unicode/utf8"

	"github.com/pion/dtls/v3/pkg/protocol"
	"github.com/pion/dtls/v3/pkg/protocol/alert"
	"github.com/pion/logging"
)

type negotiatedChannel struct {
	ID       uint16
	Label    string
	Ordered  bool
	Reliable bool
}

func parseNegotiatedChannel(headers http.Header) (*negotiatedChannel, error) {
	id := headers.Get("X-H2-Negotiated-ID")
	label := headers.Get("X-H2-Negotiated-Label")
	ordered := headers.Get("X-H2-Negotiated-Ordered")
	reliable := headers.Get("X-H2-Negotiated-Reliable")
	if id == "" && label == "" && ordered == "" && reliable == "" {
		return nil, nil
	}
	value, err := strconv.ParseUint(id, 10, 16)
	if err != nil || value == 65535 || label == "" || len(label) > 128 ||
		!utf8.ValidString(label) || strings.ContainsAny(label, "\r\n\x00") ||
		(ordered != "0" && ordered != "1") || (reliable != "0" && reliable != "1") {
		return nil, fmt.Errorf("invalid explicitly negotiated channel configuration")
	}
	return &negotiatedChannel{ID: uint16(value), Label: label,
		Ordered: ordered == "1", Reliable: reliable == "1"}, nil
}

// Pion's concrete alert error is private, but promotes the public Alert
// methods. Inspect that typed error through errors.As; a timeout or a string
// containing "BadCertificate" must never become authentication evidence.
type receivedDTLSAlert interface {
	error
	ContentType() protocol.ContentType
	Marshal() ([]byte, error)
}

type authenticationWitness struct{ received atomic.Uint32 }

func (w *authenticationWitness) observe(args ...interface{}) {
	for _, arg := range args {
		err, ok := arg.(error)
		if !ok {
			continue
		}
		var received receivedDTLSAlert
		if !errors.As(err, &received) || received.ContentType() != protocol.ContentTypeAlert {
			continue
		}
		bytes, marshalErr := received.Marshal()
		if marshalErr == nil && len(bytes) == 2 && bytes[0] == byte(alert.Fatal) {
			w.received.CompareAndSwap(0, uint32(bytes[0])<<8|uint32(bytes[1]))
		}
	}
}

// Only a fatal certificate rejection from this peer, before any channel opened,
// proves the intentionally wrong SDP fingerprint was rejected. Generic DTLS
// failures and connection timeouts are not authentication evidence.
func (w *authenticationWitness) rejectedCertificate(opened uint64) bool {
	received := w.received.Load()
	return opened == 0 && (received == uint32(alert.Fatal)<<8|uint32(alert.BadCertificate) ||
		received == uint32(alert.Fatal)<<8|uint32(alert.CertificateUnknown))
}

type witnessLoggerFactory struct {
	delegate logging.LoggerFactory
	witness  *authenticationWitness
}

func (f *witnessLoggerFactory) NewLogger(scope string) logging.LeveledLogger {
	return &witnessLogger{LeveledLogger: f.delegate.NewLogger(scope), witness: f.witness}
}

type witnessLogger struct {
	logging.LeveledLogger
	witness *authenticationWitness
}

func (l *witnessLogger) Warnf(format string, args ...interface{}) {
	l.witness.observe(args...)
	l.LeveledLogger.Warnf(format, args...)
}

func (l *witnessLogger) Errorf(format string, args ...interface{}) {
	l.witness.observe(args...)
	l.LeveledLogger.Errorf(format, args...)
}
