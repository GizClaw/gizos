/* Drives the real App transport SESSION_OPEN handling against a stubbed
 * physical link to prove the session-admission gate. Cases:
 *   gated       - before admission and within the fallback, SESSION_OPEN is
 *                 ignored: no ACK, no pending session, so the confirmation
 *                 console keeps leaving on the raw pre-session path.
 *   admit       - after the App admits sessions, SESSION_OPEN is accepted.
 *   fallback    - once the fallback deadline elapses, SESSION_OPEN is accepted
 *                 even though the App never admitted, so a boot that never
 *                 confirms cannot lock the host out of the command transport.
 *   reack       - an already-open session re-acks its own conv once admitted.
 *   invalid     - conv 0 is rejected regardless of the gate.
 */
#include <assert.h>
#include <stdint.h>
#include <string.h>
#include "h2_iostreamikcp.h"
#include "h2_jieli_wl82_atomic.h"

#define H2_WRITE_TIMEOUT_MS 5000u
#define H2_IOSTREAMIKCP_SESSION_CONTROL_PAYLOAD_LEN 4u
#define H2_SESSION_GATE_FALLBACK_MS /* FALLBACK_MS */

static uint32_t clock_ms;
static uint32_t acks_sent;
static uint32_t last_ack_conv;

struct h2_iostreamikcp { int unused; };

/* STRUCTURE */

static uint32_t timer_get_ms(void) { return clock_ms; }

/* Records every SESSION_ACK the transport tries to emit. */
static h2_pal_result_t fake_write(
    void *user, const void *buffer, size_t len, size_t *count,
    uint32_t timeout) {
  (void)user;
  (void)buffer;
  (void)timeout;
  ++acks_sent;
  *count = len;
  return H2_PAL_OK;
}

h2_pal_result_t h2_iostreamikcp_frame_encode(
    const h2_iostreamikcp_frame_t *frame, uint8_t *out, size_t size,
    size_t *len) {
  assert(size > 0u);
  last_ack_conv = frame->conv;
  out[0] = frame->flags;
  *len = 1u;
  return H2_PAL_OK;
}

h2_pal_result_t h2_iostreamikcp_input_frame(
    h2_iostreamikcp_t *stream, const h2_iostreamikcp_frame_t *frame) {
  (void)stream;
  (void)frame;
  return H2_PAL_OK;
}

static void write_le32(uint8_t out[4], uint32_t value);

/* FUNCTIONS */

static void reset_transport(TRANSPORT *self) {
  memset(self, 0, sizeof(*self));
  self->physical_io.write = fake_write;
}

static h2_iostreamikcp_frame_t session_open(uint32_t conv) {
  h2_iostreamikcp_frame_t frame;
  memset(&frame, 0, sizeof(frame));
  frame.flags = H2_IOSTREAMIKCP_FRAME_FLAG_SESSION_OPEN;
  frame.conv = conv;
  return frame;
}

int main(int argc, char **argv) {
  assert(argc == 2);
  TRANSPORT transport;
  reset_transport(&transport);

  if (strcmp(argv[1], "gated") == 0) {
    /* Gate closed, well within the fallback window. */
    transport.gate_started_ms = clock_ms;
    clock_ms += H2_SESSION_GATE_FALLBACK_MS / 2u;
    h2_iostreamikcp_frame_t frame = session_open(7u);
    assert(on_frame(&transport, &frame) == H2_PAL_OK);
    assert(transport.pending_conv == 0u);
    assert(transport.sessions_admitted == 0);
    assert(acks_sent == 0u);
  } else if (strcmp(argv[1], "admit") == 0) {
    transport.gate_started_ms = clock_ms;
    clock_ms += 1000u;
    /* App admits sessions after its confirmation console has drained. */
    transport.sessions_admitted = 1;
    h2_iostreamikcp_frame_t frame = session_open(7u);
    assert(on_frame(&transport, &frame) == H2_PAL_OK);
    assert(transport.pending_conv == 7u);
  } else if (strcmp(argv[1], "fallback") == 0) {
    transport.gate_started_ms = clock_ms;
    /* Never admitted, but the deadline has elapsed. */
    clock_ms += H2_SESSION_GATE_FALLBACK_MS;
    h2_iostreamikcp_frame_t frame = session_open(9u);
    assert(on_frame(&transport, &frame) == H2_PAL_OK);
    assert(transport.pending_conv == 9u);
    assert(transport.sessions_admitted == 1);
  } else if (strcmp(argv[1], "reack") == 0) {
    h2_iostreamikcp_t stream;
    transport.sessions_admitted = 1;
    transport.stream = &stream;
    transport.conv = 5u;
    h2_iostreamikcp_frame_t frame = session_open(5u);
    assert(on_frame(&transport, &frame) == H2_PAL_OK);
    assert(acks_sent == 1u && last_ack_conv == 5u);
  } else {
    assert(strcmp(argv[1], "invalid") == 0);
    /* conv 0 is rejected even after the gate has opened. */
    transport.sessions_admitted = 1;
    h2_iostreamikcp_frame_t frame = session_open(0u);
    assert(on_frame(&transport, &frame) == H2_PAL_ERR_INVALID_ARG);
    assert(transport.pending_conv == 0u && acks_sent == 0u);
  }
  return 0;
}
