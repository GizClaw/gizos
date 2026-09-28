package main

import (
	"context"
	"errors"
	"fmt"
	"net/http"
	"testing"

	"github.com/pion/dtls/v3/pkg/protocol/alert"
)

type testAlertError struct{ *alert.Alert }

func (e *testAlertError) Error() string { return e.Alert.String() }

func TestAuthenticationWitnessRequiresTypedReceivedAlert(t *testing.T) {
	var first, other authenticationWitness
	first.observe(context.DeadlineExceeded, errors.New("alert: Alert Fatal: BadCertificate"))
	if first.received.Load() != 0 {
		t.Fatal("timeout or a matching error string became certificate evidence")
	}
	first.observe(&testAlertError{&alert.Alert{Level: alert.Warning, Description: alert.CloseNotify}})
	if first.received.Load() != 0 {
		t.Fatal("ordinary close became certificate evidence")
	}
	first.observe(fmt.Errorf("handshake: %w", &testAlertError{
		&alert.Alert{Level: alert.Fatal, Description: alert.BadCertificate},
	}))
	if first.received.Load() != 2<<8|42 || other.received.Load() != 0 {
		t.Fatal("typed alert was lost or attributed to another session")
	}
	other.observe(&testAlertError{&alert.Alert{Level: alert.Fatal, Description: alert.HandshakeFailure}})
	if other.received.Load() == 2<<8|42 {
		t.Fatal("generic handshake failure became certificate evidence")
	}
}

func TestAuthenticationWitnessCertificateVerdict(t *testing.T) {
	for _, description := range []alert.Description{alert.BadCertificate, alert.CertificateUnknown,
		alert.HandshakeFailure, alert.UnknownCA, alert.CloseNotify} {
		t.Run(fmt.Sprint(description), func(t *testing.T) {
			var witness authenticationWitness
			witness.observe(&testAlertError{&alert.Alert{Level: alert.Fatal, Description: description}})
			expected := description == alert.BadCertificate || description == alert.CertificateUnknown
			if witness.rejectedCertificate(0) != expected {
				t.Fatalf("incorrect certificate verdict for typed alert %v", description)
			}
			if witness.rejectedCertificate(1) {
				t.Fatal("a session that opened a channel became authentication rejection evidence")
			}
		})
	}
	var pending authenticationWitness
	if pending.rejectedCertificate(0) {
		t.Fatal("absence of a typed rejection became authentication evidence")
	}
}

func TestNegotiatedChannelFixtureConfiguration(t *testing.T) {
	headers := http.Header{}
	if value, err := parseNegotiatedChannel(headers); err != nil || value != nil {
		t.Fatal("ordinary DCEP offers must keep their existing behavior")
	}
	headers.Set("X-H2-Negotiated-ID", "7")
	if _, err := parseNegotiatedChannel(headers); err == nil {
		t.Fatal("partial out-of-band configuration was accepted")
	}
	headers.Set("X-H2-Negotiated-Label", "pal/id")
	headers.Set("X-H2-Negotiated-Ordered", "1")
	headers.Set("X-H2-Negotiated-Reliable", "1")
	value, err := parseNegotiatedChannel(headers)
	if err != nil || value.ID != 7 || value.Label != "pal/id" || !value.Ordered || !value.Reliable {
		t.Fatal(value, err)
	}
	for _, invalid := range []string{"-1", "65535", "65536", "seven"} {
		headers.Set("X-H2-Negotiated-ID", invalid)
		if _, err := parseNegotiatedChannel(headers); err == nil {
			t.Fatalf("invalid stream id %q accepted", invalid)
		}
	}
}
