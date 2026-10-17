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

The AMOLED peer's `//projects/e2e/targets/h2loader_tar_zlib/lua-link/amoled:package` runs the join role; `:host-package` reuses the public host launcher with the real AMOLED BSP through `//projects/e2e/libs/esp-device:firmware.bzl`. Swap the peer and DUT roles to cover both DUT roles. Each launcher runs five complete sessions followed by the hold session; qualification requires the actual two boards' complete session ledgers.

Both ESP launchers route the exact Runtime Log scopes `lua-link-e2e` (the terminal result) and `lua` (the script's `LINK stage=...` records) through the existing H2Loader UART or USB JTAG checked result console. Each record requires a complete bounded write and TX drain; other scopes retain the original Log provider and filtering. This does not change SDK, global or tag log levels.

After command service startup and result router preparation, each launcher waits 5000 ms before its original App confirmation and first round so the reboot monitor can reconnect before the first session. The original single confirmation still precedes all five rounds; it does not qualify the sessions. After each normal or hold run returns, any sticky result-console error keeps the command service available in the diagnostic hold, without counting the session as passed or continuing to another round. Qualification still requires both boards' first complete ordered session ledgers and actual terminal results; round/summary output, early confirmation or a later replay cannot fill missing session evidence.
