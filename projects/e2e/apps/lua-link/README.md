# Lua Link E2E

Two boards run the same Lua script against each other through the `link`
module (`//libs/lua:lua_link`); one board is the `host`, the other the `join`
side. Each session logs `LINK stage=...` lines on the Runtime Log and ends with
`H2_LUA_LINK_E2E result=PASS|FAIL role=<role> ...`:

- `connected`: role and `max_datagram` of the session.
- `rtt`: 20 reliable ping/pongs, min/avg/max milliseconds (host side).
- `reliable_burst`: 200 reliable 64-byte messages each way, checked in order.
- `datagram`: 100 datagrams each way at 50 Hz and how many arrived.
- `stream`: 64 KiB byte stream each way, checked byte by byte, with kbps.
- `peer_exit`: the joiner leaves; the host must report `peer_closed`.

`hold` mode keeps one session up with 10 Hz datagrams until the link drops and
logs `hold_end reason=... ms_since_last_datagram=...`, so an operator can reset
one board and read how fast the other reports `lost`.

The App only uses the Runtime and the Lua `link` module; board launchers own the
BLE Host (started by the H2Loader command service), the role and the
advertising/scan type. No Wi-Fi is used.

The ESP entries send the original `LINK` records and final result through checked stdout; other Runtime logs keep the board's log provider. They leave a five-second serial reconnection window before starting the BLE Host and the first round. The original five rounds, round spacing, session deadlines and script assertions remain unchanged.

AMOLED defaults to `join`; `--define=H2_LUA_LINK_E2E_ROLE=host` selects the opposite fixture role. Round and summary records identify the actual Runtime board. The image still confirms before the first round when the command service becomes available; that confirmation does not report test results.
