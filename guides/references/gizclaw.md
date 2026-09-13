# GizClaw API

<!--@include: ../.generated/api/gizclaw.md-->

## Complete text input

After activating the desired Workspace and creating a Conversation route, use
`h2_gizclaw_conversation_send_text()` to submit one UTF-8 user input (1–4096 bytes).
Admission copies the span; it does not perform network I/O. Empty, oversized,
NUL-containing or malformed UTF-8 input returns `INVALID_ARG`. A Service that has
not started returns `INVALID_STATE`; a stopping/stopped Service returns `CLOSED`.
An outstanding input or an occupied Speech/playback route returns `BUSY` without
interrupting recording. Allocation/admission failures return `NO_MEMORY` or
`WOULD_BLOCK`. A started Service may queue input while connecting.

The network worker sends the existing control BOS (`kind=UNSPECIFIED`, empty
MIME), then TEXT_DONE with the same new connection-local `demo-<sequence>` stream
ID and existing `demo-home` input label. Event sequences are 0 and 1; timestamps
are 0. TEXT_DONE carries the entire text and ends the input: no additional EOS,
audio BOS, audio READY or Opus is needed. The server's
`peerStreamEventToChunk` maps it to a user text chunk with EndOfStream set.
Workspace routing follows the connection's active Workspace, not the label.

The existing completion callback runs once from `service_poll()` after sending,
failure or cancellation; admission errors do not call it. Success does not imply
server acceptance or an Agent reply. Keep the handle until completion and use
the existing cancel API if needed. Audio cannot start while this input is pending.
No readable PCM Track is needed to send text. Downstream audio retains its
connection lifetime and can play after completion; downstream text observations
retain the existing active-input callback lifetime.
