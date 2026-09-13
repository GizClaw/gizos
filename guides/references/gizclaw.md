# GizClaw API

<!--@include: ../.generated/api/gizclaw.md-->

## Complete text input

After a Session has selected the desired Workspace and created its Conversation
route, use `h2_gizclaw_session_send_text()` to submit one UTF-8 user input
(1–4096 bytes). Admission copies the span; it does not perform network I/O.
Empty, oversized, NUL-containing or malformed UTF-8 input returns `INVALID_ARG`.
A closed or preparing Session, a Workspace that is not READY, a missing route or
a Service that has not started returns `INVALID_STATE`; a stopping Service
returns `CLOSED`. Open audio input or an input still awaiting completion returns
`BUSY` without interrupting it. Allocation/admission failures return
`NO_MEMORY` or `WOULD_BLOCK`. A started Service may queue input while connecting.

Accepted text publishes the Session conversation phase `WAITING` with
`conversation_input_open == false`, in either input mode; it returns to `IDLE`
when downstream audio arrives, after `H2_GIZCLAW_SESSION_WAIT_MS` without any, or
when the input fails. Until completion the Session keeps owning the route:
`h2_gizclaw_session_conversation_release()` leaves it in place, while
`h2_gizclaw_session_audio_start()`, a Workspace switch or a delete of the current
Workspace cancel the pending text first, as they do for a previous audio input.

The network worker sends the existing control BOS (`kind=UNSPECIFIED`, empty
MIME), then TEXT_DONE with the same new connection-local `demo-<sequence>` stream
ID and existing `demo-home` input label. Event sequences are 0 and 1; timestamps
are 0. TEXT_DONE carries the entire text and ends the input: no additional EOS,
audio BOS, audio READY or Opus is needed. The server's
`peerStreamEventToChunk` maps it to a user text chunk with EndOfStream set.
Workspace routing follows the connection's active Workspace, not the label.

The Session conversation completion runs once from `service_poll()` after
sending, failure or cancellation; admission errors do not call it. Success does
not imply server acceptance or an Agent reply. No readable PCM Track is needed
to send text. Downstream audio retains its connection lifetime and can play
after completion; downstream text observations retain the existing active-input
callback lifetime.
